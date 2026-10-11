/* amiga-pi's agent loop on the host (plan phase C): pi_prompt() against the
 * stub transport (vtcon tests/pi_stub.c) with the phase B recordings, the
 * events in pi's order, the extension dispatcher, the system prompt, the
 * prompt on C:Claude's screen code, the command line. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pi_test.h"
#include "claude/util.h"
#include "pi_stub.h"
#include "claude_screen.h"
#include "llm/transform.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/prompt.h"
#include "../amiga-pi/ui_pi.h"
#include "../amiga-pi/app.h"
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

/* ---- C5: the test-only tool, under the names the recordings call ---- */

static long echo_calls;
static char echo_last[200];
static int echo_brk_at_second;  /* the second call arms the stub's break (the abort test) */

static int echo_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    jw t;
    (void)u;
    (void)s;
    echo_calls++;
    cl_copy(echo_last, args, sizeof(echo_last));
    if (an < (long)sizeof(echo_last))
        echo_last[an] = 0;
    if (echo_brk_at_second && echo_calls == 2)
        ps.brk_at = 300;
    jw_init(&t);
    jw_rawz(&t, "echo ");
    jw_raw(&t, args, an);
    jw_rawz(content, "[");
    llm_b_text(content, t.p, t.n, 0);
    jw_rawz(content, "]");
    jw_free(&t);
    *is_error = 0;
    return 0;
}

#define ECHO_PARAMS "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}"
static pi_tool t_read = { "read", "Read a file", ECHO_PARAMS, "Read file contents", 0, echo_run, 0 };
static pi_tool t_ls = { "ls", "List a directory", ECHO_PARAMS, 0, 0, echo_run, 0 };
static pi_tool t_Read = { "Read", "Read a file", ECHO_PARAMS, 0, 0, echo_run, 0 };
static pi_tool t_Glob = { "Glob", "Find files", ECHO_PARAMS, 0, 0, echo_run, 0 };

/* ---- the session over the stub ---- */

static llm_ts fixed_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L;
    t.ms = 0;
    return t;
}

static llm_model model;
static pi_session ses;

static void up(int anthropic, int tools)
{
    llm_hooks h;
    ps_reset();
    ps_pool_up(LLM_POOL);
    memset(&h, 0, sizeof(h));
    h.poll = ps_poll;
    h.ms = ps_ms;
    h.sleep = ps_sleep;
    h.notice = ps_notice;
    h.resolve = ps_resolve;
    h.log = ps_log;
    model = anthropic ? ps_model_anthropic() : ps_model_openai("http://192.168.1.10:11434/v1");
    pi_session_init(&ses, &model, anthropic ? "sk-ant-test-not-real" : 0, &ps_pool, &h);
    ses.now = fixed_now;
    ses.system = "You are amiga-pi.";
    if (tools) {
        pi_tool_add(&ses, &t_read);
        pi_tool_add(&ses, &t_ls);
        pi_tool_add(&ses, &t_Read);
        pi_tool_add(&ses, &t_Glob);
    }
    echo_calls = 0;
    echo_last[0] = 0;
    echo_brk_at_second = 0;
}

static void down(void)
{
    pi_session_free(&ses);
}

/* "user assistant(toolCall toolCall) toolResult ..." */
static void shape(const pi_session *s, char *out, long cap)
{
    int i;
    out[0] = 0;
    for (i = 0; i < s->nmsg; i++) {
        llm_info inf;
        static const char *const roles[] = { "user", "assistant", "toolResult", "system" };
        int r = llm_msg_info(s->msg[i], s->msg_n[i], &inf);
        if (i)
            cl_cat(out, " ", cap);
        cl_cat(out, r >= 0 && r <= 3 ? roles[r] : "?", cap);
        if (r == LR_ASSISTANT) {
            char n[4];
            cl_cat(out, "(", cap);
            cl_cat(out, llm_stop_name(inf.stop), cap);
            cl_cat(out, ",", cap);
            n[0] = (char)('0' + inf.ntools % 10);
            n[1] = 0;
            cl_cat(out, n, cap);
            cl_cat(out, ")", cap);
        }
    }
}

static long count(const char *hay, const char *needle)
{
    long n = 0;
    const char *p = hay;
    while (p && (p = strstr(p, needle)) != 0) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/* ---- the event recorder (C2) and the heap watch (C12) ---- */

static jw evlog;
static long updates;
static long heap0, heap_peak;

static long heap_now(void)
{
#ifdef __APPLE__
    malloc_statistics_t st;
    malloc_zone_statistics(0, &st);
    return (long)st.size_in_use;
#else
    return 0;
#endif
}

static void recorder(void *u, const pi_event *e)
{
    long h = heap_now() - heap0;
    static const char *const roles[] = { "user", "assistant", "toolResult", "system" };
    (void)u;
    if (h > heap_peak)
        heap_peak = h;
    if (e->type == PE_MESSAGE_UPDATE) {
        updates++;
        return;
    }
    jw_rawz(&evlog, pi_event_name(e->type));
    if (e->type == PE_MESSAGE_START || e->type == PE_MESSAGE_END) {
        jw_rawz(&evlog, " ");
        jw_rawz(&evlog, e->role >= 0 && e->role <= 3 ? roles[e->role] : "?");
    }
    if (e->type == PE_TOOL_START || e->type == PE_TOOL_END) {
        jw_rawz(&evlog, " ");
        jw_rawz(&evlog, e->tool_name);
        if (e->type == PE_TOOL_END && e->is_error)
            jw_rawz(&evlog, " error");
    }
    jw_rawz(&evlog, "\n");
}

static void record(void)
{
    jw_free(&evlog);
    jw_init(&evlog);
    updates = 0;
    heap0 = heap_now();
    heap_peak = 0;
    pi_subscribe(&ses.em, recorder, 0);
}

/* ---- C8, C2, C12: the reachability test, OpenAI ---- */

static void test_reach_openai(void)
{
    long r0 = pi_agent_rounds;
    char sh[300];
    up(0, 1);
    record();
    ps_sse("../pi/openai_tools.sse", 0);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "show me S/Startup-Sequence"), LS_STOP);
    CHECK_INT(pi_agent_rounds - r0, 2);
    CHECK_INT(echo_calls, 2);
    CHECK_INT(t_read.calls + t_ls.calls >= 2, 1);
    shape(&ses, sh, sizeof(sh));
    CHECK_STR(sh, "user assistant(toolUse,2) toolResult toolResult assistant(stop,0)");
    CHECK_INT(ps.nreq, 2);
    /* round 2 carries both results, in call order */
    CHECK(ps.nreq == 2 && count(ps.body[1], "\"role\":\"tool\"") == 2);
    CHECK(ps.nreq == 2 && strstr(ps.body[1], "\"tool_call_id\":\"call_ReadStartup\"") <
                              strstr(ps.body[1], "\"tool_call_id\":\"call_ListS\""));
    CHECK(ps.nreq == 2 && strstr(ps.body[0], "\"tools\":[{\"type\":\"function\"") != 0);
    CHECK(llm_feeds_openai > 0);
    pi_golden("pi_agent/events_two_rounds.txt", evlog.p ? evlog.p : "");
    CHECK(updates > 0);
    printf("  [INFO] two-round turn: peak heap %ld bytes above the start (host, malloc_zone_statistics)\n",
           heap_peak);
    down();
}

/* ---- C9: the same turn on Anthropic ---- */

static void test_reach_anthropic(void)
{
    long r0 = pi_agent_rounds, f0 = llm_feeds_anthropic;
    char sh[300];
    up(1, 1);
    record();
    ps_sse("tool_use.sse", 0);
    ps_sse("tool_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "show me S/Startup-Sequence"), LS_STOP);
    CHECK_INT(pi_agent_rounds - r0, 2);
    CHECK_INT(echo_calls, 2);
    CHECK(llm_feeds_anthropic > f0);
    shape(&ses, sh, sizeof(sh));
    CHECK_STR(sh, "user assistant(toolUse,2) toolResult toolResult assistant(stop,0)");
    CHECK(ps.nreq == 2 && strstr(ps.head[0], "x-api-key: sk-ant-test-not-real\r\n") != 0);
    CHECK(ps.nreq == 2 && count(ps.body[1], "\"type\":\"tool_result\"") == 2);
    pi_golden("pi_agent/events_two_rounds_anthropic.txt", evlog.p ? evlog.p : "");
    {
        jw t;
        jw_init(&t);
        pi_last_text(&ses, &t);
        CHECK_STR(t.p ? t.p : "", "Your Startup-Sequence runs SetPatch first.");
        jw_free(&t);
    }
    down();
}

/* ---- C1: the stop reasons ---- */

#define CH(delta, fr) "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"m\"," \
                      "\"choices\":[{\"index\":0,\"delta\":" delta ",\"finish_reason\":" fr "}]}\n\n"

/* SSE from its parts (C90 caps a string literal at 509 bytes) */
static void sse_text(const char *const *parts)
{
    jw w;
    int i;
    jw_init(&w);
    for (i = 0; parts[i]; i++)
        jw_rawz(&w, parts[i]);
    ps_sse_text(w.p, w.n, 0);
    jw_free(&w);
}

static void test_stops(void)
{
    long r0;
    char sh[300];
    static const char *const length_sse[] = {
        CH("{\"role\":\"assistant\",\"content\":\"Reading.\"}", "null"),
        CH("{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"read\","
           "\"arguments\":\"{\\\"path\\\":\\\"S:\"}}]}", "null"),
        CH("{}", "\"length\""), "data: [DONE]\n\n", 0 };
    static const char *const filter_sse[] = {
        CH("{\"role\":\"assistant\",\"content\":\"No\"}", "null"), CH("{}", "\"content_filter\""),
        "data: [DONE]\n\n", 0 };
    static const char *const unknown_sse[] = {
        CH("{\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":\"rm\","
           "\"arguments\":\"{}\"}}]}", "null"),
        CH("{}", "\"tool_calls\""), "data: [DONE]\n\n", 0 };

    /* stop: one round, no tools */
    up(0, 1);
    r0 = pi_agent_rounds;
    ps_sse("../pi/openai_text.sse", 0);
    CHECK_INT(pi_prompt(&ses, "hello"), LS_STOP);
    CHECK_INT(pi_agent_rounds - r0, 1);
    CHECK_INT(echo_calls, 0);
    down();

    /* length: the cut call is not run, the model is told so, the loop goes on */
    up(0, 1);
    sse_text(length_sse);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "read it"), LS_STOP);
    CHECK_INT(echo_calls, 0);
    shape(&ses, sh, sizeof(sh));
    CHECK_STR(sh, "user assistant(length,1) toolResult assistant(stop,0)");
    CHECK(ses.nmsg > 2 && strstr(ses.msg[2], "Tool call \\\"read\\\" was not executed: the response hit the output "
                                             "token limit") != 0);
    CHECK(ses.nmsg > 2 && strstr(ses.msg[2], "\"isError\":true") != 0);
    down();

    /* error: the prompt ends with the provider's reason */
    up(0, 1);
    r0 = pi_agent_rounds;
    record();
    sse_text(filter_sse);
    CHECK_INT(pi_prompt(&ses, "hello"), LS_ERROR);
    CHECK_INT(pi_agent_rounds - r0, 1);
    CHECK_STR(ses.err, "Provider finish_reason: content_filter");
    CHECK(evlog.p && strstr(evlog.p, "message_end assistant\nturn_end\nagent_end\n") != 0);
    down();

    /* a tool nobody registered */
    up(0, 1);
    sse_text(unknown_sse);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "remove it"), LS_STOP);
    CHECK(ses.nmsg > 2 && strstr(ses.msg[2], "\"text\":\"Tool rm not found\"") != 0);
    down();
}

/* ---- C11: Ctrl+C in round 2 ---- */

static void test_abort(void)
{
    char sh[300];
    up(0, 1);
    echo_brk_at_second = 1;     /* the second tool call arms a break 300 bytes into the next answer */
    ps_sse("../pi/openai_tools.sse", 0);
    ps_sse("../pi/openai_tools.sse", 0);
    CHECK_INT(pi_prompt(&ses, "show me S/Startup-Sequence"), LS_ABORTED);
    shape(&ses, sh, sizeof(sh));
    CHECK_STR(sh, "user assistant(toolUse,2) toolResult toolResult assistant(aborted,0)");
    CHECK(ses.nmsg == 5 && strstr(ses.msg[4], "\"text\":\"Let me look.\"") != 0);
    /* the next prompt's request: the aborted answer is not in it, every call has its result */
    ps.brk = 0;
    ps.brk_at = 0;
    ps_sse("../pi/openai_text.sse", 0);
    CHECK_INT(pi_prompt(&ses, "again"), LS_STOP);
    CHECK_INT(ps.nreq, 3);
    CHECK(ps.nreq == 3 && count(ps.body[2], "\"role\":\"assistant\"") == 1);
    CHECK(ps.nreq == 3 && count(ps.body[2], "\"role\":\"tool\"") == 2);
    down();
}

/* llm_transform alone: an orphaned call answered, a failed answer left out */
static void test_transform(void)
{
    static const char *const m[] = {
        "{\"role\":\"user\",\"content\":\"a\",\"timestamp\":1}",
        ("{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"id\":\"t1\",\"name\":\"read\",\"arguments\":{}},"
         "{\"type\":\"toolCall\",\"id\":\"t2\",\"name\":\"ls\",\"arguments\":{}}],\"stopReason\":\"toolUse\",\"timestamp\":2}"),
        ("{\"role\":\"toolResult\",\"toolCallId\":\"t1\",\"toolName\":\"read\",\"content\":[],\"isError\":false,"
         "\"timestamp\":3}"),
        "{\"role\":\"user\",\"content\":\"b\",\"timestamp\":4}",
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"half\"}],\"stopReason\":\"aborted\",\"timestamp\":5}",
        "{\"role\":\"user\",\"content\":\"c\",\"timestamp\":6}"
    };
    long n[6];
    int i;
    llm_view v;
    llm_ts ts;
    for (i = 0; i < 6; i++)
        n[i] = (long)strlen(m[i]);
    ts.sec = 7;
    ts.ms = 0;
    CHECK_INT(llm_transform(m, n, 6, ts, &v), 0);
    CHECK_INT(v.count, 6);      /* 6 - 1 aborted + 1 synthesized */
    CHECK_INT(v.synth, 1);
    CHECK_INT(v.dropped, 1);
    CHECK(v.count == 6 && strstr(v.m[3], "\"toolCallId\":\"t2\"") != 0);
    CHECK(v.count == 6 && strstr(v.m[3], "No result provided") != 0 && strstr(v.m[3], "\"isError\":true") != 0);
    CHECK(v.count == 6 && strstr(v.m[4], "\"content\":\"b\"") != 0);
    llm_view_free(&v);
}

/* ---- C3: an extension blocks, rewrites, replaces ---- */

static long ext_events;

static void ext_ev(void *u, const pi_event *e)
{
    (void)u;
    (void)e;
    ext_events++;
}

static int ext_call(void *u, const char *id, const char *name, jw *args, char *reason, long cap)
{
    (void)u;
    (void)id;
    if (!strcmp(name, "ls")) {
        cl_copy(reason, "ls is not allowed here", cap);
        return 1;
    }
    jw_reset(args);
    jw_rawz(args, "{\"path\":\"RAM:x\"}");
    return 0;
}

static void ext_result(void *u, const char *id, const char *name, const char *args, long an, jw *content,
                       int *is_error)
{
    (void)u;
    (void)id;
    (void)args;
    (void)an;
    if (!strcmp(name, "read")) {
        jw_reset(content);
        jw_rawz(content, "[{\"type\":\"text\",\"text\":\"replaced\"}]");
        *is_error = 0;
    }
}

static const pi_ext test_ext = { "test", 0, ext_ev, ext_call, ext_result };

static void test_ext_dispatch(void)
{
    up(0, 1);
    ext_events = 0;
    CHECK_INT(pi_ext_add(&ses.ext, &test_ext), 0);
    ps_sse("../pi/openai_tools.sse", 0);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_prompt(&ses, "show me S/Startup-Sequence"), LS_STOP);
    CHECK_INT(echo_calls, 1);
    CHECK_STR(echo_last, "{\"path\":\"RAM:x\"}");
    CHECK_INT(ses.ext.blocks, 1);
    CHECK(ses.nmsg == 5 && strstr(ses.msg[2], "\"text\":\"replaced\"") != 0);
    CHECK(ses.nmsg == 5 && strstr(ses.msg[3], "\"text\":\"ls is not allowed here\"") != 0 &&
          strstr(ses.msg[3], "\"isError\":true") != 0);
    /* the model is told: round 2 carries the reason */
    CHECK(ps.nreq == 2 && strstr(ps.body[1], "ls is not allowed here") != 0);
    CHECK(ext_events > 10);
    down();
}

/* ---- C4: the system prompt ---- */

static pi_tool t_bash = { "bash", "Run a command", ECHO_PARAMS, "Execute bash commands", 0, echo_run, 0 };

static void test_prompt(void)
{
    jw w;
    up(0, 0);
    pi_tool_add(&ses, &t_read);
    pi_tool_add(&ses, &t_bash);
    jw_init(&w);
    CHECK_INT(pi_system_prompt(&ses, "Work:project", &w), 0);
    pi_golden("pi_agent/prompt.txt", w.p ? w.p : "");
    CHECK(w.p && strstr(w.p, "- Use bash for file operations like ls, rg, find\n") != 0);
    jw_free(&w);
    down();
}

/* ---- C6: the prompt ---- */

static const char *lines[4];
static int nline;
static jw out;

static long l_read(void *u, char *buf, long cap)
{
    (void)u;
    if (!lines[nline])
        return -1;
    cl_copy(buf, lines[nline++], cap);
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

static void line_io(cl_io *io)
{
    memset(io, 0, sizeof(*io));
    io->read_line = l_read;
    io->write = l_write;
    io->brk = l_brk;
    jw_free(&out);
    jw_init(&out);
    nline = 0;
}

static void test_ui(void)
{
    cl_io io;
    static const char *script[] = { "hello\r", "\004", 0 };
    /* the line mode: the answer streamed, a line per tool call */
    up(0, 1);
    line_io(&io);
    lines[0] = "show me S/Startup-Sequence";
    lines[1] = "/exit";
    lines[2] = 0;
    ps_sse("../pi/openai_tools.sse", 0);
    ps_sse("../pi/openai_final.sse", 0);
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    CHECK(out.p && strstr(out.p, "amiga-pi in UP-Term. Model ollama/qwen2.5-coder:7b") != 0);
    CHECK(out.p && strstr(out.p, "Let me look.") != 0);
    CHECK(out.p && strstr(out.p, "* read {\"path\":\"S:Startup-Sequence\",\"limit\":20}") != 0);
    CHECK(out.p && strstr(out.p, "Your Startup-Sequence runs SetPatch first.") != 0);
    CHECK_INT(nline, 2);
    down();

    /* C:Claude's screen when the console answers raw mode and its size */
    up(0, 1);
    ps_sse("../pi/openai_text.sse", 0);
    cs_open(80, 24, script);
    cs_io(&io);
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    CHECK(cs_find("Hello from the Amiga!") >= 0);
    CHECK(cs.raw_calls > 0);
    cs_close();
    down();

    /* print mode */
    up(0, 1);
    line_io(&io);
    ps_sse("../pi/openai_text.sse", 0);
    CHECK_INT(pi_ui_print(&ses, &io, "hello"), 0);
    CHECK_STR(out.p ? out.p : "", "Hello from the Amiga! Grüße — ok\n");
    down();
}

/* ---- C7: the command line through app.c ---- */

static long no_env(const char *name, char *o, long cap)
{
    (void)name;
    (void)o;
    (void)cap;
    return -1;
}

static void test_app(void)
{
    cl_io io;
    pi_platform pl;
    char *argv[] = { "amiga-pi", "--model", "fixture/qwen2.5-coder:7b", "--base-url",
                     "http://192.168.1.10:11434/v1", "-p", "hello", 0 };
    char *bad[] = { "amiga-pi", "--model", "nobaseurl/x", "-p", "hi", 0 };
    ps_reset();
    ps_pool_up(LLM_POOL);
    line_io(&io);
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = fixed_now;
    pl.getenv = no_env;
    pl.cwd = "RAM:";
    ps_sse("../pi/openai_text.sse", 0);
    CHECK_INT(pi_app(7, argv, &pl), 0);
    CHECK_STR(out.p ? out.p : "", "Hello from the Amiga! Grüße — ok\n");
    CHECK(ps.nreq == 1 && strstr(ps.head[0], "POST /v1/chat/completions HTTP/1.1\r\nHost: 192.168.1.10:11434\r\n") ==
                              ps.head[0]);
    CHECK(ps.nreq == 1 && strstr(ps.head[0], "Authorization") == 0);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "<cwd>\\nRAM:\\n</cwd>") != 0);
    line_io(&io);
    CHECK_INT(pi_app(5, bad, &pl), 2);
    CHECK(out.p && strstr(out.p, "provider nobaseurl needs --base-url") != 0);
}

void suite_pi_agent(void)
{
    test_reach_openai();
    test_reach_anthropic();
    test_stops();
    test_abort();
    test_transform();
    test_ext_dispatch();
    test_prompt();
    test_ui();
    test_app();
    ps_reset();
    jw_free(&evlog);
    jw_free(&out);
}
