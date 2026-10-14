/* amiga-pi's screen against pi 1.0.4's (plan N0, N7): pi_ui_run() through C:Claude's screen harness
 * (tests/claude_screen.c, the engine as the terminal) with the answers pi was given (the .sse files of tests/pi/tui), the
 * screen compared cell by cell -- text, colours and attributes -- with the capture of the real pi
 * (the .golden files of tests/pi/tui, tools/pi_oracle/tui_gen.py). A cell that differs on purpose is listed in the
 * scenario's masks, each with the COMPATIBILITY.md row that explains it. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "pi_test.h"
#include "claude/util.h"
#include "pi_stub.h"
#include "claude_screen.h"
#include "llm/transform.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/ui_pi.h"
#include "../amiga-pi/show_pi.h"
#include "../amiga-pi/fuzzy.h"
#include "../amiga-pi/login.h"
#include "../amiga-pi/okcolor.h"
#include "../amiga-pi/themes_builtin.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/session3.h"
#include "../amiga-pi/context.h"
#include "../amiga-pi/compact.h"
#include "../amiga-pi/pstatus.h"
#include "../amiga-pi/templates.h"
#include "../amiga-pi/settings_diag.h"
#include "../amiga-pi/tools_pi.h"
#include "claude/sys_posix.h"
#include "claude/theme.h"
#include "../amiga-pi/changelog.h"
#include "view/hl_style.h"
#include <unistd.h>
#include <sys/stat.h>

/* a mask: rows r0..r1 (1-based, golden), columns c0..c1 (0-based, inclusive), the COMPATIBILITY.md row */
typedef struct mask {
    int r0, r1, c0, c1;
    const char *why;
} mask;

int test_tmpl_load(pi_tmpls *ts, cl_sys *sys, const char *agent, const char *cwd, int trusted, int no_defaults, char (*paths)[300], int npaths);

static pi_tmpls slash_tm;      /* the templates of slash_prompts */

typedef struct scen {
    const char *name;           /* the golden's name */
    const char *keys[8];        /* typed chunks; a '!' chunk is typed while the agent works */
    const char *answers[4];     /* tests/pi/tui/<file>: .sse streamed, .http whole */
    int tools;                  /* 1 read answers, 2 bash fails, 3 bash echoes, 4 bash counts to 12, 5 ls lists 25 files, 6 amiga-pi's own tools on the scenario's files */
    int header;                 /* the start header is shown (else settings.json has quietStartup)  */
    int when;                   /* 0 at rest after the keys, 1 while the agent works (after the keys), 2 while a tool runs, 4 at rest like 0 with the clock running (a turn long enough for C:Claude's notice),
                                 * 3 the screen pi leaves on the main screen when it ends (the golden's .txt) */
    mask masks[12];
} scen;
/* what settings.json holds for a scenario follows from its name, as tools/pi_oracle/tui_gen.py wrote it: reg_*
 * tuiMode "regular" (else pi's default, fullscreen), dark_* / light_* the theme, *_bar fullscreenScrollbar "always" */
#define SC_REGULAR(sc) (!strncmp((sc)->name, "reg_", 4) || !strcmp((sc)->name, "exit_regular"))
#define SC_BAR(sc) (strstr((sc)->name, "_bar") != 0)
#define SC_HINT(sc) (strstr((sc)->name, "exit_hint") != 0)     /* fullscreenExitOutput "resume-hint" */
#define SC_PAD0(sc) (strstr((sc)->name, "pad0") != 0)          /* settings.json outputPad 0 (pi 1.1.0) */
/* the window's width: 80, tui_gen.py's default, except the scenarios that name their own (footer: foot_<columns> in its table) */
static int sc_cols(const scen *sc)
{
    return !strcmp(sc->name, "start_w64") || !strcmp(sc->name, "pad0_start_w64") || !strcmp(sc->name, "start_expanded_w64") ? 64 :
           !strcmp(sc->name, "foot_exact") ? 45 : !strcmp(sc->name, "foot_cut") ? 41 : !strcmp(sc->name, "foot_left") ? 30 : 80;
}
#define SC_THEME(sc) (!strncmp((sc)->name, "dark_", 5) ? 1 : !strncmp((sc)->name, "light_", 6) ? 2 : 0)
static int xm_row, xm_c0, xm_c1;  /* one more masked span (row 1-based, columns), 0 none */
static const char *xm_why;
static int cmp_shift;           /* the capture's row r is on our screen at row r + cmp_shift (a selector drawn at the bottom) */
static int cmp_split = 1000;    /* ... for the rows above this one (0-based) only, the rows from it on are where they are */

static char *slurp(const char *name, long *n)
{
    char path[600];
    FILE *f;
    char *b;
    strcpy(path, PI_TESTS "/pi/tui/");
    strcat(path, name);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)*n + 1);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
        free(b);
        b = 0;
    }
    if (b)
        b[*n] = 0;
    fclose(f);
    return b;
}

static long echo_calls;
static void (*tool_hook)(pi_session *s);     /* what the read tool does besides answering (the tests type meanwhile) */

/* the tools under pi's names: read answers with the file, bash fails like cat of a missing file */
static int tool_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    const char *text;
    int err = 0;
    (void)args;
    (void)an;
    echo_calls++;
    if (tool_hook)
        tool_hook(s);
    if (u == (void *)2) {
        /* the bash tool of the ! scenarios: echo's text and a newline */
        static char outb[200];
        const char *c = strstr(args, "echo ");
        long k = 0;
        c = c ? c + 5 : "";
        while (*c && *c != '\"' && k < 190)
            outb[k++] = *c++;
        outb[k++] = '\\';
        outb[k++] = 'n';
        outb[k] = 0;
        text = outb;
    } else if (u == (void *)3) {
        /* R5: seq 1 12 */
        text = "1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n10\\n11\\n12\\n";
    } else if (u == (void *)4) {
        /* R5: ls of a directory with 25 files */
        static char lsb[400];
        int i;
        lsb[0] = 0;
        for (i = 1; i <= 25; i++) {
            char one[24] = "f00.txt";
            one[1] = (char)('0' + i / 10);
            one[2] = (char)('0' + i % 10);
            strcat(lsb, one);
            if (i < 25)
                strcat(lsb, "\\n");
        }
        text = lsb;
    } else if (u) {
        text = "cat: nothere.txt: No such file or directory\\n\\n\\nCommand exited with code 1";
        err = 1;
    } else
        text = "one\\ntwo\\nthree\\n";
    jw_rawz(content, "[{\"type\":\"text\",\"text\":\"");
    jw_rawz(content, text);
    jw_rawz(content, "\"}]");
    *is_error = err;
    return 0;
}

#define PARAMS "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}"
static pi_tool t_read = { "read", "Read a file", PARAMS, 0, 0, tool_run, 0, 0, 0 };
static pi_tool t_bash = { "bash", "Run a command", PARAMS, 0, (void *)1, tool_run, 0, 0, 0 };
static pi_tool t_seq = { "bash", "Run a command", PARAMS, 0, (void *)3, tool_run, 0, 0, 0 };
static pi_tool t_ls = { "ls", "List a directory", PARAMS, 0, (void *)4, tool_run, 0, 0, 0 };
static pi_tool t_bang = { "bash", "Run a command", PARAMS, 0, (void *)2, tool_run, 0, 0, 0 };

/* pi's own slash menu (core/slash-commands.js and two more entries: 26 in the capture), so the menu's layout compares */
static const cl_cmd pi_cmds[] = {
    { "/settings", "Open settings menu" }, { "/model", "<provider/model> \342\200\224 Select model (opens selector UI)" },
    { "/tree", "Navigate session tree (switch branches)" }, { "/thinking", "<level> \342\200\224 Set thinking level" },
    { "/scoped-models", "Enable/disable models for Ctrl+P cycling" },
    { "/export", "Export session (HTML default, or specify path: .html/.jsonl)" },
    { "/import", "Import and resume a session from a JSONL file" }, { "/share", "Share session as a secret GitHub gist" },
    { "/bug", "<description> \342\200\224 Report a bug to the Pi developers" }, { "/copy", "Copy last agent message to clipboard" },
    { "/name", "Set session display name" }, { "/session", "Show session info and stats" },
    { "/changelog", "Show changelog entries" }, { "/hotkeys", "Show all keyboard shortcuts" },
    { "/fork", "Create a new fork from a previous user message" },
    { "/clone", "Duplicate the current session at the current position" },
    { "/trust", "Save project trust decision for future sessions" },
    { "/login", "<provider> \342\200\224 Configure provider authentication" }, { "/logout", "Remove provider authentication" },
    { "/new", "Start a new session" }, { "/compact", "Manually compact the session context" },
    { "/resume", "Resume a different session" },
    { "/reload", "Reload keybindings, extensions, skills, prompts, themes, and context files" },
    { "/quit", "Quit pi" }, { "/skill:a", "a skill" }, { "/b", "a prompt" }
};

static llm_ts fixed_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L;
    t.ms = 0;
    return t;
}

/* ---- the golden as a screen of its own ---- */

static vt_term *want;
static int want_rows;

static int cur_cols = 80;       /* the running scenario's width */

static void load_golden(const char *name)
{
    char fn[80];
    long n, i, a = 0;
    char *g;
    int rows = 0;
    cl_copy(fn, name, sizeof(fn));
    cl_cat(fn, ".golden", sizeof(fn));
    g = slurp(fn, &n);
    want = 0;
    if (!g)
        return;
    for (i = 0; i < n; i++)
        rows += g[i] == '\n';
    want_rows = rows;
    want = h_new(cur_cols, rows, VT_XTERM);
    rows = want_rows;
    for (i = 0; i < n; i++)
        if (g[i] == '\n') {
            g[i] = 0;
            h_put(want, g + a);
            if (--rows)             /* no newline after the last row: it would scroll the screen */
                h_put(want, "\r\n");
            a = i + 1;
        }
    free(g);
}

static int masked(const scen *sc, int row, int col)
{
    int k;
    for (k = 0; k < 12 && sc->masks[k].why; k++)
        if (row >= sc->masks[k].r0 && row <= sc->masks[k].r1 && col >= sc->masks[k].c0 && col <= sc->masks[k].c1)
            return 1;
    return 0;
}

static const scen *cur;
static int taken, diffs;
static int rowdiff[64];
static char first_diff[2400];
static char snap[64][400];      /* the rows at the moment of the comparison (the run's end clears the screen) */

static void describe(vt_term *t, const vt_cell *c, char *out)
{
    vt_color fg, bg;
    vt_resolve_colors(t, c, &fg, &bg);
    snprintf(out, 96, "ch=%04x fg=%lx bg=%lx attr=%x", (unsigned)c->ch, (unsigned long)fg, (unsigned long)bg, (unsigned)c->attr);
}

static void compare(void)
{
    int r, c, shown = 0;
    taken = 1;
    for (r = 0; r < cs.rows && r < 64; r++) {
        strncpy(snap[r], cs_row(r), sizeof(snap[r]) - 1);
        snap[r][sizeof(snap[r]) - 1] = 0;
    }
    for (r = 0; r < want_rows && r < cs.rows; r++)
        for (c = 0; c < cur_cols; c++) {
            const vt_cell *a = h_cell(want, c, r), *b;
            vt_color af, ab, bf, bb;
            vt_attr mask = (vt_attr)(VT_ATTR_BOLD | VT_ATTR_FAINT | VT_ATTR_ITALIC | VT_ATTR_UNDERLINE | VT_ATTR_INVERSE | VT_ATTR_STRIKE);
            int same;
            int sh = r < cmp_split ? cmp_shift : 0;
            if (r + sh < 0 || r + sh >= cs.rows)
                continue;
            b = h_cell(cs.vt, c, r + sh);
            if (masked(cur, r + 1, c))
                continue;
            vt_resolve_colors(want, a, &af, &ab);
            vt_resolve_colors(cs.vt, b, &bf, &bb);
            same = a->ch == b->ch && (a->attr & mask) == (b->attr & mask);
            /* the colours of a blank cell without a background do not show */
            if (same && a->ch != 0x20 && (af != bf))
                same = 0;
            if (same && ab != bb)
                same = 0;
            if (!same) {
                diffs++;
                rowdiff[r]++;
                if ((getenv("PI_TUI_CELLS") ? r + 1 == atoi(getenv("PI_TUI_CELLS")) : shown++ < 8) && strlen(first_diff) < sizeof(first_diff) - 300) {
                    char d1[96], d2[96], line[300];
                    describe(want, a, d1);
                    describe(cs.vt, b, d2);
                    snprintf(line, sizeof(line), "    row %d col %d: pi [%s] amiga-pi [%s]\n", r + 1, c, d1, d2);
                    strcat(first_diff, line);
                }
            }
        }
}

/* the command pi prints on leaving; its session id is a fresh uuid in every capture (masked in exit_check) */
static const char *test_resume(void *u)
{
    (void)u;
    return "pi --session 01a130bf-0000-7000-8000-000000000000";
}

/* the text of the screen pi leaves behind (tuiMode / fullscreenExitOutput), row by row against the capture's .txt */
static void exit_check(const scen *sc)
{
    char fn[80];
    char *g, *nl;
    long n;
    int r = 0, bad = 0;
    cl_copy(fn, sc->name, sizeof(fn));
    cl_cat(fn, ".txt", sizeof(fn));
    g = slurp(fn, &n);
    CHECK(g != 0);
    if (!g)
        return;
    for (nl = g; *nl; r++) {
        char *e = strchr(nl, '\n');
        const char *want_row = nl;
        long cut = 0;
        if (e)
            *e = 0;
        if (!strncmp(want_row, "To resume this session: pi --session ", 37))
            cut = 37;       /* the id is masked */
        if (strncmp(cs_row(r), want_row, cut ? (size_t)cut : strlen(want_row) + 1) != 0) {
            bad++;
            printf("    exit row %d: pi [%s] amiga-pi [%s]\n", r + 1, want_row, cs_row(r));
        }
        if (!e)
            break;
        nl = e + 1;
    }
    CHECK_INT(bad, 0);
    free(g);
}

/* TM4: no bell and no OSC 9 in an output stream: the stream with its OSC sequences (the title ends in BEL) cut out
 * holds no BEL, and no OSC 9 was sent. 1 when quiet */
static int stream_quiet(const char *b, long n)
{
    long i;
    int quiet = 1;
    for (i = 0; i < n; i++) {
        if (b[i] == '\033' && i + 1 < n && b[i + 1] == ']') {
            if (i + 2 < n && b[i + 2] == '9' && i + 3 < n && b[i + 3] == ';')
                quiet = 0;                  /* OSC 9: a desktop notice */
            for (i += 2; i < n && b[i] != 7 && !(b[i] == '\033' && i + 1 < n && b[i + 1] == '\\'); i++)
                ;
            continue;
        }
        if (b[i] == 7)
            quiet = 0;                      /* a bare bell */
    }
    return quiet;
}

static void turn_quiet_check(void)
{
    long n = 0;
    char *raw = slurp("turn_raw.raw", &n);
    CHECK(raw != 0);
    if (raw) {
        CHECK(stream_quiet(raw, n));        /* pi 1.0.4: nothing when a turn ends */
        CHECK(strstr(raw, "late answer") != 0);     /* (the stream holds the turn: it is not empty) */
        free(raw);
    }
    CHECK(stream_quiet(cs.sent.p, cs.sent.n));
    CHECK_INT(h_bells, 0);
}

static void compare_in_tool(pi_session *s)
{
    if (!taken) {
        if (strstr(cur->name, "toolrun")) {
            /* Q9: pi's Elapsed row of a running bash call changes once a second (renderers/bash.ts:131); the golden was
             * taken a second into `sleep 4` ("Elapsed 1.0s"): the pinned clock goes a second on and the poll ticks
             * (every read of this clock is 10 ms on: 960 here, the reads since the start and this one make up the rest) */
            if (strstr(cur->name, "toolrun_out")) {
                /* the bash tool's update with the output so far (Q6: tool_execution_update per chunk) */
                pi_event e;
                pi_event_init(&e, PE_TOOL_UPDATE);
                e.tool_id = s->call_id;
                e.tool_name = "bash";
                e.args = "{}";
                e.args_n = 2;
                e.result = "[{\"type\":\"text\",\"text\":\"hi\\n\"}]";
                if (strstr(cur->name, "expanded"))      /* R5: seq 1 12 so far */
                    e.result = "[{\"type\":\"text\",\"text\":\"1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\\n10\\n11\\n12\\n\"}]";
                e.result_n = (long)strlen(e.result);
                pi_emit(&s->em, &e);
            }
            cs.clock += 960;
            if (strstr(cur->name, "expanded")) {
                /* R5: Ctrl+O is typed while the tool runs (the script would deliver it while the request is awaited); the
                 * golden was taken right after it, at "Elapsed 2.0s" */
                static const char *ctrl_o[] = { "!\017", "\004", 0 };           /* ! : typed while the agent works */
                cs.script = ctrl_o;
                cs.next = 0;
                cs.clock += 900;
            }
            s->h.poll(s->h.u);
        }
        compare();
    }
    tool_hook = 0;
}

/* Q10: pi runs the tool calls of one message in parallel, and a box takes the colour of its own result whenever that
 * arrives. amiga-pi's agent runs them one by one, so the second call's events are made here: it starts and ends while
 * the first one runs, and the agent's end of the first comes after, with the first box no longer the last thing on
 * the screen. The golden is pi's screen for both calls (dark_tools2; tools/pi_oracle/tui_gen.py). */
static void nested_tools(pi_session *s)
{
    static const char args[] = "{\"command\":\"echo hi\"}";
    static const char res[] = "[{\"type\":\"text\",\"text\":\"hi\\n\"}]";
    pi_event e;
    pi_event_init(&e, PE_TOOL_START);
    e.tool_id = "toolu_p2";
    e.tool_name = "bash";
    e.args = args;
    e.args_n = (long)strlen(args);
    pi_emit(&s->em, &e);
    pi_event_init(&e, PE_TOOL_END);
    e.tool_id = "toolu_p2";
    e.tool_name = "bash";
    e.args = args;
    e.args_n = (long)strlen(args);
    e.result = res;
    e.result_n = (long)strlen(res);
    e.duration_ms = 0;
    pi_emit(&s->em, &e);
    tool_hook = 0;
}

static void hook(void)
{
    int r, up = 0;
    for (r = 0; r < cs.rows; r++)           /* the footer is up (regular mode: not on the last row) */
        up |= strstr(cs_row(r), "fake-m") != 0 || strstr(cs_row(r), "/200k") != 0;     /* (cut at the narrow widths) */
    if (cur->when == 4)
        cs.clock += 12000;            /* every read of a long turn is twelve seconds on: C:Claude would ring at its end */
    if (taken || !up || cur->when == 2 || cur->when == 3)     /* when 2: compare_in_tool takes the screen */
        return;
    if (cur->when == 0 || cur->when == 4) {
        if (cs.read_ms > 0 && cs.script[cs.next] && !strcmp(cs.script[cs.next], "\004"))
            compare();
    } else if (cs.read_ms == 0 && cs.script[cs.next] && !strcmp(cs.script[cs.next], "\004") && cs.next > 0) {
        compare();
        cs.brk = 1;
    }
}

static void dump_rows(void)
{
    int r;
    for (r = 0; r < cs.rows && r < 64; r++)
        printf("    %2d|%s\n", r + 1, snap[r]);
}

/* U1b: the scenarios whose tools are amiga-pi's own (tools 6) run them on the files tui_gen.py gave pi (tests/pi/tui/scenario_files.json, and
 * the notes.txt every project has), in a directory of their own */
static pi_tools real_pt;
static sys_posix real_sp;
static cl_sys real_sys;
static char real_dir[80];

static void real_tools_up(pi_session *ses, const char *scenario)
{
    char *b, p[300], d[300], *e, err[200];
    long n;
    jv all, files, k, v;
    jit it;
    FILE *f;
    strcpy(real_dir, "/tmp/pit_XXXXXX");
    if (!mkdtemp(real_dir)) {
        CHECK(0);
        return;
    }
    strcpy(p, real_dir);
    strcat(p, "/notes.txt");
    f = fopen(p, "wb");
    if (f) {
        fputs("one\ntwo\nthree\n", f);
        fclose(f);
    }
    b = slurp("scenario_files.json", &n);
    CHECK(b != 0);
    if (b && !json_parse(b, n, &all) && json_get(all, scenario, &files)) {
        json_iter(files, &it);
        while (json_next(&it, &k, &v)) {
            char *rel = json_strdup(k, 0), *text = json_strdup(v, 0);
            strcpy(p, real_dir);
            strcat(p, "/");
            strcat(p, rel ? rel : "");
            strcpy(d, p);
            e = strrchr(d, '/');
            for (e = d + strlen(real_dir) + 1; *e; e++)
                if (*e == '/') {
                    *e = 0;
                    mkdir(d, 0777);
                    *e = '/';
                }
            f = fopen(p, "wb");
            if (f) {
                fputs(text ? text : "", f);
                fclose(f);
            }
            free(rel);
            free(text);
        }
    }
    free(b);
    sys_posix_init(&real_sp, &real_sys);
    strcpy(d, real_dir);
    strcat(d, "-t/");
    mkdir(d, 0777);
    CHECK_INT(pi_tools_init(&real_pt, &real_sys, real_dir, d, PI_SH_HOST), 0);
    CHECK_INT(pi_tools_add(&real_pt, ses, "read,write,edit,ls,find,grep", err, sizeof(err)), 0);
}

static void real_tools_down(void)
{
    char cmd[200];
    pi_tools_free(&real_pt);
    strcpy(cmd, "rm -rf ");
    strcat(cmd, real_dir);
    strcat(cmd, " ");
    strcat(cmd, real_dir);
    strcat(cmd, "-t");
    if (system(cmd))
        CHECK(0);
    real_dir[0] = 0;
}

static void run(const scen *sc)
{
    llm_hooks h;
    llm_model model;
    static pi_session ses;
    cl_io io;
    int i;
    char fn[80];
    char *keys[10];
    const char *script[10];
    int nk = 0;
    cur = sc;
    cur_cols = sc_cols(sc);
    pi_ui_set_theme(0);                 /* the system theme and pi's keys, whatever an earlier test left */
    pi_ui_set_keybindings(0, 0);
    pi_ui_set_tui(SC_REGULAR(sc), SC_HINT(sc), SC_BAR(sc));
    pi_ui_set_output_pad(!SC_PAD0(sc));
    pi_ui_set_resume(sc->when == 3 ? test_resume : 0, 0);
    if (SC_THEME(sc)) {
        static cl_pitheme th;
        const char *bt = SC_THEME(sc) == 1 ? pi_theme_dark : pi_theme_light;
        char err[200];
        CHECK_INT(pitheme_load(&th, bt, (long)strlen(bt), pi_okcolor, err, sizeof(err)), 0);
        pi_ui_set_theme(&th);
    }
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    ps_reset();
    ps_pool_up(LLM_POOL);
    memset(&h, 0, sizeof(h));
    h.poll = ps_poll;
    h.ms = ps_ms;
    h.sleep = ps_sleep;
    h.notice = ps_notice;
    h.resolve = ps_resolve;
    h.log = ps_log;
    model = ps_model_anthropic();
    model.id = "fake-model";
    model.context_window = 200000;
    model.cost[0] = 3000000L;
    model.cost[1] = 15000000L;
    model.cost[2] = 300000L;
    model.cost[3] = 3750000L;
    pi_session_init(&ses, &model, "k", &ps_pool, &h);
    ses.now = fixed_now;
    ses.system = "You are amiga-pi.";
    if (sc->tools == 1)
        pi_tool_add(&ses, &t_read);
    if (sc->tools == 2)
        pi_tool_add(&ses, &t_bash);
    if (sc->tools == 3)
        pi_tool_add(&ses, &t_bang);
    if (sc->tools == 4)
        pi_tool_add(&ses, &t_seq);
    if (sc->tools == 5)
        pi_tool_add(&ses, &t_ls);
    if (sc->tools == 6)
        real_tools_up(&ses, sc->name);
    for (i = 0; sc->answers[i]; i++) {
        long n;
        char *b = slurp(sc->answers[i], &n);
        CHECK(b != 0);
        if (!b)
            continue;
        if (strstr(sc->answers[i], ".http"))
            ps_http_text(b, n);
        else
            ps_sse_text(b, n, 0);
        free(b);
    }
    for (i = 0; sc->keys[i]; i++) {
        keys[nk] = (char *)malloc(strlen(sc->keys[i]) + 1);
        strcpy(keys[nk], sc->keys[i]);
        script[nk] = keys[nk];
        nk++;
    }
    script[nk++] = "\004";
    script[nk] = 0;
    cl_copy(fn, sc->name, sizeof(fn));
    load_golden(sc->name);
    CHECK(want != 0);
    if (!want)
        return;
    cs_open(cur_cols, want_rows + (sc->when == 3 ? 8 : 0), script);
    cs.before_read = hook;
    cs_io(&io);
    pi_ui_set_cwd("~/project");
    pi_ui_set_quiet(!strcmp(sc->name, "start_header_only") ? 2 : !sc->header);
    pi_ui_set_commands(pi_cmds, (int)(sizeof(pi_cmds) / sizeof(pi_cmds[0])));
    if (!strcmp(sc->name, "slash_prompts") || !strcmp(sc->name, "slash_utf")) {
        /* J2: the agent directory's prompts/ (the same files tools/pi_oracle/tui_gen.py gave pi) after the built-in rows */
        static sys_posix sp;
        static cl_sys tsys;
        int nt;
        const cl_cmd *rows;
        sys_posix_init(&sp, &tsys);
        CHECK_INT(test_tmpl_load(&slash_tm, &tsys, !strcmp(sc->name, "slash_utf") ? PI_TESTS "/pi/tui/agent_dir_u" : PI_TESTS "/pi/tui/agent_dir", "/nonexistent", 0, 0, 0, 0), 0);
        rows = pi_tmpl_menu(&slash_tm, &nt);
        pi_ui_set_extra_commands(0, rows, nt);
    }
    if (sc->when == 1)
        ps.silent = 1;
    tool_hook = sc->when == 2 ? compare_in_tool : !strcmp(sc->name, "dark_tools2") ? nested_tools : 0;
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    pi_ui_set_extra_commands(0, 0, 0);
    pi_tmpl_free(&slash_tm);
    if (sc->when == 3) {
        exit_check(sc);
        taken = 1;
    }
    if (sc->when == 4)
        turn_quiet_check();
    CHECK(taken);
    if (!strcmp(sc->name, "error_expanded")) {
        /* R5: the request's error is a line of the chat only (not in the session store): Ctrl+O keeps it and shows the status */
        int r, err = 0, status = 0;
        for (r = 0; r < 64; r++) {
            err |= strstr(snap[r], "Error: 400") != 0;
            status |= strstr(snap[r], "Tool output: expanded") != 0;
        }
        CHECK(err);
        CHECK(status);
    }
    if (diffs) {
        printf("  %s: %d cells differ from pi's capture\n%s", sc->name, diffs, first_diff);
        if (getenv("PI_TUI_DUMP")) {
            dump_rows();
            printf("  --- pi's\n");
            for (i = 0; i < want_rows; i++)
                printf("    %2d|%s\n", i + 1, h_row(want, i));
        }
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    cs_close();
    cur_cols = 80;
    vt_free(want);
    pi_session_free(&ses);
    if (sc->tools == 6)
        real_tools_down();
    for (i = 0; i < nk - 1; i++)
        free(keys[i]);
}

/* ---- N1: steering and follow-up in the agent loop (pi-agent-core's runLoop) ---- */

static void up_session(pi_session *ses, int tools)
{
    llm_hooks h;
    llm_model model;
    ps_reset();
    ps_pool_up(LLM_POOL);
    memset(&h, 0, sizeof(h));
    h.poll = ps_poll;
    h.ms = ps_ms;
    h.sleep = ps_sleep;
    h.notice = ps_notice;
    h.resolve = ps_resolve;
    h.log = ps_log;
    model = ps_model_anthropic();
    model.id = "fake-model";
    model.context_window = 200000;
    pi_session_init(ses, &model, "k", &ps_pool, &h);
    ses->now = fixed_now;
    ses->system = "You are amiga-pi.";
    if (tools)
        pi_tool_add(ses, &t_read);
}

static void answer(const char *name)
{
    long n;
    char *b = slurp(name, &n);
    CHECK(b != 0);
    if (b)
        ps_sse_text(b, n, 0);
    free(b);
}

static pi_session *hooked;
static void steer_two(pi_session *s)
{
    pi_steer(s, "go left");
    pi_steer(s, "and up");
    tool_hook = 0;
}

/* pi's runLoop order at a turn with steering: turn_end, (compaction), turn_start, the tool changes declared, then the
 * steering message. ev_log keeps the events as letters: T turn_start, E turn_end, U a user message's start */
static char ev_log[64];
static int ev_n;
static void ev_rec(void *u, const pi_event *e)
{
    (void)u;
    if (ev_n < (int)sizeof(ev_log) - 1 && (e->type == PE_TURN_START || e->type == PE_TURN_END ||
                                          (e->type == PE_MESSAGE_START && e->role == LR_USER)))
        ev_log[ev_n++] = e->type == PE_TURN_START ? 'T' : e->type == PE_TURN_END ? 'E' : 'U';
    ev_log[ev_n] = 0;
}

static int decl_armed;
static void decl_once(void *u, pi_session *s)
{
    static const char sys[] = "{\"role\":\"system\",\"content\":\"\",\"toolsAdded\":[{\"name\":\"ls\"}],\"timestamp\":0}";
    (void)u;
    if (decl_armed) {
        decl_armed = 0;
        pi_session_push_system(s, sys, (long)strlen(sys));
    }
}

static void steer_and_change(pi_session *s)
{
    pi_steer(s, "go left");
    decl_armed = 1;             /* the tools change while the tool runs: declared before the next request */
    tool_hook = 0;
}

static void test_queue_order(void)
{
    static pi_session ses;
    int k, left = -1;
    up_session(&ses, 1);
    answer("tool.1.sse");
    answer("markdown.1.sse");
    ev_n = 0;
    ev_log[0] = 0;
    pi_subscribe(&ses.em, ev_rec, 0);
    ses.declare = decl_once;
    tool_hook = steer_and_change;
    CHECK_INT(pi_prompt(&ses, "read the file"), LS_STOP);
    CHECK_INT(ps.nreq, 2);
    /* the prompt in the first turn; the steering message after the second turn_start, as pi emits it */
    CHECK_STR(ev_log, "TUETUE");
    for (k = 0; k < ses.nmsg; k++)
        if (strstr(ses.msg[k], "go left"))
            left = k;
    CHECK(left > 0);
    /* the declaration sits before the steering message (declareToolChanges inserts before the first pending one) */
    CHECK_INT(ses.nsys, 1);
    CHECK_INT(ses.nsys ? ses.sys_at[0] : -1, left);
    pi_session_free(&ses);
}

static void test_queue_agent(void)
{
    static pi_session ses;
    char *out[8];
    const char *r2;
    long left, up;
    int n;
    test_queue_order();
    /* steering typed while a tool runs: after the turn's tool calls, before the next request; one a turn */
    up_session(&ses, 1);
    answer("tool.1.sse");
    answer("markdown.1.sse");
    answer("table.1.sse");
    hooked = &ses;
    tool_hook = steer_two;
    CHECK_INT(pi_prompt(&ses, "read the file"), LS_STOP);
    CHECK_INT(ps.nreq, 3);                      /* the tool turn, then one turn for each steering message */
    left = ps.nreq > 1 ? (long)(strstr(ps.body[1], "go left") ? 1 : 0) : 0;
    CHECK_INT(left, 1);
    CHECK(ps.nreq > 1 && strstr(ps.body[1], "and up") == 0);
    CHECK(ps.nreq > 2 && strstr(ps.body[2], "and up") != 0);
    r2 = ps.nreq > 1 ? ps.body[1] : "";
    CHECK(strstr(r2, "tool_result") != 0 && strstr(r2, "tool_result") < strstr(r2, "go left"));
    CHECK_INT(ses.nsteer, 0);
    pi_session_free(&ses);
    /* followUpMode "all" / steeringMode "all": both in the same turn */
    up_session(&ses, 1);
    ses.steer_all = 1;
    answer("tool.1.sse");
    answer("markdown.1.sse");
    tool_hook = steer_two;
    CHECK_INT(pi_prompt(&ses, "read the file"), LS_STOP);
    CHECK_INT(ps.nreq, 2);
    up = ps.nreq > 1 && strstr(ps.body[1], "go left") && strstr(ps.body[1], "and up");
    CHECK_INT(up, 1);
    pi_session_free(&ses);
    /* a follow-up waits until the run would stop */
    up_session(&ses, 0);
    answer("markdown.1.sse");
    answer("table.1.sse");
    CHECK_INT(pi_follow_up(&ses, "next thing"), 0);
    CHECK_INT(pi_prompt(&ses, "first"), LS_STOP);
    CHECK_INT(ps.nreq, 2);
    CHECK(ps.nreq > 1 && strstr(ps.body[1], "next thing") != 0);
    CHECK(ps.nreq > 0 && strstr(ps.body[0], "next thing") == 0);
    CHECK_INT(ses.nfollow, 0);
    pi_session_free(&ses);
    /* the queues give their messages back, steering first */
    up_session(&ses, 0);
    pi_follow_up(&ses, "later");
    pi_steer(&ses, "now");
    n = pi_queue_take(&ses, out, 8);
    CHECK_INT(n, 2);
    CHECK(n == 2 && !strcmp(out[0], "now") && !strcmp(out[1], "later"));
    while (n > 0)
        free(out[--n]);
    CHECK_INT(ses.nsteer + ses.nfollow, 0);
    pi_session_free(&ses);
    (void)hooked;
}

/* Enter typed while the agent works steers it: through pi_ui_run, after the tool call, before the next request */
static void test_steer_ui(void)
{
    static pi_session ses;
    static const char *script[] = { "read the file\r", "!steer me\r", "\004", 0 };
    cl_io io;
    int a, b, c, d;
    up_session(&ses, 1);
    answer("tool.1.sse");
    answer("tool.2.sse");
    cs_open(80, 25, script);
    cs_io(&io);
    pi_ui_set_quiet(1);
    pi_ui_set_theme(0);
    pi_ui_set_keybindings(0, 0);
    pi_ui_set_cwd("~/project");
    pi_ui_set_commands(0, 0);
    cs.before_read = 0;
    taken = 0;
    cur = 0;
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    CHECK_INT(ps.nreq, 2);
    CHECK(ps.nreq > 1 && strstr(ps.body[1], "steer me") != 0);
    CHECK(ps.nreq > 0 && strstr(ps.body[0], "steer me") == 0);
    /* the transcript as it went: the prompt, the call, the steering message, the answer */
    a = (int)(strstr((const char *)cs.sent.p, "read the file") - (const char *)cs.sent.p);
    b = (int)(strstr((const char *)cs.sent.p, "read\033[0m") ? 1 : 1);
    c = cs.sent.p ? (int)(strstr((const char *)cs.sent.p, "steer me") - (const char *)cs.sent.p) : -1;
    d = cs.sent.p ? (int)(strstr((const char *)cs.sent.p, "Done reading") - (const char *)cs.sent.p) : -1;
    CHECK(a > 0 && c > a && d > c);
    (void)b;
    cs_close();
    pi_session_free(&ses);
}


/* the spinner turns while a request waits (owner report 2026-10-13, rig 3: after "hello" a long delay and pi's braille
 * spinner stood still): through a slow connect and TLS handshake and through the server's quiet before the first byte,
 * a new frame every 80 ms as pi's Loader draws them (pi-tui loader.ts DEFAULT_INTERVAL_MS); Esc stops a slow connect */
static char spin_glyph[8];
static long spin_turns, spin_waited;
static void spin_watch(long ms)
{
    int r = cs_find("Working");
    const char *row, *g;
    cs.clock += (unsigned long)ms;      /* the screen's clock goes as the transport waits */
    spin_waited += ms;
    if (r < 0)
        return;
    row = cs_row(r);
    for (g = row; *g && !((unsigned char)g[0] == 0xe2 && ((unsigned char)g[1] & 0xfc) == 0xa0); g++)
        ;                               /* the braille glyph: U+2800-U+28FF */
    if (!*g)
        return;
    if (memcmp(spin_glyph, g, 3)) {
        if (spin_glyph[0])
            spin_turns++;
        memcpy(spin_glyph, g, 3);
    }
}

static long spin_run(long open_ms, long quiet_ms, const char *const *script)
{
    static pi_session ses;
    cl_io io;
    up_session(&ses, 0);
    answer("markdown.1.sse");
    ps.open_ms = open_ms;
    ps.quiet_ms = quiet_ms;
    ps.waited = spin_watch;
    memset(spin_glyph, 0, sizeof(spin_glyph));
    spin_turns = spin_waited = 0;
    cs_open(80, 25, (const char **)script);
    cs_io(&io);
    pi_ui_set_quiet(1);
    pi_ui_set_theme(0);
    pi_ui_set_keybindings(0, 0);
    pi_ui_set_cwd("~/project");
    pi_ui_set_commands(0, 0);
    cs.before_read = 0;
    taken = 0;
    cur = 0;
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    cs_close();
    pi_session_free(&ses);
    return spin_turns;
}

static void test_spinner_while_waiting(void)
{
    static const char *const hello[] = { "hello\r", "\004", 0 };
    static const char *const esc[] = { "hello\r", "!\033", "\004", 0 };
    long turns;
    /* 4 s connecting (a 68020's handshake), no answer meanwhile: 50 frames at 80 ms, a few lost at the edges */
    turns = spin_run(4000, 0, hello);
    CHECK_INT(spin_waited, 4000);
    CHECK(turns >= 45);
    CHECK_INT(ps.nreq, 1);
    /* 4 s of the server's quiet before its first byte */
    turns = spin_run(0, 4000, hello);
    CHECK(spin_waited >= 4000);
    CHECK(turns >= 45);
    /* Esc while connecting: the connect ends at once and no request goes out */
    spin_run(60000, 0, esc);
    CHECK_INT(ps.open_broken, 1);
    CHECK(spin_waited < 1000);
    CHECK_INT(ps.nreq, 0);
}

/* the directory name in the title: Unix and Amiga paths, a trailing '/', a volume root (an Amiga path is not cut at '/'
 * only: "DH0:proj" names proj, "Ram Disk:" names the volume) */
static void test_title_dir_name(void)
{
    static const char *const cases[][2] = {
        { "/Users/spot/project", "project" }, { "/Users/spot/project/", "project" }, { "DH0:Work/proj", "proj" },
        { "DH0:proj", "proj" }, { "Ram Disk:", "Ram Disk" }, { "proj", "proj" }, { "/", "" }, { 0, 0 }
    };
    char out[64];
    int i;
    for (i = 0; cases[i][0]; i++) {
        pi_dir_name(cases[i][0], out, sizeof(out));
        CHECK_STR(out, cases[i][1]);
    }
    pi_dir_name("/a/abcdefgh", out, 4);
    CHECK_STR(out, "abc");
}

/* the window title is pi's -- "pi - <directory name>", spelled "pi", not U+03C0 which reads "p" on the console (T2) -- set once at the start and left alone while
 * the agent works and after (TM2; pi 1.0.4 updateTerminalTitle): never C:Claude's "Claude", "Claude - working" */
static void test_window_title(void)
{
    static pi_session ses;
    static const char *script[] = { "read the file\r", "\004", 0 };
    cl_io io;
    const char *p, *e;
    int n = 0, bad = 0;
    up_session(&ses, 1);
    answer("tool.1.sse");
    answer("tool.2.sse");
    cs_open(80, 25, script);
    cs_io(&io);
    pi_ui_set_quiet(1);
    pi_ui_set_theme(0);
    pi_ui_set_keybindings(0, 0);
    pi_ui_set_cwd("~/project");
    pi_ui_set_commands(0, 0);
    cs.before_read = 0;
    taken = 0;
    cur = 0;
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    CHECK_INT(ps.nreq, 2);                   /* the agent did work */
    for (p = (const char *)cs.sent.p; p && (p = strstr(p, "\033]2;")) != 0; p = e) {
        e = strchr(p, '\a');
        if (!e)
            break;
        n++;
        if (e - (p + 4) != (long)strlen("pi - project") || strncmp(p + 4, "pi - project", (size_t)(e - (p + 4))))
            bad++;
    }
    CHECK_INT(n, 1);
    CHECK_INT(bad, 0);
    CHECK(cs.sent.p && strstr((const char *)cs.sent.p, "Claude - working") == 0);
    cs_close();
    pi_session_free(&ses);
}

/* ---- pi_app on the screen: a sandbox (agent dir, data dir), the keys typed, a look at the screen at rest ---- */

static char aroot[400], adata[420], aagent[420], asdir[520];
static sys_posix asp;
static cl_sys asys;
static long aclock;
static int rest_any;            /* take the look at any screen at rest, not only one with the footer */
static const char *rest_find = "navigate";     /* with rest_any: the text that shows the screen is the one to take (a dialog's keys) */
static int rest_now;            /* with rest_any: take it the moment the text shows, whatever keys are left (a screen no key waits at) */
static void (*at_rest)(void);   /* called once with the screen at rest (the next chunk is the last one) */
static int rested;
static const char *rest_key = "\004";          /* the chunk that is next when the screen is at rest (a selector that takes ctrl+d: another) */

static llm_ts adv_now(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1760100000L + aclock++;
    t.ms = 7;
    return t;
}

static const char *app_pstatus;         /* PI_PROGRAM_STATUS in the test's environment (0: unset) */
static const char *app_colorterm;       /* what the test's terminal says in COLORTERM (0: unset) */
static const char *app_imgproto;        /* PI_IMAGE_PROTOCOL in the test's environment (0: unset) */
static const char *app_home;            /* HOME in the test's environment (0: unset) */
static const char *app_config_cli;      /* the arguments after `config` for the next app_run ("" none): run pi's `config` screen instead of the session */

static int app_nokeys;              /* no provider key in the environment: the models of models.json are the only ones with a credential */
static const char *app_model_cli;   /* --model for the next app_run, 0: anthropic/claude-opus-5-5 */

static long app_env(const char *name, char *o, long cap)
{
    const char *v = 0;
    if (!strcmp(name, "COLORTERM"))
        v = app_colorterm;
    else if (!strcmp(name, "TERM"))
        v = "vtcon";
    else if (!strcmp(name, "PI_PROGRAM_STATUS"))
        v = app_pstatus;
    else if (!strcmp(name, "PI_IMAGE_PROTOCOL"))
        v = app_imgproto;
    else if (!strcmp(name, "HOME"))
        v = app_home;
    if (!app_nokeys && !strcmp(name, "OPENAI_API_KEY"))
        v = "sk-test-openai";
    else if (!app_nokeys && !strcmp(name, "ANTHROPIC_API_KEY"))
        v = "sk-ant-test-not-real";
    if (!v || (long)strlen(v) >= cap)
        return -1;
    strcpy(o, v);
    return (long)strlen(v);
}

static void app_write(const char *name, const char *text)
{
    char p[520];
    FILE *f;
    strcpy(p, aagent);
    strcat(p, "/");
    strcat(p, name);
    f = fopen(p, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void rest_hook(void)
{
    if (getenv("PI_TUI_HOOK"))
        printf("  hook: next=%d chunk=%s ms=%ld find=%d bottom=[%s]\n", cs.next, cs.script[cs.next] ? cs.script[cs.next] : "(end)", cs.read_ms, rest_any ? cs_find(rest_find) : -2, cs_row(cs.rows - 1));
    if (rest_now && rest_any && !rested && cs_find(rest_find) >= 0) {
        rested = 1;
        if (at_rest)
            at_rest();
        return;
    }
    if (rested || (rest_any && cs_find(rest_find) < 0) ||
        (!rest_any && !strstr(cs_row(cs.rows - 1), "-model") && !strstr(cs_row(cs.rows - 1), "claude-")) ||
        cs.read_ms <= 0 || !cs.script[cs.next] || strcmp(cs.script[cs.next], rest_key))
        return;
    rested = 1;
    if (at_rest)
        at_rest();
}

/* pi_app with the keys typed on a screen of cols x rows; the answers streamed in order. The exit code. */
static const char *app_changelog;   /* pl.changelog for the next app_run: the CHANGELOG.md What's New and /changelog show, 0 none */
static const char *app_session;     /* --session <file> for the next app_run, 0 none */
static const char *app_agent;       /* the agent directory for the next app_run (0: the sandbox's) */
static const char *app_tui_cli;     /* --tui-mode <x> for the next app_run, 0 none */
static const char *app_name_cli;    /* --name <x> for the next app_run, 0 none */
static const char *app_sdir_cli;    /* --session-dir <x> for the next app_run, 0 none */
static int app_resume_cli;          /* -r for the next app_run */
static char *app_out;               /* every byte the last app_run wrote to the screen */

static void (*app_ready)(pi_session *s);               /* called once the resources are loaded, before the first key */

/* /login's random bytes (the PKCE verifier, the state, the device id): any, the screens leave them out */
static int app_random(void *u, unsigned char *b, long n)
{
    long i;
    static unsigned k;
    (void)u;
    for (i = 0; i < n; i++)
        b[i] = (unsigned char)((k++ * 37 + 11) & 255);
    return 0;
}

static int app_run(int cols, int rows, const char **keys, const char *const *sse, int nsse, void (*rest)(void))
{
    cl_io io;
    pi_platform pl;
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int i, rc, argc = 3;
    if (app_model_cli)
        a[2] = (char *)app_model_cli;
    if (app_session) {
        a[3] = "--session";
        a[4] = (char *)app_session;
        argc = 5;
    }
    if (app_tui_cli) {
        a[argc++] = (char *)"--tui-mode";
        a[argc++] = (char *)app_tui_cli;
    }
    if (app_name_cli) {
        a[argc++] = (char *)"--name";
        a[argc++] = (char *)app_name_cli;
    }
    if (app_sdir_cli) {
        a[argc++] = (char *)"--session-dir";
        a[argc++] = (char *)app_sdir_cli;
    }
    if (app_resume_cli)
        a[argc++] = (char *)"-r";
    if (app_config_cli) {
        a[1] = (char *)"config";
        argc = 2;
        if (app_config_cli[0])
            a[argc++] = (char *)app_config_cli;
    }
    ps_reset();
    ps_pool_up(LLM_POOL);
    for (i = 0; i < nsse; i++) {
        long n;
        char *b = slurp(sse[i], &n);
        if (b && strstr(sse[i], ".http"))
            ps_http_text(b, n);
        else if (b)
            ps_sse_text(b, n, 0);
        free(b);
    }
    cs_open(cols, rows, keys);
    cs.before_read = rest_hook;
    cs_io(&io);
    at_rest = rest;
    rested = 0;
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = app_env;
    pl.cwd = aroot;                 /* the tools run here */
    pl.sys = &asys;
    pl.tmp = aroot;
    pl.utc = 1;
    pl.data_dir = adata;
    pl.agent_dir = app_agent ? app_agent : aagent;
    pl.changelog = app_changelog;
    pl.ready = app_ready;
    pl.random = app_random;
    rc = pi_app(argc, a, &pl);
    free(app_out);
    app_out = (char *)malloc((size_t)cs.sent.n + 1);
    if (app_out)
        memcpy(app_out, cs.sent.p, (size_t)cs.sent.n + 1);
    cs_close();
    return rc;
}

static int app_newest(char *path, long cap)
{
    return s3_recent(&asys, asdir, path, cap);
}

/* the entries' types of the newest session file, e.g. "model_change thinking_level_change": into out */
static void app_shape(char *o, long cap)
{
    pi_s3 st;
    char path[600], type[40];
    int i;
    o[0] = 0;
    if (app_newest(path, sizeof(path)))
        return;
    s3_init(&st, &asys, adv_now, 0, 1, 7);
    if (s3_open(&st, path)) {
        s3_free(&st);
        return;
    }
    for (i = 0; i < st.n; i++) {
        long n;
        const char *t = s3_text(&st, i, &n);
        jv ob, v;
        type[0] = 0;
        if (t && !json_parse(t, n, &ob) && json_get(ob, "type", &v))
            json_str(v, type, sizeof(type));
        if (i)
            cl_cat(o, " ", cap);
        cl_cat(o, type, cap);
        if (!strcmp(type, "model_change") || !strcmp(type, "thinking_level_change")) {
            jv w;
            cl_cat(o, ":", cap);
            if (json_get(ob, "modelId", &w) || json_get(ob, "thinkingLevel", &w)) {
                char x[80];
                json_str(w, x, sizeof(x));
                cl_cat(o, x, cap);
            }
        }
    }
    s3_free(&st);
}

static char shot[40][200];
static int nshot;
static int shot_cx, shot_cy, shot_cursor;       /* the window's cursor at the shot: column, row (0-based), shown */

static void take_shot(void)
{
    int r;
    vt_cursor(cs.vt, &shot_cx, &shot_cy);
    shot_cursor = (vt_modes(cs.vt) & VT_MODE_CURSOR_VISIBLE) != 0;
    nshot = cs.rows < 40 ? cs.rows : 40;
    for (r = 0; r < nshot; r++) {
        strncpy(shot[r], cs_row(r), sizeof(shot[r]) - 1);
        shot[r][sizeof(shot[r]) - 1] = 0;
    }
}

static int shot_has(const char *text)
{
    int r;
    for (r = 0; r < nshot; r++)
        if (strstr(shot[r], text))
            return 1;
    return 0;
}

static void app_sandbox(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(aroot, base && *base ? base : "/tmp");
    if (aroot[strlen(aroot) - 1] == '/')
        aroot[strlen(aroot) - 1] = 0;
    strcat(aroot, "/pi_tui_XXXXXX");
    if (!mkdtemp(aroot)) {
        CHECK(0);
        return;
    }
    strcpy(adata, aroot);
    strcat(adata, "/data");
    strcpy(aagent, aroot);
    strcat(aagent, "/agent");
    mkdir(adata, 0777);
    mkdir(aagent, 0777);
    sys_posix_init(&asp, &asys);
    s3_dir(adata, aroot, asdir, sizeof(asdir));
}

static void app_cleanup(void)
{
    char cmd[600];
    if (!aroot[0] || strlen(aroot) < 10)
        return;
    strcpy(cmd, "rm -rf '");
    strcat(cmd, aroot);
    strcat(cmd, "'");
    if (system(cmd))
        CHECK(0);
}

/* ---- N2: Ctrl+P cycles the models of enabledModels, Shift+Tab the thinking level, entries written ---- */

static void test_cycle(void)
{
    static const char *keys[] = { "hello\r", "\020", "\020", "\033[Z", "\004", 0 };
    static const char *const sse[] = { "markdown.1.sse" };
    char sh[800];
    app_write("settings.json", "{\"enabledModels\":[\"anthropic/claude-opus-5-5:high\",\"openai/gpt-4.1\"],"
                               "\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys, sse, 1, take_shot), 0);
    app_shape(sh, sizeof(sh));
    /* a new session records its model and level; Ctrl+P: gpt-4.1 (off: it has no reasoning); Ctrl+P: opus-5-5 at
     * the scope's high; Shift+Tab: the next level */
    CHECK(strstr(sh, "model_change:claude-opus-5-5 thinking_level_change:medium message") == sh);
    CHECK(strstr(sh, " message message model_change:gpt-4.1 thinking_level_change:off model_change:claude-opus-5-5 "
                     "thinking_level_change:high thinking_level_change:") != 0);
    /* three statuses in a row (showStatus, interactive-mode.ts:3824-3834): each replaces the one above, the last stays */
    CHECK(!shot_has("Switched to GPT-4.1"));
    CHECK(!shot_has("(thinking: high)"));
    CHECK(shot_has("Thinking level: "));
    if (getenv("PI_TUI_DUMP")) {
        int r;
        printf("  entries: %s\n", sh);
        for (r = 0; r < nshot; r++)
            printf("    %2d|%s\n", r + 1, shot[r]);
    }
}


/* ---- N3: /tree as pi's selector, over a session with a branch (the capture is tests/pi/tui/tree.golden) ---- */

static void tree_rest(void)
{
    compare();
}

/* a session fixture of tests/pi (relative to tests/pi/tui) written to <aroot>/<name>, its project directory moved to the
 * sandbox's: path gets the file's name. 0 when the fixture is missing */
static int put_session(const char *fixture, const char *name, char *path)
{
    char *txt, *fix;
    long n;
    FILE *f;
    strcpy(path, aroot);
    strcat(path, "/");
    strcat(path, name);
    txt = slurp(fixture, &n);
    CHECK(txt != 0);
    if (!txt)
        return 0;
    fix = (char *)malloc((size_t)n * 2 + 200);
    {
        const char *from = txt, *at;
        char *o = fix;
        while ((at = strstr(from, "/path/to/project")) != 0) {
            memcpy(o, from, (size_t)(at - from));
            o += at - from;
            strcpy(o, "/work/proj");
            o += strlen("/work/proj");
            from = at + strlen("/path/to/project");
        }
        strcpy(o, from);
    }
    f = fopen(path, "wb");
    if (f) {
        fputs(fix, f);
        fclose(f);
    }
    free(txt);
    free(fix);
    return 1;
}

static void test_tree(void)
{
    static const char *keys[] = { "/tree\r", "\004", 0 };
    static scen sc = { "tree", { 0 }, { 0 }, 0, 1, 0, { { 1, 6, 0, 79, "the transcript above the selector" },
                                                        { 9, 10, 0, 79, "tree help keys" },
                                                        { 24, 25, 0, 79, "the folder and the model of the capture" } } };
    char path[600];
    int i;
    if (!put_session("../sessions/doc_branch.jsonl", "tree_session.jsonl", path))
        return;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    cur = &sc;
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    load_golden("tree");
    CHECK(want != 0);
    if (!want)
        return;
    app_session = path;
    CHECK_INT(app_run(80, want_rows, keys, 0, 0, tree_rest), 0);
    app_session = 0;
    CHECK(taken);
    CHECK(strstr(snap[24], "\342\206\22130 \342\206\22315 $0.900") != 0);   /* the totals of every entry, the other branch too */
    if (diffs) {
        printf("  tree: %d cells differ from pi's capture\n%s", diffs, first_diff);
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    vt_free(want);
}


/* P11-I9: a resumed session draws `Took` from the toolResult's recorded durationMs (pi 1.1.0 tool-execution.ts:137,
 * renderers/bash.ts:104-106): 2300 ms is "Took 2.3s", a result recorded without one shows no Took row. The golden is pi's
 * own screen for the same session file. */
static void resume_rest(void)
{
    compare();
}

static void session_case(const char *name, const char *file, const char *settings, const char **keys,
                         const char *const *sse, int nsse)
{
    static scen sc = { "", { 0 }, { 0 }, 0, 0, 0, { { 21, 25, 0, 79, "the editor border (the thinking level of the model) and the footer: the capture's model is the fake one" } } };
    char path[600];
    int i;
    sc.name = name;
    if (!put_session(file, file, path))
        return;
    app_write("settings.json", settings);
    cur = &sc;
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    load_golden(name);
    CHECK(want != 0);
    if (!want)
        return;
    sc.masks[0].r0 = want_rows - 5;     /* the editor border and the footer, at the bottom of whatever the capture's height */
    sc.masks[0].r1 = want_rows;
    app_session = path;
    CHECK_INT(app_run(80, want_rows, keys, sse, nsse, resume_rest), 0);
    app_session = 0;
    CHECK(taken);
    if (diffs) {
        printf("  %s: %d cells differ from pi's capture\n%s", name, diffs, first_diff);
        if (getenv("PI_TUI_DUMP")) {
            dump_rows();
            printf("  --- pi's\n");
            for (i = 0; i < want_rows; i++)
                printf("    %2d|%s\n", i + 1, h_row(want, i));
        }
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    vt_free(want);
}

static void resume_case(const char *name, const char *file, const char *settings)
{
    static const char *keys[] = { "\004", 0 };
    session_case(name, file, settings, keys, 0, 0);
}

/* Q17: a live /compact: pi clears the chat, redraws what the context keeps and appends the compaction summary box
 * (interactive-mode.ts compaction_end, 3700-3715) instead of a muted "Compacted: about N tokens" note. The second
 * model call (the summarization) is compact.1.sse. The golden is pi's own screen. */
static void test_compact_live(void)
{
    static const char *keys[] = { "/compact\r", "\004", 0 };
    static const char *const sse[] = { "compact.1.sse" };
    session_case("compact", "took_session.jsonl", "{\"quietStartup\":true,\"compaction\":{\"keepRecentTokens\":10}}\n",
                 keys, sse, 1);
    CHECK(strstr(app_out, "Compacted from 15 tokens") != 0);
    CHECK(strstr(app_out, "Compacted: about") == 0);
    {
        /* the automatic one: the turn's usage (1,260 tokens) passes the window less reserveTokens (999000 of 1.0M) */
        static const char *akeys[] = { "and then?\r", "\004", 0 };
        static const char *const asse[] = { "compact_auto.1.sse", "compact_auto.2.sse" };
        session_case("compact_auto", "took_session.jsonl",
                     "{\"quietStartup\":true,\"compaction\":{\"keepRecentTokens\":10,\"reserveTokens\":999000}}\n", akeys,
                     asse, 2);
        CHECK(strstr(app_out, "Compacted from 1,220 tokens") != 0);
    }
}

static void test_resume_took(void)
{
    resume_case("resume_took", "took_session.jsonl", "{\"quietStartup\":true}\n");
}

/* Q2: pi 1.1.0 getTextOutput (core/tools/render-utils.ts:39-48, used by renderers/bash.ts:52) strips escape sequences,
 * binary garbage and carriage returns from the text blocks of a tool result at render time. The session holds a bash
 * result with SGR, EL and an OSC title in its text; the golden is pi's own screen for it ("red plain tail"). */
static void test_resume_ansi(void)
{
    resume_case("resume_ansi", "ansi_session.jsonl", "{\"quietStartup\":true}\n");
}

/* Q8: a resumed session with a compaction summary, a custom message (display true) and a branch summary: pi 1.1.0's
 * three components over customMessageBg (components/compaction-summary-message.ts, custom-message.ts,
 * branch-summary-message.ts), in the system, dark and light themes and with outputPad 0 */
static void test_resume_summaries(void)
{
    resume_case("resume_summaries", "summaries_session.jsonl", "{\"quietStartup\":true}\n");
    resume_case("dark_resume_summaries", "summaries_session.jsonl", "{\"quietStartup\":true,\"theme\":\"dark\"}\n");
    resume_case("light_resume_summaries", "summaries_session.jsonl", "{\"quietStartup\":true,\"theme\":\"light\"}\n");
    resume_case("dark_pad0_resume_summaries", "summaries_session.jsonl",
                "{\"quietStartup\":true,\"theme\":\"dark\",\"outputPad\":0}\n");
}

/* ---- I1/I3: skills on the screen: the start's [Skills] list and [Skill conflicts] block, the slash menu, the /skill:name box
 * (the captures are tests/pi/tui/skills_*.golden, resume_skill*.golden, skill_cmd.golden: pi 1.1.0, tools/pi_oracle/tui_gen.py) ---- */

static char skdir[420];             /* the agent directory with the skills: short, so a path stays on one row as pi's "~/agent/..." does */
static const char *tree_json = "skills_files.json";     /* the files written into it (tui_gen.py SKILL_FILES, PROMPT_FILES, CONFLICT_FILES) */

/* a new agent directory for the next case: the old one removed (its files would be found again) */
static void fresh_agent(void)
{
    char cmd[460];
    if (skdir[0] && strlen(skdir) > 10) {
        strcpy(cmd, "rm -rf '");
        strncat(cmd, skdir, strlen(skdir) - 6);         /* /tmp/pis_XXXXXX/agent without "/agent" */
        strcat(cmd, "'");
        if (system(cmd))
            CHECK(0);
    }
    skdir[0] = 0;
}

/* the skills of tui_gen.py's SKILL_FILES (tests/pi/tui/skills_files.json) written under skdir/skills */
static void skills_tree(const char *settings)
{
    char *b, p[600], d[600];
    long n;
    jv all, k, v;
    jit it;
    FILE *f;
    char *e;
    if (!skdir[0]) {
        strcpy(skdir, "/tmp/pis_XXXXXX");
        if (!mkdtemp(skdir)) {
            CHECK(0);
            return;
        }
        strcat(skdir, "/agent");
        mkdir(skdir, 0777);
        strcpy(p, skdir);
        strcat(p, "/skills");
        mkdir(p, 0777);
    }
    b = slurp(tree_json, &n);
    CHECK(b != 0);
    if (!b || json_parse(b, n, &all)) {
        free(b);
        return;
    }
    json_iter(all, &it);
    while (json_next(&it, &k, &v)) {
        char *rel = json_strdup(k, 0), *text = json_strdup(v, 0);
        strcpy(p, skdir);
        strcat(p, "/");
        strcat(p, rel ? rel : "");
        strcpy(d, p);
        e = strrchr(d, '/');
        if (e) {
            *e = 0;
            mkdir(d, 0777);
        }
        f = fopen(p, "wb");
        if (f) {
            fputs(text ? text : "", f);
            fclose(f);
        }
        free(rel);
        free(text);
    }
    free(b);
    strcpy(p, skdir);
    strcat(p, "/settings.json");
    f = fopen(p, "wb");
    if (f) {
        fputs(settings, f);
        fclose(f);
    }
}

/* the cells of the golden's paths ("~/agent/...") are the sandbox's own: masked from the first one on, found in the capture's text */
static int skills_masks(scen *sc, const char *name, int from)
{
    char fn[80], *g, *line, *nl;
    long n;
    int row = 0, k = from;
    cl_copy(fn, name, sizeof(fn));
    cl_cat(fn, ".txt", sizeof(fn));
    g = slurp(fn, &n);
    if (!g)
        return k;
    for (line = g; line && *line && k < 11; line = nl ? nl + 1 : 0) {
        char *at;
        nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        row++;
        at = strstr(line, "~/agent");
        if (at) {
            /* columns count characters: the glyph lines hold three-byte characters before the path */
            long col = 0, i;
            for (i = 0; line + i < at; i++)
                if (((unsigned char)line[i] & 0xC0) != 0x80)
                    col++;
            sc->masks[k].r0 = sc->masks[k].r1 = row;
            sc->masks[k].c0 = (int)col;
            sc->masks[k].c1 = 79;
            sc->masks[k].why = "the sandbox's own path";
            k++;
        }
    }
    free(g);
    return k;
}

/* tui_gen.py RELOAD_WRITES (tests/pi/tui/reload_writes.json) written under skdir: what changes while the program runs */
static void reload_writes(pi_session *s)
{
    char *b, p[600], d[600];
    long n;
    jv all, k, v;
    jit it;
    (void)s;
    b = slurp("reload_writes.json", &n);
    CHECK(b != 0);
    if (!b || json_parse(b, n, &all)) {
        free(b);
        return;
    }
    json_iter(all, &it);
    while (json_next(&it, &k, &v)) {
        char *rel = json_strdup(k, 0), *text = json_strdup(v, 0), *e;
        FILE *f;
        strcpy(p, skdir);
        strcat(p, "/");
        strcat(p, rel ? rel : "");
        strcpy(d, p);
        e = strrchr(d, '/');
        if (e) {
            *e = 0;
            mkdir(d, 0777);
        }
        f = fopen(p, "wb");
        if (f) {
            fputs(text ? text : "", f);
            fclose(f);
        }
        free(rel);
        free(text);
    }
    free(b);
}

static void skills_rest(void)
{
    compare();
}

/* one capture: settings, the keys, the golden compared. mr: pairs of golden rows (first, last) left out, ended by 0: the
 * editor border and the footer carry the capture's fake model, a block compared with a shift has the rows around it left out */
static void skills_case(const char *name, const char *settings, const char **keys, const char *const *sse, int nsse,
                        int session, int shift, const int *mr, int with_skills)
{
    static scen sc;
    char path[600];
    int i, k = 0;
    memset(&sc, 0, sizeof(sc));
    sc.name = name;
    if (with_skills)
        skills_tree(settings);
    else
        app_write("settings.json", settings);       /* the sandbox's agent directory: no skills */
    if (session && !put_session("skill_session.jsonl", "skill_session.jsonl", path))
        return;
    cur = &sc;
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    load_golden(name);
    CHECK(want != 0);
    if (!want)
        return;
    for (i = 0; mr[i] && k < 6; i += 2, k++) {
        sc.masks[k].r0 = mr[i];
        sc.masks[k].r1 = mr[i + 1];
        sc.masks[k].c0 = 0;
        sc.masks[k].c1 = 79;
        sc.masks[k].why = "the start header's tips, or the editor border and the footer of the capture's fake model";
    }
    k = skills_masks(&sc, name, k);
    if (xm_row && k < 12) {
        sc.masks[k].r0 = sc.masks[k].r1 = xm_row;
        sc.masks[k].c0 = xm_c0;
        sc.masks[k].c1 = xm_c1;
        sc.masks[k].why = xm_why;
    }
    cmp_shift = shift;
    app_session = session ? path : 0;
    app_agent = with_skills ? skdir : 0;
    if (with_skills) {
        static char home[300];
        strcpy(home, skdir);
        home[strlen(home) - 6] = 0;                      /* the sandbox's agent directory is <home>/agent: pi's HOME of the capture */
        app_home = home;
    }
    CHECK_INT(app_run(80, want_rows, keys, sse, nsse, skills_rest), 0);
    app_agent = 0;
    app_session = 0;
    cmp_shift = 0;
    CHECK(taken);
    if (diffs) {
        printf("  %s: %d cells differ from pi's capture\n%s", name, diffs, first_diff);
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i + shift >= 0 ? i + shift : 0], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    vt_free(want);
}

/* U1: /login in pi's look. The selectors and the framed sign-in dialog against pi 1.1.0's own screens (tools/pi_oracle/tui_gen.py
 * login_*): the authentication method (its ExtensionSelectorComponent), the provider selector (OAuthSelectorComponent) with pi's
 * whole provider list (tests/pi/tui/login_rows.txt, tools/pi_oracle/gen_login_rows.mjs: this build has six of the nine account
 * providers), the search box, and the dialog up to the sign-in URL or the prompt. The rows below 80 columns of the footer carry the
 * capture's fake model; the URL's own query (a random state, challenge and nonce) is left out. */
static pi_login_row lrow[64];
static char lname[64][64], ltype[64][16], lsearch[64][260];
static int lrow_n, lrow_oauth;

static int load_login_rows(int want_type)         /* 1 oauth, 0 API key, -1 both */
{
    long n = 0;
    char *f = slurp("login_rows.txt", &n), *line;
    int k = 0, kinds = 0;
    lrow_oauth = 0;
    if (!f)
        return 0;
    for (line = f; *line && k < 64; ) {
        char *eol = strchr(line, '\n'), *fld[5];
        int i;
        if (!eol)
            break;
        *eol = 0;
        fld[0] = line;
        for (i = 1; i < 5; i++) {
            fld[i] = strchr(fld[i - 1], '\t');
            if (!fld[i])
                break;
            *fld[i]++ = 0;
        }
        if (i == 5) {
            int oauth = !strcmp(fld[0], "oauth");
            if (want_type < 0 || want_type == oauth) {
                strcpy(lname[k], fld[2]);
                strcpy(ltype[k], !oauth ? "API key" : fld[4][0] == '1' ? "subscription" : "account");
                snprintf(lsearch[k], sizeof(lsearch[k]), "%s %s %s %s", fld[2], fld[1], fld[0], fld[3]);
                lrow[k].name = lname[k];
                lrow[k].type = ltype[k];
                lrow[k].search = lsearch[k];
                lrow[k].status = PL_ST_NONE;
                lrow[k].status_text = "";
                kinds |= oauth ? 1 : 2;
                k++;
            }
        }
        line = eol + 1;
    }
    free(f);
    lrow_n = k;
    lrow_oauth = kinds == 3;
    return k;
}

/* a models.json of the generator's fake provider (tools/pi_oracle/tui_gen.py): fake-model and, with more, fake-small and fake-large */
static void fake_models(int reasoning, int more)
{
    char t[1500];
    strcpy(t, "{\"providers\":{\"fake\":{\"baseUrl\":\"http://127.0.0.1:9\",\"api\":\"anthropic-messages\",\"apiKey\":\"k\",\"models\":["
              "{\"id\":\"fake-model\",\"name\":\"fake-model\",\"reasoning\":");
    strcat(t, reasoning ? "true" : "false");
    strcat(t, ",\"input\":[\"text\"],\"contextWindow\":200000,\"maxTokens\":8192,\"cost\":{\"input\":3,\"output\":15,\"cacheRead\":0.3,\"cacheWrite\":3.75}}");
    if (more)
        strcat(t, ",{\"id\":\"fake-small\",\"name\":\"Fake Small\",\"reasoning\":false,\"input\":[\"text\"],\"contextWindow\":100000,\"maxTokens\":4096,"
                  "\"cost\":{\"input\":1,\"output\":2,\"cacheRead\":0,\"cacheWrite\":0}},{\"id\":\"fake-large\",\"name\":\"Fake Large\",\"reasoning\":false,"
                  "\"input\":[\"text\"],\"contextWindow\":100000,\"maxTokens\":4096,\"cost\":{\"input\":1,\"output\":2,\"cacheRead\":0,\"cacheWrite\":0}}");
    strcat(t, "]}}}\n");
    app_write("models.json", t);
}

/* P12-B9, B10, B15 (owner 2026-10-14: every menu by keyboard): /model, /thinking and /fork open pi 1.2.0's selectors over the screen's picker.
 * The goldens are pi's own screens (model_*, thinking_*, fork_*); the reachability check is that /model through pi_app opens the
 * selector (the screen shows its rows) and Enter switches the session's model (the footer and the next request name it) */
static void test_selectors(void)
{
    static const char *k_model[] = { "/model\r", "\004", 0 };
    static const char *k_search[] = { "/model\rlarge", "\004", 0 };
    static const char *k_down[] = { "/model\r\033[B", "\004", 0 };
    static const char *k_pick[] = { "/model\r\033[B\r", "\004", 0 };
    static const char *k_wrap[] = { "/model\r\033[A", "\004", 0 };
    static const char *k_think[] = { "/thinking\r", "\004", 0 };
    static const char *k_think_search[] = { "/thinking\rlow", "\004", 0 };
    static const char *k_think_pick[] = { "/thinking\r\033[B\r", "\004", 0 };
    static const int m_foot[] = { 24, 25, 0 };
    const char *quiet = "{\"quietStartup\":true,\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\"}\n";
    fresh_agent();
    app_nokeys = 1;
    app_model_cli = "fake/fake-model";
    fake_models(0, 1);
    skills_case("model_selector", quiet, k_model, 0, 0, 0, 0, m_foot, 0);
    skills_case("model_search", quiet, k_search, 0, 0, 0, 0, m_foot, 0);
    skills_case("model_down", quiet, k_down, 0, 0, 0, 0, m_foot, 0);
    rest_any = 1;                   /* (the footer names fake-small now: the status line shows when the screen is the one to take) */
    rest_find = "Model: fake-small";
    skills_case("model_select", quiet, k_pick, 0, 0, 0, 0, m_foot, 0);
    rest_any = 0;
    rest_find = "navigate";
    CHECK(app_out && strstr(app_out, "Model: fake-small") != 0);
    (void)k_wrap;
    {
        /* /model <text>: an exact id switches with no refresh; anything else refreshes every catalog first (one reachability check through
         * the program's entry point: the sentinel moved, and "Refreshing model catalogs" comes before the selector it opens) */
        static const char *k_arg[] = { "/model fake-small", "\033", "\r", "\004", 0 };
        static const char *k_arg_s[] = { "/model fak", "\033", "\r", "\004", 0 };
        long r0 = pi_model_cmd_refreshes;
        rest_any = 1;
        rest_find = "Model: fake-small";
        skills_case("model_arg", quiet, k_arg, 0, 0, 0, 0, m_foot, 0);
        rest_any = 0;
        rest_find = "navigate";
        CHECK(pi_model_cmd_refreshes == r0);
        xm_row = 12;                /* the search box: pi's setValue leaves its cursor at the start, the picker's is at the end of the text */
        xm_c0 = 2;
        xm_c1 = 5;
        xm_why = "the search input's cursor cell";
        skills_case("model_arg_search", quiet, k_arg_s, 0, 0, 0, 0, m_foot, 0);
        xm_row = 0;
        CHECK(pi_model_cmd_refreshes > r0);
        CHECK(app_out && strstr(app_out, "Refreshing model catalogs") != 0 && strstr(app_out, "Model Name") != 0 &&
              strstr(app_out, "Refreshing model catalogs") < strstr(app_out, "Model Name"));
    }
    {
        /* the scope: enabledModels shows the Scope line and the scoped list */
        const char *scoped = "{\"quietStartup\":true,\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\","
                             "\"enabledModels\":[\"fake/fake-model\",\"fake/fake-large\"]}\n";
        skills_case("model_scoped", scoped, k_model, 0, 0, 0, 0, m_foot, 0);
    }
    fake_models(1, 0);
    skills_case("thinking_selector", quiet, k_think, 0, 0, 0, 0, m_foot, 0);
    skills_case("thinking_search", quiet, k_think_search, 0, 0, 0, 0, m_foot, 0);
    rest_any = 1;
    rest_find = "Thinking level: minimal";
    skills_case("thinking_select", quiet, k_think_pick, 0, 0, 0, 0, m_foot, 0);
    rest_any = 0;
    rest_find = "navigate";
    CHECK(app_out && strstr(app_out, "Thinking level: minimal") != 0);
    app_nokeys = 0;
    app_model_cli = 0;
    app_write("models.json", "{}\n");
}

/* the agent directory's settings.json as text, "" when there is none */
static char *app_settings_text(void)
{
    char p[520];
    long n;
    char *t;
    strcpy(p, aagent);
    strcat(p, "/settings.json");
    {
        FILE *f = fopen(p, "rb");
        t = 0;
        n = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            t = (char *)calloc((size_t)n + 1, 1);
            if (t && fread(t, 1, (size_t)n, f) != (size_t)n)
                t[0] = 0;
            fclose(f);
        }
    }
    return t ? t : (char *)calloc(1, 1);
}

/* P12-B11 (owner 2026-10-14: build /scoped-models with the feature behind it): pi 1.2.0's scoped-models-selector.ts. The goldens are pi's own
 * screens (scoped_*); the settings.json pi leaves is kept as scoped_*.settings and compared by the enabledModels member. Reachability: /scoped-models
 * through pi_app opens the selector, Enter toggles a model, Ctrl+S writes enabledModels, and the next start's /model shows the Scope line
 * (the saved list is what Ctrl+P cycles). Old amiga-pi has no /scoped-models: every case fails by absence */
static void take_shot(void);
static int shot_has(const char *s);

static int settings_has(const char *needle)
{
    char *t = app_settings_text();
    int hit = strstr(t, needle) != 0;
    free(t);
    return hit;
}

static void test_scoped(void)
{
    static const char *k_open[] = { "/scoped-models\r", "\004", 0 };
    static const char *k_toggle[] = { "/scoped-models\r\033[B\r", "\004", 0 };
    static const char *k_search[] = { "/scoped-models\rlarge", "\004", 0 };
    static const char *k_move[] = { "/scoped-models\r\033[1;3B", "\004", 0 };
    static const char *k_clear[] = { "/scoped-models\r\030", "\004", 0 };
    static const char *k_prov[] = { "/scoped-models\r\030\020", "\004", 0 };
    static const char *k_save[] = { "/scoped-models\r\033[B\r\023", "\033", "\004", 0 };
    static const char *k_model[] = { "/model\r", "\004", 0 };
    static const int m_foot[] = { 24, 25, 0 };
    const char *quiet = "{\"quietStartup\":true,\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\"}\n";
    const char *listed = "{\"quietStartup\":true,\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\","
                         "\"enabledModels\":[\"fake/fake-large\",\"fake/fake-model\"]}\n";
    const char *two = "{\"quietStartup\":true,\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\","
                      "\"enabledModels\":[\"fake/fake-model\",\"fake/fake-large\"]}\n";
    fresh_agent();
    app_nokeys = 1;
    app_model_cli = "fake/fake-model";
    fake_models(0, 1);
    /* the first row is amiga-pi's built-in ollama model, whose name the catalog spells "Qwen2.5 Coder 7b"; pi gets the model from models.json with the id as its name */
    xm_row = 18;
    xm_c0 = 14;
    xm_c1 = 40;
    xm_why = "Model Name of the built-in ollama model: the catalog's name, where pi's models.json repeats the id";
    skills_case("scoped_selector", quiet, k_open, 0, 0, 0, 0, m_foot, 0);
    skills_case("scoped_clear", quiet, k_clear, 0, 0, 0, 0, m_foot, 0);
    skills_case("scoped_provider", quiet, k_prov, 0, 0, 0, 0, m_foot, 0);
    xm_row = 0;
    skills_case("scoped_listed", listed, k_open, 0, 0, 0, 0, m_foot, 0);
    skills_case("scoped_toggle", quiet, k_toggle, 0, 0, 0, 0, m_foot, 0);
    CHECK(!settings_has("enabledModels"));                 /* session-only until Ctrl+S */
    skills_case("scoped_search", quiet, k_search, 0, 0, 0, 0, m_foot, 0);
    skills_case("scoped_reorder", two, k_move, 0, 0, 0, 0, m_foot, 0);
    skills_case("scoped_save", quiet, k_save, 0, 0, 0, 0, m_foot, 0);
    /* what pi writes: the three enabled models, in the list's order, as "provider/id" strings */
    CHECK(settings_has("\"enabledModels\": [\n    \"ollama/qwen2.5-coder:7b\",\n    \"fake/fake-small\",\n    \"fake/fake-large\"\n  ]"));
    CHECK(settings_has("\"quietStartup\": true"));          /* every other member stayed */
    /* the next start: the saved list is the scope /model offers */
    {
        char *saved = app_settings_text();
        app_write("settings.json", saved);
        free(saved);
        CHECK_INT(app_run(80, 25, k_model, 0, 0, take_shot), 0);
        CHECK(shot_has("Scope:"));
    }
    /* the live effect: a model taken out in the selector is skipped by the next Ctrl+P (updateSessionModels), saved or not; the screen is
     * taken at the footer (the model changes, so the usual "-model" test of the rest does not hold) */
    {
        static const char *k_cycle[] = { "/scoped-models\r\033[B\033[B\r\003", "\020", "\004", 0 };
        static const char *k_plain[] = { "\020", "\004", 0 };
        rest_any = 1;
        rest_find = "100k (auto)";
        app_write("settings.json", quiet);
        CHECK_INT(app_run(80, 25, k_plain, 0, 0, take_shot), 0);
        CHECK(shot_has("100k (auto)") && shot_has("fake-small"));      /* all four enabled: the next model is fake-small (100k window) */
        app_write("settings.json", quiet);
        CHECK_INT(app_run(80, 25, k_cycle, 0, 0, take_shot), 0);
        CHECK(shot_has("100k (auto)") && shot_has("fake-large") && !shot_has("fake-small"));
        rest_any = 0;
        rest_find = "navigate";
    }
    app_nokeys = 0;
    app_model_cli = 0;
    app_write("models.json", "{}\n");
}

/* P12-B12, B13, B16 (owner 2026-10-14: /settings with the settings behind it): pi 1.2.0's settings-selector.ts. The goldens are pi's own
 * screens (settings_*, tools/pi_oracle/tui_gen.py), the settings.json pi leaves after a change kept as settings_*.settings. Old amiga-pi has no
 * /settings: every case fails by absence */
static const char *settings_base = "{\"quietStartup\":true,\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\"";

static const char *settings_json(const char *more)
{
    static char b[1200];
    strcpy(b, settings_base);
    if (more && more[0]) {
        strcat(b, ",");
        strcat(b, more);
    }
    strcat(b, "}\n");
    return b;
}

/* the member of settings.json: its text after "key": up to the end of the line */
static int settings_member(const char *key, const char *want)
{
    char *t = app_settings_text(), pat[120], *at;
    int ok = 0;
    strcpy(pat, "\"");
    strcat(pat, key);
    strcat(pat, "\": ");
    at = strstr(t, pat);
    if (at) {
        at += strlen(pat);
        ok = !strncmp(at, want, strlen(want)) && (at[strlen(want)] == ',' || at[strlen(want)] == '\n');
    }
    free(t);
    return ok;
}

static void test_settings_screens(void)
{
    static const char *k_open[] = { "/settings\r", "\004", 0 };
    static const char *k_end[] = { "/settings\r\033[A", "\004", 0 };
    static const char *k_down[] = { "/settings\r\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B", "\004", 0 };
    static const char *k_search[] = { "/settings\rsteer", "\004", 0 };
    static const char *k_none[] = { "/settings\rzzqq", "\004", 0 };
    static const char *k_toggle[] = { "/settings\r\r", "\004", 0 };
    static const char *k_enum[] = { "/settings\rsteering\r", "\004", 0 };
    static const char *k_follow[] = { "/settings\rfollow", "\004", 0 };
    static const int m_foot[] = { 24, 25, 0 };
    fresh_agent();
    app_nokeys = 1;
    app_model_cli = "fake/fake-model";
    fake_models(0, 0);
    skills_case("settings_list", settings_json(0), k_open, 0, 0, 0, 0, m_foot, 0);
    skills_case("settings_end", settings_json(0), k_end, 0, 0, 0, 0, m_foot, 0);
    skills_case("settings_down", settings_json(0), k_down, 0, 0, 0, 0, m_foot, 0);
    skills_case("settings_search", settings_json(0), k_search, 0, 0, 0, 0, m_foot, 0);
    skills_case("settings_nomatch", settings_json(0), k_none, 0, 0, 0, 0, m_foot, 0);
    skills_case("settings_followup", settings_json(0), k_follow, 0, 0, 0, 0, m_foot, 0);
    skills_case("settings_toggle", settings_json(0), k_toggle, 0, 0, 0, 0, m_foot, 0);
    CHECK(settings_member("enabled", "false"));                  /* compaction.enabled: pi wrote the member inside "compaction" */
    skills_case("settings_enum", settings_json(0), k_enum, 0, 0, 0, 0, m_foot, 0);
    CHECK(settings_member("steeringMode", "\"all\""));
    /* values read from settings.json, as pi's getters give them: every row off its default, the window moved down the list */
    {
        static const char *k_b[] = { "/settings\r\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B", "\004", 0 };
        static const char *k_c[] = { "/settings\r\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B"
                                     "\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B\033[B", "\004", 0 };
        static char all[1400];
        static const int m_tall[] = { 44, 45, 0 };           /* these captures are 45 rows tall */
        strcpy(all, "{\"defaultProvider\":\"fake\",\"defaultModel\":\"fake-model\",\"defaultThinkingLevel\":\"off\",\"quietStartup\":true,"
                    "\"compaction\":{\"enabled\":false,\"reserveTokens\":1000},\"steeringMode\":\"all\",\"followUpMode\":\"all\",");
        strcat(all, "\"hideThinkingBlock\":true,\"terminal\":{\"showImages\":false,\"clearOnShrink\":true},\"images\":{\"blockImages\":true},"
                    "\"outputPad\":0,\"editorPaddingX\":2,\"autocompleteMaxVisible\":10,\"defaultProjectTrust\":\"always\",");
        strcat(all, "\"doubleEscapeAction\":\"fork\",\"treeFilterMode\":\"user-only\",\"fullscreenWheelScrollLines\":7,\"httpIdleTimeoutMs\":0,"
                    "\"transport\":\"sse\",\"cacheWarming\":\"idle\",\"markdown\":{\"mermaid\":\"off\"},\"enableInstallTelemetry\":false,");
        strcat(all, "\"collapseChangelog\":true,\"showCacheMissNotices\":true,\"enableSkillCommands\":false,\"showHardwareCursor\":true,"
                    "\"tuiMode\":\"fullscreen\",\"fullscreenExitOutput\":\"resume-hint\",\"fullscreenScrollbar\":\"hidden\",");
        strcat(all, "\"fullscreenCopyOnSelect\":false,\"warnings\":{\"anthropicExtraUsage\":false}}\n");
        skills_case("settings_set", all, k_open, 0, 0, 0, 0, m_tall, 0);
        skills_case("settings_set_b", all, k_b, 0, 0, 0, 0, m_tall, 0);
        skills_case("settings_set_c", all, k_c, 0, 0, 0, 0, m_tall, 0);
        skills_case("settings_set_d", all, k_end, 0, 0, 0, 0, m_tall, 0);
    }
    /* the theme menus (B13), the warnings menu, the per-model thinking levels */
    {
        static const char *k_theme[] = { "/settings\rtheme\r", "\004", 0 };
        static const char *k_pick[] = { "/settings\rtheme\r\033[B\033[B\033[B\r", "\004", 0 };
        static const char *k_auto[] = { "/settings\rtheme\r\033[B\r", "\004", 0 };
        static const char *k_apply[] = { "/settings\rtheme\r\033[B\r\033[B\033[B\r", "\004", 0 };
        static const char *k_warn[] = { "/settings\rwarnings\r", "\004", 0 };
        static const char *k_warn_off[] = { "/settings\rwarnings\r\r", "\033", "\004", 0 };
        static const char *k_mt[] = { "/settings\rper model\r", "\004", 0 };
        static const char *k_mt_level[] = { "/settings\rper model\r\r", "\004", 0 };
        static const char *k_mt_set[] = { "/settings\rper model\r\r\033[B\033[B\r", "\004", 0 };
        static const char *k_img[] = { "/settings\r", "\004", 0 };
        static const char *k_img_off[] = { "/settings\r\033[B\r", "\004", 0 };
        skills_case("settings_theme", settings_json(0), k_theme, 0, 0, 0, 0, m_foot, 0);
        skills_case("settings_theme_auto", settings_json(0), k_auto, 0, 0, 0, 0, m_foot, 0);
        skills_case("settings_theme_pick", settings_json(0), k_pick, 0, 0, 0, 0, m_foot, 0);
        CHECK(settings_member("theme", "\"light\""));
        skills_case("settings_theme_apply", settings_json(0), k_apply, 0, 0, 0, 0, m_foot, 0);
        CHECK(settings_member("theme", "\"system/system\""));
        skills_case("settings_warnings", settings_json(0), k_warn, 0, 0, 0, 0, m_foot, 0);
        skills_case("settings_warnings_off", settings_json(0), k_warn_off, 0, 0, 0, 0, m_foot, 0);
        CHECK(settings_has("\"warnings\": {\n    \"anthropicExtraUsage\": false\n  }"));
        app_imgproto = "kitty";
        skills_case("settings_images", settings_json(0), k_img, 0, 0, 0, 0, m_foot, 0);
        skills_case("settings_images_off", settings_json(0), k_img_off, 0, 0, 0, 0, m_foot, 0);
        CHECK(settings_has("\"terminal\": {\n    \"showImages\": false\n  }"));
        app_imgproto = 0;
        fake_models(1, 1);
        skills_case("settings_modelthinking", settings_json(0), k_mt, 0, 0, 0, 0, m_foot, 0);
        skills_case("settings_modelthinking_level", settings_json(0), k_mt_level, 0, 0, 0, 0, m_foot, 0);
        skills_case("settings_modelthinking_set", settings_json(0), k_mt_set, 0, 0, 0, 0, m_foot, 0);
        CHECK(settings_has("\"modelThinkingLevels\": {\n    \"fake/fake-model\": \"low\"\n  }"));
    }
    app_nokeys = 0;
    app_model_cli = 0;
    app_write("models.json", "{}\n");
}

/* P12-B12 behaviours (owner 2026-10-14: every knob of /settings moves the output or carries a written exemption): the rows whose effect is on the
 * screen. editorPaddingX, autocompleteMaxVisible against pi's own captures (pad_*, menu_max*); showHardwareCursor, autocompact's "(auto)", the skill
 * commands of the slash menu, doubleEscapeAction, treeFilterMode, outputPad, and a model's own thinking level by the screen they change */
static void test_settings_behaviour(void)
{
    static const char *k_pad[] = { "hello padded world", "\004", 0 };
    static const char *k_menu[] = { "/", "\004", 0 };
    static const char *k_abc[] = { "abc", "\004", 0 };
    static const char *k_skill[] = { "/skill:", "\004", 0 };
    static const char *k_toggle_skill[] = { "/settings\rskill\r\003", "/skill:", "\004", 0 };
    static const char *k_two_esc[] = { "\033", "\033", "\004", 0 };
    static const char *k_tree[] = { "/tree\r", "\004", 0 };
    static const char *k_model[] = { "/model fake-model\r", "\004", 0 };
    static const int m_foot[] = { 24, 25, 0 };
    static const int m_menu[] = { 23, 23, 25, 25, 29, 30, 0 };
    fresh_agent();
    app_nokeys = 1;
    app_model_cli = "fake/fake-model";
    fake_models(0, 0);
    pi_ui_set_commands(pi_cmds, (int)(sizeof(pi_cmds) / sizeof(pi_cmds[0])));
    skills_case("pad_editor", settings_json("\"editorPaddingX\":2"), k_pad, 0, 0, 0, 0, m_foot, 0);
    skills_case("pad_menu", settings_json("\"editorPaddingX\":3,\"autocompleteMaxVisible\":10"), k_menu, 0, 0, 0, 0, m_foot, 0);
    skills_case("menu_max3", settings_json("\"autocompleteMaxVisible\":3"), k_menu, 0, 0, 0, 0, m_foot, 0);
    pi_ui_set_commands(0, 0);
    /* showHardwareCursor: the window's cursor rests on the editor's cursor cell (after "abc"), and is hidden otherwise */
    app_write("settings.json", settings_json("\"showHardwareCursor\":true"));
    CHECK_INT(app_run(80, 25, k_abc, 0, 0, take_shot), 0);
    CHECK(shot_cursor && shot_cx == 3 && shot_cy >= 0 && strstr(shot[shot_cy], "abc") != 0);
    CHECK(app_out && strstr(app_out, "\033[7m") == 0);          /* pi 1.2.0 resolveFakeCursors: no reverse-video cell with the hardware cursor */
    app_write("settings.json", settings_json(0));
    CHECK_INT(app_run(80, 25, k_abc, 0, 0, take_shot), 0);
    CHECK(!shot_cursor);
    {   /* off: the cell is ESC[7m<cell>ESC[27m, as in pi 1.2.0's own bytes */
        long gn;
        char *gb = slurp("status_bytes.bytes", &gn);
        CHECK(gb != 0 && strstr(gb, "hello\033[7m \033[27m") != 0);
        CHECK(app_out && strstr(app_out, "abc\033[7m \033[27m") != 0);
        free(gb);
    }
    /* autocompact: the footer's "(auto)" while auto-compaction is on */
    CHECK_INT(app_run(80, 25, k_abc, 0, 0, take_shot), 0);
    CHECK(shot_has("(auto)"));
    app_write("settings.json", settings_json("\"compaction\":{\"enabled\":false}"));
    CHECK_INT(app_run(80, 25, k_abc, 0, 0, take_shot), 0);
    CHECK(!shot_has("(auto)"));
    /* doubleEscapeAction: Esc twice on the empty editor opens /tree (default) or /fork, or nothing */
    {
        char path[600];
        if (put_session("../sessions/doc_branch.jsonl", "esc_session.jsonl", path)) {
            app_session = path;
            app_write("settings.json", settings_json(0));
            CHECK_INT(app_run(80, 25, k_two_esc, 0, 0, take_shot), 0);
            CHECK(shot_has("Session Tree"));
            app_write("settings.json", settings_json("\"doubleEscapeAction\":\"fork\""));
            CHECK_INT(app_run(80, 25, k_two_esc, 0, 0, take_shot), 0);
            CHECK(shot_has("Fork from Message"));
            app_write("settings.json", settings_json("\"doubleEscapeAction\":\"none\""));
            CHECK_INT(app_run(80, 25, k_two_esc, 0, 0, take_shot), 0);
            CHECK(!shot_has("Fork from Message") && !shot_has("Session Tree"));
            /* treeFilterMode: /tree starts with the filter the setting names */
            app_write("settings.json", settings_json("\"treeFilterMode\":\"user-only\""));
            CHECK_INT(app_run(80, 25, k_tree, 0, 0, take_shot), 0);
            CHECK(shot_has("[user]"));
            app_write("settings.json", settings_json(0));
            CHECK_INT(app_run(80, 25, k_tree, 0, 0, take_shot), 0);
            CHECK(shot_has("Session Tree") && !shot_has("[user]"));
            /* outputPad: the transcript's side padding, at the start and live from /settings (the transcript is drawn again) */
            {
                static const char *k_pad0[] = { "/settings\routput pad\r\003", "\004", 0 };
                static const char *k_none[] = { "\004", 0 };
                int r, at;
                app_write("settings.json", settings_json(0));
                CHECK_INT(app_run(80, 25, k_none, 0, 0, take_shot), 0);
                for (at = -1, r = 0; r < nshot; r++)
                    if (strstr(shot[r], "B answer"))
                        at = r;
                CHECK(at >= 0 && shot[at][0] == ' ' && shot[at][1] == 'B');
                CHECK_INT(app_run(80, 25, k_pad0, 0, 0, take_shot), 0);
                for (at = -1, r = 0; r < nshot; r++)
                    if (strstr(shot[r], "B answer"))
                        at = r;
                CHECK(at >= 0 && shot[at][0] == 'B');
                CHECK(settings_member("outputPad", "0"));
            }
            app_session = 0;
        } else
            CHECK(0);
    }
    /* a model's own thinking level (modelThinkingLevels): /model to it applies the level; none, the settings' default */
    fake_models(1, 1);
    app_model_cli = "fake/fake-small";
    app_write("settings.json", settings_json(0));
    CHECK_INT(app_run(80, 25, k_model, 0, 0, take_shot), 0);
    CHECK(shot_has("fake-model") && shot_has("thinking off"));
    app_write("settings.json", settings_json("\"modelThinkingLevels\":{\"fake/fake-model\":\"low\"}"));
    CHECK_INT(app_run(80, 25, k_model, 0, 0, take_shot), 0);
    CHECK(shot_has("fake-model \342\200\242 low"));
    app_model_cli = "fake/fake-model";
    /* enableSkillCommands: the /skill:name rows of the slash menu, at the start and after the row is toggled in /settings */
    app_nokeys = 0;
    app_model_cli = 0;
    skills_tree(settings_json(0));
    app_agent = skdir;
    CHECK_INT(app_run(80, 25, k_skill, 0, 0, take_shot), 0);
    CHECK(shot_has("skill:greet"));
    skills_tree(settings_json("\"enableSkillCommands\":false"));
    CHECK_INT(app_run(80, 25, k_skill, 0, 0, take_shot), 0);
    CHECK(!shot_has("skill:greet"));
    CHECK_INT(app_run(80, 25, k_toggle_skill, 0, 0, take_shot), 0);             /* toggled on in /settings: the rows are there at once */
    if (getenv("PI_TUI_DUMP")) {
        int r;
        for (r = 0; r < nshot; r++)
            printf("    %2d|%s\n", r + 1, shot[r]);
    }
    CHECK(shot_has("skill:greet"));
    skills_tree(settings_json(0));
    CHECK_INT(app_run(80, 25, k_toggle_skill, 0, 0, take_shot), 0);             /* toggled off */
    CHECK(!shot_has("skill:greet"));
    app_agent = 0;
    (void)m_menu;
    app_write("models.json", "{}\n");
}

/* P12-B17 (owner 2026-10-14: build the config selector with its command): `amiga-pi config`, pi 1.2.0's config-selector.ts, against pi's own
 * captures of `pi config -na` (config_*): the groups, the boxes, the search, the toggle (and what it writes to settings.json: an entry
 * `-<path>` of the "skills" or "prompts" array, which the next start reads back). Not compared: the settings file's name under the title (pi
 * prints ~/.pi/agent/settings.json) and pi's Built-in group (codemode, llama.cpp, mcp, tool-search: node packages amiga-pi does not have) */
static void test_config(void)
{
    static const char *k_none[] = { "\004", 0 };
    static const char *k_down[] = { "\033[B\033[B", "\004", 0 };
    static const char *k_space[] = { " ", "\004", 0 };
    static const char *k_back[] = { "\033[B\033[B \033[A ", "\004", 0 };
    static const char *k_search[] = { "fix", "\004", 0 };
    static const char *k_nomatch[] = { "zzqq", "\004", 0 };
    static const int m_list[] = { 5, 5, 16, 25, 0 };
    static const int m_hint[] = { 5, 5, 0 };
    static const int m_empty[] = { 5, 5, 8, 25, 0 };
    const char *quiet = "{\"quietStartup\":true}\n";
    char *saved;
    fresh_agent();
    tree_json = "config_files.json";
    app_config_cli = "-na";
    rest_any = 1;
    rest_find = "Global Resources";
    skills_case("config_list", quiet, k_none, 0, 0, 0, 0, m_list, 1);
    skills_case("config_down", quiet, k_down, 0, 0, 0, 0, m_list, 1);
    skills_case("config_search", quiet, k_search, 0, 0, 0, 0, m_hint, 1);
    skills_case("config_nomatch", quiet, k_nomatch, 0, 0, 0, 0, m_hint, 1);
    skills_case("config_off", "{\"quietStartup\":true,\"skills\":[\"-skills/terse/SKILL.md\"],\"prompts\":[\"-prompts/fix.md\"]}\n", k_none, 0, 0, 0, 0, m_list, 1);
    skills_case("config_toggle", quiet, k_space, 0, 0, 0, 0, m_list, 1);
    saved = 0;
    {
        char p[600];
        FILE *f;
        long n;
        strcpy(p, skdir);
        strcat(p, "/settings.json");
        f = fopen(p, "rb");
        saved = (char *)calloc(1, 2000);
        n = f ? (long)fread(saved, 1, 1999, f) : 0;
        if (f)
            fclose(f);
        (void)n;
    }
    CHECK(strstr(saved, "\"skills\": [\n    \"-skills/greet/SKILL.md\"\n  ]") != 0);              /* pi's entry: the path below the group's directory */
    free(saved);
    skills_case("config_toggle_back", quiet, k_back, 0, 0, 0, 0, m_list, 1);
    {
        char p[600];
        FILE *f;
        saved = (char *)calloc(1, 2000);
        strcpy(p, skdir);
        strcat(p, "/settings.json");
        f = fopen(p, "rb");
        if (f && fread(saved, 1, 1999, f) < 1)
            saved[0] = 0;
        if (f)
            fclose(f);
        CHECK(strstr(saved, "\"-prompts/fix.md\"") != 0 && strstr(saved, "\"-skills/terse/SKILL.md\"") != 0);
        free(saved);
    }
    /* the next start reads the entries back: greet is off in the slash menu's /skill:name rows */
    app_config_cli = 0;
    rest_any = 0;
    rest_find = "navigate";
    app_home = 0;
    (void)m_empty;
    tree_json = "skills_files.json";
}

/* P12-B2/B3 (D1): What's New after an update and /changelog, from amiga-pi's CHANGELOG.md in pi's format. The goldens are pi 1.2.0's own
 * screens for the same file (tests/pi/tui/changelog_fixture.md, PI_PACKAGE_DIR): the new entries between two rules with the loaded
 * resources above, collapsed to one line, nothing on a first start (which records the version), nothing when the version is
 * current, every entry oldest first for /changelog; a resumed session shows nothing and records nothing */
static void test_whats_new(void)
{
    static const char *keys_end[] = { "\004", 0 }, *keys_cmd[] = { "/changelog\r", "\004", 0 };
    static const int m_bot[] = { 21, 25, 0 };
    char *t;
    {
        /* pi's parseChangelog / getNewEntries: numbers compare as numbers, a "## " heading with no version ends the entry and starts none */
        static const char doc[] = "# Changelog\n\n## [Unreleased]\n\n- later\n\n## [1.10.0] - x\n\n- ten\n\n## 1.9.2\n- nine\n\n## [1.9.0]\n- old\n";
        jw o;
        char ver[24];
        jw_init(&o);
        CHECK_INT(pi_cl_newer(doc, (long)strlen(doc), "1.9.1", &o), 2);
        CHECK_STR(o.p, "## [1.10.0] - x\n\n- ten\n\n## 1.9.2\n- nine");
        jw_reset(&o);
        CHECK_INT(pi_cl_newer(doc, (long)strlen(doc), "1.10.0", &o), 0);
        jw_reset(&o);
        CHECK_INT(pi_cl_all(doc, (long)strlen(doc), &o), 3);
        CHECK(strstr(o.p, "## [1.9.0]") == o.p);                    /* oldest first */
        pi_cl_latest("junk\n## [1.10.0] - x\n## [1.9.0]", ver, sizeof(ver));
        CHECK_STR(ver, "1.10.0");
        jw_free(&o);
    }
    app_changelog = PI_TESTS "/pi/tui/changelog_fixture.md";
    fresh_agent();
    skills_case("whats_new", "{\"quietStartup\":true,\"lastChangelogVersion\":\"1.1.0\"}\n", keys_end, 0, 0, 0, 0, m_bot, 0);
    t = app_settings_text();
    CHECK(strstr(t, "\"lastChangelogVersion\": \"1.2.0\"") != 0);          /* recorded once shown */
    free(t);
    skills_case("whats_new_collapsed", "{\"quietStartup\":true,\"lastChangelogVersion\":\"1.1.0\",\"collapseChangelog\":true}\n", keys_end,
                0, 0, 0, 0, m_bot, 0);
    skills_case("whats_new_fresh", "{\"quietStartup\":true}\n", keys_end, 0, 0, 0, 0, m_bot, 0);
    t = app_settings_text();
    CHECK(strstr(t, "\"lastChangelogVersion\": \"1.2.0\"") != 0);          /* a first start records the version and shows nothing */
    free(t);
    skills_case("whats_new_current", "{\"quietStartup\":true,\"lastChangelogVersion\":\"1.2.0\"}\n", keys_end, 0, 0, 0, 0, m_bot, 0);
    skills_case("changelog_cmd", "{\"quietStartup\":true,\"lastChangelogVersion\":\"1.2.0\"}\n", keys_cmd, 0, 0, 0, 0, m_bot, 0);
    /* a resumed session: no notice, nothing recorded */
    {
        char path[600];
        static const char *k2[] = { "\004", 0 };
        if (put_session("took_session.jsonl", "took_session.jsonl", path)) {
            app_write("settings.json", "{\"quietStartup\":true,\"lastChangelogVersion\":\"1.1.0\"}\n");
            app_session = path;
            CHECK_INT(app_run(80, 25, k2, 0, 0, 0), 0);
            app_session = 0;
            CHECK(app_out && strstr(app_out, "What's New") == 0);
            t = app_settings_text();
            CHECK(strstr(t, "\"lastChangelogVersion\":\"1.1.0\"") != 0);
            free(t);
        }
    }
    /* without a CHANGELOG.md: /changelog says so, and a start with an older version shows and records nothing */
    app_changelog = 0;
    app_write("settings.json", "{\"quietStartup\":true,\"lastChangelogVersion\":\"1.1.0\"}\n");
    CHECK_INT(app_run(80, 25, keys_cmd, 0, 0, 0), 0);
    CHECK(app_out && strstr(app_out, "No changelog entries found.") != 0);
    t = app_settings_text();
    CHECK(strstr(t, "\"lastChangelogVersion\":\"1.1.0\"") != 0);
    free(t);
    app_write("settings.json", "{\"quietStartup\":true}\n");
}

static void test_login_screens(void)
{
    static const char *k_method[] = { "/login\r", "\004", 0 };
    static const char *k_accounts[] = { "/login\r", "\r", "\004", 0 };
    static const char *k_search[] = { "/login\r", "\r", "cop", "\004", 0 };
    static const char *k_keys[] = { "/login\r", "\033[B", "\r", "\004", 0 };
    static const char *k_copilot[] = { "/login github-copilot\r", "\r", "\004", 0 };
    static const char *k_openai[] = { "/login openai\r", "\r", "\004", 0 };
    static const char *k_back[] = { "/login\r", "\r", "\033", "\004", 0 };
    static const char *k_copilot_esc[] = { "/login github-copilot\r", "\r", "\033", "\004", 0 };
    static const char *k_openai_esc[] = { "/login openai\r", "\r", "\033", "\004", 0 };
    static const char *k_openai_method[] = { "/login openai\r", "\004", 0 };
    static const char *k_logout[] = { "/logout\r", "\004", 0 };
    static const char *k_device[] = { "/login github-copilot\r", "\r", "\r", 0 };
    static const char *const sse_device[] = { "login_device.1.http" };
    static const int m_device[] = { 17, 17, 24, 25, 0 };
    static const int m_foot[] = { 24, 25, 0 }, m_cancel[] = { 1, 2, 21, 21, 23, 25, 0 };
    static const int m_openai[] = { 8, 9, 11, 14, 15, 15, 24, 25, 0 };
    const char *settings = "{\"quietStartup\":true}\n";
    fresh_agent();
    /* pi offers Radius as a third method, and so does this build (L2). The status after its name is muted in pi; the extension
     * selector of vtcon's tui_menu draws plain text (a control character in a row becomes '?'), so the style of that span is not
     * compared: row 19, columns 22-38 (" \342\200\242 not configured") */
    xm_row = 19;
    xm_c0 = 22;
    xm_c1 = 38;
    xm_why = "the muted status of the Radius option: tui_menu rows take no styled text";
    skills_case("login_method", settings, k_method, 0, 0, 0, 0, m_foot, 0);
    CHECK(load_login_rows(1) == 9);
    pi_ui_set_login_rows(lrow, lrow_n, lrow_oauth);
    skills_case("login_providers", settings, k_accounts, 0, 0, 0, 0, m_foot, 0);
    skills_case("login_search", settings, k_search, 0, 0, 0, 0, m_foot, 0);
    CHECK(load_login_rows(0) == 43);
    pi_ui_set_login_rows(lrow, lrow_n, lrow_oauth);
    skills_case("login_apikey", settings, k_keys, 0, 0, 0, 0, m_foot, 0);
    pi_ui_set_login_rows(0, 0, 0);
    skills_case("login_copilot", settings, k_copilot, 0, 0, 0, 0, m_foot, 0);
    skills_case("login_openai", settings, k_openai, 0, 0, 0, 0, m_openai, 0);
    /* the method of one provider that has two; Esc in a dialog ends it with pi 1.1.0's error; Esc in the provider list goes back to the method */
    skills_case("login_openai_method", settings, k_openai_method, 0, 0, 0, 0, m_foot, 0);
    /* (the first item of an empty transcript lacks pi's spacer row above it, so rows 1 and 2 are left out; the message is looked for) */
    skills_case("login_copilot_back", settings, k_copilot_esc, 0, 0, 0, 0, m_cancel, 0);
    CHECK(app_out && strstr(app_out, "Error: Failed to login to GitHub Copilot: This operation was aborted") != 0);
    skills_case("login_openai_cancel", settings, k_openai_esc, 0, 0, 0, 0, m_cancel, 0);
    CHECK(app_out && strstr(app_out, "Error: Failed to login to OpenAI: This operation was aborted") != 0);
    skills_case("login_method", settings, k_back, 0, 0, 0, 0, m_foot, 0);
    xm_row = 0;
    /* the device code screen, up to the first poll of the provider (the answer to the device code request is played, the golden's
     * sign-in URL row has pi's "Cmd+click to open" where this build says how to copy it) */
    rest_any = 1;                   /* (the provider is polled while the request runs: the screen is taken once it shows the code) */
    rest_now = 1;
    rest_find = "Waiting for authentication";
    skills_case("login_device", settings, k_device, sse_device, 1, 0, 0, m_device, 0);
    rest_any = 0;
    rest_now = 0;
    rest_find = "navigate";
    /* /logout: the stored credentials, the kinds said because there are two */
    app_write("auth.json", "{\"openai\":{\"type\":\"api_key\",\"key\":\"sk-test\"},\"github-copilot\":{\"type\":\"oauth\","
              "\"access\":\"a\",\"refresh\":\"r\",\"expires\":99999999999999}}\n");
    skills_case("login_logout", settings, k_logout, 0, 0, 0, 0, m_foot, 0);
    app_write("auth.json", "{}\n");
}

/* U1, the reachability of the input paths: a key typed into the dialog reaches the credential file (the path through
 * login_key, the screen's ask hook and tui_dialog_read), and a search typed in the provider selector picks the provider
 * that Enter then logs in to */
static void test_login_typed(void)
{
    static const char *k_key[] = { "/login openai\r", "\033[B", "\r", "sk-live-123\r", "\004", 0 };
    static const char *k_find[] = { "/login\r", "\r", "kimi", "\r", "\004", 0 };
    char path[520], buf[2000];
    FILE *f;
    size_t n = 0;
    fresh_agent();
    app_write("settings.json", "{\"quietStartup\":true}\n");
    app_write("auth.json", "{}\n");
    CHECK_INT(app_run(80, 25, k_key, 0, 0, take_shot), 0);
    CHECK(app_out && strstr(app_out, "Login to OpenAI") != 0);
    CHECK(app_out && strstr(app_out, "sk-live-123") != 0);
    CHECK(app_out && strstr(app_out, "Saved API key for OpenAI") != 0);
    strcpy(path, aagent);
    strcat(path, "/auth.json");
    f = fopen(path, "rb");
    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    if (getenv("PI_TUI_DUMP")) printf("%s\n", buf);
    CHECK(strstr(buf, "\"openai\"") != 0 && strstr(buf, "sk-live-123") != 0);
    app_write("auth.json", "{}\n");
    CHECK_INT(app_run(80, 25, k_find, 0, 0, take_shot), 0);
    CHECK(app_out && strstr(app_out, "Login to Kimi For Coding") != 0);
}

static void test_skills_screen(void)
{
    static const char *keys_end[] = { "\004", 0 };
    static const char *keys_menu[] = { "/skill:", "\004", 0 };
    static const char *keys_exp[] = { "\017", "\004", 0 };
    static const char *keys_cmd[] = { "/skill:greet please greet Bob\r", "\004", 0 };
    static const char *const sse_cmd[] = { "skill_cmd.1.sse" };
    static const int m_start[] = { 2, 2, 22, 40, 0 }, m_quiet[] = { 26, 30, 0 }, m_menu[] = { 23, 23, 25, 25, 29, 30, 0 },
                     m_exp[] = { 36, 40, 0 };
    char cmd[460];
    /* the start: header, "[Skills]", "[Skill conflicts]" (the capture's rows 10-21; ours begin four rows higher: pi's help panel and tips are not drawn) */
    skills_case("skills_start", "{}\n", keys_end, 0, 0, 0, 0, m_start, 1);
    /* quietStartup: the diagnostics alone, at the top */
    skills_case("skills_quiet", "{\"quietStartup\":true}\n", keys_end, 0, 0, 0, 0, m_quiet, 1);
    skills_case("skills_menu", "{\"quietStartup\":true}\n", keys_menu, 0, 0, 0, 0, m_menu, 1);
    skills_case("resume_skill", "{\"quietStartup\":true}\n", keys_end, 0, 0, 1, 0, m_quiet, 0);
    skills_case("resume_skill_expanded", "{\"quietStartup\":true}\n", keys_exp, 0, 0, 1, 0, m_exp, 0);
    skills_case("skill_cmd", "{\"quietStartup\":true}\n", keys_cmd, sse_cmd, 1, 0, 0, m_quiet, 1);
    /* I8: Ctrl+O at the start draws the [Skills] list grouped by scope (pi's header is long when expanded: not drawn here, its rows
     * left out; the block is compared from its title) */
    {
        static const int m_sexp[] = { 2, 2, 18, 19, 43, 50, 0 };
        skills_case("skills_start_expanded", "{\"quietStartup\":false}\n", keys_exp, 0, 0, 0, 0, m_sexp, 1);
    }
    /* J5: the [Prompts] list, compact and grouped, and the [Prompt conflicts] block (a name two files have, YAML that is refused) */
    {
        static const int m_pstart[] = { 2, 2, 22, 40, 0 }, m_pexp[] = { 2, 2, 18, 19, 34, 40, 0 }, m_pconf[] = { 26, 30, 0 };
        fresh_agent();
        tree_json = "prompts_files.json";
        skills_case("prompts_start", "{}\n", keys_end, 0, 0, 0, 0, m_pstart, 1);
        skills_case("prompts_start_expanded", "{\"quietStartup\":false}\n", keys_exp, 0, 0, 0, 0, m_pexp, 1);
        {
            static const char *keys_msg[] = { "hello\r", "\004", 0 };
            static const char *const sse_msg[] = { "prompts_start_msg.1.sse" };
            skills_case("prompts_start_msg", "{\"quietStartup\":false}\n", keys_msg, sse_msg, 1, 0, 0, m_pstart, 1);
        }
        fresh_agent();
        tree_json = "conflict_files.json";
        skills_case("prompts_conflicts", "{\"quietStartup\":true,\"prompts\":[\"extra\"]}\n", keys_end, 0, 0, 0, 0, m_pconf, 1);
        tree_json = "skills_files.json";
    }
    /* /reload: files made after the start show in the lists (the same screen pi gives; the status line says what this port reads again) */
    {
        static const char *keys_reload[] = { "/reload\r", "\004", 0 };
        static const int m_reload[] = { 2, 2, 17, 17, 20, 40, 0 };
        fresh_agent();
        tree_json = "reload_files.json";
        app_ready = reload_writes;
        skills_case("reload_resources", "{\"quietStartup\":false}\n", keys_reload, 0, 0, 0, 0, m_reload, 1);
        app_ready = 0;
        tree_json = "skills_files.json";
    }
    if (skdir[0] && strlen(skdir) > 10) {
        strcpy(cmd, "rm -rf '");
        strncat(cmd, skdir, strlen(skdir) - 6);         /* /tmp/pis_XXXXXX/agent without "/agent" */
        strcat(cmd, "'");
        if (system(cmd))
            CHECK(0);
    }
}

/* R4: Ctrl+O (app.tools.expand) toggles pi's toolOutputExpanded (interactive-mode.ts:4557-4574): the compaction and branch
 * summaries show their Markdown (compaction-summary-message.ts:49-54, branch-summary-message.ts:43-48) and the status
 * "Tool output: expanded" takes the place of the last status line ("Session compacted 1 time", showStatus replaces a
 * status that is the last row); a second Ctrl+O collapses them again. Goldens are pi 1.1.0's own screens. */
static void test_expand_summaries(void)
{
    static const char *one[] = { "\017", "\004", 0 };
    static const char *two[] = { "\017", "\017", "\004", 0 };
    session_case("resume_summaries_expanded", "summaries_session.jsonl", "{\"quietStartup\":true}\n", one, 0, 0);
    session_case("dark_resume_summaries_expanded", "summaries_session.jsonl", "{\"quietStartup\":true,\"theme\":\"dark\"}\n", one,
                 0, 0);
    session_case("dark_pad0_resume_summaries_expanded", "summaries_session.jsonl",
                 "{\"quietStartup\":true,\"theme\":\"dark\",\"outputPad\":0}\n", one, 0, 0);
    session_case("resume_summaries_toggled", "summaries_session.jsonl", "{\"quietStartup\":true}\n", two, 0, 0);
}

/* Enter in the selector moves the leaf (a user message goes back to the editor) and the conversation is drawn again */
/* P12-B15: /fork opens pi's user-message selector (user-message-selector.ts): the title and its description, the messages with their
 * position, the newest selected, Up moving to the one before; Enter forks there (the chat is drawn again, the message is back in the
 * editor, "Forked to new session"). The goldens are pi's own screens for doc_branch.jsonl (the rows above the selector are the transcript
 * and the model warning of the capture's fake model) */
static void fork_case(const char *name, const char **keys)
{
    static scen sc = { "", { 0 }, { 0 }, 0, 1, 0, { { 1, 6, 0, 79, "the transcript above the selector, and the warning for the capture's fake model" },
                                                    { 24, 25, 0, 79, "the folder and the model of the capture" } } };
    char path[600];
    int i;
    sc.name = name;
    if (!put_session("../sessions/doc_branch.jsonl", "fork_session.jsonl", path))
        return;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    cur = &sc;
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    load_golden(name);
    CHECK(want != 0);
    if (!want)
        return;
    app_session = path;
    CHECK_INT(app_run(80, want_rows, keys, 0, 0, tree_rest), 0);
    app_session = 0;
    CHECK(taken);
    if (diffs) {
        printf("  %s: %d cells differ from pi's capture\n%s", name, diffs, first_diff);
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    vt_free(want);
}

static void test_fork_selector(void)
{
    static const char *k_fork[] = { "/fork\r", "\004", 0 };
    static const char *k_up[] = { "/fork\r\033[A", "\004", 0 };
    static const char *k_pick[] = { "/fork\r\033[A\r", "\004", 0 };
    fork_case("fork_selector", k_fork);
    fork_case("fork_up", k_up);
    /* Enter forks at the message: a new session whose chat ends before it, the message in the editor, pi's status line */
    {
        char path[600];
        strcpy(path, aroot);
        strcat(path, "/fork_session.jsonl");
        app_session = path;
        CHECK_INT(app_run(80, 40, k_pick, 0, 0, take_shot), 0);
        app_session = 0;
        CHECK(shot_has("Forked to new session"));
        CHECK(shot_has("approach A"));                      /* back in the editor */
        CHECK(!shot_has("approach B"));
        CHECK(!shot_has("Fork from Message"));
    }
}

static void test_tree_nav(void)
{
    static const char *keys[] = { "/tree\r", "\033[B", "\r", "\r", "\004", 0 };
    char path[600];
    app_session = 0;
    strcpy(path, aroot);
    strcat(path, "/tree_session.jsonl");
    app_session = path;
    CHECK_INT(app_run(80, 40, keys, 0, 0, take_shot), 0);
    app_session = 0;
    CHECK(shot_has("root answer"));
    CHECK(!shot_has("B answer"));
    CHECK(shot_has("Navigated to selected point"));
    if (getenv("PI_TUI_DUMP")) {
        int r;
        for (r = 0; r < nshot; r++)
            printf("    %2d|%s\n", r + 1, shot[r]);
    }
    {
        int r, in_editor = 0;
        for (r = 16; r < nshot; r++)
            if (!strcmp(shot[r], "approach A"))
                in_editor = 1;
        CHECK(in_editor);               /* the user message is back in the editor, as pi's navigateTree gives it */
    }
}


/* ---- N4: /export through the prompt: the page and the JSONL, pi's message ---- */

static void test_export(void)
{
    char html[600], jl[600], cmd1[700], cmd2[700], cmd3[700];
    static const char *keys[8];
    static const char *const sse[] = { "markdown.1.sse" };
    char *h, *j;
    long n;
    FILE *f;
    strcpy(html, aroot);
    strcat(html, "/out.html");
    strcpy(jl, aroot);
    strcat(jl, "/out.jsonl");
    strcpy(cmd1, "/export ");
    strcat(cmd1, html);
    strcat(cmd1, "\r");
    strcpy(cmd2, "/export ");
    strcat(cmd2, jl);
    strcat(cmd2, "\r");
    strcpy(cmd3, "/export /nonexistent/dir/x.html\r");
    keys[0] = "show markdown\r";
    keys[1] = cmd1;
    keys[2] = cmd2;
    keys[3] = cmd3;
    keys[4] = "\004";
    keys[5] = 0;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 40, keys, sse, 1, take_shot), 0);
    CHECK(shot_has("Session exported to:"));
    CHECK(shot_has("Failed to export session"));
    f = fopen(html, "rb");
    CHECK(f != 0);
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        h = (char *)malloc((size_t)n + 1);
        if (h && fread(h, 1, (size_t)n, f) == (size_t)n) {
            h[n] = 0;
            CHECK(strstr(h, "<!DOCTYPE html>") == h);
            CHECK(strstr(h, "show markdown") != 0);
            CHECK(strstr(h, "Here is **bold**") != 0);
        }
        free(h);
        fclose(f);
    }
    f = fopen(jl, "rb");
    CHECK(f != 0);
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        j = (char *)malloc((size_t)n + 1);
        if (j && fread(j, 1, (size_t)n, f) == (size_t)n) {
            j[n] = 0;
            CHECK(strstr(j, "{\"type\":\"session\",\"version\":3,\"id\":\"") == j);
            CHECK(strstr(j, "\"parentId\":null") != 0);
        }
        free(j);
        fclose(f);
    }
}


/* ---- the footer's context figure is pi's getContextUsage: with no answer's usage (here a failed request) it
 * counts the system message and its tools besides the messages (pi's error capture: 0.7%; the messages alone, as
 * before: 0.0%) ---- */

static void test_context_figure(void)
{
    static const char *keys[] = { "hello\r", "\004", 0 };
    static const char *const ans[] = { "error.1.http" };
    char path[600], want[24], num[16];
    const char *row = 0;
    pi_s3 st;
    pi_ctx c;
    long tok = -2, win = 0, tenths;
    int r;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys, ans, 1, take_shot), 0);
    for (r = 0; r < nshot; r++)
        if (strstr(shot[r], "(auto)"))
            row = shot[r];
    CHECK(row != 0);
    CHECK_INT(app_newest(path, sizeof(path)), 0);
    s3_init(&st, &asys, adv_now, 0, 1, 7);
    if (!s3_open(&st, path)) {
        ctx_init(&c);
        if (!ctx_build(&st, st.leaf, &c))
            tok = cp_context_usage(&st, st.leaf, &c);
        ctx_free(&c);
    }
    s3_free(&st);
    CHECK(tok > 500);           /* the system prompt and the tools' declarations */
    /* the window as the footer names it after the slash ("1.0M", "200k") */
    if (row && strstr(row, "%/")) {
        const char *w = strstr(row, "%/") + 2;
        win = strtol(w, (char **)&w, 10);
        if (*w == '.') {
            long frac = strtol(w + 1, (char **)&w, 10);
            win = win * 10 + frac;
            win = *w == 'M' ? win * 100000 : win * 100;
        } else
            win = *w == 'M' ? win * 1000000 : *w == 'k' ? win * 1000 : win;
    }
    CHECK(win > 0);
    if (win > 0 && tok > 0) {
        tenths = (tok * 1000 + win / 2) / win;
        cl_ltoa(tenths / 10, want);
        cl_cat(want, ".", sizeof(want));
        cl_ltoa(tenths % 10, num);
        cl_cat(want, num, sizeof(want));
        cl_cat(want, "%/", sizeof(want));
        CHECK(tenths > 0);
        CHECK(row && strstr(row, want) != 0);
        if (row && !strstr(row, want))
            printf("  context figure: want %s in [%s]\n", want, row);
    }
}


/* ---- Ctrl+D on an empty editor ends pi at once (custom-editor.js app.exit, interactive-mode.js handleCtrlD:
 * shutdown), also while the agent works ---- */

static void test_ctrl_d(void)
{
    static const char *idle[] = { "\004", "hello\r", 0 };
    static const char *busy[] = { "read the file\r", "!\004", "second\r", 0 };
    static const char *const one[] = { "markdown.1.sse" };
    static const char *const tool[] = { "tool.1.sse", "tool.2.sse", "markdown.1.sse" };
    CHECK_INT(app_run(80, 25, idle, one, 1, 0), 0);
    CHECK_INT(ps.nreq, 0);                  /* gone at the first Ctrl+D: "hello" was never sent */
    CHECK_INT(app_run(80, 25, busy, tool, 3, 0), 0);
    CHECK_INT(ps.nreq, 1);                  /* the run stopped in the tool turn and the program ended */
}


/* ---- N5: ! puts the command's output into the conversation, !! does not ---- */

static void test_bang(void)
{
    /* a chunk's first "!" is the harness's mark and is dropped: the commands start with a second one */
    static const char *keys[] = { "!!echo hi\r", "!!!echo secret\r", "what did it print\r", "\004", 0 };
    static const char *const sse[] = { "markdown.1.sse" };
    char sh[1200];
    char path[600];
    pi_s3 st;
    int i, found_hi = 0, found_secret = 0;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 40, keys, sse, 1, take_shot), 0);
    CHECK(shot_has("$ echo hi"));
    CHECK(shot_has("$ echo secret"));
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq < 1)
        return;
    /* the request holds the first command's output, as pi's convertToLlm words it, and not the second's */
    CHECK(strstr(ps.body[0], "Ran `echo hi`") != 0);
    CHECK(strstr(ps.body[0], "echo secret") == 0 && strstr(ps.body[0], "\"secret\"") == 0);
    /* the session file keeps both, the second marked */
    app_shape(sh, sizeof(sh));
    CHECK_INT(app_newest(path, sizeof(path)), 0);
    s3_init(&st, &asys, adv_now, 0, 1, 7);
    CHECK_INT(s3_open(&st, path), 0);
    for (i = 0; i < st.n; i++) {
        long n;
        const char *t = s3_text(&st, i, &n);
        if (t && strstr(t, "\"role\":\"bashExecution\"") && strstr(t, "echo hi") && !strstr(t, "excludeFromContext"))
            found_hi = 1;
        if (t && strstr(t, "\"role\":\"bashExecution\"") && strstr(t, "echo secret") && strstr(t, "\"excludeFromContext\":true"))
            found_secret = 1;
    }
    s3_free(&st);
    CHECK(found_hi);
    CHECK(found_secret);
}

/* P11-I11: pi 1.1.0 writes `\033]7501;state=...:app=pi\033\\` reports when PI_PROGRAM_STATUS=1 (tests/pi/tui/status_bytes.bytes, the
 * bytes of pi's pty for one turn): idle at the start, working while the turn runs, done when it settles. amiga-pi writes the
 * same with its own app name, and `clear` when the screen stops. Without the variable it writes none, and no query for
 * the terminal to answer either (pi's start_bytes.bytes holds its kitty query, the OSC 7501 query and device attributes). */
static int osc7501(const char *b, long n, char out[][80], int max)
{
    int k = 0;
    long i;
    for (i = 0; i + 6 < n && k < max; i++)
        if (!memcmp(b + i, "\033]7501;", 6)) {
            long e = i;
            while (e + 1 < n && !(b[e] == '\033' && b[e + 1] == '\\'))
                e++;
            if (e - i < 79) {
                memcpy(out[k], b + i, (size_t)(e - i));
                out[k][e - i] = 0;
                k++;
            }
        }
    return k;
}

static void test_program_status(void)
{
    static const char *keys[] = { "hello\r", "\004", 0 };
    static const char *const sse[] = { "turn_raw.1.sse" };
    char pi[8][80], ours[8][80];
    long n = 0;
    char *g = slurp("status_bytes.bytes", &n);
    int np = 0, no, i;
    CHECK(g != 0);
    if (g) {
        np = osc7501(g, n, pi, 8);
        free(g);
    }
    CHECK_INT(np, 3);                                   /* idle, working, done: pi never clears (tmux killed it) */
    CHECK(np == 3 && !strcmp(pi[0], "\033]7501;state=idle:app=pi"));
    g = slurp("start_bytes.bytes", &n);
    CHECK(g != 0);
    if (g) {
        CHECK(strstr(g, "\033[>7u\033[?u\033]7501;?\033\\\033[c") != 0);      /* pi's startup queries */
        CHECK(n > 1560 && strstr(g, "\033[>4;2m") != 0 && strstr(g, "\033]0;") != 0);      /* modifyOtherKeys 2 and the OSC 0 title follow the first frame (COMPATIBILITY) */
        CHECK_INT(osc7501(g, n, ours, 8), 1);           /* only the query: no report without support */
        free(g);
    }
    app_pstatus = "1";
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys, sse, 1, take_shot), 0);
    app_pstatus = 0;
    no = app_out ? osc7501(app_out, (long)strlen(app_out), ours, 8) : 0;
    CHECK_INT(no, 4);                                   /* the same three, then clear when the screen stops */
    for (i = 0; i < 3 && i < no && np == 3; i++) {
        /* pi's app name is pi; ours amiga-pi */
        char want[100];
        const char *eq = strstr(pi[i], ":app=pi");
        long head = eq ? (long)(eq - pi[i]) : 0;
        memcpy(want, pi[i], (size_t)head);
        strcpy(want + head, ":app=amiga-pi");
        CHECK_STR(ours[i], want);
    }
    CHECK(no == 4 && !strcmp(ours[3], "\033]7501;state=clear"));
    CHECK(app_out && strstr(app_out, "\033]7501;?") == 0);      /* no query is written */
    CHECK_INT(app_run(80, 25, keys, sse, 1, take_shot), 0);      /* unset: nothing */
    CHECK_INT(app_out ? osc7501(app_out, (long)strlen(app_out), ours, 8) : -1, 0);
}

/* Q1: pi reads the session's name from the session manager at every report (interactive-mode.ts programStatus), so a
 * name given at the start (--name) is on the working and done reports of the first turn, with no session-info event */
static void test_program_status_name(void)
{
    static const char *keys[] = { "hello\r", "\004", 0 };
    static const char *const sse[] = { "turn_raw.1.sse" };
    char got[8][80];
    jw w, d;
    int n, ok = 0;
    jw_init(&w);
    jw_init(&d);
    pi_pstatus_format(&w, PST_WORKING, "amiga-pi", PSK_NONE, "named at start");
    pi_pstatus_format(&d, PST_DONE, "amiga-pi", PSK_NONE, "named at start");
    app_pstatus = "1";
    app_name_cli = "named at start";
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys, sse, 1, take_shot), 0);
    app_name_cli = 0;
    app_pstatus = 0;
    n = app_out ? osc7501(app_out, (long)strlen(app_out), got, 8) : 0;
    CHECK_INT(n, 4);
    if (n == 4 && w.p && d.p) {
        /* osc7501 cuts the terminator off */
        ok = !strncmp(got[1], w.p, strlen(w.p) - 2) && !strncmp(got[2], d.p, strlen(d.p) - 2);
        CHECK(!strstr(got[0], ":msg="));                /* idle carries no message */
    }
    CHECK(ok);
    jw_free(&w);
    jw_free(&d);
}

/* the reporter's mapping of events to states (program-status-reporter.ts handleEvent) and pi's formatProgramStatus */
static char pst_out[2000];
static void pst_sink(void *u, const char *z, long n)
{
    (void)u;
    if ((size_t)n + strlen(pst_out) < sizeof(pst_out) - 1)
        strncat(pst_out, z, (size_t)n);
}

static void pst_ev(pi_pstatus *p, int type, int role, const char *msg, int aborted, int is_error, int stop)
{
    pi_event e;
    pi_event_init(&e, type);
    e.role = role;
    e.msg = msg;
    e.msg_n = msg ? (long)strlen(msg) : 0;
    e.aborted = aborted;
    e.is_error = is_error;
    e.stop = stop;
    pi_pstatus_event(p, &e);
}

static const char *pst_nm(void *u)
{
    return (const char *)u;
}

static void test_pstatus_states(void)
{
    pi_pstatus p;
    jw o;
    jw_init(&o);
    pi_pstatus_format(&o, PST_WORKING, "pi", PSK_NONE, "my session");
    CHECK_STR(o.p, "\033]7501;state=working:app=pi:msg=bXkgc2Vzc2lvbg==\033\\");
    jw_reset(&o);
    pi_pstatus_format(&o, PST_BLOCKED, "bad app", PSK_QUESTION, "Pick\none");        /* app dropped, control char a space */
    CHECK_STR(o.p, "\033]7501;state=blocked:kind=question:msg=UGljayBvbmU=\033\\");
    jw_reset(&o);
    pi_pstatus_format(&o, PST_DONE, "pi", PSK_QUESTION, 0);                              /* kind only for blocked */
    CHECK_STR(o.p, "\033]7501;state=done:app=pi\033\\");
    jw_free(&o);
    pst_out[0] = 0;
    pi_pstatus_init(&p, 1, pst_sink, 0, "pi", pst_nm, (void *)"s1");
    pi_pstatus_start(&p);
    pst_ev(&p, PE_AGENT_START, 0, 0, 0, 0, 0);
    pst_ev(&p, PE_MESSAGE_END, LR_ASSISTANT, "{\"role\":\"assistant\",\"stopReason\":\"error\",\"errorMessage\":\"boom\\nsecond\"}", 0, 0, 0);
    pst_ev(&p, PE_MESSAGE_END, LR_ASSISTANT, "{\"role\":\"assistant\",\"stopReason\":\"error\",\"errorMessage\":\"boom\\nsecond\"}", 0, 0, 0);
    pst_ev(&p, PE_AGENT_SETTLED, 0, 0, 0, 0, 0);
    /* idle; working with the session name; (the same again is not repeated); error with the first line only */
    CHECK_STR(pst_out, "\033]7501;state=idle:app=pi\033\\\033]7501;state=working:app=pi:msg=czE=\033\\"
                       "\033]7501;state=error:app=pi:msg=Ym9vbQ==\033\\");
    pst_out[0] = 0;
    pst_ev(&p, PE_AGENT_START, 0, 0, 0, 0, 0);
    pst_ev(&p, PE_AGENT_SETTLED, 0, 0, 1, 0, 0);                  /* aborted: idle, whatever the answer was */
    CHECK_STR(pst_out, "\033]7501;state=working:app=pi:msg=czE=\033\\\033]7501;state=idle:app=pi\033\\");
    pst_out[0] = 0;
    pst_ev(&p, PE_COMPACTION_START, 0, 0, 0, 0, 0);               /* "Compacting context" */
    pst_ev(&p, PE_COMPACTION_END, 0, 0, 0, 0, 0);                 /* manual (stop 0) and fine: done */
    CHECK_STR(pst_out, "\033]7501;state=working:app=pi:msg=Q29tcGFjdGluZyBjb250ZXh0\033\\\033]7501;state=done:app=pi:msg=czE=\033\\");
    pi_pstatus_blocked(&p, PSK_AUTH, "Log in");
    pi_pstatus_stop(&p);
    CHECK(strstr(pst_out, "state=blocked:app=pi:kind=auth") != 0);
    CHECK(strstr(pst_out, "state=clear\033\\") != 0);
}

/* P11-I7 reachability: settings.json's defaultTools reaches the request's tool list through pi_app (the unit cases are in
 * test_pi_tools.c) */
static void test_default_tools_setting(void)
{
    static const char *keys[] = { "hello\r", "\004", 0 };
    static const char *const sse[] = { "markdown.1.sse" };
    app_write("settings.json", "{\"quietStartup\":true,\"defaultTools\":[\"-write\",\"+ls\"]}\n");
    CHECK_INT(app_run(80, 40, keys, sse, 1, take_shot), 0);
    CHECK_INT(ps.nreq, 1);
    if (ps.nreq >= 1) {
        CHECK(strstr(ps.body[0], "\"name\":\"ls\"") != 0);
        CHECK(strstr(ps.body[0], "\"name\":\"read\"") != 0);
        CHECK(strstr(ps.body[0], "\"name\":\"write\"") == 0);
    }
    app_write("settings.json", "{\"quietStartup\":true}\n");
}

/* P11-I8 reachability: settings.json's outputPad reaches the screen through pi_app (the golden scenarios set it by hand) */
static int row_starts(const char *text)
{
    int r;
    for (r = 0; r < nshot; r++)
        if (!strncmp(shot[r], text, strlen(text)))
            return 1;
    return 0;
}

static void test_output_pad_setting(void)
{
    static const char *keys[] = { "!!echo hi\r", "\004", 0 };
    app_write("settings.json", "{\"quietStartup\":true,\"outputPad\":0}\n");
    CHECK_INT(app_run(80, 40, keys, 0, 0, take_shot), 0);
    CHECK(row_starts("$ echo hi"));
    app_write("settings.json", "{\"quietStartup\":true,\"outputPad\":1}\n");
    CHECK_INT(app_run(80, 40, keys, 0, 0, take_shot), 0);
    CHECK(row_starts(" $ echo hi"));
    app_write("settings.json", "{\"quietStartup\":true}\n");
}


/* ---- U3: an invalid settings.json is reported (core/settings-diagnostics.ts, settings-manager.ts tryLoadFromStorage). The message
 * is V8's JSON.parse: tests/pi/settings/cases.json holds 900 texts (the cases of tools/pi_oracle/gen_settings_errors.mjs: the sample
 * mutated at every position, and hand-made ones) with what Node 24 says of each ---- */
static void test_settings_errors(void)
{
    long n = 0, bad = 0, total = 0, with_msg = 0;
    char *cases = slurp("../settings/cases.json", &n);
    jv arr, row;
    jit it;
    CHECK(cases != 0);
    if (!cases)
        return;
    CHECK_INT(json_parse(cases, n, &arr), 0);
    json_iter(arr, &it);
    while (json_next(&it, 0, &row)) {
        jit in;
        jv text, want;
        char *t, *w;
        long tn = 0, wn = 0;
        jw msg;
        int got;
        json_iter(row, &in);
        if (!json_next(&in, 0, &text) || !json_next(&in, 0, &want))
            continue;
        t = json_strdup(text, &tn);
        w = json_type(want) == J_STR ? json_strdup(want, &wn) : 0;
        jw_init(&msg);
        got = pi_settings_error(t ? t : "", tn, &msg);
        jw_rawz(&msg, "");
        total++;
        with_msg += w != 0;
        if (got != (w != 0) || (w && strcmp(msg.p ? msg.p : "", w))) {
            if (bad++ < 5)
                printf("  settings text %.60s\n    pi:  %s\n    ours: %s\n", t ? t : "", w ? w : "(loads)", got ? msg.p : "(loads)");
        }
        jw_free(&msg);
        free(t);
        free(w);
    }
    CHECK_INT(bad, 0);
    CHECK(total > 800 && with_msg > 500);
    free(cases);
    {
        jw m, out;
        jw_init(&m);
        jw_init(&out);
        CHECK_INT(pi_settings_error("\"outputPad\": 0\n", 15, &m), 1);
        jw_rawz(&m, "");
        pi_settings_warning("ENVARC:amiga-pi/settings.json", m.p, &out);
        jw_rawz(&out, "");
        CHECK_STR(out.p, "Invalid settings file ENVARC:amiga-pi/settings.json: Unexpected non-whitespace character after JSON at position 11 (line 1 column 12)");
        jw_free(&m);
        jw_free(&out);
    }
}


/* ---- N6: pi's themes and keybindings ---- */

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* "#rrggbb" to channels, 0 when it is not one */
static int hex_rgb(const char *h, int *r, int *g, int *b)
{
    if (h[0] != '#' || strlen(h) < 7)
        return 0;
    *r = hexv(h[1]) * 16 + hexv(h[2]);
    *g = hexv(h[3]) * 16 + hexv(h[4]);
    *b = hexv(h[5]) * 16 + hexv(h[6]);
    return 1;
}

static int near1(int a, int b)
{
    return a - b <= 1 && b - a <= 1;
}

static void test_themes(void)
{
    cl_pitheme th;
    char err[3000], *txt, *line;
    long n;
    int i, r, g, b;
    /* pi's own `system` theme, as pi generates it with no terminal colours: slot and faint per role */
    txt = slurp("system_theme.txt", &n);
    CHECK(txt != 0);
    pitheme_system(&th);
    for (line = txt; txt && *line;) {
        char name[60], slot[8], dim[8], *nl = strchr(line, '\n');
        int role;
        if (nl)
            *nl = 0;
        if (sscanf(line, "%59s %7s %7s", name, slot, dim) == 3) {
            role = pitheme_role(name);
            CHECK(role >= 0);
            if (role >= 0) {
                CHECK(th.dim[role] == (strcmp(dim, "dim") == 0));
                CHECK(th.color[role] == (slot[0] == '-' ? HL_DEFAULT : (long)atoi(slot)));
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    free(txt);
    /* dark.json and light.json through the loader and okcolor against what pi's parseColor makes of them */
    for (i = 0; i < 2; i++) {
        const char *nm = i ? "light" : "dark", *bt = i ? pi_theme_light : pi_theme_dark;
        char fn[40];
        cl_copy(fn, "theme_", sizeof(fn));
        cl_cat(fn, nm, sizeof(fn));
        cl_cat(fn, ".txt", sizeof(fn));
        CHECK_INT(pitheme_load(&th, bt, (long)strlen(bt), pi_okcolor, err, sizeof(err)), 0);
        txt = slurp(fn, &n);
        CHECK(txt != 0);
        for (line = txt; txt && *line;) {
            char name[60], val[20], *nl = strchr(line, '\n');
            int role;
            if (nl)
                *nl = 0;
            if (sscanf(line, "%59s %19s", name, val) == 2 && (role = pitheme_role(name)) >= 0) {
                if (val[0] == '-')
                    CHECK(th.color[role] == HL_DEFAULT);
                else if (hex_rgb(val, &r, &g, &b)) {
                    long c = th.color[role];
                    CHECK(HL_IS_RGB(c));
                    CHECK(near1((int)((c >> 16) & 255), r) && near1((int)((c >> 8) & 255), g) && near1((int)(c & 255), b));
                }
            }
            if (!nl)
                break;
            line = nl + 1;
        }
        free(txt);
    }
    /* the colour spellings */
    txt = slurp("colors.txt", &n);
    CHECK(txt != 0);
    for (line = txt; txt && *line;) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n'), hexs[16];
        int er, eg, eb, gr, gg, gb, rc;
        if (nl)
            *nl = 0;
        if (tab) {
            *tab = 0;
            cl_copy(hexs, tab + 1, sizeof(hexs));
            hex_rgb(hexs, &er, &eg, &eb);
            rc = pi_okcolor(line, &gr, &gg, &gb);
            if (rc == 1)
                CHECK(near1(gr, er) && near1(gg, eg) && near1(gb, eb));
            else
                CHECK(rc == 0 && line[0] == '#');       /* hex is theme.c's own */
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    free(txt);
    CHECK_INT(pi_okcolor("okhsl(10 120% 50%)", &r, &g, &b), -1);
    CHECK_INT(pi_okcolor("red", &r, &g, &b), 0);
    /* a theme that is wrong says what pi says */
    {
        static const char missing[] = "{\"name\":\"x\",\"colors\":{\"accent\":\"#fff\"}}";
        static const char badvar[] = "{\"name\":\"x\",\"vars\":{\"a\":\"b\",\"b\":\"a\"},\"colors\":{\"accent\":\"a\"}}";
        static const char nov[] = "{\"name\":\"x\",\"colors\":{\"accent\":\"nothere\"}}";
        CHECK_INT(pitheme_load(&th, missing, (long)strlen(missing), pi_okcolor, err, sizeof(err)), -1);
        CHECK(strstr(err, "Invalid theme \"x\":\n\nMissing required color tokens:\n  - bashMode\n  - border\n") == err);
        CHECK(strstr(err, "Please add these colors to your theme's \"colors\" object.") != 0);
        CHECK_INT(pitheme_load(&th, badvar, (long)strlen(badvar), pi_okcolor, err, sizeof(err)), -1);
        CHECK(strstr(err, "Circular variable reference detected: ") != 0);
        CHECK_INT(pitheme_load(&th, nov, (long)strlen(nov), pi_okcolor, err, sizeof(err)), -1);
        CHECK(strstr(err, "Variable reference not found: nothere") != 0);
    }
    /* P12-B6: theme-schema.ts of pi 1.2.0 (tests/pi/tui/theme_errors.json is its own run): unknown keys in the root, colors and
     * export objects, appearance and the name are refused with its messages; $schema and appearance are accepted */
    {
        jv all, c, th_v, er_v;
        jit it;
        jv key;
        char *gj = slurp("theme_errors.json", &n), want[3000], name[40];
        CHECK(gj != 0);
        if (gj && !json_parse(gj, n, &all)) {
            json_iter(all, &it);
            while (json_next(&it, &key, &c)) {
                int rc;
                json_str(key, name, sizeof(name));
                CHECK(json_get(c, "theme", &th_v) && json_get(c, "error", &er_v));
                json_str(er_v, want, sizeof(want));
                err[0] = 0;
                rc = pitheme_load(&th, th_v.p, th_v.n, pi_okcolor, err, sizeof(err));
                if (want[0]) {
                    CHECK_INT(rc, -1);
                    if (strcmp(err, want))
                        printf("  theme %s:\n  want %s\n  got  %s\n", name, want, err);
                    CHECK_STR(err, want);
                } else
                    CHECK_INT(rc, 0);
            }
        } else
            CHECK(0);
        free(gj);
    }
}

static int rule_fg;
static void themed_shot(void)
{
    vt_color fg, bg;
    const vt_cell *c = h_cell(cs.vt, 0, cs.rows - 5);
    vt_resolve_colors(cs.vt, c, &fg, &bg);
    rule_fg = (int)fg;
    take_shot();
}

static void test_theme_keys_app(void)
{
    static const char *keys1[] = { "\020", "\004", 0 };
    static const char *keys2[] = { "\024", "\033[Z", "\004", 0 };
    static const char *keys3[] = { "\024", "\004", 0 };
    int system_fg;
    char path[600];
    /* the theme a file names colours the screen: the rules of the editor (thinkingOff) in dark.json's grey */
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys1, 0, 0, themed_shot), 0);
    system_fg = rule_fg;
    app_write("settings.json", "{\"quietStartup\":true,\"theme\":\"dark\"}\n");
    CHECK_INT(app_run(80, 25, keys1, 0, 0, themed_shot), 0);
    CHECK(rule_fg != system_fg);
    /* UT7: the depth is pi's, not a fixed 16. Without COLORTERM the dark theme's grey rule is an index of the
     * 256 palette (it was mapped down to ANSI 8, bright black); COLORTERM=truecolor makes it a direct colour. */
    CHECK(rule_fg >= 16 && rule_fg < 256);
    app_colorterm = "truecolor";
    CHECK_INT(app_run(80, 25, keys1, 0, 0, themed_shot), 0);
    CHECK(((unsigned long)rule_fg & VT_COLOR_RGB) != 0);
    app_colorterm = 0;
    /* a theme file of the user's in the data directory */
    strcpy(path, adata);
    strcat(path, "/themes");
    mkdir(path, 0777);
    {
        char *txt;
        long n;
        FILE *f;
        strcat(path, "/mine.json");
        txt = slurp("dark.json", &n);
        CHECK(txt != 0);
        f = fopen(path, "wb");
        if (f && txt) {
            fputs(txt, f);
            fclose(f);
        }
        free(txt);
    }
    app_write("settings.json", "{\"quietStartup\":true,\"theme\":\"mine\"}\n");
    CHECK_INT(app_run(80, 25, keys1, 0, 0, themed_shot), 0);
    CHECK(!shot_has("Theme not found"));
    app_write("settings.json", "{\"quietStartup\":true,\"theme\":\"nosuch\"}\n");
    CHECK_INT(app_run(80, 25, keys1, 0, 0, themed_shot), 0);
    /* keybindings.json in pi's shape: Ctrl+T cycles the thinking level, Shift+Tab no longer does */
    app_write("settings.json", "{\"quietStartup\":true}\n");
    app_write("keybindings.json", "{\"app.thinking.cycle\":\"ctrl+t\",\"nonsense.action\":\"ctrl+y\"}\n");
    CHECK_INT(app_run(80, 25, keys2, 0, 0, take_shot), 0);
    CHECK(shot_has("claude-opus-5-5 \342\200\242 high"));
    CHECK(shot_has("unknown action"));
    /* P12-B5 (pi 1.2.0 keybindings.ts toKeybindingsConfig): "$schema" is skipped without a warning, and a value that is
     * not a key id is dropped (the action keeps its default) */
    app_write("keybindings.json", "{\"$schema\":\"https://pi.dev/schemas/keybindings.schema.json\",\"app.thinking.cycle\":\"ctrl+t\"}\n");
    CHECK_INT(app_run(80, 25, keys2, 0, 0, take_shot), 0);
    CHECK(shot_has("claude-opus-5-5 \342\200\242 high"));
    CHECK(!shot_has("unknown action"));
    app_write("keybindings.json", "{\"app.thinking.cycle\":\"ctrl+ctrl+t\"}\n");
    CHECK_INT(app_run(80, 25, keys3, 0, 0, take_shot), 0);
    CHECK(!shot_has("claude-opus-5-5 \342\200\242 high"));                /* Ctrl+T cycles nothing: the default key stays */
    CHECK(!shot_has("unknown action"));
    app_write("keybindings.json", "{}\n");
    unlink(path);
}


/* TM1 through pi_app: settings.json tuiMode / fullscreenExitOutput and --tui-mode choose the screen. Fullscreen is the
 * default (the alternate screen is entered and left, the conversation and the resume line written on the main screen);
 * "regular" never enters it; "resume-hint" writes the line alone; the command line wins over the file */
static void test_tui_mode_app(void)
{
    static const char *keys[] = { "hello\r", "\004", 0 };
    static const char *sse[] = { "twice.1.sse" };
    const char *after;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys, sse, 1, 0), 0);
    CHECK(app_out && strstr(app_out, "\033[?1049h") != 0);
    after = app_out ? strstr(app_out, "\033[?1049l") : 0;
    CHECK(after != 0);
    CHECK(after && strstr(after, "answer one") != 0);                       /* the conversation on the main screen */
    CHECK(after && strstr(after, "To resume this session:") != 0);
    CHECK(after && strstr(after, "amiga-pi --session ") != 0);
    app_write("settings.json", "{\"quietStartup\":true,\"fullscreenExitOutput\":\"resume-hint\"}\n");
    CHECK_INT(app_run(80, 25, keys, sse, 1, 0), 0);
    after = app_out ? strstr(app_out, "\033[?1049l") : 0;
    CHECK(after != 0);
    CHECK(after && strstr(after, "answer one") == 0);                        /* the line alone */
    CHECK(after && strstr(after, "To resume this session:") != 0);
    app_write("settings.json", "{\"quietStartup\":true,\"tuiMode\":\"regular\"}\n");
    CHECK_INT(app_run(80, 25, keys, sse, 1, 0), 0);
    CHECK(app_out && strstr(app_out, "\033[?1049h") == 0);
    CHECK(app_out && strstr(app_out, "To resume this session:") != 0);
    app_tui_cli = "fullscreen";
    CHECK_INT(app_run(80, 25, keys, sse, 1, 0), 0);
    CHECK(app_out && strstr(app_out, "\033[?1049h") != 0);
    app_tui_cli = 0;
    app_write("settings.json", "{\"quietStartup\":true}\n");
    free(app_out);
    app_out = 0;
}

/* Q11: with --session-dir the resume line carries the directory, quoted when it needs it, before --session (pi's
 * formatResumeCommand; tests/pi/tui/exit_sdir.txt is pi 1.1.0's line for "<work>/my sessions"); the default directory is
 * left out, and the session is stored in that directory */
static void test_resume_session_dir(void)
{
    static const char *keys[] = { "hello\r", "\004", 0 };
    static const char *sse[] = { "twice.1.sse" };
    char dir[500], want[600], line[200];
    long n = 0;
    char *g = slurp("exit_sdir.txt", &n);
    CHECK(g != 0);
    if (g) {
        char *l = strstr(g, "To resume this session:");
        CHECK(l && strstr(l, "pi --session-dir '") != 0);          /* pi: the quoted path first, then --session */
        CHECK(l && strstr(l, "my sessions'") != 0);
        free(g);
    }
    cl_copy(dir, aroot, sizeof(dir));
    cl_cat(dir, "/my sessions", sizeof(dir));
    cl_copy(line, "To resume this session:", sizeof(line));
    cl_copy(want, "amiga-pi --session-dir '", sizeof(want));
    cl_cat(want, dir, sizeof(want));
    cl_cat(want, "' --session ", sizeof(want));
    app_write("settings.json", "{\"quietStartup\":true}\n");
    app_sdir_cli = dir;
    CHECK_INT(app_run(80, 25, keys, sse, 1, 0), 0);
    app_sdir_cli = 0;
    CHECK(app_out && strstr(app_out, line) != 0);
    CHECK(app_out && strstr(app_out, want) != 0);
    CHECK_INT(app_run(80, 25, keys, sse, 1, 0), 0);                /* no option: the line as before */
    CHECK(app_out && strstr(app_out, "amiga-pi --session ") != 0 && strstr(app_out, "--session-dir") == 0);
}

/* ---- L5: ctx.ui.select / confirm / input / notify from a Lua extension, as pi draws them (the captures are
 * tests/pi/tui/ext_ui_*.golden; tui_gen.py's EXT_UI is the TypeScript twin of this extension) ---- */

static const char *const EXT_UI_LUA[] = {   /* pieces: a string literal is limited to 509 characters in C90 */
    "pi.registerCommand('pick', { handler = function(args, ctx)\n",
    "  local v = ctx.ui.select('Pick one', { 'Alpha', 'Beta', 'Gamma' }, args ~= '' and { timeout = 60000 } or nil)\n",
    "  if v == nil then ctx.ui.notify('nothing picked', 'warning') else ctx.ui.notify('picked ' .. v) end\n",
    "end })\n",
    "pi.registerCommand('sure', { handler = function(args, ctx)\n",
    "  if ctx.ui.confirm('Delete it?', 'This cannot be undone.') then ctx.ui.notify('confirmed', 'info')\n",
    "  else ctx.ui.notify('declined', 'warning') end\n",
    "end })\n",
    "pi.registerCommand('ask', { handler = function(args, ctx)\n",
    "  local v = ctx.ui.input('Your name', 'a name')\n",
    "  if v == nil then ctx.ui.notify('no name', 'error') else ctx.ui.notify('hello ' .. v, 'info') end\n",
    "end })\n",
    "pi.registerCommand('tell', { handler = function(args, ctx)\n",
    "  ctx.ui.notify('plain note')\n",
    "  ctx.ui.notify('a warning', 'warning')\n",
    "  ctx.ui.notify('an error', 'error')\n",
    "end })\n",
    0 };

static const char *const *ext_case_lua = EXT_UI_LUA;       /* the extension the next ext_ui_case writes */

static void ext_ui_rest(void)
{
    compare();
}

/* one capture: the keys typed (a command, then the dialog's), the screen at rest compared with pi's. show: the text on the
 * screen that says the dialog or the note is up; mr: golden rows (first, last) left out, ended by 0; shift: the capture's row r
 * is ours at r + shift (0 for a note too: its Spacer's blank row is above it, in an empty chat as well, as pi) */
static void ext_ui_case(const char *name, const char **keys, const char *show, const int *mr, int shift)
{
    static scen sc;
    char dir[600];
    int i, k = 0;
    memset(&sc, 0, sizeof(sc));
    sc.name = name;
    strcpy(dir, aagent);
    strcat(dir, "/extensions");
    mkdir(dir, 0777);
    {
        char lua[2000];
        lua[0] = 0;
        for (i = 0; ext_case_lua[i]; i++)
            strcat(lua, ext_case_lua[i]);
        app_write("extensions/t.lua", lua);
    }
    app_write("settings.json", "{\"quietStartup\":true}\n");
    cur = &sc;
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    load_golden(name);
    CHECK(want != 0);
    if (!want)
        return;
    for (i = 0; mr[i] && k < 6; i += 2, k++) {
        sc.masks[k].r0 = mr[i];
        sc.masks[k].r1 = mr[i + 1];
        sc.masks[k].c0 = 0;
        sc.masks[k].c1 = 79;
        sc.masks[k].why = "the editor border and the footer carry the capture's fake model and its working directory";
    }
    rest_any = 1;
    rest_find = show;
    cmp_shift = shift;
    CHECK_INT(app_run(80, want_rows, keys, 0, 0, ext_ui_rest), 0);
    cmp_shift = 0;
    rest_any = 0;
    rest_find = "navigate";
    CHECK(taken);
    if (diffs) {
        printf("  %s: %d cells differ from pi's capture\n%s", name, diffs, first_diff);
        if (getenv("PI_TUI_DUMP"))
            for (i = 0; i < want_rows; i++)
                printf("    %2d|%s\n", i + 1, snap[i]);
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    vt_free(want);
    {
        char f[600];
        strcpy(f, aagent);
        strcat(f, "/extensions/t.lua");
        unlink(f);
        rmdir(dir);
    }
}

/* ---- L11: pi.sendMessage with display true, the default box and an extension's message renderer (the captures are
 * tests/pi/tui/ext_msg_*.golden; tui_gen.py's EXT_MSG is the TypeScript twin of this extension) ---- */

static const char *const EXT_MSG_LUA[] = {
    "pi.registerMessageRenderer('note', function(message, options, theme)\n",
    "  local body = type(message.content) == 'string' and message.content or ''\n",
    "  return ' ' .. theme.fg('accent', 'Note: ' .. body) .. (options.expanded and ' (expanded)' or '')\n",
    "end)\n",
    "pi.registerCommand('note', { handler = function() pi.sendMessage({ customType = 'note', content = 'hello there', display = true }) end })\n",
    "pi.registerCommand('plain', { handler = function() pi.sendMessage({ customType = 'plain', content = 'some **bold** text', display = true }) end })\n",
    "pi.registerCommand('quiet', { handler = function() pi.sendMessage({ customType = 'quiet', content = 'hidden', display = false }) end })\n",
    "pi.registerCommand('multi', { handler = function()\n",
    "  pi.sendMessage({ customType = 'multi', content = 'one', display = true })\n",
    "  pi.sendMessage({ customType = 'note', content = 'two', display = true })\n",
    "end })\n",
    0 };

static void test_ext_msg_screen(void)
{
    static const char *k_note[] = { "/note\r", "\004", 0 };
    static const char *k_plain[] = { "/plain\r", "\004", 0 };
    static const char *k_hidden[] = { "/quiet\r", "/note\r", "\004", 0 };
    static const char *k_expanded[] = { "/note\r", "\017", "\004", 0 };
    static const char *k_multi[] = { "/multi\r", "\004", 0 };
    static const int note[] = { 25, 30, 0 };      /* the editor and the footer: other rows, the capture's fake model and directory */
    ext_case_lua = EXT_MSG_LUA;
    ext_ui_case("ext_msg_render", k_note, "Note:", note, 0);
    ext_ui_case("ext_msg_plain", k_plain, "bold", note, 0);
    ext_ui_case("ext_msg_hidden", k_hidden, "Note:", note, 0);
    ext_ui_case("ext_msg_expanded", k_expanded, "expanded", note, 0);
    ext_ui_case("ext_msg_multi", k_multi, "two", note, 0);
    ext_case_lua = EXT_UI_LUA;
}

/* ---- L16: ctx.ui.setStatus (the footer's extra row) and setWidget (lines above and below the editor), as pi draws them (the
 * captures are tests/pi/tui/ext_ui_status.golden, ext_ui_widget_*.golden; tui_gen.py's EXT_UIX is the TypeScript twin) ---- */

static const char *const EXT_UIX_LUA[] = {
    "pi.registerCommand('stat', { handler = function(args, ctx) ctx.ui.setStatus('b', 'second'); ctx.ui.setStatus('a', 'first') end })\n",
    "pi.registerCommand('above', { handler = function(args, ctx) ctx.ui.setWidget('w', { 'widget line one', 'widget line two' }) end })\n",
    "pi.registerCommand('below', { handler = function(args, ctx)\n",
    "  ctx.ui.setWidget('wb', { 'under the editor' }, { placement = 'belowEditor' })\n",
    "end })\n",
    "pi.registerCommand('many', { handler = function(args, ctx)\n",
    "  local t = {}\n",
    "  for i = 1, 12 do t[i] = 'row ' .. i end\n",
    "  ctx.ui.setWidget('m', t)\n",
    "end })\n",
    "pi.registerCommand('clear', { handler = function(args, ctx)\n",
    "  ctx.ui.setStatus('a', nil); ctx.ui.setStatus('b', nil); ctx.ui.setWidget('w', nil); ctx.ui.setWidget('wb', nil)\n",
    "end })\n",
    0 };

static void test_ext_uix_screen(void)
{
    static const char *k_stat[] = { "/stat\r", "\004", 0 };
    static const char *k_above[] = { "/above\r", "\004", 0 };
    static const char *k_below[] = { "/below\r", "\004", 0 };
    static const char *k_many[] = { "/many\r", "\004", 0 };
    static const char *k_clear[] = { "/above\r", "/stat\r", "/clear\r", "\004", 0 };
    /* the editor's rules carry the capture's border colour for the thinking level, the footer its fake model and directory; the rows
     * between are the widgets and the status, compared */
    static const int m_stat[] = { 25, 27, 28, 29, 0 };
    static const int m_above[] = { 26, 28, 29, 30, 0 };
    static const int m_below[] = { 25, 27, 29, 30, 0 };
    static const int m_many[] = { 26, 28, 29, 30, 0 };
    static const int m_clear[] = { 26, 28, 29, 30, 0 };
    ext_case_lua = EXT_UIX_LUA;
    ext_ui_case("ext_ui_status", k_stat, "first", m_stat, 0);
    ext_ui_case("ext_ui_widget_above", k_above, "widget line", m_above, 0);
    ext_ui_case("ext_ui_widget_below", k_below, "under the", m_below, 0);
    ext_ui_case("ext_ui_widget_many", k_many, "truncated", m_many, 0);
    ext_ui_case("ext_ui_widget_clear", k_clear, "(auto)", m_clear, 0);
    ext_case_lua = EXT_UI_LUA;
}

static void test_ext_ui_screen(void)
{
    static const char *k_pick[] = { "/pick\r", "\004", 0 };
    static const char *k_pick_down[] = { "/pick\r", "\033[B", "\033[B", "\004", 0 };
    static const char *k_pick_done[] = { "/pick\r", "\033[B", "\r", "\004", 0 };
    static const char *k_pick_esc[] = { "/pick\r", "\033", "\004", 0 };
    static const char *k_pick_timeout[] = { "/pick x\r", "\004", 0 };
    static const char *k_sure[] = { "/sure\r", "\004", 0 };
    static const char *k_sure_yes[] = { "/sure\r", "\r", "\004", 0 };
    static const char *k_sure_no[] = { "/sure\r", "\033[B", "\r", "\004", 0 };
    static const char *k_ask[] = { "/ask\r", "Ada", "\004", 0 };
    static const char *k_ask_done[] = { "/ask\r", "Ada", "\r", "\004", 0 };
    static const char *k_ask_esc[] = { "/ask\r", "Ada", "\033", "\004", 0 };
    static const char *k_tell[] = { "/tell\r", "\004", 0 };
    static const int foot[] = { 29, 30, 0 };      /* the footer: the capture's fake model, directory and window */
    static const int note[] = { 25, 30, 0 };      /* a note: the editor, whose border and prompt sit on other rows, and the footer */
    ext_ui_case("ext_ui_select", k_pick, "navigate", foot, 0);
    ext_ui_case("ext_ui_select_down", k_pick_down, "navigate", foot, 0);
    ext_ui_case("ext_ui_select_done", k_pick_done, "picked Beta", note, 0);
    ext_ui_case("ext_ui_select_cancel", k_pick_esc, "nothing picked", note, 0);
    ext_ui_case("ext_ui_select_timeout", k_pick_timeout, "navigate", foot, 0);
    ext_ui_case("ext_ui_confirm", k_sure, "navigate", foot, 0);
    ext_ui_case("ext_ui_confirm_yes", k_sure_yes, "confirmed", note, 0);
    ext_ui_case("ext_ui_confirm_no", k_sure_no, "declined", note, 0);
    ext_ui_case("ext_ui_input", k_ask, "enter submit", foot, 0);
    ext_ui_case("ext_ui_input_done", k_ask_done, "hello Ada", note, 0);
    ext_ui_case("ext_ui_input_cancel", k_ask_esc, "no name", note, 0);
    ext_ui_case("ext_ui_notify", k_tell, "an error", note, 0);
}

/* ---- the trust question as pi's selector (the capture is tests/pi/tui/approval.golden) ---- */

static void approval_rest(void)
{
    compare();
    take_shot();
}

static void test_approval(void)
{
    static const char *keys[] = { "\004", 0 };
    static scen sc = { "approval", { 0 }, { 0 }, 0, 0, 0,
                       { { 1, 8, 0, 79, "the folder and the program's own name (the sandbox's path wraps)" },
                         { 10, 10, 0, 79, "the parent folder is the sandbox's" },
                         { 18, 25, 0, 79, "below the capture's selector" } } };
    char proj[700];
    int i;
    strcpy(proj, aroot);
    strcat(proj, "/.pi");
    mkdir(proj, 0777);
    strcat(proj, "/SYSTEM.md");
    {
        FILE *f = fopen(proj, "wb");
        if (f) {
            fputs("Be brief.\n", f);
            fclose(f);
        }
    }
    app_write("settings.json", "{\"quietStartup\":true}\n");
    cur = &sc;
    taken = 0;
    diffs = 0;
    memset(rowdiff, 0, sizeof(rowdiff));
    first_diff[0] = 0;
    load_golden("approval");
    CHECK(want != 0);
    if (!want)
        return;
    rest_any = 1;
    cmp_shift = 8;
    CHECK_INT(app_run(80, want_rows, keys, 0, 0, approval_rest), 0);
    rest_any = 0;
    cmp_shift = 0;
    CHECK(taken);
    CHECK(shot_has("Trust project folder?") && shot_has("This allows amiga-pi to load .pi settings") &&
          shot_has("project packages, and execute project extensions."));
    if (diffs) {
        printf("  approval: %d cells differ from pi's capture\n%s", diffs, first_diff);
        if (getenv("PI_TUI_DUMP"))
            for (i = 0; i < want_rows; i++)
                printf("    %2d|%s\n", i + 1, snap[i]);
        if (getenv("PI_TUI_ROWS"))
            for (i = 0; i < want_rows; i++)
                if (rowdiff[i]) {
                    printf("    row %2d pi       |%s\n", i + 1, h_row(want, i));
                    printf("           amiga-pi |%s   (%d cells)\n", snap[i + 8 < 64 ? i + 8 : i], rowdiff[i]);
                }
    }
    CHECK_INT(diffs, 0);
    vt_free(want);
    unlink(proj);
    proj[strlen(proj) - 10] = 0;
    rmdir(proj);
}


/* ---- U2: `-r` as pi's session selector (the captures are tests/pi/tui/resume_*.golden, tools/pi_oracle/tui_gen.py
 * PITUI_TAG=u2: pi 1.1.0 over the same sessions) ---- */

typedef struct rseed {
    const char *dir;            /* 0 this working directory, else the name of another one */
    const char *stem, *first, *ts, *name, *parent;
} rseed;

static const rseed RS_BASE[] = {
    { 0, "20200301T100000_aaaaaaaa", "fix the build on the old branch", "2020-03-01T10:00:00.000Z", 0, 0 },
    { 0, "20220815T093000_bbbbbbbb", "add a --verbose flag to the tool", "2022-08-15T09:30:00.000Z", "verbose flag work", 0 },
    { 0, "20240615T180000_cccccccc", "why does the second test hang", "2024-06-15T18:00:00.000Z", 0, 0 },
    { 0, 0, 0, 0, 0, 0 } };
static const rseed RS_OTHER[] = {
    { "other", "20230101T120000_dddddddd", "a session of another folder", "2023-01-01T12:00:00.000Z", 0, 0 }, { 0, 0, 0, 0, 0, 0 } };
static const rseed RS_FORK[] = {
    { 0, "20240616T090000_eeeeeeee", "a fork of the hang", "2024-06-16T09:00:00.000Z", 0, "20240615T180000_cccccccc" },
    { 0, "20240617T090000_ffffffff", "a second fork", "2024-06-17T09:00:00.000Z", 0, "20240615T180000_cccccccc" },
    { 0, 0, 0, 0, 0, 0 } };

/* one session file as pi writes it: the header, the user's message, the answer, the name when there is one */
static void rs_write(const rseed *r)
{
    char dir[700], path[800], parent[800];
    FILE *f;
    llm_ts t;
    long ms_hi;
    cl_copy(dir, adata, sizeof(dir));
    cl_cat(dir, "/sessions/", sizeof(dir));
    if (r->dir) {
        cl_cat(dir, "--", sizeof(dir));
        cl_cat(dir, r->dir, sizeof(dir));
        cl_cat(dir, "--", sizeof(dir));
    } else {
        char own[700];
        s3_dir(adata, aroot, own, sizeof(own));
        cl_copy(dir, own, sizeof(dir));
    }
    {
        char cmd[800];
        cl_copy(cmd, "mkdir -p '", sizeof(cmd));
        cl_cat(cmd, dir, sizeof(cmd));
        cl_cat(cmd, "'", sizeof(cmd));
        CHECK_INT(system(cmd), 0);
    }
    cl_copy(path, dir, sizeof(path));
    cl_cat(path, "/", sizeof(path));
    cl_cat(path, r->stem, sizeof(path));
    cl_cat(path, ".jsonl", sizeof(path));
    cl_copy(parent, dir, sizeof(parent));
    cl_cat(parent, "/", sizeof(parent));
    cl_cat(parent, r->parent ? r->parent : "", sizeof(parent));
    cl_cat(parent, ".jsonl", sizeof(parent));
    CHECK_INT(s3_iso_read(r->ts, &t), 0);
    ms_hi = t.sec;
    f = fopen(path, "wb");
    CHECK(f != 0);
    if (!f)
        return;
    fprintf(f, "{\"type\":\"session\",\"version\":3,\"id\":\"0199aa00-0000-7000-8000-%s0000\",\"timestamp\":\"%s\",\"cwd\":\"%s%s%s\"%s%s%s}\n",
            r->stem + strlen(r->stem) - 8, r->ts, aroot, r->dir ? "/../" : "", r->dir ? r->dir : "",
            r->parent ? ",\"parentSession\":\"" : "", r->parent ? parent : "", r->parent ? "\"" : "");
    fprintf(f, "{\"type\":\"message\",\"id\":\"u0000001\",\"parentId\":null,\"timestamp\":\"%s\",\"message\":{\"role\":\"user\",\"content\":\"%s\",\"timestamp\":%ld000}}\n",
            r->ts, r->first, ms_hi);
    fprintf(f, "{\"type\":\"message\",\"id\":\"a0000001\",\"parentId\":\"u0000001\",\"timestamp\":\"%s\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],\"timestamp\":%ld000}}\n",
            r->ts, ms_hi + 1);
    if (r->name)
        fprintf(f, "{\"type\":\"session_info\",\"id\":\"n0000001\",\"parentId\":\"a0000001\",\"timestamp\":\"%s\",\"name\":\"%s\"}\n", r->ts, r->name);
    fclose(f);
}

static void rs_seed(const rseed *a, const rseed *b, const rseed *c)
{
    const rseed *l[3], *r;
    int i;
    char cmd[800];
    cl_copy(cmd, "rm -rf '", sizeof(cmd));
    cl_cat(cmd, adata, sizeof(cmd));
    cl_cat(cmd, "/sessions'", sizeof(cmd));
    CHECK_INT(system(cmd), 0);
    l[0] = a;
    l[1] = b;
    l[2] = c;
    for (i = 0; i < 3; i++)
        for (r = l[i]; r && r->stem; r++)
            rs_write(r);
}

typedef struct rcase {
    const char *name;
    const char *keys[4];
    const rseed *seeds[3];
    mask masks[4];
} rcase;

static void sel_rest(void)
{
    compare();
}

static void test_resume_selector(void)
{
    static const rcase cases[] = {
        { "resume_list", { "\005", 0 }, { RS_BASE, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_search", { "second", "\005", 0 }, { RS_BASE, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_down", { "\033[B\033[B", "\005", 0 }, { RS_BASE, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_confirm", { "\004", "\005", 0 }, { RS_BASE, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_named", { "\016", "\005", 0 }, { RS_BASE, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_sort", { "\023", "\005", 0 }, { RS_BASE, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_fork", { "\005", 0 }, { RS_BASE, RS_FORK, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_none", { "\005", 0 }, { 0, 0, 0 }, { { 0, 0, 0, 0, 0 } } },
        { "resume_all", { "\t", "\005", 0 }, { RS_BASE, RS_OTHER, 0 },
          { { 10, 13, 2, 79, "the rows: each shows its folder, which pi shortens to ~ (the sandbox has no home) and so truncates the prompt elsewhere" } } },
        { "resume_path", { "\020", "\005", 0 }, { RS_BASE, 0, 0 },
          { { 10, 12, 2, 79, "the file's path in front of the count: the sandbox's paths are not pi's" } } },
    };
    unsigned i;
    int k;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const rcase *rc = &cases[i];
        static scen sc;
        char *msg;
        memset(&sc, 0, sizeof(sc));
        sc.name = rc->name;
        for (k = 0; k < 4 && rc->masks[k].why; k++)
            sc.masks[k] = rc->masks[k];
        cur = &sc;
        rs_seed(rc->seeds[0], rc->seeds[1], rc->seeds[2]);
        app_write("settings.json", "{\"quietStartup\":true}\n");
        taken = 0;
        diffs = 0;
        memset(rowdiff, 0, sizeof(rowdiff));
        first_diff[0] = 0;
        load_golden(rc->name);
        CHECK(want != 0);
        if (!want)
            continue;
        rest_any = 1;
        rest_find = "Resume Session";
        rest_key = "\005";
        aclock = 1791892800L - 1760100000L;         /* 2026-10-13 12:00 UTC: the ages are the capture's */
        app_resume_cli = 1;
        CHECK_INT(app_run(80, want_rows, (const char **)rc->keys, 0, 0, sel_rest), 0);
        app_resume_cli = 0;
        rest_any = 0;
        rest_find = "navigate";
        rest_key = "\004";
        aclock = 0;
        CHECK(taken);
        CHECK(app_out && strstr(app_out, "No session selected") != 0);
        CHECK_INT(ps.nreq, 0);
        msg = first_diff;
        if (diffs) {
            printf("  %s: %d cells differ from pi's capture\n%s", rc->name, diffs, msg);
            if (getenv("PI_TUI_DUMP"))
                for (k = 0; k < want_rows; k++)
                    printf("    %2d|%s\n", k + 1, snap[k]);
        }
        CHECK_INT(diffs, 0);
        vt_free(want);
    }
    /* Enter on the second row opens that session (its first prompt is drawn again) and keeps the selector's rows above */
    {
        static const char *keys[] = { "\033[B", "\r", "\004", 0 };
        rs_seed(RS_BASE, 0, 0);
        app_write("settings.json", "{\"quietStartup\":true}\n");
        app_resume_cli = 1;
        CHECK_INT(app_run(80, 25, keys, 0, 0, 0), 0);
        app_resume_cli = 0;
        if (getenv("PI_TUI_DUMP") && app_out)
            printf("OUT[%s]\n", app_out);
        CHECK(app_out && strstr(app_out, "add a --verbose flag to the tool") != 0 && strstr(app_out, "No session selected") == 0);
    }
    rs_seed(0, 0, 0);
}

/* ---- the cost of a key: pi's footer redraws the same diffed rows C:Claude's does ---- */

static long key_mark[16];
static int nmark;
static void mark_hook(void)
{
    if (nmark < 16)
        key_mark[nmark++] = cs.sent.n;
}

/* bytes sent per typed character with the given look, over a screen that is up (the first mark is the screen drawn) */
static long bytes_per_key(int look)
{
    static const char *script[] = { "a", "b", "c", "d", "e", "f", "\r", 0 };
    cl_io io;
    cl_tui t;
    cl_pitheme th;
    char buf[64];
    long total, rows0;
    cs_open(80, 25, script);
    cs_io(&io);
    cs.before_read = mark_hook;
    nmark = 0;
    CHECK_INT(tui_init(&t, &io), 0);
    if (look == TUI_LOOK_PI) {
        pitheme_system(&th);
        t.look = TUI_LOOK_PI;
        t.pith = &th;
        strcpy(t.pi_cwd, "~/project");
        strcpy(t.pi_ctx, "0.0%/200k (auto)");
        strcpy(t.pi_right, "fake-model");
    }
    t.model = "fake-model";
    CHECK_INT(tui_start(&t), 0);
    tui_frame(&t);
    rows0 = t.n_rows;
    CHECK(tui_read(&t, buf, sizeof(buf)) == 6);
    /* marks: one before each read; the bytes between the marks of the 2nd and the 7th read are six keys */
    total = nmark >= 8 ? key_mark[7] - key_mark[1] : -1;
    CHECK((t.n_rows - rows0) <= 8);            /* a key redraws a row (the box's), never the whole footer */
    CHECK_INT(t.n_full, t.n_full);
    tui_stop(&t);
    tui_free(&t);
    cs_close();
    return total;
}

/* R5: a bash result of 300 lines: collapsed, the last five rows and "(295 earlier lines, ctrl+o to expand)" (renderers/bash.ts:60-77; a
 * table of 255 rows once cut the output off there); Ctrl+O draws every line (the transcript holds them all) and no hint */
static int screen_has(const char *what)
{
    int r;
    for (r = 0; r < cs.rows; r++)
        if (strstr(cs_row(r), what))
            return 1;
    return 0;
}

/* U1b: bash's details (renderers/bash.ts:53-60, 85-100): the tool's own "[... Full output: <path>]" footer is left out of the output and one
 * warning row says where the output is and how much of it is shown. pi's own run puts a random temp path there, so no capture holds it: the
 * rows are those of the renderer's code. */
static void test_bash_details(void)
{
    static const char *script[] = { 0 };
    static const char det_lines[] =
        "{\"truncation\":{\"content\":\"x\",\"truncated\":true,\"truncatedBy\":\"lines\",\"totalLines\":3000,\"totalBytes\":13893,"
        "\"outputLines\":2000,\"outputBytes\":9999,\"lastLinePartial\":false,\"firstLineExceedsLimit\":false,\"maxLines\":2000,\"maxBytes\":51200},"
        "\"fullOutputPath\":\"/tmp/pi-bash-1.log\"}";
    static const char det_bytes[] =
        "{\"truncation\":{\"content\":\"x\",\"truncated\":true,\"truncatedBy\":\"bytes\",\"totalLines\":3000,\"totalBytes\":13893,"
        "\"outputLines\":243,\"outputBytes\":9999,\"lastLinePartial\":false,\"firstLineExceedsLimit\":false,\"maxLines\":2000,\"maxBytes\":51200}}";
    static const char res_lines[] =
        "[{\"type\":\"text\",\"text\":\"L2999\\nL3000\\n\\n[Showing lines 1001-3000 of 3000. Full output: /tmp/pi-bash-1.log]\"}]";
    static const char res_plain[] = "[{\"type\":\"text\",\"text\":\"L3000\\n\"}]";
    cl_io io;
    cl_tui t;
    cl_pitheme th;
    pi_show ps;
    cs_open(80, 25, script);
    cs_io(&io);
    pitheme_system(&th);
    CHECK_INT(tui_init(&t, &io), 0);
    t.look = TUI_LOOK_PI;
    t.pith = &th;
    CHECK_INT(tui_start(&t), 0);
    pshow_init(&ps, &t, &th);
    pshow_tool_start(&ps, "t1", "bash", "{\"command\":\"seq 1 3000\"}", 25);
    pshow_tool_end(&ps, "t1", res_lines, (long)strlen(res_lines), 0, 100, det_lines, (long)strlen(det_lines));
    pshow_tool_start(&ps, "t2", "bash", "{\"command\":\"seq 1 3000\"}", 25);
    pshow_tool_end(&ps, "t2", res_plain, (long)strlen(res_plain), 0, 100, det_bytes, (long)strlen(det_bytes));
    tui_frame(&t);
    CHECK(screen_has("L3000"));
    CHECK(!screen_has("[Showing lines"));       /* the footer is the warning row's */
    CHECK(screen_has("[Full output: /tmp/pi-bash-1.log. Truncated: showing 2000 of 3000 lines]"));
    CHECK(screen_has("[Truncated: 243 lines shown (50.0KB limit)]"));
    pshow_free(&ps);
    tui_stop(&t);
    tui_free(&t);
    cs_close();
}

static void test_bash_rows_uncapped(void)
{
    static const char *script[] = { 0 };
    cl_io io;
    cl_tui t;
    cl_pitheme th;
    pi_show ps;
    jw res;
    int i;
    cs_open(80, 25, script);
    cs_io(&io);
    pitheme_system(&th);
    CHECK_INT(tui_init(&t, &io), 0);
    t.look = TUI_LOOK_PI;
    t.pith = &th;
    CHECK_INT(tui_start(&t), 0);
    pshow_init(&ps, &t, &th);
    jw_init(&res);
    jw_rawz(&res, "[{\"type\":\"text\",\"text\":\"");
    for (i = 1; i <= 300; i++) {
        char one[16];
        one[0] = 'L';
        cl_ltoa(i, one + 1);
        jw_rawz(&res, one);
        if (i < 300)
            jw_rawz(&res, "\\n");
    }
    jw_rawz(&res, "\"}]");
    pshow_tool_start(&ps, "t1", "bash", "{\"command\":\"seq\"}", 17);
    pshow_tool_end(&ps, "t1", res.p, res.n, 0, 100, 0, 0);
    tui_frame(&t);
    CHECK(screen_has("L300"));
    CHECK(screen_has("(295 earlier lines, ctrl+o to expand)"));
    CHECK(!screen_has("L295"));
    pshow_expand(&ps, 1, "Tool output: expanded");
    tui_frame(&t);
    CHECK(screen_has("L300"));
    CHECK(screen_has("L296"));
    CHECK(!screen_has("earlier lines"));
    CHECK(screen_has("Took 0.1s"));
    jw_free(&res);
    pshow_free(&ps);
    tui_stop(&t);
    tui_free(&t);
    cs_close();
}

/* \n and \t spelled as two characters in the cases file, and the backslash doubled */
static void fuzzy_unescape(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == 'n')
            *w++ = '\n', s++;
        else if (*s == '\\' && s[1] == 't')
            *w++ = '\t', s++;
        else if (*s == '\\' && s[1] == '\\')
            *w++ = '\\', s++;
        else
            *w++ = *s;
    }
    *w = 0;
}

/* whether the menu `got` (indexes into cmd) is the names in `w` (space separated, in order) */
static int fuzzy_same(const cl_cmd *cmd, const int *idx, int got, char *w, int *want)
{
    int k = 0, ok = 1;
    while (*w) {
        char *e = strchr(w, ' ');
        if (e)
            *e = 0;
        if (k >= got || strcmp(cmd[idx[k]].name + 1, w))
            ok = 0;
        k++;
        if (!e)
            break;
        w = e + 1;
    }
    *want = k;
    return ok && k == got;
}

/* J6: the slash menu is pi-tui's fuzzy filter in order of score (pi 1.1.0's own CombinedAutocompleteProvider, tools/pi_oracle/gen_fuzzy.mjs):
 * the order of every one- and two-letter query and some more, from the file; U1: the same for non-ASCII names and queries
 * (lowercased as JavaScript, positions in UTF-16 units) and for a cursor inside the text (only the first line's text before
 * the cursor counts: the lines starting with '@') */
static void test_slash_fuzzy(void)
{
    long n = 0;
    char *f = slurp("fuzzy_cases.txt", &n), *line;
    static cl_cmd cmd[64];
    static char name[64][48];
    int ncmd = 0, cases = 0, curs = 0, bad = 0;
    CHECK(f != 0);
    if (!f)
        return;
    for (line = f; *line; ) {
        char *eol = strchr(line, '\n'), *tab;
        if (!eol)
            break;
        *eol = 0;
        if (!ncmd) {
            /* the first line: the names, in menu order */
            char *w = line;
            while (*w) {
                char *e = strchr(w, ' ');
                if (e)
                    *e = 0;
                name[ncmd][0] = '/';
                strcpy(name[ncmd] + 1, w);
                cmd[ncmd].name = name[ncmd];
                cmd[ncmd].help = "";
                ncmd++;
                if (!e)
                    break;
                w = e + 1;
            }
        } else if (line[0] == '@' && (tab = strchr(line, '\t')) != 0) {
            int idx[64], got, k;
            char *after = tab + 1, *menu = strchr(after, '\t');
            char box[256];
            long from = -1, cur;
            *tab = 0;
            *menu++ = 0;
            fuzzy_unescape(line + 1);
            fuzzy_unescape(after);
            cur = (long)strlen(line + 1);
            strcpy(box, line + 1);
            strcat(box, after);
            got = pi_slash_filter(0, cmd, ncmd, box, (long)strlen(box), cur, &from, idx);
            curs++;
            if (!fuzzy_same(cmd, idx, got, menu, &k) && bad++ < 5)
                printf("    cursor case %d (%s|%s): pi lists %d, amiga-pi %d\n", curs, line + 1, after, k, got);
            else if (got && (from < 0 || from > cur || box[from] != '/'))
                bad++, printf("    cursor case %d: from %ld\n", curs, from);
        } else if ((tab = strchr(line, '\t')) != 0) {
            int idx[64], got, k;
            char text[256];
            long from = 0;
            *tab = 0;
            strcpy(text, "/");
            strcat(text, line);
            got = pi_slash_filter(0, cmd, ncmd, text, (long)strlen(text), (long)strlen(text), &from, idx);
            cases++;
            if (!fuzzy_same(cmd, idx, got, tab + 1, &k) && bad++ < 5)
                printf("    /%s: pi lists %d, amiga-pi %d\n", line, k, got);
        }
        line = eol + 1;
    }
    CHECK(cases > 700);
    CHECK(curs > 30);
    CHECK_INT(bad, 0);
    free(f);
}

/* P12-G1: every golden is captured from the pinned pi. A .mode file records the version that drew it (tools/pi_oracle/tui_gen.py);
 * one that names another pi is a golden nobody regenerated after the pin moved (138 of 210 said 1.1.0 at the close of P12).
 * The pin is read from tools/pi_oracle/pin.mjs, the one place that has it. */
static void test_golden_pin(void)
{
    char pin[32] = "", line[160];
    FILE *f = fopen(PI_TESTS "/../tools/pi_oracle/pin.mjs", "r");
    DIR *d;
    struct dirent *e;
    int seen = 0;
    while (f && fgets(line, sizeof line, f)) {
        const char *q = strstr(line, "PINNED = \"");
        if (q && sscanf(q + 10, "%31[0-9.]", pin) == 1)
            break;
    }
    if (f)
        fclose(f);
    CHECK(pin[0] != 0);
    d = opendir(PI_TESTS "/pi/tui");
    CHECK(d != 0);
    while (d && pin[0] && (e = readdir(d)) != 0) {
        size_t n = strlen(e->d_name);
        char path[600], want[64];
        if (n < 6 || strcmp(e->d_name + n - 5, ".mode"))
            continue;
        strcpy(path, PI_TESTS "/pi/tui/");
        strncat(path, e->d_name, 200);
        strcpy(want, "pi=");
        strcat(want, pin);
        strcat(want, " ");
        f = fopen(path, "r");
        line[0] = 0;
        if (f) {
            if (!fgets(line, sizeof line, f))
                line[0] = 0;
            fclose(f);
        }
        seen++;
        if (strncmp(line, want, strlen(want))) {
            printf("[FAIL] %s says \"%.12s\", not the pinned pi %s\n", e->d_name, line, pin);
            CHECK(0);
        }
    }
    if (d)
        closedir(d);
    CHECK(seen > 200);
}

/* I9: no row of the menu is dropped: 700 rows after the built-in ones (a copy once held 512), all of them offered and
 * found by their name (the menu once showed 64 matches at most) */
static void test_menu_rows_uncapped(void)
{
    static const char *keys[] = { "/", "\004", 0 };
    static const char *keys_x[] = { "/x699", "\004", 0 };
    static const char *keys_esc[] = { "/", "\033", "\004", 0 };
    static cl_cmd rows[700];
    static char names[700][8];
    int i;
    for (i = 0; i < 700; i++) {
        names[i][0] = '/';
        names[i][1] = 'x';
        names[i][2] = (char)('0' + i / 100);
        names[i][3] = (char)('0' + i / 10 % 10);
        names[i][4] = (char)('0' + i % 10);
        names[i][5] = 0;
        rows[i].name = names[i];
        rows[i].help = "a row";
    }
    pi_ui_set_extra_commands(2, rows, 700);
    app_write("settings.json", "{\"quietStartup\":true}\n");
    CHECK_INT(app_run(80, 25, keys, 0, 0, take_shot), 0);
    CHECK(shot_has("(1/715)"));             /* fifteen built-in rows (with /login, /logout, /changelog, /settings and /scoped-models) and 700 more */
    CHECK_INT(app_run(80, 25, keys_x, 0, 0, take_shot), 0);
    CHECK(shot_has("a row"));               /* the editor holds "/x699" too: the menu row is what has a description */
    /* Esc closes a menu of more than eight rows (it once asked for the matches into a table of eight) */
    CHECK_INT(app_run(80, 25, keys_esc, 0, 0, take_shot), 0);
    CHECK(!shot_has("(1/712)"));
    pi_ui_set_extra_commands(2, 0, 0);
}

static void test_footer_cost(void)
{
    long claude = bytes_per_key(TUI_LOOK_CLAUDE), pi = bytes_per_key(TUI_LOOK_PI);
    CHECK(claude > 0 && pi > 0);
    CHECK(pi <= claude);            /* the same path (build the rows, send those that changed): never more bytes */
    if (getenv("PI_TUI_DUMP"))
        printf("  six keys: C:Claude's look %ld bytes, pi's look %ld bytes\n", claude, pi);
}

/* the start header: the program's own name where pi has its version */
#define HDR_VER { 2, 2, 5, 79, "start header" }
#define HDR_HELP { 0, 0, 0, 0, 0 }

#define HDR_VER64 { 2, 2, 5, 63, "start header" }
#define HDR_HELP64 { 0, 0, 0, 0, 0 }

static const scen scenarios[] = {
    { "start", { 0 }, { 0 }, 0, 1, 0, { HDR_VER, HDR_HELP } },
    { "editor", { "hello there", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash", { "/", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* J6: pi-tui's fuzzy filter and its order of score (autocomplete.ts:343-372); the prefix match listed 3 rows for '/re', pi lists 4+ */
    { "slash_re", { "/re", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash_ct", { "/ct", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash_prompts", { "/rev", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* U1: the menu is for the text before the cursor ('/tr' with the cursor before the e), once closed it stays so when only the cursor
     * moves, and the names match as pi-tui lowercases them ('/É' lists café) */
    { "slash_mid", { "/tre\033[D", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash_closed", { "/resx\033[D", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash_utf", { "/\303\211", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "markdown", { "show markdown\r", 0 }, { "markdown.1.sse", 0 }, 0, 0, 0, { { 15, 15, 3, 16, "code highlighting" } } },
    { "table", { "show a table\r", 0 }, { "table.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "markdown2", { "more markdown\r", 0 }, { "markdown2.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "toolerr", { "run it\r", 0 }, { "toolerr.1.sse", "toolerr.2.sse", 0 }, 2, 0, 0, { { 13, 13, 6, 8, "elapsed time of the run" } } },
    { "error", { "hello\r", 0 }, { "error.1.http", 0 }, 0, 0, 0, { { 5, 8, 0, 79, "error text" }, { 25, 25, 0, 6, "context estimate" } } },
    { "working", { "hello\r", 0 }, { 0 }, 0, 0, 1, { { 21, 21, 3, 3, "spinner frame" }, { 25, 25, 0, 6, "context estimate" } } },
    { "queued", { "hello\r", "!steer me\r", "!and then this\033\r", 0 }, { 0 }, 0, 0, 1,
      { { 21, 21, 3, 3, "spinner frame" }, { 25, 25, 0, 6, "context estimate" } } },
    { "bang", { "!!echo hi\r", "!!!echo secret\r", 0 }, { 0 }, 3, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "twice", { "first\r", "second\r", 0 }, { "twice.1.sse", "twice.2.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* TM1/TM3: pi's regular mode (the editor follows the content; fullscreen's `twice` above pins it) */
    { "reg_start", { 0 }, { 0 }, 0, 1, 0, { HDR_VER } },
    { "reg_empty", { 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "reg_twice", { "first\r", "second\r", 0 }, { "twice.1.sse", "twice.2.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "reg_markdown", { "show markdown\r", 0 }, { "markdown.1.sse", 0 }, 0, 0, 0, { { 15, 15, 3, 16, "code highlighting" } } },
    { "reg_long", { "more markdown\r", 0 }, { "markdown2.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* TM1: the fullscreen viewport: the end followed, a page back (the jump indicator), the scrollbar */
    { "fs_long", { "more markdown\r", 0 }, { "markdown2.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "fs_long_up", { "more markdown\r", "\033[5~", 0 }, { "markdown2.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "fs_long_bar", { "more markdown\r", "\033[5~", 0 }, { "markdown2.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* TM1: what pi leaves on the main screen when it ends: the conversation and the footer, then the resume line
     * (fullscreen "transcript", regular), or the resume line alone (fullscreen "resume-hint") */
    { "exit_transcript", { "first\r", 0 }, { "exit_transcript.1.sse", 0 }, 0, 0, 3, { { 0, 0, 0, 0, 0 } } },
    { "exit_hint", { "first\r", 0 }, { "exit_hint.1.sse", 0 }, 0, 0, 3, { { 0, 0, 0, 0, 0 } } },
    { "exit_regular", { "first\r", 0 }, { "exit_regular.1.sse", 0 }, 0, 0, 3, { { 0, 0, 0, 0, 0 } } },
    /* TM4: pi rings nothing at the end of a long turn (C:Claude's screen rings and sends OSC 9) */
    { "turn_raw", { "hello\r", 0 }, { "turn_raw.1.sse", 0 }, 0, 0, 4, { { 25, 25, 0, 6, "context estimate" } } },
    /* TM6: pi's dark and light themes: the panels' backgrounds */
    { "dark_start", { 0 }, { 0 }, 0, 1, 0, { HDR_VER, HDR_HELP } },
    { "dark_twice", { "first\r", "second\r", 0 }, { "twice.1.sse", "twice.2.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_markdown", { "show markdown\r", 0 }, { "markdown.1.sse", 0 }, 0, 0, 0, { { 15, 15, 3, 16, "code highlighting" } } },
    { "dark_tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_toolerr", { "run it\r", 0 }, { "toolerr.1.sse", "toolerr.2.sse", 0 }, 2, 0, 0,
      { { 13, 13, 6, 8, "elapsed time of the run" } } },
    /* the box while the tool runs (toolPendingBg) with its live "Elapsed" row */
    { "dark_toolrun", { "run it\r", 0 }, { "dark_toolrun.1.sse", "dark_toolrun.2.sse", 0 }, 2, 0, 2,
      { { 21, 21, 3, 3, "spinner frame" } } },
    { "dark_toolrun_out", { "run it\r", 0 }, { "dark_toolrun_out.1.sse", "dark_toolrun_out.2.sse", 0 }, 2, 0, 2,
      { { 21, 21, 3, 3, "spinner frame" } } },
    { "dark_tools2", { "run both\r", 0 }, { "dark_tools2.host.1.sse", "dark_tools2.host.2.sse", 0 }, 2, 0, 0,
      { { 13, 13, 0, 9, "elapsed time of the first call" } } },
    { "dark_bang", { "!!echo hi\r", 0 }, { 0 }, 3, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* P11 (pi 1.1.0): `!!` keeps its dim header; outputPad 0 takes the padding off messages, tool boxes and `!` output */
    { "bang_excluded", { "!!!echo hi\r", 0 }, { 0 }, 3, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "pad0_markdown", { "show markdown\r", 0 }, { "markdown.1.sse", 0 }, 0, 0, 0, { { 15, 15, 3, 16, "code highlighting" } } },
    { "pad0_tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "pad0_toolerr", { "run it\r", 0 }, { "toolerr.1.sse", "toolerr.2.sse", 0 }, 2, 0, 0, { { 13, 13, 0, 8, "elapsed time of the run" } } },
    { "pad0_bang", { "!!echo hi\r", "!!!echo secret\r", 0 }, { 0 }, 3, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_pad0_tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* R5: Ctrl+O over tool boxes (renderers/{read,bash,ls}.ts), the request's error (not in the session store) and a running bash call */
    { "tool_expanded", { "read the file\r", "\017", 0 }, { "tool_expanded.1.sse", "tool_expanded.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_tool_expanded", { "read the file\r", "\017", 0 }, { "dark_tool_expanded.1.sse", "dark_tool_expanded.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_bash_long", { "count\r", 0 }, { "dark_bash_long.1.sse", "dark_bash_long.2.sse", 0 }, 4, 0, 0,
      { { 15, 15, 6, 8, "elapsed time of the run" } } },
    { "dark_bash_long_expanded", { "count\r", "\017", 0 }, { "dark_bash_long_expanded.1.sse", "dark_bash_long_expanded.2.sse", 0 }, 4, 0, 0,
      { { 19, 19, 6, 8, "elapsed time of the run" } } },
    { "dark_ls_long", { "list\r", 0 }, { "dark_ls_long.1.sse", "dark_ls_long.2.sse", 0 }, 5, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_ls_long_expanded", { "list\r", "\017", 0 }, { "dark_ls_long_expanded.1.sse", "dark_ls_long_expanded.2.sse", 0 }, 5, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* U1b: the tool boxes of pi's renderers on amiga-pi's own tools: a read highlighted and in its short forms (renderers/read.ts), a write's
     * content, an edit's diff (renderers/edit.ts, components/diff.ts), the warnings of the results' details (read.ts, ls.ts) */
    { "dark_read_hl", { "show main\r", "\017", 0 }, { "dark_read_hl.1.sse", "dark_read_hl.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_read_hl_py", { "show it\r", "\017", 0 }, { "dark_read_hl_py.1.sse", "dark_read_hl_py.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_read_skill", { "read\r", 0 }, { "dark_read_skill.1.sse", "dark_read_skill.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_read_skill_expanded", { "read\r", "\017", 0 }, { "dark_read_skill_expanded.1.sse", "dark_read_skill_expanded.2.sse", 0 }, 6, 0, 0,
      { { 0, 0, 0, 0, 0 } } },
    { "dark_write", { "write it\r", 0 }, { "dark_write.1.sse", "dark_write.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_write_expanded", { "write it\r", "\017", 0 }, { "dark_write_expanded.1.sse", "dark_write_expanded.2.sse", 0 }, 6, 0, 0,
      { { 0, 0, 0, 0, 0 } } },
    { "dark_write_txt", { "write it\r", 0 }, { "dark_write_txt.1.sse", "dark_write_txt.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_edit", { "edit it\r", 0 }, { "dark_edit.1.sse", "dark_edit.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_edit_err", { "edit it\r", 0 }, { "dark_edit_err.1.sse", "dark_edit_err.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "dark_read_trunc_expanded", { "read big\r", "\017", 0 }, { "dark_read_trunc_expanded.1.sse", "dark_read_trunc_expanded.2.sse", 0 }, 6, 0, 0,
      { { 0, 0, 0, 0, 0 } } },
    { "dark_ls_limit", { "list\r", 0 }, { "dark_ls_limit.1.sse", "dark_ls_limit.2.sse", 0 }, 6, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "error_expanded", { "hello\r", "\017", 0 }, { "error_expanded.1.http", 0 }, 0, 0, 0,
      { { 5, 12, 0, 79, "error text: pi prints the response body and a hint row; the status is checked in expanded_checks" },
        { 25, 25, 0, 6, "context estimate" } } },
    { "bang_expanded", { "!!echo hi\r", "\017", 0 }, { 0 }, 3, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* a running bash call: Ctrl+O while it runs draws the chat again, the box live (the clock goes on 2.0 s: the capture) */
    { "dark_toolrun_out_expanded", { "run it\r", 0 }, { "dark_toolrun_out_expanded.1.sse", "dark_toolrun_out_expanded.2.sse", 0 }, 2, 0, 2,
      { { 28, 28, 3, 3, "spinner frame" } } },
    { "light_twice", { "first\r", "second\r", 0 }, { "twice.1.sse", "twice.2.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "light_tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* G2: the footer's right-aligned model at the widths where footer.ts:177-249 changes its mind: flush to the last column (45),
     * the model cut to the room left, never the usage (41), the usage itself cut with "..." (30) */
    /* G3: the start header at 64 columns, where the hint wraps, whatever outputPad is (BuiltInHeader's own padding of one) */
    { "start_w64", { 0 }, { 0 }, 0, 1, 0, { HDR_VER64, HDR_HELP64 } },
    /* U1b: Ctrl+O at the start: the header's long form, one hint a row (pi prints Alt as "option" on macOS, where the capture was
     * made; ours says alt); quietStartup "header" shows the header without the loaded resources, and its line says so */
    { "start_expanded", { "\017", 0 }, { 0 }, 0, 1, 0, { HDR_VER, { 18, 19, 0, 79, "option is macOS's name for alt" } } },
    { "start_expanded_w64", { "\017", 0 }, { 0 }, 0, 1, 0, { HDR_VER64, { 18, 19, 0, 63, "option is macOS's name for alt" } } },
    { "start_header_only", { 0 }, { 0 }, 0, 1, 0, { HDR_VER } },
    { "pad0_start_w64", { 0 }, { 0 }, 0, 1, 0, { HDR_VER64, HDR_HELP64 } },
    /* U1b: showStatus puts a dim status in the place of the last one (interactive-mode.ts:3824-3844): Alt+Up twice with nothing queued
     * says "No queued messages to restore" once, not twice */
    { "status_twice", { "\033[1;3A", "\033[1;3A", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "foot_exact", { "first\r", 0 }, { "foot_exact.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "foot_cut", { "first\r", 0 }, { "foot_cut.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "foot_left", { "first\r", 0 }, { "foot_left.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
};

void suite_pi_tui(void)
{
    unsigned i;
    test_queue_agent();
    test_steer_ui();
    test_spinner_while_waiting();
    test_title_dir_name();
    test_window_title();
    app_sandbox();
    test_cycle();
    test_tree();
    test_tree_nav();
    test_resume_took();
    test_resume_ansi();
    test_resume_summaries();
    test_expand_summaries();
    test_skills_screen();
    test_login_screens();
    test_whats_new();
    test_selectors();
    test_scoped();
    test_settings_screens();
    test_settings_behaviour();
    test_config();
    test_fork_selector();
    test_login_typed();
    test_compact_live();
    test_export();
    test_bang();
    test_output_pad_setting();
    test_settings_errors();
    test_default_tools_setting();
    test_program_status();
    test_pstatus_states();
    test_program_status_name();
    test_resume_session_dir();
    test_context_figure();
    test_ctrl_d();
    test_themes();
    test_theme_keys_app();
    test_tui_mode_app();
    test_approval();
    test_resume_selector();
    test_ext_ui_screen();
    test_ext_msg_screen();
    test_ext_uix_screen();
    test_footer_cost();
    test_bash_rows_uncapped();
    test_bash_details();
    test_slash_fuzzy();
    test_menu_rows_uncapped();
    test_golden_pin();
    app_cleanup();
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (getenv("PI_TUI_ONLY") && strcmp(getenv("PI_TUI_ONLY"), scenarios[i].name))
            continue;
        run(&scenarios[i]);
    }
}
