/* the model switch through pi_app, the top-level entry (plan phase F: F4 entries, F5 reachability).
 *
 * One conversation across two backends: a prompt to Anthropic (a recording with a signed thinking block), `/model
 * openai/gpt-4.1`, a prompt to OpenAI -- the second request body is the golden tests/pi/model/switch_second.json
 * (the handoff of llm/handoff.c: pi's transformMessages) and both backends' sentinels counted. The session file
 * holds the model_change / thinking_level_change entries pi writes. `!command` in models.json runs once for
 * three requests. */
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

static char root[400], data[420], sdir[520], agent[420], marker[460];
static sys_posix sp;
static cl_sys sys;
static jw out;
static const char *in_lines[12];
static int in_n;
static long clock_s;

static llm_ts adv_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L + clock_s++;
    t.ms = 7;
    return t;
}

static const char *extra_name[4], *extra_val[4];       /* a test's own environment variables */
static int bare_env;                                   /* 1: not even the default OPENAI_API_KEY / ANTHROPIC_API_KEY */

static long env(const char *name, char *o, long cap)
{
    const char *v = 0;
    int i;
    for (i = 0; i < 4; i++)
        if (extra_name[i] && !strcmp(name, extra_name[i]))
            v = extra_val[i];
    if (v)
        ;
    else if (bare_env)
        ;
    else if (!strcmp(name, "OPENAI_API_KEY"))
        v = "sk-test-openai";
    else if (!strcmp(name, "ANTHROPIC_API_KEY"))
        v = "sk-ant-test-not-real";
    if (!v || (long)strlen(v) >= cap)
        return -1;
    strcpy(o, v);
    return (long)strlen(v);
}

static int ro_at = -1;          /* the line number at which the session file turns read-only (a full disk) */
static int newest(char *path, long cap);

static long l_read(void *u, char *buf, long cap)
{
    (void)u;
    if (!in_lines[in_n])
        return -1;
    if (in_n == ro_at) {
        char p[600];
        if (!newest(p, sizeof(p)))
            chmod(p, 0444);
    }
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


static void (*ready_hook)(pi_session *s);

static int run(int argc, char **argv, const char *const *sse, int nsse)
{
    cl_io io;
    pi_platform pl;
    int i;
    ps_reset();
    ps_pool_up(LLM_POOL);
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
    pl.ready = ready_hook;
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = root;
    pl.utc = 1;
    pl.data_dir = data;
    pl.agent_dir = agent;
    for (i = 0; i < nsse; i++) {
        long l = (long)strlen(sse[i]);
        if (l > 3 && !strcmp(sse[i] + l - 3, ".es"))
            ps_stream(sse[i], "application/vnd.amazon.eventstream");        /* an AWS event stream (Amazon Bedrock) */
        else
            ps_sse(sse[i], 0);
    }
    return pi_app(argc, argv, &pl);
}

static void write_file(const char *dir, const char *name, const char *text)
{
    char p[500];
    FILE *f;
    strcpy(p, dir);
    strcat(p, "/");
    strcat(p, name);
    f = fopen(p, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void shape(pi_s3 *st, char *o, long cap)
{
    static const char *const rn[] = { "-", "user", "assistant", "toolResult", "system", "custom", "bash", "other" };
    int i;
    char type[40];
    o[0] = 0;
    for (i = 0; i < st->n; i++) {
        long n;
        const char *t = s3_text(st, i, &n);
        jv ob, v;
        type[0] = 0;
        if (t && !json_parse(t, n, &ob) && json_get(ob, "type", &v))
            json_str(v, type, sizeof(type));
        if (i)
            cl_cat(o, " ", cap);
        cl_cat(o, type, cap);
        if (st->e[i].type == S3_MESSAGE) {
            cl_cat(o, "/", cap);
            cl_cat(o, rn[st->e[i].role], cap);
        }
    }
}

static int newest(char *path, long cap)
{
    return s3_recent(&sys, sdir, path, cap);
}

/* the entry's JSON text as one field's value, "" when absent */
static void field(pi_s3 *st, int i, const char *key, char *o, long cap)
{
    long n;
    const char *t = s3_text(st, i, &n);
    jv ob, v;
    o[0] = 0;
    if (t && !json_parse(t, n, &ob) && json_get(ob, key, &v) && json_type(v) == J_STR)
        json_str(v, o, cap);
}

/* F5: Anthropic, /model openai/..., OpenAI */
static void test_switch(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0 };
    static const char *const sse[] = { "../pi/switch_anthropic.sse", "../pi/responses/plain.sse" };
    char path[600], sh[600], v[120];
    pi_s3 st;
    long f_open = llm_feeds_responses, f_anth = llm_feeds_anthropic;
    in_lines[0] = "hello";
    in_lines[1] = "/model openai/gpt-4.1";
    in_lines[2] = "again";
    in_lines[3] = "/model anthropic/claude-opus-5-5";
    in_lines[4] = "/thinking high";
    in_lines[5] = "/session";
    in_lines[6] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    /* the first request: Anthropic, adaptive thinking at pi's default level */
    CHECK(strstr(ps.head[0], "POST /v1/messages HTTP/1.1\r\nHost: api.anthropic.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "x-api-key: sk-ant-test-not-real\r\n") != 0);
    CHECK(strstr(ps.body[0], "{\"role\":\"system\",\"content\":[],\"output_config\":{\"effort\":\"medium\"}}],\"max_tokens\"") != 0);
    /* the second: OpenAI, the first answer's thinking as plain text (pi's transformMessages), the key of the environment */
    CHECK(strstr(ps.head[1], "POST /v1/responses HTTP/1.1\r\nHost: api.openai.com\r\n") == ps.head[1]);
    CHECK(strstr(ps.head[1], "Authorization: Bearer sk-test-openai\r\n") != 0);
    CHECK(strstr(ps.body[1], "\"model\":\"gpt-4.1\"") != 0);
    CHECK(strstr(ps.body[1], "\"thinking\"") == 0 && strstr(ps.body[1], "\"reasoning\"") == 0);
    /* pi's body is the golden, but for the session id (random per run): its prompt_cache_key must be the session's id */
    {
        static const char key[] = "\"prompt_cache_key\":\"";
        static const char fixed[] = "00000000-0000-4000-8000-000000000000";
        char body[40000], *k;
        CHECK_INT(newest(path, sizeof(path)), 0);
        s3_init(&st, &sys, adv_now, 0, 1, 7);
        CHECK_INT(s3_open(&st, path), 0);
        CHECK(strlen(ps.body[1]) < sizeof(body));
        strncpy(body, ps.body[1], sizeof(body) - 1);
        body[sizeof(body) - 1] = 0;
        k = strstr(body, key);
        CHECK(k != 0);
        if (k) {
            k += sizeof(key) - 1;
            CHECK(!strncmp(k, st.id, strlen(st.id)) && strlen(st.id) == strlen(fixed));
            memcpy(k, fixed, strlen(fixed));
        }
        s3_free(&st);
        pi_golden("pi/model/switch_second.json", body);
    }
    CHECK(llm_feeds_anthropic > f_anth && llm_feeds_responses > f_open);    /* both backends ran */
    /* the file: pi's entries for the switch (setModel -> setThinkingLevel, clamped to the model): gpt-4.1 has no
     * reasoning, so the level fell to off; back on opus-5-5, whose map has off: null, off clamps to low (a new entry) */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_STR(sh, "model_change thinking_level_change message/system message/user message/assistant "
                  "model_change thinking_level_change message/user message/assistant model_change "
                  "thinking_level_change thinking_level_change");
    field(&st, 0, "modelId", v, sizeof(v));
    CHECK_STR(v, "claude-opus-5-5");
    field(&st, 1, "thinkingLevel", v, sizeof(v));
    CHECK_STR(v, "medium");
    field(&st, 5, "modelId", v, sizeof(v));
    CHECK_STR(v, "gpt-4.1");
    field(&st, 6, "thinkingLevel", v, sizeof(v));
    CHECK_STR(v, "off");
    field(&st, 9, "modelId", v, sizeof(v));
    CHECK_STR(v, "claude-opus-5-5");
    field(&st, 10, "thinkingLevel", v, sizeof(v));
    CHECK_STR(v, "low");
    field(&st, 11, "thinkingLevel", v, sizeof(v));
    CHECK_STR(v, "high");
    {
        long n;
        const char *t = s3_text(&st, 4, &n);
        /* the answer names the configured model, and its cost is the model's price: 25 input tokens at 4 dollars per
         * million is 0.0001, 1200 cache-write tokens at 5 is 0.006 */
        CHECK(t && strstr(t, "\"model\":\"claude-opus-5-5\"") != 0 && strstr(t, "\"responseModel\":\"claude-opus-5-5-20260101\"") != 0);
        CHECK(t && strstr(t, "\"cost\":{\"input\":0.0001,\"output\":") != 0 && strstr(t, "\"cacheWrite\":0.006") != 0);
    }
    s3_free(&st);
    /* the screen: the switch said what it did, /session shows model, level, context and cost */
    CHECK(strstr(out.p, "[INFO] Model: openai/gpt-4.1, thinking off") != 0);
    CHECK(strstr(out.p, "[INFO] Thinking level: high") != 0);
    CHECK(strstr(out.p, "model anthropic/claude-opus-5-5") != 0);
    CHECK(strstr(out.p, "context 0% of 1000000") != 0);
    CHECK(strstr(out.p, "cost $") != 0);
}

/* the other way: OpenAI first, then an Anthropic model; the level the first had (off) clamps to the model's: low */
static void test_switch_back(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", "--no-session", 0 };
    static const char *const sse[] = { "../pi/responses/plain.sse", "text.sse" };
    long f_open = llm_feeds_responses, f_anth = llm_feeds_anthropic;
    in_lines[0] = "hello";
    in_lines[1] = "/model anthropic/claude-opus-5-5";
    in_lines[2] = "again";
    in_lines[3] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    CHECK(strstr(ps.head[0], "POST /v1/responses HTTP/1.1\r\nHost: api.openai.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[1], "POST /v1/messages HTTP/1.1\r\nHost: api.anthropic.com\r\n") == ps.head[1]);
    CHECK(strstr(ps.body[1], "{\"role\":\"system\",\"content\":[],\"output_config\":{\"effort\":\"low\"}}],\"max_tokens\"") != 0);
    /* pi's request for the Anthropic side is tests/pi/model/switch_back_second.json, byte for byte (the handoff
     * from the OpenAI answer, the managed-effort system messages, cache_control, the tool list with its placeholder) */
    pi_golden("pi/model/switch_back_second.json", ps.body[1]);
    CHECK(llm_feeds_anthropic > f_anth && llm_feeds_responses > f_open);
}

/* FB3: Anthropic alone, three prompts at low, high and medium: the requests are pi's byte for byte; the levels of the
 * earlier answers travel as the system messages pi inserts before each of them. Pi's data path: the level is recorded
 * on the assistant message (providerThinkingLevel) in the session file and comes back on resume. */
static void test_levels(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0 };
    char *c[] = { "amiga-pi", "-c", 0 };
    static const char *const sse[] = { "../pi/switch_anthropic.sse", "../pi/switch_anthropic.sse", "../pi/switch_anthropic.sse" };
    char path[600];
    pi_s3 st;
    int i, kept = 0;
    in_lines[0] = "/thinking low";
    in_lines[1] = "hello";
    in_lines[2] = "/thinking high";
    in_lines[3] = "again";
    in_lines[4] = "/thinking medium";
    in_lines[5] = "third";
    in_lines[6] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 3), 0);
    CHECK_INT(ps.nreq, 3);
    if (ps.nreq < 3)
        return;
    pi_golden("pi/model/levels_first.json", ps.body[0]);
    pi_golden("pi/model/levels_second.json", ps.body[1]);
    pi_golden("pi/model/levels_third.json", ps.body[2]);
    CHECK(strstr(ps.head[2], "anthropic-beta: mid-conversation-output-config-2026-07-01,thinking-binding-controls-2026-08-01,"
                             "inline-tools-2026-09-15\r\n") != 0);
    /* the session file keeps each answer's level */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    for (i = 0; i < st.n; i++) {
        long n;
        const char *t = s3_text(&st, i, &n);
        if (t && strstr(t, "\"role\":\"assistant\"")) {
            static const char *const want[] = { "low", "high", "medium" };
            char needle[64];
            strcpy(needle, "\"providerThinkingLevel\":\"");
            strcat(needle, want[kept < 3 ? kept : 2]);
            strcat(needle, "\"");
            CHECK(strstr(t, needle) != 0);
            kept++;
        }
    }
    s3_free(&st);
    CHECK_INT(kept, 3);
    /* resume: the next request still carries all three levels, from the file */
    in_lines[0] = "fourth";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(c), c, sse, 1), 0);
    CHECK_INT(ps.nreq, 1);
    CHECK(ps.nreq && strstr(ps.body[0], "{\"role\":\"system\",\"content\":[],\"output_config\":{\"effort\":\"low\"}}") != 0);
    CHECK(ps.nreq && strstr(ps.body[0], "{\"role\":\"system\",\"content\":[],\"output_config\":{\"effort\":\"high\"}}") != 0);
    CHECK(ps.nreq && strstr(ps.body[0], "{\"role\":\"system\",\"content\":[],\"output_config\":{\"effort\":\"medium\"}}") != 0);
}

/* FB1: a tool added while the run is going (tool_search's way) is declared to the transcript before the next request
 * of a model that takes tool changes there: a system message with toolsAdded after the tool result, which the request
 * carries as a tool_addition block -- pi's agent loop (declareToolChanges), the request tests/pi/model/declare_second.json */
static pi_tool late_tool, adder_tool;

static int adder_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    (void)u;
    (void)args;
    (void)an;
    *is_error = 0;
    jw_rawz(content, "[{\"type\":\"text\",\"text\":\"ok\"}]");
    return pi_tool_add(s, &late_tool);
}

static void ready_adder(pi_session *s)
{
    adder_tool.name = "adder";
    adder_tool.description = "Adds the tool late";
    adder_tool.parameters = "{\"type\":\"object\",\"properties\":{},\"required\":[]}";
    adder_tool.run = adder_run;
    late_tool.name = "late";
    late_tool.description = "A tool added during the run";
    late_tool.parameters = "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}},\"required\":[\"x\"]}";
    pi_tool_add(s, &adder_tool);
}

static void test_declare(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0 };
    static const char *const sse[] = { "../pi/anthropic/adder.sse", "../pi/switch_anthropic.sse" };
    char path[600], sh[600];
    pi_s3 st;
    int i, with_add = 0;
    ready_hook = ready_adder;
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    ready_hook = 0;
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    pi_golden("pi/model/declare_second.json", ps.body[1]);
    CHECK(strstr(ps.body[0], "\"name\":\"late\"") == 0);                  /* the first request does not know the tool */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_STR(sh, "model_change thinking_level_change message/system message/user message/assistant "
                  "message/toolResult message/system message/assistant");
    for (i = 0; i < st.n; i++) {
        long n;
        const char *t = s3_text(&st, i, &n);
        if (t && strstr(t, "\"role\":\"system\"") && strstr(t, "\"toolsAdded\":[{\"name\":\"late\""))
            with_add++;
    }
    CHECK_INT(with_add, 1);
    s3_free(&st);
}

/* PX2: the first request to a model whose context window is the output cap (gpt-4: 8192 and 8192) carries the cap pi's
 * buildBaseOptions clamps it to, from the estimate over the transcript pi_app sends (tests/pi/model/clamp_first.json,
 * pi-ai's request for the same prompt and tools). Through pi_app: the estimate is computed by the agent */
static void test_clamp_first(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4", 0 };
    static const char *const sse[] = { "../pi/responses/plain.sse" };
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq < 1)
        return;
    CHECK(strstr(ps.body[0], "\"max_output_tokens\":8192") == 0);          /* not the model's own cap */
    CHECK(strstr(ps.body[0], "\"max_output_tokens\":") != 0);
    {
        /* pi's body is the golden, but for the session id (random per run): its prompt_cache_key must be the session's id */
        static const char key[] = "\"prompt_cache_key\":\"";
        static const char fixed[] = "00000000-0000-4000-8000-000000000000";
        char path[600], body[40000], *k;
        pi_s3 st;
        CHECK_INT(newest(path, sizeof(path)), 0);
        s3_init(&st, &sys, adv_now, 0, 1, 7);
        CHECK_INT(s3_open(&st, path), 0);
        CHECK(strlen(ps.body[0]) < sizeof(body));
        strncpy(body, ps.body[0], sizeof(body) - 1);
        body[sizeof(body) - 1] = 0;
        k = strstr(body, key);
        CHECK(k != 0);
        if (k) {
            k += sizeof(key) - 1;
            CHECK(!strncmp(k, st.id, strlen(st.id)) && strlen(st.id) == strlen(fixed));
            memcpy(k, fixed, strlen(fixed));
        }
        s3_free(&st);
        pi_golden("pi/model/clamp_first.json", body);
    }
}

/* PX3: pi's declareToolChanges runs before every request whatever the model. A tool added during the run on a model
 * without mid-conversation system messages (gpt-4.1): the session file gets the declaring system message after the
 * tool result, as pi's agent loop writes it (tests/pi/model/declare_{roles.txt,message.json}, made by
 * tools/pi_oracle/gen_declare.mjs from pi-agent-core runAgentLoop), and the second request lists the tool as pi's does */
static int slurp_line(const char *name, char *o, long cap)
{
    char path[700];
    FILE *f;
    long n;
    strcpy(path, PI_TESTS "/pi/model/");
    strcat(path, name);
    f = fopen(path, "rb");
    o[0] = 0;
    if (!f)
        return -1;
    n = (long)fread(o, 1, (size_t)cap - 1, f);
    fclose(f);
    o[n] = 0;
    while (n > 0 && (o[n - 1] == '\n' || o[n - 1] == '\r'))
        o[--n] = 0;
    return 0;
}

static void test_declare_every_model(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", 0 };
    static const char *const sse[] = { "../pi/responses/adder.sse", "../pi/responses/plain.sse" };
    char path[600], sh[600], want[8000], got[8000], roles[200];
    pi_s3 st;
    int i, found = 0;
    jv tools;
    ready_hook = ready_adder;
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    ready_hook = 0;
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    /* the requests' tools: pi's, strict (the four built-in tools carry constrainedSampling), the new one on the second */
    CHECK_INT(slurp_line("declare_first_tools.json", want, sizeof(want)), 0);
    CHECK(json_parse(ps.body[0], (long)strlen(ps.body[0]), &tools) == 0 && json_get(tools, "tools", &tools));
    if (tools.n != (long)strlen(want) || strncmp(tools.p, want, (size_t)tools.n)) {
        printf("    first request tools\n      got  %.*s\n      want %s\n", (int)tools.n, tools.p, want);
        CHECK(0);
    }
    CHECK_INT(slurp_line("declare_second_tools.json", want, sizeof(want)), 0);
    CHECK(json_parse(ps.body[1], (long)strlen(ps.body[1]), &tools) == 0 && json_get(tools, "tools", &tools));
    if (tools.n != (long)strlen(want) || strncmp(tools.p, want, (size_t)tools.n)) {
        printf("    second request tools\n      got  %.*s\n      want %s\n", (int)tools.n, tools.p, want);
        CHECK(0);
    }
    /* the session file: user, assistant, tool result, the declaration, assistant */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_INT(slurp_line("declare_roles.txt", roles, sizeof(roles)), 0);
    {
        /* pi's roles in message_end order, after the entries that are not messages and the initial system message */
        char exp[600];
        strcpy(exp, "model_change thinking_level_change message/system");
        for (i = 0; roles[i];) {
            char w[40];
            int k = 0;
            while (roles[i] && roles[i] != ' ' && k < 39)
                w[k++] = roles[i++];
            w[k] = 0;
            while (roles[i] == ' ')
                i++;
            strcat(exp, " message/");
            strcat(exp, w);
        }
        CHECK_STR(sh, exp);
    }
    CHECK_INT(slurp_line("declare_message.json", want, sizeof(want)), 0);
    for (i = 0; i < st.n; i++) {
        long n;
        const char *t = s3_text(&st, i, &n);
        jv eo, mo;
        if (t && !json_parse(t, n, &eo) && json_get(eo, "message", &mo) && memchr(mo.p, 'l', (size_t)mo.n) &&
            strstr(t, "\"role\":\"system\"") && strstr(t, "\"toolsAdded\":[{\"name\":\"late\"")) {
            /* the message as pi wrote it, its clock aside */
            const char *ts = strstr(mo.p, "\"timestamp\":");
            const char *te = ts ? ts + 12 : 0;
            while (te && *te >= '0' && *te <= '9')
                te++;
            CHECK(ts != 0);
            if (ts && (long)(ts - mo.p) + 13 + (mo.p + mo.n - te) < (long)sizeof(got)) {
                long pre = ts - mo.p, post = mo.p + mo.n - te;
                memcpy(got, mo.p, (size_t)pre);
                memcpy(got + pre, "\"timestamp\":0", 13);
                memcpy(got + pre + 13, te, (size_t)post);
                got[pre + 13 + post] = 0;
                if (strcmp(got, want)) {
                    printf("    declaration\n      got  %s\n      want %s\n", got, want);
                    CHECK(0);
                }
            }
            found++;
        }
    }
    CHECK_INT(found, 1);
    s3_free(&st);
}

/* PX5: gpt-5.5 takes system messages in the conversation and tool additions (supportsMidConvoSystemMessages,
 * supportsAdditionalTools). A tool added during the run: the second request keeps the initial tools at the top and
 * carries the late one as an additional_tools item of its input after the tool result; both requests' "input" and
 * "tools" are pi's (tests/pi/model/declare55_*.json, made by tools/pi_oracle/gen_declare.mjs from pi-agent-core
 * runAgentLoop and pi-ai's openai-responses). Through pi_app: the agent decides to send the transcript's system messages */
static void test_declare_mid_responses(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-5.5", 0 };
    static const char *const sse[] = { "../pi/responses/adder.sse", "../pi/responses/plain.sse" };
    static const char *const gold[] = { "declare55_first.json", "declare55_second.json" };
    char want[12000], got[12000], roles[200];
    int i;
    ready_hook = ready_adder;
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    ready_hook = 0;
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    for (i = 0; i < 2; i++) {
        jv b, in, tl;
        CHECK_INT(slurp_line(gold[i], want, sizeof(want)), 0);
        CHECK(json_parse(ps.body[i], (long)strlen(ps.body[i]), &b) == 0 && json_get(b, "input", &in) && json_get(b, "tools", &tl));
        if (in.n + tl.n + 30 < (long)sizeof(got)) {
            sprintf(got, "{\"input\":%.*s,\"tools\":%.*s}", (int)in.n, in.p, (int)tl.n, tl.p);
            if (strcmp(got, want)) {
                printf("    request %d\n      got  %s\n      want %s\n", i + 1, got, want);
                CHECK(0);
            }
        } else
            CHECK(0);
    }
    CHECK_INT(slurp_line("declare55_roles.txt", roles, sizeof(roles)), 0);
    CHECK(strstr(ps.body[1], "\"type\":\"additional_tools\"") != 0);
}

/* resume: pi restores the thinking level (and the model) from the session path (sdk.ts :253); E7 restored only the model */
static void test_resume(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0 };
    char *c[] = { "amiga-pi", "-c", 0 };
    char *c2[] = { "amiga-pi", "-c", "--thinking", "low", 0 };
    static const char *const sse[] = { "text.sse" };
    char path[600], sh[600];
    pi_s3 st;
    in_lines[0] = "/thinking max";
    in_lines[1] = "hello";
    in_lines[2] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    CHECK(strstr(ps.body[0], "\"effort\":\"max\"}") != 0);
    in_lines[0] = "again";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(c), c, sse, 1), 0);
    CHECK_INT(ps.nreq, 1);
    CHECK(ps.nreq && strstr(ps.body[0], "\"effort\":\"max\"}") != 0);      /* the level the session ended with */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 9);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    /* no new entry: the model and the level were the session's */
    CHECK_STR(sh, "model_change thinking_level_change thinking_level_change message/system message/user message/assistant "
                  "message/user message/assistant");
    s3_free(&st);
    /* an explicit --thinking wins, and is recorded as a change */
    in_lines[0] = "third";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(c2), c2, sse, 1), 0);
    CHECK(ps.nreq && strstr(ps.body[0], "\"effort\":\"low\"}") != 0);
}

/* /model errors and the list; --list-models; --thinking is clamped */
static void test_commands(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-haiku-4-5", "--thinking", "xhigh", 0 };
    char *lm[] = { "amiga-pi", "--list-models", "gpt", 0 };
    char *bad[] = { "amiga-pi", "--thinking", "loud", "-p", "x", 0 };
    static const char *const sse[] = { "text.sse" };
    in_lines[0] = "/model";
    in_lines[1] = "/model nothing-like-this";
    in_lines[2] = "/model 99";
    in_lines[3] = "/thinking";
    in_lines[4] = "/thinking loud";
    in_lines[5] = "/thinking max";
    in_lines[6] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 0), 0);
    CHECK(strstr(out.p, "* ") != 0 && strstr(out.p, "anthropic/claude-haiku-4-5") != 0);
    CHECK(strstr(out.p, "ollama/qwen2.5-coder:7b") != 0);                         /* a LAN endpoint needs no key */
    CHECK(strstr(out.p, "openai/gpt-5.5") != 0);                                  /* OPENAI_API_KEY is set */
    CHECK(strstr(out.p, "[ERROR] Model \"nothing-like-this\" not found.") != 0);
    CHECK(strstr(out.p, "[ERROR] no such number") != 0);
    CHECK(strstr(out.p, "Thinking level: high") != 0);                            /* haiku has no xhigh: clamped to high */
    CHECK(strstr(out.p, "Supported: off minimal low medium high") != 0);
    CHECK(strstr(out.p, "[ERROR] Invalid thinking level \"loud\"") != 0);
    CHECK(strstr(out.p, "[INFO] Thinking level: high") != 0);                     /* max clamps to high too */
    in_lines[0] = 0;
    CHECK_INT(run(ARGC(lm), lm, sse, 0), 0);
    CHECK(strstr(out.p, "openai/gpt-5.5  openai-responses  context 272000  thinking  images") != 0);
    CHECK(strstr(out.p, "claude") == 0);
    /* --thinking loud is a warning and the run goes on (pi cli/args.ts:169-176; oracle 1.1.0: Warning line, then the answer, rc 0) */
    CHECK_INT(run(ARGC(bad), bad, sse, 1), 0);
    CHECK(strstr(out.p, "Warning: Invalid thinking level \"loud\". Valid values: off, minimal, low, medium, high, xhigh, max") != 0);
}

/* models.json: a provider with `!command` key and a header, three requests, the command ran once */
static void test_command_once(void)
{
    char *a[] = { "amiga-pi", "--model", "keyed/m1", "--no-tools", 0 };
    static const char *const sse[] = { "../pi/openai_text.sse", "../pi/openai_text.sse", "../pi/openai_text.sse" };
    char cmd[900], text[1400], line[120];
    FILE *f;
    int n = 0, i;
    strcpy(marker, root);
    strcat(marker, "/cmd-runs");
    remove(marker);
    strcpy(cmd, "echo run >> ");
    strcat(cmd, marker);
    strcat(cmd, "; echo key-from-command");
    strcpy(text, "{\"providers\":{\"keyed\":{\"baseUrl\":\"https://public.example/v1\",\"api\":\"openai-completions\","
                 "\"apiKey\":\"!");
    strcat(text, cmd);
    strcat(text, "\",\"headers\":{\"X-Team\":\"$TEAMNAME\"},\"models\":[{\"id\":\"m1\",\"contextWindow\":50000}]}}}");
    write_file(agent, "models.json", text);
    in_lines[0] = "one";
    in_lines[1] = "two";
    in_lines[2] = "three";
    in_lines[3] = 0;
    /* $TEAMNAME is not set: the header cannot be resolved, the request says so and never goes out */
    CHECK_INT(run(ARGC(a), a, sse, 3), 0);
    CHECK_INT(ps.nreq, 0);
    CHECK(strstr(out.p, "a header value of models.json for keyed could not be resolved") != 0);
    strcpy(text, "{\"providers\":{\"keyed\":{\"baseUrl\":\"https://public.example/v1\",\"api\":\"openai-completions\","
                 "\"apiKey\":\"!");
    strcat(text, cmd);
    strcat(text, "\",\"headers\":{\"X-Team\":\"amiga\"},\"models\":[{\"id\":\"m1\",\"contextWindow\":50000}]}}}");
    write_file(agent, "models.json", text);
    remove(marker);
    CHECK_INT(run(ARGC(a), a, sse, 3), 0);
    CHECK_INT(ps.nreq, 3);
    for (i = 0; i < ps.nreq; i++)
        CHECK(strstr(ps.head[i], "Authorization: Bearer key-from-command\r\n") != 0 && strstr(ps.head[i], "X-Team: amiga\r\n") != 0);
    f = fopen(marker, "rb");
    if (f) {
        while (fgets(line, sizeof(line), f))
            n++;
        fclose(f);
    }
    CHECK_INT(n, 1);                    /* the command ran once for three requests */
    CHECK(strstr(ps.body[2], "\"model\":\"m1\"") != 0);
    /* PX4: pi's supportsStore (openai-completions detectCompat): a host not listed as non-standard gets "store":false */
    CHECK(strstr(ps.body[2], ",\"store\":false,") != 0);
    /* and models.json's compat.supportsStore false overrides it (pi's getCompat): no store member */
    strcpy(text, "{\"providers\":{\"keyed\":{\"baseUrl\":\"https://public.example/v1\",\"api\":\"openai-completions\","
                 "\"apiKey\":\"!");
    strcat(text, cmd);
    strcat(text, "\",\"headers\":{\"X-Team\":\"amiga\"},\"models\":[{\"id\":\"m1\",\"contextWindow\":50000,"
                 "\"compat\":{\"supportsStore\":false}}]}}}");
    write_file(agent, "models.json", text);
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    CHECK_INT(ps.nreq, 1);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "\"store\"") == 0);
    remove(marker);
    remove(strcat(strcpy(cmd, agent), "/models.json"));
}

/* FA6: a switch applies settings.json's defaultThinkingLevel before the session's own level (pi's
 * _getThinkingLevelForModelSwitch); a session file that cannot take the entry leaves the model and level as they were */
static void test_switch_level(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", "--thinking", "high", 0 };
    static const char *const sse[] = { "text.sse" };
    char path[600], sh[600], v[120];
    pi_s3 st;
    write_file(agent, "settings.json", "{\"defaultThinkingLevel\":\"low\"}");
    in_lines[0] = "hello";                      /* a session file exists once there is an answer */
    in_lines[1] = "/model anthropic/claude-sonnet-5-5";
    in_lines[2] = "/session";
    in_lines[3] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    CHECK(strstr(out.p, "[INFO] Model: anthropic/claude-sonnet-5-5, thinking low") != 0);     /* not the session's high */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_STR(sh, "model_change thinking_level_change message/system message/user message/assistant model_change "
                  "thinking_level_change");
    field(&st, 6, "thinkingLevel", v, sizeof(v));
    CHECK_STR(v, "low");
    s3_free(&st);
    /* the session file turns read-only: the switch says so and /session still names the first model and level */
    in_lines[0] = "hello";
    in_lines[1] = "/model anthropic/claude-sonnet-5-5";
    in_lines[2] = "/session";
    in_lines[3] = 0;
    ro_at = 1;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    ro_at = -1;
    CHECK(strstr(out.p, "[ERROR] the model change could not be recorded") != 0);
    CHECK(strstr(out.p, "\nmodel anthropic/claude-opus-5-5\nthinking high") != 0);
    CHECK(strstr(out.p, "[INFO] Model: ") == 0);
    if (!newest(path, sizeof(path)))
        chmod(path, 0644);
    remove(strcat(strcpy(marker, agent), "/settings.json"));
}

/* a broken models.json is a warning, the catalog stays */
static void test_bad_models_json(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", "-p", "hi", 0 };
    static const char *const sse[] = { "text.sse" };
    write_file(agent, "models.json", "{\"providers\":{\"x\":{\"models\":[{\"id\":\"m\"}]}}}");
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    CHECK(strstr(out.p, "[WARN] Provider x, model m: no \"api\" specified. Set at provider or model level. (models.json is ignored)") != 0);
    CHECK_INT(ps.nreq, 1);
    remove(strcat(strcpy(marker, agent), "/models.json"));
}

/* PHS7: compat.allowedFallbackModels through pi_app. The request names the fallbacks (the beta header, "fallbacks" last);
 * the server answers with claude-fb (tests/claude/fallback_model.sse): the answer's cost is claude-fb's price (output 20 per
 * million, the model's own is 15), its `model` stays the configured id and responseModel names the server's */
static void test_fallback_pricing(void)
{
    char *a[] = { "amiga-pi", "--model", "fb/claude-x", "--mode", "json", "hi", 0 };
    static const char *const sse[] = { "fallback_model.sse" };
    write_file(agent, "models.json",
               "{\"providers\":{\"fb\":{\"baseUrl\":\"https://fb.example\",\"api\":\"anthropic-messages\",\"apiKey\":\"k\","
               "\"compat\":{\"allowedFallbackModels\":[{\"provider\":\"fb\",\"model\":\"claude-fb\","
               "\"cost\":{\"input\":10,\"output\":20,\"cacheRead\":1,\"cacheWrite\":2}}]},"
               "\"models\":[{\"id\":\"claude-x\",\"cost\":{\"input\":3,\"output\":15,\"cacheRead\":0.3,\"cacheWrite\":3.75}}]}}}");
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    CHECK_INT(ps.nreq, 1);
    CHECK(ps.nreq && strstr(ps.head[0], "server-side-fallback-2026-07-01") != 0);
    CHECK(ps.nreq && ps.body[0] && strstr(ps.body[0], ",\"fallbacks\":[{\"model\":\"claude-fb\"}]}") != 0);
    CHECK(out.p && strstr(out.p, "\"model\":\"claude-x\"") != 0 && strstr(out.p, "\"responseModel\":\"claude-fb\"") != 0);
    CHECK(out.p && strstr(out.p, "\"cost\":{\"input\":0,\"output\":20,\"cacheRead\":0,\"cacheWrite\":0,\"total\":20}") != 0);
    remove(strcat(strcpy(marker, agent), "/models.json"));
}

/* M8: google-generative-ai through pi_app. Prompt one: the recorded answer is one read call whose id the adapter
 * generates (<name>_<ms>_<counter>, from the app's clock); the second request carries it in the functionCall and the
 * functionResponse (gemini 3 needs ids). The key comes from GEMINI_API_KEY, goes in x-goog-api-key; the sentinel counts
 * the backend's decoder. */
static void test_google_reach(void)
{
    char *a[] = { "amiga-pi", "--model", "google/gemini-3-flash-preview", 0 };
    static const char *const sse[] = { "../pi/google/read1.sse", "../pi/google/plain.sse" };
    long f0 = llm_feeds_google;
    const char *id;
    llm_google_counter(0);
    extra_name[0] = "GEMINI_API_KEY";
    extra_val[0] = "gm-test-key";
    in_lines[0] = "read the file";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    extra_name[0] = 0;
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    CHECK(strstr(ps.head[0], "POST /v1beta/models/gemini-3-flash-preview:streamGenerateContent?alt=sse HTTP/1.1\r\n"
                             "Host: generativelanguage.googleapis.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "x-goog-api-key: gm-test-key\r\n") != 0 && strstr(ps.head[0], "Authorization") == 0);
    CHECK(strstr(ps.body[0], "{\"contents\":[{\"parts\":[{\"text\":\"read the file\"}],\"role\":\"user\"}],\"systemInstruction\":") == ps.body[0]);
    /* the generated id, then the same id in the call and its result: <name>_<seconds><ms>_<counter> */
    id = strstr(ps.body[1], "\"functionCall\":{\"args\":{\"path\":\"/work/proj/no-such-file.txt\"},\"id\":\"read_17601");
    CHECK(id != 0);
    if (id) {
        char want[160];
        const char *q = strstr(id, "\"id\":\"") + 6, *e = strchr(q, '"');
        long n = (long)(e - q);
        CHECK(n > 10 && n < 60);
        strcpy(want, "\"functionResponse\":{\"name\":\"read\",\"response\":{\"error\":");
        CHECK(strstr(ps.body[1], want) != 0);
        strcpy(want, "\"id\":\"");
        strncat(want, q, (size_t)n);
        strcat(want, "\"}}");
        CHECK(strstr(ps.body[1], want) != 0);           /* the result carries the call's id */
        CHECK(n > 4 && !strncmp(e - 4, "07_1", 4));       /* the clock's ms (7), counter 1 */
    }
    CHECK(out.p && strstr(out.p, "Hello from the Amiga.") != 0);
    CHECK(llm_feeds_google > f0);
    CHECK(strstr(ps.head[0], "x-goog-api-client: google-genai-sdk/2.21.0 gl-node/v24.10.0\r\n") != 0);
    /* M10: settings.json's thinkingBudgets replace the level's own budget (gemini 2.5 asks a budget: medium is 8192) */
    {
        char *b[] = { "amiga-pi", "--model", "google/gemini-2.5-flash", "--thinking", "medium", 0 };
        static const char *const one[] = { "../pi/google/plain.sse" };
        extra_name[0] = "GEMINI_API_KEY";
        extra_val[0] = "gm-test-key";
        in_lines[0] = "hello";
        in_lines[1] = 0;
        CHECK_INT(run(ARGC(b), b, one, 1), 0);
        CHECK(ps.nreq >= 1 && strstr(ps.body[0], "\"thinkingBudget\":8192") != 0);
        write_file(agent, "settings.json", "{\"thinkingBudgets\":{\"medium\":1234,\"high\":99}}");
        in_lines[0] = "hello";
        in_lines[1] = 0;
        CHECK_INT(run(ARGC(b), b, one, 1), 0);
        CHECK(ps.nreq >= 1 && strstr(ps.body[0], "\"thinkingBudget\":1234") != 0);
        remove(strcat(strcpy(marker, agent), "/settings.json"));
        extra_name[0] = 0;
    }
}

/* M8: azure-openai-responses through pi_app. The endpoint comes from the environment (the catalog has no base URL):
 * AZURE_OPENAI_RESOURCE_NAME, the deployment from AZURE_OPENAI_DEPLOYMENT_NAME_MAP; the key from AZURE_OPENAI_API_KEY
 * goes in an api-key header, never Authorization; the sentinel counts the backend's decoder. */
static void test_azure_reach(void)
{
    char *a[] = { "amiga-pi", "--model", "azure/gpt-4.1", 0 };
    static const char *const sse[] = { "../pi/responses/plain.sse" };
    long f0 = llm_feeds_azure, r0 = llm_feeds_responses;
    extra_name[0] = "AZURE_OPENAI_API_KEY";
    extra_val[0] = "az-test-key";
    extra_name[1] = "AZURE_OPENAI_RESOURCE_NAME";
    extra_val[1] = "contoso";
    extra_name[2] = "AZURE_OPENAI_DEPLOYMENT_NAME_MAP";
    extra_val[2] = "gpt-4.1=prod-41";
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    extra_name[0] = extra_name[1] = extra_name[2] = 0;
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq < 1)
        return;
    CHECK(strstr(ps.head[0], "POST /openai/v1/responses?api-version=v1 HTTP/1.1\r\nHost: contoso.openai.azure.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "api-key: az-test-key\r\n") != 0 && strstr(ps.head[0], "Authorization") == 0);
    CHECK(strstr(ps.body[0], "{\"model\":\"prod-41\",\"input\":[") == ps.body[0]);
    CHECK(out.p && strstr(out.p, "Hello") != 0);
    CHECK(llm_feeds_azure > f0 && llm_feeds_responses == r0);       /* its own backend ran, not openai-responses */
}

/* without a base URL in the catalog or the environment the request names what pi names */
static void test_azure_no_endpoint(void)
{
    char *a[] = { "amiga-pi", "--model", "azure/gpt-4.1", 0 };
    static const char *const sse[] = { "../pi/responses/plain.sse" };
    extra_name[0] = "AZURE_OPENAI_API_KEY";
    extra_val[0] = "az-test-key";
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run(ARGC(a), a, sse, 1);
    extra_name[0] = 0;
    CHECK_INT(ps.nreq, 0);
    CHECK(out.p && strstr(out.p, "Azure OpenAI base URL is required. Set AZURE_OPENAI_BASE_URL or AZURE_OPENAI_RESOURCE_NAME") != 0);
}

/* M8: bedrock-converse-stream through pi_app. The credentials come from the environment (AWS_ACCESS_KEY_ID and
 * AWS_SECRET_ACCESS_KEY, AWS_REGION): the request is signed with Signature Version 4 for the region's endpoint and carries
 * no Authorization: Bearer. Prompt one: the recorded event stream (tests/pi/bedrock/tools2.es) holds two read calls whose
 * ids the API gave; the second request carries them in toolUse and toolResult blocks, one user message holding both
 * results. The answer to that is a text (plain.es). The sentinel counts the backend's decoder. */
static void test_bedrock_reach(void)
{
    char *a[] = { "amiga-pi", "--model", "amazon-bedrock/anthropic.claude-sonnet-5-5", 0 };
    static const char *const sse[] = { "../pi/bedrock/tools2.es", "../pi/bedrock/plain.es" };
    long f0 = llm_feeds_bedrock;
    extra_name[0] = "AWS_ACCESS_KEY_ID";
    extra_val[0] = "AKIDEXAMPLE";
    extra_name[1] = "AWS_SECRET_ACCESS_KEY";
    extra_val[1] = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    extra_name[2] = "AWS_REGION";
    extra_val[2] = "eu-west-1";
    in_lines[0] = "show me the startup sequence";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    extra_name[0] = extra_name[1] = extra_name[2] = 0;
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    CHECK(strstr(ps.head[0], "POST /model/anthropic.claude-sonnet-5-5/converse-stream HTTP/1.1\r\n"
                             "Host: bedrock-runtime.eu-west-1.amazonaws.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "Authorization: AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/") != 0 &&
          strstr(ps.head[0], "/eu-west-1/bedrock/aws4_request, SignedHeaders=content-type;host;x-amz-date, Signature=") != 0);
    CHECK(strstr(ps.head[0], "x-amz-date: 20251010T1") != 0 && strstr(ps.head[0], "Bearer") == 0);
    CHECK(strstr(ps.head[0], "wJalrXUtnFEMI") == 0 && strstr(ps.body[0], "wJalrXUtnFEMI") == 0);
    CHECK(strstr(ps.body[0], "{\"messages\":[{\"role\":\"user\",\"content\":[{\"text\":\"show me the startup sequence\"},"
                             "{\"cachePoint\":{\"type\":\"default\"}}]}],\"system\":[{\"text\":") == ps.body[0]);
    /* the ids the API gave, in the call and in the results; both results in one user message */
    CHECK(strstr(ps.body[1], "{\"toolUse\":{\"toolUseId\":\"tooluse_abc123\",\"name\":\"read\",\"input\":{\"path\":\"/work/a.txt\"}}}") != 0);
    CHECK(strstr(ps.body[1], "{\"toolUse\":{\"toolUseId\":\"tooluse_def456\",\"name\":\"read\"") != 0);
    CHECK(strstr(ps.body[1], "{\"role\":\"user\",\"content\":[{\"toolResult\":{\"toolUseId\":\"tooluse_abc123\",\"content\":[{\"text\":") != 0);
    CHECK(strstr(ps.body[1], "\"status\":\"error\"}},{\"toolResult\":{\"toolUseId\":\"tooluse_def456\"") != 0);
    CHECK(out.p && strstr(out.p, "Hello from the Amiga.") != 0);
    CHECK(llm_feeds_bedrock > f0);
}

/* openai-codex-responses through pi_app (the ChatGPT subscription's endpoint): the key (here the environment's, as a JWT whose
 * claim names the account; an OAuth login in auth.json gives the same) goes out as Authorization: Bearer with the account
 * id and pi's originator header, to POST /backend-api/codex/responses on chatgpt.com; the Responses events of the answer
 * are decoded (the Codex backend's sentinel, not openai-responses'). Its HTTP errors read as pi's parseErrorResponse
 * words them (llm_be_openai_codex http_err, host goldens in vtcon tests/pi/httperr). */
static void test_codex_reach(void)
{
    static const char jwt[] = "eyJhbGciOiJub25lIn0.eyJodHRwczovL2FwaS5vcGVuYWkuY29tL2F1dGgiOnsiY2hhdGdwdF9hY2NvdW50X2lkIjoiYWNjdF9yaWcifX0.sig";
    char *a[] = { "amiga-pi", "--model", "openai-codex/gpt-5.3-codex-spark", 0 };
    static const char *const sse[] = { "../pi/responses/plain.sse" };
    long f0 = llm_feeds_codex, r0 = llm_feeds_responses;
    extra_name[0] = "OPENAI_CODEX_API_KEY";
    extra_val[0] = jwt;
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    extra_name[0] = 0;
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq < 1)
        return;
    CHECK(strstr(ps.head[0], "POST /backend-api/codex/responses HTTP/1.1\r\nHost: chatgpt.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "Authorization: Bearer eyJhbGciOiJub25lIn0.") != 0);
    CHECK(strstr(ps.head[0], "chatgpt-account-id: acct_rig\r\n") != 0 && strstr(ps.head[0], "originator: pi\r\n") != 0);
    CHECK(strstr(ps.body[0], "\"store\":false,\"stream\":true") != 0);
    CHECK(out.p && strstr(out.p, "Hello from the Amiga.") != 0);
    CHECK(llm_feeds_codex > f0 && llm_feeds_responses == r0);       /* its own backend ran, not openai-responses */
}

/* M8: a bearer token (AWS_BEARER_TOKEN_BEDROCK) goes in Authorization and nothing is signed, even with access keys set */
static void test_bedrock_bearer(void)
{
    char *a[] = { "amiga-pi", "--model", "amazon-bedrock/amazon.nova-pro-v1:0", 0 };
    static const char *const sse[] = { "../pi/bedrock/plain.es" };
    extra_name[0] = "AWS_BEARER_TOKEN_BEDROCK";
    extra_val[0] = "brk-test-token-not-real";
    extra_name[1] = "AWS_ACCESS_KEY_ID";
    extra_val[1] = "AKIDEXAMPLE";
    extra_name[2] = "AWS_SECRET_ACCESS_KEY";
    extra_val[2] = "secret";
    in_lines[0] = "hello";
    in_lines[1] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    extra_name[0] = extra_name[1] = extra_name[2] = 0;
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq < 1)
        return;
    CHECK(strstr(ps.head[0], "POST /model/amazon.nova-pro-v1%3A0/converse-stream HTTP/1.1\r\n"
                             "Host: bedrock-runtime.us-east-1.amazonaws.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "Authorization: Bearer brk-test-token-not-real\r\n") != 0 && strstr(ps.head[0], "x-amz-date") == 0);
    CHECK(out.p && strstr(out.p, "Hello from the Amiga.") != 0);
}

/* no credential at all: the model is not usable (pi's checkAuth); a profile alone is (pi counts it) and the request says
 * this build reads no ~/.aws */
static void test_bedrock_no_credentials(void)
{
    char *a[] = { "amiga-pi", "--model", "amazon-bedrock/anthropic.claude-sonnet-5-5", 0 };
    static const char *const sse[] = { "../pi/bedrock/plain.es" };
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run(ARGC(a), a, sse, 1);
    CHECK_INT(ps.nreq, 0);
    extra_name[0] = "AWS_PROFILE";
    extra_val[0] = "work";
    extra_name[1] = "AWS_REGION";
    extra_val[1] = "eu-west-1";
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run(ARGC(a), a, sse, 1);
    extra_name[0] = extra_name[1] = 0;
    CHECK_INT(ps.nreq, 0);
    CHECK(out.p && strstr(out.p, "No Amazon Bedrock credentials amiga-pi can use") != 0);
}

/* G4: no credential at all for the model (no ANTHROPIC_API_KEY, no auth.json, no login): pi's prompt() stops before anything is
 * kept or sent, with formatNoApiKeyFoundMessage (agent-session.ts:2036-2050, auth-guidance.ts:22-25) -- not a connection
 * attempt that ends in "Connection error." A LAN endpoint on plain http needs none and still goes out. */
static void test_no_credentials(void)
{
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0 };
    static const char *const sse[] = { "../pi/switch_anthropic.sse" };
    long p0 = pi_agent_prompts;
    in_lines[0] = "hello";
    in_lines[1] = 0;
    bare_env = 1;
    run(ARGC(a), a, sse, 1);
    bare_env = 0;
    CHECK_INT(ps.nreq, 0);
    CHECK(pi_agent_prompts > p0);                       /* the prompt did reach the agent */
    CHECK(out.p && strstr(out.p, "No API key found for anthropic.\n\nUse /login to log into a provider via OAuth or API key. See:") != 0);
    CHECK(out.p && strstr(out.p, "docs/providers.md") != 0 && strstr(out.p, "Connection error") == 0);
    /* with the key it goes out */
    extra_name[0] = "ANTHROPIC_API_KEY";
    extra_val[0] = "sk-ant-test";
    in_lines[0] = "hello";
    in_lines[1] = 0;
    run(ARGC(a), a, sse, 1);
    extra_name[0] = 0;
    CHECK_INT(ps.nreq, 1);
}

void suite_pi_model_reach(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_model_XXXXXX");
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
    test_switch();
    test_switch_back();
    test_levels();
    test_declare();
    test_declare_every_model();
    test_declare_mid_responses();
    test_clamp_first();
    test_resume();
    test_commands();
    test_command_once();
    test_switch_level();
    test_bad_models_json();
    test_fallback_pricing();
    test_google_reach();
    test_azure_reach();
    test_azure_no_endpoint();
    test_bedrock_reach();
    test_bedrock_bearer();
    test_bedrock_no_credentials();
    test_codex_reach();
    test_no_credentials();
    jw_free(&out);
}
