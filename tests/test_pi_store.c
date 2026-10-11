/* amiga-pi's session store through its top-level entry, pi_app (plan phase E):
 * persistence on message_end, the system prompt in the transcript and its replay
 * as the request's prompt, --continue / --session / --resume / --no-session /
 * --name, the model restored from the branch. The recordings are phase B's; the
 * file is read back with session3.c and context.c, which tests/test_pi_session.c
 * compares with pi's own output. */
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
#include "../amiga-pi/context.h"
#include "../amiga-pi/compact.h"

static char root[400], data[420], sdir[520];
static sys_posix sp;
static cl_sys sys;
static jw out;
static const char *in_lines[12];
static int in_n;
static long clock_s;

static char agent[420];
static long window;            /* the model's context window the next run sets (0: as the app gives it) */

static void set_window(pi_session *s)
{
    if (window)
        s->model.context_window = window;
}

static llm_ts adv_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L + clock_s++;
    t.ms = 7;
    return t;
}

static long no_env(const char *name, char *o, long cap)
{
    (void)name;
    (void)o;
    (void)cap;
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
    in_n = 0;
}

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0]) - 1))
#define MODEL_ARGS "--model", "fixture/qwen2.5-coder:7b", "--base-url", "http://192.168.1.10:11434/v1"

/* pi_app over the stub with the session directory in data; the answer queued first */
static int run(int argc, char **argv, const char *sse)
{
    cl_io io;
    pi_platform pl;
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
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = root;
    pl.shell = 0;
    pl.utc = 1;
    pl.data_dir = data;
    pl.agent_dir = agent;
    pl.ready = set_window;
    if (sse)
        ps_sse(sse, 0);
    return pi_app(argc, argv, &pl);
}

/* the newest session file */
static int newest(char *path, long cap)
{
    return s3_recent(&sys, sdir, path, cap);
}

static int count_files(void)
{
    char cmd[700];
    FILE *p;
    char b[16];
    int n = 0;
    strcpy(cmd, "ls ");
    strcat(cmd, sdir);
    strcat(cmd, " 2>/dev/null | wc -l");
    p = popen(cmd, "r");
    if (p) {
        if (fgets(b, sizeof(b), p))
            n = atoi(b);
        pclose(p);
    }
    return n;
}

/* the entries of the file as "type/role" words, space-joined, into out */
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

/* the file's system messages: how many */
static int system_count(pi_s3 *st)
{
    int i, n = 0;
    for (i = 0; i < st->n; i++)
        n += st->e[i].type == S3_MESSAGE && st->e[i].role == S3R_SYSTEM;
    return n;
}

/* the replayed system prompt of the file, JSON-escaped without quotes (as it sits in a request body) */
static void replay_escaped(pi_s3 *st, jw *esc)
{
    pi_ctx c;
    jw m, txt, q;
    ctx_init(&c);
    ctx_build(st, st->leaf, &c);
    jw_init(&m);
    jw_init(&txt);
    jw_init(&q);
    if (ctx_system(&c, &m) == 1) {
        ctx_system_text(m.p, m.n, &txt);
        jw_str(&q, txt.p ? txt.p : "", txt.n);
        jw_raw(esc, q.p + 1, q.n - 2);
    }
    jw_free(&m);
    jw_free(&txt);
    jw_free(&q);
    ctx_free(&c);
}

static void test_flow(void)
{
    char *a1[] = { "amiga-pi", MODEL_ARGS, "-p", "hello", 0 };
    char *a2[] = { "amiga-pi", "--base-url", "http://192.168.1.10:11434/v1", "-c", "-p", "and again", 0 };
    char *a3[] = { "amiga-pi", "--base-url", "http://192.168.1.10:11434/v1", "-c", "--tools", "read", "-p", "fewer", 0 };
    char *a4[] = { "amiga-pi", "--base-url", "http://192.168.1.10:11434/v1", "-c", "-n", "My  named\nsession", "-p", "x", 0 };
    char path[600], sh[400];
    pi_s3 st;
    jw esc;
    long size1;
    struct stat sb;
    jv o, v, m;
    long n;
    const char *t;
    int k, sysmsg;

    /* a new session: model and thinking level, the system message, the user and the answer, all in the file */
    CHECK_INT(run(ARGC(a1), a1, "../pi/openai_text.sse"), 0);
    CHECK_INT(count_files(), 1);
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_STR(sh, "model_change thinking_level_change message/system message/user message/assistant");
    /* pi's system message: no content, the five sections, the tools */
    sysmsg = 2;
    t = s3_text(&st, sysmsg, &n);
    m.p = "{}";
    m.n = 2;
    CHECK(t && !json_parse(t, n, &o) && json_get(o, "message", &m));
    CHECK(json_get(m, "content", &v) && json_streq(v, ""));
    CHECK(json_get(m, "sections", &v) && json_count(v) == 5);
    CHECK(json_get(m, "sections", &v) && json_get(v, "preamble", &o) && json_get(v, "cwd", &o) && json_get(v, "amiga", &o));
    CHECK(json_get(m, "toolsAdded", &v) && json_count(v) == 4);
    /* the request carried exactly the replay of the file */
    jw_init(&esc);
    replay_escaped(&st, &esc);
    CHECK(esc.n > 100);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], esc.p) != 0);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "<cwd>\\n/work/proj\\n</cwd>") != 0);
    /* the answer is the recording's */
    t = s3_text(&st, 4, &n);
    CHECK(t && strstr(t, "Hello from the Amiga!") != 0);
    stat(path, &sb);
    size1 = (long)sb.st_size;
    s3_free(&st);
    jw_free(&esc);

    /* -c continues it: the conversation goes back to the model, the model is the session's, no second
     * system message, the file only grows */
    CHECK_INT(run(ARGC(a2), a2, "../pi/openai_text.sse"), 0);
    CHECK_INT(count_files(), 1);
    s3_init(&st, &sys, adv_now, 0, 1, 2);
    CHECK_INT(s3_open(&st, path), 0);
    shape(&st, sh, sizeof(sh));
    CHECK_STR(sh, "model_change thinking_level_change message/system message/user message/assistant "
                  "message/user message/assistant");
    CHECK_INT(system_count(&st), 1);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "\"hello\"") != 0 && strstr(ps.body[0], "Hello from the Amiga!") != 0 &&
          strstr(ps.body[0], "and again") != 0);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "\"qwen2.5-coder:7b\"") != 0);
    stat(path, &sb);
    CHECK((long)sb.st_size > size1);
    s3_free(&st);

    /* fewer tools: the transcript gets the change, not a new prompt */
    CHECK_INT(run(ARGC(a3), a3, "../pi/openai_text.sse"), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 3);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(system_count(&st), 2);
    k = 0;
    {
        int i;
        for (i = 0; i < st.n; i++)
            if (st.e[i].type == S3_MESSAGE && st.e[i].role == S3R_SYSTEM)
                k = i;
    }
    t = s3_text(&st, k, &n);
    m.p = "{}";
    m.n = 2;
    CHECK(t && !json_parse(t, n, &o) && json_get(o, "message", &m));
    CHECK(json_get(m, "toolsRemoved", &v) && json_count(v) == 3);
    CHECK(!json_get(m, "toolsAdded", &v));
    CHECK(json_get(m, "sections", &v) && json_get(v, "tools", &o) && json_get(v, "rules", &o) &&
          !json_get(v, "preamble", &o) && !json_get(v, "cwd", &o));
    jw_init(&esc);
    replay_escaped(&st, &esc);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], esc.p) != 0);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "- bash:") == 0 && strstr(ps.body[0], "- read:") != 0);
    jw_free(&esc);
    s3_free(&st);

    /* the same tools again: nothing is added */
    CHECK_INT(run(ARGC(a3), a3, "../pi/openai_text.sse"), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 4);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(system_count(&st), 2);
    s3_free(&st);

    /* --name: a session_info entry, sanitised as pi does */
    CHECK_INT(run(ARGC(a4), a4, "../pi/openai_text.sse"), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 5);
    CHECK_INT(s3_open(&st, path), 0);
    {
        char nm[100];
        CHECK_INT(s3_name(&st, nm, sizeof(nm)), 1);
        CHECK_STR(nm, "My  named session");
    }
    s3_free(&st);
}

/* --session by id prefix, -r picking from the list, --no-session, a missing prefix */
static void test_find(void)
{
    char *b1[] = { "amiga-pi", MODEL_ARGS, "--session", "zzzz", "-p", "x", 0 };
    char *b3[] = { "amiga-pi", MODEL_ARGS, "--no-session", "-p", "ephemeral", 0 };
    char *b4[] = { "amiga-pi", MODEL_ARGS, "-r", "-p", "picked", 0 };
    char *b5[] = { "amiga-pi", MODEL_ARGS, "--no-session", "-c", "-p", "x", 0 };
    char *b2[8];
    char path[600], idarg[48], first[600];
    pi_s3 st;
    int files;
    /* two sessions in an empty directory: the second is the newest */
    {
        char cmd[600];
        char *n1[] = { "amiga-pi", MODEL_ARGS, "-p", "first session", 0 };
        char *n2[] = { "amiga-pi", MODEL_ARGS, "-p", "second session", 0 };
        strcpy(cmd, "rm -rf ");
        strcat(cmd, sdir);
        CHECK_INT(system(cmd), 0);
        CHECK_INT(run(ARGC(n1), n1, "../pi/openai_text.sse"), 0);
        newest(first, sizeof(first));
        CHECK_INT(run(ARGC(n2), n2, "../pi/openai_text.sse"), 0);
    }
    files = count_files();
    CHECK_INT(files, 2);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, first), 0);
    cl_copy(idarg, st.id, 14);
    s3_free(&st);
    /* by the first 13 characters of the first session's id */
    b2[0] = "amiga-pi";
    b2[1] = "--base-url";
    b2[2] = "http://192.168.1.10:11434/v1";
    b2[3] = "--session";
    b2[4] = idarg;
    b2[5] = "-p";
    b2[6] = "to the first";
    b2[7] = 0;
    CHECK_INT(run(7, b2, "../pi/openai_text.sse"), 0);
    CHECK_INT(count_files(), 2);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "first session") != 0 && strstr(ps.body[0], "second session") == 0);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, first), 0);
    CHECK(st.e[st.n - 2].role == S3R_USER);
    s3_free(&st);
    CHECK_INT(run(ARGC(b1), b1, 0), 2);
    CHECK(out.p && strstr(out.p, "no session matches zzzz") != 0);
    /* -r lists them, newest first, and takes the number typed */
    in_lines[0] = "2";
    in_lines[1] = 0;
    {
        cl_io io;
        pi_platform pl;
        ps_reset();
        ps_pool_up(LLM_POOL);
        line_io(&io);
        in_lines[0] = "2";
        in_lines[1] = 0;
        memset(&pl, 0, sizeof(pl));
        pl.io = &io;
        pl.pool = &ps_pool;
        pl.h.poll = ps_poll;
        pl.h.sleep = ps_sleep;
        pl.h.ms = ps_ms;
        pl.h.resolve = ps_resolve;
        pl.now = adv_now;
        pl.getenv = no_env;
        pl.cwd = "/work/proj";
        pl.sys = &sys;
        pl.tmp = root;
        pl.utc = 1;
        pl.data_dir = data;
        ps_sse("../pi/openai_text.sse", 0);
        CHECK_INT(pi_app(ARGC(b4), b4, &pl), 0);
        CHECK(out.p && strstr(out.p, "  1  ") != 0 && strstr(out.p, "  2  ") != 0 && strstr(out.p, "  3  ") == 0);
        /* newest first: the order of the two follows their modification times, which the run before moved */
        {
            const char *pf = out.p ? strstr(out.p, "first session") : 0, *pn = out.p ? strstr(out.p, "second session") : 0;
            const char *later = pf && pn && pf > pn ? "first session" : "second session";
            const char *earlier = pf && pn && pf > pn ? "second session" : "first session";
            CHECK(pf && pn);
            /* number 2 is the one listed second */
            CHECK(ps.nreq == 1 && strstr(ps.body[0], later) != 0 && strstr(ps.body[0], earlier) == 0);
        }
    }
    CHECK_INT(count_files(), 2);
    /* --no-session: nothing written, nothing remembered */
    CHECK_INT(run(ARGC(b3), b3, "../pi/openai_text.sse"), 0);
    CHECK_INT(count_files(), 2);
    CHECK(ps.nreq == 1 && strstr(ps.body[0], "ephemeral") != 0 && strstr(ps.body[0], "second session") == 0);
    CHECK_INT(run(ARGC(b5), b5, 0), 2);
    CHECK(out.p && strstr(out.p, "--no-session cannot be combined") != 0);
    (void)path;
}

/* ---- E4: compaction through the prompt (threshold) and /compact ---- */

static int text_in_file(pi_s3 *st, const char *needle)
{
    int i;
    for (i = 0; i < st->n; i++) {
        long n;
        const char *t = s3_text(st, i, &n);
        if (t && strstr(t, needle))
            return 1;
    }
    return 0;
}

static int count_type(pi_s3 *st, int type)
{
    int i, n = 0;
    for (i = 0; i < st->n; i++)
        n += st->e[i].type == type;
    return n;
}

static void set_settings(const char *json)
{
    char p[500];
    FILE *f;
    strcpy(p, agent);
    strcat(p, "/settings.json");
    f = fopen(p, "wb");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
}

static void test_compaction(void)
{
    char *c1[] = { "amiga-pi", MODEL_ARGS, "-p", "hello", 0 };
    char *c2[8];
    char *c3[] = { "amiga-pi", MODEL_ARGS, 0 };
    char path[600], cmd[600], long_prompt[500];
    pi_s3 st;
    pi_ctx cx;
    long t1;
    int i;
    mkdir(agent, 0777);
    strcpy(cmd, "rm -rf ");
    strcat(cmd, sdir);
    CHECK_INT(system(cmd), 0);
    set_settings("{\"compaction\":{\"keepRecentTokens\":5}}");
    window = 0;
    CHECK_INT(run(ARGC(c1), c1, "../pi/openai_text.sse"), 0);
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, path), 0);
    ctx_init(&cx);
    CHECK_INT(ctx_build(&st, st.leaf, &cx), 0);
    t1 = cp_context_tokens(&st, st.leaf, &cx);
    ctx_free(&cx);
    CHECK(t1 > 100);
    CHECK_INT(count_type(&st, S3_COMPACTION), 0);
    s3_free(&st);

    /* the window leaves room for the conversation so far and a little: the next, long prompt crosses the
     * threshold, so before the answer the conversation is summarized (one request) and then answered */
    for (i = 0; i < 400; i++)
        long_prompt[i] = (char)('a' + i % 26);
    long_prompt[400] = 0;
    window = CP_RESERVE + t1 + 50;
    c2[0] = "amiga-pi";
    c2[1] = "--base-url";
    c2[2] = "http://192.168.1.10:11434/v1";
    c2[3] = "-c";
    c2[4] = "-p";
    c2[5] = long_prompt;
    c2[6] = 0;
    {
        cl_io io;
        pi_platform pl;
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
        pl.now = adv_now;
        pl.getenv = no_env;
        pl.cwd = "/work/proj";
        pl.sys = &sys;
        pl.tmp = root;
        pl.utc = 1;
        pl.data_dir = data;
        pl.agent_dir = agent;
        pl.ready = set_window;
        ps_sse("../pi/openai_text.sse", 0);     /* the summary */
        ps_sse("../pi/openai_text.sse", 0);     /* the answer */
        CHECK_INT(pi_app(6, c2, &pl), 0);
    }
    CHECK_INT(ps.nreq, 2);
    CHECK(ps.nreq == 2 && strstr(ps.body[0], "<conversation>") != 0 && strstr(ps.body[0], "[User]: hello") != 0);
    CHECK(ps.nreq == 2 && strstr(ps.body[0], "context summarization assistant") != 0);
    CHECK(ps.nreq == 2 && strstr(ps.body[1], "compacted into the following summary") != 0 &&
          strstr(ps.body[1], long_prompt) != 0 && strstr(ps.body[1], "[User]: hello") == 0);
    s3_init(&st, &sys, adv_now, 0, 1, 2);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(count_type(&st, S3_COMPACTION), 1);
    {
        int ci = -1, k;
        for (k = 0; k < st.n; k++)
            if (st.e[k].type == S3_COMPACTION)
                ci = k;
        CHECK(ci > 0 && st.e[ci - 1].role == S3R_USER);     /* the compaction sits after the new user message */
        CHECK(text_in_file(&st, "\"firstKeptEntryId\":\"") != 0);
        {
            long n;
            const char *t = s3_text(&st, ci, &n);
            jv o, v;
            CHECK(t && !json_parse(t, n, &o) && json_get(o, "systemMessage", &v) && json_type(v) == J_OBJ);
            CHECK(t && json_get(o, "fromHook", &v) && json_type(v) == J_FALSE);
            CHECK(t && json_get(o, "firstKeptEntryId", &v) && json_streq(v, s3_id(&st, ci - 1)));
            CHECK(t && json_get(o, "summary", &v) && json_streq(v, "Hello from the Amiga! Gr\xc3\xbc\xc3\x9f" "e \xe2\x80\x94 ok"));
        }
    }
    s3_free(&st);

    /* /compact [instructions] from the prompt: three questions, then the summary of everything but the last turn
     * (the history, with the instructions) and of the turn's start (its own request), merged */
    window = 0;
    strcpy(cmd, "rm -rf ");
    strcat(cmd, sdir);
    CHECK_INT(system(cmd), 0);
    {
        cl_io io;
        pi_platform pl;
        int k;
        ps_reset();
        ps_pool_up(LLM_POOL);
        line_io(&io);
        in_lines[0] = "q one";
        in_lines[1] = "q two";
        in_lines[2] = "q three";
        in_lines[3] = "/compact only decisions";
        in_lines[4] = "/exit";
        in_lines[5] = 0;
        memset(&pl, 0, sizeof(pl));
        pl.io = &io;
        pl.pool = &ps_pool;
        pl.h.poll = ps_poll;
        pl.h.sleep = ps_sleep;
        pl.h.ms = ps_ms;
        pl.h.resolve = ps_resolve;
        pl.now = adv_now;
        pl.getenv = no_env;
        pl.cwd = "/work/proj";
        pl.sys = &sys;
        pl.tmp = root;
        pl.utc = 1;
        pl.data_dir = data;
        pl.agent_dir = agent;
        for (k = 0; k < 5; k++)
            ps_sse("../pi/openai_text.sse", 0);
        CHECK_INT(pi_app(ARGC(c3), c3, &pl), 0);
        CHECK(out.p && strstr(out.p, "* compacting the conversation") != 0);
        CHECK(out.p && strstr(out.p, "* Compacted: about") != 0);
    }
    CHECK_INT(ps.nreq, 5);
    CHECK(ps.nreq == 5 && strstr(ps.body[3], "Additional focus: only decisions") != 0 &&
          strstr(ps.body[3], "[User]: q one") != 0 && strstr(ps.body[3], "q three") == 0);
    CHECK(ps.nreq == 5 && strstr(ps.body[4], "# Conversation") != 0 && strstr(ps.body[4], "[User]: q three") != 0);
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 3);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(count_type(&st, S3_COMPACTION), 1);
    CHECK(text_in_file(&st, "**Turn Context (split turn):**") != 0);
    s3_free(&st);
    /* and again at once: the last entry is the compaction, pi refuses with its words, nothing is requested */
    {
        char *c4[] = { "amiga-pi", "--base-url", "http://192.168.1.10:11434/v1", "-c", 0 };
        in_lines[0] = "/compact";
        in_lines[1] = "/exit";
        in_lines[2] = 0;
        CHECK_INT(run(ARGC(c4), c4, 0), 0);
        CHECK_INT(ps.nreq, 0);
        CHECK(out.p && strstr(out.p, "[ERROR] Compaction failed: Already compacted") != 0);
    }
    /* a new session has nothing to compact */
    {
        char *c5[] = { "amiga-pi", MODEL_ARGS, "--no-session", 0 };
        in_lines[0] = "/compact";
        in_lines[1] = "/exit";
        in_lines[2] = 0;
        CHECK_INT(run(ARGC(c5), c5, 0), 0);
        CHECK_INT(ps.nreq, 0);
        CHECK(out.p && strstr(out.p, "[ERROR] Compaction failed: Nothing to compact (session too small)") != 0);
    }
}

/* ---- E5, E6, E10: the tree, forks and clones through the prompt ---- */

/* pi_app interactive over scripted input lines (end the script with /exit), n answers queued */
static int script(const char *const *lines, int nsse, char **argv, int argc)
{
    cl_io io;
    pi_platform pl;
    int i;
    ps_reset();
    ps_pool_up(LLM_POOL);
    line_io(&io);
    for (i = 0; lines[i] && i < 11; i++)
        in_lines[i] = lines[i];
    in_lines[i] = 0;
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = "/work/proj";
    pl.sys = &sys;
    pl.tmp = root;
    pl.utc = 1;
    pl.data_dir = data;
    pl.agent_dir = agent;
    pl.ready = set_window;
    for (i = 0; i < nsse; i++)
        ps_sse("../pi/openai_text.sse", 0);
    return pi_app(argc, argv, &pl);
}

static void wipe(void)
{
    char cmd[600];
    strcpy(cmd, "rm -rf ");
    strcat(cmd, sdir);
    CHECK_INT(system(cmd), 0);
}

static int user_count(pi_s3 *st)
{
    int i, n = 0;
    for (i = 0; i < st->n; i++)
        n += st->e[i].type == S3_MESSAGE && st->e[i].role == S3R_USER;
    return n;
}

static void test_tree(void)
{
    char *argv[] = { "amiga-pi", MODEL_ARGS, 0 };
    char path[600];
    pi_s3 st;
    int i, u1 = -1, u3 = -1;
    set_settings("{}");
    window = 0;

    /* E10: two prompts, /tree back to the first user message, the message edited, a third prompt: the file
     * holds both branches, the model sees only the new one */
    wipe();
    {
        const char *const l[] = { "first question", "second question", "/tree", "/tree 1", "edited first", "/exit", 0 };
        CHECK_INT(script(l, 3, argv, ARGC(argv)), 0);
    }
    CHECK(out.p && strstr(out.p, "user: first question") != 0 && strstr(out.p, "user: second question") != 0 &&
          strstr(out.p, "<- you are here") != 0);
    CHECK(out.p && strstr(out.p, "[INFO] Moved.") != 0 && strstr(out.p, "[INFO] The message is back") != 0);
    CHECK_INT(ps.nreq, 3);
    CHECK(strstr(ps.body[2], "edited first") != 0 && strstr(ps.body[2], "first question") == 0 &&
          strstr(ps.body[2], "second question") == 0);
    CHECK(strstr(ps.body[1], "first question") != 0);     /* the second prompt still saw the first */
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(user_count(&st), 3);
    for (i = 0; i < st.n; i++)
        if (st.e[i].type == S3_MESSAGE && st.e[i].role == S3R_USER) {
            if (u1 < 0)
                u1 = i;
            u3 = i;
        }
    CHECK(u1 >= 0 && u3 > u1 && st.e[u1].parent == st.e[u3].parent);    /* two branches from one point */
    {
        pi_ctx cx;
        jw cj;
        ctx_init(&cx);
        jw_init(&cj);
        CHECK_INT(ctx_build(&st, st.leaf, &cx), 0);
        ctx_json(&cx, &cj);
        CHECK(strstr(cj.p, "edited first") != 0 && strstr(cj.p, "second question") == 0);
        jw_free(&cj);
        ctx_free(&cx);
    }
    s3_free(&st);

    /* the number 0 and a number past the end are refused, nothing moves */
    {
        const char *const l[] = { "/tree 99", "/tree 0", "/tree x", "/exit", 0 };
        char *a2[] = { "amiga-pi", "--base-url", "http://192.168.1.10:11434/v1", "-c", 0 };
        CHECK_INT(script(l, 0, a2, ARGC(a2)), 0);
        CHECK(out.p && strstr(out.p, "No such entry") != 0);
        CHECK_INT(ps.nreq, 0);
    }

    /* a summary of the branch left, attached at the new leaf, asked for with its focus */
    wipe();
    {
        const char *const l[] = { "one", "two", "/tree 1 summarize check the parser", "/exit", 0 };
        CHECK_INT(script(l, 3, argv, ARGC(argv)), 0);
    }
    CHECK_INT(ps.nreq, 3);
    CHECK(strstr(ps.body[2], "Additional focus: check the parser") != 0 && strstr(ps.body[2], "<conversation>") != 0);
    CHECK(strstr(ps.body[2], "[User]: two") != 0 && strstr(ps.body[2], "[User]: one") == 0);
    CHECK(out.p && strstr(out.p, "Moved, with a summary of the branch left") != 0);
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 2);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(count_type(&st, S3_BRANCH_SUMMARY), 1);
    for (i = 0; i < st.n; i++)
        if (st.e[i].type == S3_BRANCH_SUMMARY) {
            long n;
            const char *t = s3_text(&st, i, &n);
            jv o, v;
            CHECK(t && !json_parse(t, n, &o) && json_get(o, "summary", &v));
            CHECK(t && strstr(t, "The user explored a different conversation branch before returning here.") != 0);
            CHECK(t && json_get(o, "fromId", &v) && json_type(v) == J_STR);
            CHECK(t && json_get(o, "fromHook", &v) && json_type(v) == J_FALSE);
            CHECK(st.e[i].parent >= 0 && st.e[st.e[i].parent].type != S3_BRANCH_SUMMARY);
        }
    CHECK(st.leaf >= 0 && st.e[st.leaf].type == S3_BRANCH_SUMMARY);
    s3_free(&st);
}

static void test_fork(void)
{
    char *argv[] = { "amiga-pi", MODEL_ARGS, 0 };
    char path[600], first[600];
    pi_s3 st;
    int files;
    wipe();
    /* /fork lists the user messages; /fork 2 starts a session from just before the second: its text comes back,
     * Enter sends it as it was; /clone copies the whole branch */
    {
        const char *const l[] = { "alpha", "beta", "/fork", "/fork 2", "", "/clone", "/exit", 0 };
        CHECK_INT(script(l, 3, argv, ARGC(argv)), 0);
    }
    CHECK(out.p && strstr(out.p, "   1   user: alpha") != 0 && strstr(out.p, "   2   user: beta") != 0);
    CHECK(out.p && strstr(out.p, "[INFO] Forked into a new session.") != 0 && strstr(out.p, "[INFO] Cloned into a new session.") != 0);
    CHECK_INT(ps.nreq, 3);
    CHECK(strstr(ps.body[2], "alpha") != 0 && strstr(ps.body[2], "beta") != 0);
    files = count_files();
    CHECK_INT(files, 3);
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, path), 0);
    /* the clone: its parent is the fork, which holds alpha and the resent beta */
    {
        jv h, v;
        char par[600];
        par[0] = 0;
        v.p = "\"\"";
        v.n = 2;
        CHECK(st.head && !json_parse(st.head, (long)strlen(st.head), &h) && json_get(h, "parentSession", &v));
        json_str(v, par, sizeof(par));
        CHECK(strstr(par, ".jsonl") != 0);
        cl_copy(first, par, sizeof(first));
        CHECK_INT(user_count(&st), 2);
    }
    s3_free(&st);
    s3_init(&st, &sys, adv_now, 0, 1, 2);
    CHECK_INT(s3_open(&st, first), 0);
    CHECK_INT(user_count(&st), 2);
    {
        jv h, v;
        CHECK(st.head && !json_parse(st.head, (long)strlen(st.head), &h) && json_get(h, "parentSession", &v));
    }
    s3_free(&st);
    /* a fork of the first message of a session is an empty session pointing at the old one */
    wipe();
    {
        const char *const l[] = { "only", "/fork 1", "/exit", 0 };
        CHECK_INT(script(l, 1, argv, ARGC(argv)), 0);
    }
    CHECK_INT(count_files(), 1);        /* the new empty session is not written until it has a message */
    CHECK(out.p && strstr(out.p, "[INFO] Forked into a new session.") != 0);
    /* nothing to clone or fork before anything was said */
    wipe();
    {
        const char *const l[] = { "/clone", "/fork 1", "/exit", 0 };
        CHECK_INT(script(l, 0, argv, ARGC(argv)), 0);
        CHECK(out.p && strstr(out.p, "This session has not been saved yet") != 0 &&
              strstr(out.p, "Invalid entry for forking") != 0);
    }
    /* --no-session: the tree and the forks work in memory */
    {
        char *a3[] = { "amiga-pi", MODEL_ARGS, "--no-session", 0 };
        const char *const l[] = { "uniqone", "uniqtwo", "/fork 2", "again", "/exit", 0 };
        CHECK_INT(script(l, 3, a3, ARGC(a3)), 0);
        CHECK_INT(count_files(), 0);
        CHECK(strstr(ps.body[2], "uniqone") != 0 && strstr(ps.body[2], "uniqtwo") == 0 && strstr(ps.body[2], "again") != 0);
    }
}

static void test_names_labels(void)
{
    char *argv[] = { "amiga-pi", MODEL_ARGS, 0 };
    char path[600];
    pi_s3 st;
    wipe();
    {
        const char *const l[] = { "hello there", "/label 1 start", "/label 1", "/label 1 again", "/name my session", "/name",
                                  "/session", "/tree", "/exit", 0 };
        CHECK_INT(script(l, 1, argv, ARGC(argv)), 0);
    }
    CHECK(out.p && strstr(out.p, "[INFO] Label set.") != 0 && strstr(out.p, "[INFO] Label cleared.") != 0);
    CHECK(out.p && strstr(out.p, "my session") != 0 && strstr(out.p, "user: hello there [again]") != 0);
    CHECK(out.p && strstr(out.p, "entries ") != 0 && strstr(out.p, "id ") != 0);
    CHECK_INT(newest(path, sizeof(path)), 0);
    s3_init(&st, &sys, adv_now, 0, 1, 1);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(count_type(&st, S3_LABEL), 3);
    {
        char nm[100];
        CHECK_INT(s3_name(&st, nm, sizeof(nm)), 1);
        CHECK_STR(nm, "my session");
    }
    s3_free(&st);
}

void suite_pi_store(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_store_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    strcpy(data, root);
    strcat(data, "/data");
    strcpy(agent, root);
    strcat(agent, "/agent");
    mkdir(data, 0777);
    sys_posix_init(&sp, &sys);
    s3_dir(data, "/work/proj", sdir, sizeof(sdir));
    jw_init(&out);
    test_flow();
    test_find();
    test_compaction();
    test_tree();
    test_fork();
    test_names_labels();
    jw_free(&out);
    ps_reset();
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
