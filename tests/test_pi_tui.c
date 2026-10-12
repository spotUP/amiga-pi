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

/* a mask: rows r0..r1 (1-based, golden), columns c0..c1 (0-based, inclusive), the COMPATIBILITY.md row */
typedef struct mask {
    int r0, r1, c0, c1;
    const char *why;
} mask;

typedef struct scen {
    const char *name;           /* the golden's name */
    const char *keys[8];        /* typed chunks; a '!' chunk is typed while the agent works */
    const char *answers[4];     /* tests/pi/tui/<file>: .sse streamed, .http whole */
    int tools;                  /* 1 read answers, 2 bash fails */
    int header;                 /* the start header is shown (else settings.json has quietStartup)  */
    int when;                   /* 0 at rest after the keys, 1 while the agent works (after the keys) */
    mask masks[12];
} scen;

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

/* the tools under pi's names: read answers with the file, bash fails like cat of a missing file */
static int tool_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    const char *text;
    int err = 0;
    (void)s;
    (void)args;
    (void)an;
    echo_calls++;
    if (u) {
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
static pi_tool t_read = { "read", "Read a file", PARAMS, 0, 0, tool_run, 0, 0 };
static pi_tool t_bash = { "bash", "Run a command", PARAMS, 0, (void *)1, tool_run, 0, 0 };

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
            const vt_cell *a = h_cell(want, c, r), *b = h_cell(cs.vt, c, r);
            vt_color af, ab, bf, bb;
            vt_attr mask = (vt_attr)(VT_ATTR_BOLD | VT_ATTR_FAINT | VT_ATTR_ITALIC | VT_ATTR_UNDERLINE | VT_ATTR_INVERSE | VT_ATTR_STRIKE);
            int same;
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

static void hook(void)
{
    if (taken || !strstr(cs_row(cs.rows - 1), "fake-model"))   /* the footer is up */
        return;
    if (cur->when == 0) {
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
    cs_open(80, want_rows, script);
    cs.before_read = hook;
    cs_io(&io);
    pi_ui_set_cwd("~/project");
    pi_ui_set_quiet(!sc->header);
    pi_ui_set_commands(pi_cmds, (int)(sizeof(pi_cmds) / sizeof(pi_cmds[0])));
    if (sc->when == 1)
        ps.silent = 1;
    CHECK_INT(pi_ui_run(&ses, &io, 0), 0);
    CHECK(taken);
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

/* the start header: the program's own name for pi's version, no startup-help panel and no pi tips */
#define HDR_VER { 2, 2, 5, 79, "start header" }
#define HDR_HELP { 5, 5, 0, 79, "start header" }, { 7, 8, 0, 79, "start header" }

static const scen scenarios[] = {
    { "start", { 0 }, { 0 }, 0, 1, 0, { HDR_VER, HDR_HELP } },
    { "editor", { "hello there", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "slash", { "/", 0 }, { 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "markdown", { "show markdown\r", 0 }, { "markdown.1.sse", 0 }, 0, 0, 0, { { 15, 15, 3, 16, "code highlighting" } } },
    { "table", { "show a table\r", 0 }, { "table.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "markdown2", { "more markdown\r", 0 }, { "markdown2.1.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "tool", { "read the file\r", 0 }, { "tool.1.sse", "tool.2.sse", 0 }, 1, 0, 0, { { 0, 0, 0, 0, 0 } } },
    { "toolerr", { "run it\r", 0 }, { "toolerr.1.sse", "toolerr.2.sse", 0 }, 2, 0, 0, { { 13, 13, 6, 8, "elapsed time of the run" } } },
    { "error", { "hello\r", 0 }, { "error.1.http", 0 }, 0, 0, 0, { { 5, 8, 0, 79, "error text" }, { 25, 25, 0, 6, "context estimate" } } },
    { "working", { "hello\r", 0 }, { 0 }, 0, 0, 1, { { 21, 21, 3, 3, "spinner frame" }, { 25, 25, 0, 6, "context estimate" } } },
    { "twice", { "first\r", "second\r", 0 }, { "twice.1.sse", "twice.2.sse", 0 }, 0, 0, 0, { { 0, 0, 0, 0, 0 } } },
};

void suite_pi_tui(void)
{
    unsigned i;
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (getenv("PI_TUI_ONLY") && strcmp(getenv("PI_TUI_ONLY"), scenarios[i].name))
            continue;
        run(&scenarios[i]);
    }
}
