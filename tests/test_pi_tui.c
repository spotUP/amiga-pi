/* amiga-pi's screen against pi 1.0.4's (plan N0, N7): pi_ui_run() through C:Claude's screen harness
 * (tests/claude_screen.c, the engine as the terminal) with the answers pi was given (the .sse files of tests/pi/tui), the
 * screen compared cell by cell -- text, colours and attributes -- with the capture of the real pi
 * (the .golden files of tests/pi/tui, tools/pi_oracle/tui_gen.py). A cell that differs on purpose is listed in the
 * scenario's masks, each with the COMPATIBILITY.md row that explains it. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pi_test.h"
#include "claude/util.h"
#include "pi_stub.h"
#include "claude_screen.h"
#include "llm/transform.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/ui_pi.h"
#include "../amiga-pi/show_pi.h"
#include "../amiga-pi/fuzzy.h"
#include "../amiga-pi/okcolor.h"
#include "../amiga-pi/themes_builtin.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/session3.h"
#include "../amiga-pi/context.h"
#include "../amiga-pi/compact.h"
#include "../amiga-pi/pstatus.h"
#include "../amiga-pi/templates.h"
#include "claude/sys_posix.h"
#include "claude/theme.h"
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
    int tools;                  /* 1 read answers, 2 bash fails, 3 bash echoes, 4 bash counts to 12, 5 ls lists 25 files */
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
#define SC_THEME(sc) (!strncmp((sc)->name, "dark_", 5) ? 1 : !strncmp((sc)->name, "light_", 6) ? 2 : 0)
static int cmp_shift;           /* the capture's row r is on our screen at row r + cmp_shift (a selector drawn at the bottom) */

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
    want = h_new(80, rows, VT_XTERM);
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
        for (c = 0; c < 80; c++) {
            const vt_cell *a = h_cell(want, c, r), *b;
            vt_color af, ab, bf, bb;
            vt_attr mask = (vt_attr)(VT_ATTR_BOLD | VT_ATTR_FAINT | VT_ATTR_ITALIC | VT_ATTR_UNDERLINE | VT_ATTR_INVERSE | VT_ATTR_STRIKE);
            int same;
            if (r + cmp_shift < 0 || r + cmp_shift >= cs.rows)
                continue;
            b = h_cell(cs.vt, c, r + cmp_shift);
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
        up |= strstr(cs_row(r), "fake-model") != 0;
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
    cs_open(80, want_rows + (sc->when == 3 ? 8 : 0), script);
    cs.before_read = hook;
    cs_io(&io);
    pi_ui_set_cwd("~/project");
    pi_ui_set_quiet(!sc->header);
    pi_ui_set_commands(pi_cmds, (int)(sizeof(pi_cmds) / sizeof(pi_cmds[0])));
    if (!strcmp(sc->name, "slash_prompts")) {
        /* J2: the agent directory's prompts/ (the same files tools/pi_oracle/tui_gen.py gave pi) after the built-in rows */
        static sys_posix sp;
        static cl_sys tsys;
        int nt;
        const cl_cmd *rows;
        sys_posix_init(&sp, &tsys);
        CHECK_INT(test_tmpl_load(&slash_tm, &tsys, PI_TESTS "/pi/tui/agent_dir", "/nonexistent", 0, 0, 0, 0), 0);
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
            err |= strstr(snap[r], "Error: HTTP 400") != 0;
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
    vt_free(want);
    pi_session_free(&ses);
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

/* the window title is pi's -- "pi - <directory name>", the letter pi first -- set once at the start and left alone while
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
        if (e - (p + 4) != (long)strlen("\317\200 - project") || strncmp(p + 4, "\317\200 - project", (size_t)(e - (p + 4))))
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
static void (*at_rest)(void);   /* called once with the screen at rest (the next chunk is the last one) */
static int rested;

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

static long app_env(const char *name, char *o, long cap)
{
    const char *v = 0;
    if (!strcmp(name, "COLORTERM"))
        v = app_colorterm;
    else if (!strcmp(name, "TERM"))
        v = "vtcon";
    else if (!strcmp(name, "PI_PROGRAM_STATUS"))
        v = app_pstatus;
    if (!strcmp(name, "OPENAI_API_KEY"))
        v = "sk-test-openai";
    else if (!strcmp(name, "ANTHROPIC_API_KEY"))
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
        printf("  hook: next=%d chunk=%s ms=%ld bottom=[%s]\n", cs.next, cs.script[cs.next] ? cs.script[cs.next] : "(end)", cs.read_ms, cs_row(cs.rows - 1));
    if (rested || (rest_any && cs_find("navigate") < 0) ||
        (!rest_any && !strstr(cs_row(cs.rows - 1), "-model") && !strstr(cs_row(cs.rows - 1), "claude-")) ||
        cs.read_ms <= 0 || !cs.script[cs.next] || strcmp(cs.script[cs.next], "\004"))
        return;
    rested = 1;
    if (at_rest)
        at_rest();
}

/* pi_app with the keys typed on a screen of cols x rows; the answers streamed in order. The exit code. */
static const char *app_session;     /* --session <file> for the next app_run, 0 none */
static const char *app_agent;       /* the agent directory for the next app_run (0: the sandbox's) */
static const char *app_tui_cli;     /* --tui-mode <x> for the next app_run, 0 none */
static const char *app_name_cli;    /* --name <x> for the next app_run, 0 none */
static const char *app_sdir_cli;    /* --session-dir <x> for the next app_run, 0 none */
static char *app_out;               /* every byte the last app_run wrote to the screen */

static int app_run(int cols, int rows, const char **keys, const char *const *sse, int nsse, void (*rest)(void))
{
    cl_io io;
    pi_platform pl;
    char *a[] = { "amiga-pi", "--model", "anthropic/claude-opus-5-5", 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int i, rc, argc = 3;
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
static void take_shot(void)
{
    int r;
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
    CHECK(shot_has("Switched to GPT-4.1"));
    CHECK(shot_has("Switched to Claude Opus 5.5 (thinking: high)") || shot_has("(thinking: high)"));
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
    b = slurp("skills_files.json", &n);
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
    skills_masks(&sc, name, k);
    cmp_shift = shift;
    app_session = session ? path : 0;
    app_agent = with_skills ? skdir : 0;
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

static void test_skills_screen(void)
{
    static const char *keys_end[] = { "\004", 0 };
    static const char *keys_menu[] = { "/skill:", "\004", 0 };
    static const char *keys_exp[] = { "\017", "\004", 0 };
    static const char *keys_cmd[] = { "/skill:greet please greet Bob\r", "\004", 0 };
    static const char *const sse_cmd[] = { "skill_cmd.1.sse" };
    static const int m_start[] = { 1, 9, 22, 40, 0 }, m_quiet[] = { 26, 30, 0 }, m_menu[] = { 23, 23, 25, 25, 29, 30, 0 },
                     m_exp[] = { 36, 40, 0 };
    char cmd[460];
    /* the start: header, "[Skills]", "[Skill conflicts]" (the capture's rows 10-21; ours begin four rows higher: pi's help panel and tips are not drawn) */
    skills_case("skills_start", "{}\n", keys_end, 0, 0, 0, -4, m_start, 1);
    /* quietStartup: the diagnostics alone, at the top */
    skills_case("skills_quiet", "{\"quietStartup\":true}\n", keys_end, 0, 0, 0, 0, m_quiet, 1);
    skills_case("skills_menu", "{\"quietStartup\":true}\n", keys_menu, 0, 0, 0, 0, m_menu, 1);
    skills_case("resume_skill", "{\"quietStartup\":true}\n", keys_end, 0, 0, 1, 0, m_quiet, 0);
    skills_case("resume_skill_expanded", "{\"quietStartup\":true}\n", keys_exp, 0, 0, 1, 0, m_exp, 0);
    skills_case("skill_cmd", "{\"quietStartup\":true}\n", keys_cmd, sse_cmd, 1, 0, 0, m_quiet, 1);
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
        CHECK(memcmp(g, "", 0) == 0 && strstr(g, "\033[>7u\033[?u\033]7501;?\033\\\033[c") != 0);      /* pi's startup queries */
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
    pshow_tool_end(&ps, "t1", res.p, res.n, 0, 100);
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

/* J6: the slash menu is pi-tui's fuzzy filter in order of score (pi 1.1.0's own CombinedAutocompleteProvider, tools/pi_oracle/gen_fuzzy.mjs):
 * the order of every one- and two-letter query and some more, from the file */
static void test_slash_fuzzy(void)
{
    long n = 0;
    char *f = slurp("fuzzy_cases.txt", &n), *line;
    static cl_cmd cmd[64];
    static char name[64][40];
    int ncmd = 0, cases = 0, bad = 0;
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
        } else if ((tab = strchr(line, '\t')) != 0) {
            int idx[64], got, k = 0, ok = 1;
            char text[80];
            char *w = tab + 1;
            *tab = 0;
            strcpy(text, "/");
            strcat(text, line);
            got = pi_slash_filter(0, cmd, ncmd, text, (long)strlen(text), idx);
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
            if (k != got)
                ok = 0;
            cases++;
            if (!ok && bad++ < 5)
                printf("    /%s: pi lists %d, amiga-pi %d\n", line, k, got);
        }
        line = eol + 1;
    }
    CHECK(cases > 700);
    CHECK_INT(bad, 0);
    free(f);
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
    CHECK(shot_has("(1/710)"));             /* ten built-in rows and 700 more */
    CHECK_INT(app_run(80, 25, keys_x, 0, 0, take_shot), 0);
    CHECK(shot_has("a row"));               /* the editor holds "/x699" too: the menu row is what has a description */
    /* Esc closes a menu of more than eight rows (it once asked for the matches into a table of eight) */
    CHECK_INT(app_run(80, 25, keys_esc, 0, 0, take_shot), 0);
    CHECK(!shot_has("(1/710)"));
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

/* the start header: the program's own name for pi's version, no startup-help panel and no pi tips */
#define HDR_VER { 2, 2, 5, 79, "start header" }
#define HDR_HELP { 5, 5, 0, 79, "start header" }, { 7, 8, 0, 79, "start header" }

static const scen scenarios[] = {
    { "start", { 0 }, { 0 }, 0, 1, 0, { HDR_VER, HDR_HELP } },
    { "editor", { "hello there", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash", { "/", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* J6: pi-tui's fuzzy filter and its order of score (autocomplete.ts:343-372); the prefix match listed 3 rows for '/re', pi lists 4+ */
    { "slash_re", { "/re", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash_ct", { "/ct", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash_prompts", { "/rev", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
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
    { "reg_start", { 0 }, { 0 }, 0, 1, 0,
      { HDR_VER, { 5, 25, 0, 79, "start header: pi's help panel and tips are not drawn, the footer follows five rows higher" } } },
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
    { "error_expanded", { "hello\r", "\017", 0 }, { "error_expanded.1.http", 0 }, 0, 0, 0,
      { { 5, 12, 0, 79, "error text: pi prints the response body and a hint row; the status is checked in expanded_checks" },
        { 25, 25, 0, 6, "context estimate" } } },
    { "bang_expanded", { "!!echo hi\r", "\017", 0 }, { 0 }, 3, 0, 0, { { 0, 0, 0, 0, 0 } } },
    /* a running bash call: Ctrl+O while it runs draws the chat again, the box live (the clock goes on 2.0 s: the capture) */
    { "dark_toolrun_out_expanded", { "run it\r", 0 }, { "dark_toolrun_out_expanded.1.sse", "dark_toolrun_out_expanded.2.sse", 0 }, 2, 0, 2,
      { { 28, 28, 3, 3, "spinner frame" } } },
    { "light_twice", { "first\r", "second\r", 0 }, { "twice.1.sse", "twice.2.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "light_tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
};

void suite_pi_tui(void)
{
    unsigned i;
    test_queue_agent();
    test_steer_ui();
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
    test_compact_live();
    test_export();
    test_bang();
    test_output_pad_setting();
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
    test_footer_cost();
    test_bash_rows_uncapped();
    test_slash_fuzzy();
    test_menu_rows_uncapped();
    app_cleanup();
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (getenv("PI_TUI_ONLY") && strcmp(getenv("PI_TUI_ONLY"), scenarios[i].name))
            continue;
        run(&scenarios[i]);
    }
}
