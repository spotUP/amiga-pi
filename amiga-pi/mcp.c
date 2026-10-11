/* mcp -- amiga-pi's MCP client; see mcp.h. */
#include <stdlib.h>
#include <string.h>
#include "mcp.h"
#include "sha256.h"
#include "truncate.h"
#include "claude/util.h"
#include "claude/http.h"
#include "claude/schema.h"
#include "llm/llm.h"

long mcp_call_count;

static const char *const exposure_names[] = {"codemode", "deferred", "direct", "hidden"};
static const char exposures_list[] = "\"codemode\", \"deferred\", \"direct\", \"hidden\"";

static char *dupn(const char *s, long n)
{
    char *d = (char *)malloc((size_t)n + 1);
    if (d) {
        memcpy(d, s, (size_t)n);
        d[n] = 0;
    }
    return d;
}

static char *dupz(const char *s)
{
    return s ? dupn(s, (long)strlen(s)) : 0;
}

static int ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* a JSON number without floating point: its value times 10^scale, cut toward zero (scale 3: seconds to ms).
 * 0, or -1 when it does not fit 9 digits before the point. *neg for a minus sign, *frac when digits beyond the
 * scale are not zero. */
static int num_scaled(jv v, int scale, long *out, int *neg, int *frac)
{
    const char *p = v.p, *e = v.p + v.n;
    char dig[64];
    int nd = 0, point = -1, ex = 0, exneg = 0, i, lead;
    long r = 0;
    *neg = *frac = 0;
    if (p < e && *p == '-') {
        *neg = 1;
        p++;
    }
    for (; p < e && ((*p >= '0' && *p <= '9') || *p == '.'); p++) {
        if (*p == '.')
            point = nd;
        else if (nd < (int)sizeof(dig))
            dig[nd++] = *p;
    }
    if (point < 0)
        point = nd;
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-'))
            exneg = *p++ == '-';
        for (; p < e && *p >= '0' && *p <= '9'; p++)
            ex = ex * 10 + (*p - '0') > 400 ? 400 : ex * 10 + (*p - '0');
    }
    point += (exneg ? -ex : ex) + scale;      /* digits before the (scaled) point */
    for (lead = 0, i = 0; i < point; i++) {
        int d = i < nd ? dig[i] - '0' : 0;
        if (r || d)
            lead++;
        if (lead > 9)
            return -1;
        r = r * 10 + d;
    }
    for (i = point < 0 ? 0 : point; i < nd; i++)
        if (dig[i] != '0')
            *frac = 1;
    *out = r;
    return 0;
}

static void err_line(mcp *m, const char *a, const char *b, const char *c)
{
    jw_rawz(&m->errors, a);
    if (b)
        jw_rawz(&m->errors, b);
    if (c)
        jw_rawz(&m->errors, c);
    jw_rawz(&m->errors, "\n");
}

/* ---- the config (pi's config.ts, mcp-servers.ts) ---- */

static int exposure_of(jv v)
{
    int i;
    if (json_type(v) != J_STR)
        return -1;
    if (json_streq(v, "codemode-deferred"))
        return MX_CODEMODE;     /* pi's alias */
    for (i = 0; i < 4; i++)
        if (json_streq(v, exposure_names[i]))
            return i;
    return -1;
}

static int string_record(jv v)
{
    jit it;
    jv k, x;
    if (json_type(v) != J_OBJ)
        return 0;
    json_iter(v, &it);
    while (json_next(&it, &k, &x))
        if (json_type(x) != J_STR)
            return 0;
    return 1;
}

static int name_ok(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' ||
              *s == '-'))
            return 0;
    return 1;
}

/* mcpNamespace: "-" as "_" (the server names that clash) */
static void namespace_of(const char *name, char *out, long cap)
{
    long i;
    cl_copy(out, "mcp__", cap);
    i = (long)strlen(out);
    for (; *name && i < cap - 1; name++)
        out[i++] = *name == '-' ? '_' : *name;
    out[i] = 0;
}

/* a positive number of seconds (pi: typeof number and > 0) */
static int timeout_ok(jv x)
{
    long ms;
    int neg, frac;
    if (json_type(x) != J_NUM)
        return 0;
    if (num_scaled(x, 3, &ms, &neg, &frac))
        return !neg;            /* very large: positive unless negative */
    return !neg && (ms > 0 || frac);
}

/* validateMcpServerConfig's checks of the fields an override may set too; 0 ok, else err set */
static int check_common(const char *name, jv v, char *err, long cap)
{
    jv x, k, e;
    jit it;
    cl_copy(err, "server \"", cap);
    cl_cat(err, name, cap);
    cl_cat(err, "\": ", cap);
    if (json_get(v, "exposure", &x) && exposure_of(x) < 0) {
        cl_cat(err, "exposure must be one of ", cap);
        cl_cat(err, exposures_list, cap);
        return -1;
    }
    if (json_get(v, "toolExposure", &x)) {
        if (json_type(x) != J_OBJ) {
            cl_cat(err, "toolExposure must map tool names to exposures", cap);
            return -1;
        }
        json_iter(x, &it);
        while (json_next(&it, &k, &e))
            if (exposure_of(e) < 0) {
                char t[128];
                json_str(k, t, sizeof(t));
                cl_cat(err, "toolExposure \"", cap);
                cl_cat(err, t, cap);
                cl_cat(err, "\" must be one of ", cap);
                cl_cat(err, exposures_list, cap);
                return -1;
            }
    }
    if (json_get(v, "enabled", &x) && json_type(x) != J_TRUE && json_type(x) != J_FALSE) {
        cl_cat(err, "enabled must be a boolean", cap);
        return -1;
    }
    if (json_get(v, "description", &x) && json_type(x) != J_STR) {
        cl_cat(err, "description must be a string", cap);
        return -1;
    }
    if (json_get(v, "timeout", &x) && !timeout_ok(x)) {
        cl_cat(err, "timeout must be a positive number of seconds", cap);
        return -1;
    }
    return 0;
}

static int url_ok(const char *u)
{
    http_url hu;
    return (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) && !http_parse_url(u, &hu) && hu.host[0];
}

/* validateMcpServerConfig: MK_STDIO / MK_HTTP, or -1 with err */
static int validate(const char *name, jv v, char *err, long cap)
{
    jv x, type, it_v;
    char url[512];
    int has_type;
    if (!name_ok(name)) {
        cl_copy(err, "invalid server name \"", cap);
        cl_cat(err, name, cap);
        cl_cat(err, "\" (use letters, digits, \"_\" and \"-\")", cap);
        return -1;
    }
    if (json_type(v) != J_OBJ) {
        cl_copy(err, "server \"", cap);
        cl_cat(err, name, cap);
        cl_cat(err, "\" must be an object", cap);
        return -1;
    }
    if (check_common(name, v, err, cap))
        return -1;
    has_type = json_get(v, "type", &type);
    if (has_type && json_streq(type, "sse")) {
        cl_cat(err, "legacy SSE transport is not supported; use the streamable HTTP URL", cap);
        return -1;
    }
    if (json_get(v, "url", &x) && json_type(x) == J_STR &&
        (!has_type || json_streq(type, "http") || json_streq(type, "streamable-http"))) {
        json_str(x, url, sizeof(url));
        if (!url_ok(url)) {
            cl_cat(err, "url must be an http or https URL", cap);
            return -1;
        }
        if (json_get(v, "headers", &it_v) && !string_record(it_v)) {
            cl_cat(err, "headers must map names to strings", cap);
            return -1;
        }
        return MK_HTTP;
    }
    if (json_get(v, "command", &x) && json_type(x) == J_STR && (!has_type || json_streq(type, "stdio"))) {
        if (json_get(v, "args", &it_v)) {
            jit it;
            jv a;
            int ok = json_type(it_v) == J_ARR;
            if (ok) {
                json_iter(it_v, &it);
                while (json_next(&it, 0, &a))
                    if (json_type(a) != J_STR)
                        ok = 0;
            }
            if (!ok) {
                cl_cat(err, "args must be an array of strings", cap);
                return -1;
            }
        }
        if (json_get(v, "env", &it_v) && !string_record(it_v)) {
            cl_cat(err, "env must map names to strings", cap);
            return -1;
        }
        if (json_get(v, "cwd", &it_v) && json_type(it_v) != J_STR) {
            cl_cat(err, "cwd must be a string", cap);
            return -1;
        }
        return MK_STDIO;
    }
    cl_copy(err, "server \"", cap);
    cl_cat(err, name, cap);
    cl_cat(err, "\" needs either \"command\" (stdio) or \"url\" (streamable HTTP)", cap);
    return -1;
}

static void free_server_config(mcp_server *s)
{
    free(s->url);
    free(s->headers);
    free(s->command);
    free(s->args);
    free(s->env);
    free(s->cwd);
    free(s->tool_exposure);
    free(s->description);
    s->url = s->headers = s->command = s->args = s->env = s->cwd = s->tool_exposure = s->description = 0;
}

static char *member_text(jv v, const char *key)
{
    jv x;
    return json_get(v, key, &x) ? dupn(x.p, x.n) : 0;
}

static char *member_str(jv v, const char *key)
{
    jv x;
    return json_get(v, key, &x) && json_type(x) == J_STR ? json_strdup(x, 0) : 0;
}

/* the override fields (enabled, exposure, toolExposure) of v onto s */
static void apply_common(mcp_server *s, jv v)
{
    jv x;
    if (json_get(v, "enabled", &x))
        s->enabled = json_type(x) == J_TRUE;
    if (json_get(v, "exposure", &x))
        s->exposure = exposure_of(x);
    if (json_get(v, "toolExposure", &x)) {
        free(s->tool_exposure);
        s->tool_exposure = dupn(x.p, x.n);
    }
}

static void set_config(mcp_server *s, jv v, int kind)
{
    jv x;
    free_server_config(s);
    s->kind = kind;
    s->enabled = 1;
    s->exposure = MX_NONE;
    s->timeout_ms = MCP_TIMEOUT_MS;
    if (json_get(v, "timeout", &x)) {
        long ms;
        int neg, frac;
        s->timeout_ms = num_scaled(x, 3, &ms, &neg, &frac) ? 0x7fffffffL : ms > 0 ? ms : 1;
    }
    s->description = member_str(v, "description");
    if (kind == MK_HTTP) {
        s->url = member_str(v, "url");
        s->headers = member_text(v, "headers");
    } else {
        s->command = member_str(v, "command");
        s->args = member_text(v, "args");
        s->env = member_text(v, "env");
        s->cwd = member_str(v, "cwd");
    }
    apply_common(s, v);
}

static mcp_server *add_server(mcp *m, const char *name)
{
    mcp_server *s;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 4;
        mcp_server *ns = (mcp_server *)realloc(m->srv, (size_t)nc * sizeof(*ns));
        if (!ns)
            return 0;
        m->srv = ns;
        m->cap = nc;
    }
    s = &m->srv[m->n++];
    memset(s, 0, sizeof(*s));
    s->m = m;
    cl_copy(s->name, name, sizeof(s->name));
    jw_init(&s->pending);
    return s;
}

mcp_server *mcp_server_named(mcp *m, const char *name)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->srv[i].name, name))
            return &m->srv[i];
    return 0;
}

static int is_override(jv v)
{
    jv x;
    return json_type(v) == J_OBJ && !json_get(v, "command", &x) && !json_get(v, "url", &x) && !json_get(v, "type", &x);
}

int mcp_load_text(mcp *m, const char *text, long n, const char *path, int project)
{
    jv top, servers, k, v, x;
    jit it;
    char name[96], err[400];
    if (json_parse(text, n, &top)) {
        err_line(m, path, ": not valid JSON", 0);
        return -1;
    }
    if (json_type(top) != J_OBJ || (json_get(top, "mcpServers", &servers) && json_type(servers) != J_OBJ)) {
        err_line(m, path, ": expected an object with an \"mcpServers\" object", 0);
        return -1;
    }
    if (json_get(top, "autoEnableCodemode", &x) && json_type(x) != J_TRUE && json_type(x) != J_FALSE)
        err_line(m, path, ": autoEnableCodemode must be a boolean", 0);
    if (!json_get(top, "mcpServers", &servers))
        return 0;
    json_iter(servers, &it);
    while (json_next(&it, &k, &v)) {
        mcp_server *s;
        int kind, i;
        json_str(k, name, sizeof(name));
        if (project && is_override(v)) {
            /* an override of a user-level server: enabled, exposure, toolExposure only */
            jit it2;
            jv k2, v2;
            int extra = 0;
            json_iter(v, &it2);
            while (json_next(&it2, &k2, &v2))
                if (!json_streq(k2, "enabled") && !json_streq(k2, "exposure") && !json_streq(k2, "toolExposure"))
                    extra = 1;
            s = mcp_server_named(m, name);
            if (!s) {
                cl_copy(err, ": server \"", sizeof(err));
                cl_cat(err, name, sizeof(err));
                cl_cat(err, "\" needs \"command\" or \"url\", or a global server to override", sizeof(err));
                err_line(m, path, err, 0);
            } else if (extra) {
                cl_copy(err, ": server \"", sizeof(err));
                cl_cat(err, name, sizeof(err));
                cl_cat(err, "\": an override can only set enabled, exposure, toolExposure", sizeof(err));
                err_line(m, path, err, 0);
            } else if (check_common(name, v, err, sizeof(err))) {
                err_line(m, path, ": ", err);
            } else
                apply_common(s, v);
            continue;
        }
        kind = validate(name, v, err, sizeof(err));
        if (kind < 0) {
            err_line(m, path, ": ", err);
            continue;
        }
        /* names that differ only in "-" and "_" would share a namespace */
        {
            char ns1[96], ns2[96];
            int clash = -1;
            namespace_of(name, ns1, sizeof(ns1));
            for (i = 0; i < m->n; i++) {
                namespace_of(m->srv[i].name, ns2, sizeof(ns2));
                if (strcmp(m->srv[i].name, name) && !strcmp(ns1, ns2))
                    clash = i;
            }
            if (clash >= 0) {
                cl_copy(err, ": server \"", sizeof(err));
                cl_cat(err, name, sizeof(err));
                cl_cat(err, "\" conflicts with \"", sizeof(err));
                cl_cat(err, m->srv[clash].name, sizeof(err));
                cl_cat(err, "\"", sizeof(err));
                err_line(m, path, err, 0);
                continue;
            }
        }
        if (project && kind == MK_HTTP && json_get(v, "auth", &x)) {
            cl_copy(err, ": server \"", sizeof(err));
            cl_cat(err, name, sizeof(err));
            cl_cat(err, "\": auth is only allowed in the global mcp.json", sizeof(err));
            err_line(m, path, err, 0);
            continue;
        }
        s = mcp_server_named(m, name);
        if (!s)
            s = add_server(m, name);
        if (!s) {
            err_line(m, path, ": out of memory", 0);
            return -1;
        }
        set_config(s, v, kind);
        cl_copy(s->source, path, sizeof(s->source));
    }
    return 0;
}

int mcp_load(mcp *m, const char *path, int project)
{
    char *text = 0;
    long n = 0;
    int rc;
    if (!m->sys || m->sys->kind(m->sys->u, path) != 1)
        return 0;               /* no file: no servers */
    if (m->sys->read(m->sys->u, path, 1024L * 1024, &text, &n)) {
        err_line(m, path, ": ", m->sys->err(m->sys->u));
        return -1;
    }
    rc = mcp_load_text(m, text, n, path, project);
    free(text);
    return rc;
}

/* ---- values: ${VAR} from the environment, "!command" its output ---- */

void mcp_value(mcp *m, const char *v, jw *out)
{
    if (v[0] == '!' && m->sys && m->sys->run) {
        static char buf[4096];
        long n = 0, rc = 0;
        if (!m->sys->run(m->sys->u, v + 1, 30, buf, sizeof(buf), &n, &rc)) {
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
                n--;
            jw_raw(out, buf, n);
        }
        return;
    }
    while (*v) {
        const char *e;
        if (v[0] == '$' && v[1] == '{' && (e = strchr(v + 2, '}')) != 0 && e - v - 2 < 120) {
            char name[128], val[1024];
            long k;
            memcpy(name, v + 2, (size_t)(e - v - 2));
            name[e - v - 2] = 0;
            k = m->getenv ? m->getenv(name, val, sizeof(val)) : -1;
            if (k > 0)
                jw_raw(out, val, k < (long)sizeof(val) ? k : (long)sizeof(val) - 1);
            v = e + 1;
        } else
            jw_raw(out, v++, 1);
    }
}

/* ---- JSON-RPC ---- */

typedef struct rpc_wait {
    mcp_server *s;
    long id;
    int got, is_err;
    long code;
    jw *res;
    char msg[300];
} rpc_wait;

static void set_err(mcp_server *s, const char *a, const char *b)
{
    cl_copy(s->err, a, sizeof(s->err));
    if (b)
        cl_cat(s->err, b, sizeof(s->err));
}

static void file_url(const char *cwd, jw *w)
{
    jw_rawz(w, cwd[0] == '/' ? "file://" : "file:///");
    jw_rawz(w, cwd);
}

/* the server's request: ping and roots/list answered (pi's client), others refused */
static void answer(mcp_server *s, jv id, jv method)
{
    jw r;
    jw_init(&r);
    jw_rawz(&r, "{\"jsonrpc\":\"2.0\",\"id\":");
    jw_raw(&r, id.p, id.n);
    if (json_streq(method, "ping"))
        jw_rawz(&r, ",\"result\":{}}");
    else if (json_streq(method, "roots/list")) {
        const char *cwd = s->m->cwd ? s->m->cwd : "";
        const char *base = cwd + strlen(cwd);
        jw u;
        while (base > cwd && base[-1] != '/' && base[-1] != ':')
            base--;
        jw_init(&u);
        file_url(cwd, &u);
        jw_rawz(&r, ",\"result\":{\"roots\":[{\"uri\":");
        jw_str(&r, u.p ? u.p : "", u.n);
        jw_rawz(&r, ",\"name\":");
        jw_strz(&r, base);
        jw_rawz(&r, "}]}}");
        jw_free(&u);
    } else {
        char m[160];
        json_str(method, m, sizeof(m));
        jw_rawz(&r, ",\"error\":{\"code\":-32601,\"message\":");
        {
            jw t;
            jw_init(&t);
            jw_rawz(&t, "Method not found: ");
            jw_rawz(&t, m);
            jw_str(&r, t.p, t.n);
            jw_free(&t);
        }
        jw_rawz(&r, "}}");
    }
    if (s->tr.inline_reply)
        s->tr.send(s, r.p, r.n, 0, 0, 0);
    else {
        jw_raw(&s->pending, r.p, r.n);
        jw_raw(&s->pending, "\n", 1);
    }
    jw_free(&r);
}

static int on_msg(void *u, const char *p, long n)
{
    rpc_wait *w = (rpc_wait *)u;
    jv v, id, x, method;
    int has_method;
    if (json_parse(p, n, &v) || json_type(v) != J_OBJ)
        return 0;               /* pi: an invalid message is reported, nothing waits on it */
    has_method = json_get(v, "method", &method) && json_type(method) == J_STR;
    if (!has_method) {
        if (!json_get(v, "id", &id) || json_type(id) != J_NUM || json_long(id, -1) != w->id)
            return 0;
        if (json_get(v, "error", &x) && json_type(x) == J_OBJ) {
            jv c, mm;
            w->is_err = 1;
            w->code = json_get(x, "code", &c) ? json_long(c, 0) : 0;
            if (json_get(x, "message", &mm) && json_type(mm) == J_STR)
                json_str(mm, w->msg, sizeof(w->msg));
        } else if (json_get(v, "result", &x))
            jw_raw(w->res, x.p, x.n);
        else {
            w->is_err = 1;
            w->code = -32600;
            cl_copy(w->msg, "Received invalid JSON-RPC message", sizeof(w->msg));
        }
        w->got = 1;
        return 1;
    }
    if (json_get(v, "id", &id))
        answer(w->s, id, method);
    return 0;                   /* a notification: progress, list changes, logging -- not acted on */
}

static void flush_pending(mcp_server *s)
{
    jw q;
    long i, st = 0;
    if (!s->pending.n)
        return;
    q = s->pending;
    jw_init(&s->pending);
    for (i = 0; i < q.n; i++)
        if (q.p[i] == '\n') {
            s->tr.send(s, q.p + st, i - st, 0, 0, 0);
            st = i + 1;
        }
    jw_free(&q);
}

static int reconnect(mcp_server *s);

/* a request: 0 with the result's JSON in res; -1 with s->err (a JSON-RPC error: its message, as pi's McpError) */
static int rpc(mcp_server *s, const char *method, const char *params, jw *res)
{
    jw msg;
    rpc_wait w;
    int rc;
    memset(&w, 0, sizeof(w));
    w.s = s;
    w.id = ++s->next_id;
    w.res = res;
    jw_init(&msg);
    jw_rawz(&msg, "{\"jsonrpc\":\"2.0\",\"id\":");
    jw_long(&msg, w.id);
    jw_rawz(&msg, ",\"method\":");
    jw_strz(&msg, method);
    if (params) {
        jw_rawz(&msg, ",\"params\":");
        jw_rawz(&msg, params);
    }
    jw_rawz(&msg, "}");
    s->err[0] = 0;
    rc = s->tr.send ? s->tr.send(s, msg.p, msg.n, 1, on_msg, &w) : -1;
    jw_free(&msg);
    flush_pending(s);
    if (rc && s->kind == MK_HTTP && !strcmp(s->err, "MCP session expired") && strcmp(method, "initialize") &&
        !s->expired_once) {
        /* pi: a new session, then the request again (once) */
        s->expired_once = 1;
        rc = reconnect(s) ? -1 : rpc(s, method, params, res);
        s->expired_once = 0;
        return rc;
    }
    if (rc) {
        /* a dropped connection is reconnected on the next call; a timeout or Ctrl+C keeps it */
        if (strncmp(s->err, "MCP request timed out", 21) && strcmp(s->err, "Operation aborted"))
            s->connected = 0;
        return -1;
    }
    if (!w.got) {
        if (!s->err[0])
            set_err(s, "MCP server closed the connection before answering ", method);
        return -1;
    }
    if (w.is_err) {
        set_err(s, w.msg[0] ? w.msg : "MCP error", 0);
        return -1;
    }
    return 0;
}

static int notify(mcp_server *s, const char *method)
{
    jw msg;
    int rc;
    jw_init(&msg);
    jw_rawz(&msg, "{\"jsonrpc\":\"2.0\",\"method\":");
    jw_strz(&msg, method);
    jw_rawz(&msg, "}");
    rc = s->tr.send(s, msg.p, msg.n, 0, 0, 0);
    jw_free(&msg);
    return rc;
}

/* ---- connecting: initialize, tools ---- */

static const char *const supported[] = {MCP_PROTOCOL, "2025-06-18", "2025-03-26", "2024-11-05"};

static void free_tools(mcp_server *s)
{
    int i;
    for (i = 0; i < s->ntools; i++) {
        free(s->tools[i].name);
        free(s->tools[i].tname);
        free(s->tools[i].desc);
        free(s->tools[i].params);
    }
    free(s->tools);
    s->tools = 0;
    s->ntools = 0;
}

static void disconnect(mcp_server *s)
{
    if (s->tr.close)
        s->tr.close(s);
    s->tr.close = 0;
    s->tr.send = 0;
    s->connected = 0;
    s->session[0] = 0;
    s->version[0] = 0;
}

static int initialize(mcp_server *s)
{
    jw res;
    jv v, x, si, n2, v2;
    int i, ok = 0;
    char ver[24];
    jw_init(&res);
    if (rpc(s, "initialize",
            "{\"protocolVersion\":\"" MCP_PROTOCOL "\",\"capabilities\":{\"roots\":{}},"
            "\"clientInfo\":{\"name\":\"amiga-pi\",\"version\":\"0.1\"}}",
            &res)) {
        jw_free(&res);
        return -1;
    }
    if (json_parse(res.p ? res.p : "", res.n, &v) || json_type(v) != J_OBJ || !json_get(v, "protocolVersion", &x) ||
        json_type(x) != J_STR || !json_get(v, "capabilities", &x) || json_type(x) != J_OBJ ||
        !json_get(v, "serverInfo", &si) || json_type(si) != J_OBJ || !json_get(si, "name", &n2) ||
        json_type(n2) != J_STR || !json_get(si, "version", &v2) || json_type(v2) != J_STR ||
        (json_get(v, "instructions", &x) && json_type(x) != J_STR)) {
        set_err(s, "Invalid MCP initialize result", 0);
        jw_free(&res);
        return -1;
    }
    json_get(v, "protocolVersion", &x);
    json_str(x, ver, sizeof(ver));
    for (i = 0; i < 4; i++)
        if (!strcmp(ver, supported[i]))
            ok = 1;
    if (!ok) {
        set_err(s, "MCP server selected unsupported protocol version ", ver);
        jw_free(&res);
        return -1;
    }
    cl_copy(s->version, ver, sizeof(s->version));
    free(s->instructions);
    s->instructions = json_get(v, "instructions", &x) ? json_strdup(x, 0) : 0;
    jw_free(&res);
    if (notify(s, "notifications/initialized"))
        return -1;
    return 0;
}

/* toolExposure: an exact name first, then the first pattern ('*' any characters) that matches */
static int glob_match(const char *p, const char *s)
{
    if (!*p)
        return !*s;
    if (*p == '*') {
        for (;; s++) {
            if (glob_match(p + 1, s))
                return 1;
            if (!*s)
                return 0;
        }
    }
    return *s && *p == *s && glob_match(p + 1, s + 1);
}

static int tool_exposure(const mcp_server *s, const char *tool)
{
    int e = s->exposure;
    jv o, k, v;
    jit it;
    if (s->tool_exposure && !json_parse(s->tool_exposure, (long)strlen(s->tool_exposure), &o)) {
        int found = 0;
        json_iter(o, &it);
        while (json_next(&it, &k, &v))
            if (json_streq(k, tool)) {
                e = exposure_of(v);
                found = 1;
            }
        if (!found) {
            json_iter(o, &it);
            while (json_next(&it, &k, &v)) {
                char pat[128];
                json_str(k, pat, sizeof(pat));
                if (strchr(pat, '*') && glob_match(pat, tool)) {
                    e = exposure_of(v);
                    break;
                }
            }
        }
    }
    if (e == MX_NONE)
        e = MX_DIRECT;          /* amiga-pi's default (pi's is codemode, not built here) */
    if (e == MX_CODEMODE)
        e = MX_DEFERRED;        /* reached through tool_search */
    return e;
}

void mcp_tool_name(const char *server, const char *tool, int taken, char *out, long cap)
{
    jw raw;
    char name[512], hex[65];
    long i, n;
    jw_init(&raw);
    jw_rawz(&raw, "mcp__");
    jw_rawz(&raw, server);
    jw_rawz(&raw, "__");
    jw_rawz(&raw, tool);
    /* pi replaces each UTF-16 unit: a character outside ASCII is one '_' (two beyond U+FFFF) */
    for (i = n = 0; i < raw.n && n < (long)sizeof(name) - 2; i++) {
        unsigned char c = (unsigned char)raw.p[i];
        if ((c & 0xc0) == 0x80)
            continue;
        if (c >= 0xf0)
            name[n++] = '_';
        name[n++] = (char)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? c : '_');
    }
    name[n] = 0;
    jw_free(&raw);
    if (n <= MCP_NAME_MAX && !taken) {
        cl_copy(out, name, cap);
        return;
    }
    jw_init(&raw);
    jw_rawz(&raw, server);
    jw_raw(&raw, "\0", 1);
    jw_rawz(&raw, tool);
    pi_sha256_hex(raw.p, raw.n, hex);
    jw_free(&raw);
    name[n < MCP_NAME_MAX - 9 ? n : MCP_NAME_MAX - 9] = 0;
    cl_copy(out, name, cap);
    cl_cat(out, "_", cap);
    hex[8] = 0;
    cl_cat(out, hex, cap);
}

/* pi's toParameters: "type" defaults to "object", "properties" to {} (added at the end, as the spread does) */
static char *parameters_of(jv schema)
{
    jw w;
    jv x;
    jw_init(&w);
    jw_raw(&w, schema.p, schema.n - 1);     /* without the closing brace */
    while (w.n > 0 && (w.p[w.n - 1] == ' ' || w.p[w.n - 1] == '\n' || w.p[w.n - 1] == '\t' || w.p[w.n - 1] == '\r'))
        w.n--;
    w.p[w.n] = 0;
    if (!json_get(schema, "type", &x)) {
        jw_rawz(&w, json_count(schema) ? ",\"type\":\"object\"" : "\"type\":\"object\"");
    }
    if (!json_get(schema, "properties", &x))
        jw_rawz(&w, ",\"properties\":{}");
    jw_rawz(&w, "}");
    return w.p;
}

static int tool_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error);
static int is_taken(mcp *m, const mcp_server *self, const char *name);

static int list_tools(mcp_server *s)
{
    mcp *m = s->m;
    char *cursor = 0;
    char **seen = 0;
    int nseen = 0, rc = 0, i, cap = 0;
    jw all;                     /* the tools of every page, JSON array elements joined by ',' */
    jw_init(&all);
    for (;;) {
        jw res, params;
        jv v, items, x, t;
        jit it;
        jw_init(&res);
        jw_init(&params);
        if (cursor) {
            jw_rawz(&params, "{\"cursor\":");
            jw_strz(&params, cursor);
            jw_rawz(&params, "}");
        }
        if (rpc(s, "tools/list", cursor ? params.p : 0, &res)) {
            jw_free(&res);
            jw_free(&params);
            rc = -1;
            break;
        }
        jw_free(&params);
        if (json_parse(res.p ? res.p : "", res.n, &v) || json_type(v) != J_OBJ || !json_get(v, "tools", &items) ||
            json_type(items) != J_ARR) {
            set_err(s, "Invalid MCP tools/list result", 0);
            jw_free(&res);
            rc = -1;
            break;
        }
        json_iter(items, &it);
        while (json_next(&it, 0, &t)) {
            jv nm, sc;
            if (json_type(t) != J_OBJ || !json_get(t, "name", &nm) || json_type(nm) != J_STR ||
                !json_get(t, "inputSchema", &sc) || json_type(sc) != J_OBJ) {
                set_err(s, "Invalid entry in MCP tools/list result", 0);
                rc = -1;
                break;
            }
            if (all.n)
                jw_rawz(&all, ",");
            jw_raw(&all, t.p, t.n);
        }
        free(cursor);
        cursor = 0;
        if (!rc && json_get(v, "nextCursor", &x) && json_type(x) != J_NULL && !(json_type(x) == J_STR && x.n == 2)) {
            if (json_type(x) != J_STR) {
                set_err(s, "Invalid MCP tools/list cursor", 0);
                rc = -1;
            } else {
                cursor = json_strdup(x, 0);
                for (i = 0; i < nseen; i++)
                    if (cursor && !strcmp(seen[i], cursor)) {
                        set_err(s, "MCP tools/list returned duplicate cursor: ", cursor);
                        rc = -1;
                    }
                if (!rc) {
                    if (nseen == cap) {
                        cap = cap ? cap * 2 : 8;
                        seen = (char **)realloc(seen, (size_t)cap * sizeof(char *));
                    }
                    if (seen)
                        seen[nseen++] = dupz(cursor);
                }
            }
        }
        jw_free(&res);
        if (rc || !cursor)
            break;
    }
    free(cursor);
    for (i = 0; i < nseen; i++)
        free(seen[i]);
    free(seen);
    if (rc) {
        jw_free(&all);
        return -1;
    }
    /* the tools: pi's createMcpToolDefinition, names by registerTools' rule */
    {
        jw arr;
        jv a, t;
        jit it;
        long n, k = 0;
        char **plain;
        jw_init(&arr);
        jw_rawz(&arr, "[");
        if (all.n)
            jw_raw(&arr, all.p, all.n);
        jw_rawz(&arr, "]");
        jw_free(&all);
        json_parse(arr.p, arr.n, &a);
        n = json_count(a);
        free_tools(s);
        s->tools = (mcp_tool *)calloc((size_t)(n ? n : 1), sizeof(mcp_tool));
        plain = (char **)calloc((size_t)(n ? n : 1), sizeof(char *));
        if (!s->tools || !plain) {
            free(plain);
            jw_free(&arr);
            set_err(s, "out of memory", 0);
            return -1;
        }
        /* registerTools: the plain names of the server's distinct tool names; a name two of them sanitise to
         * is taken for both, as is one another server's tool holds or one given out already */
        {
            long j = 0;
            jit i2;
            jv t2, n2;
            json_iter(a, &it);
            while (json_next(&it, 0, &t)) {
                jv nm;
                char tn[256], pn[600];
                int dup = 0;
                long q = 0;
                json_get(t, "name", &nm);
                json_iter(a, &i2);
                while (q < j && json_next(&i2, 0, &t2)) {
                    if (json_get(t2, "name", &n2) && json_type(n2) == J_STR && n2.n == nm.n && !memcmp(n2.p, nm.p, (size_t)nm.n))
                        dup = 1;
                    q++;
                }
                j++;
                if (dup)
                    continue;
                json_str(nm, tn, sizeof(tn));
                mcp_tool_name(s->name, tn, 0, pn, sizeof(pn));
                plain[k++] = dupz(pn);
            }
        }
        json_iter(a, &it);
        while (json_next(&it, 0, &t)) {
            jv nm, sc, x, ann;
            mcp_tool *mt = &s->tools[s->ntools];
            char tn[256], name[600];
            int taken, j, c2 = 0;
            long q;
            json_get(t, "name", &nm);
            json_str(nm, tn, sizeof(tn));
            mcp_tool_name(s->name, tn, 0, name, sizeof(name));
            taken = is_taken(m, s, name);
            for (q = 0; q < k; q++)
                if (plain[q] && !strcmp(plain[q], name))
                    c2++;
            if (c2 > 1)
                taken = 1;
            for (j = 0; j < s->ntools; j++)
                if (!strcmp(s->tools[j].name, name))
                    taken = 1;
            mcp_tool_name(s->name, tn, taken, name, sizeof(name));
            mt->srv = s;
            mt->name = dupz(name);
            mt->tname = dupz(tn);
            json_get(t, "inputSchema", &sc);
            mt->params = parameters_of(sc);
            /* description: the server's (trimmed), else the title, else pi's words */
            {
                char *d = json_get(t, "description", &x) && json_type(x) == J_STR ? json_strdup(x, 0) : 0;
                char *b = d, *e;
                if (d) {
                    while (*b == ' ' || *b == '\n' || *b == '\t' || *b == '\r')
                        b++;
                    e = b + strlen(b);
                    while (e > b && (e[-1] == ' ' || e[-1] == '\n' || e[-1] == '\t' || e[-1] == '\r'))
                        e--;
                    *e = 0;
                }
                if (b && *b)
                    mt->desc = dupz(b);
                else if (json_get(t, "title", &x) && json_type(x) == J_STR && x.n > 2)
                    mt->desc = json_strdup(x, 0);
                else if (json_get(t, "annotations", &ann) && json_type(ann) == J_OBJ && json_get(ann, "title", &x) &&
                         json_type(x) == J_STR && x.n > 2)
                    mt->desc = json_strdup(x, 0);
                else {
                    jw w;
                    jw_init(&w);
                    jw_rawz(&w, "MCP tool ");
                    jw_rawz(&w, tn);
                    jw_rawz(&w, " from server ");
                    jw_rawz(&w, s->name);
                    mt->desc = w.p;
                }
                free(d);
            }
            mt->exposure = tool_exposure(s, tn);
            mt->pt.name = mt->name;
            mt->pt.description = mt->desc;
            mt->pt.parameters = mt->params;
            mt->pt.u = mt;
            mt->pt.run = tool_run;
            s->ntools++;
        }
        while (k > 0)
            free(plain[--k]);
        free(plain);
        jw_free(&arr);
    }
    return 0;
}

/* a name another server's tool holds */
static int is_taken(mcp *m, const mcp_server *self, const char *name)
{
    int i, j;
    for (i = 0; i < m->n; i++) {
        if (&m->srv[i] == self)
            continue;
        for (j = 0; j < m->srv[i].ntools; j++)
            if (!strcmp(m->srv[i].tools[j].name, name))
                return 1;
    }
    return 0;
}

/* the same server again (a dropped connection, an expired session): its tools stay as they were */
static int reconnect(mcp_server *s)
{
    disconnect(s);
    s->err[0] = 0;
    if ((s->kind == MK_HTTP ? mcp_http_open(s) : mcp_stdio_open(s)) || initialize(s)) {
        char e[300];
        cl_copy(e, s->err, sizeof(e));
        disconnect(s);
        cl_copy(s->err, e, sizeof(s->err));
        return -1;
    }
    s->connected = 1;
    return 0;
}

/* pi's open(): an HTTP server that failed with a network error or a transient status is tried again after
 * 250 ms and after 1 s more */
static int connect_one(mcp_server *s)
{
    static const long delays[] = {250, 1000};
    for (s->attempts = 1;; s->attempts++) {
        disconnect(s);
        s->err[0] = 0;
        s->next_id = 0;
        s->transient = 0;
        if (!((s->kind == MK_HTTP ? mcp_http_open(s) : mcp_stdio_open(s)) || initialize(s) || list_tools(s))) {
            s->connected = 1;
            return 0;
        }
        {
            char e[300];
            cl_copy(e, s->err, sizeof(e));
            disconnect(s);
            cl_copy(s->err, e, sizeof(s->err));
        }
        if (s->kind != MK_HTTP || !s->transient || s->attempts > 2)
            return -1;
        if (mcp_hooks(s->m)->sleep ? mcp_hooks(s->m)->sleep(mcp_hooks(s->m)->u, delays[s->attempts - 1]) : 0)
            return -1;          /* Ctrl+C in the wait */
    }
}

int mcp_connect(mcp *m)
{
    int i, n = 0;
    for (i = 0; i < m->n; i++) {
        mcp_server *s = &m->srv[i];
        if (!s->enabled)
            continue;
        if (connect_one(s)) {
            jw_rawz(&m->errors, "MCP server \"");
            jw_rawz(&m->errors, s->name);
            jw_rawz(&m->errors, "\" failed: ");
            err_line(m, s->err, 0, 0);
        } else
            n++;
    }
    return n;
}

/* ---- results (pi's tools.ts convertMcpResult, pi-mcp's toLlmContent) ---- */

static int text_mime(const char *mt)
{
    char t[96];
    long i;
    if (!mt)
        return 0;
    for (i = 0; mt[i] && mt[i] != ';' && i < (long)sizeof(t) - 1; i++)
        t[i] = (char)(mt[i] >= 'A' && mt[i] <= 'Z' ? mt[i] + 32 : mt[i]);
    t[i] = 0;
    while (i > 0 && t[i - 1] == ' ')
        t[--i] = 0;
    {
        long n = (long)strlen(t);
        return !strncmp(t, "text/", 5) || !strcmp(t, "application/json") || (n >= 5 && !strcmp(t + n - 5, "+json")) ||
               (n >= 4 && !strcmp(t + n - 4, "+xml"));
    }
}

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

/* Buffer.from(blob, "base64"): characters outside the alphabet skipped, padding ends it */
static void b64_decode(const char *s, jw *out)
{
    unsigned long acc = 0;
    int bits = 0;
    for (; *s && *s != '='; s++) {
        int v = b64v((unsigned char)*s);
        if (v < 0)
            continue;
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            char c;
            bits -= 8;
            c = (char)((acc >> bits) & 0xff);
            jw_raw(out, &c, 1);
        }
    }
}

/* JSON.stringify(value, null, 2): members and elements on lines of their own, two spaces a level; strings
 * escaped as JavaScript does (the escapes JSON requires, other characters as they are); numbers as written */
static void js_string(jw *w, jv v)
{
    long n, i;
    char *s = json_strdup(v, &n);
    static const char hex[] = "0123456789abcdef";
    jw_rawz(w, "\"");
    for (i = 0; s && i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')
            jw_rawz(w, "\\\"");
        else if (c == '\\')
            jw_rawz(w, "\\\\");
        else if (c == '\n')
            jw_rawz(w, "\\n");
        else if (c == '\r')
            jw_rawz(w, "\\r");
        else if (c == '\t')
            jw_rawz(w, "\\t");
        else if (c == '\b')
            jw_rawz(w, "\\b");
        else if (c == '\f')
            jw_rawz(w, "\\f");
        else if (c < 0x20) {
            char e[7];
            e[0] = '\\';
            e[1] = 'u';
            e[2] = '0';
            e[3] = '0';
            e[4] = hex[c >> 4];
            e[5] = hex[c & 15];
            e[6] = 0;
            jw_rawz(w, e);
        } else
            jw_raw(w, (const char *)&s[i], 1);
    }
    jw_rawz(w, "\"");
    free(s);
}

static void indent(jw *w, int d)
{
    int i;
    jw_rawz(w, "\n");
    for (i = 0; i < d; i++)
        jw_rawz(w, "  ");
}

static void js_pretty(jw *w, jv v, int d)
{
    jit it;
    jv k, x;
    int t = json_type(v), any = 0;
    if (t == J_OBJ || t == J_ARR) {
        jw_rawz(w, t == J_OBJ ? "{" : "[");
        json_iter(v, &it);
        while (json_next(&it, t == J_OBJ ? &k : 0, &x)) {
            if (any)
                jw_rawz(w, ",");
            any = 1;
            indent(w, d + 1);
            if (t == J_OBJ) {
                js_string(w, k);
                jw_rawz(w, ": ");
            }
            js_pretty(w, x, d + 1);
        }
        if (any)
            indent(w, d);
        jw_rawz(w, t == J_OBJ ? "}" : "]");
    } else if (t == J_STR)
        js_string(w, v);
    else
        jw_raw(w, v.p, v.n);
}

/* one model block appended to blocks (commas kept); the text also to texts (pi's textOf, "\n" between) */
typedef struct conv {
    jw blocks, texts;
    int nb, nt;
} conv;

static void add_text(conv *c, const char *s, long n)
{
    if (c->nb++)
        jw_rawz(&c->blocks, ",");
    llm_b_text(&c->blocks, s, n, 0);
    if (c->nt++)
        jw_rawz(&c->texts, "\n");
    jw_raw(&c->texts, s, n);
}

static void add_textz(conv *c, const char *s)
{
    add_text(c, s, (long)strlen(s));
}

static void add_image(conv *c, jv data, jv mime)
{
    char *d = json_strdup(data, 0), *mt = json_strdup(mime, 0);
    if (c->nb++)
        jw_rawz(&c->blocks, ",");
    llm_b_image(&c->blocks, d ? d : "", mt ? mt : "");
    free(d);
    free(mt);
}

static char *str_of(jv o, const char *key)
{
    jv x;
    return json_get(o, key, &x) && json_type(x) == J_STR ? json_strdup(x, 0) : 0;
}

/* the extension of a saved binary resource: the one its URI's path ends in (1-8 letters and digits), else .bin */
static void ext_of(const char *uri, char *out, long cap)
{
    const char *p = uri, *e, *dot;
    long i, k;
    const char *q = strstr(uri, "://");
    if (q) {
        p = strchr(q + 3, '/');
        if (!p)
            p = q + strlen(q);
    }
    e = p;
    while (*e && *e != '?' && *e != '#')
        e++;
    dot = e;
    while (dot > p && dot[-1] != '.' && dot[-1] != '/')
        dot--;
    k = (long)(e - dot);
    if (dot > p && dot[-1] == '.' && k >= 1 && k <= 8) {
        for (i = 0; i < k; i++)
            if (!((dot[i] >= 'a' && dot[i] <= 'z') || (dot[i] >= 'A' && dot[i] <= 'Z') || (dot[i] >= '0' && dot[i] <= '9')))
                break;
        if (i == k) {
            cl_copy(out, ".", cap);
            if (k < cap - 1) {
                memcpy(out + 1, dot, (size_t)k);
                out[k + 1] = 0;
            }
            return;
        }
    }
    cl_copy(out, ".bin", cap);
}

/* a binary resource: a text type decoded as text, anything else saved to a temporary file and named */
static void resource_blob(mcp *m, const char *id, int *nsaved, const char *uri, const char *mt, jv blob, conv *c)
{
    jw data;
    char *bs = json_strdup(blob, 0);
    jw_init(&data);
    b64_decode(bs ? bs : "", &data);
    free(bs);
    if (text_mime(mt))
        add_text(c, data.p ? data.p : "", data.n);
    else {
        char kind[160], sz[32], path[300], ext[16], sid[96];
        jw w;
        cl_copy(kind, mt ? mt : "unknown type", sizeof(kind));
        pi_size(data.n, sz, sizeof(sz));
        cl_cat(kind, ", ", sizeof(kind));
        cl_cat(kind, sz, sizeof(kind));
        ext_of(uri ? uri : "", ext, sizeof(ext));
        cl_copy(sid, id, sizeof(sid));
        cl_cat(sid, "-", sizeof(sid));
        cl_ltoa((long)++*nsaved, sz);
        cl_cat(sid, sz, sizeof(sid));
        pi_tmp_path(m->tmp, sid, ext, path, sizeof(path));
        jw_init(&w);
        jw_rawz(&w, "[Binary resource ");
        jw_rawz(&w, uri ? uri : "undefined");
        jw_rawz(&w, " (");
        jw_rawz(&w, kind);
        if (m->sys && !m->sys->write(m->sys->u, path, data.p ? data.p : "", data.n)) {
            jw_rawz(&w, ") saved to ");
            jw_rawz(&w, path);
        } else {
            jw_rawz(&w, ") could not be saved: ");
            jw_rawz(&w, m->sys ? m->sys->err(m->sys->u) : "no file system");
        }
        jw_rawz(&w, "]");
        add_text(c, w.p, w.n);
        jw_free(&w);
    }
    jw_free(&data);
}

static void block_content(mcp *m, const char *id, int *nsaved, jv b, conv *c)
{
    jv x, r;
    char *type = str_of(b, "type");
    if (!type)
        type = dupz("");
    if (!strcmp(type, "text")) {
        char *t = str_of(b, "text");
        add_textz(c, t ? t : "");
        free(t);
    } else if (!strcmp(type, "image")) {
        jv d, mt;
        if (json_get(b, "data", &d) && json_get(b, "mimeType", &mt))
            add_image(c, d, mt);
    } else if (!strcmp(type, "audio")) {
        char *mt = str_of(b, "mimeType");
        jw w;
        jw_init(&w);
        jw_rawz(&w, "[audio ");
        jw_rawz(&w, mt ? mt : "undefined");
        jw_rawz(&w, " omitted]");
        add_text(c, w.p, w.n);
        jw_free(&w);
        free(mt);
    } else if (!strcmp(type, "resource_link")) {
        /* tools.ts: [Resource <uri> "<title or name>" (<mime>, <size>): <description>] */
        char *uri = str_of(b, "uri"), *title = str_of(b, "title"), *name = str_of(b, "name");
        char *mt = str_of(b, "mimeType"), *desc = str_of(b, "description");
        jw w;
        int det = 0;
        jw_init(&w);
        jw_rawz(&w, "[Resource ");
        jw_rawz(&w, uri ? uri : "");
        jw_rawz(&w, " \"");
        jw_rawz(&w, title ? title : name ? name : "undefined");
        jw_rawz(&w, "\"");
        if (mt && *mt) {
            jw_rawz(&w, " (");
            jw_rawz(&w, mt);
            det = 1;
        }
        if (json_get(b, "size", &x) && json_type(x) == J_NUM) {
            char sz[32];
            pi_size(json_long(x, 0), sz, sizeof(sz));
            jw_rawz(&w, det ? ", " : " (");
            jw_rawz(&w, sz);
            det = 1;
        }
        if (det)
            jw_rawz(&w, ")");
        if (desc && *desc) {
            jw_rawz(&w, ": ");
            jw_rawz(&w, desc);
        }
        jw_rawz(&w, "]");
        add_text(c, w.p, w.n);
        jw_free(&w);
        free(uri);
        free(title);
        free(name);
        free(mt);
        free(desc);
    } else if (!strcmp(type, "resource") && json_get(b, "resource", &r) && json_type(r) == J_OBJ) {
        /* tools.ts: a blob that is not an image is text (a text type) or a saved file; the rest is toLlmContent's:
         * text as text, an image type as an image, else a placeholder */
        char *mt = str_of(r, "mimeType"), *uri = str_of(r, "uri");
        int image = mt && !strncmp(mt, "image/", 6);
        jv text, blob, mtv;
        if (json_get(r, "blob", &blob) && !image)
            resource_blob(m, id, nsaved, uri, mt, blob, c);
        else if (json_get(r, "text", &text)) {
            char *t = json_strdup(text, 0);
            add_textz(c, t ? t : "undefined");
            free(t);
        } else if (image && json_get(r, "mimeType", &mtv) && json_get(r, "blob", &blob))
            add_image(c, blob, mtv);
        else {
            jw w;
            jw_init(&w);
            jw_rawz(&w, "[binary resource ");
            jw_rawz(&w, uri ? uri : "undefined");
            jw_rawz(&w, " (");
            jw_rawz(&w, mt ? mt : "unknown type");
            jw_rawz(&w, ") omitted]");
            add_text(c, w.p, w.n);
            jw_free(&w);
        }
        free(mt);
        free(uri);
    } else {
        jw w;
        jw_init(&w);
        jw_rawz(&w, "[unsupported MCP content ");
        jw_rawz(&w, type[0] || json_get(b, "type", &x) ? type : "undefined");
        jw_rawz(&w, "]");
        add_text(c, w.p, w.n);
        jw_free(&w);
    }
    free(type);
}

/* truncateMiddle's cut of s[0..n) to max bytes: head end, tail start, removed characters */
static long utf8_chars(const char *s, long n)
{
    long i, k = 0;
    for (i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            k++;
    return k;
}

int mcp_limit(cl_sys *sys, const char *tmp, const char *id, const char *blocks, long n, jw *out)
{
    jv a, b, x;
    jit it;
    jw texts;
    int nt = 0;
    long lines, he, ts, removed;
    jw_init(&texts);
    if (json_parse(blocks, n, &a) || json_type(a) != J_ARR) {
        jw_raw(out, blocks, n);
        return 0;
    }
    json_iter(a, &it);
    while (json_next(&it, 0, &b))
        if (json_get(b, "type", &x) && json_streq(x, "text") && json_get(b, "text", &x)) {
            char *t;
            long tn;
            t = json_strdup(x, &tn);
            if (nt++)
                jw_rawz(&texts, "\n");
            jw_raw(&texts, t ? t : "", t ? tn : 0);
            free(t);
        }
    if (texts.n <= MCP_OUTPUT_MAX) {
        jw_free(&texts);
        jw_raw(out, blocks, n);
        return 0;
    }
    /* splitLinesForCounting */
    {
        long i;
        lines = 1;
        for (i = 0; i < texts.n; i++)
            if (texts.p[i] == '\n')
                lines++;
        if (texts.p[texts.n - 1] == '\n')
            lines--;
    }
    he = MCP_OUTPUT_MAX / 2;
    while (he > 0 && ((unsigned char)texts.p[he] & 0xc0) == 0x80)
        he--;
    ts = texts.n - (MCP_OUTPUT_MAX - MCP_OUTPUT_MAX / 2);
    while (ts < texts.n && ((unsigned char)texts.p[ts] & 0xc0) == 0x80)
        ts++;
    removed = utf8_chars(texts.p + he, ts - he);
    {
        jw t;
        char num[24], path[300];
        int saved;
        jw_init(&t);
        jw_rawz(&t, "Warning: truncated output (original token count: ");
        cl_ltoa((texts.n + 3) / 4, num);
        jw_rawz(&t, num);
        jw_rawz(&t, ")\nTotal output lines: ");
        cl_ltoa(lines, num);
        jw_rawz(&t, num);
        jw_rawz(&t, "\n\n");
        jw_raw(&t, texts.p, he);
        jw_rawz(&t, "\xe2\x80\xa6");
        cl_ltoa(removed, num);
        jw_rawz(&t, num);
        jw_rawz(&t, " chars truncated\xe2\x80\xa6");
        jw_raw(&t, texts.p + ts, texts.n - ts);
        jw_rawz(&t, "\n\n");
        pi_cut_path(tmp, id, path, sizeof(path));
        saved = sys && !sys->write(sys->u, path, texts.p, texts.n);
        if (saved) {
            jw_rawz(&t, "[Full output: ");
            jw_rawz(&t, path);
            jw_rawz(&t, " (read it with offset/limit)]");
        } else {
            jw_rawz(&t, "[Could not save the full output: ");
            jw_rawz(&t, sys ? sys->err(sys->u) : "no file system");
            jw_rawz(&t, "]");
        }
        jw_rawz(out, "[");
        llm_b_text(out, t.p, t.n, 0);
        json_iter(a, &it);
        while (json_next(&it, 0, &b))
            if (json_get(b, "type", &x) && json_streq(x, "image")) {
                jw_rawz(out, ",");
                jw_raw(out, b.p, b.n);
            }
        jw_rawz(out, "]");
        jw_free(&t);
    }
    jw_free(&texts);
    return 1;
}

int mcp_result(mcp *m, const char *server, const char *tool, const char *id, const char *res, long n, jw *out,
               int *is_error)
{
    jv v, content, sc, b, x;
    jit it;
    conv c;
    int nsaved = 0;
    *is_error = 0;
    if (json_parse(res, n, &v) || json_type(v) != J_OBJ ||
        (json_get(v, "content", &content) && json_type(content) != J_ARR)) {
        jw_rawz(out, "[");
        llm_b_text(out, "Invalid MCP tools/call result", 29, 0);
        jw_rawz(out, "]");
        *is_error = 1;
        return -1;
    }
    if (json_get(v, "structuredContent", &sc) && json_type(sc) != J_OBJ) {
        jw_rawz(out, "[");
        llm_b_text(out, "Invalid MCP tools/call structured content", 41, 0);
        jw_rawz(out, "]");
        *is_error = 1;
        return -1;
    }
    memset(&c, 0, sizeof(c));
    jw_init(&c.blocks);
    jw_init(&c.texts);
    if (json_get(v, "content", &content) && json_count(content) > 0) {
        json_iter(content, &it);
        while (json_next(&it, 0, &b))
            if (json_type(b) == J_OBJ)
                block_content(m, id, &nsaved, b, &c);
    } else if (json_get(v, "structuredContent", &sc)) {
        jw p;
        jw_init(&p);
        js_pretty(&p, sc, 0);
        add_text(&c, p.p, p.n);
        jw_free(&p);
    }
    *is_error = json_get(v, "isError", &x) && json_type(x) == J_TRUE;
    if (*is_error && c.texts.n == 0) {
        jw w;
        jw_init(&w);
        jw_rawz(&w, "MCP tool ");
        jw_rawz(&w, server);
        jw_rawz(&w, "/");
        jw_rawz(&w, tool);
        jw_rawz(&w, " returned an error");
        add_text(&c, w.p, w.n);
        jw_free(&w);
    }
    {
        jw arr;
        jw_init(&arr);
        jw_rawz(&arr, "[");
        if (c.blocks.n)
            jw_raw(&arr, c.blocks.p, c.blocks.n);
        jw_rawz(&arr, "]");
        mcp_limit(m->sys, m->tmp, id, arr.p, arr.n, out);
        jw_free(&arr);
    }
    jw_free(&c.blocks);
    jw_free(&c.texts);
    return 0;
}

/* ---- the tool ---- */

static int tool_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    mcp_tool *t = (mcp_tool *)u;
    mcp_server *sv = t->srv;
    const char *id = s && s->call_id ? s->call_id : "call";
    jv a, sch;
    char err[300];
    *is_error = 1;
    if (json_parse(args, an, &a) || json_type(a) != J_OBJ) {
        jw w;
        jw_init(&w);
        jw_rawz(&w, "Validation failed for tool \"");
        jw_rawz(&w, t->name);
        jw_rawz(&w, "\": the arguments are not a JSON object");
        jw_rawz(content, "[");
        llm_b_text(content, w.p, w.n, 0);
        jw_rawz(content, "]");
        jw_free(&w);
        return 0;
    }
    if (!json_parse(t->params, (long)strlen(t->params), &sch) && schema_check(sch, a, err, sizeof(err))) {
        jw w;
        jw_init(&w);
        jw_rawz(&w, "Validation failed for tool \"");
        jw_rawz(&w, t->name);
        jw_rawz(&w, "\":\n  - ");
        jw_rawz(&w, err);
        jw_rawz(content, "[");
        llm_b_text(content, w.p, w.n, 0);
        jw_rawz(content, "]");
        jw_free(&w);
        return 0;
    }
    /* a dropped connection reconnects on the next call (pi) */
    if (!sv->connected && sv->enabled && reconnect(sv)) {
        jw w;
        jw_init(&w);
        jw_rawz(&w, "MCP server \"");
        jw_rawz(&w, sv->name);
        jw_rawz(&w, "\" is not connected: ");
        jw_rawz(&w, sv->err);
        jw_rawz(content, "[");
        llm_b_text(content, w.p, w.n, 0);
        jw_rawz(content, "]");
        jw_free(&w);
        return 0;
    }
    {
        jw params, res;
        int rc;
        jw_init(&params);
        jw_init(&res);
        jw_rawz(&params, "{\"name\":");
        jw_strz(&params, t->tname);
        jw_rawz(&params, ",\"arguments\":");
        jw_raw(&params, args, an);
        jw_rawz(&params, "}");
        mcp_call_count++;
        rc = rpc(sv, "tools/call", params.p, &res);
        jw_free(&params);
        if (rc) {
            jw_rawz(content, "[");
            llm_b_text(content, sv->err, (long)strlen(sv->err), 0);
            jw_rawz(content, "]");
            *is_error = 1;
        } else
            mcp_result(sv->m, sv->name, t->tname, id, res.p ? res.p : "{}", res.p ? res.n : 2, content, is_error);
        jw_free(&res);
    }
    return s && s->h.poll && s->h.poll(s->h.u) ? 1 : 0;
}

mcp_tool *mcp_tool_named(mcp *m, const char *name)
{
    int i, j;
    for (i = 0; i < m->n; i++)
        for (j = 0; j < m->srv[i].ntools; j++)
            if (!strcmp(m->srv[i].tools[j].name, name))
                return &m->srv[i].tools[j];
    return 0;
}

/* ---- fixed point for BM25 (no floating point in amiga-pi: a 68020 has no FPU) ---- */

#define FX_ONE (1UL << 20)
#define U32(x) ((x) & 0xffffffffUL)

/* a * b / c with the product in 64 bits, all values 32-bit unsigned (the quotient must fit 32 bits) */
static unsigned long mul_div(unsigned long a, unsigned long b, unsigned long c)
{
    unsigned long al = a & 0xffff, ah = U32(a) >> 16, bl = b & 0xffff, bh = U32(b) >> 16;
    unsigned long lo = al * bl, m1 = al * bh, m2 = ah * bl, hi = ah * bh, t, q = 0, r;
    int i;
    t = (lo >> 16) + (m1 & 0xffff) + (m2 & 0xffff);
    lo = (lo & 0xffff) | U32((t & 0xffff) << 16);
    hi = U32(hi + (m1 >> 16) + (m2 >> 16) + (t >> 16));
    if (!c)
        return 0;
    r = hi % c;                 /* hi < c when the quotient fits; the remainder carries on */
    for (i = 31; i >= 0; i--) {
        int top = (int)((r >> 31) & 1);
        r = U32((r << 1) | ((lo >> i) & 1));
        q = U32(q << 1);
        if (top || r >= c) {
            r = U32(r - c);
            q |= 1;
        }
    }
    return q;
}

/* ln(x) * 2^20 for x >= 1: x = 2^k m, ln m = 2 atanh((m - 1) / (m + 1)) by its series */
static unsigned long ln_fx(unsigned long x)
{
    static const unsigned long ln2 = 726817UL;      /* ln 2 * 2^20 */
    unsigned long p = 1, z, zz, term, sum;
    int k = 0;
    long n;
    while (k < 31 && (p << 1) <= x) {
        p <<= 1;
        k++;
    }
    /* z = (x - p) / (x + p) at 2^28, at most 1/3 */
    z = mul_div(x - p, 1UL << 28, x + p);
    zz = mul_div(z, z, 1UL << 28);
    sum = z;
    term = z;
    for (n = 3; term; n += 2) {
        term = mul_div(term, zz, 1UL << 28);
        sum += term / (unsigned long)n;
    }
    return (unsigned long)k * ln2 + ((2 * sum + 128) >> 8);
}

/* ---- tool_search (pi's tool-search/tool.ts) ---- */

static const char *const stop_words[] = {"a", "an", "and", "are", "as", "at", "be", "by", "for", "from", "in",
                                         "is", "it", "of", "on", "or", "that", "the", "this", "to", "with", 0};

/* tokenize: camelCase split, lower case, non-alphanumerics split, stop words out, naive singular; terms as
 * "term\n" in out, their count */
static long tokenize(const char *s, jw *out)
{
    jw t;
    long i, n, count = 0;
    jw_init(&t);
    n = (long)strlen(s);
    /* replace(/([a-z0-9])([A-Z])/g, "$1 $2") then replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2") */
    for (i = 0; i < n; i++) {
        char c = s[i];
        jw_raw(&t, &c, 1);
        if (((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) && i + 1 < n && s[i + 1] >= 'A' && s[i + 1] <= 'Z') {
            /* the regex consumes both characters: an upper case letter after it cannot start another match */
            jw_raw(&t, " ", 1);
            jw_raw(&t, &s[++i], 1);
        }
    }
    {
        jw u;
        long j, k;
        jw_init(&u);
        k = t.n;
        for (j = 0; j < k;) {
            /* ([A-Z]+)([A-Z][a-z]): the greedy run of capitals, its last one starts the next word */
            if (t.p[j] >= 'A' && t.p[j] <= 'Z') {
                long e = j;
                while (e < k && t.p[e] >= 'A' && t.p[e] <= 'Z')
                    e++;
                if (e - j >= 2 && e < k && t.p[e] >= 'a' && t.p[e] <= 'z') {
                    jw_raw(&u, t.p + j, e - 1 - j);
                    jw_raw(&u, " ", 1);
                    jw_raw(&u, t.p + e - 1, 2);
                    j = e + 1;
                    continue;
                }
                jw_raw(&u, t.p + j, e - j);
                j = e;
                continue;
            }
            jw_raw(&u, t.p + j, 1);
            j++;
        }
        jw_free(&t);
        t = u;
    }
    for (i = 0; i < t.n;) {
        char term[128];
        long k = 0, len;
        int sw = 0, w;
        while (i < t.n && !((t.p[i] >= 'a' && t.p[i] <= 'z') || (t.p[i] >= 'A' && t.p[i] <= 'Z') ||
                            (t.p[i] >= '0' && t.p[i] <= '9')))
            i++;
        while (i < t.n && ((t.p[i] >= 'a' && t.p[i] <= 'z') || (t.p[i] >= 'A' && t.p[i] <= 'Z') ||
                           (t.p[i] >= '0' && t.p[i] <= '9'))) {
            if (k < (long)sizeof(term) - 1)
                term[k++] = (char)(t.p[i] >= 'A' && t.p[i] <= 'Z' ? t.p[i] + 32 : t.p[i]);
            i++;
        }
        term[k] = 0;
        if (!k)
            continue;
        for (w = 0; stop_words[w]; w++)
            if (!strcmp(term, stop_words[w]))
                sw = 1;
        if (sw)
            continue;
        len = k;
        if (len > 4 && !strcmp(term + len - 3, "ies")) {
            term[len - 3] = 'y';
            term[len - 2] = 0;
        } else if (len > 4 && (!strcmp(term + len - 4, "ches") || !strcmp(term + len - 4, "shes") ||
                               !strcmp(term + len - 4, "sses") || !strcmp(term + len - 3, "xes") ||
                               !strcmp(term + len - 3, "zes")))
            term[len - 2] = 0;
        else if (len > 3 && term[len - 1] == 's' && term[len - 2] != 's')
            term[len - 1] = 0;
        jw_rawz(out, term);
        jw_rawz(out, "\n");
        count++;
    }
    jw_free(&t);
    return count;
}

/* schemaText: descriptions and property names, recursively (items, anyOf, oneOf, allOf) */
static void schema_text(jv sc, jw *parts)
{
    jv x, k, p;
    jit it;
    static const char *const keys[] = {"anyOf", "oneOf", "allOf"};
    int i;
    if (json_type(sc) != J_OBJ)
        return;
    if (json_get(sc, "description", &x) && json_type(x) == J_STR) {
        char *d = json_strdup(x, 0);
        jw_rawz(parts, d ? d : "");
        jw_raw(parts, "\0", 1);
        free(d);
    }
    if (json_get(sc, "properties", &x) && json_type(x) == J_OBJ) {
        json_iter(x, &it);
        while (json_next(&it, &k, &p)) {
            char *nm = json_strdup(k, 0);
            jw_rawz(parts, nm ? nm : "");
            jw_raw(parts, "\0", 1);
            free(nm);
            schema_text(p, parts);
        }
    }
    if (json_get(sc, "items", &x))
        schema_text(x, parts);
    for (i = 0; i < 3; i++)
        if (json_get(sc, keys[i], &x) && json_type(x) == J_ARR) {
            jv e;
            json_iter(x, &it);
            while (json_next(&it, 0, &e))
                schema_text(e, parts);
        }
}

static int blank(const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            return 0;
    return 1;
}

/* createToolSearchDocument: the parts that are not blank, joined by " " */
static void search_doc(const mcp_tool *t, jw *doc)
{
    jw parts;
    long i, st = 0;
    int any = 0;
    char ns[96];
    jv sc;
    jw_init(&parts);
    jw_rawz(&parts, t->name);
    jw_raw(&parts, "\0", 1);
    for (i = 0; t->name[i]; i++)
        jw_raw(&parts, t->name[i] == '_' ? " " : &t->name[i], 1);
    jw_raw(&parts, "\0", 1);
    jw_rawz(&parts, t->desc);
    jw_raw(&parts, "\0", 1);
    if (!json_parse(t->params, (long)strlen(t->params), &sc))
        schema_text(sc, &parts);
    namespace_of(t->srv->name, ns, sizeof(ns));
    jw_rawz(&parts, ns);
    jw_raw(&parts, "\0", 1);
    jw_rawz(&parts, t->srv->description ? t->srv->description : "");
    jw_raw(&parts, "\0", 1);
    jw_rawz(&parts, t->srv->instructions ? t->srv->instructions : "");
    jw_raw(&parts, "\0", 1);
    for (i = 0; i < parts.n; i++)
        if (!parts.p[i]) {
            if (!blank(parts.p + st)) {
                if (any++)
                    jw_rawz(doc, " ");
                jw_rawz(doc, parts.p + st);
            }
            st = i + 1;
        }
    jw_free(&parts);
}

/* the count of term in a "t\n" list */
static long term_count(const char *list, const char *term)
{
    long c = 0, n = (long)strlen(term);
    const char *p = list;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        if (e && e - p == n && !strncmp(p, term, (size_t)n))
            c++;
        p = e ? e + 1 : 0;
    }
    return c;
}

int mcp_search(mcp *m, pi_session *s, const char *query, long limit, jw *text)
{
    mcp_tool **cand;
    jw *terms;
    long *len, nc = 0, i, j, nq, kept = 0;
    unsigned long *score, total = 0;
    jw q, qu;
    int k;
    if (blank(query)) {
        jw_rawz(text, "query must not be empty");
        return -1;
    }
    if (limit <= 0) {
        jw_rawz(text, "limit must be a positive integer");
        return -1;
    }
    for (k = 0; k < m->n; k++)
        nc += m->srv[k].ntools;
    cand = (mcp_tool **)calloc((size_t)(nc + 1), sizeof(*cand));
    terms = (jw *)calloc((size_t)(nc + 1), sizeof(jw));
    len = (long *)calloc((size_t)(nc + 1), sizeof(long));
    score = (unsigned long *)calloc((size_t)(nc + 1), sizeof(unsigned long));
    if (!cand || !terms || !len || !score) {
        free(cand);
        free(terms);
        free(len);
        free(score);
        jw_rawz(text, "out of memory");
        return -1;
    }
    nc = 0;
    for (k = 0; k < m->n; k++)
        for (j = 0; j < m->srv[k].ntools; j++) {
            mcp_tool *t = &m->srv[k].tools[j];
            if (t->exposure == MX_DEFERRED && !t->active && m->srv[k].connected)
                cand[nc++] = t;
        }
    jw_init(&q);
    jw_init(&qu);
    nq = tokenize(query, &q);
    /* the query's distinct terms in order */
    {
        const char *p = q.p;
        while (p && *p) {
            const char *e = strchr(p, '\n');
            char term[128];
            long n = (long)(e - p);
            if (n >= (long)sizeof(term))
                n = (long)sizeof(term) - 1;
            memcpy(term, p, (size_t)n);
            term[n] = 0;
            if (!term_count(qu.p ? qu.p : "", term)) {
                jw_rawz(&qu, term);
                jw_rawz(&qu, "\n");
            }
            p = e + 1;
        }
    }
    (void)nq;
    if (qu.n && nc) {
        for (i = 0; i < nc; i++) {
            jw doc;
            jw_init(&doc);
            search_doc(cand[i], &doc);
            jw_init(&terms[i]);
            len[i] = tokenize(doc.p ? doc.p : "", &terms[i]);
            total += (unsigned long)len[i];
            jw_free(&doc);
        }
        {
            const char *p = qu.p;
            while (*p) {
                const char *e = strchr(p, '\n');
                char term[128];
                long n = (long)(e - p), freq = 0;
                unsigned long idf;
                memcpy(term, p, (size_t)n);
                term[n] = 0;
                for (i = 0; i < nc; i++)
                    if (term_count(terms[i].p ? terms[i].p : "", term))
                        freq++;
                /* log(1 + (N - f + 0.5) / (f + 0.5)) = ln(2N + 2) - ln(2f + 1) */
                idf = ln_fx((unsigned long)(2 * nc + 2)) - ln_fx((unsigned long)(2 * freq + 1));
                for (i = 0; i < nc; i++) {
                    unsigned long c = (unsigned long)term_count(terms[i].p ? terms[i].p : "", term);
                    if (c) {
                        /* c (k1 + 1) / (c + k1 (1 - b + b len / avg)), avg = total / N, k1 1.2, b 0.75:
                         * 22 c L / (10 c L + 3 L + 9 len N) */
                        unsigned long num = 22UL * c * total;
                        unsigned long den = 10UL * c * total + 3UL * total + 9UL * (unsigned long)len[i] * (unsigned long)nc;
                        score[i] += mul_div(idf, mul_div(num, FX_ONE, den), FX_ONE);
                    }
                }
                p = e + 1;
            }
        }
    }
    /* matches (score > 0), best first, ties in document order, at most limit */
    {
        long *order = (long *)calloc((size_t)(nc + 1), sizeof(long)), no = 0;
        int full;
        for (i = 0; i < nc; i++)
            if (score[i] > 0) {
                long at = no;
                while (at > 0 && score[order[at - 1]] < score[i]) {
                    order[at] = order[at - 1];
                    at--;
                }
                order[at] = i;
                no++;
            }
        kept = no < limit ? no : limit;
        /* declared first: a tool the session has no room for (PI_TOOLS) is not loaded, not counted, and stays
         * findable by a later search */
        {
            long loaded = 0;
            full = 0;
            for (i = 0; i < kept; i++) {
                mcp_tool *t = cand[order[i]];
                if (s && pi_tool_add(s, &t->pt))
                    continue;
                t->active = 1;
                order[loaded++] = order[i];
            }
            full = kept && !loaded;
            kept = loaded;
        }
        if (full)
            jw_rawz(text, "No tools could be loaded: the session holds as many tools as it can.");
        else if (!kept)
            jw_rawz(text, "No matching tools found.");
        else {
            char num[16];
            jw_rawz(text, "Loaded ");
            cl_ltoa(kept, num);
            jw_rawz(text, num);
            jw_rawz(text, kept == 1 ? " tool" : " tools");
            jw_rawz(text, ". They are available from your next call:");
            for (i = 0; i < kept; i++) {
                mcp_tool *t = cand[order[i]];
                const char *d = t->desc, *e, *end;
                while (ws(*d))
                    d++;
                end = d + strlen(d);
                while (end > d && ws(end[-1]))
                    end--;
                e = d;
                while (e < end && *e != '\n')
                    e++;
                if (e < end && e > d && e[-1] == '\r')
                    e--;
                jw_rawz(text, "\n- ");
                jw_rawz(text, t->name);
                jw_rawz(text, ": ");
                jw_raw(text, d, (long)(e - d));
            }
        }
        free(order);
    }
    for (i = 0; i < nc; i++)
        jw_free(&terms[i]);
    free(cand);
    free(terms);
    free(len);
    free(score);
    jw_free(&q);
    jw_free(&qu);
    return 0;
}

static const char search_desc[] =
    "# Tool discovery\n\nSearches over deferred tool metadata with BM25 and exposes matching tools for the next "
    "model call.\n\nSome of the tools, such as tools of MCP servers, may not have been provided to you upfront, "
    "and you should use this tool (`tool_search`) to search for the required tools. For MCP tool discovery, "
    "always use `tool_search`.";
static const char search_params[] =
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query for deferred "
    "tools.\"},\"limit\":{\"type\":\"number\",\"description\":\"Maximum number of tools to return. Defaults to "
    "8.\"}},\"required\":[\"query\"]}";

static int search_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    mcp *m = (mcp *)u;
    jv a, x, sch;
    char *query = 0, err[300];
    long limit = MCP_SEARCH_LIMIT;
    jw text;
    jw_init(&text);
    *is_error = 0;
    if (json_parse(args, an, &a) || json_type(a) != J_OBJ) {
        jw_rawz(&text, "Validation failed for tool \"tool_search\": the arguments are not a JSON object");
        *is_error = 1;
    } else if (!json_parse(search_params, (long)strlen(search_params), &sch) &&
               schema_check(sch, a, err, sizeof(err))) {
        jw_rawz(&text, "Validation failed for tool \"tool_search\":\n  - ");
        jw_rawz(&text, err);
        *is_error = 1;
    } else {
        json_get(a, "query", &x);
        query = json_strdup(x, 0);
        if (json_get(a, "limit", &x)) {
            long l;
            int neg, frac;
            limit = !num_scaled(x, 0, &l, &neg, &frac) && !neg && !frac ? l : 0;   /* Number.isInteger and > 0 */
        }
        if (mcp_search(m, s, query ? query : "", limit, &text))
            *is_error = 1;
    }
    jw_rawz(content, "[");
    llm_b_text(content, text.p ? text.p : "", text.n, 0);
    jw_rawz(content, "]");
    jw_free(&text);
    free(query);
    return 0;
}

/* ---- the mcp_servers section (pi's renderServersSection; the reach is always tool_search here) ---- */

/* serverSummary: the trimmed description, else the instructions; its first line, trimmed */
static void summary_of(const mcp_server *s, char *out, long cap)
{
    const char *b = s->description ? s->description : "", *e;
    long n;
    while (ws(*b))
        b++;
    if (!*b)
        b = s->instructions ? s->instructions : "";
    e = b;
    while (*e && *e != '\n')
        e++;
    while (b < e && ws(*b))
        b++;
    while (e > b && ws(e[-1]))
        e--;
    n = (long)(e - b) < cap - 1 ? (long)(e - b) : cap - 1;
    memcpy(out, b, (size_t)n);
    out[n] = 0;
}

static int indirect(const mcp_server *s)
{
    jv o, k, v;
    jit it;
    if (s->exposure == MX_CODEMODE || s->exposure == MX_DEFERRED)
        return 1;
    if (s->tool_exposure && !json_parse(s->tool_exposure, (long)strlen(s->tool_exposure), &o)) {
        json_iter(o, &it);
        while (json_next(&it, &k, &v))
            if (exposure_of(v) == MX_CODEMODE || exposure_of(v) == MX_DEFERRED)
                return 1;
    }
    return 0;
}

#define SEC_MAX 4096L
#define SEC_DESC 250L
static const char sec_intro[] =
    "MCP servers whose tools are not declared to you. Load the tools of `tool_search` servers with `tool_search`.";

/* the omission line's length: "- … N more server[s]; find their tools with searchTools()" (UTF-16 units) */
static long omit_len(long count)
{
    char num[16];
    if (count <= 0)
        return 0;
    cl_ltoa(count, num);
    return 4 + (long)strlen(num) + 12 + (count == 1 ? 0 : 1) + 37;
}

static void server_section(mcp *m)
{
    int i, j, nl = 0, kept;
    long *hl, per;
    mcp_server **listed = (mcp_server **)calloc((size_t)(m->n + 1), sizeof(*listed));
    jw_reset(&m->section);
    hl = (long *)calloc((size_t)(m->n + 1), sizeof(long));
    if (!listed || !hl) {
        free(listed);
        free(hl);
        return;
    }
    for (i = 0; i < m->n; i++) {
        mcp_server *s = &m->srv[i];
        if (!s->enabled || !indirect(s))
            continue;
        for (j = nl; j > 0 && strcmp(listed[j - 1]->name, s->name) > 0; j--)
            listed[j] = listed[j - 1];
        listed[j] = s;
        nl++;
    }
    if (nl) {
        char ns[96], sum[1200];
        long size = 0;
        for (i = 0; i < nl; i++) {
            namespace_of(listed[i]->name, ns, sizeof(ns));
            hl[i] = 2 + (long)strlen(ns) + 14;      /* "- " ns " (tool_search)" */
        }
        /* size(kept): the intro, the first kept heads and the omission line, joined by "\n" */
        for (kept = nl;; kept--) {
            size = (long)strlen(sec_intro);
            for (i = 0; i < kept; i++)
                size += 1 + hl[i];
            if (nl - kept > 0)
                size += 1 + omit_len(nl - kept);
            if (kept == 0 || size <= SEC_MAX)
                break;
        }
        per = kept == 0 ? 0 : (SEC_MAX - size) / kept - 2;
        if (per > SEC_DESC)
            per = SEC_DESC;
        jw_rawz(&m->section, sec_intro);
        for (i = 0; i < kept; i++) {
            long n, c;
            namespace_of(listed[i]->name, ns, sizeof(ns));
            jw_rawz(&m->section, "\n- ");
            jw_rawz(&m->section, ns);
            jw_rawz(&m->section, " (tool_search)");
            summary_of(listed[i], sum, sizeof(sum));
            n = (long)strlen(sum);
            if (per <= 0 || !n)
                continue;
            jw_rawz(&m->section, ": ");
            c = utf8_chars(sum, n);
            if (c <= per)
                jw_raw(&m->section, sum, n);
            else if (per > 1) {
                /* truncate: per - 1 characters, trailing white space dropped, an ellipsis */
                long k = 0, cc = 0;
                while (k < n && cc < per - 1) {
                    k++;
                    while (k < n && ((unsigned char)sum[k] & 0xc0) == 0x80)
                        k++;
                    cc++;
                }
                while (k > 0 && ws(sum[k - 1]))
                    k--;
                jw_raw(&m->section, sum, k);
                jw_rawz(&m->section, "\xe2\x80\xa6");
            } else
                m->section.n -= 2;  /* "" after the truncation: no ": " (pi: summary empty, the head alone) */
        }
        if (nl - kept > 0) {
            char num[16];
            cl_ltoa(nl - kept, num);
            jw_rawz(&m->section, "\n- \xe2\x80\xa6 ");
            jw_rawz(&m->section, num);
            jw_rawz(&m->section, nl - kept == 1 ? " more server" : " more servers");
            jw_rawz(&m->section, "; find their tools with searchTools()");
        }
        m->section.p[m->section.n] = 0;
    }
    free(listed);
    free(hl);
}

int mcp_attach(mcp *m, pi_session *s)
{
    int i, j, deferred = 0, rc = 0;
    m->s = s;
    for (i = 0; i < m->n; i++) {
        for (j = 0; j < m->srv[i].ntools; j++)
            if (m->srv[i].tools[j].exposure == MX_DEFERRED)
                deferred = 1;
        if (m->srv[i].enabled && !m->srv[i].connected &&
            (m->srv[i].exposure == MX_DEFERRED || m->srv[i].exposure == MX_CODEMODE))
            deferred = 1;
    }
    /* a direct tool the session has no room for is left out (pi has no cap; PI_TOOLS is ours); the last slot
     * is kept for tool_search, and the section follows either way */
    for (i = 0; i < m->n; i++)
        for (j = 0; j < m->srv[i].ntools; j++) {
            mcp_tool *t = &m->srv[i].tools[j];
            if (t->exposure == MX_DIRECT && s->ntools + (deferred ? 1 : 0) < PI_TOOLS && !pi_tool_add(s, &t->pt))
                t->active = 1;
        }
    server_section(m);
    if (deferred) {
        m->search.name = "tool_search";
        m->search.description = search_desc;
        m->search.parameters = search_params;
        m->search.snippet = "Search for tools that are not loaded yet and load the matches";
        m->search.u = m;
        m->search.run = search_run;
        if (pi_tool_add(s, &m->search))
            rc = -1;                /* nothing left to declare it in; the section still follows */
        else
            m->search_on = 1;
    }
    s->mcp_section = m->section.n ? m->section.p : 0;
    return rc;
}

/* the hooks to poll and wait through: the session's own once it is attached (the UI sets its abort key there
 * after mcp_init copied them), the copy before */
const llm_hooks *mcp_hooks(mcp *m)
{
    return m->s ? &m->s->h : &m->h;
}

void mcp_init(mcp *m, cl_sys *sys, llm_pool *pool, const llm_hooks *h,
              long (*getenv)(const char *name, char *out, long cap), const char *cwd, const char *tmp)
{
    memset(m, 0, sizeof(*m));
    m->sys = sys;
    m->pool = pool;
    if (h)
        m->h = *h;
    m->getenv = getenv;
    m->cwd = cwd;
    m->tmp = tmp;
    jw_init(&m->errors);
    jw_init(&m->section);
}

void mcp_free(mcp *m)
{
    int i;
    for (i = 0; i < m->n; i++) {
        disconnect(&m->srv[i]);
        free_tools(&m->srv[i]);
        free_server_config(&m->srv[i]);
        free(m->srv[i].instructions);
        jw_free(&m->srv[i].pending);
    }
    free(m->srv);
    jw_free(&m->errors);
    jw_free(&m->section);
    memset(m, 0, sizeof(*m));
}
