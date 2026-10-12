/* amiga-pi's prompt templates (plan phase J; pi 1.1.0 core/prompt-templates.ts):
 *   subst     J1: 73 substitution cases and 20 /name expansions, every expected value made by pi's own
 *             parseCommandArgs, substituteArgs and expandPromptTemplate (tests/pi/templates/cases.json,
 *             tools/pi_oracle/gen_templates.mjs);
 *   discover  J2: the agent directory's prompts/ and a trusted project's .pi/prompts/, the project's name wins,
 *             hidden files and other extensions left out, frontmatter and first-line descriptions, the menu rows;
 *   reach     J3: pi_app -> pi_prompt -> the request body carries the expansion of "/review src" (the sentinels:
 *             files read, expansions made, prompts sent); trust, --no-prompt-templates and --prompt-template.
 * The get_commands record and the prompts through RPC are in test_pi_modes.c (scenario prompt_templates). */
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
#include "../amiga-pi/templates.h"

static char root[400];
static sys_posix sp;
static cl_sys sys;
static jw out;

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

/* ---- J1: the table ---- */

static int same_args(char **got, int ng, jv want)
{
    jit it;
    jv e;
    int i = 0, ok = 1;
    json_iter(want, &it);
    while (json_next(&it, 0, &e)) {
        char *w = json_strdup(e, 0);
        ok = ok && i < ng && w && !strcmp(got[i], w);
        free(w);
        i++;
    }
    return ok && i == ng;
}

static void test_table(void)
{
    long n;
    char *txt = slurp_text(PI_TESTS "/pi/templates/cases.json", &n);
    jv doc, sub, ex, cs, tpls, e, v;
    jit it;
    int cases = 0;
    memset(&tpls, 0, sizeof(tpls));
    memset(&cs, 0, sizeof(cs));
    CHECK(txt != 0);
    if (!txt)
        return;
    CHECK_INT(json_parse(txt, n, &doc), 0);
    CHECK(json_get(doc, "subst", &sub));
    json_iter(sub, &it);
    while (json_next(&it, 0, &e)) {
        char *c = 0, *a = 0, *want = 0, **args;
        int na;
        jw o;
        jv x;
        if (json_get(e, "c", &x))
            c = json_strdup(x, 0);
        if (json_get(e, "a", &x))
            a = json_strdup(x, 0);
        if (json_get(e, "out", &x))
            want = json_strdup(x, 0);
        CHECK(c && a && want);
        if (c && a && want) {
            jw_init(&o);
            CHECK_INT(pi_tmpl_args(a, &args, &na), 0);
            CHECK(json_get(e, "args", &x) && same_args(args, na, x));
            pi_tmpl_subst(&o, c, args, na);
            if (strcmp(o.p ? o.p : "", want)) {
                printf("  case %d: \"%s\" with \"%s\": got \"%s\", pi \"%s\"\n", cases, c, a, o.p ? o.p : "", want);
                CHECK(0);
            } else
                CHECK(1);
            pi_tmpl_args_free(args, na);
            jw_free(&o);
        }
        free(c);
        free(a);
        free(want);
        cases++;
    }
    CHECK(cases >= 20);
    CHECK_INT(cases, 73);
    /* the expansions: a table of templates, the text typed, pi's answer (the text itself when no template) */
    CHECK(json_get(doc, "expand", &ex) && json_get(ex, "templates", &tpls) && json_get(ex, "cases", &cs));
    {
        pi_tmpls ts;
        int k = 0, expand = 0;
        memset(&ts, 0, sizeof(ts));
        ts.t = (pi_tmpl *)calloc(8, sizeof(pi_tmpl));
        json_iter(tpls, &it);
        while (json_next(&it, 0, &e) && k < 8) {
            char *nm = 0;
            if (json_get(e, "name", &v))
                nm = json_strdup(v, 0);
            cl_copy(ts.t[k].name, nm ? nm : "", sizeof(ts.t[k].name));
            free(nm);
            if (json_get(e, "content", &v))
                ts.t[k].content = json_strdup(v, 0);
            k++;
        }
        ts.n = k;
        json_iter(cs, &it);
        while (json_next(&it, 0, &e)) {
            char *text = 0, *want = 0;
            jw o;
            int hit;
            if (json_get(e, "text", &v))
                text = json_strdup(v, 0);
            if (json_get(e, "out", &v))
                want = json_strdup(v, 0);
            CHECK(text && want);
            if (text && want) {
                jw_init(&o);
                hit = pi_tmpl_expand(&ts, text, &o);
                if (hit)
                    CHECK_STR(o.p ? o.p : "", want);
                else
                    CHECK_STR(text, want);          /* not a template: pi returns the text as it is */
                jw_free(&o);
            }
            free(text);
            free(want);
            expand++;
        }
        CHECK_INT(expand, 20);
        while (k--)
            free(ts.t[k].content);
        free(ts.t);
    }
    free(txt);
}

/* ---- the trees ---- */

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

static void at(char *o, long cap, const char *rel)
{
    cl_copy(o, root, cap);
    cl_cat(o, "/", cap);
    cl_cat(o, rel, cap);
}

static void put(const char *rel, const char *text)
{
    char p[600], d[600];
    char *sl;
    at(p, sizeof(p), rel);
    cl_copy(d, p, sizeof(d));
    sl = strrchr(d, '/');
    *sl = 0;
    mkdirs(d);
    CHECK_INT(sys.write(sys.u, p, text, (long)strlen(text)), 0);
}

static const pi_tmpl *find(const pi_tmpls *ts, const char *name)
{
    int i;
    for (i = 0; i < ts->n; i++)
        if (!strcmp(ts->t[i].name, name))
            return &ts->t[i];
    return 0;
}

/* ---- J2: discovery ---- */

static void test_discover(void)
{
    pi_tmpls ts;
    char agent[400], cwd[400], pth[1][300];
    const pi_tmpl *t;
    const cl_cmd *m;
    int n;
    put("d1/agent/prompts/review.md", "---\ndescription: Review code\nargument-hint: \"<path> [focus]\"\n---\nUser review of $1\n");
    put("d1/agent/prompts/sum.md", "---\r\ndescription: 'Sum it'\r\n---\r\n\r\n  body $@  \r\n\r\n");
    put("d1/agent/prompts/long.md", "\n   \nThis first line is much longer than sixty characters, so it is cut and dots follow\nsecond\n");
    put("d1/agent/prompts/.hidden.md", "hidden\n");
    put("d1/agent/prompts/notes.txt", "not a template\n");
    put("d1/agent/prompts/sub/deep.md", "not scanned: the directory is not recursive\n");
    put("d1/w/.pi/prompts/review.md", "---\ndescription: Project review\n---\nProject review of $ARGUMENTS\n");
    put("d1/w/.pi/prompts/fix.md", "Fix $1\n");
    at(agent, sizeof(agent), "d1/agent");
    at(cwd, sizeof(cwd), "d1/w");
    /* trusted: the project's first, its review wins; the agent directory's others follow, by name */
    CHECK_INT(pi_tmpl_load(&ts, &sys, agent, cwd, 1, 0, 0, 0), 0);
    CHECK_INT(ts.n, 4);
    CHECK_STR(ts.t[0].name, "fix");
    CHECK_STR(ts.t[1].name, "review");
    CHECK_STR(ts.t[2].name, "long");
    CHECK_STR(ts.t[3].name, "sum");
    t = find(&ts, "review");
    CHECK(t && t->scope == PI_TMPL_PROJECT && !strcmp(t->content, "Project review of $ARGUMENTS"));
    CHECK(t && !strcmp(t->description, "Project review") && !strcmp(t->hint, ""));
    t = find(&ts, "fix");
    CHECK(t && !strcmp(t->description, "Fix $1") && !strcmp(t->content, "Fix $1\n"));         /* no frontmatter: the text as it is */
    t = find(&ts, "long");
    CHECK(t && !strcmp(t->description, "This first line is much longer than sixty characters, so it ..."));
    t = find(&ts, "sum");
    CHECK(t && t->scope == PI_TMPL_USER && !strcmp(t->description, "Sum it") && !strcmp(t->content, "body $@"));   /* CRLF, trimmed */
    CHECK(!find(&ts, ".hidden") && !find(&ts, "notes") && !find(&ts, "deep"));
    m = pi_tmpl_menu(&ts, &n);
    CHECK_INT(n, 4);
    CHECK_STR(m[0].name, "/fix");
    CHECK_STR(m[0].help, "[p] Fix $1");
    CHECK_STR(m[1].help, "[p] Project review");
    CHECK_STR(m[3].help, "[u] Sum it");
    pi_tmpl_free(&ts);
    /* not trusted: the project's directory is not read; the user's review is there */
    CHECK_INT(pi_tmpl_load(&ts, &sys, agent, cwd, 0, 0, 0, 0), 0);
    CHECK_INT(ts.n, 3);
    t = find(&ts, "review");
    CHECK(t && t->scope == PI_TMPL_USER && !strcmp(t->hint, "<path> [focus]"));
    CHECK(!find(&ts, "fix"));
    m = pi_tmpl_menu(&ts, &n);
    CHECK_STR(m[1].name, "/review");
    CHECK_STR(m[1].help, "<path> [focus] \xe2\x80\x94 [u] Review code");
    pi_tmpl_free(&ts);
    /* --no-prompt-templates with --prompt-template: only the path's, the directory read whole */
    put("d1/w/extra/hidden-ok.md", "x\n");
    put("d1/w/extra/.dot.md", "dot\n");
    cl_copy(pth[0], "extra", sizeof(pth[0]));
    CHECK_INT(pi_tmpl_load(&ts, &sys, agent, cwd, 1, 1, pth, 1), 0);
    CHECK_INT(ts.n, 2);
    CHECK(find(&ts, "hidden-ok") && find(&ts, ".dot") && find(&ts, "hidden-ok")->scope == PI_TMPL_PATH);
    m = pi_tmpl_menu(&ts, &n);
    CHECK_STR(m[1].help, "[t] x");
    pi_tmpl_free(&ts);
    /* a path that is one file; it wins a name over the defaults */
    put("d1/w/one.md", "Path review\n");
    cl_copy(pth[0], "one.md", sizeof(pth[0]));
    CHECK_INT(pi_tmpl_load(&ts, &sys, agent, cwd, 1, 0, pth, 1), 0);
    CHECK(find(&ts, "one") && find(&ts, "review")->scope == PI_TMPL_PROJECT);
    CHECK_STR(ts.t[0].name, "one");
    pi_tmpl_free(&ts);
    /* nothing there: no templates, no menu */
    CHECK_INT(pi_tmpl_load(&ts, &sys, "/nonexistent-agent", "/nonexistent-cwd", 1, 0, 0, 0), 0);
    CHECK_INT(ts.n, 0);
    m = pi_tmpl_menu(&ts, &n);
    CHECK(n == 0 && m == 0);
    pi_tmpl_free(&ts);
}

/* ---- J3: through pi_app ---- */

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

/* pi_app in root/cwd_rel with the agent directory root/agent_rel, the message `prompt`; opt1/opt2 two more flags (0 none) */
static int app_run(const char *cwd_rel, const char *agent_rel, const char *prompt, const char *opt1, const char *opt2)
{
    cl_io io;
    pi_platform pl;
    char cwd[400], agent[400], tmp[420], *argv[16];
    int a = 0;
    at(cwd, sizeof(cwd), cwd_rel);
    at(agent, sizeof(agent), agent_rel);
    mkdirs(cwd);
    mkdirs(agent);
    cl_copy(tmp, root, sizeof(tmp));
    cl_cat(tmp, "/", sizeof(tmp));
    memset(&io, 0, sizeof(io));
    io.write = l_write;
    io.brk = l_brk;
    jw_free(&out);
    jw_init(&out);
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
    argv[a++] = "-p";
    argv[a++] = "--no-session";
    argv[a++] = "--no-mcp";
    if (opt1)
        argv[a++] = (char *)opt1;
    if (opt2)
        argv[a++] = (char *)opt2;
    argv[a++] = "--model";
    argv[a++] = "fixture/qwen2.5-coder:7b";
    argv[a++] = "--base-url";
    argv[a++] = "http://192.168.1.10:11434/v1";
    argv[a++] = (char *)prompt;
    argv[a] = 0;
    return pi_app(a, argv, &pl);
}

static int sent(const char *needle)
{
    return ps.nreq >= 1 && strstr(ps.body[0], needle) != 0;
}

static void test_reach(void)
{
    long reads, exps, prompts;
    put("r1/agent/prompts/review.md", "---\ndescription: Review\n---\nPlease review USER-$1 with ${2:-nothing else}.");
    put("r1/w/.pi/prompts/review.md", "Please review PROJECT-$1 with ${2:-nothing else}.");
    /* the project is trusted by -a: its review answers, the argument in place, the default used */
    reads = pi_tmpl_reads;
    exps = pi_tmpl_expansions;
    prompts = pi_agent_prompts;
    CHECK_INT(app_run("r1/w", "r1/agent", "/review src", "-a", 0), 0);
    CHECK_INT(pi_tmpl_reads - reads, 2);                /* the sentinels: the loader read both files, */
    CHECK_INT(pi_tmpl_expansions - exps, 1);            /* one /name was expanded, */
    CHECK_INT(pi_agent_prompts - prompts, 1);           /* and pi_prompt sent it */
    CHECK_INT(ps.nreq, 1);
    CHECK(sent("Please review PROJECT-src with nothing else."));
    CHECK(!sent("/review src"));                        /* the typed line itself is not what the model got */
    /* the knobs: not trusted, the user's template answers */
    CHECK_INT(app_run("r1/w", "r1/agent", "/review src", "-na", 0), 0);
    CHECK(sent("Please review USER-src with nothing else."));
    /* --no-prompt-templates: no template, the line goes as typed */
    exps = pi_tmpl_expansions;
    CHECK_INT(app_run("r1/w", "r1/agent", "/review src", "-a", "--no-prompt-templates"), 0);
    CHECK(sent("/review src") && !sent("review PROJECT") && !sent("review USER"));
    CHECK_INT(pi_tmpl_expansions - exps, 0);
    /* --prompt-template: the path's template, ahead of the agent directory's (print mode does not trust the project) */
    put("r1/w/more/review.md", "Path review of $@");
    CHECK_INT(app_run("r1/w", "r1/agent", "/review a b", "--prompt-template", "more"), 0);
    CHECK(sent("Path review of a b"));
    /* a name that is no template goes through untouched (pi: the text itself) */
    CHECK_INT(app_run("r1/w", "r1/agent", "/nosuch a", "-a", 0), 0);
    CHECK(sent("/nosuch a"));
}

void suite_pi_templates(void)
{
    const char *tmp = getenv("TMPDIR");
    strcpy(root, tmp && tmp[0] ? tmp : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_tmpl_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    sys_posix_init(&sp, &sys);
    jw_init(&out);
    test_table();
    test_discover();
    test_reach();
    jw_free(&out);
    ps_reset();
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
