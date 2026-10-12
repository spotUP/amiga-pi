/* amiga-pi's project instructions, SYSTEM.md, APPEND_SYSTEM.md and project trust (plan phase G):
 *   goldens   the prompt's sections for 5 layouts against what pi's own code gives
 *             (tests/pi/resources/NAME.json, tools/pi_oracle/gen_resources.ts); trust.json bytes, the
 *             nearest decision, the answers' labels and the "needs trust" check against pi's
 *             ProjectTrustStore / getProjectTrustOptions / hasTrustRequiringProjectResources (trust.txt);
 *   decide    pi_trust_decide's order: -a / -na, the saved decision, defaultProjectTrust, the question;
 *   reach     pi_app -> the request body: AGENTS.md at 2 levels root-most first (G4), SYSTEM.md and
 *             APPEND_SYSTEM.md, trust by -a / -na / the question / a saved decision / settings.json, /trust.
 * The trees are made in a temp directory; pi's goldens name it @ROOT@. */
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
#include "claude/trust.h"
#include "pi_stub.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/prompt.h"
#include "../amiga-pi/resources.h"
#include "../amiga-pi/args.h"

static char root[400];
static sys_posix sp;
static cl_sys sys;
static jw out;
static const char *in_lines[12];
static int in_n;

static void mkdirs(const char *path)
{
    char b[600];
    char *p;
    cl_copy(b, path, sizeof(b));
    for (p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0777);
            *p = '/';
        }
    mkdir(b, 0777);
}

/* root/rel written (its directories made) */
static void put(const char *rel, const char *text)
{
    char p[600], d[600];
    char *sl;
    cl_copy(p, root, sizeof(p));
    cl_cat(p, "/", sizeof(p));
    cl_cat(p, rel, sizeof(p));
    cl_copy(d, p, sizeof(d));
    sl = strrchr(d, '/');
    *sl = 0;
    mkdirs(d);
    CHECK_INT(sys.write(sys.u, p, text, (long)strlen(text)), 0);
}

static void at(char *out_path, long cap, const char *rel)
{
    cl_copy(out_path, root, cap);
    cl_cat(out_path, "/", cap);
    cl_cat(out_path, rel, cap);
}

static char *slurp_text(const char *path, long *n)
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
    *n = len;
    return b;
}

/* the golden NAME of tests/pi/<dir>/ with @ROOT@ replaced by the temp directory */
static char *golden_in(const char *dir, const char *name, long *n)
{
    char path[600], *raw, *res;
    long rn, i, k = 0, rl = (long)strlen(root);
    cl_copy(path, PI_TESTS "/pi/", sizeof(path));
    cl_cat(path, dir, sizeof(path));
    cl_cat(path, "/", sizeof(path));
    cl_cat(path, name, sizeof(path));
    raw = slurp_text(path, &rn);
    if (!raw)
        return 0;
    for (i = 0, k = 0; i + 6 <= rn; i++)
        if (!strncmp(raw + i, "@ROOT@", 6))
            k++;
    res = (char *)malloc((size_t)(rn + 1) + (size_t)k * (size_t)rl);
    k = 0;
    for (i = 0; i < rn;) {
        if (!strncmp(raw + i, "@ROOT@", 6)) {
            memcpy(res + k, root, (size_t)rl);
            k += rl;
            i += 6;
        } else
            res[k++] = raw[i++];
    }
    res[k] = 0;
    *n = k;
    free(raw);
    return res;
}

static char *golden(const char *name, long *n)
{
    return golden_in("resources", name, n);
}

/* ---- the prompt's sections against pi's ---- */

static const char *const no_guide[] = { 0 };
static pi_tool t_read = { "read", "", "{}", "Read file contents", 0, 0, 0, no_guide, 0 };
static pi_tool t_bash = { "bash", "", "{}", "Execute bash commands", 0, 0, 0, no_guide, 0 };
static pi_session ps_s;

/* one layout: pi_res_load for cwd (root/rel) and the agent directory, the sections compared with NAME.json. The
 * differences from pi are the deliberate ones: pi's "docs" section is amiga-pi's "amiga" (text not compared), and
 * the default preamble says amiga-pi where pi says pi */
static void layout(const char *name, const char *cwd_rel, const char *agent_rel, int trusted, long reads)
{
    pi_res r;
    pi_psec sec;
    char cwd[400], agent[400], golden_name[64];
    char *g;
    long gn;
    jv gv, k, v;
    jit it;
    int i = 0;
    at(cwd, sizeof(cwd), cwd_rel);
    at(agent, sizeof(agent), agent_rel);
    mkdirs(cwd);
    mkdirs(agent);
    CHECK_INT(pi_res_load(&r, &sys, agent, cwd, trusted), 0);
    CHECK_INT(r.reads, reads);
    memset(&ps_s, 0, sizeof(ps_s));
    ps_s.tools[0] = &t_read;
    ps_s.tools[1] = &t_bash;
    ps_s.ntools = 2;
    ps_s.res = &r;
    CHECK_INT(pi_prompt_sections(&ps_s, cwd, &sec), 0);
    cl_copy(golden_name, name, sizeof(golden_name));
    cl_cat(golden_name, ".json", sizeof(golden_name));
    g = golden(golden_name, &gn);
    CHECK(g != 0);
    if (g && !json_parse(g, gn, &gv)) {
        CHECK_INT(json_count(gv), sec.n);
        json_iter(gv, &it);
        while (json_next(&it, &k, &v) && i < sec.n) {
            char key[32];
            char *want;
            long wn;
            json_str(k, key, sizeof(key));
            if (!strcmp(sec.name[i], "amiga")) {
                CHECK_STR(key, "docs");
            } else {
                CHECK_STR(key, sec.name[i]);
                want = json_strdup(v, &wn);
                if (want && !strcmp(key, "preamble") && !strncmp(want, "You are an expert coding assistant operating inside pi,", 54)) {
                    /* the default preamble: "inside pi," is "inside amiga-pi," */
                    char *fixed = (char *)malloc((size_t)wn + 16);
                    memcpy(fixed, want, 52);
                    memcpy(fixed + 52, "amiga-", 6);
                    memcpy(fixed + 58, want + 52, (size_t)(wn - 52) + 1);
                    free(want);
                    want = fixed;
                    wn += 6;
                }
                CHECK(want != 0 && sec.len[i] == wn && memcmp(sec.text.p + sec.off[i], want, (size_t)wn) == 0);
                if (want && (sec.len[i] != wn || memcmp(sec.text.p + sec.off[i], want, (size_t)wn)))
                    printf("  section %s of %s differs:\n--- want\n%s\n--- got\n%.*s\n", key, name, want, (int)sec.len[i],
                           sec.text.p + sec.off[i]);
                free(want);
            }
            i++;
        }
    }
    free(g);
    pi_psec_free(&sec);
    pi_res_free(&r);
}

static void test_goldens(void)
{
    /* the same trees as tools/pi_oracle/gen_resources.ts */
    put("l1/agent/AGENTS.md", "AGENT-WIDE\n");
    put("l1/w/AGENTS.md", "\xEF\xBB\xBFOUTER rules\n");
    put("l1/w/proj/CLAUDE.md", "INNER rules\nline two");
    layout("default", "l1/w/proj", "l1/agent", 0, 3);
    put("l2/agent/SYSTEM.md", "You are AGENT-SYSTEM.");
    put("l2/agent/APPEND_SYSTEM.md", "AGENT-APPEND\n");
    put("l2/w/proj/AGENTS.md", "P2\n");
    layout("agent_system", "l2/w/proj", "l2/agent", 0, 3);
    put("l3/agent/SYSTEM.md", "AGENT-SYSTEM");
    put("l3/agent/APPEND_SYSTEM.md", "AGENT-APPEND");
    put("l3/w/proj/.pi/SYSTEM.md", "PROJECT-SYSTEM");
    put("l3/w/proj/.pi/APPEND_SYSTEM.md", "PROJECT-APPEND");
    put("l3/w/proj/AGENTS.override.md", "OVERRIDE");
    put("l3/w/proj/AGENTS.md", "NOT-READ");
    layout("project_trusted", "l3/w/proj", "l3/agent", 1, 3);        /* override, the two .pi files */
    layout("project_untrusted", "l3/w/proj", "l3/agent", 0, 3);      /* override, the agent's two */
    put("l5/w/proj/.pi/APPEND_SYSTEM.md", "ONLY-APPEND");
    layout("append_only", "l5/w/proj", "l5/agent", 1, 1);
}

/* ---- trust ---- */

static char asked_title[800];
static char asked_label[6][360];
static int asked_n, asked_count, answer;

static int ask_fn(void *u, const char *title, const char *const *labels, int n)
{
    int i;
    (void)u;
    asked_count++;
    cl_copy(asked_title, title, sizeof(asked_title));
    asked_n = n;
    for (i = 0; i < n && i < 6; i++)
        cl_copy(asked_label[i], labels[i], sizeof(asked_label[0]));
    return answer;
}

static void test_trust_store(void)
{
    char agent[400], f[420], *want, *got, line[600];
    long wn, gn, i;
    jw lines, o;
    jv dummy;
    (void)dummy;
    at(agent, sizeof(agent), "trustagent");
    mkdirs(agent);
    cl_copy(f, agent, sizeof(f));
    cl_cat(f, "/trust.json", sizeof(f));
    jw_init(&o);
    /* pi's updates, in the order gen_resources.ts makes them (parent trust deletes the child's entry) */
    CHECK_INT(trust_put(&sys, f, "/w/proj/a", 1), 0);
    CHECK_INT(pi_trust_command(&sys, agent, "/w/proj/a", 2, &o), 0);
    CHECK_STR(o.p, "Saved trust decision: trusted. Restart amiga-pi for this to take effect.");
    CHECK_INT(trust_put(&sys, f, "/w/b", 0), 0);
    CHECK_INT(trust_put(&sys, f, "/w/Zeta", 1), 0);
    CHECK_INT(trust_put(&sys, f, "/w/b/c", 1), 0);
    CHECK_INT(trust_put(&sys, f, "/w/b/c", -1), 0);
    want = golden("trust.json", &wn);
    got = slurp_text(f, &gn);
    CHECK(want && got && !strcmp(want, got));
    free(want);
    free(got);
    /* trust.txt: the nearest decisions, the answers' labels, the needs-trust check */
    jw_init(&lines);
    {
        static const struct {
            const char *p;
        } q[] = { { "/w/proj/a/x" }, { "/w/b/c" }, { "/q" } };
        for (i = 0; i < 3; i++) {
            int d = trust_near(&sys, f, q[i].p, 0, 0);
            jw_rawz(&lines, "get ");
            jw_rawz(&lines, q[i].p);
            jw_rawz(&lines, d < 0 ? " null\n" : d ? " true\n" : " false\n");
        }
    }
    {
        char cwd[400];
        int k;
        at(cwd, sizeof(cwd), "l3/w/proj");
        answer = -1;
        asked_count = 0;
        CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, 0, ask_fn, 0), 0);
        CHECK_INT(asked_count, 1);
        for (k = 0; k < asked_n; k++) {
            jw_rawz(&lines, "option ");
            jw_rawz(&lines, asked_label[k]);
            jw_rawz(&lines, !strncmp(asked_label[k], "Trust", 5) ? " true\n" : " false\n");
        }
    }
    {
        static const char *const rels[] = { "l1/w/proj", "l3/w/proj", "n1/p", "n2/p", "n3/p" };
        char p[400];
        for (i = 0; i < 5; i++) {
            if (i == 2)
                put("n1/p/.pi/skills/x", "");
            if (i == 3)
                mkdirs((at(p, sizeof(p), "n2/p/.pi"), p));
            if (i == 4) {
                at(p, sizeof(p), "n3/p");
                mkdirs(p);
                put("n3/.agents/skills/x", "");
            }
            at(p, sizeof(p), rels[i]);
            cl_copy(line, "needed ", sizeof(line));
            cl_cat(line, rels[i], sizeof(line));
            cl_cat(line, pi_trust_needed(&sys, p) ? " true\n" : " false\n", sizeof(line));
            jw_rawz(&lines, line);
        }
    }
    want = golden("trust.txt", &wn);
    CHECK(want && lines.p && !strcmp(want, lines.p));
    if (want && lines.p && strcmp(want, lines.p))
        printf("--- want\n%s--- got\n%s", want, lines.p);
    free(want);
    jw_free(&lines);
    jw_free(&o);
}

/* pi_trust_decide's order */
static void test_decide(void)
{
    char agent[400], cwd[400], parent[400], f[420], *got;
    long gn;
    at(agent, sizeof(agent), "decide_agent");
    at(cwd, sizeof(cwd), "l3/w/proj");
    at(parent, sizeof(parent), "l3/w");
    mkdirs(agent);
    cl_copy(f, agent, sizeof(f));
    cl_cat(f, "/trust.json", sizeof(f));
    remove(f);
    /* print mode (nobody to ask) is not trusted; a project with nothing guarded needs no question */
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "ask", 0, 0), 0);
    {
        char plain[400];
        at(plain, sizeof(plain), "l1/w/proj");
        asked_count = 0;
        CHECK_INT(pi_trust_decide(&sys, agent, plain, -1, "ask", ask_fn, 0), 1);
        CHECK_INT(asked_count, 0);
    }
    /* -a / -na beat everything */
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, 1, "never", 0, 0), 1);
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, 0, "always", 0, 0), 0);
    /* defaultProjectTrust */
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "always", 0, 0), 1);
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "never", ask_fn, 0), 0);
    CHECK_INT(asked_count, 0);
    /* the question: pi's title and its five answers; "this session only" keeps nothing */
    answer = PI_TRUST_SESSION;
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "ask", ask_fn, 0), 1);
    CHECK_INT(asked_count, 1);
    CHECK_INT(asked_n, 5);
    CHECK(!strncmp(asked_title, "Trust project folder?\n", 22) && strstr(asked_title, cwd) != 0);
    CHECK_STR(asked_label[0], "Trust");
    CHECK_STR(asked_label[2], "Trust (this session only)");
    CHECK_STR(asked_label[4], "Do not trust (this session only)");
    CHECK_INT(sys.kind(sys.u, f), 0);
    answer = PI_TRUST_NO_SESSION;
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "ask", ask_fn, 0), 0);
    CHECK_INT(sys.kind(sys.u, f), 0);
    /* Do not trust: kept; the next session is not asked and an always-default does not undo it */
    answer = PI_TRUST_NO;
    asked_count = 0;
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "ask", ask_fn, 0), 0);
    CHECK_INT(asked_count, 1);
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "always", ask_fn, 0), 0);
    CHECK_INT(asked_count, 1);
    CHECK_INT(pi_trust_decide(&sys, agent, cwd, 1, "ask", ask_fn, 0), 1);       /* -a still wins */
    /* Trust parent folder (/trust 2): the parent is kept and the child's own entry is gone */
    {
        jw o;
        jw_init(&o);
        CHECK_INT(pi_trust_command(&sys, agent, cwd, 2, &o), 0);
        CHECK(strstr(o.p, "Saved trust decision: trusted. Restart amiga-pi for this to take effect.") != 0);
        jw_free(&o);
        got = slurp_text(f, &gn);
        CHECK(got && strstr(got, parent) != 0 && strstr(got, "false") == 0);
        free(got);
        asked_count = 0;
        CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "never", ask_fn, 0), 1);
        CHECK_INT(asked_count, 0);
    }
}

/* GA3: a trust.json pi's readTrustFile would refuse stops pi; it is not "no decision" and not rewritten;
 * an answer that cannot be kept is an error, not a silent not-kept */
static void test_trust_errors(void)
{
    char agent[400], cwd[400], f[420], blocker[420], *got;
    long gn;
    jw o;
    int k;
    static const char *const bad[] = { "{\"/w\": true,", "[true]", "{\"/w/proj\": \"yes\", \"/w\": true}", 0 };
    at(agent, sizeof(agent), "err_agent");
    at(cwd, sizeof(cwd), "l3/w/proj");
    mkdirs(agent);
    cl_copy(f, agent, sizeof(f));
    cl_cat(f, "/trust.json", sizeof(f));
    jw_init(&o);
    for (k = 0; bad[k]; k++) {
        char err[400];
        put("err_agent/trust.json", bad[k]);
        asked_count = 0;
        answer = PI_TRUST_YES;
        CHECK_INT(pi_trust_decide(&sys, agent, cwd, -1, "always", ask_fn, 0), -1);   /* not "always" over a lost "no" */
        CHECK_INT(asked_count, 0);
        pi_trust_why(&sys, agent, err, sizeof(err));
        CHECK(strstr(err, "trust store ") != 0 && strstr(err, "trust.json") != 0);
        jw_free(&o);
        jw_init(&o);
        CHECK_INT(pi_trust_command(&sys, agent, cwd, 0, &o), -1);
        CHECK(strstr(o.p, "[ERROR] ") != 0 && strstr(o.p, "trust store ") != 0);
        jw_free(&o);
        jw_init(&o);
        CHECK_INT(pi_trust_command(&sys, agent, cwd, 1, &o), -1);
        got = slurp_text(f, &gn);
        CHECK(got != 0 && !strcmp(got, bad[k]));                                   /* not rewritten */
        free(got);
    }
    /* the kept answer that cannot be written is pi's error too: the agent directory is under a file */
    remove(f);
    put("err_blocker", "x");
    at(blocker, sizeof(blocker), "err_blocker/agent");
    answer = PI_TRUST_YES;
    CHECK_INT(pi_trust_decide(&sys, blocker, cwd, -1, "ask", ask_fn, 0), -1);
    jw_free(&o);
    jw_init(&o);
    CHECK_INT(pi_trust_command(&sys, blocker, cwd, 1, &o), -1);
    CHECK(strstr(o.p, "[ERROR] Failed to write trust store ") != 0);
    jw_free(&o);
}

/* ---- through pi_app ---- */

static llm_ts adv_now(void *u)
{
    llm_ts t;
    static long clock_s;
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

/* a second flag with its value and the prompt, for the next app_run only (0: none, "hello") */
static const char *app_flag2, *app_value2, *app_prompt;

/* pi_app in root/cwd_rel with the agent directory root/agent_rel; extra the flag between the options and the
 * prompt (0 none); print: -p; lines the typed input (the trust answer first) */
static int app_run(const char *cwd_rel, const char *agent_rel, const char *extra, int print, const char *const *lines)
{
    cl_io io;
    pi_platform pl;
    char cwd[400], agent[400], tmp[420], *argv[16];
    int a = 0, i;
    at(cwd, sizeof(cwd), cwd_rel);
    at(agent, sizeof(agent), agent_rel);
    mkdirs(cwd);
    mkdirs(agent);
    cl_copy(tmp, root, sizeof(tmp));
    cl_cat(tmp, "/", sizeof(tmp));
    memset(&io, 0, sizeof(io));
    io.read_line = l_read;
    io.write = l_write;
    io.brk = l_brk;
    jw_free(&out);
    jw_init(&out);
    for (i = 0; i < 12; i++)
        in_lines[i] = 0;
    for (i = 0; lines && lines[i] && i < 11; i++)
        in_lines[i] = lines[i];
    in_n = 0;
    ps_reset();
    ps_pool_up(LLM_POOL);
    ps_sse("../pi/openai_text.sse", 0);
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = cwd;
    pl.sys = &sys;
    pl.tmp = tmp;
    pl.utc = 1;
    pl.agent_dir = agent;
    argv[a++] = "amiga-pi";
    if (print)
        argv[a++] = "-p";
    argv[a++] = "--no-session";
    argv[a++] = "--no-mcp";
    if (extra)
        argv[a++] = (char *)extra;
    if (app_flag2) {
        argv[a++] = (char *)app_flag2;
        if (app_value2)
            argv[a++] = (char *)app_value2;
    }
    argv[a++] = "--model";
    argv[a++] = "fixture/qwen2.5-coder:7b";
    argv[a++] = "--base-url";
    argv[a++] = "http://192.168.1.10:11434/v1";
    argv[a++] = (char *)(app_prompt ? app_prompt : "hello");
    argv[a] = 0;
    app_flag2 = app_value2 = app_prompt = 0;
    return pi_app(a, argv, &pl);
}

/* the first request's body mentions needle */
static int sent(const char *needle)
{
    return ps.nreq >= 1 && strstr(ps.body[0], needle) != 0;
}

static const char *pos(const char *needle)
{
    return ps.nreq >= 1 ? strstr(ps.body[0], needle) : 0;
}

/* G4: AGENTS.md at two levels reach the request, root-most first, through pi_app -> pi_prompt */
static void test_reach_context(void)
{
    long reads = pi_res_reads;
    const char *o, *i;
    put("r1/w/AGENTS.md", "OUTER-RULES-7");
    put("r1/w/proj/AGENTS.md", "INNER-RULES-9");
    CHECK_INT(app_run("r1/w/proj", "r1/agent", 0, 1, 0), 0);
    CHECK_INT(pi_res_reads - reads, 2);                 /* the sentinel: the loader read both files */
    CHECK_INT(ps.nreq, 1);
    o = pos("OUTER-RULES-7");
    i = pos("INNER-RULES-9");
    CHECK(o != 0 && i != 0 && o < i);                   /* root-most first */
    CHECK(sent("<project_context>"));
    CHECK(sent("<project_instructions path=\\\""));
    CHECK(sent("expert coding assistant"));             /* the default prompt otherwise */
    /* the file is the knob: gone, the request carries neither */
    {
        char p[400];
        at(p, sizeof(p), "r1/w/proj/AGENTS.md");
        remove(p);
        at(p, sizeof(p), "r1/w/AGENTS.md");
        remove(p);
        CHECK_INT(app_run("r1/w/proj", "r1/agent", 0, 1, 0), 0);
        CHECK(!sent("OUTER-RULES-7") && !sent("INNER-RULES-9") && !sent("<project_context>"));
    }
}


static void write_file(const char *rel, const char *text)
{
    put(rel, text);
}

/* GA6: the agent directory spelled another way (an assign, a link) than the working directory it is inside
 * of, or is, loads its AGENTS.md once */
static void test_agent_dir_alias(void)
{
    char cwd[400], link[400], lnk_target[400];
    pi_res r;
    put("r6/w/AGENTS.md", "ALIASED-RULES-3");
    at(cwd, sizeof(cwd), "r6/w");
    at(link, sizeof(link), "r6/link");
    at(lnk_target, sizeof(lnk_target), "r6/w");
    remove(link);
    CHECK_INT(symlink(lnk_target, link), 0);
    CHECK_INT(pi_res_load(&r, &sys, link, cwd, 0), 0);
    CHECK_INT(r.nf, 1);                                 /* running inside the agent directory: the file once */
    pi_res_free(&r);
    /* a different agent directory still contributes its own file */
    put("r6/agent/AGENTS.md", "AGENT-RULES-4");
    at(link, sizeof(link), "r6/agent");
    CHECK_INT(pi_res_load(&r, &sys, link, cwd, 0), 0);
    CHECK_INT(r.nf, 2);
    pi_res_free(&r);
}

/* G2/G3: SYSTEM.md and APPEND_SYSTEM.md, and who may supply them, through pi_app */
/* GA3 through pi_app: a broken trust.json ends the run with pi's error, nothing is sent */
static void test_reach_trust_error(void)
{
    char f[420], *got;
    long gn;
    put("r5/w/proj/.pi/SYSTEM.md", "PROJECT-SYSTEM-TEXT");
    put("r5/agent/trust.json", "{\"/w\": 1}");
    CHECK_INT(app_run("r5/w/proj", "r5/agent", 0, 1, 0), 2);
    CHECK_INT(ps.nreq, 0);
    CHECK(strstr(out.p, "amiga-pi: Invalid trust store ") != 0 && strstr(out.p, "must be true, false, or null") != 0);
    at(f, sizeof(f), "r5/agent/trust.json");
    got = slurp_text(f, &gn);
    CHECK(got != 0 && !strcmp(got, "{\"/w\": 1}"));
    free(got);
    /* -na and -a do not read the file (pi decides from the flag first) */
    CHECK_INT(app_run("r5/w/proj", "r5/agent", "-na", 1, 0), 0);
}

static void test_reach_trust(void)
{
    char f[420], *got;
    long gn;
    const char *yes[] = { "1", 0 };
    const char *cmds[] = { "1", "/trust", "/trust 3", 0 };
    const char *no[] = { "4", 0 };
    put("r2/agent/SYSTEM.md", "AGENT-SYSTEM-TEXT");
    put("r2/agent/APPEND_SYSTEM.md", "AGENT-APPEND-TEXT");
    put("r2/w/proj/.pi/SYSTEM.md", "PROJECT-SYSTEM-TEXT");
    put("r2/w/proj/.pi/APPEND_SYSTEM.md", "PROJECT-APPEND-TEXT");
    at(f, sizeof(f), "r2/agent/trust.json");

    /* print mode, nothing decided: not trusted, nobody asked; the agent directory's files */
    CHECK_INT(app_run("r2/w/proj", "r2/agent", 0, 1, 0), 0);
    CHECK(sent("AGENT-SYSTEM-TEXT") && sent("AGENT-APPEND-TEXT"));
    CHECK(!sent("PROJECT-SYSTEM-TEXT") && !sent("PROJECT-APPEND-TEXT"));
    CHECK(!sent("expert coding assistant"));            /* SYSTEM.md replaces the default preamble */
    CHECK(!sent("<tools>") && !sent("<rules>") && !sent("<amiga>"));       /* pi's customPrompt: no tools, rules, docs */
    CHECK(sent("<addendum>") && sent("<cwd>"));
    CHECK(strstr(out.p, "Trust project folder?") == 0);
    CHECK_INT(sys.kind(sys.u, f), 0);
    /* -a: the project's files win, and nothing is kept */
    CHECK_INT(app_run("r2/w/proj", "r2/agent", "-a", 1, 0), 0);
    CHECK(sent("PROJECT-SYSTEM-TEXT") && sent("PROJECT-APPEND-TEXT"));
    CHECK(!sent("AGENT-SYSTEM-TEXT") && !sent("AGENT-APPEND-TEXT"));
    CHECK_INT(sys.kind(sys.u, f), 0);
    CHECK_INT(app_run("r2/w/proj", "r2/agent", "--approve", 1, 0), 0);
    CHECK(sent("PROJECT-SYSTEM-TEXT"));
    /* -na */
    CHECK_INT(app_run("r2/w/proj", "r2/agent", "-na", 1, 0), 0);
    CHECK(sent("AGENT-SYSTEM-TEXT") && !sent("PROJECT-SYSTEM-TEXT"));
    /* the question (typed mode): answer 1 trusts and keeps it; /trust lists, /trust 3 changes the kept decision */
    CHECK_INT(app_run("r2/w/proj", "r2/agent", 0, 0, cmds), 0);
    CHECK(strstr(out.p, "Trust project folder?") != 0 && strstr(out.p, "Trust parent folder (") != 0);
    CHECK(sent("PROJECT-SYSTEM-TEXT"));
    CHECK(strstr(out.p, "Kept: trusted (") != 0);
    CHECK(strstr(out.p, "Saved trust decision: untrusted. Restart amiga-pi for this to take effect.") != 0);
    got = slurp_text(f, &gn);
    CHECK(got && strstr(got, "false") != 0 && strstr(got, "true") == 0);
    free(got);
    /* the kept "no" is not asked again; print mode and a typed run both use the agent's files */
    CHECK_INT(app_run("r2/w/proj", "r2/agent", 0, 0, yes), 0);
    CHECK(strstr(out.p, "Trust project folder?") == 0 && sent("AGENT-SYSTEM-TEXT") && !sent("PROJECT-SYSTEM-TEXT"));
    /* a kept "yes" in the file is read by the next print-mode run */
    {
        char agent[400], cwd[400];
        at(agent, sizeof(agent), "r2/agent");
        at(cwd, sizeof(cwd), "r2/w/proj");
        CHECK_INT(trust_put(&sys, f, cwd, 1), 0);
    }
    CHECK_INT(app_run("r2/w/proj", "r2/agent", 0, 1, 0), 0);
    CHECK(sent("PROJECT-SYSTEM-TEXT"));
    /* the typed run, "Do not trust" (4), kept for a fresh project */
    put("r3/w/proj/.pi/SYSTEM.md", "P3-SYSTEM");
    put("r3/agent/SYSTEM.md", "A3-SYSTEM");
    CHECK_INT(app_run("r3/w/proj", "r3/agent", 0, 0, no), 0);
    CHECK(strstr(out.p, "Trust project folder?") != 0 && sent("A3-SYSTEM") && !sent("P3-SYSTEM"));
    at(f, sizeof(f), "r3/agent/trust.json");
    got = slurp_text(f, &gn);
    CHECK(got && strstr(got, "false") != 0);
    free(got);
    /* settings.json defaultProjectTrust */
    put("r4/w/proj/.pi/SYSTEM.md", "P4-SYSTEM");
    put("r4/agent/SYSTEM.md", "A4-SYSTEM");
    write_file("r4/agent/settings.json", "{\"defaultProjectTrust\": \"always\"}");
    CHECK_INT(app_run("r4/w/proj", "r4/agent", 0, 1, 0), 0);
    CHECK(sent("P4-SYSTEM") && !sent("A4-SYSTEM"));
    write_file("r4/agent/settings.json", "{\"defaultProjectTrust\": \"never\"}");
    CHECK_INT(app_run("r4/w/proj", "r4/agent", 0, 0, yes), 0);
    CHECK(strstr(out.p, "Trust project folder?") == 0 && sent("A4-SYSTEM"));
    CHECK_INT(app_run("r4/w/proj", "r4/agent", "-a", 1, 0), 0);          /* -a over the setting */
    CHECK(sent("P4-SYSTEM"));
    /* a project with nothing trust guards loads its context files without a question */
    put("r5/w/proj/AGENTS.md", "R5-RULES");
    CHECK_INT(app_run("r5/w/proj", "r5/agent", 0, 0, yes), 0);
    CHECK(strstr(out.p, "Trust project folder?") == 0 && sent("R5-RULES"));
    /* the flags are pi's: an unknown one is refused, -a and -na parse */
    CHECK_INT(app_run("r5/w/proj", "r5/agent", "--approved", 1, 0), 1);
}

/* ---- skills (plan phase I): tests/pi/skills/layouts.json trees, loaded and compared with what pi 1.1.0's own code gave
 * (tools/pi_oracle/gen_skills.mjs: DefaultResourceLoader, loadSkills, buildSystemPromptSections, _expandSkillCommand) ---- */

static pi_tool t_grep = { "grep", "", "{}", "Search file contents", 0, 0, 0, no_guide, 0 };

/* a string member of a JSON object, malloc'ed, 0 absent */
static char *jstr(jv o, const char *key)
{
    jv v;
    long n;
    return json_get(o, key, &v) && json_type(v) == J_STR ? json_strdup(v, &n) : 0;
}

/* the member key of o equals got (shown when it does not) */
static int same(jv o, const char *key, const char *got, const char *layout, const char *what)
{
    char *want = jstr(o, key);
    int ok = want != 0 && !strcmp(want, got);
    if (!ok)
        printf("  skills %s %s: want [%s] got [%s]\n", layout, what, want ? want : "(none)", got);
    free(want);
    return ok;
}

static int diag_type(const char *t)
{
    return !strcmp(t, "warning") ? SKD_WARNING : !strcmp(t, "error") ? SKD_ERROR : SKD_COLLISION;
}

/* one layout: the tree made, loaded, and every result compared with NAME.json */
static void skills_layout(const char *name, jv lay)
{
    char agent[400], cwd[400], rel[500], paths[8][300], dir[400];
    const char *pp[8];
    char *g, *cwd_rel;
    long gn;
    jv gv, f, k, v, item, o2;
    jit it;
    pi_skills sk;
    pi_skill_opts o;
    int i, np = 0;
    cl_copy(dir, name, sizeof(dir));
    json_get(lay, "files", &f);
    json_iter(f, &it);
    while (json_next(&it, &k, &v)) {
        char *p = json_strdup(k, 0), *text = json_strdup(v, 0);
        cl_copy(rel, name, sizeof(rel));
        cl_cat(rel, "/", sizeof(rel));
        cl_cat(rel, p ? p : "", sizeof(rel));
        put(rel, text ? text : "");
        free(p);
        free(text);
    }
    cwd_rel = jstr(lay, "cwd");
    cl_copy(rel, name, sizeof(rel));
    cl_cat(rel, "/", sizeof(rel));
    cl_cat(rel, cwd_rel ? cwd_rel : "", sizeof(rel));
    at(cwd, sizeof(cwd), rel);
    mkdirs(cwd);
    cl_copy(rel, name, sizeof(rel));
    cl_cat(rel, "/agent", sizeof(rel));
    at(agent, sizeof(agent), rel);
    mkdirs(agent);
    free(cwd_rel);
    if (json_get(lay, "skillPaths", &v) && json_type(v) == J_ARR) {
        json_iter(v, &it);
        while (json_next(&it, 0, &item) && np < 8) {
            json_str(item, paths[np], sizeof(paths[0]));
            pp[np] = paths[np];
            np++;
        }
    }
    memset(&o, 0, sizeof(o));
    o.agent_dir = agent;
    o.cwd = cwd;
    o.trusted = json_get(lay, "trusted", &v) && json_type(v) == J_TRUE;
    o.none = json_get(lay, "noSkills", &v) && json_type(v) == J_TRUE;
    o.paths = pp;
    o.npaths = np;
    CHECK_INT(pi_skills_load(&sk, &sys, &o), 0);
    cl_copy(rel, name, sizeof(rel));
    cl_cat(rel, ".json", sizeof(rel));
    g = golden_in("skills", rel, &gn);
    CHECK(g != 0);
    if (!g || json_parse(g, gn, &gv)) {
        CHECK(0);
        pi_skills_free(&sk);
        free(g);
        return;
    }
    /* the skills, in pi's order, with their source labels and the slash menu's descriptions */
    json_get(gv, "skills", &v);
    CHECK_INT(json_count(v), sk.n);
    json_iter(v, &it);
    for (i = 0; json_next(&it, 0, &item) && i < sk.n; i++) {
        char label[40], cname[200], cdesc[1400];
        CHECK(same(item, "name", sk.s[i].name, name, "name"));
        CHECK(same(item, "description", sk.s[i].desc, name, "description"));
        CHECK(same(item, "filePath", sk.s[i].path, name, "filePath"));
        CHECK(same(item, "baseDir", sk.s[i].base, name, "baseDir"));
        CHECK_INT(json_get(item, "disableModelInvocation", &o2) && json_type(o2) == J_TRUE, sk.s[i].hidden);
        pi_skill_label(&sk.s[i], label, sizeof(label));
        CHECK(same(item, "label", label, name, "label"));
        pi_skill_command(&sk.s[i], cname, sizeof(cname), cdesc, sizeof(cdesc));
        CHECK(same(item, "command", cdesc, name, "slash menu description"));
    }
    /* the diagnostics: pi's messages, paths and order */
    json_get(gv, "diagnostics", &v);
    CHECK_INT(json_count(v), sk.nd);
    json_iter(v, &it);
    for (i = 0; json_next(&it, 0, &item) && i < sk.nd; i++) {
        char *t = jstr(item, "type");
        CHECK(t != 0 && diag_type(t) == sk.d[i].type);
        free(t);
        CHECK(same(item, "message", sk.d[i].msg, name, "diagnostic message"));
        CHECK(same(item, "path", sk.d[i].path, name, "diagnostic path"));
        if (json_get(item, "collision", &o2)) {
            CHECK(same(o2, "name", sk.d[i].name ? sk.d[i].name : "", name, "collision name"));
            CHECK(same(o2, "winnerPath", sk.d[i].winner, name, "collision winner"));
            CHECK(same(o2, "loserPath", sk.d[i].path, name, "collision loser"));
        }
    }
    CHECK_INT(json_get(gv, "enableSkillCommands", &v) && json_type(v) == J_TRUE, sk.commands);
    /* get_commands' skills (rpc-mode.ts:701-708), with each skill's sourceInfo */
    {
        jw cj;
        jw_init(&cj);
        jw_rawz(&cj, "[");
        pi_skills_commands(&sk, &cj);
        jw_rawz(&cj, "]");
        CHECK(same(gv, "rpcCommands", cj.p, name, "get_commands"));
        jw_free(&cj);
    }
    /* the prompt's "skills" section for each tool set, through pi_prompt_sections */
    if (json_get(gv, "sections", &v)) {
        json_iter(v, &it);
        while (json_next(&it, &k, &item)) {
            char keyname[80], *w, *plus;
            pi_res r;
            pi_psec sec;
            int j, found = -1;
            json_str(k, keyname, sizeof(keyname));
            memset(&ps_s, 0, sizeof(ps_s));
            for (w = keyname; w && *w;) {
                plus = strchr(w, '+');
                if (plus)
                    *plus = 0;
                ps_s.tools[ps_s.ntools++] = !strcmp(w, "read") ? &t_read : !strcmp(w, "bash") ? &t_bash : &t_grep;
                w = plus ? plus + 1 : 0;
            }
            memset(&r, 0, sizeof(r));
            r.skills = sk;
            ps_s.res = &r;
            CHECK_INT(pi_prompt_sections(&ps_s, cwd, &sec), 0);
            for (j = 0; j < sec.n; j++)
                if (!strcmp(sec.name[j], "skills"))
                    found = j;
            if (json_type(item) == J_NULL)
                CHECK(found < 0);
            else {
                char *want = json_strdup(item, &gn);
                CHECK(found >= 0 && want != 0 && sec.len[found] == gn && !memcmp(sec.text.p + sec.off[found], want, (size_t)gn));
                if (found >= 0 && want && (sec.len[found] != gn || memcmp(sec.text.p + sec.off[found], want, (size_t)gn)))
                    printf("  skills %s section [%s]:\n--- want\n%s\n--- got\n%.*s\n", name, keyname, want, (int)sec.len[found],
                           sec.text.p + sec.off[found]);
                free(want);
            }
            pi_psec_free(&sec);
        }
    }
    /* /skill:name args: pi's expansion, or the text as typed */
    if (json_get(gv, "expand", &v)) {
        json_iter(v, &it);
        while (json_next(&it, 0, &item)) {
            char *in = jstr(item, "input"), *want = jstr(item, "output");
            jw w;
            int rc;
            jw_init(&w);
            rc = pi_skill_expand(&sk, in ? in : "", &w);
            if (in && want && !strcmp(in, want))
                CHECK_INT(rc, 0);               /* not a skill command, or an unknown skill: passed through */
            else {
                CHECK_INT(rc, 1);
                CHECK(want != 0 && w.p != 0 && !strcmp(w.p, want));
                if (rc == 1 && want && w.p && strcmp(w.p, want))
                    printf("  skills %s expand [%s]:\n--- want\n%s\n--- got\n%s\n", name, in, want, w.p);
            }
            jw_free(&w);
            free(in);
            free(want);
        }
    }
    pi_skills_free(&sk);
    free(g);
}

static void test_skills_goldens(void)
{
    char *b;
    long bn;
    jv all, lay, k;
    jit it;
    long reads = pi_skills_reads;
    b = slurp_text(PI_TESTS "/pi/skills/layouts.json", &bn);
    CHECK(b != 0);
    if (!b || json_parse(b, bn, &all)) {
        CHECK(0);
        free(b);
        return;
    }
    json_iter(all, &it);
    while (json_next(&it, &k, &lay)) {
        char name[64];
        json_str(k, name, sizeof(name));
        skills_layout(name, lay);
    }
    CHECK(pi_skills_reads > reads);
    free(b);
}

/* --skill and --no-skills on the command line (pi cli/args.ts:193-195, 210-211) */
static void test_skills_args(void)
{
    pi_args a;
    char *argv1[] = { "amiga-pi", "--skill", "one", "-ns", "--skill", "two.md", "go", 0 };
    char *argv2[] = { "amiga-pi", "--skill", 0 };
    CHECK_INT(pi_args_parse(&a, 7, argv1), 0);
    CHECK_INT(a.nskills, 2);
    CHECK_STR(a.skills[0], "one");
    CHECK_STR(a.skills[1], "two.md");
    CHECK_INT(a.no_skills, 1);
    CHECK_STR(a.prompt, "go");
    CHECK_INT(pi_args_parse(&a, 2, argv2), -1);        /* a trailing --skill is an unknown option, as pi's */
    CHECK(strstr(a.err, "Unknown option: --skill") != 0);
}

/* the escaped form of text as it stands inside a JSON string of the request body */
static char *json_inner(const char *text)
{
    jw w;
    char *r;
    jw_init(&w);
    jw_str(&w, text, (long)strlen(text));
    r = (char *)malloc((size_t)w.n);
    memcpy(r, w.p + 1, (size_t)w.n - 2);
    r[w.n - 2] = 0;
    jw_free(&w);
    return r;
}

/* I5/I2/I4/I3 through pi_app -> pi_prompt: the "reach" layout (two skills the model sees, one it does not) in the request */
static void test_reach_skills(void)
{
    long reads = pi_skills_reads, gn;
    char *g = golden_in("skills", "reach.json", &gn), *sec = 0, *exp1 = 0, *exp2 = 0, *esc;
    jv gv, v, item;
    jit it;
    char p[420];
    const char *s, *c, *pc;
    CHECK(g != 0 && json_parse(g, gn, &gv) == 0);
    if (!g)
        return;
    json_get(gv, "sections", &v);
    json_get(v, "read+bash", &item);
    sec = json_strdup(item, &gn);
    json_get(gv, "expand", &v);
    json_iter(v, &it);
    json_next(&it, 0, &item);
    exp1 = jstr(item, "output");
    json_next(&it, 0, &item);
    exp2 = jstr(item, "output");
    /* the section, between the project context and cwd, with the hidden skill left out */
    CHECK_INT(app_run("reach/w/proj", "reach/agent", 0, 1, 0), 0);
    CHECK_INT(pi_skills_reads - reads, 3);              /* the sentinel: skills.c read the three SKILL.md files */
    CHECK_INT(ps.nreq, 1);
    esc = json_inner(sec);
    CHECK(sent(esc));
    free(esc);
    CHECK(sent("<name>alpha</name>") && sent("<name>zeta</name>") && !sent("<name>hid</name>"));
    s = pos("<skills>");
    c = pos("<cwd>");
    pc = pos("expert coding assistant");
    CHECK(s != 0 && c != 0 && pc != 0 && pc < s && s < c);
    /* /skill:name args: the block and the arguments are the user's message */
    app_prompt = "/skill:alpha review it";
    CHECK_INT(app_run("reach/w/proj", "reach/agent", 0, 1, 0), 0);
    esc = json_inner(exp1);
    CHECK(sent(esc));
    free(esc);
    CHECK(!sent("/skill:alpha"));
    /* a hidden skill is not in the section, and /skill: loads it all the same */
    app_prompt = "/skill:hid";
    CHECK_INT(app_run("reach/w/proj", "reach/agent", 0, 1, 0), 0);
    esc = json_inner(exp2);
    CHECK(sent(esc));
    free(esc);
    /* an unknown skill goes to the model as typed */
    app_prompt = "/skill:nope x";
    CHECK_INT(app_run("reach/w/proj", "reach/agent", 0, 1, 0), 0);
    CHECK(sent("/skill:nope x"));
    /* --no-skills: nothing is read, no section; --skill brings one back */
    reads = pi_skills_reads;
    CHECK_INT(app_run("reach/w/proj", "reach/agent", "--no-skills", 1, 0), 0);
    CHECK_INT(pi_skills_reads - reads, 0);
    CHECK(!sent("<skills>") && !sent("<available_skills>"));
    app_flag2 = "--skill";
    app_value2 = "../../agent/skills/zeta";
    CHECK_INT(app_run("reach/w/proj", "reach/agent", "--no-skills", 1, 0), 0);
    CHECK(sent("<name>zeta</name>") && !sent("<name>alpha</name>"));
    /* the file is the knob: without it the section loses the skill */
    at(p, sizeof(p), "reach/agent/skills/alpha/SKILL.md");
    CHECK_INT(remove(p), 0);
    CHECK_INT(app_run("reach/w/proj", "reach/agent", 0, 1, 0), 0);
    CHECK(!sent("<name>alpha</name>") && sent("<name>zeta</name>"));
    /* no skills at all: no section */
    at(p, sizeof(p), "reach/agent/skills/zeta/SKILL.md");
    CHECK_INT(remove(p), 0);
    at(p, sizeof(p), "reach/agent/skills/hid/SKILL.md");
    CHECK_INT(remove(p), 0);
    CHECK_INT(app_run("reach/w/proj", "reach/agent", 0, 1, 0), 0);
    CHECK(!sent("<skills>"));
    free(sec);
    free(exp1);
    free(exp2);
    free(g);
}

void suite_pi_resources(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_res_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    sys_posix_init(&sp, &sys);
    jw_init(&out);
    test_goldens();
    test_trust_store();
    test_decide();
    test_trust_errors();
    test_reach_context();
    test_agent_dir_alias();
    test_reach_trust();
    test_reach_trust_error();
    test_skills_goldens();
    test_skills_args();
    test_reach_skills();
    jw_free(&out);
    ps_reset();
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
