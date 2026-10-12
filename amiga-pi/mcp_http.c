/* mcp_http -- MCP's streamable HTTP transport (pi f10993bc
 * packages/mcp/src/transports/streamable-http.ts) over vtcon's llm_post:
 * every message a POST with Accept: application/json, text/event-stream;
 * Mcp-Session-Id once the server gave one, MCP-Protocol-Version once
 * initialize chose one, the configured headers (${VAR}, !command); the
 * answer an application/json body (one message or a batch) or an event
 * stream whose events carry the messages. Notifications and answers get
 * 202. A 404 while a session is open: the session expired. No GET stream
 * and no DELETE on close (single-threaded; COMPATIBILITY.md). The LAN rule:
 * plain http only to a LAN address, and no Authorization header over it.
 * Portable C89. */
#include <stdlib.h>
#include <string.h>
#include "mcp.h"
#include "claude/util.h"
#include "claude/http.h"

typedef struct mh {
    http_url url;
    char buf[4096];
    http_resp resp;
    sse ss;
    jw errbody, body, head;
    jw vals;                    /* the configured header values, NUL-separated, resolved once at open */
    long offs[32];
    char names[32][64];
    int nc;                     /* configured headers kept */
    int skipped_auth;           /* an Authorization header left out: plain http */
    char type[64];              /* the answer's content type, lower case, no parameters */
    mcp_server *s;
    mcp_msg_fn on;
    void *ou;
    int done;                   /* on() saw the answer */
    int noticed;                /* the plain-http credential notice was given */
} mh;

static void h_header(void *u, const char *name, const char *value)
{
    mh *h = (mh *)u;
    if (!strcmp(name, "mcp-session-id") && *value)
        cl_copy(h->s->session, value, sizeof(h->s->session));
    else if (!strcmp(name, "content-type")) {
        long i;
        for (i = 0; value[i] && value[i] != ';' && i < (long)sizeof(h->type) - 1; i++)
            h->type[i] = (char)(value[i] >= 'A' && value[i] <= 'Z' ? value[i] + 32 : value[i]);
        while (i > 0 && h->type[i - 1] == ' ')
            i--;
        h->type[i] = 0;
    }
}

static void h_body(void *u, const char *s, long n)
{
    mh *h = (mh *)u;
    jw_raw(&h->body, s, n);
}

static void h_event(void *u, const char *event, const char *data, long n)
{
    mh *h = (mh *)u;
    (void)event;
    if (h->on && !h->done && n > 0 && h->on(h->ou, data, n))
        h->done = 1;
}

static int h_head(void *u, const char **head, long *hn)
{
    mh *h = (mh *)u;
    *head = h->head.p;
    *hn = h->head.n;
    return 0;
}

/* the m->h hooks the caller set, with this transport's own on top */
static int h_poll(void *u)
{
    mh *h = (mh *)u;
    const llm_hooks *k = mcp_hooks(h->s->m);
    return k->poll ? k->poll(k->u) : 0;
}

static unsigned long h_ms(void *u)
{
    mh *h = (mh *)u;
    const llm_hooks *k = mcp_hooks(h->s->m);
    return k->ms ? k->ms(k->u) : 0;
}

/* the configured headers' values (${VAR}, "!command") are resolved once, here, when the transport opens: a
 * "!command" runs once, not at every POST */
static int resolve_headers(mh *h)
{
    mcp_server *s = h->s;
    jw_init(&h->vals);
    if (s->headers) {
        jv o, k, v;
        jit it;
        if (!json_parse(s->headers, (long)strlen(s->headers), &o)) {
            json_iter(o, &it);
            while (json_next(&it, &k, &v) && h->nc < 32) {
                char *val = json_strdup(v, 0);
                long mark = h->vals.n;
                json_str(k, h->names[h->nc], sizeof(h->names[h->nc]));
                /* pi resolves every header (resolveHeadersOrThrow): one that fails fails the server */
                if (mcp_value(s, "header", h->names[h->nc], val ? val : "", &h->vals)) {
                    free(val);
                    return -1;
                }
                if (!h->url.tls && cl_strieq(h->names[h->nc], "authorization")) {
                    /* the LAN rule: no credential over plain http */
                    h->skipped_auth = 1;
                    h->vals.n = mark;
                    free(val);
                    continue;
                }
                h->offs[h->nc] = mark;
                jw_raw(&h->vals, "\0", 1);
                free(val);
                h->nc++;
            }
        }
    }
    return 0;
}

static int build_head(mh *h, long body_len)
{
    mcp_server *s = h->s;
    http_hdr hd[40];
    int nh = 0, i;
    hd[nh].name = "Accept";
    hd[nh++].value = "application/json, text/event-stream";
    hd[nh].name = "Content-Type";
    hd[nh++].value = "application/json";
    if (h->skipped_auth && !h->noticed) {
        const llm_hooks *k = mcp_hooks(s->m);
        if (k->notice) {
            jw t;
            jw_init(&t);
            jw_rawz(&t, "credential for MCP server ");
            jw_rawz(&t, s->name);
            jw_rawz(&t, " not sent: plain http");
            k->notice(k->u, t.p);
            jw_free(&t);
        }
        h->noticed = 1;
    }
    for (i = 0; i < h->nc; i++) {
        hd[nh].name = h->names[i];
        hd[nh++].value = h->vals.p + h->offs[i];
    }
    if (s->session[0]) {
        hd[nh].name = "Mcp-Session-Id";
        hd[nh++].value = s->session;
    }
    if (s->version[0]) {
        hd[nh].name = "MCP-Protocol-Version";
        hd[nh++].value = s->version;
    }
    jw_reset(&h->head);
    {
        long cap = 4096, n;
        char *out = (char *)malloc((size_t)cap);
        if (!out)
            return -1;
        n = http_post_head(h->url.host, h->url.port, h->url.tls, h->url.path, hd, nh, body_len, out, cap);
        if (n > 0)
            jw_raw(&h->head, out, n);
        free(out);
        return n > 0 ? 0 : -1;
    }
}

static int h_send(mcp_server *s, const char *msg, long n, int request, mcp_msg_fn on, void *ou)
{
    mh *h = (mh *)s->tu;
    llm_call c;
    int rc;
    char num[24];
    if (build_head(h, n)) {
        cl_copy(s->err, "MCP request head too long", sizeof(s->err));
        return -1;
    }
    jw_reset(&h->body);
    h->type[0] = 0;
    h->on = on;
    h->ou = ou;
    h->done = 0;
    memset(&c, 0, sizeof(c));
    c.pool = s->m->pool;
    c.host = h->url.host;
    c.port = h->url.port;
    c.tls = h->url.tls;
    c.body = msg;
    c.bn = n;
    c.h.u = h;
    c.h.poll = h_poll;
    c.h.ms = h_ms;
    c.h.head = h_head;
    c.h.header = h_header;
    c.h.body = h_body;
    c.h.event = h_event;
    c.idle_ms = s->timeout_ms > 0 ? s->timeout_ms : MCP_TIMEOUT_MS;
    c.buf = h->buf;
    c.bufcap = sizeof(h->buf);
    c.resp = &h->resp;
    c.sse = &h->ss;
    c.errbody = &h->errbody;
    rc = llm_post(&c);
    s->transient = rc == LP_CONNECT || rc == LP_SEND || rc == LP_RECV || rc == LP_EOF || rc == LP_BADHTTP ||
                   (rc == LP_OK && (h->resp.status == 408 || h->resp.status == 429 ||
                                    (h->resp.status >= 500 && h->resp.status != 501)));
    if (rc == LP_BREAK) {
        cl_copy(s->err, "Operation aborted", sizeof(s->err));
        return -1;
    }
    if (rc == LP_IDLE) {
        cl_ltoa(c.idle_ms, num);
        cl_copy(s->err, "MCP request timed out after ", sizeof(s->err));
        cl_cat(s->err, num, sizeof(s->err));
        cl_cat(s->err, "ms", sizeof(s->err));
        return -1;
    }
    if (rc != LP_OK) {
        cl_copy(s->err, rc == LP_CONNECT ? "cannot connect to " : "connection to ", sizeof(s->err));
        cl_cat(s->err, h->url.host, sizeof(s->err));
        cl_cat(s->err, rc == LP_CONNECT ? "" : " failed", sizeof(s->err));
        if (c.err && *c.err) {
            cl_cat(s->err, ": ", sizeof(s->err));
            cl_cat(s->err, c.err, sizeof(s->err));
        }
        return -1;
    }
    if (h->resp.status < 200 || h->resp.status > 299) {
        if (h->resp.status == 401)
            cl_copy(s->err, "MCP server requires authentication", sizeof(s->err));
        else if (h->resp.status == 404 && s->session[0])
            cl_copy(s->err, "MCP session expired", sizeof(s->err));
        else {
            cl_ltoa(h->resp.status, num);
            cl_copy(s->err, "MCP server answered HTTP ", sizeof(s->err));
            cl_cat(s->err, num, sizeof(s->err));
            if (h->errbody.n) {
                long k = h->errbody.n < 200 ? h->errbody.n : 200;
                char line[208];
                long i;
                for (i = 0; i < k && h->errbody.p[i] != '\n' && h->errbody.p[i] != '\r'; i++)
                    line[i] = h->errbody.p[i];
                line[i] = 0;
                if (i) {
                    cl_cat(s->err, ": ", sizeof(s->err));
                    cl_cat(s->err, line, sizeof(s->err));
                }
            }
        }
        return -1;
    }
    if (!request)
        return 0;               /* acknowledged (202); any body ignored */
    if (h->resp.status == 202 || h->resp.status == 204) {
        cl_copy(s->err, "MCP server accepted the request without a response", sizeof(s->err));
        return -1;
    }
    if (!strcmp(h->type, "application/json")) {
        jv v, e;
        jit it;
        if (json_parse(h->body.p ? h->body.p : "", h->body.n, &v)) {
            cl_copy(s->err, "MCP server sent invalid JSON", sizeof(s->err));
            return -1;
        }
        if (json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &e))
                if (!h->done && on(ou, e.p, e.n))
                    h->done = 1;
        } else if (on(ou, v.p, v.n))
            h->done = 1;
        return 0;
    }
    if (!strcmp(h->type, "text/event-stream"))
        return 0;               /* the events went to on() as they came */
    cl_copy(s->err, "Unsupported MCP response content type: ", sizeof(s->err));
    cl_cat(s->err, h->type[0] ? h->type : "missing", sizeof(s->err));
    return -1;
}

static void h_close(mcp_server *s)
{
    mh *h = (mh *)s->tu;
    if (!h)
        return;
    jw_free(&h->errbody);
    jw_free(&h->body);
    jw_free(&h->head);
    jw_free(&h->vals);
    free(h);
    s->tu = 0;
}

int mcp_http_open(mcp_server *s)
{
    mh *h;
    http_url u;
    if (!s->url || http_parse_url(s->url, &u)) {
        cl_copy(s->err, "url must be an http or https URL", sizeof(s->err));
        return -1;
    }
    if (!u.tls) {
        /* the LAN rule (ARCHITECTURE.md): the address the name resolves to */
        unsigned char ip[4];
        const llm_hooks *k = mcp_hooks(s->m);
        int have = llm_ipv4(u.host, ip) || (k->resolve && !k->resolve(k->u, u.host, ip));
        if (!llm_lan_ok(u.host, have ? ip : 0)) {
            cl_copy(s->err, "plain http is only for the LAN (", sizeof(s->err));
            cl_cat(s->err, u.host, sizeof(s->err));
            cl_cat(s->err, " is not); use https", sizeof(s->err));
            return -1;
        }
    }
    if (!s->m->pool) {
        cl_copy(s->err, "no network", sizeof(s->err));
        return -1;
    }
    h = (mh *)calloc(1, sizeof(*h));
    if (!h) {
        cl_copy(s->err, "out of memory", sizeof(s->err));
        return -1;
    }
    h->url = u;
    h->s = s;
    jw_init(&h->errbody);
    jw_init(&h->body);
    jw_init(&h->head);
    if (resolve_headers(h)) {
        jw_free(&h->errbody);
        jw_free(&h->body);
        jw_free(&h->head);
        jw_free(&h->vals);
        free(h);
        return -1;                      /* s->err says which header, as pi does */
    }
    s->tu = h;
    s->tr.u = h;
    s->tr.inline_reply = 0;
    s->tr.send = h_send;
    s->tr.close = h_close;
    return 0;
}
