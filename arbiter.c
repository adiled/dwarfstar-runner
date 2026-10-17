/*
 * DwarfStar GPU arbiter.
 *
 * A small always-on HTTP process that lets an end-harness switch which ds4
 * model is serving without ever killing a process by hand. It is a pure client
 * of the orch toolchain:
 *
 *   roster : `orch parse servers/<name>.orch`          (name, port, health)
 *   start  : `orchd ... --state-dir ~/.orch/g/<name> grow`
 *   stop   : `orchd ... --state-dir ~/.orch/g/<name> fell --keep-data`
 *
 * Each model is its own grove: one spec file, one namespace, one state dir.
 * Because orchd's `fell` tears down whatever is in its state dir, giving each
 * model its own state dir is what makes `fell` stop exactly one model. The
 * arbiter lives in its own grove, so nothing it fells can stop the arbiter.
 *
 * The GPU itself is still guarded by ds4's own flock: every model spec sets the
 * same DS4_LOCK_FILE, so at most one model ever runs.
 *
 *   GET  /v1/models       -> every advertised id
 *   POST /v1/...          -> resolve "model" -> raise the grove -> relay
 */

#define _DARWIN_C_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tinyjson.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_MODELS 64
#define RECV_MAX 65536

typedef struct {
    char name[128];
    char file[PATH_MAX];
    char host[256];
    int  port;
    char health[512];
    char ids[1024];   /* comma-separated model ids this grove answers to */
} model_t;

static model_t models[MAX_MODELS];
static int     nmodels = 0;

static char g_dir[PATH_MAX];
static char g_orch[PATH_MAX];
static char g_orchd[PATH_MAX];
static char g_state_root[PATH_MAX];
static char g_listen_host[128] = "127.0.0.1";
static int  g_listen_port = 8899;
static int  g_timeout = 900;

static int parse_url(const char *url, char *host, size_t hostn,
                     int *port, char *path, size_t pathn);

static void logf_(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void expand_tilde(const char *in, char *out, size_t n) {
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        snprintf(out, n, "%s%s", home ? home : "/", in + 1);
    } else {
        snprintf(out, n, "%s", in);
    }
}

/* Run cmd, collect stdout. Returns exit code, or -1. *out is malloc'd. */
static int run_capture(const char *cmd, char **out) {
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(f); return -1; }
    size_t r;
    while ((r = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += r;
        if (cap - len < 2) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); pclose(f); return -1; }
            buf = tmp;
        }
    }
    buf[len] = '\0';
    int st = pclose(f);
    if (out) *out = buf; else free(buf);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Run cmd for side effects; returns exit code or -1. */
static int run_status(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Extract the first "--port N" from a run command. */
static int port_from_cmd(const char *cmd) {
    const char *p = cmd;
    while ((p = strstr(p, "--port")) != NULL) {
        const char *q = p + 6;
        while (*q == ' ' || *q == '=') q++;
        if (isdigit((unsigned char)*q)) return atoi(q);
        p = q;
    }
    return 0;
}

static int discover(char *err, size_t errn) {
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s/servers/*.orch", g_dir);

    glob_t gl;
    if (glob(pattern, 0, NULL, &gl) != 0) {
        snprintf(err, errn, "no specs matched %s", pattern);
        return -1;
    }

    for (size_t i = 0; i < gl.gl_pathc && nmodels < MAX_MODELS; i++) {
        const char *file = gl.gl_pathv[i];
        char cmd[PATH_MAX * 3];
        snprintf(cmd, sizeof cmd, "'%s' parse '%s' 2>/dev/null", g_orch, file);

        char *json = NULL;
        if (run_capture(cmd, &json) != 0 || !json) { free(json); continue; }

        char *name = json_get_string(json, "services[0].name");
        char *run  = json_get_string(json, "services[0].run_command");
        char *hc   = json_get_string(json, "services[0].healthcheck");
        char *ids  = json_get_string(json, "services[0].env.MODELS");
        free(json);

        if (!name || !run) { free(name); free(run); free(hc); free(ids); continue; }

        model_t *m = &models[nmodels++];
        snprintf(m->name, sizeof m->name, "%s", name);
        snprintf(m->file, sizeof m->file, "%s", file);
        snprintf(m->host, sizeof m->host, "127.0.0.1");
        m->port = port_from_cmd(run);

        if (hc && hc[0]) {
            snprintf(m->health, sizeof m->health, "%s", hc);
            /* The health URL carries the address the server answers on, so it
             * takes precedence over the port scraped from the run command. */
            char hpath[512];
            int hport = 0;
            if (parse_url(hc, m->host, sizeof m->host, &hport, hpath, sizeof hpath) == 0) {
                if (hport > 0) m->port = hport;
            } else {
                logf_("%s: health URL is not plain HTTP, falling back to %s:%d",
                      name, m->host, m->port);
            }
        } else {
            snprintf(m->health, sizeof m->health,
                     "http://%s:%d/v1/models", m->host, m->port);
        }
        /* Model ids the front door advertises for this grove: ENV MODELS if
         * set, else the service name. */
        snprintf(m->ids, sizeof m->ids, "%s", (ids && ids[0]) ? ids : name);

        free(name); free(run); free(hc); free(ids);
    }
    globfree(&gl);

    if (nmodels == 0) {
        snprintf(err, errn, "no services with a port under %s/servers", g_dir);
        return -1;
    }
    return 0;
}

/* Split "http://host:port/path" into host, port and path. A bare
 * "host:port/path" is accepted too, and an IPv6 literal may be bracketed.
 * Returns 0 on success and -1 for a scheme the arbiter cannot speak. */
static int parse_url(const char *url, char *host, size_t hostn,
                     int *port, char *path, size_t pathn) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) return -1;

    const char *slash = strchr(p, '/');
    char hostport[512];
    if (slash) {
        snprintf(hostport, sizeof hostport, "%.*s", (int)(slash - p), p);
        snprintf(path, pathn, "%s", slash);
    } else {
        snprintf(hostport, sizeof hostport, "%s", p);
        snprintf(path, pathn, "/");
    }

    *port = 80;
    if (hostport[0] == '[') {
        char *rb = strchr(hostport, ']');
        if (!rb) return -1;
        *rb = '\0';
        snprintf(host, hostn, "%s", hostport + 1);
        if (rb[1] == ':') *port = atoi(rb + 2);
    } else {
        char *colon = strrchr(hostport, ':');
        if (colon) { *colon = '\0'; *port = atoi(colon + 1); }
        snprintf(host, hostn, "%s", hostport);
    }
    return 0;
}

/* Open a TCP connection to host:port. Names, IPv4 and IPv6 all work. */
static int dial(const char *host, int port) {
    char service[16];
    snprintf(service, sizeof service, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* GET the health URL and report whether it answers 200. */
static int http_ok(const char *url) {
    char host[256], path[512];
    int port;
    if (parse_url(url, host, sizeof host, &port, path, sizeof path) != 0) return 0;

    int fd = dial(host, port);
    if (fd < 0) return 0;

    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    char req[1024];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     path, host);
    if (write(fd, req, (size_t)n) < 0) { close(fd); return 0; }

    char buf[512];
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = '\0';
    return strstr(buf, " 200") != NULL;
}

/* --- groves ------------------------------------------------------------- */

/* `orchd --state-dir <root>/<name> ... grow` : plant + start one model. */
static void grove_grow(const char *name, const char *file) {
    char cmd[PATH_MAX * 4];
    snprintf(cmd, sizeof cmd,
             "'%s' --project-dir '%s' --state-dir '%s/%s' --orchfile '%s' "
             "--namespace '%s' --user grow",
             g_orchd, g_dir, g_state_root, name, file, name);
    run_status(cmd);
}

/* `orchd --state-dir <root>/<name> ... fell` : stop + uninstall one model. */
static void grove_fell(const char *name) {
    char cmd[PATH_MAX * 4];
    snprintf(cmd, sizeof cmd,
             "'%s' --project-dir '%s' --state-dir '%s/%s' "
             "--namespace '%s' --user fell --keep-data",
             g_orchd, g_dir, g_state_root, name, name);
    run_status(cmd);
}

static int find_model(const char *name) {
    for (int i = 0; i < nmodels; i++)
        if (strcmp(models[i].name, name) == 0) return i;
    return -1;
}

/*
 * Who is running, according to orchd itself. `orchd survey --json` reports a
 * PID per service; a positive PID that is still alive means the grove holds
 * the GPU. We ask orchd rather than poking launchd/plists directly — it owns
 * the lifecycle, so it owns the answer.
 *
 * (Trust the PID, not the `state` field: launchd 6.x reports `state = active`
 * while orchd 0.3.4 only maps the literal `running` to "active", so `state`
 * reads "inactive" even for a live job. The PID is correct either way.)
 *
 * Returns the PID, or 0 when the grove is absent or not running.
 */
static int grove_pid(const char *name) {
    char cmd[PATH_MAX * 4];
    snprintf(cmd, sizeof cmd,
             "'%s' --project-dir '%s' --state-dir '%s/%s' --namespace '%s' "
             "--user survey --json 2>/dev/null",
             g_orchd, g_dir, g_state_root, name, name);

    char *json = NULL;
    if (run_capture(cmd, &json) != 0 || !json) { free(json); return 0; }

    long pid = 0;
    int ok = json_get_int(json, "[0].pid", &pid);
    free(json);

    if (ok == 0 && pid > 0 && kill((pid_t)pid, 0) == 0) return (int)pid;
    return 0;
}

/*
 * Switch to `name`. Returns its port on success, -1 on timeout, -2 if unknown.
 */
static int switch_to(const char *name, int timeout, char *err, size_t errn) {
    int ti = find_model(name);
    if (ti < 0) { snprintf(err, errn, "unknown model"); return -2; }

    /* Already the orchd holder and answering health? Nothing to do. */
    if (grove_pid(models[ti].name) > 0 && http_ok(models[ti].health))
        return models[ti].port;

    /* orchd is the source of truth for who is running: fell every other grove
     * that it reports as holding a live PID. Idempotent — a grove that was
     * never grown just falls through. */
    for (int i = 0; i < nmodels; i++) {
        if (i == ti) continue;
        int pid = grove_pid(models[i].name);
        if (pid > 0) {
            logf_("stopping holder %s (pid %d)", models[i].name, pid);
            grove_fell(models[i].name);
        }
    }

    logf_("starting %s", name);
    grove_grow(models[ti].name, models[ti].file);

    time_t deadline = time(NULL) + timeout;
    while (time(NULL) < deadline) {
        if (http_ok(models[ti].health)) {
            logf_("%s ready on :%d", name, models[ti].port);
            return models[ti].port;
        }
        sleep(1);
    }
    snprintf(err, errn, "%s not ready within %ds", name, timeout);
    return -1;
}

/* --- HTTP --------------------------------------------------------------- */

/* --- /v1 front door ----------------------------------------------------- */

static void send_all(int fd, int code, const char *reason, const char *body);
static void dispatch(int fd, const char *method, const char *path,
                     char *body, long clen, int timeout);

/* Resolve a requested model id to a grove, honouring each grove's ENV MODELS. */
static int find_by_model_id(const char *id) {
    size_t idl = strlen(id);
    for (int i = 0; i < nmodels; i++) {
        const char *p = models[i].ids;
        while (*p) {
            const char *c = strchr(p, ',');
            size_t tl = c ? (size_t)(c - p) : strlen(p);
            if (tl == idl && strncmp(p, id, tl) == 0) return i;
            if (!c) break;
            p = c + 1;
        }
    }
    return -1;
}

/* Forward an inference request to the resolved grove and relay the response
 * verbatim (status line, headers and body — streaming included). */
static void proxy_to(int cfd, int ti, const char *path,
                     const char *body, long clen) {
    int bfd = dial(models[ti].host, models[ti].port);
    if (bfd < 0) {
        send_all(cfd, 502, "Bad Gateway", "{\"error\":\"backend unreachable\"}");
        return;
    }

    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
                     "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Content-Type: application/json\r\nContent-Length: %ld\r\n"
                     "Connection: close\r\n\r\n",
                     path, models[ti].host, models[ti].port, clen);
    if (write(bfd, hdr, (size_t)n) < 0) { close(bfd); return; }
    if (body && clen > 0) {
        long sent = 0;
        while (sent < clen) {
            ssize_t w = write(bfd, body + sent, (size_t)(clen - sent));
            if (w <= 0) break;
            sent += w;
        }
    }

    char rbuf[65536];
    for (;;) {
        ssize_t r = read(bfd, rbuf, sizeof rbuf);
        if (r <= 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(cfd, rbuf + off, (size_t)(r - off));
            if (w <= 0) { close(bfd); return; }
            off += w;
        }
    }
    close(bfd);
}

/* --- HTTP --------------------------------------------------------------- */

typedef struct { char *buf; size_t len, cap; } sbuf_t;

static void sb_add(sbuf_t *s, const char *fmt, ...) {
    if (s->cap - s->len < 256) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        char *tmp = realloc(s->buf, s->cap);
        if (!tmp) return;
        s->buf = tmp;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) s->len += (size_t)n;
}

static void send_all(int fd, int code, const char *reason, const char *body) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, reason, strlen(body));
    if (write(fd, hdr, (size_t)n) < 0) return;
    (void)!write(fd, body, strlen(body));
}

static char *find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0) return (char *)p;
    return NULL;
}

static void handle(int fd) {
    /* Grow a buffer: headers first, then the declared body. Big prompts are
     * common on /v1, so this can't be a fixed stack buffer. */
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    if (!buf) return;

    char *he = NULL;
    while (!he) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *t = realloc(buf, cap);
            if (!t) { free(buf); return; }
            buf = t;
        }
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r <= 0) { free(buf); return; }
        len += (size_t)r;
        buf[len] = '\0';
        he = strstr(buf, "\r\n\r\n");
    }

    char method[8] = {0}, path[1024] = {0};
    sscanf(buf, "%7s %1023s", method, path);

    long clen = 0;
    char *cl = find_ci(buf, "content-length:");
    if (cl) clen = atol(cl + 15);
    if (clen < 0) clen = 0;

    size_t hdr_len = (size_t)(he - buf) + 4;
    size_t need = hdr_len + (size_t)clen;
    while (len < need) {
        if (need + 1 > cap) {
            cap = need + 1;
            char *t = realloc(buf, cap);
            if (!t) { free(buf); return; }
            buf = t;
        }
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r <= 0) break;
        len += (size_t)r;
        buf[len] = '\0';
    }

    int timeout = g_timeout;
    const char *qs = strchr(path, '?');
    if (qs) {
        const char *t = strstr(qs + 1, "timeout=");
        if (t) { int v = atoi(t + 8); if (v > 0) timeout = v; }
    }

    dispatch(fd, method, path, buf + hdr_len, clen, timeout);
    free(buf);
}

static void dispatch(int fd, const char *method, const char *path,
                     char *body, long clen, int timeout) {
    char q[512];
    snprintf(q, sizeof q, "%s", path);
    char *qs = strchr(q, '?');
    if (qs) *qs = '\0';

    /* The whole public surface is OpenAI's /v1. A harness names a model and
     * nothing else; model lifecycle is not its concern. */

    if (strcmp(method, "GET") == 0 && strcmp(q, "/v1/models") == 0) {
        /* Every name any grove answers to, so a client can ask for one and
         * let the front door raise it. */
        sbuf_t s = {0};
        sb_add(&s, "{\"object\":\"list\",\"data\":[");
        int first = 1;
        for (int i = 0; i < nmodels; i++) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", models[i].ids);
            for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
                if (tok[0]) {
                    sb_add(&s, "%s{\"id\":\"%s\",\"object\":\"model\","
                               "\"owned_by\":\"ds4\"}",
                           first ? "" : ",", tok);
                    first = 0;
                }
        }
        sb_add(&s, "]}");
        send_all(fd, 200, "OK",
                 s.buf ? s.buf : "{\"object\":\"list\",\"data\":[]}");
        free(s.buf);
        return;
    }

    if (strcmp(method, "POST") == 0 && strncmp(q, "/v1/", 4) == 0) {
        /* Read the requested model, raise its grove if needed, then stream
         * the backend's response back verbatim. */
        char *mid = (body && clen > 0) ? json_get_string(body, "model") : NULL;
        if (!mid) {
            send_all(fd, 400, "Bad Request", "{\"error\":\"missing model\"}");
            return;
        }
        int ti = find_by_model_id(mid);
        if (ti < 0) {
            char out[320];
            snprintf(out, sizeof out,
                     "{\"error\":\"unknown model\",\"model\":\"%s\"}", mid);
            send_all(fd, 404, "Not Found", out);
            free(mid);
            return;
        }
        free(mid);

        if (!(grove_pid(models[ti].name) > 0 && http_ok(models[ti].health))) {
            char err[256] = {0};
            logf_("/v1 wants %s -> switching", models[ti].name);
            if (switch_to(models[ti].name, timeout, err, sizeof err) < 0) {
                char out[320];
                snprintf(out, sizeof out, "{\"error\":\"%s\"}", err);
                send_all(fd, 503, "Service Unavailable", out);
                return;
            }
        }
        logf_("/v1 %s -> :%d", q, models[ti].port);
        proxy_to(fd, ti, path, body, clen);
        return;
    }

    send_all(fd, 404, "Not Found", "{\"error\":\"not found\"}");
}

static int serve(void) {
    char service[16];
    snprintf(service, sizeof service, "%d", g_listen_port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int gai = getaddrinfo(g_listen_host[0] ? g_listen_host : NULL,
                          service, &hints, &res);
    if (gai != 0) {
        logf_("listen %s:%d: %s", g_listen_host, g_listen_port, gai_strerror(gai));
        return 1;
    }

    int lfd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (lfd < 0) continue;
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(lfd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(lfd);
        lfd = -1;
    }
    freeaddrinfo(res);
    if (lfd < 0) {
        logf_("bind %s:%d: %s", g_listen_host, g_listen_port, strerror(errno));
        return 1;
    }
    if (listen(lfd, 16) != 0) { logf_("listen: %s", strerror(errno)); return 1; }

    logf_("arbiter on http://%s:%d", g_listen_host, g_listen_port);
    logf_("orch: %s   orchd: %s", g_orch, g_orchd);
    logf_("state root: %s   dir: %s", g_state_root, g_dir);
    for (int i = 0; i < nmodels; i++)
        logf_("  %-22s :%-6d %s", models[i].name, models[i].port, models[i].health);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* reap forked connections automatically */
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }

        /* Fork per connection: a switch can block for minutes and an
         * inference response streams for many seconds; the accept loop must
         * stay free. The child owns the socket. */
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            handle(fd);
            close(fd);
            _exit(0);
        }
        close(fd);
    }
    close(lfd);
    return 0;
}

static const char *arg_val(int argc, char **argv, const char *flag) {
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    const char *v;

    v = arg_val(argc, argv, "--dir");        expand_tilde(v ? v : ".", g_dir, sizeof g_dir);
    v = arg_val(argc, argv, "--orch");       expand_tilde(v ? v : "orch", g_orch, sizeof g_orch);
    v = arg_val(argc, argv, "--orchd");      expand_tilde(v ? v : "orchd", g_orchd, sizeof g_orchd);
    v = arg_val(argc, argv, "--state-root"); expand_tilde(v ? v : "~/.orch/g", g_state_root, sizeof g_state_root);

    v = arg_val(argc, argv, "--listen");
    if (v) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", v);
        if (tmp[0] == '[') {
            char *rb = strchr(tmp, ']');
            if (rb) {
                *rb = '\0';
                snprintf(g_listen_host, sizeof g_listen_host, "%s", tmp + 1);
                if (rb[1] == ':') g_listen_port = atoi(rb + 2);
            }
        } else {
            char *colon = strrchr(tmp, ':');
            if (colon) { *colon = '\0'; g_listen_port = atoi(colon + 1);
                         snprintf(g_listen_host, sizeof g_listen_host, "%s", tmp); }
        }
    }
    v = arg_val(argc, argv, "--timeout");
    if (v) g_timeout = atoi(v);

    int dump = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--dump") == 0) dump = 1;

    if (access(g_orch, X_OK) != 0) { logf_("orch not executable: %s", g_orch); return 1; }
    if (access(g_orchd, X_OK) != 0) { logf_("orchd not executable: %s", g_orchd); return 1; }

    char err[256] = {0};
    if (discover(err, sizeof err) != 0) { logf_("%s", err); return 1; }

    if (dump) {
        printf("dir: %s\ndiscovered %d models:\n", g_dir, nmodels);
        for (int i = 0; i < nmodels; i++)
            printf("  %-22s :%-6d %s\n", models[i].name, models[i].port, models[i].health);
        return 0;
    }

    return serve();
}
