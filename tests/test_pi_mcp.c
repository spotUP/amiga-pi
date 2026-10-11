/* amiga-pi's MCP client (plan phase K) on the host:
 *   goldens   names, the 20 KB cut, result conversion, tool definitions and
 *             tool_search, the mcp_servers section -- each against what pi's
 *             own code gives (tests/pi/mcp/NAME.expected, tools/pi_oracle/mcp_oracle.sh);
 *   protocol  initialize, tools/list with cursors, tools/call, a JSON-RPC
 *             error, the server's roots/list, over stdio (spawn2) and
 *             streamable HTTP (JSON and SSE answers, the session id, an
 *             expired session, the LAN rule) against tools/fake_mcp.py;
 *   config    pi's validation and words, the project override, exposures;
 *   reach     pi_app -> pi_prompt -> the model calls mcp__fake__echo over
 *             HTTP: mcp_call_count moves by exactly one (K7).
 * The fake server is started through spawn2 (python3); the HTTP tests use
 * real sockets (net_posix) to 127.0.0.1. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/sys_posix.h"
#include "claude/net_posix.h"
#include "pi_stub.h"
#include "llm/llm.h"
#include "../amiga-pi/mcp.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/truncate.h"

static char root[400], tmpd[420], agent[420], logf[520], fake[600];
static sys_posix sp;
static cl_sys sys;
static net_posix np[LLM_POOL];
static cl_net nets[LLM_POOL];
static cl_net *netp[LLM_POOL];
static llm_pool rpool;          /* real sockets, for the HTTP server */
static char notice[200];
static int notices;

static void on_notice(void *u, const char *t)
{
    (void)u;
    notices++;
    cl_copy(notice, t, sizeof(notice));
}

static long env_tok(const char *name, char *o, long cap)
{
    if (!strcmp(name, "TOK")) {
        cl_copy(o, "tv", cap);
        return 2;
    }
    return -1;
}

static void new_mcp(mcp *m)
{
    llm_hooks h;
    memset(&h, 0, sizeof(h));
    h.resolve = ps_resolve;
    h.ms = ps_ms;
    h.notice = on_notice;
    mcp_init(m, &sys, &rpool, &h, env_tok, "/work/proj", tmpd);
}

/* ---- the fake server ---- */

static void reset_log(void)
{
    remove(logf);
}

static char *read_all(const char *path, long *n)
{
    char *b = 0;
    if (sys.read(sys.u, path, 8L * 1024 * 1024, &b, n))
        return 0;
    return b;
}

/* how many logged messages have method m (0: any) and contain s (0: any) */
static int log_count(const char *m, const char *s)
{
    long n = 0;
    char *b = read_all(logf, &n), *p, *e;
    int c = 0;
    char key[120];
    if (!b)
        return 0;
    key[0] = 0;
    if (m) {
        cl_copy(key, "\"method\": \"", sizeof(key));
        cl_cat(key, m, sizeof(key));
        cl_cat(key, "\"", sizeof(key));
    }
    for (p = b; *p; p = e + 1) {
        e = strchr(p, '\n');
        if (!e)
            break;
        *e = 0;
        if ((!m || strstr(p, key)) && (!s || strstr(p, s)))
            c++;
    }
    free(b);
    return c;
}

/* the n-th logged line (0-based) into out */
static void log_line(int i, char *out, long cap)
{
    long n = 0;
    char *b = read_all(logf, &n), *p, *e;
    out[0] = 0;
    if (!b)
        return;
    for (p = b; *p && i >= 0; p = e + 1, i--) {
        e = strchr(p, '\n');
        if (!e)
            break;
        if (!i) {
            *e = 0;
            cl_copy(out, p, cap);
        }
    }
    free(b);
}

/* fake_mcp.py --http 0 <extra>: its port, the child in *h */
static int start_http(const char *extra, void **h)
{
    const char *argv[12];
    char buf[64];
    long k = 0, n;
    int a = 0;
    argv[a++] = "python3";
    argv[a++] = fake;
    argv[a++] = "--http";
    argv[a++] = "0";
    argv[a++] = "--log";
    argv[a++] = logf;
    if (extra && !strcmp(extra, "sse"))
        argv[a++] = "--sse";
    else if (extra && !strcmp(extra, "expire")) {
        argv[a++] = "--expire";
        argv[a++] = "2";
    } else if (extra && !strncmp(extra, "fail", 4)) {
        /* "fail2" two 503s, "fail3" three, "fail501" one 501 */
        argv[a++] = "--fail-first";
        argv[a++] = !strcmp(extra, "fail501") ? "1" : extra + 4;
        if (!strcmp(extra, "fail501")) {
            argv[a++] = "--fail-status";
            argv[a++] = "501";
        }
    }
    argv[a] = 0;
    if (sys.spawn2(sys.u, argv, 0, 0, h))
        return -1;
    while (k < (long)sizeof(buf) - 1 && (k == 0 || buf[k - 1] != '\n')) {
        n = sys.spawn2_read(sys.u, *h, buf + k, (long)sizeof(buf) - 1 - k, 10000);
        if (n <= 0)
            return -1;
        k += n;
    }
    buf[k] = 0;
    return strncmp(buf, "port ", 5) ? -1 : atoi(buf + 5);
}

/* ---- content as text lines (the oracle's format) ---- */

static void lines_of(const char *json, long n, jw *out)
{
    jv a, b, x, d, mt;
    jit it;
    int any = 0;
    if (json_parse(json, n, &a) || json_type(a) != J_ARR)
        return;
    json_iter(a, &it);
    while (json_next(&it, 0, &b)) {
        if (any++)
            jw_rawz(out, "\n");
        if (json_get(b, "type", &x) && json_streq(x, "text") && json_get(b, "text", &x)) {
            long tn, i;
            char *t = json_strdup(x, &tn);
            jw_rawz(out, "text: ");
            for (i = 0; t && i < tn; i++) {
                if (t[i] == '\\')
                    jw_rawz(out, "\\\\");
                else if (t[i] == '\n')
                    jw_rawz(out, "\\n");
                else
                    jw_raw(out, t + i, 1);
            }
            free(t);
        } else if (json_get(b, "data", &d) && json_get(b, "mimeType", &mt)) {
            char m[64], num[16];
            json_str(mt, m, sizeof(m));
            jw_rawz(out, "image: ");
            jw_rawz(out, m);
            jw_rawz(out, " ");
            cl_ltoa(d.n - 2, num);
            jw_rawz(out, num);
        }
    }
}

/* every occurrence of what replaced by with (in place, in a jw) */
static void replace_all(jw *w, const char *what, const char *with)
{
    jw o;
    const char *p, *q;
    long wl = (long)strlen(what);
    if (!w->p || !wl)
        return;
    jw_init(&o);
    for (p = w->p; (q = strstr(p, what)) != 0; p = q + wl) {
        jw_raw(&o, p, (long)(q - p));
        jw_rawz(&o, with);
    }
    jw_rawz(&o, p);
    jw_free(w);
    *w = o;
}

/* the oracle's longText */
static void long_text(long n, jw *out)
{
    jw s;
    long i, e, k;
    char line[80];
    jw_init(&s);
    for (i = 0; s.n < n; i++) {
        line[0] = (char)('0' + i / 1000 % 10);
        line[1] = (char)('0' + i / 100 % 10);
        line[2] = (char)('0' + i / 10 % 10);
        line[3] = (char)('0' + i % 10);
        line[4] = 0;
        jw_rawz(&s, "line ");
        jw_rawz(&s, line);
        jw_rawz(&s, " of the long answer, with \xc3\xbc and \xe2\x82\xac in it\n");
    }
    e = n;
    k = e - 1;
    while (k >= 0 && ((unsigned char)s.p[k] & 0xc0) == 0x80)
        k--;
    if (k >= 0) {
        unsigned char lead = (unsigned char)s.p[k];
        int want = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
        if (e - k < want)
            e = k;
    }
    jw_raw(out, s.p, e);
    jw_free(&s);
}

/* ---- goldens ---- */

static void test_names(void)
{
    static const char *const cases[][3] = {
        {"fake", "echo", "0"},
        {"my-server", "get.file/contents", "0"},
        {"s", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "0"},
        {"github", "a_tool_with_a_name_long_enough_to_pass_the_limit_of_64", "0"},
        {"fake", "echo", "1"},
        {"dev-radius", "do-it", "0"},
        {"s", "\xc3\xbcml", "0"},
    };
    jw g;
    int i;
    char name[200];
    jw_init(&g);
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        mcp_tool_name(cases[i][0], cases[i][1], cases[i][2][0] == '1', name, sizeof(name));
        jw_rawz(&g, cases[i][0]);
        jw_rawz(&g, "|");
        jw_rawz(&g, cases[i][1]);
        jw_rawz(&g, "|");
        jw_rawz(&g, cases[i][2]);
        jw_rawz(&g, " -> ");
        jw_rawz(&g, name);
        jw_rawz(&g, "\n");
    }
    pi_golden("pi/mcp/names.expected", g.p);
    jw_free(&g);
}

static void limit_case(jw *g, const char *name, const char *blocks, long n)
{
    jw out, ls;
    char path[500];
    int cut;
    jw_init(&out);
    jw_init(&ls);
    cut = mcp_limit(&sys, tmpd, "call_1", blocks, n, &out);
    lines_of(out.p, out.n, &ls);
    pi_cut_path(tmpd, "call_1", path, sizeof(path));
    replace_all(&ls, path, "PATH");
    jw_rawz(g, "== ");
    jw_rawz(g, name);
    jw_rawz(g, cut ? " (cut 1)\n" : " (cut 0)\n");
    jw_raw(g, ls.p ? ls.p : "", ls.n);
    jw_rawz(g, "\n");
    jw_free(&out);
    jw_free(&ls);
}

static void test_limit(void)
{
    static const char img[] = "{\"type\":\"image\",\"data\":\"AAAA\",\"mimeType\":\"image/png\"}";
    static const long sizes[] = {20480, 20481, 30000};
    static const char *const names[] = {"exact 20480", "over 20481", "long 30000"};
    jw g, b, t, t2;
    int i;
    jw_init(&g);
    for (i = 0; i < 3; i++) {
        jw_init(&b);
        jw_init(&t);
        long_text(sizes[i], &t);
        jw_rawz(&b, "[");
        llm_b_text(&b, t.p, t.n, 0);
        jw_rawz(&b, "]");
        limit_case(&g, names[i], b.p, b.n);
        if (i == 2) {
            /* the whole text in the file, byte for byte; the model's part exactly 20480 bytes of it */
            char path[500], *f;
            long fn = 0;
            pi_cut_path(tmpd, "call_1", path, sizeof(path));
            f = read_all(path, &fn);
            CHECK(f && fn == t.n && !memcmp(f, t.p, (size_t)t.n));
            free(f);
        }
        jw_free(&b);
        jw_free(&t);
    }
    jw_init(&b);
    jw_init(&t);
    jw_init(&t2);
    long_text(12000, &t);
    long_text(9000, &t2);
    jw_rawz(&b, "[");
    llm_b_text(&b, t.p, t.n, 0);
    jw_rawz(&b, ",");
    jw_rawz(&b, img);
    jw_rawz(&b, ",");
    llm_b_text(&b, t2.p, t2.n, 0);
    jw_rawz(&b, "]");
    limit_case(&g, "two blocks and an image", b.p, b.n);
    jw_reset(&b);
    jw_rawz(&b, "[");
    llm_b_text(&b, "short", 5, 0);
    jw_rawz(&b, ",");
    jw_rawz(&b, img);
    jw_rawz(&b, "]");
    limit_case(&g, "short", b.p, b.n);
    pi_golden("pi/mcp/limit.expected", g.p);
    /* the cut is exact: the head and tail kept are 20480 bytes together (the 30000 case, ASCII at the cut) */
    {
        jw out, tt;
        jv a, x, bb;
        jit it;
        char *s, *mark;
        jw_init(&out);
        jw_init(&tt);
        long_text(30000, &tt);
        jw_reset(&b);
        jw_rawz(&b, "[");
        llm_b_text(&b, tt.p, tt.n, 0);
        jw_rawz(&b, "]");
        mcp_limit(&sys, tmpd, "call_1", b.p, b.n, &out);
        json_parse(out.p, out.n, &a);
        json_iter(a, &it);
        json_next(&it, 0, &bb);
        json_get(bb, "text", &x);
        s = json_strdup(x, 0);
        mark = s ? strstr(s, "\xe2\x80\xa6") : 0;
        CHECK(mark != 0);
        if (mark) {
            char *head = strstr(s, "\n\n") + 2, *tail = strstr(mark + 3, "\xe2\x80\xa6") + 3;
            char *end = strstr(tail, "\n\n[Full output: ");
            CHECK_INT((long)(mark - head) + (long)(end - tail), 20480);
        }
        free(s);
        jw_free(&out);
        jw_free(&tt);
    }
    jw_free(&b);
    jw_free(&t);
    jw_free(&t2);
    jw_free(&g);
}

static void test_convert(void)
{
    mcp m;
    char *res;
    long n = 0;
    jv all, k, v;
    jit it;
    jw g;
    char path[600];
    new_mcp(&m);
    jw_init(&g);
    cl_copy(path, PI_TESTS "/pi/mcp/results.json", sizeof(path));
    res = read_all(path, &n);
    CHECK(res != 0);
    if (res && !json_parse(res, n, &all)) {
        json_iter(all, &it);
        while (json_next(&it, &k, &v)) {
            char name[64], p[500], num[8];
            jw out, ls;
            int is_error = 0, i;
            json_str(k, name, sizeof(name));
            jw_init(&out);
            jw_init(&ls);
            mcp_result(&m, "fake", name, "call_1", v.p, v.n, &out, &is_error);
            lines_of(out.p, out.n, &ls);
            for (i = 1; i <= 4; i++) {
                char id[32];
                cl_copy(id, "call_1-", sizeof(id));
                cl_ltoa(i, num);
                cl_cat(id, num, sizeof(id));
                pi_tmp_path(tmpd, id, ".bin", p, sizeof(p));
                replace_all(&ls, p, "SAVED.bin");
            }
            jw_rawz(&g, "== ");
            jw_rawz(&g, name);
            jw_rawz(&g, is_error ? " (isError 1)\n" : " (isError 0)\n");
            jw_raw(&g, ls.p ? ls.p : "", ls.n);
            jw_rawz(&g, "\n");
            jw_free(&out);
            jw_free(&ls);
        }
    }
    pi_golden("pi/mcp/convert.expected", g.p ? g.p : "");
    /* the binary resource is in its file, decoded */
    {
        char p[500], *f;
        long fn = 0;
        int i, ok = 1;
        pi_tmp_path(tmpd, "call_1-1", ".bin", p, sizeof(p));
        f = read_all(p, &fn);
        CHECK_INT(fn, 16);
        for (i = 0; f && i < 16 && i < fn; i++)
            if (f[i] != (char)i)
                ok = 0;
        CHECK(f && ok);
        CHECK((long)strlen(p) - (long)strlen(tmpd) <= 30);
        free(f);
    }
    /* not a result: pi's words */
    {
        jw out;
        int e = 0;
        jw_init(&out);
        CHECK_INT(mcp_result(&m, "fake", "x", "c", "{\"content\":3}", 13, &out, &e), -1);
        CHECK(e && strstr(out.p, "Invalid MCP tools/call result") != 0);
        jw_free(&out);
    }
    free(res);
    jw_free(&g);
    mcp_free(&m);
}

/* JSON text without the white space between tokens (JSON.stringify of the parsed value, ASCII input) */
static void compact(const char *s, jw *out)
{
    int str = 0;
    for (; *s; s++) {
        if (str) {
            jw_raw(out, s, 1);
            if (*s == '\\' && s[1]) {
                jw_raw(out, ++s, 1);
            } else if (*s == '"')
                str = 0;
        } else if (*s == '"') {
            str = 1;
            jw_raw(out, s, 1);
        } else if (*s != ' ' && *s != '\n' && *s != '\t' && *s != '\r')
            jw_raw(out, s, 1);
    }
}

static void test_search(void)
{
    mcp m;
    pi_session s;
    llm_model mo = ps_model_openai("http://192.168.1.10:11434/v1");
    jw cfg, g;
    int i, j;
    static const char *const qs[][2] = {
        {"issue", 0}, {"create pull request", "1"}, {"file contents", 0}, {"HTTP server", 0},
        {"comment on an issue", 0}, {"the a of", 0}, {"zzz", 0}, {"issues", 0}, {"pull request draft", "2"},
        {"repository branches", 0}
    };
    new_mcp(&m);
    jw_init(&cfg);
    jw_init(&g);
    jw_rawz(&cfg, "{\"mcpServers\":{\"gh\":{\"command\":\"python3\",\"args\":[");
    jw_strz(&cfg, fake);
    jw_rawz(&cfg, ",\"--toolset\",\"github\"],\"exposure\":\"deferred\","
                  "\"description\":\"GitHub: issues, pull requests and code\"}}}");
    CHECK_INT(mcp_load_text(&m, cfg.p, cfg.n, "test", 0), 0);
    CHECK_INT(mcp_connect(&m), 1);
    CHECK_INT(m.n ? m.srv[0].ntools : 0, 10);
    for (i = 0; m.n && i < m.srv[0].ntools; i++) {
        mcp_tool *t = &m.srv[0].tools[i];
        jw_rawz(&g, "tool ");
        jw_rawz(&g, t->name);
        jw_rawz(&g, "\ndescription ");
        jw_rawz(&g, t->desc);
        jw_rawz(&g, "\nparameters ");
        compact(t->params, &g);
        jw_rawz(&g, "\n");
    }
    pi_session_init(&s, &mo, 0, &ps_pool, 0);
    CHECK_INT(mcp_attach(&m, &s), 0);
    CHECK_INT(s.ntools, 1);                     /* tool_search only: every tool is deferred */
    CHECK_STR(s.ntools ? s.tools[0]->name : "", "tool_search");
    jw_rawz(&g, "search description ");
    jw_strz(&g, m.search.description);
    jw_rawz(&g, "\n");
    for (i = 0; i < (int)(sizeof(qs) / sizeof(qs[0])); i++) {
        jw args, content, ls;
        int before = s.ntools, e = 0;
        jw_init(&args);
        jw_init(&content);
        jw_init(&ls);
        jw_rawz(&args, "{\"query\":");
        jw_strz(&args, qs[i][0]);
        if (qs[i][1]) {
            jw_rawz(&args, ",\"limit\":");
            jw_rawz(&args, qs[i][1]);
        }
        jw_rawz(&args, "}");
        m.search.run(m.search.u, &s, args.p, args.n, &content, &e);
        CHECK_INT(e, 0);
        jw_rawz(&g, "== ");
        jw_rawz(&g, qs[i][0]);
        jw_rawz(&g, " (limit ");
        jw_rawz(&g, qs[i][1] ? qs[i][1] : "-");
        jw_rawz(&g, ")\n");
        {
            jv a, b, x;
            jit it;
            char *t;
            json_parse(content.p, content.n, &a);
            json_iter(a, &it);
            json_next(&it, 0, &b);
            json_get(b, "text", &x);
            t = json_strdup(x, 0);
            jw_rawz(&g, t ? t : "");
            free(t);
        }
        jw_rawz(&g, "\nloaded ");
        for (j = before; j < s.ntools; j++) {
            if (j > before)
                jw_rawz(&g, ",");
            jw_rawz(&g, s.tools[j]->name);
        }
        jw_rawz(&g, "\n");
        jw_free(&args);
        jw_free(&content);
        jw_free(&ls);
    }
    pi_golden("pi/mcp/search.expected", g.p);
    /* pi's errors: an empty query, a limit that is not a positive integer */
    {
        jw c;
        int e = 0;
        jw_init(&c);
        m.search.run(m.search.u, &s, "{\"query\":\"  \"}", 14, &c, &e);
        CHECK(e && strstr(c.p, "query must not be empty") != 0);
        jw_reset(&c);
        m.search.run(m.search.u, &s, "{\"query\":\"x\",\"limit\":1.5}", 25, &c, &e);
        CHECK(e && strstr(c.p, "limit must be a positive integer") != 0);
        jw_free(&c);
    }
    pi_session_free(&s);
    mcp_free(&m);
    jw_free(&cfg);
    jw_free(&g);
}

static void section_case(jw *g, const char *name, const char *cfg, const char *instr_server, const char *instr)
{
    mcp m;
    pi_session s;
    llm_model mo = ps_model_openai("http://192.168.1.10:11434/v1");
    new_mcp(&m);
    pi_session_init(&s, &mo, 0, &ps_pool, 0);
    CHECK_INT(mcp_load_text(&m, cfg, (long)strlen(cfg), "test", 0), 0);
    CHECK_STR(m.errors.p ? m.errors.p : "", "");
    if (instr_server) {
        mcp_server *sv = mcp_server_named(&m, instr_server);
        if (sv)
            sv->instructions = (char *)strcpy((char *)malloc(strlen(instr) + 1), instr);
    }
    mcp_attach(&m, &s);
    jw_rawz(g, "== ");
    jw_rawz(g, name);
    jw_rawz(g, "\n");
    jw_rawz(g, s.mcp_section ? s.mcp_section : "(none)");
    jw_rawz(g, "\n");
    pi_session_free(&s);
    mcp_free(&m);
}

static void test_section(void)
{
    jw g, many;
    int i;
    jw_init(&g);
    jw_init(&many);
    section_case(&g, "none", "{\"mcpServers\":{\"a\":{\"command\":\"x\",\"exposure\":\"direct\"}}}", 0, 0);
    {
        jw c;
        jw_init(&c);
        jw_rawz(&c, "{\"mcpServers\":{\"zeta\":{\"url\":\"http://h/mcp\",\"exposure\":\"deferred\",\"description\":\"  Last "
                    "one.\\nSecond line  \"},\"alpha-one\":{\"command\":\"x\",\"exposure\":\"direct\",\"toolExposure\":{\"get_*\":"
                    "\"deferred\"}},\"off\":{\"command\":\"x\",\"exposure\":\"deferred\",\"enabled\":false},\"mid\":{\"command\":"
                    "\"x\",\"exposure\":\"deferred\"},\"long\":{\"command\":\"x\",\"exposure\":\"deferred\",\"description\":\"");
        for (i = 0; i < 10; i++)
            jw_rawz(&c, "A server whose description goes on and on. ");
        jw_rawz(&c, "\"}}}");
        section_case(&g, "three", c.p, "alpha-one", "From the instructions.\nMore.");
        jw_free(&c);
    }
    jw_rawz(&many, "{\"mcpServers\":{");
    for (i = 0; i < 90; i++) {
        char nm[80], num[8];
        cl_copy(nm, "server-with-a-rather-long-name-", sizeof(nm));
        num[0] = (char)('0' + i / 100);
        num[1] = (char)('0' + i / 10 % 10);
        num[2] = (char)('0' + i % 10);
        num[3] = 0;
        cl_cat(nm, num, sizeof(nm));
        cl_ltoa(i, num);
        if (i)
            jw_rawz(&many, ",");
        jw_strz(&many, nm);
        jw_rawz(&many, ":{\"command\":\"x\",\"exposure\":\"deferred\",\"description\":\"Server number ");
        jw_rawz(&many, num);
        jw_rawz(&many, "\"}");
    }
    jw_rawz(&many, "}}");
    section_case(&g, "many", many.p, 0, 0);
    pi_golden("pi/mcp/section.expected", g.p);
    jw_free(&g);
    jw_free(&many);
}

/* ---- config ---- */

static void cfg_error(const char *cfg, int project, const char *want)
{
    mcp m;
    new_mcp(&m);
    mcp_load_text(&m, cfg, (long)strlen(cfg), "P", project);
    if (!m.errors.p || !strstr(m.errors.p, want)) {
        printf("  config %s\n  errors %s  want %s\n", cfg, m.errors.p ? m.errors.p : "(none)\n", want);
        CHECK(0);
    } else
        CHECK(1);
    mcp_free(&m);
}

static void test_config(void)
{
    mcp m;
    mcp_server *s;
    static const char user[] =
        "{\"mcpServers\":{\"a\":{\"command\":\"x\",\"env\":{\"K\":\"v\"},\"exposure\":\"codemode-deferred\","
        "\"toolExposure\":{\"get_*\":\"direct\",\"get_one\":\"hidden\",\"*\":\"deferred\"}},"
        "\"b\":{\"url\":\"https://e.example/mcp\",\"timeout\":2.5}}}";
    static const char proj[] =
        "{\"mcpServers\":{\"a\":{\"enabled\":false,\"exposure\":\"direct\"},\"b\":{\"command\":\"y\"},"
        "\"c\":{\"enabled\":false}}}";
    cfg_error("[1]", 0, "P: expected an object with an \"mcpServers\" object");
    cfg_error("{\"mcpServers\":[]}", 0, "P: expected an object with an \"mcpServers\" object");
    cfg_error("{bad", 0, "P: not valid JSON");
    cfg_error("{\"mcpServers\":{\"a b\":{\"command\":\"x\"}}}", 0,
              "P: invalid server name \"a b\" (use letters, digits, \"_\" and \"-\")");
    cfg_error("{\"mcpServers\":{\"a\":3}}", 0, "P: server \"a\" must be an object");
    cfg_error("{\"mcpServers\":{\"a\":{\"command\":\"x\",\"exposure\":\"all\"}}}", 0,
              "P: server \"a\": exposure must be one of \"codemode\", \"deferred\", \"direct\", \"hidden\"");
    cfg_error("{\"mcpServers\":{\"a\":{\"command\":\"x\",\"toolExposure\":{\"t\":\"x\"}}}}", 0,
              "P: server \"a\": toolExposure \"t\" must be one of");
    cfg_error("{\"mcpServers\":{\"a\":{\"command\":\"x\",\"enabled\":\"no\"}}}", 0, "P: server \"a\": enabled must be a boolean");
    cfg_error("{\"mcpServers\":{\"a\":{\"command\":\"x\",\"timeout\":0}}}", 0,
              "P: server \"a\": timeout must be a positive number of seconds");
    cfg_error("{\"mcpServers\":{\"a\":{\"url\":\"http://h/sse\",\"type\":\"sse\"}}}", 0,
              "P: server \"a\": legacy SSE transport is not supported; use the streamable HTTP URL");
    cfg_error("{\"mcpServers\":{\"a\":{\"url\":\"ftp://h/x\"}}}", 0, "P: server \"a\": url must be an http or https URL");
    cfg_error("{\"mcpServers\":{\"a\":{\"url\":\"http://h/x\",\"headers\":{\"A\":1}}}}", 0,
              "P: server \"a\": headers must map names to strings");
    cfg_error("{\"mcpServers\":{\"a\":{\"command\":\"x\",\"args\":[1]}}}", 0, "P: server \"a\": args must be an array of strings");
    cfg_error("{\"mcpServers\":{\"a\":{\"command\":\"x\",\"env\":[]}}}", 0, "P: server \"a\": env must map names to strings");
    cfg_error("{\"mcpServers\":{\"a\":{\"type\":\"http\",\"command\":\"x\"}}}", 0,
              "P: server \"a\" needs either \"command\" (stdio) or \"url\" (streamable HTTP)");
    cfg_error("{\"mcpServers\":{\"a-b\":{\"command\":\"x\"},\"a_b\":{\"command\":\"y\"}}}", 0,
              "P: server \"a_b\" conflicts with \"a-b\"");
    cfg_error("{\"mcpServers\":{\"zz\":{\"enabled\":false}}}", 1,
              "P: server \"zz\" needs \"command\" or \"url\", or a global server to override");
    /* the user's file, then the project's: an override keeps env and the rest, a full entry replaces */
    new_mcp(&m);
    CHECK_INT(mcp_load_text(&m, user, (long)strlen(user), "U", 0), 0);
    s = mcp_server_named(&m, "a");
    CHECK(s && s->exposure == MX_CODEMODE && s->enabled);
    CHECK_INT(mcp_server_named(&m, "b") ? mcp_server_named(&m, "b")->timeout_ms : 0, 2500);
    CHECK_INT(mcp_load_text(&m, proj, (long)strlen(proj), "R", 1), 0);
    CHECK(s && !s->enabled && s->exposure == MX_DIRECT && s->env && strstr(s->env, "\"K\"") != 0);
    CHECK(mcp_server_named(&m, "b") && mcp_server_named(&m, "b")->kind == MK_STDIO);
    CHECK(m.errors.p && strstr(m.errors.p, "R: server \"c\" needs \"command\" or \"url\"") != 0);
    mcp_free(&m);
    /* ${VAR} and !command values */
    {
        jw w;
        new_mcp(&m);
        jw_init(&w);
        mcp_value(&m, "Bearer ${TOK}-${NOPE}!", &w);
        CHECK_STR(w.p, "Bearer tv-!");
        jw_reset(&w);
        mcp_value(&m, "!echo from-a-command", &w);
        CHECK_STR(w.p ? w.p : "", "from-a-command");
        jw_free(&w);
        mcp_free(&m);
    }
}

/* ---- the protocol over stdio ---- */

static int run_tool(mcp *m, const char *name, const char *args, jw *content)
{
    mcp_tool *t = mcp_tool_named(m, name);
    pi_session s;
    llm_model mo = ps_model_openai("http://192.168.1.10:11434/v1");
    int e = -1;
    if (!t)
        return -1;
    pi_session_init(&s, &mo, 0, &ps_pool, 0);
    s.call_id = "call_1";
    t->pt.run(t->pt.u, &s, args, (long)strlen(args), content, &e);
    pi_session_free(&s);
    return e;
}

static void stdio_config(jw *cfg, const char *extra)
{
    jw_rawz(cfg, "{\"mcpServers\":{\"fake\":{\"command\":\"python3\",\"args\":[");
    jw_strz(cfg, fake);
    jw_rawz(cfg, ",\"--log\",");
    jw_strz(cfg, logf);
    if (extra) {
        jw_rawz(cfg, ",");
        jw_rawz(cfg, extra);
    }
    jw_rawz(cfg, "]}}}");
}

static void test_stdio(void)
{
    mcp m;
    jw cfg, c;
    long calls;
    char line[2000];
    new_mcp(&m);
    jw_init(&cfg);
    jw_init(&c);
    reset_log();
    stdio_config(&cfg, "\"--pages\"");
    CHECK_INT(mcp_load_text(&m, cfg.p, cfg.n, "test", 0), 0);
    CHECK_INT(mcp_connect(&m), 1);
    CHECK_INT(m.n ? m.srv[0].ntools : 0, 11);           /* two pages */
    CHECK_STR(m.n ? m.srv[0].version : "", "2025-11-25");
    CHECK_STR(m.n && m.srv[0].instructions ? m.srv[0].instructions : "", "Fake server for amiga-pi tests.\nSecond line.");
    /* initialize as pi sends it, then notifications/initialized, then the pages */
    log_line(0, line, sizeof(line));
    CHECK(strstr(line, "\"method\": \"initialize\"") && strstr(line, "\"protocolVersion\": \"2025-11-25\"") &&
          strstr(line, "\"capabilities\": {\"roots\": {}}") && strstr(line, "\"clientInfo\": {\"name\": \"amiga-pi\""));
    log_line(1, line, sizeof(line));
    CHECK(strstr(line, "notifications/initialized") && !strstr(line, "\"id\""));
    CHECK_INT(log_count("tools/list", 0), 2);
    CHECK_INT(log_count("tools/list", "\"cursor\": \"page2\""), 1);
    /* a call, its answer (a notification came before it) */
    calls = mcp_call_count;
    CHECK_INT(run_tool(&m, "mcp__fake__echo", "{\"text\":\"hi\"}", &c), 0);
    CHECK(strstr(c.p, "\"text\":\"echo: hi\"") != 0);
    CHECK_INT(mcp_call_count - calls, 1);
    /* a JSON-RPC error: its message, an error result */
    jw_reset(&c);
    CHECK_INT(run_tool(&m, "mcp__fake__boom", "{}", &c), 1);
    CHECK(strstr(c.p, "\"text\":\"boom happened\"") != 0);
    /* isError from the server */
    jw_reset(&c);
    CHECK_INT(run_tool(&m, "mcp__fake__fail", "{}", &c), 1);
    CHECK(strstr(c.p, "it failed") != 0);
    /* the server asks roots/list in the middle of a call: answered at once */
    jw_reset(&c);
    CHECK_INT(run_tool(&m, "mcp__fake__ask_roots", "{}", &c), 0);
    CHECK(strstr(c.p, "file:///work/proj") && strstr(c.p, "\\\"name\\\": \\\"proj\\\""));
    CHECK_INT(log_count(0, "\"roots\": [{\"uri\": \"file:///work/proj\", \"name\": \"proj\"}]"), 1);
    /* arguments the schema refuses: no request */
    jw_reset(&c);
    calls = mcp_call_count;
    CHECK_INT(run_tool(&m, "mcp__fake__echo", "{\"text\":5}", &c), 1);
    CHECK(strstr(c.p, "Validation failed for tool \\\"mcp__fake__echo\\\"") != 0);
    CHECK_INT(mcp_call_count - calls, 0);
    /* the server goes away: the call fails, the next one starts it again (tools kept) */
    {
        mcp_tool *t = mcp_tool_named(&m, "mcp__fake__echo");
        m.srv[0].tr.close(&m.srv[0]);
        m.srv[0].tr.send = 0;
        m.srv[0].connected = 0;
        jw_reset(&c);
        CHECK_INT(run_tool(&m, "mcp__fake__echo", "{\"text\":\"again\"}", &c), 0);
        CHECK(strstr(c.p, "echo: again") != 0);
        CHECK(t == mcp_tool_named(&m, "mcp__fake__echo"));
        CHECK_INT(log_count("initialize", 0), 2);
    }
    mcp_free(&m);
    /* a command that is not there: the error, no tools */
    new_mcp(&m);
    jw_reset(&cfg);
    jw_rawz(&cfg, "{\"mcpServers\":{\"gone\":{\"command\":\"/nonexistent/server\"}}}");
    mcp_load_text(&m, cfg.p, cfg.n, "test", 0);
    CHECK_INT(mcp_connect(&m), 0);
    CHECK(m.errors.p && strstr(m.errors.p, "MCP server \"gone\" failed: MCP server closed the connection") != 0);
    mcp_free(&m);
    jw_free(&cfg);
    jw_free(&c);
}

/* ---- streamable HTTP ---- */

static void http_config(jw *cfg, const char *host, int port, const char *more)
{
    char num[16];
    cl_ltoa(port, num);
    jw_rawz(cfg, "{\"mcpServers\":{\"fake\":{\"url\":\"http://");
    jw_rawz(cfg, host);
    jw_rawz(cfg, ":");
    jw_rawz(cfg, num);
    jw_rawz(cfg, "/mcp\"");
    if (more)
        jw_rawz(cfg, more);
    jw_rawz(cfg, "}}}");
}

static void test_http(void)
{
    mcp m;
    jw cfg, c, big_http, big_stdio;
    void *h = 0;
    int port, i;
    char line[3000];
    jw_init(&cfg);
    jw_init(&c);
    jw_init(&big_http);
    jw_init(&big_stdio);
    for (i = 0; i < 2; i++) {
        /* JSON answers, then SSE answers */
        reset_log();
        port = start_http(i ? "sse" : 0, &h);
        CHECK(port > 0);
        new_mcp(&m);
        jw_reset(&cfg);
        http_config(&cfg, "127.0.0.1", port, ",\"headers\":{\"X-Token\":\"${TOK}\",\"Authorization\":\"Bearer secret\"}");
        notices = 0;
        CHECK_INT(mcp_load_text(&m, cfg.p, cfg.n, "test", 0), 0);
        CHECK_INT(mcp_connect(&m), 1);
        CHECK_INT(m.n ? m.srv[0].ntools : 0, 11);
        jw_reset(&c);
        CHECK_INT(run_tool(&m, "mcp__fake__echo", "{\"text\":\"over http\"}", &c), 0);
        CHECK(strstr(c.p, "echo: over http") != 0);
        jw_reset(&c);
        CHECK_INT(run_tool(&m, "mcp__fake__boom", "{}", &c), 1);
        CHECK(strstr(c.p, "boom happened") != 0);
        /* initialize carries no session; everything after it the session and the protocol version */
        log_line(0, line, sizeof(line));
        CHECK(strstr(line, "\"method\": \"initialize\"") && !strstr(line, "mcp-session-id"));
        CHECK(strstr(line, "\"accept\": \"application/json, text/event-stream\"") != 0);
        log_line(1, line, sizeof(line));
        CHECK(strstr(line, "notifications/initialized") && strstr(line, "\"mcp-session-id\": \"sess-1\"") &&
              strstr(line, "\"mcp-protocol-version\": \"2025-11-25\""));
        CHECK_INT(log_count("tools/call", "\"mcp-session-id\": \"sess-1\""), 2);
        /* ${TOK} from the environment; the credential not sent over plain http, said once */
        CHECK_INT(log_count(0, "\"x-token\": \"tv\""), log_count(0, 0));
        CHECK_INT(log_count(0, "authorization"), 0);
        CHECK_INT(notices, 1);
        CHECK_STR(notice, "credential for MCP server fake not sent: plain http");
        if (i) {
            /* the 20 KB cut on a long result: the same bytes over HTTP (SSE) as over stdio */
            CHECK_INT(run_tool(&m, "mcp__fake__big", "{\"n\":30000}", &big_http), 0);
        }
        mcp_free(&m);
        llm_pool_close(&rpool);
        sys.spawn2_close(sys.u, h, 3000);
    }
    {
        mcp ms;
        jw sc;
        new_mcp(&ms);
        jw_init(&sc);
        stdio_config(&sc, 0);
        mcp_load_text(&ms, sc.p, sc.n, "test", 0);
        CHECK_INT(mcp_connect(&ms), 1);
        CHECK_INT(run_tool(&ms, "mcp__fake__big", "{\"n\":30000}", &big_stdio), 0);
        CHECK(big_http.n > 20000 && strstr(big_http.p, "Warning: truncated output") != 0);
        CHECK_INT(big_http.n, big_stdio.n);
        CHECK(big_http.p && big_stdio.p && !strcmp(big_http.p, big_stdio.p));
        mcp_free(&ms);
        jw_free(&sc);
    }
    /* an expired session (404): initialized again, the call goes through on the new session */
    reset_log();
    port = start_http("expire", &h);
    new_mcp(&m);
    jw_reset(&cfg);
    http_config(&cfg, "127.0.0.1", port, 0);
    mcp_load_text(&m, cfg.p, cfg.n, "test", 0);
    CHECK_INT(mcp_connect(&m), 1);
    jw_reset(&c);
    CHECK_INT(run_tool(&m, "mcp__fake__echo", "{\"text\":\"after expiry\"}", &c), 0);
    CHECK(strstr(c.p, "echo: after expiry") != 0);
    CHECK_INT(log_count("initialize", 0), 2);
    CHECK_INT(log_count("tools/call", "\"mcp-session-id\": \"sess-2\""), 1);
    mcp_free(&m);
    llm_pool_close(&rpool);
    sys.spawn2_close(sys.u, h, 3000);
    /* pi's connect retries: two transient failures (503) and the third attempt connects, after 250 + 1000 ms;
     * three fail; a 501 is not tried again */
    {
        static const char *const modes[] = {"fail2", "fail3", "fail501"};
        static const int want_ok[] = {1, 0, 0}, want_attempts[] = {3, 3, 1};
        static const long want_slept[] = {1250, 1250, 0};
        int k;
        for (k = 0; k < 3; k++) {
            long slept0;
            reset_log();
            port = start_http(modes[k], &h);
            new_mcp(&m);
            m.h.sleep = ps_sleep;
            jw_reset(&cfg);
            http_config(&cfg, "127.0.0.1", port, 0);
            mcp_load_text(&m, cfg.p, cfg.n, "test", 0);
            slept0 = ps.slept;
            CHECK_INT(mcp_connect(&m), want_ok[k]);
            CHECK_INT(m.srv[0].attempts, want_attempts[k]);
            CHECK_INT(ps.slept - slept0, want_slept[k]);
            mcp_free(&m);
            llm_pool_close(&rpool);
            sys.spawn2_close(sys.u, h, 3000);
        }
    }
    /* the LAN rule: a public address over plain http is refused before connecting */
    {
        long opens = rpool.opens;
        new_mcp(&m);
        jw_reset(&cfg);
        http_config(&cfg, "public.example", 80, 0);
        mcp_load_text(&m, cfg.p, cfg.n, "test", 0);
        CHECK_INT(mcp_connect(&m), 0);
        CHECK(m.errors.p && strstr(m.errors.p, "plain http is only for the LAN (public.example is not); use https"));
        CHECK_INT(rpool.opens, opens);
        mcp_free(&m);
    }
    jw_free(&cfg);
    jw_free(&c);
    jw_free(&big_http);
    jw_free(&big_stdio);
}

/* ---- K7: reachability through pi_app ---- */

static jw out;
static long l_read(void *u, char *buf, long cap)
{
    (void)u;
    (void)buf;
    (void)cap;
    return -1;
}
static void l_write(void *u, const char *s, long n)
{
    (void)u;
    jw_raw(&out, s, n);
}
static int l_brk(void *u)
{
    (void)u;
    return 0;
}
static llm_ts adv_now(void *u)
{
    static long c;
    llm_ts t;
    (void)u;
    t.sec = 1760100000L + c++;
    t.ms = 1;
    return t;
}
static long no_env(const char *name, char *o, long cap)
{
    (void)name;
    (void)o;
    (void)cap;
    return -1;
}

static const char sse_call[] =
    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"created\":1760100000,\"model\":\"m\",\"choices\":[{\"index\":0,"
    "\"delta\":{\"role\":\"assistant\",\"tool_calls\":[{\"index\":0,\"id\":\"call_mcp1\",\"type\":\"function\",\"function\":"
    "{\"name\":\"mcp__fake__echo\",\"arguments\":\"{\\\"text\\\":\\\"from the model\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"created\":1760100000,\"model\":\"m\",\"choices\":[{\"index\":0,"
    "\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";
static const char sse_final[] =
    "data: {\"id\":\"c2\",\"object\":\"chat.completion.chunk\",\"created\":1760100000,\"model\":\"m\",\"choices\":[{\"index\":0,"
    "\"delta\":{\"role\":\"assistant\",\"content\":\"The server said it back.\"},\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"c2\",\"object\":\"chat.completion.chunk\",\"created\":1760100000,\"model\":\"m\",\"choices\":[{\"index\":0,"
    "\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

static int app_run(int no_mcp)
{
    cl_io io;
    pi_platform pl;
    char *argv[10];
    int a = 0;
    memset(&io, 0, sizeof(io));
    io.read_line = l_read;
    io.write = l_write;
    io.brk = l_brk;
    jw_free(&out);
    jw_init(&out);
    ps_reset();
    ps_pool_up(LLM_POOL);
    ps_sse_text(sse_call, (long)sizeof(sse_call) - 1, 0);
    ps_sse_text(sse_final, (long)sizeof(sse_final) - 1, 0);
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.mcp_pool = &rpool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = tmpd;
    pl.utc = 1;
    pl.agent_dir = agent;
    argv[a++] = "amiga-pi";
    argv[a++] = "-p";
    argv[a++] = "--no-session";
    if (no_mcp)
        argv[a++] = "--no-mcp";
    argv[a++] = "--model";
    argv[a++] = "fixture/qwen2.5-coder:7b";
    argv[a++] = "--base-url";
    argv[a++] = "http://192.168.1.10:11434/v1";
    argv[a++] = "say it back";
    argv[a] = 0;
    return pi_app(a, argv, &pl);
}

static void test_reach(void)
{
    void *h = 0;
    int port;
    long calls;
    jw cfg;
    char path[500];
    reset_log();
    port = start_http(0, &h);
    CHECK(port > 0);
    jw_init(&cfg);
    http_config(&cfg, "127.0.0.1", port, 0);
    cl_copy(path, agent, sizeof(path));
    cl_cat(path, "/mcp.json", sizeof(path));
    CHECK_INT(sys.write(sys.u, path, cfg.p, cfg.n), 0);
    calls = mcp_call_count;
    CHECK_INT(app_run(0), 0);
    /* the sentinel: exactly one tools/call, from the model's tool call through pi_prompt */
    CHECK_INT(mcp_call_count - calls, 1);
    CHECK_INT(log_count("tools/call", "\"name\": \"mcp__fake__echo\""), 0);    /* the server's own name goes out */
    CHECK_INT(log_count("tools/call", "\"arguments\": {\"text\": \"from the model\"}"), 1);
    CHECK_INT(ps.nreq, 2);
    CHECK(ps.nreq >= 1 && strstr(ps.body[0], "\"name\":\"mcp__fake__echo\"") != 0);     /* declared (direct) */
    CHECK(ps.nreq >= 2 && strstr(ps.body[1], "echo: from the model") != 0);            /* its result went back */
    CHECK(out.p && strstr(out.p, "The server said it back.") != 0);
    /* --no-mcp: the knob moves the output -- the tool is not declared, nothing is called */
    calls = mcp_call_count;
    app_run(1);
    CHECK_INT(mcp_call_count - calls, 0);
    CHECK(ps.nreq >= 1 && strstr(ps.body[0], "mcp__fake__echo") == 0);
    remove(path);
    llm_pool_close(&rpool);
    sys.spawn2_close(sys.u, h, 3000);
    jw_free(&cfg);
    jw_free(&out);
}

void suite_pi_mcp(void)
{
    const char *base = getenv("TMPDIR");
    int i;
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_mcp_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    strcpy(tmpd, root);
    strcat(tmpd, "/");
    strcpy(agent, root);
    strcat(agent, "/agent");
    mkdir(agent, 0777);
    strcpy(logf, root);
    strcat(logf, "/fake.log");
    strcpy(fake, PI_TESTS "/../tools/fake_mcp.py");
    sys_posix_init(&sp, &sys);
    for (i = 0; i < LLM_POOL; i++) {
        net_posix_init(&np[i], &nets[i]);
        netp[i] = &nets[i];
    }
    llm_pool_init(&rpool, netp, LLM_POOL);
    jw_init(&out);
    test_names();
    test_limit();
    test_convert();
    test_section();
    test_config();
    test_search();
    test_stdio();
    test_http();
    test_reach();
}
