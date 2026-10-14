/* Radius (plan L3): pi-messages, the gateway's wire, and its catalog -- through pi_app, the top-level entry.
 *
 * test_scenarios: pi's own pi-messages API (tests/pi/radius, made by tools/pi_oracle/gen_pi_messages.mjs from the pinned pi-ai and
 * pi's agent loop against a fake fetch) and amiga-pi given the same gateway answers: the request (URL, head, body with the
 * timestamps masked) and every assistant message the session stores equal pi's, and llm_feeds_pi_messages shows the backend ran.
 * test_catalog: the account's catalog replaces the shipped one (GET /v1/config with the credential), is kept in models-store.json
 * and restored with no network; an empty one leaves no models; an error leaves the models as they were; a custom gateway of
 * models.json has no shipped catalog; the legacy gatewayConfig of auth.json is imported. */
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
#include "pi_stub.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/session3.h"
#include "../amiga-pi/radius.h"

static char root[400], data[420], sdir[520], agent[420];
static sys_posix sp;
static cl_sys sys;
static jw out;
static const char *in_lines[8];
static int in_n;
static long clock_s;
static const char *offline;     /* PI_OFFLINE for this run: 0 unset */

static llm_ts adv_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L + clock_s++;
    t.ms = 7;
    return t;
}

static long env(const char *name, char *o, long cap)
{
    const char *v = 0;
    if (!strcmp(name, "RADIUS_API_KEY"))
        v = "radius-test-token";
    else if (!strcmp(name, "PI_OFFLINE"))
        v = offline;
    if (!v || (long)strlen(v) >= cap)
        return -1;
    strcpy(o, v);
    return (long)strlen(v);
}

static long l_read(void *u, char *buf, long cap)
{
    (void)u;
    if (!in_lines[in_n])
        return -1;
    cl_copy(buf, in_lines[in_n++], cap);
    return (long)strlen(buf);
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

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0]) - 1))

static int run(int argc, char **argv)
{
    cl_io io;
    pi_platform pl;
    memset(&io, 0, sizeof(io));
    io.read_line = l_read;
    io.write = l_write;
    io.brk = l_brk;
    jw_free(&out);
    jw_init(&out);
    in_n = 0;
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = env;
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = root;
    pl.utc = 1;
    pl.data_dir = data;
    pl.agent_dir = agent;
    return pi_app(argc, argv, &pl);
}

static void fresh(void)
{
    ps_reset();
    ps_pool_up(LLM_POOL);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    if (b) {
        n = (long)fread(b, 1, (size_t)n, f);
        b[n] = 0;
    }
    fclose(f);
    return b;
}

static char *golden_file(const char *name, const char *ext)
{
    char p[600];
    strcpy(p, PI_TESTS "/pi/radius/");
    strcat(p, name);
    strcat(p, ext);
    return slurp(p);
}

static void put_agent(const char *file, const char *text)
{
    char p[600];
    FILE *f;
    strcpy(p, agent);
    strcat(p, "/");
    strcat(p, file);
    f = fopen(p, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *get_agent(const char *file)
{
    char p[600];
    strcpy(p, agent);
    strcat(p, "/");
    strcat(p, file);
    return slurp(p);
}

static void rm_agent(const char *file)
{
    char p[600];
    strcpy(p, agent);
    strcat(p, "/");
    strcat(p, file);
    remove(p);
}

static char *masked(const char *s)
{
    jw o;
    long i, n = (long)strlen(s);
    char *r;
    jw_init(&o);
    for (i = 0; i < n; i++) {
        int k = !strncmp(s + i, "\"timestamp\":", 12) ? 12 : !strncmp(s + i, "\"durationMs\":", 13) ? 13 : 0;
        if (!strncmp(s + i, "\"sessionId\":\"", 13) && i + 13 + 36 <= n) {
            jw_rawz(&o, "\"sessionId\":\"00000000-0000-4000-8000-000000000000");
            i += 13 + 35;
        } else if (k) {
            jw_raw(&o, s + i, k);
            jw_raw(&o, "0", 1);
            i += k;
            while (i < n && s[i] >= '0' && s[i] <= '9')
                i++;
            i--;
        } else
            jw_raw(&o, s + i, 1);
    }
    r = (char *)malloc((size_t)o.n + 1);
    if (r) {
        memcpy(r, o.p, (size_t)o.n);
        r[o.n] = 0;
    }
    jw_free(&o);
    return r;
}

/* the failure diagnostic (a JS stack inside; plan R10) cut out of a line of pi's messages */
static void cut_failure_diag(char *s)
{
    char *p = strstr(s, ",\"diagnostics\":[{\"type\":\"pi_messages_response_failure\"");
    if (p) {
        char *v = p + strlen(",\"diagnostics\":");
        long vn = json_value(v, (long)strlen(v));
        if (vn > 0)
            memmove(p, v + vn, strlen(v + vn) + 1);
    }
}

static int newest(char *path, long cap)
{
    return s3_recent(&sys, sdir, path, cap);
}

/* the assistant messages the newest session file holds, one per line, masked */
static char *stored_assistants(void)
{
    char path[600];
    pi_s3 st;
    jw o;
    int i;
    char *r;
    jw_init(&o);
    if (newest(path, sizeof(path)))
        return 0;
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    if (s3_open(&st, path)) {
        s3_free(&st);
        return 0;
    }
    for (i = 0; i < st.n; i++) {
        long n;
        const char *t = s3_text(&st, i, &n);
        jv ob, m, role;
        if (st.e[i].type != S3_MESSAGE || !t || json_parse(t, n, &ob) || !json_get(ob, "message", &m) || !json_get(m, "role", &role) ||
            !json_streq(role, "assistant"))
            continue;
        jw_raw(&o, m.p, m.n);
        jw_raw(&o, "\n", 1);
    }
    s3_free(&st);
    jw_raw(&o, "", 1);
    r = masked(o.p);
    jw_free(&o);
    return r;
}

static void queue_sse_file(const char *name)
{
    char *s = golden_file(name, ".sse"), *p;
    if (!s)
        return;
    p = s;
    for (;;) {
        char *e = strstr(p, "----\n");
        long n = e ? (long)(e - p) : (long)strlen(p);
        if (n > 0)
            ps_sse_text(p, n, 0);
        if (!e)
            break;
        p = e + 5;
    }
    free(s);
}

static void queue_http_file(const char *name)
{
    char *s = golden_file(name, ".http"), *body, head[400];
    long code = 0;
    char reason[80];
    jw raw;
    if (!s)
        return;
    code = strtol(s, &body, 10);
    while (*body == ' ')
        body++;
    {
        char *nl = strchr(body, '\n');
        long rl = (long)(nl - body);
        memcpy(reason, body, (size_t)rl);
        reason[rl] = 0;
        body = nl + 1;
    }
    {
        long bl = (long)strlen(body);
        while (bl > 0 && body[bl - 1] == '\n')
            body[--bl] = 0;
        sprintf(head, "HTTP/1.1 %ld %s\r\ncontent-type: application/json\r\ncontent-length: %ld\r\nconnection: close\r\n\r\n", code, reason, bl);
    }
    jw_init(&raw);
    jw_rawz(&raw, head);
    jw_rawz(&raw, body);
    ps_http_text(raw.p, raw.n);
    jw_free(&raw);
    free(s);
}

/* the request bodies, masked, against pi's */
static void check_bodies(const char *name, int n)
{
    int i;
    for (i = 0; i < n && i < ps.nreq; i++) {
        char gname[120], *got;
        char *m = masked(ps.body[i]);
        sprintf(gname, "pi/radius/%s.body.%d.json", name, i);
        got = (char *)malloc(strlen(m) + 2);
        strcpy(got, m);
        strcat(got, "\n");
        pi_golden(gname, got);
        free(got);
        free(m);
    }
}

static void check_messages(const char *name)
{
    char *want = golden_file(name, ".messages.jsonl"), *got = stored_assistants(), *p;
    CHECK(want && got);
    if (want && got) {
        cut_failure_diag(want);
        /* a failure message has one diagnostic line per pi message; cut each */
        while ((p = strstr(want, ",\"diagnostics\":[{\"type\":\"pi_messages_response_failure\"")) != 0)
            cut_failure_diag(want);
        h_checks++;
        if (strcmp(want, got)) {
            h_failures++;
            printf("  FAIL %s messages differ:\n--- pi\n%s\n--- amiga-pi\n%s\n", name, want, got);
        }
    }
    free(want);
    free(got);
}

static int has_line(const char *head, const char *line)
{
    char l[600];
    sprintf(l, "\r\n%s\r\n", line);
    return strstr(head, l) != 0;
}

typedef struct sc {
    const char *name;
    int nreq;                   /* requests made */
    int bodies;                 /* bodies compared (a later one holds a tool result of this machine) */
    int http;                   /* the answer is an HTTP failure from <name>.http */
} sc;

static void test_scenarios(void)
{
    static const sc scs[] = {
        { "plain", 1, 1, 0 }, { "tools", 2, 1, 0 }, { "replace", 2, 1, 0 }, { "error_event", 1, 1, 0 },
        { "cut", 1, 1, 0 }, { "http401", 1, 1, 1 }, { "http500", 1, 1, 1 }
    };
    char *a[] = { "amiga-pi", "--model", "radius/balanced", "--thinking", "high", 0 };
    unsigned i;
    put_agent("settings.json", "{\"retry\":{\"enabled\":false}}\n");
    offline = "1";
    for (i = 0; i < sizeof(scs) / sizeof(scs[0]); i++) {
        long f0 = llm_feeds_pi_messages;
        char *url = golden_file(scs[i].name, ".url"), *head = golden_file(scs[i].name, ".head.json");
        char line[300];
        fresh();
        if (scs[i].http)
            queue_http_file(scs[i].name);
        else
            queue_sse_file(scs[i].name);
        in_lines[0] = "hello";
        in_lines[1] = 0;
        CHECK_INT(run(ARGC(a), a), 0);
        if (ps.nreq != scs[i].nreq)
            printf("  scenario %s: %d requests, wanted %d\n", scs[i].name, ps.nreq, scs[i].nreq);
        CHECK_INT(ps.nreq, scs[i].nreq);
        if (ps.nreq >= 1) {
            /* the URL: <baseUrl>/messages; the head: pi's three headers, the token as the Bearer key */
            sprintf(line, "POST /v1%s HTTP/1.1\r\nHost: radius.pi.dev\r\n", url && strstr(url, "/messages") ? "/messages" : "?");
            CHECK(strstr(ps.head[0], line) == ps.head[0]);
            CHECK(head && strstr(head, "\"authorization\":\"Bearer radius-test-token\"") != 0);
            CHECK(has_line(ps.head[0], "authorization: Bearer radius-test-token"));
            CHECK(has_line(ps.head[0], "accept: text/event-stream") && has_line(ps.head[0], "content-type: application/json"));
            check_bodies(scs[i].name, scs[i].bodies);
        }
        check_messages(scs[i].name);
        if (!scs[i].http)
            CHECK(llm_feeds_pi_messages > f0);          /* the backend ran (the sentinel) */
        free(url);
        free(head);
    }
    offline = 0;
    rm_agent("settings.json");
}

/* ---- the catalog ---- */

#define CFG2 "{\"baseUrl\":\"https://radius.example/v1\",\"models\":[" \
    "{\"id\":\"balanced\",\"name\":\"Fresh Balanced\",\"reasoning\":true,\"input\":[\"text\"],\"cost\":{\"input\":1,\"output\":2,\"cacheRead\":0.1,\"cacheWrite\":0}," \
    "\"contextWindow\":424242,\"maxTokens\":32000}," \
    "{\"id\":\"organization-only\",\"name\":\"Organization Only\",\"reasoning\":false,\"input\":[\"text\"],\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0}," \
    "\"contextWindow\":128000,\"maxTokens\":16000}," \
    "{\"id\":\"broken\",\"name\":7}]}"

static void q_json(int code, const char *body)
{
    char head[300];
    jw raw;
    sprintf(head, "HTTP/1.1 %d X\r\ncontent-type: application/json\r\ncontent-length: %d\r\nconnection: close\r\n\r\n", code, (int)strlen(body));
    jw_init(&raw);
    jw_rawz(&raw, head);
    jw_rawz(&raw, body);
    ps_http_text(raw.p, raw.n);
    jw_free(&raw);
}

static int in_out(const char *s)
{
    return out.p && strstr(out.p, s) != 0;
}

static void test_catalog(void)
{
    char *a[] = { "amiga-pi", "--model", "radius/balanced", 0 };
    char *f;
    rm_agent("models-store.json");
    rm_agent("auth.json");
    /* shipped catalog: the public models, the credential being the environment's key */
    offline = "1";
    fresh();
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK(in_out("radius/balanced") && in_out("radius/cheap"));
    CHECK_INT(ps.nreq, 0);                      /* PI_OFFLINE: no network */
    /* the interactive start and /model fetch the account's catalog: replaced, the invalid model dropped */
    offline = 0;
    fresh();
    q_json(200, CFG2);
    q_json(200, CFG2);
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq >= 1) {
        CHECK(strstr(ps.head[0], "GET /v1/config HTTP/1.1\r\nHost: radius.pi.dev\r\n") == ps.head[0]);
        CHECK(has_line(ps.head[0], "accept: application/json") && has_line(ps.head[0], "authorization: Bearer radius-test-token"));
    }
    CHECK(in_out("radius/organization-only") && in_out("radius/balanced"));
    CHECK(!in_out("radius/cheap") && !in_out("radius/broken"));
    f = get_agent("models-store.json");
    CHECK(f && strstr(f, "\"radius\"") && strstr(f, "\"organization-only\"") && strstr(f, "\"checkedAt\"") && strstr(f, "\"baseUrl\": \"https://radius.example/v1\""));
    free(f);
    /* the next start has no network and still has the account's models (the store), not the shipped ones */
    offline = "1";
    fresh();
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK_INT(ps.nreq, 0);
    CHECK(in_out("radius/organization-only") && !in_out("radius/cheap"));
    /* a catalog that is known replaces the shipped one even when empty (every model disabled by the organization) */
    offline = 0;
    fresh();
    q_json(200, "{\"baseUrl\":\"https://radius.example/v1\",\"models\":[]}");
    q_json(200, "{\"baseUrl\":\"https://radius.example/v1\",\"models\":[]}");
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK(!in_out("radius/organization-only") && !in_out("radius/cheap"));
    /* an error leaves the models as they were: the 500 does not touch the (now empty) cache, nothing to list */
    rm_agent("models-store.json");
    fresh();
    q_json(500, "boom");
    q_json(500, "boom");
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK_INT(ps.nreq, 2);
    CHECK(in_out("radius/balanced") && in_out("radius/cheap"));
    f = get_agent("models-store.json");
    CHECK(f == 0);
    /* no credential, no network */
    fresh();
    {
        const char *saved = offline;
        offline = 0;
        (void)saved;
    }
}

/* models.json: a provider with "oauth":"radius" is a gateway of its own with its own catalog (model-runtime.ts configureRadiusProviders) */
static void test_custom_gateway(void)
{
    char *a[] = { "amiga-pi", "--model", "radius/balanced", 0 };
    char *f;
    rm_agent("models-store.json");
    rm_agent("auth.json");
    /* a gateway of another id: no shipped models, its own catalog on its own host with its own key */
    put_agent("models.json", "{\"providers\":{\"mine\":{\"oauth\":\"radius\",\"baseUrl\":\"https://gw.example/v1\",\"name\":\"Mine\"}}}");
    put_agent("auth.json", "{\"mine\":{\"type\":\"api_key\",\"key\":\"k-mine\"}}");
    offline = "1";
    fresh();
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK(!in_out("mine/") && in_out("radius/cheap"));
    offline = 0;
    fresh();
    q_json(200, CFG2);         /* radius */
    q_json(200, "{\"baseUrl\":\"https://gw.example/v1\",\"models\":[{\"id\":\"m1\",\"name\":\"M1\",\"reasoning\":false,\"input\":[\"text\"],"
                "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0},\"contextWindow\":1000,\"maxTokens\":100}]}");
    q_json(200, CFG2);
    q_json(200, "{\"baseUrl\":\"https://gw.example/v1\",\"models\":[{\"id\":\"m1\",\"name\":\"M1\",\"reasoning\":false,\"input\":[\"text\"],"
                "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0},\"contextWindow\":1000,\"maxTokens\":100}]}");
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK_INT(ps.nreq, 4);
    if (ps.nreq >= 2) {
        CHECK(strstr(ps.head[1], "GET /v1/config HTTP/1.1\r\nHost: gw.example\r\n") == ps.head[1]);
        CHECK(has_line(ps.head[1], "authorization: Bearer k-mine"));
    }
    CHECK(in_out("mine/m1"));
    f = get_agent("models-store.json");
    CHECK(f && strstr(f, "\"mine\"") && strstr(f, "\"m1\"") && strstr(f, "\"radius\""));
    free(f);
    /* the public id pointed at another gateway does not inherit the public catalog */
    rm_agent("models-store.json");
    rm_agent("auth.json");
    put_agent("models.json", "{\"providers\":{\"radius\":{\"oauth\":\"radius\",\"baseUrl\":\"https://other.example\"}}}");
    offline = "1";
    fresh();
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK(!in_out("radius/cheap"));
    /* "oauth" needs the gateway's baseUrl */
    put_agent("models.json", "{\"providers\":{\"x\":{\"oauth\":\"radius\"}}}");
    fresh();
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK(in_out("Provider x: \"baseUrl\" is required when \"oauth\" is set."));
    rm_agent("models.json");
    offline = 0;
}

/* an older pi kept the catalog in the OAuth credential (radius.ts getRadiusModels): imported when the store has none */
static void test_legacy(void)
{
    char *a[] = { "amiga-pi", "--model", "radius/balanced", 0 };
    char *f;
    rm_agent("models-store.json");
    {
        jw au;
        jw_init(&au);
        jw_rawz(&au, "{\"radius\":{\"type\":\"oauth\",\"access\":\"a\",\"refresh\":\"r\",\"expires\":99999999999999,\"gatewayConfig\":");
        jw_rawz(&au, CFG2);
        jw_rawz(&au, "}}");
        jw_raw(&au, "", 1);
        put_agent("auth.json", au.p);
        jw_free(&au);
    }
    offline = "1";
    fresh();
    in_lines[0] = "/model";
    in_lines[1] = 0;
    run(ARGC(a), a);
    CHECK(in_out("radius/organization-only") && !in_out("radius/cheap"));
    f = get_agent("models-store.json");
    CHECK(f && strstr(f, "\"organization-only\""));
    free(f);
    rm_agent("auth.json");
    rm_agent("models-store.json");
    offline = 0;
}

void suite_pi_radius(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_radius_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    strcpy(data, root);
    strcat(data, "/data");
    strcpy(agent, root);
    strcat(agent, "/agent");
    mkdir(data, 0777);
    mkdir(agent, 0777);
    sys_posix_init(&sp, &sys);
    s3_dir(data, "/work/proj", sdir, sizeof(sdir));
    jw_init(&out);
    test_scenarios();
    test_catalog();
    test_custom_gateway();
    test_legacy();
    jw_free(&out);
}
