/* +----------------------------------------------------------------------- *\
   | */ static const char pkgname[]   = "darkhttpd/0.1";                    /*
   | */ static const char copyright[] = "copyright (c) 2003 Emil Mikulic";  /*
   +----------------------------------------------------------------------- */

/*
 * $Id$
 */

/*
 * TODO:
 *  x Ignore SIGPIPE.
 *  x Actually serve files.
 *  . Generate directory listings.
 *  x Log to file.
 *  . Partial content.
 *  x If-Modified-Since.
 *  x Test If-Mod-Since with IE, Phoenix, lynx, links, Opera
 *  . Keep-alive connections.
 *  . Chroot, set{uid|gid}.
 *  . Port to Win32.
 *  x Detect Content-Type from a list of content types.
 *  x Log Referer, User-Agent.
 *  x Ensure URIs requested are safe.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* for easy defusal */
#define debugf printf

#ifndef min
#define min(a,b) ( ((a)<(b)) ? (a) : (b) )
#endif

LIST_HEAD(conn_list_head, connection) connlist =
    LIST_HEAD_INITIALIZER(conn_list_head);

struct connection
{
    LIST_ENTRY(connection) entries;

    int socket;
    in_addr_t client;
    time_t last_active;
    enum {
        RECV_REQUEST,   /* receiving request */
        SEND_HEADER,    /* sending generated header */
        SEND_REPLY,     /* sending reply */
        DONE            /* connection closed, need to remove from queue */
        } state;

    /* char request[request_length+1] is null-terminated */
    char *request;
    unsigned int request_length;

    /* request fields */
    char *method, *uri, *referer, *user_agent;

    char *header;
    unsigned int header_sent, header_length;
    int header_dont_free, header_only, http_code;

    enum { REPLY_GENERATED, REPLY_FROMFILE } reply_type;
    char *reply, *lastmod; /* reply lastmod, not request if-mod-since */
    int reply_dont_free;
    FILE *reply_file;
    unsigned int reply_sent, reply_length;

    unsigned int total_sent; /* header + body = total, for logging */
};



LIST_HEAD(mime_map_head, mime_mapping) mime_map =
    LIST_HEAD_INITIALIZER(mime_map_head);

struct mime_mapping
{
    LIST_ENTRY(mime_mapping) entries;

    char *extension, *mimetype;
};



/* If a connection is idle for IDLETIME seconds or more, it gets closed and
 * removed from the connlist.  Define to 0 to remove the timeout
 * functionality.
 */
#define IDLETIME 60

/* To prevent a malformed request from eating up too much memory, die once the
 * request exceeds this many bytes:
 */
#define MAX_REQUEST_LENGTH 4000



/* Defaults can be overridden on the command-line */
static in_addr_t bindaddr = INADDR_ANY;
static u_int16_t bindport = 80;
static int max_connections = -1;        /* kern.ipc.somaxconn */
static const char *index_name = "index.html";

static int sockin = -1;             /* socket to accept connections from */
static char *wwwroot = NULL;        /* a path name */
static char *logfile_name = NULL;   /* NULL = no logging */
static FILE *logfile = NULL;
static int want_chroot = 0;

static const char *default_extension_map[] = {
    /* Linear search used - order affects speed significantly.
     * This could be done in a better way.
     */
    "text/html          html htm",
    "image/png          png",
    "image/jpeg         jpeg jpe jpg",
    "image/gif          gif",
    "audio/mpeg         mp2 mp3 mpga",
    "application/ogg    ogg",
    "text/css           css",
    "text/plain         txt asc",
    "text/xml           xml",
    "video/mpeg         mpeg mpe mpg",
    "video/x-msvideo    avi",
    NULL
};

static const char default_mimetype[] = "application/octet-stream";



/* ---------------------------------------------------------------------------
 * malloc that errx()s if it can't allocate.
 */
static void *xmalloc(const size_t size)
{
    void *ptr = malloc(size);
    if (ptr == NULL) errx(1, "can't allocate %u bytes", size);
    return ptr;
}



/* ---------------------------------------------------------------------------
 * realloc() that errx()s if it can't allocate.
 */
static void *xrealloc(void *original, const size_t size)
{
    void *ptr = realloc(original, size);
    if (ptr == NULL) errx(1, "can't reallocate %u bytes", size);
    return ptr;
}



/* ---------------------------------------------------------------------------
 * strdup() that errx()s if it can't allocate.
 */
static char *xstrdup(const char *src)
{
    char *dest = strdup(src);
    if (dest == NULL) errx(1, "out of memory in strdup()");
    return dest;
}



/* ---------------------------------------------------------------------------
 * asprintf() that errx()s if it fails.
 */
static unsigned int xvasprintf(char **ret, const char *format, va_list ap)
{
    int len = vasprintf(ret, format, ap);
    if (ret == NULL || len == -1) errx(1, "out of memory in vasprintf()");
    return (unsigned int)len;
}



/* ---------------------------------------------------------------------------
 * asprintf() that errx()s if it fails.
 */
static unsigned int xasprintf(char **ret, const char *format, ...)
{
    va_list va;
    int len;

    va_start(va, format);
    len = xvasprintf(ret, format, va);
    va_end(va);
    return len;
}



/* ---------------------------------------------------------------------------
 * Make the specified socket non-blocking.
 */
static void nonblock_socket(const int sock)
{
    if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1)
        err(1, "fcntl() to set O_NONBLOCK");
}



/* ---------------------------------------------------------------------------
 * Split string out of src with range [left:right-1]
 */
static char *split_string(const char *src,
    const unsigned int left, const unsigned int right)
{
    char *dest = (char*) xmalloc(right - left + 1);
    memcpy(dest, src+left, right-left);
    dest[right-left] = '\0';
    return dest;
}



/* ---------------------------------------------------------------------------
 * Resolve /./ and /../ in a URI, returing a new, safe URI, or NULL if the URI
 * is invalid/unsafe.
 */
static char *make_safe_uri(const char *uri)
{
    char **elements, **reassembly, *out;
    unsigned int slashes, elem, reasm, urilen, i, j;

    if (uri[0] != '/') return NULL;
    urilen = strlen(uri);

    /* count the slashes */
    for (i=0, slashes=0; i<urilen; i++)
        if (uri[i] == '/') slashes++;

    /* make an array for the URI elements */
    elements = (char**) xmalloc(sizeof(char*) * slashes);
    for (i=0; i<slashes; i++) elements[i] = NULL;

    /* split by slash */
    elem = i = 0;
    while (i < urilen) /* i is the left bound */
    {
        /* look for a non-slash */
        for (; uri[i] == '/'; i++);

        /* look for the next slash */
        for (j=i+1; uri[j] != '/' && uri[j] != '\0'; j++);

        elements[elem++] = split_string(uri, i, j);
        i = j; /* iterate */
    }

    reassembly = (char**) xmalloc(sizeof(char*) * slashes);
    for (i=0; i<slashes; i++) reassembly[i] = NULL;
    reasm = 0;

    /* process */
    for (i=0; i<elem; i++)
    {
        if (strcmp(elements[i], ".") == 0)
        { /* do nothing */ }
        else if (strcmp(elements[i], "..") == 0)
        {
            /* try to backstep */
            if (reasm == 0)
            {
                /* user walked out of wwwroot! unsafe uri! */
                for (j=0; j<elem; j++)
                    if (elements[j] != NULL) free(elements[j]);
                free(elements);
                free(reassembly);
                return NULL;
            }
            /* else */
            reasm--;
            reassembly[reasm] = NULL;
        }
        else
        {
            /* plain copy */
            reassembly[reasm++] = elements[i];
        }
    }

    /* reassemble */
    out = (char*) xmalloc(urilen+1);
    out[0] = '\0';
    
    for (i=0; i<reasm; i++)
    {
        strcat(out, "/");
        strcat(out, reassembly[i]);
    }

    out = (char*) xrealloc(out, strlen(out)+1); /* shorten buffer */
    debugf("`%s' -safe-> `%s'\n", uri, out);
    return out;
}



/* ---------------------------------------------------------------------------
 * Parses a mime.types line and adds the parsed data to the mime_map.
 */
static void parse_mimetype_line(const char *line)
{
    unsigned int pad, bound1, lbound, rbound;

    /* parse mimetype */
    for (pad=0; line[pad] == ' ' || line[pad] == '\t'; pad++);
    if (line[pad] == 0 ||   /* empty line */
        line[pad] == '#')   /* comment */
        return;

    for (bound1=pad+1;
        line[bound1] != ' ' &&
        line[bound1] != '\t';
        bound1++)
    {
        if (line[bound1] == 0) return; /* malformed line */
    }

    lbound = bound1;
    for (;;)
    {
        struct mime_mapping *mapping;

        /* find beginning of extension */
        for (; line[lbound] == ' ' || line[lbound] == '\t'; lbound++);
        if (line[lbound] == 0) return; /* end of line */

        /* find end of extension */
        for (rbound = lbound;
            line[rbound] != ' ' &&
            line[rbound] != '\t' &&
            line[rbound] != '\0';
            rbound++);

        mapping = (struct mime_mapping *)
            xmalloc(sizeof(struct mime_mapping));
        mapping->mimetype = split_string(line, pad, bound1);
        mapping->extension = split_string(line, lbound, rbound);

        assert(strlen(mapping->mimetype) > 0);
        assert(strlen(mapping->extension) > 0);

        debugf("*.%s \t-> %s\n", mapping->extension, mapping->mimetype);

        LIST_INSERT_HEAD(&mime_map, mapping, entries);

        if (line[rbound] == 0) return; /* end of line */
        else lbound = rbound+1;
    }
}



/* ---------------------------------------------------------------------------
 * Adds contents of default_extension_map[] to mime_map list.
 */
static void parse_default_extension_map(void)
{
    int i;

    for (i=0; default_extension_map[i] != NULL; i++)
        parse_mimetype_line(default_extension_map[i]);
}



/* ---------------------------------------------------------------------------
 * Adds contents of specified file to mime_map list.
 */
static void parse_extension_map_file(const char *filename)
{
    char *buf = NULL;
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) err(1, "fopen(\"%s\")", filename);

    while (!feof(fp))
    {
        size_t line_len;
        char c;
        long filepos;

        /* store current file position */
        filepos = ftell(fp);
        if (filepos == -1) err(1, "ftell()");

        /* read to newline */
        for (c=0, line_len=0;
            !feof(fp) && c != '\n' && c != '\r';
            c = fgetc(fp), line_len++);

        /* jump back to beginning of current line */
        if (fseek(fp, filepos, SEEK_SET) == -1)
            err(1, "fseek()");

        if (line_len-1 != 0)
        {
            /* alloc and fill up buf */
            buf = (char*) xmalloc(line_len);
            if (fread(buf, 1, line_len-1, fp) != (line_len-1))
                err(1, "fread()");
            buf[line_len-1] = '\0';

            /* parse it */
            parse_mimetype_line(buf);
            free(buf);
        }

        c = fgetc(fp); /* read last char (newline) */
    }

    fclose(fp);
}



/* ---------------------------------------------------------------------------
 * Uses the mime_map to determine a Content-Type: for a requested URI.
 */
static const char *uri_content_type(const char *uri)
{
    struct mime_mapping *mapping;
    int urilen = strlen(uri);

    LIST_FOREACH(mapping, &mime_map, entries)
    {
        int extlen = strlen(mapping->extension);
        if (urilen >= extlen+3) /* "/a." + "ext" */
        {
            if (uri[urilen-1-extlen] == '.' &&
                strcmp(uri+urilen-extlen, mapping->extension) == 0)
                    return mapping->mimetype;
        }
    }
    return default_mimetype;
}



/* ---------------------------------------------------------------------------
 * Initialize the sockin global.  This is the socket that we accept
 * connections from.
 */
static void init_sockin(void)
{
    struct sockaddr_in addrin;
    int sockopt;

    /* create incoming socket */
    sockin = socket(PF_INET, SOCK_STREAM, 0);
    if (sockin == -1) err(1, "socket()");

    /* reuse address */
    sockopt = 1;
    if (setsockopt(sockin, SOL_SOCKET, SO_REUSEADDR,
            &sockopt, sizeof(sockopt)) == -1)
        err(1, "setsockopt(SO_REUSEADDR)");

    nonblock_socket(sockin);

    /* bind socket */
    addrin.sin_family = (u_char)PF_INET;
    addrin.sin_port = htons(bindport);
    addrin.sin_addr.s_addr = bindaddr;
    memset(&(addrin.sin_zero), 0, 8);
    if (bind(sockin, (struct sockaddr *)&addrin,
            sizeof(struct sockaddr)) == -1)
        err(1, "bind(port %u)", bindport);

    debugf("listening on %s:%u\n", inet_ntoa(addrin.sin_addr), bindport);

    /* listen on socket */
    if (listen(sockin, max_connections) == -1)
        err(1, "listen()");
}



/* ---------------------------------------------------------------------------
 * Prints a usage statement.
 */
static void usage(void)
{
    printf("\n  usage: darkhttpd /path/to/wwwroot [options]\n\n"
    "options:\n\n"
    "\t--port number (default: %u)\n" /* bindport */
    "\t\tSpecifies which port to listen on for connections.\n"
    "\n"
    "\t--addr ip (default: all)\n"
    "\t\tIf multiple interfaces are present, specifies\n"
    "\t\twhich one to bind the listening port to.\n"
    "\n"
    "\t--maxconn number (default: system maximum)\n"
    "\t\tSpecifies how many concurrent connections to accept.\n"
    "\n"
    "\t--log filename (default: no logging)\n"
    "\t\tSpecifies which file to append the request log to.\n"
    "\n"
    "\t--chroot (default: don't chroot)\n"
    "\t\tLocks server into wwwroot directory for added security.\n"
    "\n"
    "\t--index filename (default: %s)\n" /* index_name */
    "\t\tDefault file to serve when a directory is requested.\n"
    "\n"
    "\t--mimetypes filename (optional)\n"
    "\t\tParses specified file for extension-MIME associations.\n"
    "\n"
    /* "\t--uid blah, --gid blah\n" FIXME */
    , bindport, index_name);
    exit(EXIT_FAILURE);
}



/* ---------------------------------------------------------------------------
 * Expands a path beginning with a tilde.  The returned string needs to be
 * deallocated.
 */
static char *expand_tilde(const char *path)
{
    const char *home;
    char *tmp = NULL;

    if (path[0] != '~') return xstrdup(path); /* do nothing */

    home = getenv("HOME");
    if (home == NULL)
    {
        /* no ENV variable, try getpwuid() */
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }

    if (home == NULL) errx(1, "can't expand `~'");

    xasprintf(&tmp, "%s%s", home, path+1);
    return tmp;
}



/* ---------------------------------------------------------------------------
 * Strips the ending slash from a string (if there is one)
 */
static void strip_endslash(char **str)
{
    if (strlen(*str) < 1) return;
    if ((*str)[strlen(*str)-1] != '/') return;

    (*str)[strlen(*str)-1] = 0;
    *str = (char*) xrealloc(*str, strlen(*str)+1);
}



/* ---------------------------------------------------------------------------
 * Parses commandline options.
 */
static void parse_commandline(const int argc, char *argv[])
{
    int i;

    if (
        (argc < 2) ||
        (argc == 2 && strcmp(argv[1], "--help") == 0)
       )
        usage(); /* no wwwroot given */

    wwwroot = expand_tilde( argv[1] ); /* ~/html -> /home/user/html */
    strip_endslash(&wwwroot);

    /* walk through the remainder of the arguments (if any) */
    for (i=2; i<argc; i++)
    {
        if (strcmp(argv[i], "--port") == 0)
        {
            if (++i >= argc) errx(1, "missing number after --port");
            bindport = (u_int16_t)atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--addr") == 0)
        {
            if (++i >= argc) errx(1, "missing ip after --addr");
            bindaddr = inet_addr(argv[i]);
            if (bindaddr == (in_addr_t)INADDR_NONE)
                errx(1, "malformed --addr argument");
        }
        else if (strcmp(argv[i], "--maxconn") == 0)
        {
            if (++i >= argc) errx(1, "missing number after --maxconn");
            max_connections = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "--log") == 0)
        {
            if (++i >= argc) errx(1, "missing filename after --log");
            logfile_name = argv[i];
        }
        else if (strcmp(argv[i], "--chroot") == 0)
        {
            want_chroot = 1;
        }
        else if (strcmp(argv[i], "--index") == 0)
        {
            if (++i >= argc) errx(1, "missing filename after --index");
            index_name = argv[i];
        }
        else if (strcmp(argv[i], "--mimetypes") == 0)
        {
            if (++i >= argc) errx(1, "missing filename after --mimetypes");
            parse_extension_map_file(argv[i]);
        }
        else
            errx(1, "unknown argument `%s'", argv[i]);
    }
}



/* ---------------------------------------------------------------------------
 * Allocate and initialize an empty connection.
 */
static struct connection *new_connection(void)
{
    struct connection *conn = (struct connection *)
        xmalloc(sizeof(struct connection));

    conn->socket = -1;
    conn->client = INADDR_ANY;
    conn->last_active = time(NULL);
    conn->request = NULL;
    conn->method = conn->uri = NULL;
    conn->referer = conn->user_agent = NULL;
    conn->request_length = 0;
    conn->header = NULL;
    conn->header_sent = conn->header_length = 0;
    conn->header_dont_free = conn->header_only = 0;
    conn->http_code = 0;
    conn->reply = conn->lastmod = NULL;
    conn->reply_dont_free = 0;
    conn->reply_file = NULL;
    conn->reply_sent = conn->reply_length = 0;
    conn->total_sent = 0;

    /* Make it harmless so it gets garbage-collected if it should, for some
     * reason, fail to be correctly filled out.
     */
    conn->state = DONE;

    return conn;
}



/* ---------------------------------------------------------------------------
 * Accept a connection from sockin and add it to the connection queue.
 */
static void accept_connection(void)
{
    struct sockaddr_in addrin;
    socklen_t sin_size;
    struct connection *conn;

    /* allocate and initialise struct connection */
    conn = new_connection();

    sin_size = (socklen_t)sizeof(struct sockaddr);
    conn->socket = accept(sockin, (struct sockaddr *)&addrin,
            &sin_size);
    if (conn->socket == -1) err(1, "accept()");

    nonblock_socket(conn->socket);

    conn->state = RECV_REQUEST;
    conn->client = addrin.sin_addr.s_addr;
    LIST_INSERT_HEAD(&connlist, conn, entries);

    debugf("accepted connection from %s:%u\n",
        inet_ntoa(addrin.sin_addr),
        ntohs(addrin.sin_port) );
}



/* ---------------------------------------------------------------------------
 * Cleanly deallocate the internals of a struct connection
 */
static void free_connection(struct connection *conn)
{
    if (conn->socket != -1) close(conn->socket);
    if (conn->request != NULL) free(conn->request);
    if (conn->method != NULL) free(conn->method);
    if (conn->uri != NULL) free(conn->uri);
    if (conn->referer != NULL) free(conn->referer);
    if (conn->user_agent != NULL) free(conn->user_agent);
    if (conn->header != NULL && !conn->header_dont_free) free(conn->header);
    if (conn->reply != NULL && !conn->reply_dont_free) free(conn->reply);
    if (conn->lastmod != NULL) free(conn->lastmod);
    if (conn->reply_file != NULL) fclose(conn->reply_file);
}



/* ---------------------------------------------------------------------------
 * Uppercasify all characters in a string of given length.
 */
static void strntoupper(char *str, const size_t length)
{
    size_t i;
    for (i=0; i<length; i++)
        str[i] = toupper(str[i]);
}



/* ---------------------------------------------------------------------------
 * If a connection has been idle for more than IDLETIME seconds, it will be
 * marked as DONE and killed off in httpd_poll()
 */
static void poll_check_timeout(struct connection *conn)
{
#if IDLETIME > 0
    if (time(NULL) - conn->last_active >= IDLETIME)
    {
        debugf("poll_check_timeout(%d) caused closure\n", conn->socket);
        conn->state = DONE;
    }
#endif
}



/* ---------------------------------------------------------------------------
 * Build an RFC1123 date in the static buffer _date[] and return it.
 */
#define MAX_DATE_LENGTH 29 /* strlen("Fri, 28 Feb 2003 00:02:08 GMT") */
static char _date[MAX_DATE_LENGTH + 1];
static char *rfc1123_date(const time_t when)
{
    time_t now = when;
    strftime(_date, MAX_DATE_LENGTH,
        "%a, %d %b %Y %H:%M:%S %Z", gmtime(&now) );
    return _date;
}



/* ---------------------------------------------------------------------------
 * Decode URL by converting %XX (where XX are hexadecimal digits) to the
 * character it represents.  Don't forget to free the return value.
 */
static char *urldecode(const char *url)
{
    size_t len = strlen(url);
    char *out = (char*)xmalloc(len+1);
    int i, pos;

    for (i=0, pos=0; i<len; i++)
    {
        if (url[i] == '%' && i+2 < len &&
            isxdigit(url[i+1]) && isxdigit(url[i+2]))
        {
            /* decode %XX */
            #define HEX_TO_DIGIT(hex) ( \
                ((hex) >= 'A' && (hex) <= 'F') ? ((hex)-'A'+10): \
                ((hex) >= 'a' && (hex) <= 'f') ? ((hex)-'a'+10): \
                ((hex)-'0') )

            out[pos++] = HEX_TO_DIGIT(url[i+1]) * 16 +
                         HEX_TO_DIGIT(url[i+2]);
            i += 2;

            #undef HEX_TO_DIGIT
        }
        else
        {
            /* straight copy */
            out[pos++] = url[i];
        }
    }
    out[pos] = 0;

    out = xrealloc(out, strlen(out)+1);  /* dealloc what we don't need */
    return out;
}



/* ---------------------------------------------------------------------------
 * A default reply for any (erroneous) occasion.
 */
static void default_reply(struct connection *conn,
    const int errcode, const char *errname, const char *format, ...)
{
    char *reason;
    va_list va;

    va_start(va, format);
    xvasprintf(&reason, format, va);
    va_end(va);

    conn->reply_length = xasprintf(&(conn->reply),
     "<html><head><title>%d %s</title></head><body>\n"
     "<h1>%s</h1>\n" /* errname */
     "%s\n" /* reason */
     "<hr>\n"
     "Generated by %s on %s\n"
     "</body></html>\n",
     errcode, errname, errname, reason, pkgname, rfc1123_date(time(NULL)));
    free(reason);

    conn->header_length = xasprintf(&(conn->header),
     "HTTP/1.1 %d %s\r\n"
     "Date: %s\r\n"
     "Server: %s\r\n"
     "Connection: close\r\n" /* FIXME: remove for keepalive */
     "Content-Length: %d\r\n"
     "Content-Type: text/html\r\n"
     "\r\n",
     errcode, errname, rfc1123_date(time(NULL)), pkgname,
     conn->reply_length);

    conn->reply_type = REPLY_GENERATED;
    conn->http_code = errcode;
}



/* ---------------------------------------------------------------------------
 * Parses a single HTTP request field.  Returns string from end of [field] to
 * first \r, \n or end of request string.  Returns NULL if [field] can't be
 * matched.
 *
 * You need to remember to deallocate the result.
 * example: parse_field(conn, "Referer: ");
 */
static char *parse_field(const struct connection *conn, const char *field)
{
    unsigned int bound1, bound2;
    char *pos;

    /* find start */
    pos = strstr(conn->request, field);
    if (pos == NULL) return NULL;
    bound1 = pos - conn->request + strlen(field);

    /* find end */
    for (bound2 = bound1;
        conn->request[bound2] != '\r' &&
        bound2 < conn->request_length; bound2++)
            ;

    /* copy to buffer */
    return split_string(conn->request, bound1, bound2);
}



/* ---------------------------------------------------------------------------
 * Parse an HTTP request like "GET / HTTP/1.1" to get the method (GET), the
 * url (/), the referer (if given) and the user-agent (if given).  Remember to
 * deallocate all these buffers.  The method will be returned in uppercase.
 */
static void parse_request(struct connection *conn)
{
    unsigned int bound1, bound2;
    assert(conn->request_length == strlen(conn->request));

    /* parse method */
    for (bound1 = 0; bound1 < conn->request_length &&
        conn->request[bound1] != ' '; bound1++);

    conn->method = split_string(conn->request, 0, bound1);
    strntoupper(conn->method, bound1);

    /* parse uri */
    for (bound2=bound1+1; bound2 < conn->request_length &&
        conn->request[bound2] != ' ' &&
        conn->request[bound2] != '\r'; bound2++);

    conn->uri = split_string(conn->request, bound1+1, bound2);

    /* parse referer, user_agent */
    conn->referer = parse_field(conn, "Referer: ");
    conn->user_agent = parse_field(conn, "User-Agent: ");
}



/* ---------------------------------------------------------------------------
 * Process a GET/HEAD request
 */
static void process_get(struct connection *conn)
{
    char *decoded_url, *safe_url, *target, *if_mod_since;
    const char *mimetype = NULL;
    struct stat filestat;

    /* FIXME */
    printf("-----\n%s-----\n\n", conn->request);

    /* work out path of file being requested */
    decoded_url = urldecode(conn->uri);

    /* make sure it's safe */
    safe_url = make_safe_uri(decoded_url);
    free(decoded_url); decoded_url = NULL;
    if (safe_url == NULL)
    {
        default_reply(conn, 400, "Bad Request",
            "You requested an invalid URI: %s", conn->uri);
        return;
    }

    /* does it end in a slash? serve up url/index_name */
    if (safe_url[strlen(safe_url)-1] == '/')
    {
        xasprintf(&target, "%s%s%s", wwwroot, safe_url, index_name);
        mimetype = uri_content_type(index_name);
    }
    else /* points to a file */
    {
        xasprintf(&target, "%s%s", wwwroot, safe_url);
        mimetype = uri_content_type(safe_url);
    }
    free(safe_url); safe_url = NULL;

    debugf("uri=%s, target=%s, content-type=%s\n",
        conn->uri, target, mimetype);
    conn->reply_file = fopen(target, "rb");
    free(target); target = NULL;

    if (conn->reply_file == NULL)
    {
        /* fopen() failed */
        if (errno == ENOENT)
            default_reply(conn, 404, "Not Found",
                "The URI you requested (%s) was not found.", conn->uri);
        else
            default_reply(conn, 403, "Forbidden",
                "The URI you requested (%s) cannot be returned.<br>\n"
                "%s.", /* reason why */
                conn->uri, strerror(errno));

        return;
    }

    /* get information on the file */
    if (fstat(fileno(conn->reply_file), &filestat) == -1)
    {
        default_reply(conn, 500, "Internal Server Error",
            "fstat() failed: %s.", strerror(errno));
        return;
    }

    conn->reply_type = REPLY_FROMFILE;
    conn->reply_length = filestat.st_size;
    conn->lastmod = xstrdup(rfc1123_date(filestat.st_mtime));

    /* check for If-Modified-Since, may not have to send */
    if_mod_since = parse_field(conn, "If-Modified-Since: ");
    if (if_mod_since != NULL &&
        strcmp(if_mod_since, conn->lastmod) == 0)
    {
        debugf("not modified since %s\n", if_mod_since);
        default_reply(conn, 304, "Not Modified", "");
        conn->header_only = 1;
        return;
    }

    conn->header_length = xasprintf(&(conn->header),
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Server: %s\r\n"
        "Connection: close\r\n" /* FIXME: remove this for keepalive */
        "Content-Length: %d\r\n"
        "Content-Type: %s\r\n"
        "Last-Modified: %s\r\n"
        "\r\n"
        ,
        rfc1123_date(time(NULL)), pkgname, conn->reply_length,
        mimetype, conn->lastmod
    );
    conn->http_code = 200;
}



/* ---------------------------------------------------------------------------
 * Process a request: build the header and reply, advance state.
 */
static void process_request(struct connection *conn)
{
    parse_request(conn);

    if      (strcmp(conn->method, "GET") == 0)
    {
        process_get(conn);
    }
    else if (strcmp(conn->method, "HEAD") == 0)
    {
        process_get(conn);
        conn->header_only = 1;
    }
    else if (strcmp(conn->method, "OPTIONS") == 0 ||
             strcmp(conn->method, "POST") == 0 ||
             strcmp(conn->method, "PUT") == 0 ||
             strcmp(conn->method, "DELETE") == 0 ||
             strcmp(conn->method, "TRACE") == 0 ||
             strcmp(conn->method, "CONNECT") == 0)
    {
        default_reply(conn, 501, "Not Implemented",
            "The method you specified (%s) is not implemented.",
            conn->method);
    }
    else
    {
        default_reply(conn, 400, "Bad Request",
            "%s is not a valid HTTP/1.1 method.", conn->method);
    }

    /* advance state */
    conn->state = SEND_HEADER;

    /* request not needed anymore */
    free(conn->request); conn->request = NULL;
}



/* ---------------------------------------------------------------------------
 * Receiving request.
 */
static void poll_recv_request(struct connection *conn)
{
    #define BUFSIZE 65536
    char buf[BUFSIZE];
    ssize_t recvd;

    recvd = recv(conn->socket, buf, BUFSIZE, 0);
    debugf("poll_recv_request(%d) got %d bytes\n", conn->socket, recvd);
    if (recvd == -1) err(1, "recv()");
    if (recvd == 0)
    {
        /* socket closed on us */
        conn->state = DONE;
        return;
    }
    conn->last_active = time(NULL);
    #undef BUFSIZE

    /* append to conn->request */
    conn->request = xrealloc(conn->request, conn->request_length+recvd+1);
    memcpy(conn->request+conn->request_length, buf, (size_t)recvd);
    conn->request_length += recvd;
    conn->request[conn->request_length] = 0;

    /* process request if we have all of it */
    if (conn->request_length > 4 &&
        memcmp(conn->request+conn->request_length-4, "\r\n\r\n", 4) == 0)
        process_request(conn);

    /* die if it's too long */
    if (conn->request_length > MAX_REQUEST_LENGTH)
    {
        default_reply(conn, 413, "Request Entity Too Large",
            "Your request was dropped because it was too long.");
        conn->state = SEND_HEADER;
    }
}



/* ---------------------------------------------------------------------------
 * Sending header.  Assumes conn->header is not NULL.
 */
static void poll_send_header(struct connection *conn)
{
    ssize_t sent;

    assert(conn->header_length == strlen(conn->header));

    sent = send(conn->socket, conn->header + conn->header_sent,
        conn->header_length - conn->header_sent, 0);
    conn->last_active = time(NULL);
    debugf("poll_send_header(%d) sent %d bytes\n", conn->socket, sent);

    /* handle any errors (-1) or closure (0) in send() */
    if (sent < 1)
    {
        if (sent == -1) debugf("send() error: %s\n", strerror(errno));
        conn->state = DONE;
        return;
    }
    conn->header_sent += (unsigned int)sent;
    conn->total_sent += (unsigned int)sent;

    /* check if we're done sending */
    if (conn->header_sent == conn->header_length)
    {
        if (!conn->header_dont_free) free(conn->header);
        conn->header = NULL;

        if (conn->header_only)
            conn->state = DONE;
        else
            conn->state = SEND_REPLY;
    }
}



/* ---------------------------------------------------------------------------
 * Sending reply.
 */
static void poll_send_reply(struct connection *conn)
{
    ssize_t sent;

    assert( (conn->reply_type == REPLY_GENERATED && 
        conn->reply_length == strlen(conn->reply)) ||
        conn->reply_type == REPLY_FROMFILE);

    if (conn->reply_type == REPLY_GENERATED)
    {
        sent = send(conn->socket, conn->reply + conn->reply_sent,
            conn->reply_length - conn->reply_sent, 0);
    }
    else
    {
        /* from file! */
        #define BUFSIZE 65000
        char buf[BUFSIZE];
        size_t amount = min(BUFSIZE, conn->reply_length - conn->reply_sent);
        #undef BUFSIZE

        if (fseek(conn->reply_file, (long)conn->reply_sent, SEEK_SET) == -1)
            err(1, "fseek(%d)", conn->reply_sent);

        if (fread(buf, amount, 1, conn->reply_file) != 1)
            err(1, "fread()");

        sent = send(conn->socket, buf, amount, 0);
    }
    conn->last_active = time(NULL);
    debugf("poll_send_reply(%d) sent %d bytes [%d to %d]\n",
        conn->socket, sent, conn->reply_sent, conn->reply_sent+sent-1);

    /* handle any errors (-1) or closure (0) in send() */
    if (sent < 1)
    {
        if (sent == -1) debugf("send() error: %s\n", strerror(errno));
        conn->state = DONE;
        return;
    }
    conn->reply_sent += (unsigned int)sent;
    conn->total_sent += (unsigned int)sent;

    /* check if we're done sending */
    if (conn->reply_sent == conn->reply_length)
    {
        if (!conn->reply_dont_free && conn->reply != NULL)
        {
            free(conn->reply);
            conn->reply = NULL;
        }
        if (conn->reply_file != NULL) fclose(conn->reply_file);
        conn->state = DONE;
    }
}



/* ---------------------------------------------------------------------------
 * Add a connection's details to the logfile.
 */
static void log_connection(const struct connection *conn)
{
    struct in_addr inaddr;

    assert(conn->http_code != 0);
    if (logfile == NULL) return;

    /* Separated by tabs:
     * time client_ip method uri http_code bytes_sent "referer" "user-agent"
     */

    inaddr.s_addr = conn->client;

    fprintf(logfile, "%lu\t%s\t%s\t%s\t%d\t%u\t\"%s\"\t\"%s\"\n",
        time(NULL), inet_ntoa(inaddr), conn->method, conn->uri,
        conn->http_code, conn->total_sent,
        (conn->referer == NULL)?"":conn->referer,
        (conn->user_agent == NULL)?"":conn->user_agent
        );
    fflush(logfile);
}



/* ---------------------------------------------------------------------------
 * Main loop of the httpd - a select() and then delegation to accept
 * connections, handle receiving of requests, and sending of replies.
 */
static void httpd_poll(void)
{
    fd_set recv_set, send_set;
    int max_fd, select_ret;
    struct connection *conn;
    struct timeval timeout = { IDLETIME, 0 };
    int bother_with_timeout = 0;

    FD_ZERO(&recv_set);
    FD_ZERO(&send_set);
    max_fd = 0;

    /* set recv/send fd_sets */
    #define MAX_FD_SET(sock, fdset) FD_SET(sock,fdset), \
                                    max_fd = (max_fd<sock) ? sock : max_fd

    MAX_FD_SET(sockin, &recv_set);

    LIST_FOREACH(conn, &connlist, entries)
    {
        poll_check_timeout(conn);
        switch (conn->state)
        {
        case RECV_REQUEST:
            MAX_FD_SET(conn->socket, &recv_set);
            bother_with_timeout = 1;
            break;

        case SEND_HEADER:
        case SEND_REPLY:
            MAX_FD_SET(conn->socket, &send_set);
            bother_with_timeout = 1;
            break;

        case DONE:
            /* clean out stale connections while we're at it */
            LIST_REMOVE(conn, entries);
            log_connection(conn);
            free_connection(conn);
            free(conn);
            break;

        default: errx(1, "invalid state");
        }
    }
    #undef MAX_FD_SET

    debugf("select("), fflush(stdout);
    select_ret = select(max_fd + 1, &recv_set, &send_set, NULL,
        (bother_with_timeout) ? &timeout : NULL);
    if (select_ret == 0)
    {
        if (!bother_with_timeout)
            errx(1, "select() timed out");
        else
            return;
    }
    if (select_ret == -1) err(1, "select()");
    debugf(")\n");

    /* poll connections that select() says need attention */
    if (FD_ISSET(sockin, &recv_set)) accept_connection();

    LIST_FOREACH(conn, &connlist, entries)
    switch (conn->state)
    {
    case RECV_REQUEST:
        if (FD_ISSET(conn->socket, &recv_set)) poll_recv_request(conn);
        break;

    case SEND_HEADER:
        if (FD_ISSET(conn->socket, &send_set)) poll_send_header(conn);
        break;

    case SEND_REPLY:
        if (FD_ISSET(conn->socket, &send_set)) poll_send_reply(conn);
        break;

    default: errx(1, "invalid state");
    }
}



/* ---------------------------------------------------------------------------
 * Execution starts here.
 */
int main(int argc, char *argv[])
{
    printf("%s, %s.\n", pkgname, copyright);
    parse_commandline(argc, argv);
    parse_default_extension_map();
    init_sockin();

    /* open logfile */
    if (logfile_name != NULL)
    {
        logfile = fopen(logfile_name, "ab");
        if (logfile == NULL) err(1, "fopen(\"%s\")", logfile_name);
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        err(1, "signal(ignore SIGPIPE)");

    for (;;) httpd_poll();

    (void) close(sockin); /* unreachable =/ fix later */
    return 0;
}

/* vim:set tabstop=4 shiftwidth=4 expandtab tw=78: */
