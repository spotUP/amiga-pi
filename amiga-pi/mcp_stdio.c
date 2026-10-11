/* mcp_stdio -- MCP's stdio transport (pi f10993bc
 * packages/mcp/src/transports/stdio.ts) over cl_sys spawn2: the server is
 * `command` with `args` (no shell), `env` added to the environment
 * (${VAR}, !command), `cwd` relative to the session's directory; one JSON
 * message per line each way; its standard error is not kept (pi keeps a
 * tail for /mcp; COMPATIBILITY.md). Closing: its input closed, 2 s to end,
 * then stopped (pi: SIGTERM, then SIGKILL; on the Amiga the PTY: pipes
 * close, ARCHITECTURE.md K1). A request waits up to the server's timeout
 * for a byte, polling the break every 250 ms. Portable C89. */
#include <stdlib.h>
#include <string.h>
#include "mcp.h"
#include "claude/util.h"

typedef struct ms {
    void *h;                    /* spawn2's child */
    jw in;                      /* bytes read, not yet a whole line */
} ms;

static int s_send(mcp_server *s, const char *msg, long n, int request, mcp_msg_fn on, void *ou)
{
    ms *t = (ms *)s->tu;
    cl_sys *sys = s->m->sys;
    long idle = 0, limit = s->timeout_ms > 0 ? s->timeout_ms : MCP_TIMEOUT_MS;
    static char buf[4096];
    if (sys->spawn2_write(sys->u, t->h, msg, n) || sys->spawn2_write(sys->u, t->h, "\n", 1)) {
        cl_copy(s->err, "MCP server closed its input", sizeof(s->err));
        return -1;
    }
    if (!request)
        return 0;
    for (;;) {
        long i, st = 0, k;
        /* whole lines kept from before (an earlier read may have brought more than one answer) */
        for (i = 0; i < t->in.n; i++)
            if (t->in.p[i] == '\n') {
                long e = i;
                int got;
                if (e > st && t->in.p[e - 1] == '\r')
                    e--;
                got = e > st && on(ou, t->in.p + st, e - st);
                st = i + 1;
                if (got) {
                    memmove(t->in.p, t->in.p + st, (size_t)(t->in.n - st));
                    t->in.n -= st;
                    t->in.p[t->in.n] = 0;
                    return 0;
                }
            }
        if (st) {
            memmove(t->in.p, t->in.p + st, (size_t)(t->in.n - st));
            t->in.n -= st;
            t->in.p[t->in.n] = 0;
        }
        if (s->m->h.poll && s->m->h.poll(s->m->h.u)) {
            cl_copy(s->err, "Operation aborted", sizeof(s->err));
            return -1;
        }
        k = sys->spawn2_read(sys->u, t->h, buf, sizeof(buf), 250);
        if (k == -2) {
            idle += 250;
            if (idle >= limit) {
                char num[24];
                cl_ltoa(limit, num);
                cl_copy(s->err, "MCP request timed out after ", sizeof(s->err));
                cl_cat(s->err, num, sizeof(s->err));
                cl_cat(s->err, "ms", sizeof(s->err));
                return -1;
            }
            continue;
        }
        if (k <= 0) {
            cl_copy(s->err, "MCP server closed the connection", sizeof(s->err));
            return -1;
        }
        idle = 0;
        jw_raw(&t->in, buf, k);
    }
}

static void s_close(mcp_server *s)
{
    ms *t = (ms *)s->tu;
    if (!t)
        return;
    if (t->h)
        s->m->sys->spawn2_close(s->m->sys->u, t->h, 2000);
    jw_free(&t->in);
    free(t);
    s->tu = 0;
}

/* a relative cwd under the session's directory ("Work:proj" + "sub" -> "Work:proj/sub") */
static void join_dir(const char *base, const char *rel, char *out, long cap)
{
    long n;
    if (!base || !*base || rel[0] == '/' || strchr(rel, ':')) {
        cl_copy(out, rel, cap);
        return;
    }
    cl_copy(out, base, cap);
    n = (long)strlen(out);
    if (n && out[n - 1] != '/' && out[n - 1] != ':')
        cl_cat(out, "/", cap);
    cl_cat(out, rel, cap);
}

int mcp_stdio_open(mcp_server *s)
{
    mcp *m = s->m;
    ms *t;
    const char **argv = 0, **env = 0;
    jw strs;                    /* argv and env strings, NUL-separated */
    long *offs = 0, *eoffs = 0, na = 0, ne = 0, i;
    char dir[512];
    int rc;
    jv v, x;
    jit it;
    if (!m->sys || !m->sys->spawn2) {
        cl_copy(s->err, "stdio servers cannot be started here (no spawn2)", sizeof(s->err));
        return -1;
    }
    jw_init(&strs);
    if (s->args && !json_parse(s->args, (long)strlen(s->args), &v))
        na = json_count(v);
    if (s->env && !json_parse(s->env, (long)strlen(s->env), &v))
        ne = json_count(v);
    argv = (const char **)calloc((size_t)(na + 2), sizeof(char *));
    env = (const char **)calloc((size_t)(ne + 1), sizeof(char *));
    offs = (long *)calloc((size_t)(na + 2), sizeof(long));
    eoffs = (long *)calloc((size_t)(ne + 1), sizeof(long));
    t = (ms *)calloc(1, sizeof(*t));
    if (!argv || !env || !offs || !eoffs || !t) {
        cl_copy(s->err, "out of memory", sizeof(s->err));
        rc = -1;
        goto done;
    }
    offs[0] = 0;
    jw_rawz(&strs, s->command);
    jw_raw(&strs, "\0", 1);
    na = 0;
    if (s->args && !json_parse(s->args, (long)strlen(s->args), &v)) {
        json_iter(v, &it);
        while (json_next(&it, 0, &x)) {
            char *a = json_strdup(x, 0);
            offs[++na] = strs.n;
            jw_rawz(&strs, a ? a : "");
            jw_raw(&strs, "\0", 1);
            free(a);
        }
    }
    ne = 0;
    if (s->env && !json_parse(s->env, (long)strlen(s->env), &v)) {
        jv k;
        json_iter(v, &it);
        while (json_next(&it, &k, &x)) {
            char *name = json_strdup(k, 0), *val = json_strdup(x, 0);
            eoffs[ne++] = strs.n;
            jw_rawz(&strs, name ? name : "");
            jw_rawz(&strs, "=");
            mcp_value(m, val ? val : "", &strs);
            jw_raw(&strs, "\0", 1);
            free(name);
            free(val);
        }
    }
    if (strs.oom) {
        cl_copy(s->err, "out of memory", sizeof(s->err));
        rc = -1;
        goto done;
    }
    for (i = 0; i <= na; i++)
        argv[i] = strs.p + offs[i];
    argv[na + 1] = 0;
    for (i = 0; i < ne; i++)
        env[i] = strs.p + eoffs[i];
    env[ne] = 0;
    if (s->cwd)
        join_dir(m->cwd, s->cwd, dir, sizeof(dir));
    jw_init(&t->in);
    rc = m->sys->spawn2(m->sys->u, argv, ne ? env : 0, s->cwd ? dir : 0, &t->h);
    if (rc) {
        cl_copy(s->err, "cannot start ", sizeof(s->err));
        cl_cat(s->err, s->command, sizeof(s->err));
        cl_cat(s->err, ": ", sizeof(s->err));
        cl_cat(s->err, m->sys->err(m->sys->u), sizeof(s->err));
    } else {
        s->tu = t;
        s->tr.u = t;
        s->tr.inline_reply = 1;
        s->tr.send = s_send;
        s->tr.close = s_close;
        t = 0;
    }
done:
    free(argv);
    free(env);
    free(offs);
    free(eoffs);
    if (t) {
        jw_free(&t->in);
        free(t);
    }
    jw_free(&strs);
    return rc;
}
