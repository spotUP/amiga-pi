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

static long env(const char *name, char *o, long cap)
{
    const char *v = 0;
    if (!strcmp(name, "OPENAI_API_KEY"))
        v = "sk-test-openai";
    else if (!strcmp(name, "ANTHROPIC_API_KEY"))
        v = "sk-ant-test-not-real";
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

/* an Anthropic answer with thinking text, as the server sends it; the model it names is a dated snapshot */
static const char *const anth_parts[] = {
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
    "\"model\":\"claude-opus-5-5-20260101\",\"content\":[],\"stop_reason\":null,\"usage\":{\"input_tokens\":25,"
    "\"cache_creation_input_tokens\":1200,\"cache_read_input_tokens\":0,\"output_tokens\":1}}}\n\n",
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"\"}}\n\n",
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Check the greeting first.\"}}\n\n",
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"EqQBsig==\"}}\n\n",
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n",
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n",
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi there.\"}}\n\n",
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n",
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},"
    "\"usage\":{\"output_tokens\":9}}\n\n",
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n\n",
    0
};
static char anth_thinking[2200];
static void build_anth(void)
{
    int i;
    anth_thinking[0] = 0;
    for (i = 0; anth_parts[i]; i++)
        strcat(anth_thinking, anth_parts[i]);
}
static int first_text;          /* queue anth_thinking before the recordings */

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
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = root;
    pl.utc = 1;
    pl.data_dir = data;
    pl.agent_dir = agent;
    if (first_text)
        {
        build_anth();
        ps_sse_text(anth_thinking, (long)strlen(anth_thinking), 0);
    }
    for (i = 0; i < nsse; i++)
        ps_sse(sse[i], 0);
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
    static const char *const sse[] = { "../pi/openai_text.sse" };
    char path[600], sh[600], v[120];
    pi_s3 st;
    long f_open = llm_feeds_openai, f_anth = llm_feeds_anthropic;
    in_lines[0] = "hello";
    in_lines[1] = "/model openai/gpt-4.1";
    in_lines[2] = "again";
    in_lines[3] = "/model anthropic/claude-opus-5-5";
    in_lines[4] = "/thinking high";
    in_lines[5] = "/session";
    in_lines[6] = 0;
    first_text = 1;
    CHECK_INT(run(ARGC(a), a, sse, 1), 0);
    first_text = 0;
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    /* the first request: Anthropic, adaptive thinking at pi's default level */
    CHECK(strstr(ps.head[0], "POST /v1/messages HTTP/1.1\r\nHost: api.anthropic.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[0], "x-api-key: sk-ant-test-not-real\r\n") != 0);
    CHECK(strstr(ps.body[0], "\"thinking\":{\"type\":\"adaptive\",\"display\":\"summarized\"},\"output_config\":{\"effort\":\"medium\"}") != 0);
    /* the second: OpenAI, the first answer's thinking as plain text (pi's transformMessages), the key of the environment */
    CHECK(strstr(ps.head[1], "POST /v1/chat/completions HTTP/1.1\r\nHost: api.openai.com\r\n") == ps.head[1]);
    CHECK(strstr(ps.head[1], "Authorization: Bearer sk-test-openai\r\n") != 0);
    CHECK(strstr(ps.body[1], "\"model\":\"gpt-4.1\"") != 0);
    CHECK(strstr(ps.body[1], "\"thinking\"") == 0 && strstr(ps.body[1], "reasoning_effort") == 0);
    pi_golden("pi/model/switch_second.json", ps.body[1] ? ps.body[1] : "");
    CHECK(llm_feeds_anthropic > f_anth && llm_feeds_openai > f_open);       /* both backends ran */
    /* the file: pi's entries for the switch; gpt-4.1 has no reasoning, so the level fell to off; back on an
     * Anthropic model it stays off (pi keeps the level it has: no entry) until /thinking high records one */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_STR(sh, "model_change thinking_level_change message/system message/user message/assistant "
                  "model_change thinking_level_change message/user message/assistant model_change "
                  "thinking_level_change");
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

/* the other way: OpenAI first, then an Anthropic model; the level stays what the first had (off), so thinking is disabled */
static void test_switch_back(void)
{
    char *a[] = { "amiga-pi", "--model", "openai/gpt-4.1", "--no-session", 0 };
    static const char *const sse[] = { "../pi/openai_text.sse", "text.sse" };
    long f_open = llm_feeds_openai, f_anth = llm_feeds_anthropic;
    in_lines[0] = "hello";
    in_lines[1] = "/model anthropic/claude-opus-5-5";
    in_lines[2] = "again";
    in_lines[3] = 0;
    CHECK_INT(run(ARGC(a), a, sse, 2), 0);
    CHECK_INT(ps.nreq, 2);
    if (ps.nreq < 2)
        return;
    CHECK(strstr(ps.head[0], "POST /v1/chat/completions HTTP/1.1\r\nHost: api.openai.com\r\n") == ps.head[0]);
    CHECK(strstr(ps.head[1], "POST /v1/messages HTTP/1.1\r\nHost: api.anthropic.com\r\n") == ps.head[1]);
    CHECK(strstr(ps.body[1], "\"thinking\":{\"type\":\"disabled\"}") != 0);
    pi_golden("pi/model/switch_back_second.json", ps.body[1] ? ps.body[1] : "");
    CHECK(llm_feeds_anthropic > f_anth && llm_feeds_openai > f_open);
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
    CHECK(strstr(out.p, "openai/gpt-5.5  openai-completions  context 400000  thinking  images") != 0);
    CHECK(strstr(out.p, "claude") == 0);
    CHECK_INT(run(ARGC(bad), bad, sse, 0), 2);
    CHECK(strstr(out.p, "Invalid thinking level \"loud\"") != 0);
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
    remove(marker);
    remove(strcat(strcpy(cmd, agent), "/models.json"));
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
    test_resume();
    test_commands();
    test_command_once();
    test_bad_models_json();
    jw_free(&out);
}
