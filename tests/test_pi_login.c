/* amiga-pi's subscription logins (plan phase O: O2 /login, O3 refresh, O4 the flows against a fake OAuth server, O5
 * reachability).
 *
 * The flows (oauth.c) run against tools/claude_fixture.py --oauth, which plays the providers' OAuth servers from a
 * script, over real sockets to 127.0.0.1 (net_posix). Every scenario of tests/pi/oauth/scenarios.json is also run by
 * pi 1.1.0's own code (tools/pi_oracle/gen_oauth.mjs, on the pinned install, with a capturing fetch); its requests
 * (<name>.req) and what the user saw and the end (<name>.out) are what this side must produce, byte for byte: the
 * request lines, the headers pi set, the bodies; the credential JSON; pi's error texts. Nothing here updates an
 * expectation.
 *
 * The rest goes through pi_app, the top-level entry, over the stub transport: an expired credential is renewed once
 * before the request (sentinel pi_auth_refreshes) and the request carries the new token; a 401 renews it once and the
 * request goes again once; /login and /logout typed at the prompt; each provider's request (the Bearer form, the base
 * URL a Copilot token brings, Codex's account header). No token byte reaches the console, the debug log or the session
 * file; no credential goes over plain http. */
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
#include "../amiga-pi/app.h"
#include "../amiga-pi/login.h"
#include "../amiga-pi/authstore.h"
#include "../amiga-pi/oauth.h"
#include "llm/creds.h"

static char root[400], agent[460], data[460], odir[460], script_path[520], reqlog[520];
static sys_posix sp;
static cl_sys sys;

/* ---- files ---- */

static char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long len;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)len + 1);
    len = b ? (long)fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    if (b)
        b[len] = 0;
    if (n)
        *n = len;
    return b;
}

static void put(const char *path, const char *text, long n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(text, 1, (size_t)n, f);
        fclose(f);
    }
}

static char *golden(const char *name)
{
    char path[600];
    strcpy(path, PI_TESTS "/pi/oauth/");
    strcat(path, name);
    return slurp(path, 0);
}

/* ---- the fake OAuth server ---- */

static void *fx_h;
static int fx_port;

static int fixture_start(void)
{
    const char *argv[12];
    char buf[160];
    long k = 0, n;
    int a = 0;
    argv[a++] = "python3";
    argv[a++] = "-I";
    argv[a++] = "tools/claude_fixture.py";
    argv[a++] = "--port";
    argv[a++] = "0";
    argv[a++] = "--delay";
    argv[a++] = "0";
    argv[a++] = "--oauth";
    argv[a++] = odir;
    argv[a] = 0;
    if (sys.spawn2(sys.u, argv, 0, 0, &fx_h))
        return -1;
    while (k < (long)sizeof(buf) - 1 && (k == 0 || buf[k - 1] != '\n')) {
        n = sys.spawn2_read(sys.u, fx_h, buf + k, (long)sizeof(buf) - 1 - k, 10000);
        if (n <= 0)
            return -1;
        k += n;
    }
    buf[k] = 0;
    {
        const char *p = strstr(buf, "on port ");
        return p ? atoi(p + 8) : -1;
    }
}

/* ---- the world the flows run in: a clock that moves when they sleep, a random generator that counts ---- */

#define NOW_SEC 1760100000L
static long clock_ms;
static long rk;
static net_posix np[LLM_POOL];
static cl_net nets[LLM_POOL];
static cl_net *netp[LLM_POOL];
static llm_pool pool;
static pi_models models;
static pi_session sess;
static pi_auth auth;
static char url_base[64];
static const char *env_kimi_host;       /* KIMI_OAUTH_HOST, for the plain-http test */
static char env_cb_host[80];            /* PI_OAUTH_CALLBACK_HOST of a scenario ("callbackHost") */
#define LOCAL_COUNT 14

static llm_ts t_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = NOW_SEC + clock_ms / 1000;
    t.ms = (int)(clock_ms % 1000);
    return t;
}

static int t_sleep(void *u, long ms)
{
    (void)u;
    clock_ms += ms;
    return 0;
}

static int t_poll(void *u)
{
    (void)u;
    return 0;
}

static int fixed_random;                /* every byte 'A': the state a test types back is then known */

static int t_random(void *u, unsigned char *b, long n)
{
    long i;
    (void)u;
    for (i = 0; i < n; i++)
        b[i] = fixed_random ? 'A' : (unsigned char)((rk++ * 37 + 11) & 255);
    return 0;
}

static long t_getenv(const char *name, char *out, long cap)
{
    if (env_cb_host[0] && !strcmp(name, "PI_OAUTH_CALLBACK_HOST")) {
        cl_copy(out, env_cb_host, cap);
        return (long)strlen(out);
    }
    if (env_kimi_host && !strcmp(name, "KIMI_OAUTH_HOST")) {
        cl_copy(out, env_kimi_host, cap);
        return (long)strlen(out);
    }
    return -1;
}

static jw ev;                           /* what the user was shown and asked, one line each */
static int sel_n;
static int sels[8];
static int txt_n;
static const char *txts[8];
static char paste[1400];
static char auth_url[1600];

static void line(const char *a, const char *b, const char *c)
{
    jw_rawz(&ev, a);
    if (b)
        jw_rawz(&ev, b);
    if (c)
        jw_rawz(&ev, c);
    jw_rawz(&ev, "\n");
}

static void t_info(void *u, int kind, const char *a, const char *b)
{
    (void)u;
    switch (kind) {
    case OA_I_AUTH_URL:
        cl_copy(auth_url, a, sizeof(auth_url));
        jw_rawz(&ev, "auth_url ");
        jw_rawz(&ev, a);
        jw_rawz(&ev, " | ");
        jw_rawz(&ev, b ? b : "");
        jw_rawz(&ev, "\n");
        break;
    case OA_I_DEVICE_CODE:
        jw_rawz(&ev, "device_code ");
        jw_rawz(&ev, a);
        jw_rawz(&ev, " ");
        jw_rawz(&ev, b);
        jw_rawz(&ev, "\n");
        break;
    case OA_I_INFO:
        line("info ", a, 0);
        break;
    default:
        line("progress ", a, 0);
    }
}

static int sel_i, txt_i;

static int t_prompt(void *u, int kind, const char *msg, const char *ph, char *out, long cap)
{
    (void)u;
    jw_rawz(&ev, kind == OA_P_MANUAL ? "prompt manual " : "prompt text ");
    jw_rawz(&ev, msg);
    jw_rawz(&ev, " | ");
    jw_rawz(&ev, ph ? ph : "");
    jw_rawz(&ev, "\n");
    if (kind == OA_P_MANUAL) {
        /* the pasted redirect with $STATE replaced by the state parameter of the URL shown */
        char state[200], *p = strstr(auth_url, "state="), *q = paste, *r;
        long n = 0;
        state[0] = 0;
        if (p) {
            p += 6;
            while (p[n] && p[n] != '&' && n < (long)sizeof(state) - 1) {
                state[n] = p[n];
                n++;
            }
            state[n] = 0;
        }
        out[0] = 0;
        while ((r = strstr(q, "$STATE")) != 0) {
            strncat(out, q, (size_t)(r - q));
            strcat(out, state);
            q = r + 6;
        }
        strcat(out, q);
        return 0;
    }
    cl_copy(out, txt_i < txt_n ? txts[txt_i++] : "", cap);
    return 0;
}

static int t_select(void *u, const char *msg, const char *const *labels, int n)
{
    int i;
    (void)u;
    jw_rawz(&ev, "prompt select ");
    jw_rawz(&ev, msg);
    jw_rawz(&ev, " [");
    for (i = 0; i < n; i++) {
        if (i)
            jw_rawz(&ev, " | ");
        jw_rawz(&ev, labels[i]);
    }
    jw_rawz(&ev, "]\n");
    return sel_i < sel_n ? sels[sel_i++] : -1;
}

static void world_up(void)
{
    int i;
    for (i = 0; i < LLM_POOL; i++) {
        net_posix_init(&np[i], &nets[i]);
        netp[i] = &nets[i];
    }
    llm_pool_init(&pool, netp, 2);
    {
        llm_model m = ps_model_openai("http://x.example/v1");
        llm_hooks h;
        memset(&h, 0, sizeof(h));
        h.poll = t_poll;
        h.sleep = t_sleep;
        pi_session_init(&sess, &m, 0, &pool, &h);
    }
    sess.now = t_now;
    pi_models_init(&models, &sys, agent, t_getenv);
    memset(&auth, 0, sizeof(auth));
    auth.random = t_random;
    sprintf(url_base, "http://127.0.0.1:%d", fx_port);
    auth.url_base = url_base;
    pi_auth_init(&auth, &models, &sess, &sys, agent, t_getenv);
}

static void world_down(void)
{
    llm_pool_close(&pool);
    pi_models_free(&models);
    pi_session_free(&sess);
}

/* ---- the scenarios against pi's own run of them ---- */

static void cmp_text(const char *what, const char *name, const char *want, const char *got)
{
    h_checks++;
    if (want && !strcmp(want, got))
        return;
    h_failures++;
    printf("  FAIL %s of %s differs:\n--- pi\n%s\n--- here\n%s\n", what, name, want ? want : "(missing)", got);
}

/* "normalizePort": the digits of a 127.0.0.1 port are replaced by PORT (pi listens on a port of its own, this build on none) */
static void norm_port(jw *w)
{
    jw out;
    const char *p = w->p, *end = w->p + w->n;
    jw_init(&out);
    while (p < end) {
        if (end - p > 10 && !strncmp(p, "127.0.0.1", 9) && (p[9] == ':' || !strncmp(p + 9, "%3A", 3))) {
            int k = p[9] == ':' ? 1 : 3;
            const char *q = p + 9 + k;
            if (*q >= '0' && *q <= '9') {
                jw_raw(&out, p, 9 + k);
                while (*q >= '0' && *q <= '9')
                    q++;
                jw_rawz(&out, "PORT");
                p = q;
                continue;
            }
        }
        jw_raw(&out, p, 1);
        p++;
    }
    jw_free(w);
    *w = out;
}

static void run_scenario(jv sc)
{
    char name[96], prov[40], op[16], path[600], err[600];
    jv v, rs, cred;
    jw script, end;
    const oa_provider *p;
    oa_io io;
    jit it;
    jv e;
    char devid[64];
    int rc;
    char *want_out, *want_req, *got_req;
    json_get(sc, "name", &v);
    json_str(v, name, sizeof(name));
    json_get(sc, "provider", &v);
    json_str(v, prov, sizeof(prov));
    json_get(sc, "op", &v);
    json_str(v, op, sizeof(op));
    jw_init(&script);
    jw_rawz(&script, "{\"responses\":");
    json_get(sc, "responses", &rs);
    jw_raw(&script, rs.p, rs.n);
    jw_rawz(&script, "}");
    put(script_path, script.p, script.n);
    jw_free(&script);
    remove(reqlog);
    clock_ms = 0;
    rk = 0;
    jw_reset(&ev);
    sel_n = sel_i = txt_n = txt_i = 0;
    paste[0] = 0;
    auth_url[0] = 0;
    devid[0] = 0;
    if (json_get(sc, "select", &v)) {
        json_iter(v, &it);
        while (json_next(&it, 0, &e) && sel_n < 8)
            sels[sel_n++] = (int)json_long(e, 0);
    }
    {
        static char tx[8][160];
        if (json_get(sc, "text", &v)) {
            json_iter(v, &it);
            while (json_next(&it, 0, &e) && txt_n < 8) {
                json_str(e, tx[txt_n], sizeof(tx[txt_n]));
                txts[txt_n] = tx[txt_n];
                txt_n++;
            }
        }
    }
    if (json_get(sc, "paste", &v))
        json_str(v, paste, sizeof(paste));
    if (json_get(sc, "deviceId", &v))
        json_str(v, devid, sizeof(devid));
    env_cb_host[0] = 0;
    if (json_get(sc, "callbackHost", &v))
        json_str(v, env_cb_host, sizeof(env_cb_host));
    p = oa_find(prov);
    CHECK(p != 0);
    if (!p)
        return;
    pi_auth_io(&auth, &io);
    io.info = t_info;
    io.prompt = t_prompt;
    io.select = t_select;
    io.device_id = devid[0] ? devid : 0;
    jw_init(&end);
    err[0] = 0;
    if (!strcmp(op, "login"))
        rc = p->login(&io, &end, err, sizeof(err));
    else {
        json_get(sc, "credential", &cred);
        rc = p->refresh(&io, cred, &end, err, sizeof(err));
    }
    jw_rawz(&ev, rc ? "error " : "ok ");
    jw_rawz(&ev, rc ? err : end.p);
    jw_rawz(&ev, "\n");
    jw_free(&end);
    if (json_get(sc, "normalizePort", &v))
        norm_port(&ev);
    strcpy(path, name);
    strcat(path, ".out");
    want_out = golden(path);
    cmp_text("what the user saw", name, want_out, ev.p);
    free(want_out);
    strcpy(path, name);
    strcat(path, ".req");
    want_req = golden(path);
    got_req = slurp(reqlog, 0);
    cmp_text("the requests", name, want_req, got_req ? got_req : "");
    free(want_req);
    free(got_req);
}

/* the scenarios of a file: scenarios.json is the oracle's (tools/pi_oracle/gen_oauth.mjs writes their goldens from pi's own run);
 * scenarios_local.json is what pi cannot be run on here -- its loopback callback server has no counterpart on this machine, so a
 * redirect is pasted back -- with goldens written by hand from oauth/radius.ts and oauth/openrouter.ts */
static void test_scenarios(const char *file, int expect)
{
    char *text;
    long n;
    jv root, list, sc;
    jit it;
    int count = 0;
    char path[600];
    const char *only = getenv("PI_LOGIN_ONLY");
    strcpy(path, PI_TESTS "/pi/oauth/");
    strcat(path, file);
    text = slurp(path, &n);
    CHECK(text != 0);
    if (!text)
        return;
    CHECK_INT(json_parse(text, n, &root), 0);
    json_get(root, "scenarios", &list);
    json_iter(list, &it);
    while (json_next(&it, 0, &sc)) {
        if (only) {
            jv nm;
            char nmb[96];
            json_get(sc, "name", &nm);
            json_str(nm, nmb, sizeof(nmb));
            if (strcmp(nmb, only))
                continue;
        }
        run_scenario(sc);
        count++;
    }
    CHECK(only || count == expect);     /* every scenario of the file */
    free(text);
}


/* ---- units ---- */

static void test_units(void)
{
    jw o;
    char err[200], *want;
    const char *in;
    long n;
    /* URLSearchParams: node's encoding of the same pair */
    jw_init(&o);
    oa_form_add(&o, "a b", "x*y-z._~!'()%/+ \303\251");
    CHECK_STR(o.p, "a+b=x*y-z._%7E%21%27%28%29%25%2F%2B+%C3%A9");
    oa_form_add(&o, "k", "v");
    CHECK_STR(o.p, "a+b=x*y-z._%7E%21%27%28%29%25%2F%2B+%C3%A9&k=v");
    jw_free(&o);
    /* auth.json as pi writes it (JSON.stringify(x, null, 2)): the members that stay come back as they were */
    in = (const char *)golden("store_in.json");
    n = (long)strlen(in);
    {
        static const char *const names[] = { "store_added.json", "store_replaced.json", "store_removed.json" };
        static const char *const keys[] = { "kimi-coding", "openai", "openai" };
        static const char *const vals[] = { "{\"type\":\"oauth\",\"access\":\"k\",\"refresh\":\"r2\",\"expires\":5}",
                                            "{\"type\":\"api_key\",\"key\":\"sk-y\"}", 0 };
        int i;
        for (i = 0; i < 3; i++) {
            char *w = golden(names[i]);
            long wn = w ? (long)strlen(w) : 0;
            jw_init(&o);
            CHECK_INT(pi_jobj_set(in, n, keys[i], vals[i], &o, err, sizeof(err)), 0);
            if (wn && w[wn - 1] == '\n')
                w[wn - 1] = 0;
            CHECK(w && !strcmp(w, o.p ? o.p : ""));
            if (w && strcmp(w, o.p ? o.p : ""))
                printf("--- want\n%s\n--- got\n%s\n", w, o.p);
            jw_free(&o);
            free(w);
        }
    }
    free((char *)in);
    jw_init(&o);
    CHECK_INT(pi_jobj_set("", 0, "kimi-coding", "{\"type\":\"oauth\",\"access\":\"k\",\"refresh\":\"r2\",\"expires\":5}", &o, err, sizeof(err)), 0);
    want = golden("store_first.json");
    if (want && want[strlen(want) - 1] == '\n')
        want[strlen(want) - 1] = 0;
    CHECK(want && !strcmp(want, o.p));
    free(want);
    jw_reset(&o);
    CHECK_INT(pi_jobj_set("{\"a\":{\"x\":1}}", 13, "a", 0, &o, err, sizeof(err)), 0);
    CHECK_STR(o.p, "{}");
    /* raw tokens stay: a number and an escape the way the file had them */
    jw_reset(&o);
    CHECK_INT(pi_jobj_set("{\"keep\":{\"n\":1e3,\"s\":\"\\u00e9\\/\"}}", 33, "b", "1", &o, err, sizeof(err)), 0);
    CHECK_STR(o.p, "{\n  \"keep\": {\n    \"n\": 1e3,\n    \"s\": \"\\u00e9\\/\"\n  },\n  \"b\": 1\n}");
    /* a file that is not an object is not written over */
    jw_reset(&o);
    CHECK_INT(pi_jobj_set("[1]", 3, "b", "1", &o, err, sizeof(err)), -1);
    CHECK_INT(pi_jobj_set("{\"a\":", 5, "b", "1", &o, err, sizeof(err)), -1);
    jw_free(&o);
    /* the JWT claim of a Codex token, base64url as well as base64 (pi's atob takes only the latter) */
    {
        char id[80];
        CHECK_INT(llm_jwt_account_id("e30.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8_In19.x", id, sizeof(id)), 0);
        CHECK_STR(id, "acct_?");
        CHECK_INT(llm_jwt_account_id("abc.def", id, sizeof(id)), -1);
        CHECK_INT(llm_jwt_account_id("a.b.c.d", id, sizeof(id)), -1);
    }
    /* expiry: five minutes ahead counts as expired, a credential with no expires is left alone */
    {
        jv c;
        llm_ts t;
        t.sec = 1000;
        t.ms = 0;
        json_parse("{\"expires\":1300000}", 19, &c);
        CHECK(oa_expires_soon(c, t, 300));
        t.sec = 999;
        CHECK(!oa_expires_soon(c, t, 300));
        json_parse("{\"x\":1}", 7, &c);
        CHECK(!oa_expires_soon(c, t, 300));
    }
    /* the providers that have a login: pi's six, never Anthropic */
    CHECK(llm_oauth_provider("openai") && llm_oauth_provider("openai-codex") && llm_oauth_provider("github-copilot") &&
          llm_oauth_provider("kimi-coding") && llm_oauth_provider("xai") && llm_oauth_provider("meta"));
    CHECK(!llm_oauth_provider("anthropic") && !llm_oauth_provider("google") && !llm_oauth_provider("azure"));
    CHECK(oa_find("anthropic") && !oa_find("anthropic")->login);
}

/* ---- auth.json written and read after an interrupted write ---- */

static void test_store_files(void)
{
    char err[200], *t;
    long n;
    char p[600];
    CHECK_INT(pi_store_write(&sys, agent, "x.json", "{\"a\":1}", 7, err, sizeof(err)), 0);
    t = pi_store_read(&sys, agent, "x.json", &n);
    CHECK(t && n == 7 && !strcmp(t, "{\"a\":1}"));
    free(t);
    CHECK_INT(pi_store_write(&sys, agent, "x.json", "{\"a\":2}", 7, err, sizeof(err)), 0);
    t = pi_store_read(&sys, agent, "x.json", &n);
    CHECK(t && !strcmp(t, "{\"a\":2}"));
    free(t);
    strcpy(p, agent);
    strcat(p, "/x.json.new");
    CHECK(access(p, F_OK) != 0);        /* nothing left behind */
    /* the file gone but the new one written (an interruption between the two renames): the new one is read */
    strcpy(p, agent);
    strcat(p, "/x.json");
    remove(p);
    strcat(p, ".new");
    put(p, "{\"a\":3}", 7);
    t = pi_store_read(&sys, agent, "x.json", &n);
    CHECK(t && !strcmp(t, "{\"a\":3}"));
    free(t);
    remove(p);
    strcpy(p, agent);
    strcat(p, "/x.json.old");
    put(p, "{\"a\":4}", 7);
    t = pi_store_read(&sys, agent, "x.json", &n);
    CHECK(t && !strcmp(t, "{\"a\":4}"));
    free(t);
    remove(p);
    t = pi_store_read(&sys, agent, "x.json", &n);
    CHECK(!t);
}

/* ---- through pi_app: the credential of a request, 401, /login, /logout ---- */

#define J1 "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF8xMjMifSwiZXhwIjoxNzYwMjAwMDAwfQ.sig"
#define FAR_FUTURE "2000000000000"

static jw out;
static const char *in_lines[16];
static int in_n;
static long clock_s;
static const char *env_name[4], *env_val[4];

static llm_ts adv_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = NOW_SEC + clock_s++;
    t.ms = 7;
    return t;
}

static long app_env(const char *name, char *o, long cap)
{
    int i;
    for (i = 0; i < 4; i++)
        if (env_name[i] && !strcmp(name, env_name[i]) && (long)strlen(env_val[i]) < cap) {
            strcpy(o, env_val[i]);
            return (long)strlen(o);
        }
    return -1;
}

static long l_read(void *u, char *buf, long cap)
{
    (void)u;
    if (!in_lines[in_n])
        return -1;
    cl_copy(buf, in_lines[in_n++], cap);
    return (long)strlen(buf);
}

static void l_write(void *u, const char *z, long n)
{
    (void)u;
    jw_raw(&out, z, n);
}

static int brk_flag;                    /* Ctrl+C is pressed */

static int l_brk(void *u)
{
    (void)u;
    return brk_flag;
}

/* the stub forgets the last run's requests and answers when the next run's answers are queued (or the run starts) */
static int stale = 1;

static void q_begin(void)
{
    if (stale) {
        ps_reset();
        ps_pool_up(LLM_POOL);
        stale = 0;
    }
}

/* the queue of answers the stub gives, in the order of the requests */
static void q_http(int status, const char *body)
{
    char head[200], *all;
    long bl = (long)strlen(body);
    q_begin();
    sprintf(head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %ld\r\n\r\n", status, bl);
    all = (char *)malloc(strlen(head) + (size_t)bl + 1);
    strcpy(all, head);
    strcat(all, body);
    ps_http_text(all, (long)strlen(all));
    free(all);
}

static void q_sse(const char *name)
{
    q_begin();
    ps_sse(name, 0);
}

static void write_agent(const char *file, const char *text)
{
    char path[600];
    strcpy(path, agent);
    strcat(path, "/");
    strcat(path, file);
    put(path, text, (long)strlen(text));
}

static char *read_agent(const char *file)
{
    char path[600];
    strcpy(path, agent);
    strcat(path, "/");
    strcat(path, file);
    return slurp(path, 0);
}

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0]) - 1))

static int run_app(int argc, char **argv)
{
    cl_io io;
    pi_platform pl;
    q_begin();
    stale = 1;
    memset(&io, 0, sizeof(io));
    io.read_line = l_read;
    io.write = l_write;
    io.brk = l_brk;
    jw_free(&out);
    jw_init(&out);
    in_n = 0;
    clock_s = 0;
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.h.log = ps_log;
    pl.now = adv_now;
    pl.getenv = app_env;
    pl.random = t_random;
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = root;
    pl.utc = 1;
    pl.data_dir = data;
    pl.agent_dir = agent;
    {
        int rc = pi_app(argc, argv, &pl);
        if (getenv("PI_LOGIN_DEBUG") && out.p)
            printf("---- run %s: nreq %d\n%s\n", argc > 2 ? argv[2] : "", ps.nreq, out.p);
        return rc;
    }
}

/* ---- no token byte anywhere it must not be ---- */

static const char *const secrets[] = { "cgat-old-token", "cgrt-1", "cgat-new-token", "cgrt-new", "kmat-access-0001", "kmrt-refresh-0001", "kmat-access-0002", "gho_githubtoken1",
                                       "xat-access-0001", "xrt-refresh-0001", "mkey-minted-0001", "mid-identity-1", "sk-typed-key", "rat-access-0001", "rrt-refresh-0001", "sk-or-v1-dialog", "rrt-refresh-0002", J1, 0 };

static int has_secret(const char *text, long n)
{
    int i;
    for (i = 0; secrets[i]; i++) {
        long m = (long)strlen(secrets[i]), k;
        for (k = 0; k + m <= n; k++)
            if (!memcmp(text + k, secrets[i], (size_t)m))
                return i + 1;
    }
    return 0;
}

/* every file below dir (the session files, the diagnostics): none holds a secret */
static int dir_leaks(const char *path)
{
    char cmd[700], *b;
    int leak = 0;
    FILE *f;
    char list[600];
    long n;
    strcpy(list, root);
    strcat(list, "/files.txt");
    sprintf(cmd, "find '%s' -type f > '%s'", path, list);
    if (system(cmd))
        return 0;
    b = slurp(list, &n);
    if (b) {
        char *l = b, *nl;
        while (*l) {
            char *c;
            long cn;
            nl = strchr(l, '\n');
            if (nl)
                *nl = 0;
            c = slurp(l, &cn);
            if (c) {
                if (has_secret(c, cn)) {
                    leak = 1;
                    printf("  leak in %s\n", l);
                }
                free(c);
            }
            if (!nl)
                break;
            l = nl + 1;
        }
        free(b);
    }
    f = 0;
    (void)f;
    return leak;
}

/* after a run: the console, the debug log and the session files */
static void no_leaks(void)
{
    CHECK(!has_secret(out.p ? out.p : "", out.n));
    CHECK(!has_secret(ps.log.p ? ps.log.p : "", ps.log.n));
    CHECK(!dir_leaks(data));
}

/* a request head's header, case as sent: 1 when the line is in the head */
static int has_line(const char *head, const char *l)
{
    char pat[700];
    strcpy(pat, "\r\n");
    strcat(pat, l);
    strcat(pat, "\r\n");
    return strstr(head, pat) != 0;
}

static int count_of(const char *head, const char *name)
{
    char pat[100];
    const char *p = head;
    int n = 0;
    strcpy(pat, "\r\n");
    strcat(pat, name);
    strcat(pat, ":");
    while ((p = strstr(p, pat)) != 0) {
        n++;
        p += 2;
    }
    return n;
}

static void remove_agent_files(void)
{
    char p[600];
    strcpy(p, agent);
    strcat(p, "/auth.json");
    remove(p);
    strcpy(p, agent);
    strcat(p, "/settings.json");
    remove(p);
}

static void auth_json(const char *text)
{
    write_agent("auth.json", text);
}

#define OPENAI_OLD "{\n  \"openai\": {\"type\":\"oauth\",\"access\":\"cgat-old-token\",\"refresh\":\"cgrt-1\",\"expires\":%s,\"clientId\":\"app_issued_1\",\"scopes\":[\"openid\",\"chatgpt.tokens.use.direct\"]},\n  \"google\": {\"type\":\"api_key\",\"key\":\"gemini-stored\"},\n  \"future-provider\": {\"type\":\"newkind\",\"x\":[1,2,3]}\n}\n"
#define TOKEN_NEW "{\"access_token\":\"cgat-new-token\",\"refresh_token\":\"cgrt-new\",\"id_token\":\"i.d.t\",\"scope\":\"openid chatgpt.tokens.use.direct\",\"expires_in\":3600}"

static void openai_file(const char *expires)
{
    char t[1200];
    sprintf(t, OPENAI_OLD, expires);
    auth_json(t);
}

/* O5: an expired credential is renewed once before the request and the request carries the new token */
static void test_refresh_reach(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    long r0 = pi_auth_refreshes, f0 = llm_feeds_responses;
    char *file, *want_google;
    jv root_v, e, v;
    env_name[0] = "OPENAI_API_KEY";
    env_val[0] = "sk-env-not-used";
    openai_file("1");
    q_http(200, TOKEN_NEW);
    q_sse("../pi/responses/plain.sse");
    q_sse("../pi/responses/plain.sse");
    in_lines[0] = "hello";
    in_lines[1] = "again";
    in_lines[2] = 0;
    CHECK_INT(run_app(ARGC(a), a), 0);
    CHECK_INT(pi_auth_refreshes - r0, 1);               /* once, for two prompts */
    CHECK_INT(ps.nreq, 3);
    if (ps.nreq < 3)
        return;
    CHECK(strstr(ps.head[0], "POST /api/accounts/oauth/token HTTP/1.1\r\nHost: auth.openai.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.body[0], "grant_type=refresh_token&client_id=app_issued_1&refresh_token=cgrt-1&resource=https%3A%2F%2Fapi.openai.com%2Fv1") == ps.body[0]);
    CHECK(strstr(ps.head[1], "POST /v1/responses HTTP/1.1\r\nHost: api.openai.com\r\n") == ps.head[1]);
    CHECK(has_line(ps.head[1], "Authorization: Bearer cgat-new-token"));
    CHECK(has_line(ps.head[2], "Authorization: Bearer cgat-new-token"));
    CHECK(strstr(ps.head[1], "cgat-old-token") == 0 && strstr(ps.head[1], "sk-env-not-used") == 0);   /* the stored login owns the provider */
    CHECK(out.p && strstr(out.p, "Hello") != 0);
    CHECK(llm_feeds_responses > f0);                    /* the request itself ran */
    /* the rotated refresh token is on disk, the other entries as they were */
    file = read_agent("auth.json");
    CHECK(file != 0);
    if (file) {
        CHECK_INT(json_parse(file, (long)strlen(file), &root_v), 0);
        CHECK(json_get(root_v, "openai", &e) && json_get(e, "access", &v) && json_streq(v, "cgat-new-token"));
        CHECK(json_get(e, "refresh", &v) && json_streq(v, "cgrt-new"));
        CHECK(json_get(e, "clientId", &v) && json_streq(v, "app_issued_1"));
        CHECK(json_get(e, "expires", &v) && json_type(v) == J_NUM);
        CHECK(json_get(root_v, "google", &e) && json_get(e, "key", &v) && json_streq(v, "gemini-stored"));
        CHECK(json_get(root_v, "future-provider", &e) && json_get(e, "x", &v) && json_count(v) == 3);
        want_google = strstr(file, "\"google\": {\n    \"type\": \"api_key\",\n    \"key\": \"gemini-stored\"\n  }");
        CHECK(want_google != 0);                        /* pi's layout */
        free(file);
    }
    no_leaks();
    /* a credential that is good for an hour: no refresh at all */
    r0 = pi_auth_refreshes;
    openai_file(FAR_FUTURE);
    q_sse("../pi/responses/plain.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(a), a), 0);
    CHECK_INT(pi_auth_refreshes - r0, 0);
    CHECK_INT(ps.nreq, 1);
    CHECK(has_line(ps.head[0], "Authorization: Bearer cgat-old-token"));
    no_leaks();
    env_name[0] = 0;
}

/* O3: the API refuses the token (401): renewed once, the request goes again once, then the error */
static void test_401(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    long r0;
    const char *bad = "{\"error\":{\"message\":\"Incorrect token\"}}";
    openai_file(FAR_FUTURE);
    r0 = pi_auth_refreshes;
    q_http(401, bad);
    q_http(200, TOKEN_NEW);
    q_sse("../pi/responses/plain.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(a), a), 0);
    CHECK_INT(ps.nreq, 3);
    CHECK_INT(pi_auth_refreshes - r0, 1);
    if (ps.nreq == 3) {
        CHECK(has_line(ps.head[0], "Authorization: Bearer cgat-old-token"));
        CHECK(strstr(ps.head[1], "POST /api/accounts/oauth/token") == ps.head[1]);
        CHECK(has_line(ps.head[2], "Authorization: Bearer cgat-new-token"));
    }
    CHECK(out.p && strstr(out.p, "Hello") != 0);
    no_leaks();
    /* refused again: no third attempt */
    openai_file(FAR_FUTURE);
    r0 = pi_auth_refreshes;
    q_http(401, bad);
    q_http(200, TOKEN_NEW);
    q_http(401, bad);
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(ps.nreq, 3);
    CHECK_INT(pi_auth_refreshes - r0, 1);
    CHECK(out.p && strstr(out.p, "OpenAI API error (401): {\"message\":\"Incorrect token\"}") != 0);
    no_leaks();
    /* the renewal itself refused: the error is the refresh's, and the request is not sent again */
    openai_file(FAR_FUTURE);
    r0 = pi_auth_refreshes;
    q_http(401, bad);
    q_http(400, "{\"error\":\"invalid_grant\"}");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(ps.nreq, 2);
    CHECK_INT(pi_auth_refreshes - r0, 1);
    CHECK(out.p && strstr(out.p, "OAuth refresh failed for openai: OpenAI OAuth token request failed (400): {\"error\":\"invalid_grant\"}") != 0);
    no_leaks();
    /* expired and the renewal refused: the request is never sent */
    openai_file("1");
    q_http(400, "{\"error\":\"invalid_grant\"}");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(ps.nreq, 1);
    CHECK(out.p && strstr(out.p, "OAuth refresh failed for openai: ") != 0);
    no_leaks();
    /* a key typed in with /login is not a login: a 401 is just an error */
    auth_json("{\"openai\":{\"type\":\"api_key\",\"key\":\"sk-stored-key\"}}");
    r0 = pi_auth_refreshes;
    q_http(401, bad);
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(ps.nreq, 1);
    CHECK_INT(pi_auth_refreshes - r0, 0);
    CHECK(has_line(ps.head[0], "Authorization: Bearer sk-stored-key"));
    CHECK(out.p && strstr(out.p, "OpenAI API error (401)") != 0);
    /* Meta: the identity token cannot be renewed; the key is minted again from it, and when it is refused pi's message */
    {
        char *m[] = { "amiga-pi", "--model", "meta/muse-spark-1.3", 0 };
        auth_json("{\"meta\":{\"type\":\"oauth\",\"access\":\"mkey-old-token\",\"refresh\":\"mid-identity-1\",\"expires\":1}}");
        q_http(200, "{\"api_key\":\"mkey-minted-0002\"}");
        q_sse("../pi/responses/plain.sse");
        in_lines[0] = "hello";
        in_lines[1] = 0;
        CHECK_INT(run_app(ARGC(m), m), 0);
        CHECK_INT(ps.nreq, 2);
        if (ps.nreq == 2) {
            CHECK(strstr(ps.head[0], "POST /muse-code/key HTTP/1.1\r\nHost: api.meta.ai\r\n") == ps.head[0]);
            CHECK(has_line(ps.head[0], "Authorization: Bearer mid-identity-1") && has_line(ps.head[0], "x-api-version: 1.0.0"));
            CHECK(strstr(ps.head[1], "POST /v1/responses HTTP/1.1\r\nHost: api.meta.ai\r\n") == ps.head[1]);
            CHECK(has_line(ps.head[1], "Authorization: Bearer mkey-minted-0002"));
        }
        no_leaks();
        auth_json("{\"meta\":{\"type\":\"oauth\",\"access\":\"mkey-old-token\",\"refresh\":\"mid-identity-1\",\"expires\":1}}");
        q_http(401, "{\"detail\":\"expired\"}");
        in_lines[0] = "hello";
        in_lines[1] = 0;
        run_app(ARGC(m), m);
        CHECK_INT(ps.nreq, 1);
        CHECK(out.p && strstr(out.p, "OAuth refresh failed for meta: Meta session expired (status 401). Run `/login meta` to sign in again.: expired") != 0);
        no_leaks();
    }
}

/* each provider's request: the Bearer form, the base URL, the headers its login brings */
static void test_requests(void)
{
    char *k[] = { "amiga-pi", "--model", "kimi-coding/kimi-for-coding", 0 };
    char *c1[] = { "amiga-pi", "--model", "github-copilot/gpt-5.4", 0 };
    char *c2[] = { "amiga-pi", "--model", "github-copilot/claude-sonnet-5", 0 };
    char *cx[] = { "amiga-pi", "--model", "openai-codex/gpt-6.1-sol", 0 };
    char *x[] = { "amiga-pi", "--model", "xai/grok-4.7", 0 };
    long r0 = pi_auth_refreshes, fa = llm_feeds_anthropic, fc = llm_feeds_codex;
    /* Kimi: anthropic-messages with Authorization: Bearer in place of x-api-key */
    auth_json("{\"kimi-coding\":{\"type\":\"oauth\",\"access\":\"kmat-access-0001\",\"refresh\":\"kmrt-refresh-0001\",\"expires\":" FAR_FUTURE "}}");
    q_sse("../pi/switch_anthropic.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(k), k), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /coding/v1/messages HTTP/1.1\r\nHost: api.kimi.com\r\n") == ps.head[0]);
        CHECK(has_line(ps.head[0], "Authorization: Bearer kmat-access-0001"));
        CHECK(strstr(ps.head[0], "x-api-key") == 0);
    }
    CHECK(llm_feeds_anthropic > fa);
    no_leaks();
    /* Copilot: the base URL is the token's (proxy-ep), the catalog's four headers once each, the dynamic ones */
    auth_json("{\"github-copilot\":{\"type\":\"oauth\",\"refresh\":\"gho_githubtoken1\",\"access\":\"tid=a;proxy-ep=proxy.business.githubcopilot.com;x=1\","
              "\"expires\":" FAR_FUTURE ",\"availableModelIds\":[\"gpt-5.4\",\"claude-sonnet-5\"]}}");
    q_sse("../pi/responses/plain.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(c1), c1), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /responses HTTP/1.1\r\nHost: api.business.githubcopilot.com\r\n") == ps.head[0]);
        CHECK(has_line(ps.head[0], "Authorization: Bearer tid=a;proxy-ep=proxy.business.githubcopilot.com;x=1"));
        CHECK(has_line(ps.head[0], "User-Agent: GitHubCopilotChat/0.35.0") && count_of(ps.head[0], "User-Agent") == 1);
        CHECK(has_line(ps.head[0], "Editor-Version: vscode/1.107.0") && has_line(ps.head[0], "Editor-Plugin-Version: copilot-chat/0.35.0"));
        CHECK(has_line(ps.head[0], "Copilot-Integration-Id: vscode-chat"));
        CHECK(has_line(ps.head[0], "X-Initiator: user") && has_line(ps.head[0], "Openai-Intent: conversation-edits"));
        CHECK(strstr(ps.head[0], "Copilot-Vision-Request") == 0);
    }
    no_leaks();
    q_sse("../pi/switch_anthropic.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(c2), c2), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /v1/messages HTTP/1.1\r\nHost: api.business.githubcopilot.com\r\n") == ps.head[0]);
        CHECK(has_line(ps.head[0], "Authorization: Bearer tid=a;proxy-ep=proxy.business.githubcopilot.com;x=1"));
        CHECK(strstr(ps.head[0], "x-api-key") == 0);
        CHECK_INT(count_of(ps.head[0], "User-Agent"), 1);
    }
    /* an account's models are the list its login saved: gpt-5-mini is not in it */
    {
        char *c3[] = { "amiga-pi", "--model", "github-copilot/gpt-5.4", 0 };
        in_lines[0] = "/model";
        in_lines[1] = 0;
        run_app(ARGC(c3), c3);
        CHECK(out.p && strstr(out.p, "github-copilot/gpt-5.4") != 0 && strstr(out.p, "github-copilot/claude-sonnet-5") != 0);
        CHECK(out.p && strstr(out.p, "gpt-5-mini") == 0 && strstr(out.p, "gemini-3.5-flash") == 0);
    }
    /* Codex: ChatGPT's backend, the account id from the token */
    auth_json("{\"openai-codex\":{\"type\":\"oauth\",\"access\":\"" J1 "\",\"refresh\":\"cxrt-refresh-0001\",\"expires\":" FAR_FUTURE ",\"accountId\":\"acct_123\"}}");
    q_sse("../pi/responses/plain.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(cx), cx), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /backend-api/codex/responses HTTP/1.1\r\nHost: chatgpt.com\r\n") == ps.head[0]);
        CHECK(has_line(ps.head[0], "Authorization: Bearer " J1));
        CHECK(has_line(ps.head[0], "chatgpt-account-id: acct_123") && has_line(ps.head[0], "originator: pi"));
        CHECK(has_line(ps.head[0], "OpenAI-Beta: responses=experimental") && has_line(ps.head[0], "accept: text/event-stream"));
        CHECK(strstr(ps.body[0], "{\"model\":\"gpt-6.1-sol\",\"store\":false,\"stream\":true,\"instructions\":\"") == ps.body[0]);
        CHECK(strstr(ps.body[0], "\"input\":[{\"role\":\"user\"") != 0);
        CHECK(strstr(ps.body[0], "\"text\":{\"verbosity\":\"low\"},\"include\":[\"reasoning.encrypted_content\"],\"prompt_cache_key\":") != 0);
        CHECK(strstr(ps.body[0], ",\"tool_choice\":\"auto\",\"parallel_tool_calls\":true") != 0);
        /* no output cap in the body; pi's tools carry constrainedSampling, which the Responses tools turn into strict: true */
        CHECK(strstr(ps.body[0], "\"max_output_tokens\"") == 0 && strstr(ps.body[0], "\"strict\":true") != 0);
    }
    CHECK(llm_feeds_codex > fc && out.p && strstr(out.p, "Hello") != 0);
    no_leaks();
    /* xAI */
    auth_json("{\"xai\":{\"type\":\"oauth\",\"access\":\"xat-access-0001\",\"refresh\":\"xrt-refresh-0001\",\"expires\":" FAR_FUTURE "}}");
    q_sse("../pi/responses/plain.sse");
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run_app(ARGC(x), x), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /v1/responses HTTP/1.1\r\nHost: api.x.ai\r\n") == ps.head[0]);
        CHECK(has_line(ps.head[0], "Authorization: Bearer xat-access-0001"));
    }
    no_leaks();
    /* OpenRouter (L2): the OAuth result is a plain API key (openrouter.ts toAuth), never renewed (expires is MAX_SAFE_INTEGER): the
     * request goes to OpenRouter's chat completions with it as the Bearer key, and nothing is refreshed */
    {
        char *orm[] = { "amiga-pi", "--model", "openrouter/moonshotai/kimi-k2.6", 0 };
        auth_json("{\"openrouter\":{\"type\":\"oauth\",\"access\":\"sk-or-v1-reach\",\"refresh\":\"\",\"expires\":9007199254740991}}");
        q_sse("../pi/openai_text.sse");
        in_lines[0] = "hello";
        in_lines[1] = 0;
        CHECK_INT(run_app(ARGC(orm), orm), 0);
        CHECK_INT(ps.nreq, 1);
        if (ps.nreq == 1) {
            CHECK(strstr(ps.head[0], "POST /api/v1/chat/completions HTTP/1.1\r\nHost: openrouter.ai\r\n") == ps.head[0]);
            CHECK(has_line(ps.head[0], "Authorization: Bearer sk-or-v1-reach"));
        }
        CHECK(!(out.p && strstr(out.p, "sk-or-v1-reach")));
    }
    CHECK_INT(pi_auth_refreshes - r0, 0);
    /* a stored Anthropic OAuth entry is no credential here (pi would use it; the owner said never): it is not sent */
    {
        char *an[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0 };
        auth_json("{\"anthropic\":{\"type\":\"oauth\",\"access\":\"sk-ant-oat01-secret\",\"refresh\":\"r\",\"expires\":" FAR_FUTURE "}}");
        env_name[0] = "ANTHROPIC_API_KEY";
        env_val[0] = "sk-ant-env-key";
        q_sse("../pi/switch_anthropic.sse");
        in_lines[0] = "hello";
        in_lines[1] = 0;
        run_app(ARGC(an), an);
        env_name[0] = 0;
        CHECK(ps.nreq == 0 || (strstr(ps.head[0], "sk-ant-oat01-secret") == 0 && strstr(ps.head[0], "sk-ant-env-key") == 0));
        CHECK(!has_secret(out.p ? out.p : "", out.n) && !(out.p && strstr(out.p, "sk-ant-oat01-secret")));
    }
}


/* the base URL a Copilot token brings, the Bearer form, the model list of an account (vtcon llm/creds.c) */
static void test_creds(void)
{
    llm_cfg c;
    llm_oauth_use u;
    llm_model m;
    static const char *const tokens[][2] = {
        { "tid=a;proxy-ep=proxy.individual.githubcopilot.com;x=1", "https://api.individual.githubcopilot.com" },
        { "tid=a;proxy-ep=proxy.business.githubcopilot.com", "https://api.business.githubcopilot.com" },
        { "proxy-ep=copilot.example.com;tid=a", "https://copilot.example.com" },
        { "tid=a;x=1", "https://api.individual.githubcopilot.com" }
    };
    char auth[600];
    int i;
    llm_cfg_init(&c);
    llm_model_init(&m);
    m.id = "gpt-5.4";
    m.provider = "github-copilot";
    m.api = "openai-responses";
    for (i = 0; i < 4; i++) {
        sprintf(auth, "{\"github-copilot\":{\"type\":\"oauth\",\"access\":\"%s\",\"refresh\":\"r\",\"expires\":1}}", tokens[i][0]);
        c.auth = auth;
        c.auth_n = (long)strlen(auth);
        CHECK_INT(llm_cfg_oauth(&c, &m, &u), 1);
        CHECK_STR(u.base, tokens[i][1]);
        CHECK(u.copilot == 1 && u.bearer == 0);
    }
    /* an enterprise account without a proxy-ep in its token */
    strcpy(auth, "{\"github-copilot\":{\"type\":\"oauth\",\"access\":\"tid=a\",\"refresh\":\"r\",\"expires\":1,\"enterpriseUrl\":\"https://Company.GHE.com/x\"}}");
    c.auth = auth;
    c.auth_n = (long)strlen(auth);
    CHECK_INT(llm_cfg_oauth(&c, &m, &u), 1);
    CHECK_STR(u.base, "https://copilot-api.company.ghe.com");
    m.api = "anthropic-messages";
    CHECK_INT(llm_cfg_oauth(&c, &m, &u), 1);
    CHECK_INT(u.bearer, 1);             /* copilot's Anthropic route is Bearer auth */
    /* availableModelIds: only those models; without the list, all */
    strcpy(auth, "{\"github-copilot\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":1,\"availableModelIds\":[\"gpt-5.4\"]}}");
    c.auth = auth;
    c.auth_n = (long)strlen(auth);
    m.id = "claude-sonnet-5";
    CHECK(llm_cfg_model_ok(&c, &m) == 0);
    m.id = "gpt-5.4";
    CHECK(llm_cfg_model_ok(&c, &m) == 1);
    strcpy(auth, "{\"github-copilot\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":1}}");
    c.auth_n = (long)strlen(auth);
    m.id = "other";
    CHECK(llm_cfg_model_ok(&c, &m) == 1);
    /* an API key entry is not a login; Anthropic has none */
    strcpy(auth, "{\"github-copilot\":{\"type\":\"api_key\",\"key\":\"k\"},\"anthropic\":{\"type\":\"oauth\",\"access\":\"sk-ant-oat01-x\",\"refresh\":\"r\",\"expires\":1}}");
    c.auth_n = (long)strlen(auth);
    CHECK_INT(llm_cfg_oauth(&c, &m, &u), 0);
    m.provider = "anthropic";
    m.api = "anthropic-messages";
    {
        char key[100];
        const llm_model mm = m;
        CHECK_INT(llm_cfg_oauth(&c, &mm, &u), 0);
        CHECK_INT(llm_cfg_key(&c, &mm, key, sizeof(key), 0), 0);       /* the stored entry owns the provider and gives nothing */
    }
    llm_cfg_free(&c);
}

static void t_sink(void *u, const llm_event *e)
{
    (void)u;
    if (e->type == LE_ERROR && getenv("PI_LOGIN_DEBUG"))
        printf("request error: %.*s\n", (int)e->n, e->p);
}

/* Copilot's per-request headers: X-Initiator by the last message, the vision header for an image; the model's own
 * User-Agent replaces pi's default */
static void test_copilot_headers(void)
{
    llm_req q;
    llm_ctx c;
    llm_model m;
    llm_sink k;
    jw um, um2, tr, hdr;
    const char *msgs[2];
    long mn[2];
    llm_ts ts;
    ts.sec = 1760100000L;
    ts.ms = 0;
    memset(&q, 0, sizeof(q));
    jw_init(&um);
    jw_init(&um2);
    jw_init(&tr);
    jw_init(&hdr);
    llm_m_user(&um, "[{\"type\":\"text\",\"text\":\"hi\"},{\"type\":\"image\",\"data\":\"QQ==\",\"mimeType\":\"image/png\"}]",
               (long)strlen("[{\"type\":\"text\",\"text\":\"hi\"},{\"type\":\"image\",\"data\":\"QQ==\",\"mimeType\":\"image/png\"}]"), ts);
    llm_m_toolresult(&tr, "call_1", "read", "[{\"type\":\"text\",\"text\":\"x\"}]", (long)strlen("[{\"type\":\"text\",\"text\":\"x\"}]"), 0, 0, 0, -1, ts);
    llm_m_user(&um2, "[{\"type\":\"text\",\"text\":\"hi\"}]", (long)strlen("[{\"type\":\"text\",\"text\":\"hi\"}]"), ts);
    llm_model_init(&m);
    m.id = "gpt-5.4";
    m.provider = "github-copilot";
    m.api = "openai-responses";
    m.base_url = "https://api.individual.githubcopilot.com";
    m.max_tokens = 4096;
    m.context_window = 100000;
    m.headers = "{\"User-Agent\":\"GitHubCopilotChat/0.35.0\",\"Editor-Version\":\"vscode/1.107.0\"}";
    memset(&c, 0, sizeof(c));
    c.model = &m;
    c.msgs = msgs;
    c.msg_n = mn;
    k.u = 0;
    k.ev = t_sink;
    /* a user message last: a user-initiated request, no image */
    q_begin();
    q_sse("../pi/responses/plain.sse");
    msgs[0] = um2.p;
    mn[0] = um2.n;
    c.nmsg = 1;
    q.pool = &ps_pool;
    q.model = &m;
    q.be = llm_backend_of(m.api);
    q.ctx = &c;
    q.user_agent = "UP-Term-amiga-pi/0.1 (AmigaOS)";
    q.sink = &k;
    q.key = "tid=abcdefgh;x=1";
    q.copilot = 1;
    jw_rawz(&hdr, "User-Agent: GitHubCopilotChat/0.35.0\r\nEditor-Version: vscode/1.107.0\r\n");
    q.xhead = hdr.p;
    q.retries = -1;
    llm_request(&q);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(has_line(ps.head[0], "X-Initiator: user") && has_line(ps.head[0], "Openai-Intent: conversation-edits"));
        CHECK(strstr(ps.head[0], "Copilot-Vision-Request") == 0);
        CHECK_INT(count_of(ps.head[0], "User-Agent"), 1);
        CHECK(has_line(ps.head[0], "User-Agent: GitHubCopilotChat/0.35.0") && has_line(ps.head[0], "Editor-Version: vscode/1.107.0"));
        CHECK(has_line(ps.head[0], "Authorization: Bearer tid=abcdefgh;x=1"));
    }
    stale = 1;
    /* an image in the conversation, a tool result last: agent-initiated, with the vision header */
    q_begin();
    q_sse("../pi/responses/plain.sse");
    msgs[0] = um.p;
    mn[0] = um.n;
    msgs[1] = tr.p;
    mn[1] = tr.n;
    c.nmsg = 2;
    llm_request(&q);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(has_line(ps.head[0], "X-Initiator: agent") && has_line(ps.head[0], "Copilot-Vision-Request: true"));
    }
    stale = 1;
    jw_free(&um);
    jw_free(&um2);
    jw_free(&tr);
    jw_free(&hdr);
}


/* a login's token, like any key, never goes over plain http: not as a key, not in the Bearer form */
static void test_plain_http_key(void)
{
    llm_req q;
    llm_ctx c;
    llm_model m;
    llm_sink k;
    jw um;
    const char *msgs[1];
    long mn[1];
    llm_ts ts;
    int bearer;
    ts.sec = NOW_SEC;
    ts.ms = 0;
    for (bearer = 0; bearer < 2; bearer++) {
        memset(&q, 0, sizeof(q));
        jw_init(&um);
        llm_m_user(&um, "[{\"type\":\"text\",\"text\":\"hi\"}]", (long)strlen("[{\"type\":\"text\",\"text\":\"hi\"}]"), ts);
        llm_model_init(&m);
        m.id = "kimi-for-coding";
        m.provider = "kimi-coding";
        m.api = bearer ? "anthropic-messages" : "openai-responses";
        m.base_url = "http://192.168.1.10:8080";
        m.max_tokens = 4096;
        m.context_window = 100000;
        memset(&c, 0, sizeof(c));
        c.model = &m;
        msgs[0] = um.p;
        mn[0] = um.n;
        c.msgs = msgs;
        c.msg_n = mn;
        c.nmsg = 1;
        k.u = 0;
        k.ev = t_sink;
        q_begin();
        q_sse(bearer ? "../pi/switch_anthropic.sse" : "../pi/responses/plain.sse");
        q.pool = &ps_pool;
        q.model = &m;
        q.be = llm_backend_of(m.api);
        q.ctx = &c;
        q.user_agent = "UP-Term-amiga-pi/0.1 (AmigaOS)";
        q.sink = &k;
        q.key = "kmat-access-0001";
        q.bearer = bearer;
        q.h.resolve = ps_resolve;
        q.h.notice = ps_notice;
        q.retries = -1;
        llm_request(&q);
        CHECK_INT(ps.nreq, 1);
        if (ps.nreq == 1)
            CHECK(strstr(ps.head[0], "kmat-access-0001") == 0 && strstr(ps.head[0], "Authorization") == 0 && strstr(ps.head[0], "x-api-key") == 0);
        CHECK(!q.key_sent);                   /* the notice is given once per provider and run */
        stale = 1;
        jw_free(&um);
    }
}

/* the dialog: /login and /logout typed at the prompt, through pi_app */
static void test_login_dialog(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    long l0 = pi_auth_logins;
    char *file;
    jv r, e, v;
    const char *dev = "{\"device_code\":\"kd-1\",\"user_code\":\"KIMI-1234\",\"verification_uri\":\"https://www.kimi.com/code/authorize_device\","
                      "\"verification_uri_complete\":\"https://www.kimi.com/code/authorize_device?user_code=KIMI-1234\",\"interval\":1,\"expires_in\":600}";
    remove_agent_files();
    /* /login with no argument: the method, the provider; Kimi's device flow; the session's model (OpenAI, no key) is replaced
     * by the provider's default; then a prompt goes out with the new token */
    q_http(200, dev);
    q_http(400, "{\"error\":\"authorization_pending\"}");
    q_http(200, "{\"access_token\":\"kmat-access-0001\",\"refresh_token\":\"kmrt-refresh-0001\",\"expires_in\":2592000}");
    q_sse("../pi/switch_anthropic.sse");
    in_lines[0] = "/login";
    in_lines[1] = "1";
    in_lines[2] = "2";
    in_lines[3] = "hello";
    in_lines[4] = 0;
    CHECK_INT(run_app(ARGC(a), a), 0);
    CHECK_INT(pi_auth_logins - l0, 1);
    CHECK(out.p && strstr(out.p, "Select authentication method:") != 0 && strstr(out.p, "Sign in with an account") != 0);
    CHECK(out.p && strstr(out.p, "Select provider to configure:") != 0 && strstr(out.p, "Kimi For Coding [subscription]") == 0);
    CHECK(out.p && strstr(out.p, "Login to Kimi For Coding") != 0);
    CHECK(out.p && strstr(out.p, "https://www.kimi.com/code/authorize_device?user_code=KIMI-1234") != 0);
    CHECK(out.p && strstr(out.p, "Enter code: KIMI-1234") != 0);
    CHECK(out.p && strstr(out.p, "Logged in to Kimi For Coding. Selected kimi-for-coding. Credentials saved to ") != 0);
    CHECK_INT(ps.nreq, 4);
    if (ps.nreq == 4) {
        CHECK(strstr(ps.head[0], "POST /api/oauth/device_authorization HTTP/1.1\r\nHost: auth.kimi.com\r\n") == ps.head[0]);
        CHECK(strstr(ps.body[0], "client_id=17e5f671-d194-4dfb-9706-5516cb48c098") == ps.body[0]);
        CHECK(has_line(ps.head[3], "Authorization: Bearer kmat-access-0001"));
    }
    file = read_agent("auth.json");
    CHECK(file != 0);
    if (file) {
        CHECK_INT(json_parse(file, (long)strlen(file), &r), 0);
        CHECK(json_get(r, "kimi-coding", &e) && json_get(e, "type", &v) && json_streq(v, "oauth"));
        CHECK(json_get(e, "access", &v) && json_streq(v, "kmat-access-0001") && json_get(e, "refresh", &v) && json_streq(v, "kmrt-refresh-0001"));
        free(file);
    }
    file = read_agent("settings.json");
    CHECK(file && strstr(file, "\"defaultProvider\": \"kimi-coding\"") && strstr(file, "\"defaultModel\": \"kimi-for-coding\""));
    free(file);
    no_leaks();
    /* /login openai: both methods exist, so the method is asked; an API key is typed and kept as pi keeps it */
    remove_agent_files();
    in_lines[0] = "/login openai";
    in_lines[1] = "2";
    in_lines[2] = "sk-typed-key";
    in_lines[3] = 0;
    l0 = pi_auth_logins;
    run_app(ARGC(a), a);
    CHECK(out.p && strstr(out.p, "Select authentication method for OpenAI:") != 0 && strstr(out.p, "Sign in with ChatGPT") != 0);
    CHECK(out.p && strstr(out.p, "Enter OpenAI API key") != 0 && strstr(out.p, "Saved API key for OpenAI. Credentials saved to ") != 0);
    file = read_agent("auth.json");
    CHECK(file && !strcmp(file, "{\n  \"openai\": {\n    \"type\": \"api_key\",\n    \"key\": \"sk-typed-key\"\n  }\n}"));
    free(file);
    CHECK(!has_secret(out.p ? out.p : "", out.n));
    /* /logout: the stored credentials listed, one removed, the others kept */
    auth_json("{\n  \"openai\": {\"type\":\"api_key\",\"key\":\"sk-typed-key\"},\n  \"kimi-coding\": {\"type\":\"oauth\",\"access\":\"kmat-access-0001\",\"refresh\":\"kmrt-refresh-0001\",\"expires\":1},\n  \"mystery\": {\"type\":\"oauth\",\"access\":\"z\",\"refresh\":\"z\",\"expires\":1}\n}");
    in_lines[0] = "/logout";
    in_lines[1] = "1";
    in_lines[2] = "/logout";
    in_lines[3] = "2";
    in_lines[4] = 0;
    run_app(ARGC(a), a);
    CHECK(out.p && strstr(out.p, "Select provider to logout:") != 0);
    CHECK(out.p && strstr(out.p, "Logged out of Kimi For Coding") != 0);
    CHECK(out.p && strstr(out.p, "Removed stored API key for OpenAI. Environment variables and models.json config are unchanged.") != 0);
    file = read_agent("auth.json");
    CHECK(file && strstr(file, "mystery") && !strstr(file, "kimi-coding") && !strstr(file, "\"openai\""));
    free(file);
    /* nothing stored */
    auth_json("{}");
    in_lines[0] = "/logout";
    in_lines[1] = 0;
    run_app(ARGC(a), a);
    CHECK(out.p && strstr(out.p, "No stored credentials to remove. /logout only removes credentials saved by /login; environment variables and models.json config are unchanged.") != 0);
    /* a cancelled menu changes nothing */
    remove_agent_files();
    in_lines[0] = "/login";
    in_lines[1] = "";
    in_lines[2] = 0;
    l0 = pi_auth_logins;
    run_app(ARGC(a), a);
    CHECK_INT(pi_auth_logins - l0, 0);
    CHECK(read_agent("auth.json") == 0);
}


/* position of needle in hay, -1 when it is not there */
static long pos_of(const char *hay, const char *needle)
{
    const char *p = hay ? strstr(hay, needle) : 0;
    return p ? (long)(p - hay) : -1;
}

/* OpenRouter and Radius through /login (pi 1.1.0: oauth/openrouter.ts, oauth/radius.ts, interactive-mode.ts
 * showLoginAuthTypeSelector, showAuthSelect): the top-level selector's last option is Radius with its status; the account list
 * has both providers in pi's order and never Anthropic; Radius' selector carries its intro; both logins end in auth.json and the
 * flow went through the program's own entry (pi_auth_logins) */
static void test_openrouter_radius_dialog(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    static const char *const intro = "Radius is a service crafted for Pi by the builders of Pi, Earendil Works";
    long l0, i_codex, i_or, i_rad, i_xai;
    char *file;
    const char *list;
    jv r, e, v;
    /* the top-level selector: Radius is the third and last option, with the status suffix of the provider list */
    remove_agent_files();
    in_lines[0] = "/login";
    in_lines[1] = "";
    in_lines[2] = 0;
    run_app(ARGC(a), a);
    i_or = pos_of(out.p, "1) Sign in with an account");
    i_rad = pos_of(out.p, "2) Sign in with an API key");
    i_xai = pos_of(out.p, "3) Sign in with Radius - not configured");
    CHECK(i_or >= 0 && i_rad > i_or && i_xai > i_rad);
    CHECK(pos_of(out.p, "4) ") < 0);
    /* the status of the option: an account, an API key stored, an API key in the environment */
    write_agent("auth.json", "{\"radius\":{\"type\":\"oauth\",\"access\":\"x\",\"refresh\":\"y\",\"expires\":99999999999999}}");
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "3) Sign in with Radius - configured") >= 0);
    write_agent("auth.json", "{\"radius\":{\"type\":\"api_key\",\"key\":\"k\"}}");
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "3) Sign in with Radius - API key configured") >= 0);
    remove_agent_files();
    env_name[0] = "RADIUS_API_KEY";
    env_val[0] = "e";
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "3) Sign in with Radius - API key configured") >= 0);
    env_name[0] = 0;
    /* the account list: OpenRouter and Radius between OpenAI Codex and xAI, no Anthropic */
    in_lines[0] = "/login";
    in_lines[1] = "1";
    in_lines[2] = "";
    in_lines[3] = 0;
    run_app(ARGC(a), a);
    list = out.p ? strstr(out.p, "Select provider to configure:") : 0;
    CHECK(list != 0);
    i_codex = pos_of(list, "OpenAI Codex (legacy) - not configured");
    i_or = pos_of(list, ") OpenRouter - not configured");
    i_rad = pos_of(list, ") Radius - not configured");
    i_xai = pos_of(list, ") xAI - not configured");
    CHECK(i_codex >= 0 && i_or > i_codex && i_rad > i_or && i_xai > i_rad);
    CHECK(pos_of(out.p, "Anthropic") < 0);
    /* /login radius: the provider has an account and an API key, so only the two kinds are offered (no Radius option on top) */
    in_lines[0] = "/login radius";
    in_lines[1] = "";
    in_lines[2] = 0;
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "Select authentication method for Radius:") >= 0 && pos_of(out.p, "Sign in with an account") >= 0);
    CHECK(pos_of(out.p, "Sign in with Radius") < 0);
    /* Sign in with Radius: the method (with the intro under the title), the device code, the credential kept */
    remove_agent_files();
    l0 = pi_auth_logins;
    q_http(200, "{\"device_code\":\"rd-1\",\"user_code\":\"RAD-42\",\"verification_uri\":\"https://radius.pi.dev/device\",\"expires_in\":600,\"interval\":1}");
    q_http(400, "{\"error\":\"authorization_pending\"}");
    q_http(200, "{\"access_token\":\"rat-access-0001\",\"refresh_token\":\"rrt-refresh-0001\",\"expires_in\":3600,\"scope\":\"gateway offline_access\"}");
    q_http(500, "no catalog today");           /* the catalog refresh that follows a Radius login (L3): a failure keeps the cached models */
    in_lines[0] = "/login";
    in_lines[1] = "3";
    in_lines[2] = "2";
    in_lines[3] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(pi_auth_logins - l0, 1);
    i_or = pos_of(out.p, "Sign in to Radius:");
    i_rad = pos_of(out.p, intro);
    CHECK(i_or >= 0 && i_rad > i_or && pos_of(out.p, "1) Sign in with browser (recommended)") > i_rad);
    CHECK(pos_of(out.p, "2) Sign in with device code (when signing in from another device)") >= 0);
    CHECK(pos_of(out.p, "Login to Radius") >= 0 && pos_of(out.p, "Enter code: RAD-42") >= 0);
    CHECK(pos_of(out.p, "Logged in to Radius") >= 0);
    CHECK_INT(ps.nreq, 4);
    if (ps.nreq == 4) {
        CHECK(strstr(ps.head[3], "GET /v1/config HTTP/1.1\r\nHost: radius.pi.dev\r\n") == ps.head[3]);
        CHECK(strstr(ps.head[3], "authorization: Bearer rat-access-0001\r\n") != 0);
        CHECK(pos_of(out.p, "Logged in to Radius, but its model catalog could not be refreshed; using cached models.") >= 0);
        CHECK(strstr(ps.head[0], "POST /v1/oauth/device HTTP/1.1\r\nHost: radius.pi.dev\r\n") == ps.head[0]);
        CHECK(strstr(ps.body[0], "client_id=pi-gateway&scope=gateway+offline_access") == ps.body[0]);
        CHECK(strstr(ps.body[2], "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=pi-gateway&device_code=rd-1") == ps.body[2]);
    }
    file = read_agent("auth.json");
    CHECK(file != 0);
    if (file) {
        CHECK_INT(json_parse(file, (long)strlen(file), &r), 0);
        CHECK(json_get(r, "radius", &e) && json_get(e, "type", &v) && json_streq(v, "oauth"));
        CHECK(json_get(e, "access", &v) && json_streq(v, "rat-access-0001") && json_get(e, "refresh", &v) && json_streq(v, "rrt-refresh-0001"));
        CHECK(json_get(e, "scope", &v) && json_streq(v, "gateway offline_access"));
        free(file);
    }
    no_leaks();
    /* the intro belongs to Radius' selector alone: Codex' method selector has none */
    remove_agent_files();
    in_lines[0] = "/login openai-codex";
    in_lines[1] = "";
    in_lines[2] = 0;
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "Select OpenAI Codex login method:") >= 0 && pos_of(out.p, intro) < 0);
    /* Sign in with OpenRouter: the method of the provider, the authorization URL, the code pasted back, the key exchanged */
    remove_agent_files();
    l0 = pi_auth_logins;
    q_http(200, "{\"key\":\"sk-or-v1-dialog\"}");
    in_lines[0] = "/login openrouter";
    in_lines[1] = "1";
    in_lines[2] = "http://127.0.0.1:50000/oauth/callback/x?code=my-code";
    in_lines[3] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(pi_auth_logins - l0, 1);
    CHECK(pos_of(out.p, "Select authentication method for OpenRouter:") >= 0 && pos_of(out.p, "1) Sign in with OpenRouter") >= 0);
    CHECK(pos_of(out.p, "Login to OpenRouter") >= 0 && pos_of(out.p, "https://openrouter.ai/auth?callback_url=http%3A%2F%2F127.0.0.1%3A") >= 0);
    CHECK(pos_of(out.p, "Logged in to OpenRouter") >= 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /api/v1/auth/keys HTTP/1.1\r\nHost: openrouter.ai\r\n") == ps.head[0]);
        CHECK(strstr(ps.body[0], "{\"code\":\"my-code\",\"code_verifier\":\"") == ps.body[0]);
    }
    file = read_agent("auth.json");
    CHECK(file && !strcmp(file, "{\n  \"openrouter\": {\n    \"type\": \"oauth\",\n    \"access\": \"sk-or-v1-dialog\",\n    \"refresh\": \"\",\n    \"expires\": 9007199254740991\n  }\n}"));
    free(file);
    no_leaks();
}


/* the Radius login's device flow answers, queued */
static void q_radius_login(void)
{
    q_http(200, "{\"device_code\":\"rd-1\",\"user_code\":\"RAD-42\",\"verification_uri\":\"https://radius.pi.dev/device\",\"expires_in\":600,\"interval\":1}");
    q_http(200, "{\"access_token\":\"rat-access-0001\",\"refresh_token\":\"rrt-refresh-0001\",\"expires_in\":3600}");
}

/* the file in the agent directory, 0 none */
static int agent_file_is(const char *file, const char *want)
{
    char *f = read_agent(file);
    int same = f && want && !strcmp(f, want);
    free(f);
    return same;
}

static void remove_mcp_json(void)
{
    char p[600];
    strcpy(p, agent);
    strcat(p, "/mcp.json");
    remove(p);
}

/* After a Radius login pi offers to point the Radius MCP server of the global mcp.json at it (interactive-mode.ts
 * offerRadiusMcpServer): add it, repoint one on the same URL (dropping its oauth), pick radius-mcp when "radius" is taken, ask
 * nothing when a server already uses the login */
static void test_radius_mcp_offer(void)
{
    char *a[] = { "amiga-pi", "--no-mcp", "--model", "openai/gpt-4.1", 0 };
    static const char *const added = "{\n  \"mcpServers\": {\n    \"radius\": {\n      \"url\": \"https://radius.pi.dev/mcp\",\n"
                                     "      \"auth\": {\n        \"provider\": \"radius\"\n      }\n    }\n  }\n}\n";
    /* none: the question, Yes writes the server */
    remove_agent_files();
    remove_mcp_json();
    q_radius_login();
    in_lines[0] = "/login";
    in_lines[1] = "3";
    in_lines[2] = "2";
    in_lines[3] = "1";
    in_lines[4] = 0;
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "Configure Radius MCP in ") >= 0 && pos_of(out.p, "mcp.json?") >= 0);
    CHECK(agent_file_is("mcp.json", added));
    /* No writes nothing */
    remove_agent_files();
    remove_mcp_json();
    q_radius_login();
    in_lines[3] = "2";
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "Configure Radius MCP in ") >= 0);
    CHECK(read_agent("mcp.json") == 0);
    /* a server on the same URL (a trailing slash does not matter) is repointed: auth in, oauth out, the rest kept */
    remove_agent_files();
    write_agent("mcp.json", "{\n    \"mcpServers\": {\n        \"mine\": {\"url\": \"https://radius.pi.dev/mcp/\", \"oauth\": {\"clientId\": \"x\"}, \"timeout\": 5},\n"
                "        \"other\": {\"command\": \"x\"}\n    }\n}\n");
    q_radius_login();
    in_lines[3] = "1";
    run_app(ARGC(a), a);
    CHECK(agent_file_is("mcp.json", "{\n  \"mcpServers\": {\n    \"mine\": {\n      \"url\": \"https://radius.pi.dev/mcp/\",\n      \"timeout\": 5,\n"
                                    "      \"auth\": {\n        \"provider\": \"radius\"\n      }\n    },\n    \"other\": {\n      \"command\": \"x\"\n    }\n  }\n}\n"));
    /* "radius" taken by another server: radius-mcp */
    remove_agent_files();
    write_agent("mcp.json", "{\"mcpServers\":{\"radius\":{\"command\":\"x\"}}}");
    q_radius_login();
    run_app(ARGC(a), a);
    CHECK(agent_file_is("mcp.json", "{\n  \"mcpServers\": {\n    \"radius\": {\n      \"command\": \"x\"\n    },\n    \"radius-mcp\": {\n"
                                    "      \"url\": \"https://radius.pi.dev/mcp\",\n      \"auth\": {\n        \"provider\": \"radius\"\n      }\n    }\n  }\n}\n"));
    /* a server that already uses this login: nothing is asked, the file is as it was */
    remove_agent_files();
    write_agent("mcp.json", "{\"mcpServers\":{\"r\":{\"url\":\"https://radius.pi.dev/mcp\",\"auth\":{\"provider\":\"radius\"}}}}");
    q_radius_login();
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "Logged in to Radius") >= 0 && pos_of(out.p, "Configure Radius MCP") < 0);
    CHECK(agent_file_is("mcp.json", "{\"mcpServers\":{\"r\":{\"url\":\"https://radius.pi.dev/mcp\",\"auth\":{\"provider\":\"radius\"}}}}"));
    /* a file that is not pi's shape is reported, not written over */
    remove_agent_files();
    write_agent("mcp.json", "{\"mcpServers\":[]}");
    q_radius_login();
    run_app(ARGC(a), a);
    CHECK(agent_file_is("mcp.json", "{\"mcpServers\":[]}"));
    remove_mcp_json();
    remove_agent_files();
}

/* An MCP server with "auth": {"provider": "radius"} sends the login's token at every request (mcp/runtime.ts providerToken,
 * streamable-http.ts headers), renews it first when it is about to expire, and says to sign in on a 401 */
static void test_mcp_provider_auth(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    static const char *const init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{}},"
                                    "\"serverInfo\":{\"name\":\"radius\",\"version\":\"1\"}}}";
    static const char *const tools = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[]}}";
    long mark;
    char *file;
    jv r, e, v;
    remove_agent_files();
    write_agent("mcp.json", "{\"mcpServers\":{\"radius\":{\"url\":\"https://radius.pi.dev/mcp\",\"auth\":{\"provider\":\"radius\"},"
                "\"headers\":{\"Authorization\":\"Bearer configured\",\"X-Extra\":\"1\"}}}}");
    /* a stored token that is good: it is the bearer of every request, over a configured Authorization header */
    write_agent("auth.json", "{\"radius\":{\"type\":\"oauth\",\"access\":\"rat-access-0001\",\"refresh\":\"rrt-refresh-0001\",\"expires\":" FAR_FUTURE "}}");
    q_http(200, init);
    q_http(202, "");
    q_http(200, tools);
    q_http(500, "");                            /* the interactive start's catalog refresh (L3), after the server was started */
    in_lines[0] = 0;
    run_app(ARGC(a), a);
    CHECK_INT(ps.nreq, 4);
    if (ps.nreq == 4) {
        int i;
        CHECK(strstr(ps.head[0], "POST /mcp HTTP/1.1\r\nHost: radius.pi.dev\r\n") == ps.head[0]);
        for (i = 0; i < 3; i++) {
            CHECK(has_line(ps.head[i], "Authorization: Bearer rat-access-0001"));
            CHECK(count_of(ps.head[i], "Authorization") == 1 && strstr(ps.head[i], "Bearer configured") == 0);
            CHECK(has_line(ps.head[i], "X-Extra: 1"));
        }
    }
    CHECK(!has_secret(out.p ? out.p : "", out.n));
    /* a token that expired is renewed before the first request, and the rotated refresh token is saved */
    write_agent("auth.json", "{\"radius\":{\"type\":\"oauth\",\"access\":\"rat-old\",\"refresh\":\"rrt-refresh-0001\",\"expires\":1}}");
    mark = pi_auth_refreshes;
    q_http(200, "{\"access_token\":\"rat-access-0001\",\"refresh_token\":\"rrt-refresh-0002\",\"expires_in\":3600}");
    q_http(200, init);
    q_http(202, "");
    q_http(200, tools);
    q_http(500, "");
    run_app(ARGC(a), a);
    CHECK_INT(pi_auth_refreshes - mark, 1);
    CHECK_INT(ps.nreq, 5);
    if (ps.nreq == 5) {
        CHECK(strstr(ps.head[0], "POST /v1/oauth/token HTTP/1.1\r\nHost: radius.pi.dev\r\n") == ps.head[0]);
        CHECK(strstr(ps.body[0], "grant_type=refresh_token&client_id=pi-gateway&refresh_token=rrt-refresh-0001") == ps.body[0]);
        CHECK(has_line(ps.head[1], "Authorization: Bearer rat-access-0001"));
    }
    file = read_agent("auth.json");
    CHECK(file && json_parse(file, (long)strlen(file), &r) == 0 && json_get(r, "radius", &e) && json_get(e, "refresh", &v) &&
          json_streq(v, "rrt-refresh-0002"));
    free(file);
    /* no login: no token, the server answers 401, and pi's words say what to run */
    remove_agent_files();
    q_http(401, "{\"error\":\"unauthorized\"}");
    run_app(ARGC(a), a);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1)
        CHECK(strstr(ps.head[0], "Bearer rat-") == 0 && has_line(ps.head[0], "Authorization: Bearer configured"));    /* no token: the configured header stays */
    CHECK(pos_of(out.p, "MCP server \"radius\" requires sign-in. Run /login radius to sign in.") >= 0);
    /* auth needs an https URL (or a loopback one), and a provider name */
    write_agent("mcp.json", "{\"mcpServers\":{\"a\":{\"url\":\"http://mcp.example/x\",\"auth\":{\"provider\":\"radius\"}},"
                "\"b\":{\"url\":\"https://mcp.example/x\",\"auth\":{}}}}");
    in_lines[0] = 0;
    run_app(ARGC(a), a);
    CHECK(pos_of(out.p, "server \"a\": auth requires an https URL, or http on localhost, 127.0.0.1, or [::1]") >= 0);
    CHECK(pos_of(out.p, "server \"b\": auth.provider must be a provider name") >= 0);
    remove_mcp_json();
    remove_agent_files();
}

/* Sign in with ChatGPT through the dialog: the URL to open elsewhere, the redirect pasted back, the device id kept */
static void test_chatgpt_dialog(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    static const char *const state = "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUE";
    char paste_url[400], *file, *dev;
    long l0 = pi_auth_logins;
    jv r, e, v;
    fixed_random = 1;
    remove_agent_files();
    sprintf(paste_url, "http://127.0.0.1:1455/auth/callback?code=CODE-9&state=%s&client_id=app_issued_9", state);
    q_http(200, "{\"access_token\":\"cgat-access-0009\",\"refresh_token\":\"cgrt-refresh-0009\",\"id_token\":\"i.d.t\","
                "\"scope\":\"openid chatgpt.tokens.use.direct\",\"expires_in\":3600}");
    in_lines[0] = "/login openai";
    in_lines[1] = "1";
    in_lines[2] = paste_url;
    in_lines[3] = 0;
    CHECK_INT(run_app(ARGC(a), a), 0);
    CHECK_INT(pi_auth_logins - l0, 1);
    CHECK(out.p && strstr(out.p, "https://auth.openai.com/api/accounts/authorize?client_id=dynamic_agent_client&agent_name_hint=Pi"
                                 "&ext_agent_host_id=urn%3Auuid%3A41414141-4141-4141-8141-414141414141&response_type=code") != 0);
    CHECK(out.p && strstr(out.p, "Complete sign-in in your browser. If the callback does not complete, paste the final redirect URL here.") != 0);
    CHECK(out.p && strstr(out.p, "Complete login in your browser, or paste the final redirect URL here:") != 0);
    CHECK(out.p && strstr(out.p, "e.g., http://127.0.0.1:1455/auth/callback") != 0);
    CHECK(out.p && strstr(out.p, "Logged in to OpenAI. Credentials saved to ") != 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq == 1) {
        CHECK(strstr(ps.head[0], "POST /api/accounts/oauth/token HTTP/1.1\r\nHost: auth.openai.com\r\n") == ps.head[0]);
        CHECK(strstr(ps.body[0], "grant_type=authorization_code&client_id=app_issued_9&code=CODE-9&code_verifier=QUFB") == ps.body[0]);
    }
    file = read_agent("auth.json");
    CHECK(file != 0);
    if (file) {
        CHECK_INT(json_parse(file, (long)strlen(file), &r), 0);
        CHECK(json_get(r, "openai", &e) && json_get(e, "clientId", &v) && json_streq(v, "app_issued_9"));
        free(file);
    }
    dev = read_agent("settings.json");
    CHECK(dev && !strcmp(dev, "{\n  \"deviceId\": \"41414141-4141-4141-8141-414141414141\"\n}"));
    free(dev);
    /* the second login names the same installation and asks for no random bytes for it */
    fixed_random = 0;
    rk = 0;
    in_lines[0] = "/login openai";
    in_lines[1] = "1";
    in_lines[2] = "not a url";
    in_lines[3] = 0;
    l0 = pi_auth_logins;
    run_app(ARGC(a), a);
    CHECK(out.p && strstr(out.p, "ext_agent_host_id=urn%3Auuid%3A41414141-4141-4141-8141-414141414141") != 0);
    CHECK(out.p && strstr(out.p, "Failed to login to OpenAI") != 0 && strstr(out.p, "Paste the full callback URL from the browser") != 0);
    CHECK_INT(pi_auth_logins - l0, 0);
    CHECK_INT(ps.nreq, 0);
    /* Ctrl+C while a device login waits: nothing is saved */
    remove_agent_files();
    q_begin();
    brk_flag = 1;
    in_lines[0] = "/login kimi-coding";
    in_lines[1] = "1";
    in_lines[2] = 0;
    run_app(ARGC(a), a);
    brk_flag = 0;
    CHECK(out.p && strstr(out.p, "Login cancelled") != 0);
    CHECK(read_agent("auth.json") == 0);
    CHECK_INT(ps.nreq, 0);
    no_leaks();
}

/* nothing goes over plain http, and no random source is no login */
static void test_refusals(void)
{
    jv none;
    oa_io io;
    const oa_provider *p = oa_find("kimi-coding");
    jw entry;
    char err[300], url[100], *log;
    sprintf(url, "http://127.0.0.1:%d", fx_port);
    remove(reqlog);
    put(script_path, "{\"responses\":[]}", 16);
    world_up();
    auth.url_base = 0;                  /* the program's setting: no exception for the loopback server */
    env_kimi_host = url;
    pi_auth_io(&auth, &io);
    io.info = t_info;
    io.prompt = t_prompt;
    io.select = t_select;
    jw_init(&entry);
    jw_reset(&ev);
    CHECK_INT(p->login(&io, &entry, err, sizeof(err)), OA_FAIL);
    CHECK(strstr(err, "refusing to send a login over plain http (127.0.0.1); use https") != 0);
    log = slurp(reqlog, 0);
    CHECK(log == 0);                    /* the server saw nothing */
    free(log);
    CHECK_INT(json_parse("{\"refresh\":\"kmrt-refresh-0001\"}", (long)strlen("{\"refresh\":\"kmrt-refresh-0001\"}"), &none), 0);
    jw_reset(&entry);
    CHECK_INT(p->refresh(&io, none, &entry, err, sizeof(err)), OA_FAIL);
    log = slurp(reqlog, 0);
    CHECK(log == 0);
    free(log);
    env_kimi_host = 0;
    /* ChatGPT's login needs random bytes */
    auth.random = 0;
    pi_auth_io(&auth, &io);
    io.info = t_info;
    io.prompt = t_prompt;
    io.select = t_select;
    io.device_id = "5b0f1c2e-1111-4222-8333-444455556666";
    CHECK_INT(oa_find("openai")->login(&io, &entry, err, sizeof(err)), OA_FAIL);
    CHECK_STR(err, "no secure random source on this system");
    jw_free(&entry);
    world_down();
}

void suite_pi_login(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_login_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    strcpy(data, root);
    strcat(data, "/data");
    strcpy(agent, root);
    strcat(agent, "/agent");
    strcpy(odir, root);
    strcat(odir, "/oauth");
    mkdir(data, 0777);
    mkdir(agent, 0777);
    mkdir(odir, 0777);
    strcpy(script_path, odir);
    strcat(script_path, "/script.json");
    strcpy(reqlog, odir);
    strcat(reqlog, "/requests.log");
    sys_posix_init(&sp, &sys);
    jw_init(&ev);
    test_units();
    test_store_files();
    jw_init(&out);
    fx_port = fixture_start();
    CHECK(fx_port > 0);
    if (fx_port > 0) {
        world_up();
        test_scenarios("scenarios.json", 161);
        test_scenarios("scenarios_local.json", LOCAL_COUNT);
        world_down();
        test_refusals();
    }
    test_refresh_reach();
    test_401();
    test_requests();
    test_creds();
    test_copilot_headers();
    test_plain_http_key();
    test_login_dialog();
    test_chatgpt_dialog();
    test_openrouter_radius_dialog();
    test_radius_mcp_offer();
    test_mcp_provider_auth();
    if (fx_h)
        sys.spawn2_close(sys.u, fx_h, 500);
    jw_free(&ev);
    jw_free(&out);
}
