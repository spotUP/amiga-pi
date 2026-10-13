/* see ui_pi.h */
#include <stdlib.h>
#include <string.h>
#include "ui_pi.h"
#include "fuzzy.h"
#include "args.h"
#include "claude/tui.h"
#include "claude/show.h"
#include "claude/util.h"
#include "show_pi.h"
#include "resources.h"
#include "templates.h"
#include "resreport.h"
#include "pstatus.h"
#include "claude/tools.h"
#include "compact.h"
#include "tree_pi.h"
#include "store.h"
#include "context.h"
#include "login.h"

#define XU_KEYS 16                  /* statuses and widgets an extension session keeps at most */
#define XU_MAX_WIDGET_LINES 10      /* interactive-mode.ts MAX_WIDGET_LINES */

typedef struct pi_ui {
    cl_ui ui;
    cl_render render;
    pi_session *s;
    long text_index;            /* the streaming answer's text block, -1 none */
    int print;                  /* -p: nothing shown while it runs */
    int look;                   /* the screen runs in pi's look (show_pi.c writes the transcript) */
    pi_show ps;
    cl_pitheme theme;
    pi_pstatus pst;             /* OSC 7501 program status (pstatus.h) */
    int pst_on;                 /* the screen is up and reports go through pst */
    char name[160];             /* the session's name as pst_name last read it from the store */
    cl_cmd *xmenu;              /* the extensions' commands (slot 1) and their strings */
    char *xmenu_text;
    cl_cmd *menu;               /* the slash menu: the commands, then /skill:name for each skill (settings.json enableSkillCommands) */
    char *menu_text;            /* its strings */
    cl_cmd *all;                /* the slash menu as the screen gets it: the built-in rows, then each slot's (ui_menu) */
    /* what extensions put on the screen: statuses (footer row) and widgets (above or below the editor), by key */
    struct { char key[64]; char *text; } xstat[XU_KEYS];
    int nxstat;
    struct { char key[64]; char *lines; int n, below; } xwid[XU_KEYS];
    int nxwid;
    int xwork;                  /* an extension set the loader's label (setWorkingMessage) */
    char xwork_text[96];
} pi_ui;

/* the slash commands the menu offers: pi's own names and descriptions (core/slash-commands.js, in its order) of
 * the ones amiga-pi has; pi_ui_set_commands replaces the list (the tests give it pi's whole list) */
static const cl_cmd default_cmds[] = {
    { "/model", "<provider/model> \342\200\224 Select model (opens selector UI)" },
    { "/tree", "Navigate session tree (switch branches)" },
    { "/thinking", "<level> \342\200\224 Set thinking level" },
    { "/export", "Export session (HTML default, or specify path: .html/.jsonl)" },
    { "/name", "Set session display name" },
    { "/session", "Show session info and stats" },
    { "/fork", "Create a new fork from a previous user message" },
    { "/clone", "Duplicate the current session at the current position" },
    { "/trust", "Save project trust decision for future sessions" },
    { "/login", "<provider> \342\200\224 Configure provider authentication" },
    { "/logout", "Remove provider authentication" },
    { "/quit", "Quit amiga-pi" }
};
static const cl_cmd *ui_cmds = default_cmds;
static int ui_ncmds = (int)(sizeof(default_cmds) / sizeof(default_cmds[0]));

void pi_ui_set_commands(const cl_cmd *cmds, int n)
{
    ui_cmds = cmds ? cmds : default_cmds;
    ui_ncmds = cmds ? n : (int)(sizeof(default_cmds) / sizeof(default_cmds[0]));
}

/* the rows after the built-in ones: prompt templates (templates.h pi_tmpl_menu), in pi's order (built-in, templates,
 * extensions, skills); the arrays belong to the caller and stay until the run ends */
static const cl_cmd *ui_extra[4];
static int ui_nextra[4];

void pi_ui_set_extra_commands(int slot, const cl_cmd *cmds, int n)
{
    if (slot >= 0 && slot < 4) {
        ui_extra[slot] = cmds;
        ui_nextra[slot] = cmds ? n : 0;
    }
}

/* the menu: the built-in list, then each slot's rows, every one of them (a copy sized to the rows, owned by p->all) */
static const cl_cmd *ui_menu(pi_ui *p, int *n)
{
    int i, k, m = 0, total = ui_ncmds;
    if (!ui_nextra[0] && !ui_nextra[1] && !ui_nextra[2] && !ui_nextra[3]) {
        *n = ui_ncmds;
        return ui_cmds;
    }
    for (k = 0; k < 4; k++)
        total += ui_nextra[k];
    free(p->all);
    p->all = (cl_cmd *)malloc(sizeof(cl_cmd) * (size_t)total);
    if (!p->all) {
        *n = ui_ncmds;              /* out of memory: the built-in rows alone */
        return ui_cmds;
    }
    for (i = 0; i < ui_ncmds; i++)
        p->all[m++] = ui_cmds[i];
    for (k = 0; k < 4; k++)
        for (i = 0; i < ui_nextra[k]; i++)
            p->all[m++] = ui_extra[k][i];
    *n = m;
    return p->all;
}

/* what the footer shows of the program (pi_ui_set_cwd): the working directory */
static char ui_cwd[300];
static char ui_version[40] = "";
static int ui_quiet;
static cl_pitheme ui_theme;
static int ui_have_theme;
static const char *ui_kb;
static long ui_kb_n;

void pi_ui_set_theme(const cl_pitheme *th)
{
    if (th) {
        ui_theme = *th;
        ui_have_theme = 1;
    } else
        ui_have_theme = 0;
}

void pi_ui_set_keybindings(const char *json, long n)
{
    ui_kb = json;
    ui_kb_n = n;
}

void pi_ui_set_quiet(int quiet)
{
    ui_quiet = quiet;               /* 0 the header and what is loaded, 1 neither (quietStartup true), 2 the header only ("header") */
}

static char ui_warn[4][1100];
static int ui_nwarn;

void pi_ui_add_warning(const char *text)
{
    if (ui_nwarn < 4) {
        cl_copy(ui_warn[ui_nwarn], "Warning: ", sizeof(ui_warn[0]));
        cl_cat(ui_warn[ui_nwarn], text, sizeof(ui_warn[0]));
        ui_nwarn++;
    }
}

static int ui_regular, ui_hint_only, ui_bar;
static const char *(*ui_resume)(void *u);
static void *ui_resume_u;

void pi_ui_set_tui(int regular, int hint_only, int scrollbar_always)
{
    ui_regular = regular;
    ui_hint_only = hint_only;
    ui_bar = scrollbar_always;
}

static int ui_pad = 1;

void pi_ui_set_output_pad(int pad)
{
    ui_pad = pad ? 1 : 0;
}

void pi_ui_set_resume(const char *(*fn)(void *u), void *u)
{
    ui_resume = fn;
    ui_resume_u = u;
}

void pi_ui_set_cwd(const char *cwd)
{
    cl_copy(ui_cwd, cwd ? cwd : "", sizeof(ui_cwd));
}

static int ui_pstatus;

void pi_ui_set_program_status(int on)
{
    ui_pstatus = on;
}

static void pst_write(void *u, const char *z, long n)
{
    pi_ui *p = (pi_ui *)u;
    if (p->ui.tui)
        tui_raw(p->ui.tui, z, n);
}

/* pi: getSessionName() on the session manager at every report (interactive-mode.ts programStatus), so a name given
 * at the start (--name, a resumed named session) is there before any session-info event */
static const char *pst_name(void *u)
{
    pi_ui *p = (pi_ui *)u;
    pi_store *st = (pi_store *)p->s->command_u;
    if (st && p->s->command && s3_name(&st->s3, p->name, sizeof(p->name)) > 0)
        return p->name;
    return 0;
}


/* pi's renderInitialMessages (interactive-mode.ts:4208-4222): the context's AgentMessages drawn (a stored session: the
 * custom, compaction-summary and branch-summary messages as their own components; ses->msg holds only what the model
 * is sent), then "Session compacted N time(s)" when the file holds any compaction entry */
/* redraw_chat's modes: renderInitialMessages (with its "Session compacted" note) and compaction_end's */
enum { REDRAW_INITIAL, REDRAW_COMPACTED };
static void redraw_chat(pi_ui *p, int mode);

static void redraw_session(pi_ui *p)
{
    redraw_chat(p, REDRAW_INITIAL);
}

/* pi's compaction_end (interactive-mode.ts:3700-3715): the chat cleared, the context's entries after the compaction
 * redrawn (renderSessionEntries(entries.slice(1))), the summary box appended below them at its chronological place,
 * and no "Session compacted" note (that is renderInitialMessages' alone) */
static void redraw_compacted(pi_ui *p, long tokens_before, const char *summary, long summary_n)
{
    pshow_clear(&p->ps);
    redraw_chat(p, REDRAW_COMPACTED);
    pshow_compaction_summary(&p->ps, tokens_before, summary, summary_n);
}

static void redraw_chat(pi_ui *p, int mode)
{
    int after_compaction = mode == REDRAW_COMPACTED;
    pi_store *st = (pi_store *)p->s->command_u;
    pi_ctx c;
    int i, compactions = 0;
    if (st && p->s->command) {
        ctx_init(&c);
        if (!ctx_build(&st->s3, st->s3.leaf, &c)) {
            /* the context holds the latest compaction as a message (after the system message, when it records one);
             * the live redraw takes it out and puts its box last */
            int k = 0;
            if (after_compaction)
                while (k < c.n && !strstr(c.msg[k], "\"role\":\"compactionSummary\""))
                    k++;
            if (after_compaction && k < c.n) {
                int j;
                free(c.msg[k]);
                for (j = k; j + 1 < c.n; j++) {
                    c.msg[j] = c.msg[j + 1];
                    c.msg_n[j] = c.msg_n[j + 1];
                }
                c.n--;
            }
            pshow_replay_msgs(&p->ps, c.msg, c.msg_n, c.n);
        } else
            pshow_replay(&p->ps, p->s);
        ctx_free(&c);
        for (i = 0; i < st->s3.n; i++)
            compactions += st->s3.e[i].type == S3_COMPACTION;
    } else
        pshow_replay(&p->ps, p->s);
    if (compactions && mode == REDRAW_INITIAL) {
        char t[48];
        if (compactions == 1)
            cl_copy(t, "Session compacted 1 time", sizeof(t));
        else {
            char n[16];
            cl_ltoa(compactions, n);
            cl_copy(t, "Session compacted ", sizeof(t));
            cl_cat(t, n, sizeof(t));
            cl_cat(t, " times", sizeof(t));
        }
        pshow_note(&p->ps, PR_DIM, t);
    }
}

static void status(pi_ui *p);
static void user_text(const char *json, long n, jw *out);
static void sync_queue(pi_ui *p);

static int u_poll(void *u)
{
    pi_ui *p = (pi_ui *)u;
    if (p->look)
        pshow_tick(&p->ps);     /* a running bash call's Elapsed row */
    return ui_poll(&p->ui);
}

/* an extension's message renderer for pshow (pi_show.render) */
static int ps_render(void *u, const char *type, const char *msg, long n, const pi_ext_render *r, jw *lines)
{
    pi_ui *p = (pi_ui *)u;
    return pi_exts_render(&p->s->ext, type, msg, n, r, lines);
}

static void on_event(void *u, const pi_event *e)
{
    pi_ui *p = (pi_ui *)u;
    if (p->pst_on) {
        pi_pstatus_event(&p->pst, e);
    }
    if (p->print && !(e->type == PE_COMPACTION_END && e->is_error))
        return;                 /* print mode shows the answer, and a compaction that failed */
    switch (e->type) {
    case PE_AGENT_START:
        if (p->look)
            cl_copy(p->ui.tui->pi_work, p->xwork ? p->xwork_text : "Working", sizeof(p->ui.tui->pi_work));
        ui_busy(&p->ui, 1);
        break;
    case PE_AGENT_END:
        if (p->look)
            p->ui.tui->pi_work[0] = 0;
        ui_busy(&p->ui, 0);
        break;
    case PE_MESSAGE_START:
        p->text_index = -1;
        break;
    case PE_MESSAGE_UPDATE:
        if (e->ae->type == LE_BLOCK_START && e->ae->kind == LB_TEXT) {
            p->text_index = e->ae->index;
            if (e->ae->n > 0)
                p->render.text(p->render.u, e->ae->p, e->ae->n);
        } else if (e->ae->type == LE_DELTA && e->ae->index == p->text_index && e->ae->n > 0)
            p->render.text(p->render.u, e->ae->p, e->ae->n);
        else if (e->ae->type == LE_BLOCK_END && e->ae->index == p->text_index) {
            p->render.end(p->render.u);
            p->text_index = -1;
        }
        break;
    case PE_MESSAGE_END:
        if (e->role == PI_ROLE_CUSTOM && p->look && e->msg) {
            /* a custom message made in the run (pi.sendMessage, before_agent_start): shown when display is true (interactive-mode.ts
             * addMessageToChat "custom") */
            jv m, v, c, b, t, x;
            jit it;
            jw txt;
            char ty[200];
            ty[0] = 0;
            if (!json_parse(e->msg, e->msg_n, &m) && json_get(m, "display", &v) && json_type(v) == J_TRUE) {
                jw_init(&txt);
                if (json_get(m, "customType", &v))
                    json_str(v, ty, sizeof(ty));
                if (json_get(m, "content", &c) && json_type(c) == J_STR) {
                    long n;
                    char *z = json_strdup(c, &n);
                    if (z)
                        jw_raw(&txt, z, n);
                    free(z);
                } else if (json_get(m, "content", &c) && json_type(c) == J_ARR) {
                    int any = 0;
                    json_iter(c, &it);
                    while (json_next(&it, 0, &b))
                        if (json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                            long n;
                            char *z = json_strdup(x, &n);
                            if (z) {
                                if (any)
                                    jw_raw(&txt, "\n", 1);
                                jw_raw(&txt, z, n);
                                any = 1;
                            }
                            free(z);
                        }
                }
                pshow_custom(&p->ps, ty, txt.p ? txt.p : "", txt.n, e->msg, e->msg_n);
                jw_free(&txt);
                status(p);
            }
        }
        if (e->role == LR_USER && p->look) {
            /* the user's message is in the conversation now (a typed prompt, or a queued one delivered): it
             * leaves the waiting list and shows in the transcript */
            jw txt;
            jw_init(&txt);
            user_text(e->msg, e->msg_n, &txt);
            sync_queue(p);
            pshow_user(&p->ps, txt.p ? txt.p : "");
            jw_free(&txt);
            status(p);          /* pi draws the footer each frame: the context figure counts the message now */
        }
        if (e->role == LR_ASSISTANT && p->text_index >= 0) {
            p->render.end(p->render.u);
            p->text_index = -1;
        }
        if (e->role == LR_ASSISTANT && p->look)
            status(p);
        break;
    case PE_COMPACTION_START:
        if (p->look) {
            cl_copy(p->ui.tui->pi_work, e->stop ? "Auto-compacting... (esc to cancel)" : "Compacting context... (esc to cancel)",
                    sizeof(p->ui.tui->pi_work));
            tui_frame(p->ui.tui);
            break;
        }
        ui_line(&p->ui, "* compacting the conversation");
        break;
    case PE_COMPACTION_END:
        if (p->look) {
            p->ui.tui->pi_work[0] = 0;
            if (p->s->em.count[PE_AGENT_START] > p->s->em.count[PE_AGENT_END])
                cl_copy(p->ui.tui->pi_work, p->xwork ? p->xwork_text : "Working", sizeof(p->ui.tui->pi_work));
            if (!e->is_error && e->result && p->s->command_u) {
                jv o, v;
                if (!json_parse(e->result, e->result_n, &o) && json_get(o, "tokensBefore", &v) && json_type(v) == J_NUM) {
                    long tb = json_long(v, 0), sn = 0;
                    char *sum = json_get(o, "summary", &v) && json_type(v) == J_STR ? json_strdup(v, &sn) : 0;
                    redraw_compacted(p, tb, sum, sn);
                    free(sum);
                    break;
                }
            }
            pshow_note(&p->ps, e->is_error ? PR_ERROR : PR_MUTED, e->msg ? e->msg : "");
            break;
        }
        {
            char line[300];
            cl_copy(line, e->is_error ? "  [ERROR] " : "* ", sizeof(line));
            cl_cat(line, e->msg ? e->msg : "", sizeof(line));
            ui_line(&p->ui, line);
        }
        break;
    case PE_TOOL_START:
        if (p->look) {
            pshow_tool_start(&p->ps, e->tool_id, e->tool_name, e->args, e->args_n);
            break;
        }
        {
            char line[200];
            cl_copy(line, "* ", sizeof(line));
            cl_cat(line, e->tool_name, sizeof(line));
            cl_cat(line, " ", sizeof(line));
            if (e->args_n < 120)
                cl_cat(line, e->args, sizeof(line));
            ui_line(&p->ui, line);
        }
        break;
    case PE_TOOL_UPDATE:
        if (p->look)
            pshow_tool_update(&p->ps, e->tool_id, e->result, e->result_n);
        break;
    case PE_TOOL_END:
        if (p->look) {
            pshow_tool_end(&p->ps, e->tool_id, e->result, e->result_n, e->is_error, e->duration_ms, e->details, e->details_n);    /* pi 1.1.0: the recorded durationMs, -1 the clock */
            break;
        }
        if (e->is_error) {
            jv c, b, t;
            jit it;
            char line[240], msg[200];
            msg[0] = 0;
            if (!json_parse(e->result, e->result_n, &c)) {
                json_iter(c, &it);
                if (json_next(&it, 0, &b) && json_get(b, "text", &t))
                    json_str(t, msg, sizeof(msg));
            }
            cl_copy(line, "  [ERROR] ", sizeof(line));
            cl_cat(line, msg, sizeof(line));
            ui_line(&p->ui, line);
        }
        break;
    }
}

/* the gate's question (ext_gate.h): C:Claude's, in C:Claude's screen; in pi's look the same options as pi's selector */
static int u_ask(void *u, int tool, const char *name, const char *what, int outside)
{
    pi_ui *p = (pi_ui *)u;
    char line[360];
    if (p->look && p->ui.tui) {
        const char *opt[4];
        char title[400], yes2[80];
        int n, c;
        cl_copy(title, name, sizeof(title));
        cl_cat(title, " ", sizeof(title));
        cl_cat(title, what, sizeof(title));
        if (outside)
            cl_cat(title, "\n\nThis is outside the start directory.", sizeof(title));
        cl_copy(yes2, "Yes, and don't ask again this session", sizeof(yes2));
        opt[0] = "Yes";
        if (outside) {
            opt[1] = "No";
            n = 2;
        } else {
            opt[1] = yes2;
            opt[2] = "Yes, and don't ask again in this project";
            opt[3] = "No";
            n = 4;
        }
        (void)tool;
        if (p->pst_on)
            pi_pstatus_blocked(&p->pst, PSK_QUESTION, title);     /* an extension dialog: blocked until it is answered */
        c = tui_menu(p->ui.tui, title, 0, opt, n, 0, n - 1);
        if (p->pst_on)
            pi_pstatus_blocked(&p->pst, PSK_NONE, 0);
        if (c < 0)
            return ASK_NO;
        if (c == 0)
            return ASK_ONCE;
        if (c == n - 1)
            return ASK_NO;
        return n == 4 && c == 2 ? ASK_PROJECT : ASK_SESSION;
    }
    cl_copy(line, "* ", sizeof(line));
    cl_cat(line, name, sizeof(line));
    cl_cat(line, " ", sizeof(line));
    cl_cat(line, what, sizeof(line));
    ui_line(&p->ui, line);
    return ui_ask(&p->ui, tool, name, what, outside);
}

/* ---- ctx.ui for an extension (L5; ext.h pi_ext_ui; pi's ExtensionUIContext in interactive-mode.ts:2690-2970) ---- */

/* an extension dialog is open: the program status says the agent is blocked on it (pi: setBlocked "extension-dialog") */
static void xu_blocked(pi_ui *p, int kind, const char *title)
{
    if (p->pst_on)
        pi_pstatus_blocked(&p->pst, kind, kind == PSK_NONE ? 0 : title);
}

/* the screen without pi's look (line mode): the question printed, the answer a line. With timeout_ms the line is read in slices of the
 * console's timed read (io->read; AmigaDOS WaitForChar, the host's poll) until the time is up: -2, as pi's dialog timeout
 * (ExtensionUIDialogOptions.timeout) gives undefined; a console without one waits as long as it takes. -1 at the end of input */
static int xu_line(pi_ui *p, const char *question, char *ans, long cap, long timeout_ms)
{
    long n;
    ui_puts(&p->ui, "\033[1m");
    ui_puts(&p->ui, question);
    ui_puts(&p->ui, "\033[0m ");
    if (timeout_ms > 0 && p->ui.io->read && p->ui.io->ms) {
        unsigned long t0 = p->ui.io->ms(p->ui.io->u);
        long have = 0;
        for (;;) {
            char buf[64];
            long left = timeout_ms - (long)(p->ui.io->ms(p->ui.io->u) - t0), got, i;
            if (left <= 0) {
                p->ui.col0 = 0;
                ui_line(&p->ui, "");
                p->ui.col0 = 1;
                return -2;
            }
            got = p->ui.io->read(p->ui.io->u, buf, sizeof(buf), left > 100 ? 100 : left);
            if (got < 0)
                return -1;
            if (p->ui.io->brk && p->ui.io->brk(p->ui.io->u))
                return -1;
            for (i = 0; i < got; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    ans[have] = 0;
                    p->ui.col0 = 1;
                    return (int)have;
                }
                if (have < cap - 1)
                    ans[have++] = buf[i];
            }
        }
    }
    n = p->ui.io->read_line(p->ui.io->u, ans, cap);
    p->ui.col0 = 1;
    return n < 0 ? -1 : (int)n;
}

static int xu_select(void *u, const char *title, const char *const *opt, int n, long timeout_ms, char *value, long cap)
{
    pi_ui *p = (pi_ui *)u;
    int c;
    if (p->look && p->ui.tui) {
        xu_blocked(p, PSK_QUESTION, title);
        c = tui_ext_menu(p->ui.tui, title, opt, n, timeout_ms);
        xu_blocked(p, PSK_NONE, 0);
        if (c < 0)
            return 0;               /* Escape, Ctrl+C, the end of input, or the timeout: pi's undefined */
    } else {
        char ans[40], num[16];
        int i;
        ui_line(&p->ui, title);
        for (i = 0; i < n; i++) {
            cl_ltoa(i + 1, num);
            cl_copy(ans, num, sizeof(ans));
            cl_cat(ans, ") ", sizeof(ans));
            ui_puts(&p->ui, ans);
            ui_line(&p->ui, opt[i]);
        }
        c = xu_line(p, "Number (empty cancels):", ans, sizeof(ans), timeout_ms);
        if (c <= 0)
            return 0;
        c = atoi(ans) - 1;
        if (c < 0 || c >= n)
            return 0;
    }
    cl_copy(value, opt[c], cap);
    return 1;
}

/* confirm is pi's selector over "Yes" and "No" with the message under the title (interactive-mode.ts:2750-2760) */
static int xu_confirm(void *u, const char *title, const char *message, long timeout_ms)
{
    pi_ui *p = (pi_ui *)u;
    static const char *const yn[] = { "Yes", "No" };
    char both[800];
    int c;
    cl_copy(both, title, sizeof(both));
    cl_cat(both, "\n", sizeof(both));
    cl_cat(both, message, sizeof(both));
    if (p->look && p->ui.tui) {
        xu_blocked(p, PSK_PERMISSION, title);
        c = tui_ext_menu(p->ui.tui, both, yn, 2, timeout_ms);
        xu_blocked(p, PSK_NONE, 0);
        return c == 0;
    } else {
        char ans[16];
        ui_line(&p->ui, both);
        return xu_line(p, "Yes (y) or No (n):", ans, sizeof(ans), timeout_ms) > 0 && (ans[0] == 'y' || ans[0] == 'Y');
    }
}

static int xu_input(void *u, const char *title, const char *placeholder, long timeout_ms, char *value, long cap)
{
    pi_ui *p = (pi_ui *)u;
    (void)placeholder;              /* ExtensionInputComponent ignores it too (extension-input.ts, _placeholder) */
    if (p->look && p->ui.tui) {
        int r;
        xu_blocked(p, PSK_QUESTION, title);
        r = tui_ext_input(p->ui.tui, title, value, cap, timeout_ms);
        xu_blocked(p, PSK_NONE, 0);
        return r == 1;
    }
    ui_line(&p->ui, title);
    return xu_line(p, ">", value, cap, timeout_ms) >= 0;
}

/* showExtensionNotify: info is showStatus, warning and error their own rows with pi's prefixes */
static void xu_notify(void *u, const char *message, const char *type)
{
    pi_ui *p = (pi_ui *)u;
    char line[600];
    int role = PR_DIM;
    if (type && !strcmp(type, "error")) {
        cl_copy(line, "Error: ", sizeof(line));
        cl_cat(line, message, sizeof(line));
        if (p->look)
            pshow_error(&p->ps, line);
        else
            ui_line(&p->ui, line);
        return;
    }
    if (type && !strcmp(type, "warning")) {
        cl_copy(line, "Warning: ", sizeof(line));
        cl_cat(line, message, sizeof(line));
        message = line;
        role = PR_WARNING;
    }
    if (p->look)
        pshow_note(&p->ps, role, message);
    else
        ui_line(&p->ui, message);
}

/* the extension statuses as footer.ts draws them: sorted by key (localeCompare), line breaks and tabs spaces, runs of spaces one, trimmed,
 * joined by a space */
static int xu_keycmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static void xu_refresh(pi_ui *p)
{
    char line[240];
    int i, j, order[XU_KEYS];
    jw above, below;
    int cut_a = 0, cut_b = 0;
    if (!p->look || !p->ui.tui)
        return;
    for (i = 0; i < p->nxstat; i++)
        order[i] = i;
    for (i = 1; i < p->nxstat; i++)
        for (j = i; j > 0 && xu_keycmp(p->xstat[order[j - 1]].key, p->xstat[order[j]].key) > 0; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    line[0] = 0;
    for (i = 0; i < p->nxstat; i++) {
        const char *t = p->xstat[order[i]].text;
        char one[240];
        long k = 0;
        int sp = 1;
        for (; *t && k < (long)sizeof(one) - 1; t++) {
            char c = *t == '\r' || *t == '\n' || *t == '\t' ? ' ' : *t;
            if (c == ' ' && sp)
                continue;
            sp = c == ' ';
            one[k++] = c;
        }
        while (k && one[k - 1] == ' ')
            k--;
        one[k] = 0;
        if (line[0])
            cl_cat(line, " ", sizeof(line));
        cl_cat(line, one, sizeof(line));
    }
    tui_pi_ext_status(p->ui.tui, line);
    jw_init(&above);
    jw_init(&below);
    for (i = 0; i < p->nxwid; i++) {
        jw *to = p->xwid[i].below ? &below : &above;
        if (p->xwid[i].n > XU_MAX_WIDGET_LINES) {
            if (p->xwid[i].below)
                cut_b = 1;
            else
                cut_a = 1;
        }
        jw_rawz(to, p->xwid[i].lines);
    }
    tui_pi_widgets(p->ui.tui, above.n ? above.p : 0, cut_a, below.n ? below.p : 0, cut_b);
    jw_free(&above);
    jw_free(&below);
    tui_frame(p->ui.tui);
}

static void xu_status(void *u, const char *key, const char *text)
{
    pi_ui *p = (pi_ui *)u;
    int i;
    for (i = 0; i < p->nxstat && strcmp(p->xstat[i].key, key); i++)
        ;
    if (!text) {
        if (i < p->nxstat) {
            free(p->xstat[i].text);
            p->xstat[i] = p->xstat[--p->nxstat];
        }
    } else {
        char *c = (char *)malloc(strlen(text) + 1);
        if (!c)
            return;
        strcpy(c, text);
        if (i == p->nxstat) {
            if (p->nxstat == XU_KEYS) {
                free(c);
                return;
            }
            p->nxstat++;
            cl_copy(p->xstat[i].key, key, sizeof(p->xstat[i].key));
        } else
            free(p->xstat[i].text);
        p->xstat[i].text = c;
    }
    xu_refresh(p);
}

static void xu_widget(void *u, const char *key, const char *const *lines, int n, const char *placement)
{
    pi_ui *p = (pi_ui *)u;
    int i;
    for (i = 0; i < p->nxwid && strcmp(p->xwid[i].key, key); i++)
        ;
    if (i < p->nxwid) {
        free(p->xwid[i].lines);         /* the widgets keep the order they were set in (a Map's insertion order) */
        memmove(&p->xwid[i], &p->xwid[i + 1], sizeof(p->xwid[0]) * (size_t)(p->nxwid - i - 1));
        p->nxwid--;
    }
    if (n >= 0) {
        jw w;
        int k, cap = n > XU_MAX_WIDGET_LINES ? XU_MAX_WIDGET_LINES : n;
        if (p->nxwid == XU_KEYS)
            return;
        jw_init(&w);
        for (k = 0; k < cap; k++) {
            jw_rawz(&w, lines[k]);
            jw_rawz(&w, "\n");
        }
        if (w.n)
            w.n--;                                  /* no line feed after the last line */
        i = p->nxwid++;
        cl_copy(p->xwid[i].key, key, sizeof(p->xwid[i].key));
        p->xwid[i].lines = (char *)malloc((size_t)w.n + 1);
        if (p->xwid[i].lines) {
            memcpy(p->xwid[i].lines, w.p ? w.p : "", (size_t)w.n);
            p->xwid[i].lines[w.n] = 0;
        }
        p->xwid[i].n = n;
        p->xwid[i].below = placement && !strcmp(placement, "belowEditor");
        jw_free(&w);
    }
    xu_refresh(p);
}

static void xu_title(void *u, const char *title)
{
    pi_ui *p = (pi_ui *)u;
    if (p->look && p->ui.tui)
        tui_title(p->ui.tui, title);
}

static void xu_working(void *u, const char *message)
{
    pi_ui *p = (pi_ui *)u;
    if (p->look && p->ui.tui) {
        p->xwork = message != 0;
        cl_copy(p->xwork_text, message ? message : "", sizeof(p->xwork_text));
        if (p->ui.tui->pi_work[0]) {                    /* the agent is working: the label changes now */
            cl_copy(p->ui.tui->pi_work, message ? message : "Working", sizeof(p->ui.tui->pi_work));
            tui_frame(p->ui.tui);
        }
    }
}

static pi_ext_ui xu_ui = { 0, xu_select, xu_confirm, xu_input, xu_notify, xu_status, xu_widget, xu_title, 0, xu_working, 0 };

/* a user message's text blocks, joined */
static void user_text(const char *json, long n, jw *out)
{
    jv m, c, b, t, x;
    jit it;
    int any = 0;
    if (json_parse(json, n, &m) || !json_get(m, "content", &c))
        return;
    if (json_type(c) == J_STR) {
        long l;
        char *z = json_strdup(c, &l);
        if (z)
            jw_raw(out, z, l);
        free(z);
        return;
    }
    if (json_type(c) != J_ARR)
        return;
    json_iter(c, &it);
    while (json_next(&it, 0, &b))
        if (json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
            long l;
            char *z = json_strdup(x, &l);
            if (z) {
                if (any)
                    jw_raw(out, "\n", 1);
                jw_raw(out, z, l);
                any = 1;
            }
            free(z);
        }
    jw_rawz(out, "");
}

/* the screen's waiting list mirrors the session's queues (the strings stay the session's) */
static void sync_queue(pi_ui *p)
{
    cl_tui *t = p->ui.tui;
    int i;
    if (!t)
        return;
    t->n_steer = p->s->nsteer;
    t->n_follow = p->s->nfollow;
    for (i = 0; i < p->s->nsteer && i < TUI_QUEUE; i++)
        t->pi_steer[i] = p->s->steer[i];
    for (i = 0; i < p->s->nfollow && i < TUI_QUEUE; i++)
        t->pi_follow[i] = p->s->follow[i];
}

/* Enter (steering) and Alt+Enter (follow-up) while the agent works */
static int cb_queue(void *u, const char *text, int follow)
{
    pi_ui *p = (pi_ui *)u;
    if (follow ? pi_follow_up(p->s, text) : pi_steer(p->s, text))
        return 0;
    sync_queue(p);
    tui_frame(p->ui.tui);
    return 1;
}

/* Alt+Up, and Esc while the agent works: the queued messages back to the editor (steering first) */
static char *cb_dequeue(void *u, int *count, int quiet)
{
    pi_ui *p = (pi_ui *)u;
    char *q[2 * PI_QUEUE], *r;
    int n = pi_queue_take(p->s, q, 2 * PI_QUEUE), i;
    jw w;
    jw_init(&w);
    for (i = 0; i < n; i++) {
        if (i)
            jw_rawz(&w, "\n\n");
        jw_rawz(&w, q[i]);
        free(q[i]);
    }
    *count = n;
    sync_queue(p);
    if (!quiet) {
        if (!n)
            pshow_note(&p->ps, PR_DIM, "No queued messages to restore");
        else {
            char m[80], num[16];
            cl_copy(m, "Restored ", sizeof(m));
            cl_ltoa(n, num);
            cl_cat(m, num, sizeof(m));
            cl_cat(m, n > 1 ? " queued messages to editor" : " queued message to editor", sizeof(m));
            pshow_note(&p->ps, PR_DIM, m);
        }
    }
    if (!n) {
        jw_free(&w);
        return 0;
    }
    jw_rawz(&w, "");
    r = (char *)malloc((size_t)w.n + 1);
    if (r)
        memcpy(r, w.p, (size_t)w.n + 1);
    jw_free(&w);
    return r;
}

/* Ctrl+P / Shift+Ctrl+P and Shift+Tab: the session cycles, the line pi would show goes in the transcript */
static void cb_cycle(void *u, int what, int dir)
{
    pi_ui *p = (pi_ui *)u;
    char msg[240];
    int rc;
    if (!p->s->cycle)
        return;
    msg[0] = 0;
    rc = p->s->cycle(p->s->cycle_u, p->s, what, dir, msg, sizeof(msg));
    if (rc < 0)
        pshow_error(&p->ps, msg);
    else
        pshow_note(&p->ps, PR_DIM, msg);
    status(p);
}

/* app.tools.expand (Ctrl+O): pi's toggleToolOutputExpansion (interactive-mode.ts:4557-4574) flips toolOutputExpanded, sets it
 * on every expandable child of the chat and shows "Tool output: expanded|collapsed". The chat's items (show_pi.h) are drawn again
 * with the new setting, so what is not in the session store (an error, a command's output) stays and a tool that runs stays live. */
static void cb_expand(void *u)
{
    pi_ui *p = (pi_ui *)u;
    pshow_expand(&p->ps, !p->ps.expanded, p->ps.expanded ? "Tool output: collapsed" : "Tool output: expanded");
    status(p);
}

static void attach(pi_ui *p, pi_session *s, cl_io *io, int print)
{
    memset(p, 0, sizeof(*p));
    ui_init(&p->ui, io);
    ui_plain(&p->ui, &p->render);
    p->s = s;
    p->print = print;
    p->text_index = -1;
    pi_subscribe(&s->em, on_event, p);
    if (!print) {
        s->h.u = p;
        s->h.poll = u_poll;
        s->ask = u_ask;
        s->ask_u = p;
        xu_ui.u = p;
        s->ext.ui = &xu_ui;     /* ctx.hasUI is true and ctx.ui reaches this screen (print mode has no user: hasUI false) */
    } else {
        s->ask = 0;             /* print mode: nobody answers the gate */
        s->ext.ui = 0;          /* nor an extension's dialogs: ctx.hasUI is false */
    }
}

/* pi's formatTokens */
static void fmt_tokens(long v, char *out)
{
    char n[16];
    if (v < 1000) {
        cl_ltoa(v, out);
    } else if (v < 10000) {
        cl_ltoa(v / 1000, out);
        cl_cat(out, ".", 16);
        cl_ltoa((v % 1000 + 50) / 100 % 10, n);
        /* toFixed(1) rounds 9.95k to 10.0k: carry */
        if ((v % 1000 + 50) / 100 >= 10) {
            cl_ltoa(v / 1000 + 1, out);
            cl_cat(out, ".0", 16);
        } else
            cl_cat(out, n, 16);
        cl_cat(out, "k", 16);
    } else if (v < 1000000) {
        cl_ltoa((v + 500) / 1000, out);
        cl_cat(out, "k", 16);
    } else if (v < 10000000) {
        cl_ltoa(v / 1000000, out);
        cl_cat(out, ".", 16);
        cl_ltoa((v % 1000000 + 50000) / 100000 % 10, n);
        cl_cat(out, n, 16);
        cl_cat(out, "M", 16);
    } else {
        cl_ltoa((v + 500000) / 1000000, out);
        cl_cat(out, "M", 16);
    }
}

/* the footer (pi footer.js): the directory, the usage of the session so far, the context figure, the model */
/* pi's usage totals: every entry of the session file, other branches too (footer.js getSessionStats) */
static void totals(pi_ui *p, long *in, long *out, long *cr, long *cw, long *cost)
{
    pi_store *st = (pi_store *)p->s->command_u;
    int i;
    *in = p->s->tok_in;
    *out = p->s->tok_out;
    *cr = p->s->tok_cr;
    *cw = p->s->tok_cw;
    *cost = p->s->cost_micro;
    if (!st || !p->s->command)
        return;
    *in = *out = *cr = *cw = *cost = 0;
    for (i = 0; i < st->s3.n; i++) {
        long n;
        const char *tx;
        jv o, m, u;
        llm_usage us;
        int ty = st->s3.e[i].type;
        if (ty != S3_MESSAGE && ty != S3_USAGE && ty != S3_COMPACTION && ty != S3_BRANCH_SUMMARY)
            continue;
        if (ty == S3_MESSAGE && st->s3.e[i].role != S3R_ASSISTANT)
            continue;
        tx = s3_text(&st->s3, i, &n);
        if (!tx || json_parse(tx, n, &o))
            continue;
        if (ty == S3_MESSAGE) {
            if (!json_get(o, "message", &m) || !json_get(m, "usage", &u))
                continue;
        } else if (!json_get(o, "usage", &u))
            continue;
        llm_r_usage(u, &us);
        *in += us.input;
        *out += us.output;
        *cr += us.cache_read;
        *cw += us.cache_write;
        *cost += us.cost[0] + us.cost[1] + us.cost[2] + us.cost[3];
    }
}

/* the window title as pi's updateTerminalTitle sets it: "pi - <session name> - <directory name>", or
 * "pi - <directory name>" with no name (pi's APP_TITLE is U+03C0, which the console's Latin-1 stand-in shows as "p": spelled "pi" here, T2). Set at the start and with every status. */
void pi_dir_name(const char *path, char *out, int n)
{
    int end = (int)strlen(path), start;
    while (end > 1 && path[end - 1] == '/')
        end--;
    if (end > 1 && path[end - 1] == ':')
        end--;
    for (start = end; start > 0 && path[start - 1] != '/' && path[start - 1] != ':'; start--)
        ;
    if (n <= 0)
        return;
    if (end - start > n - 1)
        end = start + n - 1;
    memcpy(out, path + start, (size_t)(end - start));
    out[end - start] = 0;
}

static void window_title(pi_ui *p, cl_tui *t)
{
    pi_store *st = (pi_store *)p->s->command_u;
    char nm[128], base[128];
    pi_dir_name(ui_cwd, base, sizeof(base));
    cl_copy(t->pi_title, "pi - ", sizeof(t->pi_title));
    if (st && p->s->command && s3_name(&st->s3, nm, sizeof(nm))) {
        cl_cat(t->pi_title, nm, sizeof(t->pi_title));
        cl_cat(t->pi_title, " - ", sizeof(t->pi_title));
    }
    cl_cat(t->pi_title, base, sizeof(t->pi_title));
    tui_title(t, t->pi_title);
}

static void footer(pi_ui *p)
{
    cl_tui *t = p->ui.tui;
    pi_session *s = p->s;
    char n[24], *l = t->pi_left;
    long tenths, t_in, t_out, t_cr, t_cw, t_cost;
    int level = s->model.reasoning ? s->thinking : 0;
    totals(p, &t_in, &t_out, &t_cr, &t_cw, &t_cost);
    cl_copy(t->pi_cwd, ui_cwd, sizeof(t->pi_cwd));
    window_title(p, t);
    l[0] = 0;
    if (t_in) {
        cl_cat(l, "\342\206\221", sizeof(t->pi_left));
        fmt_tokens(t_in, n);
        cl_cat(l, n, sizeof(t->pi_left));
    }
    if (t_out) {
        cl_cat(l, l[0] ? " \342\206\223" : "\342\206\223", sizeof(t->pi_left));
        fmt_tokens(t_out, n);
        cl_cat(l, n, sizeof(t->pi_left));
    }
    if (t_cr) {
        cl_cat(l, l[0] ? " R" : "R", sizeof(t->pi_left));
        fmt_tokens(t_cr, n);
        cl_cat(l, n, sizeof(t->pi_left));
    }
    if (t_cw) {
        cl_cat(l, l[0] ? " W" : "W", sizeof(t->pi_left));
        fmt_tokens(t_cw, n);
        cl_cat(l, n, sizeof(t->pi_left));
    }
    if (t_cost > 0) {
        /* toFixed(3) on a sum of floats: a tie (exactly half a thousandth) goes to the even digit */
        long milli = t_cost / 1000, rest = t_cost % 1000;
        if (rest > 500 || (rest == 500 && (milli & 1)))
            milli++;
        cl_cat(l, l[0] ? " $" : "$", sizeof(t->pi_left));
        cl_ltoa(milli / 1000, n);
        cl_cat(l, n, sizeof(t->pi_left));
        cl_cat(l, ".", sizeof(t->pi_left));
        cl_ltoa(milli % 1000 / 100, n);
        cl_cat(l, n, sizeof(t->pi_left));
        cl_ltoa(milli % 100 / 10, n);
        cl_cat(l, n, sizeof(t->pi_left));
        cl_ltoa(milli % 10, n);
        cl_cat(l, n, sizeof(t->pi_left));
    }
    t->pi_ctx_role = 0;
    {
        /* pi's getContextUsage over the session (the projected estimate: the last answer's usage and what followed,
         * else the current system message, its tools and the messages), "?" after a compaction until an answer;
         * a session without a store (the screen tests' bare agent) estimates its messages. No window: pi's 0.0%/0 */
        pi_store *st = (pi_store *)s->command_u;
        long ctx = 0;
        tenths = 0;
        if (s->model.context_window > 0) {
            if (st && s->command) {
                pi_ctx c;
                ctx_init(&c);
                ctx = ctx_build(&st->s3, st->s3.leaf, &c) ? 0 : cp_context_usage(&st->s3, st->s3.leaf, &c);
                ctx_free(&c);
            } else if (s->nmsg) {
                int last;
                ctx = cp_estimate_context(s->msg, s->msg_n, s->nmsg, &last);
            }
            if (ctx >= 0)
                tenths = (ctx * 1000 + s->model.context_window / 2) / s->model.context_window;
        }
        if (ctx < 0)
            cl_copy(t->pi_ctx, "?", sizeof(t->pi_ctx));
        else {
            cl_ltoa(tenths / 10, t->pi_ctx);
            cl_cat(t->pi_ctx, ".", sizeof(t->pi_ctx));
            cl_ltoa(tenths % 10, n);
            cl_cat(t->pi_ctx, n, sizeof(t->pi_ctx));
            cl_cat(t->pi_ctx, "%", sizeof(t->pi_ctx));
        }
        cl_cat(t->pi_ctx, "/", sizeof(t->pi_ctx));
        fmt_tokens(s->model.context_window > 0 ? s->model.context_window : 0, n);
        cl_cat(t->pi_ctx, n, sizeof(t->pi_ctx));
        cl_cat(t->pi_ctx, " (auto)", sizeof(t->pi_ctx));
        if (tenths > 900)
            t->pi_ctx_role = PR_ERROR + 1;
        else if (tenths > 700)
            t->pi_ctx_role = PR_WARNING + 1;
    }
    cl_copy(t->pi_right, s->model.id, sizeof(t->pi_right));
    if (s->model.reasoning) {
        cl_cat(t->pi_right, " \342\200\242 ", sizeof(t->pi_right));
        cl_cat(t->pi_right, level == LT_OFF ? "thinking off" : llm_think_name(level), sizeof(t->pi_right));
    }
    t->pi_rule = PR_THINKING_OFF + level;
}

/* the status line: the model, the thinking level, the context left of the model's window (the last answer's tokens) */
static void status(pi_ui *p)
{
    cl_tui *t = p->ui.tui;
    pi_session *s = p->s;
    if (!t)
        return;
    t->model = s->model.id;
    t->effort = s->model.reasoning ? llm_think_name(s->thinking) : 0;
    if (s->model.context_window > 0 && s->ctx_tokens > 0) {
        long used = s->ctx_tokens * 100 / s->model.context_window;
        t->ctx_left = used >= 100 ? 0 : (int)(100 - used);
    } else
        t->ctx_left = -1;
    if (p->look)
        footer(p);
    tui_frame(t);
}

static void say_end(pi_ui *p)
{
    char line[300];
    if (p->look && (p->s->nsteer || p->s->nfollow)) {
        /* the run ended (aborted, an error) with messages still waiting: they go back to the editor */
        int n;
        char *q = cb_dequeue(p, &n, 1);
        if (q) {
            tui_set_text(p->ui.tui, q);
            free(q);
        }
    }
    status(p);
    if (p->s->stop != LS_ERROR && p->s->stop != LS_ABORTED)
        return;
    if (p->look) {
        if (p->s->stop == LS_ABORTED)
            pshow_error(&p->ps, "Operation aborted");
        else {
            cl_copy(line, "Error: ", sizeof(line));
            cl_cat(line, p->s->err[0] ? p->s->err : "the request failed", sizeof(line));
            pshow_error(&p->ps, line);
        }
        return;
    }
    cl_copy(line, p->s->stop == LS_ABORTED ? "[INFO] Stopped." : "[ERROR] ", sizeof(line));
    if (p->s->stop == LS_ERROR)
        cl_cat(line, p->s->err[0] ? p->s->err : "the request failed", sizeof(line));
    ui_line(&p->ui, line);
}

int pi_ui_print(pi_session *s, cl_io *io, const char *prompt)
{
    static pi_ui p;
    jw t;
    int stop;
    attach(&p, s, io, 1);
    stop = pi_prompt(s, prompt);
    jw_init(&t);
    pi_last_text(s, &t);
    if (t.n) {
        io->write(io->u, t.p, t.n);
        if (t.p[t.n - 1] != '\n')
            io->write(io->u, "\n", 1);
    }
    jw_free(&t);
    p.print = 0;
    say_end(&p);
    /* print-mode.ts: the exit code is 1 only when the last answer is an error or was aborted -- a prompt an extension handled (a
     * command, the input event) ran no answer and exits 0 */
    return stop == LS_STOP || (stop == LS_NONE && !s->err[0]) ? 0 : 1;
}

/* C:Claude's screen without its REPL (claude/repl.c repl_screen's part that is the screen's), in pi's look */
/* pi's autocomplete rows for the commands extensions registered (interactive-mode.ts:793-802): the invocation name with
 * "[u] description" ("[u]" alone without one; u user, p project, t temporary: getAutocompleteSourceTag); a command named like
 * a built-in one is left out. The rows and their strings are malloc'ed (the caller frees both); the count, 0 and nothing
 * allocated when there are none. */
int pi_ui_ext_menu(const pi_exts *x, cl_cmd **menu, char **menu_text)
{
    pi_ext_rcmd *r = (pi_ext_rcmd *)malloc(sizeof(*r) * PI_EXT_CMDS);
    int n = r ? pi_exts_commands(x, r, PI_EXT_CMDS) : 0, i, k = 0, j;
    long at = 0, text = 0;
    cl_cmd *rows = 0;
    char *txt = 0;
    *menu = 0;
    *menu_text = 0;
    for (i = 0; i < n; i++)
        text += (long)strlen(r[i].invocation) + (r[i].c->description ? (long)strlen(r[i].c->description) : 0) + 16;
    if (n) {
        rows = (cl_cmd *)malloc(sizeof(cl_cmd) * (size_t)n);
        txt = (char *)malloc((size_t)text);
    }
    if (!rows || !txt) {
        free(rows);
        free(txt);
        free(r);
        return 0;
    }
    for (i = 0; i < n; i++) {
        const char *scope = r[i].c->owner->scope;
        char *name = txt + at, *help;
        int builtin = 0;
        for (j = 0; j < ui_ncmds; j++)
            if (ui_cmds[j].name[0] == '/' && !strcmp(ui_cmds[j].name + 1, r[i].c->name))
                builtin = 1;
        if (builtin)
            continue;
        name[0] = '/';
        strcpy(name + 1, r[i].invocation);
        help = name + strlen(name) + 1;
        strcpy(help, scope && !strcmp(scope, "user") ? "[u]" : scope && !strcmp(scope, "project") ? "[p]" : "[t]");
        if (r[i].c->description) {
            strcat(help, " ");
            strcat(help, r[i].c->description);
        }
        rows[k].name = name;
        rows[k].help = help;
        k++;
        at = (long)(help + strlen(help) + 1 - txt);
    }
    free(r);
    if (!k) {
        free(rows);
        free(txt);
        return 0;
    }
    *menu = rows;
    *menu_text = txt;
    return k;
}

/* the rows of pi_ui_ext_menu as slot 1 of the extra commands (after the templates, before the skills); freed when the run ends */
static void build_ext_rows(pi_ui *p)
{
    int k;
    pi_ui_set_extra_commands(1, 0, 0);
    free(p->xmenu);
    free(p->xmenu_text);
    k = pi_ui_ext_menu(&p->s->ext, &p->xmenu, &p->xmenu_text);
    pi_ui_set_extra_commands(1, p->xmenu, k);
}

/* pi's autocomplete rows for skills (interactive-mode.ts:804-822): skill:<name> with "[u] description", registered as slot 3 of the
 * extra commands (after the templates and extensions, pi's order); freed when the run ends */
static void build_skill_rows(pi_ui *p)
{
    const pi_skills *k = p->s->res ? &p->s->res->skills : 0;
    long text = 0, at = 0;
    int i;
    pi_ui_set_extra_commands(3, 0, 0);
    free(p->menu);
    free(p->menu_text);
    p->menu = 0;
    p->menu_text = 0;
    if (!k || !k->n || !k->commands)
        return;
    for (i = 0; i < k->n; i++)
        text += (long)strlen(k->s[i].name) + (long)strlen(k->s[i].desc) + 64;
    p->menu = (cl_cmd *)malloc(sizeof(cl_cmd) * (size_t)k->n);
    p->menu_text = (char *)malloc((size_t)text);
    if (!p->menu || !p->menu_text) {
        free(p->menu);
        free(p->menu_text);
        p->menu = 0;
        p->menu_text = 0;
        return;
    }
    for (i = 0; i < k->n; i++) {
        char *name = p->menu_text + at, *desc;
        long nl = (long)strlen(k->s[i].name) + 16, dl = (long)strlen(k->s[i].desc) + 8;
        desc = name + nl;
        pi_skill_command(&k->s[i], name + 1, nl - 1, desc, dl);
        name[0] = '/';
        p->menu[i].name = name;
        p->menu[i].help = desc;
        at += nl + dl;
    }
    pi_ui_set_extra_commands(3, p->menu, k->n);
}

static cl_tui *screen(pi_ui *p, cl_io *io)
{
    cl_tui *t;
    cl_show *sh;
    if (!io->read || !io->raw)
        return 0;
    t = (cl_tui *)malloc(sizeof(cl_tui));
    sh = (cl_show *)malloc(sizeof(cl_show));
    if (!t || !sh || tui_init(t, io)) {
        if (t)
            tui_free(t);
        free(t);
        free(sh);
        return 0;
    }
    t->model = p->s->model.id;
    t->effort = p->s->model.reasoning ? llm_think_name(p->s->thinking) : 0;
    if (ui_have_theme)
        p->theme = ui_theme;
    else
        pitheme_system(&p->theme);
    t->look = TUI_LOOK_PI;
    t->screen = ui_regular ? TUI_SCREEN_REGULAR : TUI_SCREEN_FULL;
    t->exit_hint_only = ui_hint_only;
    t->scrollbar = ui_bar;
    t->pith = &p->theme;
    build_ext_rows(p);
    build_skill_rows(p);
    t->cmds = ui_menu(p, &t->ncmds);
    t->slash_filter = pi_slash_filter;      /* pi-tui's fuzzy filter and order, not C:Claude's prefix match (autocomplete.ts:343-372) */
    p->look = 1;
    show_init(sh, t);
    window_title(p, t);         /* before the start, which writes it */
    if (tui_start(t)) {
        show_free(sh);
        tui_free(t);
        free(t);
        free(sh);
        p->look = 0;
        return 0;
    }
    p->ui.tui = t;
    p->ui.show = sh;
    t->on_queue = cb_queue;
    t->on_dequeue = cb_dequeue;
    t->on_cycle = cb_cycle;
    t->on_expand = cb_expand;
    t->qu = p;
    km_free(&t->km);
    if (ui_kb)
        km_load_pi(&t->km, ui_kb, ui_kb_n);
    else
        km_init_pi(&t->km);
    pshow_init(&p->ps, t, &p->theme);
    p->ps.render = ps_render;
    p->ps.render_u = p;
    pshow_set_pad(&p->ps, ui_pad);
    pshow_set_root(&p->ps, ui_cwd);
    pshow_render(&p->ps, &p->render);
    footer(p);
    tui_frame(t);
    return t;
}

/* a slash command the store knows: its lines shown, a message taken back kept for the next input.
 * 1 when handled */
static char prefill[4096];

static void note(pi_ui *p, const char *a)
{
    if (p->look)
        pshow_note(&p->ps, PR_MUTED, a);
    else
        ui_line(&p->ui, a);
}

static int tree_filter;

/* /tree in pi's look: the selector, pi's "Summarize branch?" question, the move, the conversation drawn again */
static void tree_command(pi_ui *p, cl_tui *t)
{
    pi_store *st = (pi_store *)p->s->command_u;
    int sel = -1;
    if (!st->s3.n) {
        pshow_note(&p->ps, PR_DIM, "No entries in session");
        return;
    }
    for (;;) {
        static const char *const opt[3] = { "No summary", "Summarize", "Summarize with custom prompt" };
        int idx = pi_tree_pick(st, t, sel, &tree_filter), choice = 0, back = 0;
        char instr[300];
        jw reply, editor;
        instr[0] = 0;
        if (idx < 0)
            break;
        if (idx == st->s3.leaf) {
            pshow_note(&p->ps, PR_DIM, "Already at this point");
            break;
        }
        for (;;) {
            choice = tui_menu(t, "Summarize branch?", 0, opt, 3, 0, -1);
            if (choice < 0) {
                back = 1;
                break;
            }
            if (choice == 2) {
                int r = tui_line_input(t, " Custom summarization instructions", instr, sizeof(instr));
                if (r != 1)
                    continue;
            }
            break;
        }
        if (back) {
            sel = idx;
            continue;
        }
        jw_init(&reply);
        jw_init(&editor);
        pi_tree_navigate(st, idx, choice > 0, instr[0] ? instr : 0, choice == 2, &reply, &editor);
        if (reply.p && strstr(reply.p, "[ERROR]"))
            pshow_error(&p->ps, reply.p + 8);
        else {
            pshow_clear(&p->ps);
            redraw_session(p);
            if (editor.n && !t->ed.n)
                tui_set_text(t, editor.p);
            pshow_note(&p->ps, PR_DIM, "Navigated to selected point");
        }
        jw_free(&reply);
        jw_free(&editor);
        break;
    }
    status(p);
}

/* ! runs a command and puts its output into the conversation, !! keeps it out (pi: executeBash). 1 when handled */
static int bang(pi_ui *p, const char *line)
{
    pi_session *s = p->s;
    int excl = line[1] == '!', code = 0, stopped = 0, by_ext = 0;
    const char *cmd = line + (excl ? 2 : 1);
    jw text;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (!*cmd)
        return 0;
    jw_init(&text);
    if (pi_bash_user(s, cmd, excl, &text, &code, &stopped, &by_ext)) {
        pshow_error(&p->ps, "The bash tool is not available");
        jw_free(&text);
        return 1;
    }
    if (s->command_u && s->command)
        pi_store_bash((pi_store *)s->command_u, cmd, text.p ? text.p : "", code, stopped, excl);
    pshow_bash(&p->ps, cmd, text.p ? text.p : "", code, stopped, excl);
    jw_free(&text);
    status(p);
    return 1;
}

/* /login and /logout (login.h): pi's dialogs as menus and lines. In pi's look the menus are its selector frames and the
 * lines its one-line input; in the line mode numbered lists and plain prompts */
typedef struct lg {
    pi_ui *p;
    cl_tui *t;
} lg;

static void lg_note(void *u, int kind, const char *text)
{
    lg *l = (lg *)u;
    static const int role[] = { PR_TEXT, PR_DIM, PR_WARNING, PR_ERROR, PR_ACCENT, PR_SUCCESS };
    if (l->p->look && kind == PL_ERROR) {
        char line[1200];                            /* showError: "Error: " and the message */
        cl_copy(line, "Error: ", sizeof(line));
        cl_cat(line, text, sizeof(line));
        pshow_error(&l->p->ps, line);
    } else if (l->p->look)
        pshow_note(&l->p->ps, role[kind], text);
    else
        ui_line(&l->p->ui, text);
}

/* the provider selector (oauth-selector.ts OAuthSelectorComponent) over the screen's picker: the rows the login code gave, the
 * search box's text filtering them as pi's fuzzyFilter does */
typedef struct lgpick {
    lg *l;
    const pi_login_row *rows;
    int n, show_type;
    int *vis;                   /* the rows matching the search, in order of score */
    long *score;
    tui_pick pk;
} lgpick;

static void lgp_filter(lgpick *k)
{
    int i, m = 0, j;
    const char *q = k->pk.query;
    for (i = 0; i < k->n; i++) {
        long sc;
        if (!q[0] || pi_fuzzy_item(q, (long)strlen(q), k->rows[i].search, (long)strlen(k->rows[i].search), &sc)) {
            if (!q[0])
                sc = 0;
            j = m++;
            while (j > 0 && k->score[j - 1] > sc) {      /* Array.sort is stable: ties stay in row order */
                k->vis[j] = k->vis[j - 1];
                k->score[j] = k->score[j - 1];
                j--;
            }
            k->vis[j] = i;
            k->score[j] = sc;
        }
    }
    k->pk.n = m;
    if (k->pk.sel > m - 1)
        k->pk.sel = m ? m - 1 : 0;
}

/* a row: the arrow and the name in the accent colour when selected, the type in muted, the status (formatAuthSelectorProviderStatus) */
static void lgp_row(void *u, int i, int selected, char *out, long cap)
{
    lgpick *k = (lgpick *)u;
    const pi_login_row *r = &k->rows[k->vis[i]];
    const cl_pitheme *th = &k->l->p->theme;
    char acc[48], txt[48], mut[48], suc[48], warn[48];
    pitheme_sgr(th, PR_ACCENT, acc);
    pitheme_sgr(th, PR_TEXT, txt);
    pitheme_sgr(th, PR_MUTED, mut);
    pitheme_sgr(th, PR_SUCCESS, suc);
    pitheme_sgr(th, PR_WARNING, warn);
    cl_copy(out, selected ? acc : "", cap);
    cl_cat(out, selected ? "\342\206\222 " : "  ", cap);
    cl_cat(out, selected ? "" : txt, cap);
    cl_cat(out, r->name, cap);
    cl_cat(out, "\033[0m", cap);
    if (k->show_type) {
        cl_cat(out, mut, cap);
        cl_cat(out, " [", cap);
        cl_cat(out, r->type, cap);
        cl_cat(out, "]\033[0m", cap);
    }
    if (r->status == PL_ST_NONE) {
        cl_cat(out, mut, cap);
        cl_cat(out, " \342\200\242 not configured\033[0m", cap);
    } else if (r->status == PL_ST_OTHER) {
        cl_cat(out, mut, cap);
        cl_cat(out, " \342\200\242 \033[0m", cap);
        cl_cat(out, warn, cap);
        cl_cat(out, r->status_text, cap);
        cl_cat(out, "\033[0m", cap);
    } else {
        cl_cat(out, suc, cap);
        cl_cat(out, " \342\234\223 ", cap);
        cl_cat(out, r->status_text[0] ? r->status_text : "configured", cap);
        cl_cat(out, "\033[0m", cap);
    }
}

/* the keys of the selector: up and down stop at the ends, typing goes to the search box, Enter and Esc are the picker's */
static int lgp_key(void *u, const cl_key *key, int act)
{
    lgpick *k = (lgpick *)u;
    long ql = (long)strlen(k->pk.query);
    (void)act;
    switch (key->k) {
    case K_UP:
        if (k->pk.sel > 0)
            k->pk.sel--;
        return 1;
    case K_DOWN:
        if (k->pk.sel + 1 < k->pk.n)
            k->pk.sel++;
        return 1;
    case K_ENTER:
    case K_ESC:
        return 0;
    case K_BS:
        if (ql) {
            do
                ql--;
            while (ql > 0 && ((unsigned char)k->pk.query[ql] & 0xc0) == 0x80);
            k->pk.query[ql] = 0;
            lgp_filter(k);
        }
        return 1;
    case K_CHAR:
        if (key->ch >= 0x20 && ql + 5 < (long)sizeof(k->pk.query)) {
            ql += vw_put_utf8(k->pk.query + ql, key->ch);
            k->pk.query[ql] = 0;
            lgp_filter(k);
        }
        return 1;
    case K_PASTE: {
        long i;
        for (i = 0; i < key->n && ql + 1 < (long)sizeof(k->pk.query); i++)
            if ((unsigned char)key->text[i] >= 0x20)
                k->pk.query[ql++] = key->text[i];
        k->pk.query[ql] = 0;
        lgp_filter(k);
        return 1;
    }
    default:
        return key->k == K_CTRL && key->ch == 'c' ? 0 : 1;
    }
}

static const pi_login_row *test_rows;
static int test_nrows, test_show_type;

void pi_ui_set_login_rows(const struct pi_login_row *rows, int n, int show_type)
{
    test_rows = rows;
    test_nrows = rows ? n : 0;
    test_show_type = show_type;
}

static int lg_pick(void *u, const char *title, const pi_login_row *rows, int n, int show_type, const char *initial,
                   const char *empty)
{
    lg *l = (lg *)u;
    lgpick k;
    int res;
    if (test_nrows) {
        rows = test_rows;
        n = test_nrows;
        show_type = test_show_type;
    }
    memset(&k, 0, sizeof(k));
    k.l = l;
    k.rows = rows;
    k.n = n;
    k.show_type = show_type;
    k.vis = (int *)malloc(sizeof(int) * (size_t)(n ? n : 1));
    k.score = (long *)malloc(sizeof(long) * (size_t)(n ? n : 1));
    if (!k.vis || !k.score) {
        free(k.vis);
        free(k.score);
        return -1;
    }
    k.pk.title = title;
    k.pk.maxrows = 8;
    k.pk.row = lgp_row;
    k.pk.key = lgp_key;
    k.pk.u = &k;
    k.pk.layout = TUI_PICK_AUTH;
    k.pk.empty = empty;
    cl_copy(k.pk.query, initial ? initial : "", sizeof(k.pk.query));
    lgp_filter(&k);
    res = tui_pick_run(l->t, &k.pk);
    res = res < 0 ? -1 : k.vis[res];
    free(k.vis);
    free(k.score);
    return res;
}

/* the sign-in dialog (login-dialog.ts): its rows, and the hint "(escape/ctrl+c to cancel)" as pi's keyHint colours it */
static void lg_hint(lg *l, const char *a, const char *b, const char *c, const char *d)
{
    char dim[48], mut[48], row[300];
    pitheme_sgr(&l->p->theme, PR_DIM, dim);
    pitheme_sgr(&l->p->theme, PR_MUTED, mut);
    cl_copy(row, "(", sizeof(row));
    cl_cat(row, dim, sizeof(row));
    cl_cat(row, a, sizeof(row));
    cl_cat(row, "\033[0m", sizeof(row));
    cl_cat(row, mut, sizeof(row));
    cl_cat(row, b, sizeof(row));
    cl_cat(row, "\033[0m", sizeof(row));
    if (c) {
        cl_cat(row, " ", sizeof(row));
        cl_cat(row, dim, sizeof(row));
        cl_cat(row, c, sizeof(row));
        cl_cat(row, "\033[0m", sizeof(row));
        cl_cat(row, mut, sizeof(row));
        cl_cat(row, d, sizeof(row));
        cl_cat(row, "\033[0m", sizeof(row));
    }
    cl_cat(row, ")", sizeof(row));
    tui_dialog_add(l->t, TUI_DLG_ANSI, 0, row);
}

static void lg_hint_url(lg *l)
{
    char dim[48], row[200];
    pitheme_sgr(&l->p->theme, PR_DIM, dim);
    cl_copy(row, dim, sizeof(row));
    cl_cat(row, "Select the URL with the mouse to copy it\033[0m", sizeof(row));
    tui_dialog_add(l->t, TUI_DLG_ANSI, 0, row);
}

static void lg_dialog(void *u, int op, const char *a, const char *b)
{
    lg *l = (lg *)u;
    cl_tui *t = l->t;
    switch (op) {
    case PD_OPEN:
        tui_dialog_open(t, a);
        break;
    case PD_CLOSE:
        tui_dialog_close(t);
        break;
    case PD_AUTH:
        tui_dialog_clear(t);
        tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
        tui_dialog_add(t, TUI_DLG_TEXT, PR_ACCENT, a);
        lg_hint_url(l);
        if (b && *b) {
            tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
            tui_dialog_add(t, TUI_DLG_TEXT, PR_WARNING, b);
        }
        break;
    case PD_DEVICE: {
        char code[300];
        tui_dialog_clear(t);
        tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
        tui_dialog_add(t, TUI_DLG_TEXT, PR_ACCENT, b);
        lg_hint_url(l);
        tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
        cl_copy(code, "Enter code: ", sizeof(code));
        cl_cat(code, a, sizeof(code));
        tui_dialog_add(t, TUI_DLG_TEXT, PR_WARNING, code);
        tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
        tui_dialog_add(t, TUI_DLG_TEXT, PR_DIM, "Waiting for authentication...");
        lg_hint(l, "escape/ctrl+c", " to cancel", 0, 0);
        break;
    }
    case PD_INFO:
        tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
        tui_dialog_add(t, TUI_DLG_TEXT, PR_TEXT, a);
        break;
    default:
        tui_dialog_add(t, TUI_DLG_TEXT, PR_DIM, a);
        break;
    }
}

static int lg_ask(void *u, int manual, const char *message, const char *placeholder, char *out, long cap)
{
    lg *l = (lg *)u;
    cl_tui *t = l->t;
    char *buf = (char *)malloc(1100);
    int r;
    if (!buf)
        return -1;
    tui_dialog_add(t, TUI_DLG_BLANK, 0, 0);
    tui_dialog_add(t, TUI_DLG_TEXT, manual ? PR_DIM : PR_TEXT, message);
    if (!manual && placeholder && *placeholder) {
        char ph[400];
        cl_copy(ph, "e.g., ", sizeof(ph));
        cl_cat(ph, placeholder, sizeof(ph));
        tui_dialog_add(t, TUI_DLG_TEXT, PR_DIM, ph);
    }
    tui_dialog_add(t, TUI_DLG_INPUT, 0, "");
    if (manual)
        lg_hint(l, "escape/ctrl+c", " to cancel", 0, 0);
    else
        lg_hint(l, "escape/ctrl+c", " to cancel,", "enter", " to submit");
    r = tui_dialog_read(t, buf, 1100);
    if (r == 1)
        cl_copy(out, buf, cap);
    memset(buf, 0, 1100);
    free(buf);
    return r == 1 ? 0 : -1;
}

static int lg_cancelled(void *u)
{
    lg *l = (lg *)u;
    return tui_dialog_cancelled(l->t);
}

static int lg_menu(void *u, const char *title, const char *const *opt, int n)
{
    lg *l = (lg *)u;
    if (l->t && l->p->look)
        return tui_ext_menu(l->t, title, opt, n, 0);
    {
        char ans[32], num[16];
        int i;
        ui_line(&l->p->ui, title);
        for (i = 0; i < n; i++) {
            char row[400];
            cl_ltoa(i + 1, num);
            cl_copy(row, " ", sizeof(row));
            cl_cat(row, num, sizeof(row));
            cl_cat(row, ") ", sizeof(row));
            cl_cat(row, opt[i], sizeof(row));
            ui_line(&l->p->ui, row);
        }
        for (;;) {
            long k;
            ui_puts(&l->p->ui, "Number (empty to cancel): ");
            k = l->p->ui.io->read_line(l->p->ui.io->u, ans, sizeof(ans));
            l->p->ui.col0 = 1;
            if (k <= 0)
                return -1;
            i = (int)strtol(ans, 0, 10);
            if (i >= 1 && i <= n)
                return i - 1;
        }
    }
}

static int lg_line(void *u, const char *message, const char *placeholder, char *out, long cap)
{
    lg *l = (lg *)u;
    if (placeholder && *placeholder) {
        char ph[400];
        cl_copy(ph, "e.g., ", sizeof(ph));
        cl_cat(ph, placeholder, sizeof(ph));
        lg_note(u, PL_DIM, ph);
    }
    if (l->t && l->p->look) {
        char *buf = (char *)malloc(1100);
        int r;
        if (!buf)
            return -1;
        r = tui_line_input_hint(l->t, message, "enter submit", buf, 1100);
        if (r == 1)
            cl_copy(out, buf, cap);
        memset(buf, 0, 1100);
        free(buf);
        return r == 1 ? 0 : -1;
    }
    {
        long k;
        ui_puts(&l->p->ui, message);
        ui_puts(&l->p->ui, " ");
        k = l->p->ui.io->read_line(l->p->ui.io->u, out, cap);
        l->p->ui.col0 = 1;
        return k < 0 ? -1 : 0;
    }
}

static int login_command(pi_ui *p, cl_tui *t, const char *line)
{
    pi_auth *a = (pi_auth *)p->s->auth_u;
    lg l;
    pi_login_ui ui;
    int logout = !strcmp(line, "/logout");
    if (!a)
        return 0;
    l.p = p;
    l.t = t;
    ui.u = &l;
    memset(&ui, 0, sizeof(ui));
    ui.u = &l;
    ui.menu = lg_menu;
    ui.line = lg_line;
    ui.note = lg_note;
    if (t && p->look) {
        ui.pick = lg_pick;
        ui.dialog = lg_dialog;
        ui.ask = lg_ask;
        ui.cancelled = lg_cancelled;
    }
    if (p->pst_on)
        pi_pstatus_blocked(&p->pst, logout ? PSK_QUESTION : PSK_AUTH, logout ? "Log out" : "Log in");
    if (logout)
        pi_logout_run(a, &ui);
    else
        pi_login_run(a, line[6] ? line + 7 : 0, &ui);
    if (p->pst_on)
        pi_pstatus_blocked(&p->pst, PSK_NONE, 0);
    status(p);
    return 1;
}

static void build_skill_rows(pi_ui *p);
static void resources_report(pi_ui *p, int listing);

/* /reload (interactive-mode.ts handleReloadCommand): the resources are read again, the menu rows and the start-up block follow, and
 * a status line says so; a failure is "Reload failed: <message>" */
static void reload_command(pi_ui *p)
{
    char err[200];
    int nt;
    const cl_cmd *tm;
    if (!p->s->reload || p->s->reload(p->s->reload_u, p->s, err, sizeof(err))) {
        char m[260];
        cl_copy(m, "Reload failed: ", sizeof(m));
        cl_cat(m, p->s->reload ? err : "nothing to read", sizeof(m));
        if (p->look)
            pshow_error(&p->ps, m);
        else
            ui_line(&p->ui, m);
        return;
    }
    build_skill_rows(p);
    tm = p->s->templates ? pi_tmpl_menu((pi_tmpls *)p->s->templates, &nt) : 0;
    pi_ui_set_extra_commands(0, tm, tm ? nt : 0);
    if (p->look)
        pshow_resources_begin(&p->ps);
    resources_report(p, ui_quiet == 0);
    if (p->look)
        pshow_resources_end(&p->ps);
    note(p, "Reloaded skills, prompts, and context files");
}

static int run_command(pi_ui *p, cl_tui *t, const char *line)
{
    jw reply, editor;
    int rc;
    char *a, *nl;
    if (!strcmp(line, "/reload")) {
        reload_command(p);
        status(p);
        return 1;
    }
    if (p->look && t && !strcmp(line, "/tree") && p->s->command) {
        tree_command(p, t);
        return 1;
    }
    if (!strcmp(line, "/logout") || !strcmp(line, "/login") || !strncmp(line, "/login ", 7))
        return login_command(p, t, line);
    if (line[0] != '/' || !p->s->command)
        return 0;
    jw_init(&reply);
    jw_init(&editor);
    rc = p->s->command(p->s->command_u, p->s, line, &reply, &editor);
    if (rc && reply.n) {
        for (a = reply.p; *a;) {
            nl = strchr(a, '\n');
            if (nl)
                *nl = 0;
            note(p, a);
            if (!nl)
                break;
            a = nl + 1;
        }
    }
    if (rc && editor.n) {
        if (t)
            tui_set_text(t, editor.p);
        else {
            cl_copy(prefill, editor.p, sizeof(prefill));
            ui_line(&p->ui, "[INFO] The message is back: type it again edited, or press Enter to send it as it was.");
        }
    }
    jw_free(&reply);
    jw_free(&editor);
    if (rc)
        status(p);
    return rc;
}

static int is_exit(const char *line)
{
    return !strcmp(line, "/exit") || !strcmp(line, "/quit");
}

static void user_echo(pi_ui *p, const char *line)
{
    if (!p->look)               /* in pi's look the message shows when the conversation takes it (message_end) */
        ui_user(&p->ui, line);
}

/* the lines of the resources report: pi's show in its look, plain lines (the compact form) otherwise */
static void rr_line(void *u, int kind, int mode, const char *text)
{
    pi_ui *p = (pi_ui *)u;
    if (p->look)
        pshow_resource_line(&p->ps, kind, mode, text);
    else if (mode != RRM_EXPANDED)
        ui_line(&p->ui, text);
}

static void rr_count(void *u, int kind, int mode, const char *text)
{
    (void)kind;
    (void)mode;
    (void)text;
    (*(int *)u)++;
}

/* one "[title]" section of the report */
static void rr_section(pi_ui *p, int role, const char *title, int is_skills, int diagnostics)
{
    const pi_skills *k = p->s->res ? &p->s->res->skills : 0;
    const pi_tmpls *tm = p->s->templates;
    char plain[40];
    if (p->look)
        pshow_resource_title(&p->ps, role, title);
    else {
        cl_copy(plain, "[", sizeof(plain));
        cl_cat(plain, title, sizeof(plain));
        cl_cat(plain, "]", sizeof(plain));
        ui_line(&p->ui, plain);
    }
    if (is_skills) {
        if (diagnostics)
            pi_skills_report(k, rr_line, p);
        else
            pi_skills_listing(k, rr_line, p);
    } else if (diagnostics)
        pi_tmpls_report(tm, rr_line, p);
    else
        pi_tmpls_listing(tm, rr_line, p);
    if (p->look)
        pshow_resource_end(&p->ps);
}

/* pi's showLoadedResources for skills and prompt templates (interactive-mode.ts:1772-1930, 2044): the "[Skills]" and "[Prompts]"
 * lists unless quietStartup (compact; Ctrl+O draws them grouped by scope), and the "[Skill conflicts]" and "[Prompt conflicts]"
 * diagnostics always (the start header's closing Spacer separates the block from it) */
static void resources_report(pi_ui *p, int listing)
{
    const pi_skills *k = p->s->res ? &p->s->res->skills : 0;
    const pi_tmpls *tm = p->s->templates;
    int nsd = 0, ntd = 0, ls, lt;
    if (k)
        pi_skills_report(k, rr_count, &nsd);
    if (tm)
        pi_tmpls_report(tm, rr_count, &ntd);
    ls = listing && k && k->n;
    lt = listing && tm && tm->n;
    if (!ls && !lt && !nsd && !ntd)
        return;
    if (ls)
        rr_section(p, PR_MD_HEADING, "Skills", 1, 0);
    if (lt)
        rr_section(p, PR_MD_HEADING, "Prompts", 0, 0);
    if (nsd)
        rr_section(p, PR_WARNING, "Skill conflicts", 1, 1);
    if (ntd)
        rr_section(p, PR_WARNING, "Prompt conflicts", 0, 1);
}

int pi_ui_run(pi_session *s, cl_io *io, const char *first)
{
    static pi_ui p;
    char *line = (char *)malloc(8192);
    cl_tui *t;
    char hello[200];
    if (!line)
        return 0;
    attach(&p, s, io, 0);
    t = screen(&p, io);
    p.pst_on = t && p.look;
    if (p.pst_on) {
        pi_pstatus_init(&p.pst, ui_pstatus, pst_write, &p, "amiga-pi", pst_name, &p);
        pi_pstatus_start(&p.pst);
    }
    if (p.look && p.ui.tui->km.nwarn) {
        /* the problems of keybindings.json, a line each, as pi tells them at the start */
        char *w = p.ui.tui->km.warn.p ? p.ui.tui->km.warn.p : (char *)"", *nl;
        while (*w) {
            nl = strchr(w, '\n');
            if (nl)
                *nl = 0;
            pshow_note(&p.ps, PR_WARNING, w);
            if (!nl)
                break;
            w = nl + 1;
        }
    }
    p.ps.details = ui_quiet == 0;
    if (p.look && ui_quiet != 1)
        pshow_header(&p.ps, "amiga-pi", ui_version[0] ? ui_version : "(pi " PI_VERSION ")");
    else if (!p.look) {
        /* the UP-Term mention is amiga-pi's own (pi's header is the name and version): only on a console that is UP-Term's */
        cl_copy(hello, io->upterm && io->upterm(io->u) ? "amiga-pi in UP-Term. Model " : "amiga-pi. Model ", sizeof(hello));
        cl_cat(hello, s->model.provider, sizeof(hello));
        cl_cat(hello, "/", sizeof(hello));
        cl_cat(hello, s->model.id, sizeof(hello));
        cl_cat(hello, ". Type /exit to leave.", sizeof(hello));
        ui_line(&p.ui, hello);
    }
    resources_report(&p, ui_quiet == 0);
    if (p.look && s->nmsg)
        redraw_session(&p);            /* a resumed session shows its conversation, as pi's renderInitialMessages */
    {
        /* run(): the startup diagnostics as warnings (interactive-mode.ts:1197-1204) */
        int i;
        for (i = 0; i < ui_nwarn; i++) {
            if (p.look)
                pshow_note(&p.ps, PR_WARNING, ui_warn[i]);
            else
                ui_line(&p.ui, ui_warn[i]);
        }
        ui_nwarn = 0;
    }
    if (first && *first) {
        user_echo(&p, first);
        pi_prompt(s, first);
        say_end(&p);
    }
    for (;;) {
        long n;
        if (t)
            n = tui_read(t, line, 8192);
        else {
            ui_puts(&p.ui, "\n\033[1m>\033[0m ");
            n = io->read_line(io->u, line, 8192);
            p.ui.col0 = 1;
        }
        if (n < 0 || is_exit(line))
            break;
        if (!n && prefill[0]) {
            cl_copy(line, prefill, 8192);
            n = (long)strlen(line);
        }
        prefill[0] = 0;
        if (!n)
            continue;
        if (p.look && t && line[0] == '!' && bang(&p, line))
            continue;
        if (run_command(&p, t, line))
            continue;
        user_echo(&p, line);
        pi_prompt(s, line);
        say_end(&p);
    }
    if (t) {
        if (p.look && ui_resume) {
            /* pi: "To resume this session: <command>" when the session has a file */
            const char *cmd = ui_resume(ui_resume_u);
            if (cmd && cmd[0]) {
                cl_copy(t->exit_hint, "\033[2mTo resume this session:\033[22m ", sizeof(t->exit_hint));
                cl_cat(t->exit_hint, cmd, sizeof(t->exit_hint));
            }
        }
        if (p.pst_on)
            pi_pstatus_stop(&p.pst);
        p.pst_on = 0;
        tui_stop(t);
        if (p.look)
            pshow_free(&p.ps);
        show_free(p.ui.show);
        tui_free(t);
        free(t);
        free(p.ui.show);
        p.ui.tui = 0;
        p.ui.show = 0;
    }
    free(line);
    free(p.menu);
    free(p.menu_text);
    free(p.xmenu);
    free(p.xmenu_text);
    free(p.all);
    p.menu = 0;
    p.menu_text = 0;
    p.xmenu = 0;
    p.xmenu_text = 0;
    p.all = 0;
    pi_ui_set_extra_commands(1, 0, 0);
    pi_ui_set_extra_commands(3, 0, 0);
    s->ext.ui = 0;
    return 0;
}
