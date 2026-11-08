/* std/http — runtime HTTP/1.1 untuk tree-lang (model fasthttp).
 *
 *   thread I/O  : satu thread, socket non-blocking + poll; parsing HTTP/1.1 tanpa alokasi per request
 *                 (span ke buffer baca koneksi), keep-alive + pipelining, file statis, error protokol.
 *   goroutine   : worker tree-lang mengambil request lewat channel runtime (tl_http_next → parkir),
 *                 menjalankan middleware + handler, lalu tl_http_finish → respons dikirim thread I/O.
 *
 * Satu request per koneksi berjalan pada satu waktu: selama request dipegang worker, thread I/O tidak
 * menyentuh koneksi itu (tidak membaca socket), jadi span ke buffer tetap valid tanpa kunci.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#define IS_DIR(m) (((m) & _S_IFDIR) != 0)
#else
#define IS_DIR(m) S_ISDIR(m)
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32")
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define poll_fn WSAPoll
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
typedef CRITICAL_SECTION mtx_t;
#define mtx_init(m) InitializeCriticalSection(m)
#define mtx_lock(m) EnterCriticalSection(m)
#define mtx_unlock(m) LeaveCriticalSection(m)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define poll_fn poll
#define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
typedef pthread_mutex_t mtx_t;
#define mtx_init(m) pthread_mutex_init(m, NULL)
#define mtx_lock(m) pthread_mutex_lock(m)
#define mtx_unlock(m) pthread_mutex_unlock(m)
#endif

/* ---- dari runtime tree-lang ---- */
typedef struct {
    char* data;
    int64_t len;
    int64_t cap;
} tl_dyn;
int32_t tl_bytes_put(tl_dyn* out, const void* p, int64_t n);
int64_t tl_chan_new(int64_t elem, int64_t cap);
int32_t tl_chan_recv(int64_t h, void* data);
int32_t tl_chan_offer(int64_t h, const void* data);
void tl_ext_hold(int32_t delta);
int64_t tl_os_clock_ns(void);

/* ---------------- batas ---------------- */
#define MAX_HEADER_BYTES (8 * 1024)
#define MAX_HEADERS 100
#define MAX_PARAMS 16
#define MAX_CONNS 65536
#define NMETH 7
enum { M_GET, M_HEAD, M_POST, M_PUT, M_DELETE, M_PATCH, M_OPTIONS };
static const char* meth_names[NMETH] = {"GET", "HEAD", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};

typedef struct {
    const char* p;
    int32_t n;
} span;

/* ---------------- buffer sederhana (malloc, milik runtime http) ---------------- */
typedef struct {
    char* p;
    size_t n, cap;
} buf;

static int buf_reserve(buf* b, size_t need) {
    if (need <= b->cap) return 1;
    size_t c = b->cap ? b->cap * 2 : 1024;
    while (c < need) c *= 2;
    char* np = (char*)realloc(b->p, c);
    if (!np) return 0;
    b->p = np;
    b->cap = c;
    return 1;
}
static int buf_put(buf* b, const void* p, size_t n) {
    if (!buf_reserve(b, b->n + n)) return 0;
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    return 1;
}
static int buf_puts(buf* b, const char* s) { return buf_put(b, s, strlen(s)); }
static void buf_free(buf* b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* ---------------- router ---------------- */
typedef struct route {
    void* handler;      /* func(&Ctx) dari tree-lang (hanya disimpan & dikembalikan) */
    int* mw;            /* indeks ke srv->mws */
    int nmw;
    int* after;         /* indeks ke srv->afters */
    int nafter;
} route;

typedef struct node {
    char* seg;          /* segmen statis */
    int kind;           /* 0 statis, 1 :param, 2 * wildcard */
    char* pname;
    int optional;       /* :param? */
    struct node** kids;
    int nkids;
    int route[NMETH];   /* indeks ke srv->routes, -1 = tidak ada */
} node;

typedef struct {
    char* prefix;
    void* fn;
} hook;

typedef struct {
    char* prefix;       /* URL */
    char* dir;          /* folder */
} static_dir;

typedef struct server {
    node* root;
    route* routes;
    int nroutes;
    hook* mws;
    int nmws;
    hook* afters;
    int nafters;
    char** prefixes;    /* grup: prefix lengkap per id */
    int nprefixes;
    static_dir* statics;
    int nstatics;
    int64_t max_body;
    int64_t read_timeout_ms;
    int64_t handler_timeout_ms;
    int64_t chan;       /* Chan[i64] runtime: request siap → worker */
    int64_t stop_chan;
    sock_t lsock;
    sock_t wake_r, wake_w;
    struct sockaddr_in wake_addr;
    mtx_t done_lock;
    int* done;          /* koneksi yang responsnya siap */
    int ndone, capdone;
    int listening;
} server;

#define MAX_SERVERS 16
static server* servers[MAX_SERVERS];
static int nservers;

/* ---------------- koneksi ---------------- */
enum { C_READ, C_FLIGHT, C_WRITE, C_CLOSE_AFTER_WRITE };

typedef struct conn {
    server* srv;
    sock_t s;
    uint32_t gen;
    int state;
    buf in;             /* data mentah dari socket */
    size_t consumed;    /* byte request saat ini (termasuk body) di 'in' */
    buf out;            /* respons yang sedang dikirim */
    size_t sent;
    int64_t last_ms;
    int64_t flight_ms;
    int abandoned;      /* handler kelewat lama: socket ditutup, koneksi dibebaskan saat finish */
    int finished;       /* worker sudah memanggil finish (respons siap); diambil thread I/O */
    int pending;        /* request siap tetapi channel penuh: dicoba lagi */
    /* request yang sedang berjalan */
    int method;
    int head_only;
    int http10;
    int keep_alive;
    span target;
    char* path;         /* path ter-decode */
    int32_t path_len;
    int32_t path_cap;
    span query;
    span hname[MAX_HEADERS], hval[MAX_HEADERS];
    int nh;
    const char* body;
    int64_t body_len;
    buf chunked;        /* body chunked yang sudah di-decode */
    int kind;           /* 0 = ada route, 1 = 404, 2 = 405 */
    route* rt;
    span pname[MAX_PARAMS], pval[MAX_PARAMS];
    int np;
    char allow[64];
    int64_t start_ns;
    /* respons dari handler */
    int status;
    buf rhdr;           /* "Nama: nilai\r\n"... */
    buf rbody;
    int has_ctype;
} conn;

static conn* conns[MAX_CONNS];
static uint32_t conn_gens[MAX_CONNS];
static int conn_hi; /* indeks tertinggi yang pernah dipakai + 1 (hanya thread I/O) */

static int64_t now_ms(void) { return tl_os_clock_ns() / 1000000; }

/* ---------------- util ---------------- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int span_ieq(span a, const char* b, int64_t n) {
    if (a.n != n) return 0;
    for (int64_t i = 0; i < n; i++)
        if (lower((unsigned char)a.p[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}
static int span_eq(span a, const char* b, int64_t n) { return a.n == n && memcmp(a.p, b, (size_t)n) == 0; }
static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* decode %xx (dan '+' → spasi bila plus); -1 bila rusak */
static int64_t url_decode(const char* p, int64_t n, char* out, int plus) {
    int64_t o = 0;
    for (int64_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '%') {
            if (i + 2 >= n) return -1;
            int a = hexv((unsigned char)p[i + 1]), b = hexv((unsigned char)p[i + 2]);
            if (a < 0 || b < 0) return -1;
            out[o++] = (char)(a * 16 + b);
            i += 2;
        } else if (c == '+' && plus) {
            out[o++] = ' ';
        } else {
            out[o++] = c;
        }
    }
    return o;
}
static int put_dyn(tl_dyn* out, const char* p, int64_t n) {
    out->len = 0;
    return tl_bytes_put(out, p, n);
}
static int parse_i64(const char* p, int64_t n, int64_t* out) {
    if (n <= 0 || n > 19) return 0;
    int neg = 0;
    int64_t i = 0, v = 0;
    if (p[0] == '-') {
        neg = 1;
        i = 1;
        if (n == 1) return 0;
    }
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *out = neg ? -v : v;
    return 1;
}
static char* sdup(const char* p, int64_t n) {
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, p, (size_t)n);
    s[n] = 0;
    return s;
}

static const char* reason(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Status";
    }
}

/* Date: di-cache per detik (dipanggil dari banyak thread; ditulis ulang utuh) */
static void http_date(char out[40]) {
    time_t t = time(NULL);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(out, 40, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[g.tm_wday], g.tm_mday, mo[g.tm_mon],
             g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
}

/* ---------------- router ---------------- */
static node* node_new(void) {
    node* n = (node*)calloc(1, sizeof(node));
    for (int i = 0; i < NMETH; i++) n->route[i] = -1;
    return n;
}

/* pecah path "/a/b/c" menjadi segmen (tanpa '/' awal/akhir) */
static int split(const char* p, int64_t n, span* segs, int max) {
    int k = 0;
    int64_t i = 0;
    while (i < n) {
        while (i < n && p[i] == '/') i++;
        if (i >= n) break;
        int64_t j = i;
        while (j < n && p[j] != '/') j++;
        if (k >= max) return -1;
        segs[k].p = p + i;
        segs[k].n = (int32_t)(j - i);
        k++;
        i = j;
    }
    return k;
}

static int path_has_prefix(const char* path, int64_t n, const char* prefix) {
    size_t pl = strlen(prefix);
    while (pl > 0 && prefix[pl - 1] == '/') pl--;
    if (pl == 0) return 1;
    if ((size_t)n < pl || memcmp(path, prefix, pl) != 0) return 0;
    return (size_t)n == pl || path[pl] == '/';
}

#define MAX_SEGS 64

/* 0 ok, 1 pola rusak, 2 route ganda */
static int add_route(server* s, int method_mask, const char* pat, int64_t n, void* handler) {
    span segs[MAX_SEGS];
    int k = split(pat, n, segs, MAX_SEGS);
    if (k < 0) return 1;
    node* cur = s->root;
    for (int i = 0; i < k; i++) {
        span sg = segs[i];
        int kind = 0, opt = 0;
        const char* name = sg.p;
        int32_t nl = sg.n;
        if (sg.p[0] == ':') {
            kind = 1;
            name++;
            nl--;
            if (nl > 0 && name[nl - 1] == '?') {
                opt = 1;
                nl--;
                if (i != k - 1) return 1; /* :x? hanya di akhir */
            }
            if (nl <= 0) return 1;
        } else if (sg.n == 1 && sg.p[0] == '*') {
            kind = 2;
            if (i != k - 1) return 1;
        }
        node* nx = NULL;
        for (int c = 0; c < cur->nkids; c++) {
            node* kd = cur->kids[c];
            if (kd->kind != kind) continue;
            if (kind == 0 && strlen(kd->seg) == (size_t)sg.n && memcmp(kd->seg, sg.p, (size_t)sg.n) == 0) nx = kd;
            if (kind == 1 && strlen(kd->pname) == (size_t)nl && memcmp(kd->pname, name, (size_t)nl) == 0 &&
                kd->optional == opt)
                nx = kd;
            if (kind == 2) nx = kd;
            if (nx) break;
        }
        if (!nx) {
            nx = node_new();
            nx->kind = kind;
            nx->optional = opt;
            if (kind == 0) nx->seg = sdup(sg.p, sg.n);
            else nx->pname = kind == 1 ? sdup(name, nl) : sdup("*", 1);
            cur->kids = (node**)realloc(cur->kids, sizeof(node*) * (size_t)(cur->nkids + 1));
            cur->kids[cur->nkids++] = nx;
        }
        cur = nx;
    }
    /* route baru: middleware & after yang sudah terdaftar dengan prefix yang cocok */
    s->routes = (route*)realloc(s->routes, sizeof(route) * (size_t)(s->nroutes + 1));
    route* r = &s->routes[s->nroutes];
    memset(r, 0, sizeof *r);
    r->handler = handler;
    for (int i = 0; i < s->nmws; i++)
        if (path_has_prefix(pat, n, s->mws[i].prefix)) {
            r->mw = (int*)realloc(r->mw, sizeof(int) * (size_t)(r->nmw + 1));
            r->mw[r->nmw++] = i;
        }
    for (int i = 0; i < s->nafters; i++)
        if (path_has_prefix(pat, n, s->afters[i].prefix)) {
            r->after = (int*)realloc(r->after, sizeof(int) * (size_t)(r->nafter + 1));
            r->after[r->nafter++] = i;
        }
    int dup = 0;
    for (int m = 0; m < NMETH; m++)
        if ((method_mask >> m) & 1) {
            if (cur->route[m] >= 0) dup = 1;
            cur->route[m] = s->nroutes;
        }
    s->nroutes++;
    return dup ? 2 : 0;
}

/* cocokkan segmen; statis > :param > * ; parameter disimpan di c */
static node* match(node* nd, span* segs, int i, int k, conn* c) {
    if (i == k) {
        for (int m = 0; m < NMETH; m++)
            if (nd->route[m] >= 0) return nd;
        /* :param? kosong di akhir */
        for (int q = 0; q < nd->nkids; q++) {
            node* kd = nd->kids[q];
            if (kd->kind == 1 && kd->optional) {
                if (c->np < MAX_PARAMS) {
                    c->pname[c->np].p = kd->pname;
                    c->pname[c->np].n = (int32_t)strlen(kd->pname);
                    c->pval[c->np].p = "";
                    c->pval[c->np].n = 0;
                    c->np++;
                }
                return kd;
            }
            if (kd->kind == 2) {
                if (c->np < MAX_PARAMS) {
                    c->pname[c->np].p = "*";
                    c->pname[c->np].n = 1;
                    c->pval[c->np].p = "";
                    c->pval[c->np].n = 0;
                    c->np++;
                }
                return kd;
            }
        }
        return NULL;
    }
    for (int q = 0; q < nd->nkids; q++) {
        node* kd = nd->kids[q];
        if (kd->kind == 0 && strlen(kd->seg) == (size_t)segs[i].n && memcmp(kd->seg, segs[i].p, (size_t)segs[i].n) == 0) {
            node* r = match(kd, segs, i + 1, k, c);
            if (r) return r;
        }
    }
    for (int q = 0; q < nd->nkids; q++) {
        node* kd = nd->kids[q];
        if (kd->kind == 1) {
            int save = c->np;
            if (c->np < MAX_PARAMS) {
                c->pname[c->np].p = kd->pname;
                c->pname[c->np].n = (int32_t)strlen(kd->pname);
                c->pval[c->np] = segs[i];
                c->np++;
            }
            node* r = match(kd, segs, i + 1, k, c);
            if (r) return r;
            c->np = save;
        }
    }
    for (int q = 0; q < nd->nkids; q++) {
        node* kd = nd->kids[q];
        if (kd->kind == 2) {
            /* sisa path (dari segmen i sampai akhir) */
            const char* start = segs[i].p;
            const char* end = segs[k - 1].p + segs[k - 1].n;
            if (c->np < MAX_PARAMS) {
                c->pname[c->np].p = "*";
                c->pname[c->np].n = 1;
                c->pval[c->np].p = start;
                c->pval[c->np].n = (int32_t)(end - start);
                c->np++;
            }
            return kd;
        }
    }
    return NULL;
}

static void route_request(conn* c) {
    server* s = c->srv;
    span segs[MAX_SEGS];
    c->np = 0;
    c->rt = NULL;
    c->allow[0] = 0;
    int k = split(c->path, c->path_len, segs, MAX_SEGS);
    node* nd = k < 0 ? NULL : match(s->root, segs, 0, k, c);
    if (!nd) {
        c->kind = 1;
        return;
    }
    int m = c->method;
    int r = nd->route[m];
    if (r < 0 && m == M_HEAD) r = nd->route[M_GET];
    if (r < 0) {
        c->kind = 2;
        size_t o = 0;
        for (int i = 0; i < NMETH; i++)
            if (nd->route[i] >= 0 || (i == M_HEAD && nd->route[M_GET] >= 0)) {
                int w = snprintf(c->allow + o, sizeof c->allow - o, "%s%s", o ? ", " : "", meth_names[i]);
                if (w > 0) o += (size_t)w;
                if (o >= sizeof c->allow) break;
            }
        return;
    }
    c->kind = 0;
    c->rt = &s->routes[r];
}

/* ---------------- respons ---------------- */
static void reset_response(conn* c) {
    c->status = 200;
    c->rhdr.n = 0;
    c->rbody.n = 0;
    c->has_ctype = 0;
}

/* susun respons lengkap ke c->out (dipanggil worker atau thread I/O) */
static void build_response(conn* c) {
    char date[40], line[160];
    http_date(date);
    c->out.n = 0;
    c->sent = 0;
    int code = c->status;
    snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\nServer: tree-lang\r\nDate: %s\r\n", code, reason(code), date);
    buf_puts(&c->out, line);
    if (c->rhdr.n) buf_put(&c->out, c->rhdr.p, c->rhdr.n);
    int no_body = code == 204 || code == 304 || (code >= 100 && code < 200);
    if (!c->has_ctype && !no_body) buf_puts(&c->out, "Content-Type: text/plain; charset=utf-8\r\n");
    if (!no_body) {
        snprintf(line, sizeof line, "Content-Length: %llu\r\n", (unsigned long long)c->rbody.n);
        buf_puts(&c->out, line);
    }
    if (!c->keep_alive) buf_puts(&c->out, "Connection: close\r\n");
    else if (c->http10) buf_puts(&c->out, "Connection: keep-alive\r\n");
    buf_puts(&c->out, "\r\n");
    if (!c->head_only && !no_body && c->rbody.n) buf_put(&c->out, c->rbody.p, c->rbody.n);
}

/* respons error langsung dari thread I/O; koneksi ditutup setelah terkirim */
static void error_response(conn* c, int code) {
    reset_response(c);
    c->status = code;
    c->keep_alive = 0;
    c->head_only = 0;
    buf_puts(&c->rbody, reason(code));
    build_response(c);
    c->state = C_CLOSE_AFTER_WRITE;
}

/* ---------------- file statis ---------------- */
static const char* mime(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const char* map[][2] = {{".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
                                   {".css", "text/css; charset=utf-8"},   {".js", "text/javascript; charset=utf-8"},
                                   {".json", "application/json"},         {".txt", "text/plain; charset=utf-8"},
                                   {".png", "image/png"},                 {".jpg", "image/jpeg"},
                                   {".jpeg", "image/jpeg"},               {".gif", "image/gif"},
                                   {".svg", "image/svg+xml"},             {".ico", "image/x-icon"},
                                   {".wasm", "application/wasm"},         {".pdf", "application/pdf"},
                                   {".xml", "application/xml"},           {".webp", "image/webp"}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
#ifdef _WIN32
        if (_stricmp(dot, map[i][0]) == 0) return map[i][1];
#else
        if (strcasecmp(dot, map[i][0]) == 0) return map[i][1];
#endif
    return "application/octet-stream";
}

static span find_header(conn* c, const char* name) {
    span none = {NULL, -1};
    int64_t n = (int64_t)strlen(name);
    for (int i = 0; i < c->nh; i++)
        if (span_ieq(c->hname[i], name, n)) return c->hval[i];
    return none;
}

/* 1 bila dilayani sebagai file statis (respons sudah disusun) */
static int serve_static(conn* c) {
    server* s = c->srv;
    if (c->method != M_GET && c->method != M_HEAD) return 0;
    for (int i = 0; i < s->nstatics; i++) {
        static_dir* sd = &s->statics[i];
        if (!path_has_prefix(c->path, c->path_len, sd->prefix)) continue;
        size_t pl = strlen(sd->prefix);
        while (pl > 0 && sd->prefix[pl - 1] == '/') pl--;
        const char* rel = c->path + pl;
        int64_t rl = c->path_len - (int64_t)pl;
        /* tolak '..', '\', ':' dan NUL: tidak boleh keluar dari folder */
        for (int64_t j = 0; j < rl; j++) {
            char ch = rel[j];
            if (ch == '\\' || ch == ':' || ch == 0) return 0;
            if (ch == '.' && j + 1 < rl && rel[j + 1] == '.' && (j == 0 || rel[j - 1] == '/') &&
                (j + 2 == rl || rel[j + 2] == '/'))
                goto forbidden;
        }
        {
            char full[4096];
            int w = snprintf(full, sizeof full, "%s%.*s", sd->dir, (int)rl, rel);
            if (w <= 0 || (size_t)w >= sizeof full - 16) return 0;
            struct stat st;
            if (stat(full, &st) == 0 && IS_DIR(st.st_mode)) {
                size_t L = strlen(full);
                snprintf(full + L, sizeof full - L, "%sindex.html", (L && full[L - 1] == '/') ? "" : "/");
                if (stat(full, &st) != 0) return 0;
            } else if (stat(full, &st) != 0) {
                return 0; /* lanjut ke router (mungkin 404) */
            }
            reset_response(c);
            char lm[40];
            struct tm g;
            time_t mt = st.st_mtime;
#ifdef _WIN32
            gmtime_s(&g, &mt);
#else
            gmtime_r(&mt, &g);
#endif
            strftime(lm, sizeof lm, "%a, %d %b %Y %H:%M:%S GMT", &g);
            span ims = find_header(c, "If-Modified-Since");
            buf_puts(&c->rhdr, "Last-Modified: ");
            buf_puts(&c->rhdr, lm);
            buf_puts(&c->rhdr, "\r\n");
            if (ims.n > 0 && span_eq(ims, lm, (int64_t)strlen(lm))) {
                c->status = 304;
            } else {
                FILE* f = fopen(full, "rb");
                if (!f) return 0;
                char tmp[1 << 15];
                size_t nr;
                while ((nr = fread(tmp, 1, sizeof tmp, f)) > 0) buf_put(&c->rbody, tmp, nr);
                fclose(f);
                buf_puts(&c->rhdr, "Content-Type: ");
                buf_puts(&c->rhdr, mime(full));
                buf_puts(&c->rhdr, "\r\n");
                c->has_ctype = 1;
            }
            build_response(c);
            c->state = C_WRITE;
            return 1;
        }
    forbidden:
        reset_response(c);
        c->status = 403;
        buf_puts(&c->rbody, "Forbidden");
        build_response(c);
        c->state = C_WRITE;
        return 1;
    }
    return 0;
}

/* ---------------- parser ---------------- */
/* 1 = request lengkap, 0 = butuh data, <0 = kode error HTTP (-400, -413, ...) */
static int parse_request(conn* c) {
    const char* b = c->in.p;
    size_t n = c->in.n;
    /* lewati CRLF kosong di antara request (RFC 7230 3.5) */
    size_t start = 0;
    while (start + 1 < n && b[start] == '\r' && b[start + 1] == '\n') start += 2;
    if (start) {
        memmove(c->in.p, c->in.p + start, n - start);
        c->in.n = n -= start;
    }
    size_t lim = n < MAX_HEADER_BYTES + 4 ? n : MAX_HEADER_BYTES + 4;
    size_t hend = 0;
    for (size_t i = 3; i < lim; i++)
        if (b[i - 3] == '\r' && b[i - 2] == '\n' && b[i - 1] == '\r' && b[i] == '\n') {
            hend = i + 1;
            break;
        }
    if (!hend) return n > MAX_HEADER_BYTES ? -431 : 0;
    /* request line */
    size_t p = 0;
    size_t ms = p;
    while (p < hend && b[p] != ' ') p++;
    size_t me = p;
    if (p >= hend || me == ms) return -400;
    p++;
    size_t ts = p;
    while (p < hend && b[p] != ' ' && b[p] != '\r') p++;
    size_t te = p;
    if (p >= hend || b[p] != ' ' || te == ts) return -400;
    p++;
    size_t vs = p;
    while (p < hend && b[p] != '\r') p++;
    if (p + 1 >= hend || b[p + 1] != '\n') return -400;
    span ver = {b + vs, (int32_t)(p - vs)};
    if (span_eq(ver, "HTTP/1.1", 8)) c->http10 = 0;
    else if (span_eq(ver, "HTTP/1.0", 8)) c->http10 = 1;
    else if (ver.n >= 5 && memcmp(ver.p, "HTTP/", 5) == 0) return -505;
    else return -400;
    p += 2;
    span meth = {b + ms, (int32_t)(me - ms)};
    c->method = -1;
    for (int i = 0; i < NMETH; i++)
        if (span_eq(meth, meth_names[i], (int64_t)strlen(meth_names[i]))) c->method = i;
    if (c->method < 0) {
        for (int32_t i = 0; i < meth.n; i++)
            if (meth.p[i] < 'A' || meth.p[i] > 'Z') return -400;
        return -501;
    }
    c->head_only = c->method == M_HEAD;
    c->target.p = b + ts;
    c->target.n = (int32_t)(te - ts);
    /* header */
    c->nh = 0;
    while (p < hend - 2) {
        size_t ls = p;
        while (p < hend && b[p] != '\r') p++;
        if (p + 1 >= hend || b[p + 1] != '\n') return -400;
        size_t le = p;
        p += 2;
        if (le == ls) break;
        size_t colon = ls;
        while (colon < le && b[colon] != ':') colon++;
        if (colon == le || colon == ls) return -400;
        for (size_t q = ls; q < colon; q++)
            if (b[q] == ' ' || b[q] == '\t') return -400; /* spasi sebelum ':' dilarang */
        if (c->nh >= MAX_HEADERS) return -431;
        size_t vs2 = colon + 1, ve = le;
        while (vs2 < ve && (b[vs2] == ' ' || b[vs2] == '\t')) vs2++;
        while (ve > vs2 && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
        c->hname[c->nh].p = b + ls;
        c->hname[c->nh].n = (int32_t)(colon - ls);
        c->hval[c->nh].p = b + vs2;
        c->hval[c->nh].n = (int32_t)(ve - vs2);
        c->nh++;
    }
    /* keep-alive */
    span conh = find_header(c, "Connection");
    if (c->http10) c->keep_alive = conh.n >= 0 && span_ieq(conh, "keep-alive", 10);
    else c->keep_alive = !(conh.n >= 0 && span_ieq(conh, "close", 5));
    /* body */
    span cl = find_header(c, "Content-Length");
    span te2 = find_header(c, "Transfer-Encoding");
    c->body = b + hend;
    c->body_len = 0;
    if (te2.n >= 0) {
        if (cl.n >= 0) return -400;
        if (!span_ieq(te2, "chunked", 7)) return -501;
        /* decode chunked; data harus lengkap di buffer */
        c->chunked.n = 0;
        size_t q = hend;
        for (;;) {
            size_t ls = q;
            while (q + 1 < n && !(b[q] == '\r' && b[q + 1] == '\n')) q++;
            if (q + 1 >= n) return 0;
            int64_t sz = 0;
            size_t k = ls;
            int digits = 0;
            for (; k < q && b[k] != ';'; k++) {
                int h = hexv((unsigned char)b[k]);
                if (h < 0) return -400;
                if (++digits > 15) return -413;
                sz = sz * 16 + h;
            }
            if (!digits) return -400;
            q += 2;
            if (sz == 0) {
                /* trailer (diabaikan) sampai baris kosong */
                for (;;) {
                    size_t ts2 = q;
                    while (q + 1 < n && !(b[q] == '\r' && b[q + 1] == '\n')) q++;
                    if (q + 1 >= n) return 0;
                    int empty = q == ts2;
                    q += 2;
                    if (empty) break;
                }
                break;
            }
            if ((int64_t)c->chunked.n + sz > c->srv->max_body) return -413;
            if (q + (size_t)sz + 2 > n) return 0;
            if (b[q + sz] != '\r' || b[q + sz + 1] != '\n') return -400;
            buf_put(&c->chunked, b + q, (size_t)sz);
            q += (size_t)sz + 2;
        }
        c->body = c->chunked.p ? c->chunked.p : "";
        c->body_len = (int64_t)c->chunked.n;
        c->consumed = q;
    } else {
        if (cl.n >= 0) {
            int64_t len;
            if (!parse_i64(cl.p, cl.n, &len) || len < 0) return -400;
            if (len > c->srv->max_body) return -413;
            if ((int64_t)(n - hend) < len) {
                /* Expect: 100-continue → biarkan klien mengirim body */
                span ex = find_header(c, "Expect");
                if (ex.n >= 0 && span_ieq(ex, "100-continue", 12) && n == hend) {
                    static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                    send(c->s, cont, (int)sizeof cont - 1, 0);
                }
                return 0;
            }
            c->body_len = len;
        }
        c->consumed = hend + (size_t)c->body_len;
    }
    /* target: origin-form atau absolute-form */
    const char* tp = c->target.p;
    int64_t tn = c->target.n;
    if (tn >= 7 && (memcmp(tp, "http://", 7) == 0 || (tn >= 8 && memcmp(tp, "https://", 8) == 0))) {
        int64_t i = tp[4] == 's' ? 8 : 7;
        while (i < tn && tp[i] != '/') i++;
        tp += i;
        tn -= i;
        if (tn == 0) {
            tp = "/";
            tn = 1;
        }
    }
    if (!(tn == 1 && tp[0] == '*') && (tn == 0 || tp[0] != '/')) return -400;
    int64_t qpos = 0;
    while (qpos < tn && tp[qpos] != '?' && tp[qpos] != '#') qpos++;
    c->query.p = qpos < tn && tp[qpos] == '?' ? tp + qpos + 1 : "";
    c->query.n = 0;
    if (qpos < tn && tp[qpos] == '?') {
        int64_t qe = qpos + 1;
        while (qe < tn && tp[qe] != '#') qe++;
        c->query.n = (int32_t)(qe - qpos - 1);
    }
    if (qpos + 1 > c->path_cap) {
        char* np = (char*)realloc(c->path, (size_t)qpos + 1);
        if (!np) return -500;
        c->path = np;
        c->path_cap = (int32_t)qpos + 1;
    }
    int64_t pl = url_decode(tp, qpos, c->path, 0);
    if (pl < 0) return -400;
    for (int64_t i = 0; i < pl; i++)
        if (c->path[i] == 0) return -400;
    /* hilangkan '/' di akhir (kecuali root) */
    while (pl > 1 && c->path[pl - 1] == '/') pl--;
    c->path_len = (int32_t)pl;
    return 1;
}

/* ---------------- loop I/O ---------------- */
static void conn_free(int idx) {
    conn* c = conns[idx];
    if (!c) return;
    if (c->s != BAD_SOCK) sock_close(c->s);
    buf_free(&c->in);
    buf_free(&c->out);
    buf_free(&c->chunked);
    buf_free(&c->rhdr);
    buf_free(&c->rbody);
    free(c->path);
    free(c);
    conns[idx] = NULL;
    conn_gens[idx]++;
}

static int64_t handle_of(int idx) { return ((int64_t)conn_gens[idx] << 20) | (int64_t)(idx + 1); }

static conn* conn_of(int64_t h) {
    int64_t idx = (h & 0xFFFFF) - 1;
    if (idx < 0 || idx >= MAX_CONNS) return NULL;
    conn* c = conns[idx];
    if (!c || (uint32_t)(h >> 20) != conn_gens[idx] || c->state != C_FLIGHT || c->finished) return NULL;
    return c;
}

/* request lengkap: file statis, atau ke worker lewat channel */
static void dispatch(int idx) {
    conn* c = conns[idx];
    if (serve_static(c)) return;
    route_request(c);
    reset_response(c);
    c->state = C_FLIGHT;
    c->finished = 0;
    c->flight_ms = now_ms();
    c->start_ns = tl_os_clock_ns();
    int64_t h = handle_of(idx);
    int r = tl_chan_offer(c->srv->chan, &h);
    c->pending = r == 0;
    if (r < 0) {
        c->state = C_READ;
        error_response(c, 503);
    }
}

/* coba parse request berikutnya di buffer (keep-alive / pipelining) */
static void advance(int idx) {
    conn* c = conns[idx];
    if (c->in.n == 0) {
        c->state = C_READ;
        return;
    }
    int r = parse_request(c);
    if (r == 0) {
        c->state = C_READ;
        return;
    }
    if (r < 0) {
        error_response(c, -r);
        return;
    }
    dispatch(idx);
}

static void after_write(int idx) {
    conn* c = conns[idx];
    if (c->state == C_CLOSE_AFTER_WRITE || !c->keep_alive) {
        conn_free(idx);
        return;
    }
    /* buang request yang selesai, lanjut ke data berikutnya */
    size_t used = c->consumed <= c->in.n ? c->consumed : c->in.n;
    memmove(c->in.p, c->in.p + used, c->in.n - used);
    c->in.n -= used;
    c->consumed = 0;
    c->out.n = 0;
    c->state = C_READ;
    advance(idx);
}

static void try_write(int idx) {
    conn* c = conns[idx];
    while (c->sent < c->out.n) {
        size_t left = c->out.n - c->sent;
        int w = send(c->s, c->out.p + c->sent, (int)(left > (1 << 30) ? (1 << 30) : left), 0);
        if (w < 0) {
            if (WOULD_BLOCK()) return;
            conn_free(idx);
            return;
        }
        c->sent += (size_t)w;
    }
    c->last_ms = now_ms();
    after_write(idx);
}

static void set_nonblock(sock_t s) {
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void accept_all(server* s) {
    for (;;) {
        sock_t a = accept(s->lsock, NULL, NULL);
        if (a == BAD_SOCK) return;
        int idx = -1;
        for (int i = 0; i < conn_hi; i++)
            if (!conns[i]) {
                idx = i;
                break;
            }
        if (idx < 0 && conn_hi < MAX_CONNS) idx = conn_hi++;
        if (idx < 0) {
            sock_close(a);
            return;
        }
        set_nonblock(a);
        int one = 1;
        setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
        conn* c = (conn*)calloc(1, sizeof(conn));
        if (!c) {
            sock_close(a);
            return;
        }
        c->srv = s;
        c->s = a;
        c->state = C_READ;
        c->last_ms = now_ms();
        c->gen = conn_gens[idx];
        conns[idx] = c;
    }
}

static void read_conn(int idx) {
    conn* c = conns[idx];
    for (;;) {
        size_t limit = (size_t)MAX_HEADER_BYTES + (size_t)c->srv->max_body + 64 * 1024;
        if (c->in.n >= limit) break;
        if (!buf_reserve(&c->in, c->in.n + 16 * 1024)) {
            conn_free(idx);
            return;
        }
        size_t room = c->in.cap - c->in.n;
        int r = recv(c->s, c->in.p + c->in.n, (int)(room > (1 << 30) ? (1 << 30) : room), 0);
        if (r == 0) {
            conn_free(idx);
            return;
        }
        if (r < 0) {
            if (WOULD_BLOCK()) break;
            conn_free(idx);
            return;
        }
        c->in.n += (size_t)r;
        c->last_ms = now_ms();
        if ((size_t)r < room) break;
    }
    advance(idx);
    if (conns[idx] && (conns[idx]->state == C_WRITE || conns[idx]->state == C_CLOSE_AFTER_WRITE)) try_write(idx);
}

static void drain_done(server* s) {
    char tmp[256];
    while (recv(s->wake_r, tmp, sizeof tmp, 0) > 0) {
    }
    mtx_lock(&s->done_lock);
    int n = s->ndone;
    int* list = (int*)malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (list && n) memcpy(list, s->done, sizeof(int) * (size_t)n);
    s->ndone = 0;
    mtx_unlock(&s->done_lock);
    for (int i = 0; list && i < n; i++) {
        int idx = list[i];
        conn* c = conns[idx];
        if (!c) continue;
        if (c->abandoned) {
            conn_free(idx);
            continue;
        }
        c->state = C_WRITE;
        try_write(idx);
    }
    free(list);
}

#ifdef _WIN32
static DWORD WINAPI io_thread(void* arg)
#else
static void* io_thread(void* arg)
#endif
{
    server* s = (server*)arg;
    struct pollfd* fds = (struct pollfd*)malloc(sizeof(struct pollfd) * (MAX_CONNS + 2));
    int* map = (int*)malloc(sizeof(int) * (MAX_CONNS + 2));
    for (;;) {
        int nf = 0;
        fds[nf].fd = s->wake_r;
        fds[nf].events = POLLIN;
        map[nf++] = -1;
        fds[nf].fd = s->lsock;
        fds[nf].events = POLLIN;
        map[nf++] = -2;
        int any_pending = 0;
        int64_t now = now_ms();
        for (int i = 0; i < conn_hi; i++) {
            conn* c = conns[i];
            if (!c) continue;
            if (c->srv != s) continue;
            if (c->state == C_FLIGHT) {
                if (c->pending) {
                    int64_t h = handle_of(i);
                    int r = tl_chan_offer(s->chan, &h);
                    if (r > 0) c->pending = 0;
                    else any_pending = 1;
                } else if (s->handler_timeout_ms > 0 && now - c->flight_ms > s->handler_timeout_ms && !c->abandoned) {
                    /* worker mati (abort) atau terlalu lama: tutup socket; koneksi dilepas saat finish */
                    c->abandoned = 1;
                    sock_close(c->s);
                    c->s = BAD_SOCK;
                }
                continue;
            }
            if (c->state == C_READ && s->read_timeout_ms > 0 && now - c->last_ms > s->read_timeout_ms) {
                conn_free(i);
                continue;
            }
            fds[nf].fd = c->s;
            fds[nf].events = (short)(c->state == C_READ ? POLLIN : POLLOUT);
            fds[nf].revents = 0;
            map[nf++] = i;
        }
        for (int i = 0; i < nf; i++) fds[i].revents = 0;
        int r = poll_fn(fds, (unsigned long)nf, any_pending ? 1 : 500);
        if (r < 0) continue;
        for (int i = 0; i < nf; i++) {
            if (!fds[i].revents) continue;
            int m = map[i];
            if (m == -1) drain_done(s);
            else if (m == -2) accept_all(s);
            else if (conns[m]) {
                if (conns[m]->state == C_READ) read_conn(m);
                else if (conns[m]->state == C_WRITE || conns[m]->state == C_CLOSE_AFTER_WRITE) {
                    if (fds[i].revents & (POLLERR | POLLHUP)) conn_free(m);
                    else try_write(m);
                }
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---------------- API untuk std/http (tree-lang) ---------------- */

static server* srv_of(int64_t h) { return h >= 1 && h <= nservers ? servers[h - 1] : NULL; }

int64_t tl_http_new(int64_t max_body, int64_t read_timeout_ms, int64_t handler_timeout_ms) {
#ifdef _WIN32
    static int wsa = 0;
    if (!wsa) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return 0;
        wsa = 1;
    }
#endif
    if (nservers >= MAX_SERVERS) return 0;
    server* s = (server*)calloc(1, sizeof(server));
    if (!s) return 0;
    s->root = node_new();
    s->max_body = max_body > 0 ? max_body : 4 << 20;
    s->read_timeout_ms = read_timeout_ms;
    s->handler_timeout_ms = handler_timeout_ms;
    s->lsock = BAD_SOCK;
    mtx_init(&s->done_lock);
    /* grup 0 = root */
    s->prefixes = (char**)malloc(sizeof(char*));
    s->prefixes[0] = sdup("", 0);
    s->nprefixes = 1;
    servers[nservers++] = s;
    return nservers;
}

/* prefix lengkap grup baru: parent + prefix */
int64_t tl_http_group(int64_t sh, int64_t parent, const char* p, int64_t n) {
    server* s = srv_of(sh);
    if (!s || parent < 0 || parent >= s->nprefixes) return 0;
    const char* base = s->prefixes[parent];
    size_t bl = strlen(base);
    char* full = (char*)malloc(bl + (size_t)n + 2);
    if (!full) return 0;
    memcpy(full, base, bl);
    size_t o = bl;
    if (n > 0 && p[0] != '/') full[o++] = '/';
    memcpy(full + o, p, (size_t)n);
    o += (size_t)n;
    while (o > 0 && full[o - 1] == '/') o--;
    full[o] = 0;
    s->prefixes = (char**)realloc(s->prefixes, sizeof(char*) * (size_t)(s->nprefixes + 1));
    s->prefixes[s->nprefixes] = full;
    return s->nprefixes++;
}

static char* join_path(server* s, int64_t group, const char* p, int64_t n) {
    const char* base = group >= 0 && group < s->nprefixes ? s->prefixes[group] : "";
    size_t bl = strlen(base);
    char* full = (char*)malloc(bl + (size_t)n + 2);
    if (!full) return NULL;
    memcpy(full, base, bl);
    size_t o = bl;
    if (n == 0 || p[0] != '/') full[o++] = '/';
    memcpy(full + o, p, (size_t)n);
    o += (size_t)n;
    full[o] = 0;
    return full;
}

/* method: 0..6, atau -1 = semua. 0 ok, 1 pola rusak, 2 route ganda, 3 sudah listen */
int64_t tl_http_route(int64_t sh, int64_t group, int64_t method, const char* p, int64_t n, void* handler) {
    server* s = srv_of(sh);
    if (!s) return 1;
    if (s->listening) return 3;
    char* full = join_path(s, group, p, n);
    if (!full) return 1;
    int mask = method < 0 ? (1 << NMETH) - 1 : (1 << method);
    int r = add_route(s, mask, full, (int64_t)strlen(full), handler);
    free(full);
    return r;
}

int64_t tl_http_use(int64_t sh, int64_t group, const char* p, int64_t n, void* fn, int64_t after) {
    server* s = srv_of(sh);
    if (!s) return 1;
    if (s->listening) return 3;
    char* full = join_path(s, group, p, n);
    if (!full) return 1;
    size_t L = strlen(full);
    while (L > 0 && full[L - 1] == '/') full[--L] = 0;
    hook** list = after ? &s->afters : &s->mws;
    int* cnt = after ? &s->nafters : &s->nmws;
    *list = (hook*)realloc(*list, sizeof(hook) * (size_t)(*cnt + 1));
    (*list)[*cnt].prefix = full;
    (*list)[*cnt].fn = fn;
    (*cnt)++;
    return 0;
}

int64_t tl_http_after(int64_t sh, int64_t group, const char* p, int64_t n, void* fn, int64_t after) {
    (void)after;
    return tl_http_use(sh, group, p, n, fn, 1);
}

int64_t tl_http_static(int64_t sh, int64_t group, const char* p, int64_t n, const char* d, int64_t dn) {
    server* s = srv_of(sh);
    if (!s || dn <= 0) return 1;
    char* full = join_path(s, group, p, n);
    if (!full) return 1;
    size_t L = strlen(full);
    while (L > 0 && full[L - 1] == '/') full[--L] = 0;
    char* dir = sdup(d, dn);
    size_t dl = strlen(dir);
    while (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) dir[--dl] = 0;
    s->statics = (static_dir*)realloc(s->statics, sizeof(static_dir) * (size_t)(s->nstatics + 1));
    s->statics[s->nstatics].prefix = full;
    s->statics[s->nstatics].dir = dir;
    s->nstatics++;
    return 0;
}

/* ":3000", "127.0.0.1:8080", "localhost:80". 1 = mendengarkan */
int32_t tl_http_listen(int64_t sh, const char* a, int64_t an, int64_t queue) {
    server* s = srv_of(sh);
    if (!s || s->listening || an <= 0 || an > 255) return 0;
    char addr[256];
    memcpy(addr, a, (size_t)an);
    addr[an] = 0;
    char* colon = strrchr(addr, ':');
    const char* port = colon ? colon + 1 : addr;
    const char* host = NULL;
    if (colon) {
        *colon = 0;
        if (addr[0]) host = addr;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return 0;
    sock_t ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ls == BAD_SOCK) {
        freeaddrinfo(res);
        return 0;
    }
#ifndef _WIN32
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#endif
    if (bind(ls, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(ls, 1024) != 0) {
        freeaddrinfo(res);
        sock_close(ls);
        return 0;
    }
    freeaddrinfo(res);
    set_nonblock(ls);
    s->lsock = ls;
    /* socket bangun: UDP loopback */
    s->wake_r = socket(AF_INET, SOCK_DGRAM, 0);
    s->wake_w = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&s->wake_addr, 0, sizeof s->wake_addr);
    s->wake_addr.sin_family = AF_INET;
    s->wake_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s->wake_addr.sin_port = 0;
    if (bind(s->wake_r, (struct sockaddr*)&s->wake_addr, sizeof s->wake_addr) != 0) return 0;
    int alen = sizeof s->wake_addr;
    getsockname(s->wake_r, (struct sockaddr*)&s->wake_addr, (void*)&alen);
    set_nonblock(s->wake_r);
    s->chan = tl_chan_new(8, queue > 0 ? queue : 4096);
    s->stop_chan = tl_chan_new(8, 1);
    s->listening = 1;
    tl_ext_hold(1);
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, io_thread, s, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, io_thread, s) != 0) return 0;
    pthread_detach(t);
#endif
    return 1;
}

/* log server harus langsung terlihat (stdout sering berupa pipe/file) */
void tl_http_flush(void) { fflush(stdout); }

/* port yang benar-benar dipakai (berguna untuk ":0") */
int64_t tl_http_port(int64_t sh) {
    server* s = srv_of(sh);
    if (!s || s->lsock == BAD_SOCK) return 0;
    struct sockaddr_in sa;
    int len = sizeof sa;
    if (getsockname(s->lsock, (struct sockaddr*)&sa, (void*)&len) != 0) return 0;
    return ntohs(sa.sin_port);
}

/* goroutine Listen menunggu di sini (parkir) sampai server berhenti */
void tl_http_wait(int64_t sh) {
    server* s = srv_of(sh);
    if (!s) return;
    int64_t v;
    tl_chan_recv(s->stop_chan, &v);
}

/* worker: request berikutnya (goroutine parkir sampai ada); 0 = server berhenti */
int64_t tl_http_next(int64_t sh) {
    server* s = srv_of(sh);
    if (!s) return 0;
    int64_t h = 0;
    if (!tl_chan_recv(s->chan, &h)) return 0;
    return h;
}

/* 0 = ada route, 1 = tidak ditemukan, 2 = method tidak diizinkan */
int64_t tl_http_kind(int64_t h) {
    conn* c = conn_of(h);
    return c ? c->kind : 1;
}

void* tl_http_handler(int64_t h) {
    conn* c = conn_of(h);
    return c && c->rt ? c->rt->handler : NULL;
}

/* middleware / after untuk request ini: untuk 404/405 hanya yang berlaku global ("/") */
static int global_hooks(server* s, int after, int k) {
    hook* list = after ? s->afters : s->mws;
    int n = after ? s->nafters : s->nmws;
    int seen = 0;
    for (int i = 0; i < n; i++)
        if (list[i].prefix[0] == 0) {
            if (seen == k) return i;
            seen++;
        }
    return -1;
}
int64_t tl_http_hook_count(int64_t h, int64_t after) {
    conn* c = conn_of(h);
    if (!c) return 0;
    if (c->rt) return after ? c->rt->nafter : c->rt->nmw;
    int k = 0;
    while (global_hooks(c->srv, (int)after, k) >= 0) k++;
    return k;
}
void* tl_http_hook(int64_t h, int64_t after, int64_t k) {
    conn* c = conn_of(h);
    if (!c || k < 0) return NULL;
    server* s = c->srv;
    hook* list = after ? s->afters : s->mws;
    if (c->rt) {
        int n = after ? c->rt->nafter : c->rt->nmw;
        if (k >= n) return NULL;
        return list[(after ? c->rt->after : c->rt->mw)[k]].fn;
    }
    int i = global_hooks(s, (int)after, (int)k);
    return i >= 0 ? list[i].fn : NULL;
}

void* tl_http_after_hook(int64_t h, int64_t after, int64_t k) {
    (void)after;
    return tl_http_hook(h, 1, k);
}

/* 404 / 405 bawaan (setelah middleware) */
void tl_http_default(int64_t h) {
    conn* c = conn_of(h);
    if (!c) return;
    c->status = c->kind == 2 ? 405 : 404;
    c->rbody.n = 0;
    buf_puts(&c->rbody, reason(c->status));
    if (c->kind == 2 && c->allow[0]) {
        buf_puts(&c->rhdr, "Allow: ");
        buf_puts(&c->rhdr, c->allow);
        buf_puts(&c->rhdr, "\r\n");
    }
}

int64_t tl_http_method(int64_t h) {
    conn* c = conn_of(h);
    return c ? c->method : 0;
}

int32_t tl_http_path(int64_t h, tl_dyn* out) {
    conn* c = conn_of(h);
    if (!c) return 0;
    return put_dyn(out, c->path, c->path_len);
}

int32_t tl_http_path_is(int64_t h, const char* p, int64_t n) {
    conn* c = conn_of(h);
    return c && c->path_len == n && memcmp(c->path, p, (size_t)n) == 0;
}

static int find_param(conn* c, const char* name, int64_t n, span* out) {
    for (int i = 0; i < c->np; i++)
        if (span_eq(c->pname[i], name, n)) {
            *out = c->pval[i];
            return 1;
        }
    return 0;
}

int32_t tl_http_param(int64_t h, const char* name, int64_t n, tl_dyn* out) {
    conn* c = conn_of(h);
    span v;
    out->len = 0;
    if (!c || !find_param(c, name, n, &v)) return 0;
    return put_dyn(out, v.p, v.n);
}

int32_t tl_http_param_int(int64_t h, const char* name, int64_t n, int64_t* out) {
    conn* c = conn_of(h);
    span v;
    if (!c || !find_param(c, name, n, &v)) return 0;
    return parse_i64(v.p, v.n, out);
}

/* nilai query ter-decode ke 'dst' (cap ≥ panjang mentah); -1 bila tidak ada */
static int64_t find_query(conn* c, const char* name, int64_t n, char* dst, int64_t cap) {
    const char* q = c->query.p;
    int64_t qn = c->query.n, i = 0;
    char key[256];
    while (i < qn) {
        int64_t s = i;
        while (i < qn && q[i] != '&') i++;
        int64_t e = i;
        i++;
        int64_t eq = s;
        while (eq < e && q[eq] != '=') eq++;
        int64_t kl = eq - s;
        if (kl <= 0 || kl > (int64_t)sizeof key) continue;
        int64_t dk = url_decode(q + s, kl, key, 1);
        if (dk != n || memcmp(key, name, (size_t)n) != 0) continue;
        int64_t vs = eq < e ? eq + 1 : e;
        if (e - vs > cap) return -1;
        return url_decode(q + vs, e - vs, dst, 1);
    }
    return -1;
}

int32_t tl_http_query(int64_t h, const char* name, int64_t n, tl_dyn* out) {
    conn* c = conn_of(h);
    out->len = 0;
    if (!c) return 0;
    char* tmp = (char*)malloc((size_t)c->query.n + 1);
    if (!tmp) return 0;
    int64_t r = find_query(c, name, n, tmp, c->query.n);
    int32_t ok = r >= 0 && put_dyn(out, tmp, r);
    free(tmp);
    return ok;
}

int32_t tl_http_query_int(int64_t h, const char* name, int64_t n, int64_t* out) {
    conn* c = conn_of(h);
    if (!c) return 0;
    char tmp[64];
    int64_t r = find_query(c, name, n, tmp, (int64_t)sizeof tmp);
    return r >= 0 && parse_i64(tmp, r, out);
}

int32_t tl_http_header(int64_t h, const char* name, int64_t n, tl_dyn* out) {
    conn* c = conn_of(h);
    out->len = 0;
    if (!c) return 0;
    for (int i = 0; i < c->nh; i++)
        if (span_ieq(c->hname[i], name, n)) return put_dyn(out, c->hval[i].p, c->hval[i].n);
    return 0;
}

int32_t tl_http_header_is(int64_t h, const char* name, int64_t n, const char* v, int64_t vn) {
    conn* c = conn_of(h);
    if (!c) return 0;
    for (int i = 0; i < c->nh; i++)
        if (span_ieq(c->hname[i], name, n)) return span_eq(c->hval[i], v, vn);
    return 0;
}

int32_t tl_http_body(int64_t h, tl_dyn* out) {
    conn* c = conn_of(h);
    out->len = 0;
    if (!c) return 0;
    return put_dyn(out, c->body, c->body_len);
}

int64_t tl_http_body_len(int64_t h) {
    conn* c = conn_of(h);
    return c ? c->body_len : 0;
}

void tl_http_status(int64_t h, int64_t code) {
    conn* c = conn_of(h);
    if (c && code >= 100 && code <= 999) c->status = (int)code;
}

int64_t tl_http_get_status(int64_t h) {
    conn* c = conn_of(h);
    return c ? c->status : 0;
}

/* header respons; CR/LF dalam nama/nilai ditolak (header injection) */
int32_t tl_http_set(int64_t h, const char* k, int64_t kn, const char* v, int64_t vn) {
    conn* c = conn_of(h);
    if (!c || kn <= 0) return 0;
    for (int64_t i = 0; i < kn; i++)
        if (k[i] == '\r' || k[i] == '\n' || k[i] == ':' || k[i] == ' ') return 0;
    for (int64_t i = 0; i < vn; i++)
        if (v[i] == '\r' || v[i] == '\n') return 0;
    if (kn == 14 && span_ieq((span){k, (int32_t)kn}, "Content-Length", 14)) return 0; /* diisi otomatis */
    if (kn == 12 && span_ieq((span){k, (int32_t)kn}, "Content-Type", 12)) c->has_ctype = 1;
    buf_put(&c->rhdr, k, (size_t)kn);
    buf_puts(&c->rhdr, ": ");
    buf_put(&c->rhdr, v, (size_t)vn);
    buf_puts(&c->rhdr, "\r\n");
    return 1;
}

void tl_http_send(int64_t h, const char* p, int64_t n, int64_t append) {
    conn* c = conn_of(h);
    if (!c) return;
    if (!append) c->rbody.n = 0;
    buf_put(&c->rbody, p, (size_t)n);
}

int64_t tl_http_elapsed_us(int64_t h) {
    conn* c = conn_of(h);
    return c ? (tl_os_clock_ns() - c->start_ns) / 1000 : 0;
}

/* handler selesai: susun respons di thread worker, serahkan ke thread I/O */
void tl_http_finish(int64_t h) {
    conn* c = conn_of(h);
    if (!c) return;
    server* s = c->srv;
    build_response(c);
    int idx = (int)((h & 0xFFFFF) - 1);
    mtx_lock(&s->done_lock);
    c->finished = 1;
    if (s->ndone == s->capdone) {
        s->capdone = s->capdone ? s->capdone * 2 : 256;
        s->done = (int*)realloc(s->done, sizeof(int) * (size_t)s->capdone);
    }
    s->done[s->ndone++] = idx;
    mtx_unlock(&s->done_lock);
    char one = 1;
    sendto(s->wake_w, &one, 1, 0, (struct sockaddr*)&s->wake_addr, sizeof s->wake_addr);
}

/* panic di handler: buang respons setengah jadi, kirim 500 */
void tl_http_fail(int64_t h, int64_t code) {
    conn* c = conn_of(h);
    if (!c) return;
    reset_response(c);
    c->status = (int)code;
    buf_puts(&c->rbody, reason((int)code));
    tl_http_finish(h);
}
