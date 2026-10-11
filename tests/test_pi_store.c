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

static char root[400], data[420], sdir[520];
static sys_posix sp;
static cl_sys sys;
static jw out;
static const char *in_lines[4];
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
    pl.agent_dir = 0;
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
        CHECK(out.p && strstr(out.p, "second session") < strstr(out.p, "first session"));
        /* number 2 is the older one: the first session */
        CHECK(ps.nreq == 1 && strstr(ps.body[0], "first session") != 0 && strstr(ps.body[0], "second session") == 0);
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
    mkdir(data, 0777);
    sys_posix_init(&sp, &sys);
    s3_dir(data, "/work/proj", sdir, sizeof(sdir));
    jw_init(&out);
    test_flow();
    test_find();
    jw_free(&out);
    ps_reset();
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
