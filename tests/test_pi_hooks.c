/* amiga-pi's hook commands (plan L1; ext_hooks.h): the dispatcher's own rules, with scripts as the hook commands. What the
 * hooks receive and the decisions equal to pi's are compared with pi 1.1.0 in test_pi_modes.c (scenarios ext_*, group
 * hooks_pi); this suite holds what pi's goldens cannot show -- a command that fails, the order of several hooks, a command
 * that says nothing, a matcher, a timeout, the turn index. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/sys_posix.h"
#include "pi_stub.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/ext_hooks.h"

static char root[400];
static sys_posix sp;
static cl_sys sys;
static llm_model model;
static pi_session ses;
static pi_hooks hk;
static cl_sys *use_sys;             /* the machine the hooks run on: sys, or a copy that times out */
static char told_last[400];
static long told_n;

static void on_notice(void *u, const char *text)
{
    (void)u;
    told_n++;
    cl_copy(told_last, text, sizeof(told_last));
}

static void put(const char *name, const char *text)
{
    char path[500];
    FILE *f;
    strcpy(path, root);
    strcat(path, "/");
    strcat(path, name);
    f = fopen(path, "wb");
    CHECK(f != 0);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int exists(const char *name)
{
    char path[500];
    strcpy(path, root);
    strcat(path, "/");
    strcat(path, name);
    return access(path, F_OK) == 0;
}

static char *read_all(const char *name)
{
    char path[500];
    FILE *f;
    char *b;
    long n;
    strcpy(path, root);
    strcat(path, "/");
    strcat(path, name);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)n + 1);
    n = b ? (long)fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (b)
        b[n] = 0;
    return b;
}

/* a hook entry {"type":"command","command":"sh <root>/<script>"[,"timeout":t]} */
static void entry(jw *w, const char *script, int timeout_s)
{
    jw_rawz(w, "{\"type\":\"command\",\"command\":");
    {
        char cmd[600];
        strcpy(cmd, "sh ");
        strcat(cmd, root);
        strcat(cmd, "/");
        strcat(cmd, script);
        jw_strz(w, cmd);
    }
    if (timeout_s) {
        jw_rawz(w, ",\"timeout\":");
        jw_long(w, timeout_s);
    }
    jw_rawz(w, "}");
}

/* the session with hooks: event -> [matcher, script...] given as a settings.json text built by the caller */
static void up(const char *settings)
{
    memset(&model, 0, sizeof(model));
    pi_session_init(&ses, &model, 0, 0, 0);
    ses.h.notice = on_notice;
    told_last[0] = 0;
    told_n = 0;
    pi_hooks_init(&hk, &ses, use_sys ? use_sys : &sys, root, root);
    CHECK_INT(pi_hooks_settings(&hk, settings, (long)strlen(settings), "settings.json"), 0);
    CHECK_INT(pi_hooks_add(&hk), 0);
}

static void down(void)
{
    pi_hooks_free(&hk);
    pi_session_free(&ses);
}

/* {"hooks":{"<event>":[{"matcher":"<m>","hooks":[<script, timeout>...]}]}} for up to three scripts */
static void settings_for(jw *w, const char *event, const char *matcher, const char *s1, const char *s2, int timeout_s)
{
    jw_rawz(w, "{\"hooks\":{");
    jw_strz(w, event);
    jw_rawz(w, ":[{");
    if (matcher) {
        jw_rawz(w, "\"matcher\":");
        jw_strz(w, matcher);
        jw_rawz(w, ",");
    }
    jw_rawz(w, "\"hooks\":[");
    entry(w, s1, timeout_s);
    if (s2) {
        jw_rawz(w, ",");
        entry(w, s2, timeout_s);
    }
    jw_rawz(w, "]}]}}");
}

static int run_timeout(void *u, const char *cmd, int timeout_s, char *out, long cap, long *outn, long *rc)
{
    (void)u;
    (void)cmd;
    (void)timeout_s;
    (void)out;
    (void)cap;
    *outn = 0;
    *rc = 0;
    return SYS_TIMEOUT;
}

/* a tool_call hook that exits non-zero, times out or cannot run is pi's handler that throws: the call is blocked with
 * the failure as the result's text (agent-session.ts _beforeToolCall) */
static void test_call_failure(void)
{
    jw s, args;
    char reason[300];
    cl_sys tsys;
    jw_init(&s);
    jw_init(&args);
    put("fail.sh", "cat >/dev/null\necho boom\nexit 1\n");
    settings_for(&s, "tool_call", 0, "fail.sh", 0, 0);
    up(s.p);
    jw_rawz(&args, "{\"command\":\"ls\"}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "t1", "bash", &args, reason, sizeof(reason)), 1);
    CHECK(strstr(reason, "failed with 1: boom") != 0 && strstr(reason, "fail.sh") != 0);
    down();
    /* the host's sys_posix has no timeout: a copy of it whose run runs out of time stands for the Amiga's */
    tsys = sys;
    tsys.run = run_timeout;
    use_sys = &tsys;
    jw_reset(&s);
    settings_for(&s, "tool_call", 0, "fail.sh", 0, 1);
    up(s.p);
    CHECK_INT(pi_exts_tool_call(&ses.ext, "t1", "bash", &args, reason, sizeof(reason)), 1);
    CHECK(strstr(reason, "ran out of time") != 0);
    down();
    use_sys = 0;
    jw_free(&s);
    jw_free(&args);
}

/* several hooks are pi's handlers in order: a later one sees an earlier one's new input, the first block ends it */
static void test_call_order(void)
{
    jw s, args;
    char reason[300], *seen;
    jw_init(&s);
    jw_init(&args);
    put("mut.sh", "cat >/dev/null\necho '{\"input\":{\"command\":\"echo mutated\"}}'\n");
    put("trace.sh", "cat > \"$CLAUDE_PROJECT_DIR/trace.json\"\necho '{\"block\":true,\"reason\":\"stop here\"}'\n");
    put("after.sh", "cat > \"$CLAUDE_PROJECT_DIR/after.json\"\n");
    settings_for(&s, "tool_call", 0, "mut.sh", "trace.sh", 0);
    /* a third hook after the blocker: appended to the same group by hand */
    jw_reset(&s);
    jw_rawz(&s, "{\"hooks\":{\"tool_call\":[{\"hooks\":[");
    entry(&s, "mut.sh", 0);
    jw_rawz(&s, ",");
    entry(&s, "trace.sh", 0);
    jw_rawz(&s, ",");
    entry(&s, "after.sh", 0);
    jw_rawz(&s, "]}]}}");
    up(s.p);
    jw_rawz(&args, "{\"command\":\"ls\"}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "t1", "bash", &args, reason, sizeof(reason)), 1);
    CHECK_STR(reason, "stop here");
    CHECK(args.p && strstr(args.p, "echo mutated") != 0);       /* the input the tool would get is the mutated one */
    seen = read_all("trace.json");
    CHECK(seen && strstr(seen, "\"input\":{\"command\":\"echo mutated\"}") != 0);
    free(seen);
    CHECK(!exists("after.json"));                               /* the first blocker wins: no later hook ran */
    down();
    jw_free(&s);
    jw_free(&args);
}

/* an answer that is not a decision (nothing, text, {}, {"action":"continue"}) changes nothing; a matcher keeps a hook to its
 * tools */
static void test_no_decision_and_matcher(void)
{
    jw s, args, text;
    char reason[300];
    jw_init(&s);
    jw_init(&args);
    jw_init(&text);
    put("chat.sh", "cat >/dev/null\necho 'just some words'\n");
    put("empty.sh", "cat >/dev/null\necho '{}'\n");
    put("cont.sh", "cat >/dev/null\necho '{\"action\":\"continue\"}'\n");
    settings_for(&s, "tool_call", "bash", "chat.sh", "empty.sh", 0);
    up(s.p);
    jw_rawz(&args, "{\"command\":\"ls\"}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "t1", "bash", &args, reason, sizeof(reason)), 0);
    CHECK_STR(args.p, "{\"command\":\"ls\"}");
    CHECK_INT((int)pi_hook_runs > 0, 1);
    {
        long r0 = pi_hook_runs;
        CHECK_INT(pi_exts_tool_call(&ses.ext, "t2", "read", &args, reason, sizeof(reason)), 0);
        CHECK_INT(pi_hook_runs - r0, 0);                        /* the matcher says bash only */
    }
    down();
    jw_reset(&s);
    settings_for(&s, "input", 0, "cont.sh", "chat.sh", 0);
    up(s.p);
    jw_rawz(&text, "fix the build");
    CHECK_INT(pi_exts_input(&ses.ext, &text), 0);
    CHECK_STR(text.p, "fix the build");
    down();
    jw_free(&s);
    jw_free(&args);
    jw_free(&text);
}

/* input: transforms chain, "handled" ends the dispatch; a failing input hook is told and the text goes on */
static void test_input(void)
{
    jw s, text;
    char *seen;
    jw_init(&s);
    jw_init(&text);
    put("t1.sh", "cat >/dev/null\ncat <<'EOF'\n{\"action\":\"transform\",\"text\":\"fix the \\\"build\\\"\\nplease\"}\nEOF\n");
    put("t2.sh", "cat > \"$CLAUDE_PROJECT_DIR/t2.json\"\necho '{\"action\":\"handled\"}'\n");
    put("t3.sh", "cat > \"$CLAUDE_PROJECT_DIR/t3.json\"\n");
    jw_rawz(&s, "{\"hooks\":{\"input\":[{\"hooks\":[");
    entry(&s, "t1.sh", 0);
    jw_rawz(&s, ",");
    entry(&s, "t2.sh", 0);
    jw_rawz(&s, ",");
    entry(&s, "t3.sh", 0);
    jw_rawz(&s, "]}]}}");
    up(s.p);
    ses.ext.source = "rpc";
    jw_rawz(&text, "fix");
    CHECK_INT(pi_exts_input(&ses.ext, &text), 1);
    CHECK_STR(text.p, "fix the \"build\"\nplease");
    seen = read_all("t2.json");
    CHECK(seen && !strcmp(seen, "{\"type\":\"input\",\"text\":\"fix the \\\"build\\\"\\nplease\",\"source\":\"rpc\"}\n"));
    free(seen);
    CHECK(!exists("t3.json"));
    down();
    jw_reset(&s);
    put("bad.sh", "cat >/dev/null\nexit 3\n");
    settings_for(&s, "input", 0, "bad.sh", 0, 0);
    up(s.p);
    jw_reset(&text);
    jw_rawz(&text, "hello");
    CHECK_INT(pi_exts_input(&ses.ext, &text), 0);
    CHECK_STR(text.p, "hello");
    CHECK_INT(told_n, 1);
    CHECK(strstr(told_last, "failed with 3") != 0);
    CHECK_INT(hk.errors, 1);
    down();
    jw_free(&s);
    jw_free(&text);
}

/* agent-session.ts _queueUserInput (2173-2199): a steer or follow-up message goes through the input event too, with
 * streamingBehavior "steer" / "followUp"; a transform is what is queued, "handled" queues nothing */
static void test_queue_input(void)
{
    jw s;
    char *seen;
    jw_init(&s);
    put("q1.sh", "cat > \"$CLAUDE_PROJECT_DIR/q1.json\"\ncat <<'EOF'\n{\"action\":\"transform\",\"text\":\"changed\"}\nEOF\n");
    settings_for(&s, "input", 0, "q1.sh", 0, 0);
    up(s.p);
    CHECK_INT(pi_steer(&ses, "look at this"), 0);
    CHECK_INT(ses.nsteer, 1);
    CHECK(ses.nsteer == 1 && !strcmp(ses.steer[0], "changed"));
    seen = read_all("q1.json");
    CHECK(seen && !strcmp(seen, "{\"type\":\"input\",\"text\":\"look at this\",\"source\":\"interactive\",\"streamingBehavior\":\"steer\"}\n"));
    free(seen);
    CHECK_INT(pi_follow_up(&ses, "and this"), 0);
    CHECK(ses.nfollow == 1 && !strcmp(ses.follow[0], "changed"));
    seen = read_all("q1.json");
    CHECK(seen && strstr(seen, "\"streamingBehavior\":\"followUp\"") != 0);
    free(seen);
    CHECK(ses.ext.streaming == 0);
    down();
    put("q2.sh", "cat >/dev/null\necho '{\"action\":\"handled\"}'\n");
    jw_reset(&s);
    settings_for(&s, "input", 0, "q2.sh", 0, 0);
    up(s.p);
    CHECK_INT(pi_steer(&ses, "eaten"), 0);
    CHECK_INT(pi_follow_up(&ses, "eaten"), 0);
    CHECK_INT(ses.nsteer + ses.nfollow, 0);
    down();
    jw_free(&s);
}

/* tool_result: the content replaced without structuredContent drops the structured content (runner.ts emitToolResult,
 * types.ts ToolResultEventResult); with it, both are kept; isError follows */
static void test_result(void)
{
    jw s, content;
    int is_error = 0;
    jw_init(&s);
    jw_init(&content);
    put("r1.sh", "cat >/dev/null\necho '{\"content\":[{\"type\":\"text\",\"text\":\"redacted\"}],\"isError\":true}'\n");
    put("r2.sh", "cat >/dev/null\necho '{\"content\":[{\"type\":\"text\",\"text\":\"kept\"}],\"structuredContent\":{\"a\":1}}'\n");
    settings_for(&s, "tool_result", 0, "r1.sh", 0, 0);
    up(s.p);
    jw_rawz(&ses.structured, "{\"output\":\"x\"}");
    ses.ext.structured = &ses.structured;
    ses.ext.details = &ses.details;
    jw_rawz(&content, "[{\"type\":\"text\",\"text\":\"secret\"}]");
    pi_exts_tool_result(&ses.ext, "t1", "bash", "{}", 2, &content, &is_error);
    CHECK_STR(content.p, "[{\"type\":\"text\",\"text\":\"redacted\"}]");
    CHECK_INT(is_error, 1);
    CHECK_INT(ses.structured.n, 0);
    down();
    jw_reset(&s);
    settings_for(&s, "tool_result", 0, "r2.sh", 0, 0);
    up(s.p);
    jw_rawz(&ses.structured, "{\"output\":\"x\"}");
    ses.ext.structured = &ses.structured;
    ses.ext.details = &ses.details;
    jw_reset(&content);
    jw_rawz(&content, "[{\"type\":\"text\",\"text\":\"secret\"}]");
    pi_exts_tool_result(&ses.ext, "t1", "bash", "{}", 2, &content, &is_error);
    CHECK_STR(content.p, "[{\"type\":\"text\",\"text\":\"kept\"}]");
    CHECK_STR(ses.structured.p, "{\"a\":1}");
    down();
    jw_free(&s);
    jw_free(&content);
}

/* turn_start carries the turn's index: 0 at agent_start, one more after each turn_end; observers get no say */
static void test_turn_index(void)
{
    jw s;
    pi_event e;
    char *seen;
    jw_init(&s);
    put("log.sh", "cat >> \"$CLAUDE_PROJECT_DIR/log.jsonl\"\n");
    settings_for(&s, "turn_start", 0, "log.sh", 0, 0);
    up(s.p);
    pi_event_init(&e, PE_AGENT_START);
    pi_emit(&ses.em, &e);
    pi_event_init(&e, PE_TURN_START);
    pi_emit(&ses.em, &e);
    pi_event_init(&e, PE_TURN_END);
    pi_emit(&ses.em, &e);
    pi_event_init(&e, PE_TURN_START);
    pi_emit(&ses.em, &e);
    seen = read_all("log.jsonl");
    CHECK(seen && strstr(seen, "{\"type\":\"turn_start\",\"turnIndex\":0,\"timestamp\":") != 0 &&
          strstr(seen, "{\"type\":\"turn_start\",\"turnIndex\":1,\"timestamp\":") != 0);
    free(seen);
    down();
    jw_free(&s);
}

/* disableAllHooks, and settings with no hooks at all, put nothing in the session's list */
static void test_off(void)
{
    jw s;
    jw_init(&s);
    settings_for(&s, "tool_call", 0, "fail.sh", 0, 0);
    s.p[s.n - 1] = ',';
    jw_rawz(&s, "\"disableAllHooks\":true}");
    up(s.p);
    CHECK_INT(ses.ext.n, 0);
    down();
    up("{\"model\":\"x\"}");
    CHECK_INT(ses.ext.n, 0);
    CHECK_INT(pi_hooks_any(&hk), 0);
    down();
    jw_free(&s);
}

void suite_pi_hooks(void)
{
    strcpy(root, "/tmp");
    if (getenv("TMPDIR")) {
        strncpy(root, getenv("TMPDIR"), sizeof(root) - 40);
        root[sizeof(root) - 40] = 0;
    }
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_hooks_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    sys_posix_init(&sp, &sys);
    test_call_failure();
    test_call_order();
    test_no_decision_and_matcher();
    test_input();
    test_queue_input();
    test_result();
    test_turn_index();
    test_off();
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
