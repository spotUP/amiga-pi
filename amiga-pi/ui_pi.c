/* see ui_pi.h */
#include <stdlib.h>
#include <string.h>
#include "ui_pi.h"
#include "fuzzy.h"
#include "changelog.h"
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
#include "models.h"
#include "radius.h"
#include "llm/radius.h"
#include "authstore.h"
#include "session3.h"
#include "scoped.h"
#include "settings_pi.h"

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
    int redraw_after;           /* a setting of the screen changed: the transcript is drawn again when the selector closes */
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
    { "/settings", "Open settings menu" },
    { "/model", "<provider/model> \342\200\224 Select model (opens selector UI)" },
    { "/tree", "Navigate session tree (switch branches)" },
    { "/thinking", "<level> \342\200\224 Set thinking level" },
    { "/scoped-models", "Enable/disable models for Ctrl+P cycling" },
    { "/export", "Export session (HTML default, or specify path: .html/.jsonl)" },
    { "/name", "Set session display name" },
    { "/session", "Show session info and stats" },
    { "/changelog", "Show changelog entries" },
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
static int ui_images;                   /* the terminal draws images (pi_ui_set_images) */
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

void pi_ui_set_images(int on)
{
    ui_images = on;
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

static const char *ui_cl_file, *ui_cl_news;
static long ui_cl_file_n, ui_cl_news_n;
static int ui_cl_collapsed;

void pi_ui_set_changelog(const char *file, long file_n, const char *news, long news_n, int collapsed)
{
    ui_cl_file = file;
    ui_cl_file_n = file_n;
    ui_cl_news = news;
    ui_cl_news_n = news_n;
    ui_cl_collapsed = collapsed;
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

/* the prompt history kept across runs (pi keeps it in memory only; the owner asked for it on disk, 2026-11-01) */
static cl_sys *ui_hist_sys;
static char ui_hist_file[400];

void pi_ui_set_history(cl_sys *sys, const char *path)
{
    ui_hist_sys = sys;
    cl_copy(ui_hist_file, path ? path : "", sizeof(ui_hist_file));
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
static void settings_live(pi_ui *p, pi_store *st, const char *id);
static void tui_settings_live(cl_tui *t, cl_sys *sys, const char *dir, const char *id);
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
        if (!s->command_u || ((pi_store *)s->command_u)->cp_enabled)      /* the footer's "(auto)": auto-compaction is on */
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
    if (ui_hist_sys && ui_hist_file[0]) {
        /* pi's editor history, one list for every directory ("" as the project): the last 100 prompts, no repeat of the
         * newest (editor.js addToHistory), read from the file and written after each submitted prompt (tui.c remember) */
        t->sys = ui_hist_sys;
        t->project = "";
        t->histfile = ui_hist_file;
        hist_load(&t->hist, ui_hist_sys, ui_hist_file);
        hist_fill(&t->hist, &t->ed, "");
    }
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
    if (p->s->command_u && ((pi_store *)p->s->command_u)->models) {
        pi_store *ps0 = (pi_store *)p->s->command_u;
        tui_settings_live(t, ps0->models->sys, ps0->models->agent_dir, 0);      /* the first frame follows the settings too */
    }
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
    if (p->s->command_u && ((pi_store *)p->s->command_u)->models)
        settings_live(p, (pi_store *)p->s->command_u, 0);     /* the screen's own settings, from settings.json */
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
    {
        /* showTreeSelector: the selector starts with the filter settings.json's treeFilterMode names */
        static const char *const modes[TF_COUNT] = { "default", "no-tools", "user-only", "labeled-only", "all" };
        char mode[24];
        int f;
        pi_set_str(st->models ? st->models->sys : 0, st->models ? st->models->agent_dir : 0, "treeFilterMode", "default", mode, sizeof(mode));
        tree_filter = TF_DEFAULT;
        for (f = 0; f < TF_COUNT; f++)
            if (!strcmp(mode, modes[f]))
                tree_filter = f;
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

/* the items whose text matches the query as pi's fuzzyFilter does, best first (Array.sort is stable: ties stay in item order): their
 * indexes into vis, the scores into score; how many. An empty query keeps every item in order */
static int fuzzy_order(const char *q, int n, const char *(*text)(void *u, int i), void *u, int *vis, long *score)
{
    int i, m = 0, j;
    for (i = 0; i < n; i++) {
        long sc;
        const char *tx = text(u, i);
        if (!q[0] || pi_fuzzy_item(q, (long)strlen(q), tx, (long)strlen(tx), &sc)) {
            if (!q[0])
                sc = 0;
            j = m++;
            while (j > 0 && score[j - 1] > sc) {
                vis[j] = vis[j - 1];
                score[j] = score[j - 1];
                j--;
            }
            vis[j] = i;
            score[j] = sc;
        }
    }
    return m;
}

static const char *lgp_text(void *u, int i)
{
    return ((lgpick *)u)->rows[i].search;
}

static void lgp_filter(lgpick *k)
{
    k->pk.n = fuzzy_order(k->pk.query, k->n, lgp_text, k, k->vis, k->score);
    if (k->pk.sel > k->pk.n - 1)
        k->pk.sel = k->pk.n ? k->pk.n - 1 : 0;
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
    default:
        if (key->k == K_CTRL && key->ch == 'c')
            return 0;
        if (tui_pick_qkey(&k->pk, key))
            lgp_filter(k);
        return 1;
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
    k.pk.qcur1 = k.pk.query[0] ? 1 : 0;      /* Input.setValue leaves the cursor at the start (oauth-selector.ts:107, input.ts:66) */
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

/* a titled list (ExtensionSelectorComponent), with a line under the title when desc is not 0. The pi look draws the line in the
 * menu's question style (bold; pi has it in the text colour) */
static int lg_menu_d(void *u, const char *title, const char *desc, const char *const *opt, int n)
{
    lg *l = (lg *)u;
    if (l->t && l->p->look) {
        if (!desc)
            return tui_ext_menu(l->t, title, opt, n, 0);
        l->t->m_ext = 1;
        l->t->m_timeout_ms = -1;
        return tui_menu(l->t, title, desc, opt, n, 0, -1);
    }
    {
        char ans[32], num[16];
        int i;
        ui_line(&l->p->ui, title);
        if (desc)
            ui_line(&l->p->ui, desc);
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

static int lg_menu(void *u, const char *title, const char *const *opt, int n)
{
    return lg_menu_d(u, title, 0, opt, n);
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
    ui.menu_desc = lg_menu_d;
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
/* the resources read again and the slash menu's rows made from them: 0, -1 with err */
static int reload_resources(pi_ui *p, char *err, long cap)
{
    int nt;
    const cl_cmd *tm;
    if (!p->s->reload || p->s->reload(p->s->reload_u, p->s, err, cap)) {
        if (!p->s->reload)
            cl_copy(err, "nothing to read", cap);
        return -1;
    }
    build_skill_rows(p);
    tm = p->s->templates ? pi_tmpl_menu((pi_tmpls *)p->s->templates, &nt) : 0;
    pi_ui_set_extra_commands(0, tm, tm ? nt : 0);
    if (p->ui.tui)
        p->ui.tui->cmds = ui_menu(p, &p->ui.tui->ncmds);        /* the screen's menu follows (the rows were made again, the old array is gone) */
    return 0;
}

static void reload_command(pi_ui *p)
{
    char err[200];
    if (reload_resources(p, err, sizeof(err))) {
        char m[260];
        cl_copy(m, "Reload failed: ", sizeof(m));
        cl_cat(m, err, sizeof(m));
        if (p->look)
            pshow_error(&p->ps, m);
        else
            ui_line(&p->ui, m);
        return;
    }
    if (p->look)
        pshow_resources_begin(&p->ps);
    resources_report(p, ui_quiet == 0);
    if (p->look)
        pshow_resources_end(&p->ps);
    note(p, "Reloaded skills, prompts, and context files");
}

/* ---- /model, /thinking and /fork as pi's selectors (model-selector.ts, thinking-selector.ts, user-message-selector.ts) over the screen's
 * picker (tui_pick, TUI_PICK_FRAME): the keys of /login and -r -- Up and Down wrap, typing filters with fuzzy.c, Enter selects, Esc or
 * Ctrl+C cancels, Ctrl+S sets the default -- and the frames, rows and hints of pi 1.2.0 (tests/pi/tui model_*, thinking_*, fork_*) ---- */

static const char *role_sg(const pi_ui *p, int role, char *buf)
{
    pitheme_sgr(p->ps.th, role, buf);
    return buf;
}

/* a key the selectors leave to the picker (movement, Enter, Esc, Ctrl+C); 0 for the rest, which a selector takes for itself */
static int pick_keeps(const cl_key *key, int act)
{
    return act == KA_SEL_NEXT || act == KA_SEL_PREV || act == KA_SEL_ACCEPT || act == KA_SEL_CANCEL || key->k == K_UP ||
           key->k == K_DOWN || key->k == K_ENTER || key->k == K_ESC || (key->k == K_CTRL && key->ch == 'c');
}

/* the number of the first list row on the screen: a selector's window of maxv rows around the selection */
static void pick_window(const tui_pick *pk, int maxv, int *start, int *end)
{
    int st = pk->sel - maxv / 2;
    if (st > pk->n - maxv)
        st = pk->n - maxv;
    if (st < 0)
        st = 0;
    *start = st;
    *end = st + maxv < pk->n ? st + maxv : pk->n;
}

static void pick_scroll(const pi_ui *p, const tui_pick *pk, int maxv, char *out, long cap)
{
    int st, en;
    pick_window(pk, maxv, &st, &en);
    if (st > 0 || en < pk->n) {
        char mut[48], num[16];
        cl_cat(out, role_sg(p, PR_MUTED, mut), cap);
        cl_cat(out, "  (", cap);
        cl_ltoa(pk->sel + 1, num);
        cl_cat(out, num, cap);
        cl_cat(out, "/", cap);
        cl_ltoa(pk->n, num);
        cl_cat(out, num, cap);
        cl_cat(out, ")\033[0m\n", cap);
    }
}

/* "text" in a role's colour, the colour switched off after it */
static void cat_role(const pi_ui *p, int role, const char *text, char *out, long cap)
{
    char sgr[48];
    cl_cat(out, role_sg(p, role, sgr), cap);
    cl_cat(out, text, cap);
    cl_cat(out, "\033[0m", cap);
}

/* findExactModelReferenceMatch (model-resolver.ts): provider/id in any case, else the id alone; only one match counts */
static const llm_model *exact_model(pi_models *ms, const char *ref, const llm_model **list, int n)
{
    char low[300], full[300];
    const llm_model *hit = 0;
    int i, cnt = 0;
    long l = (long)strlen(ref);
    while (l > 0 && (ref[l - 1] == ' ' || ref[l - 1] == '\t'))
        l--;
    while (l > 0 && (*ref == ' ' || *ref == '\t')) {
        ref++;
        l--;
    }
    if (!l || l >= (long)sizeof(low))
        return 0;
    for (i = 0; i < l; i++)
        low[i] = (char)((ref[i] >= 'A' && ref[i] <= 'Z') ? ref[i] + 32 : ref[i]);
    low[l] = 0;
    (void)ms;
    for (i = 0; i < n; i++) {
        long k;
        cl_copy(full, list[i]->provider, sizeof(full));
        cl_cat(full, "/", sizeof(full));
        cl_cat(full, list[i]->id, sizeof(full));
        for (k = 0; full[k]; k++)
            if (full[k] >= 'A' && full[k] <= 'Z')
                full[k] = (char)(full[k] + 32);
        if (!strcmp(full, low)) {
            hit = list[i];
            cnt++;
        }
    }
    if (cnt)
        return cnt == 1 ? hit : 0;
    for (cnt = 0, i = 0; i < n; i++) {
        long k;
        cl_copy(full, list[i]->id, sizeof(full));
        for (k = 0; full[k]; k++)
            if (full[k] >= 'A' && full[k] <= 'Z')
                full[k] = (char)(full[k] + 32);
        if (!strcmp(full, low)) {
            hit = list[i];
            cnt++;
        }
    }
    return cnt == 1 ? hit : 0;
}

#define MS_MAX 600

typedef struct msel {
    pi_ui *p;
    pi_models *ms;
    const llm_model *all[MS_MAX], *sc[MS_MAX];   /* every model with a credential (current first, the default second, then by provider); the scope */
    int nall, nsc, scoped;                       /* scoped: the scope is what the list shows */
    int vis[MS_MAX];
    long score[MS_MAX];
    char defp[100], defid[200];
    int has_def, save;
    char refresh[100], err[300];
    char top[700];
    tui_pick pk;
} msel;

static int ms_same(const llm_model *a, const llm_model *b)
{
    return !strcmp(a->provider, b->provider) && !strcmp(a->id, b->id);
}

static int ms_is_default(const msel *m, const llm_model *mo)
{
    return m->has_def && !strcmp(mo->provider, m->defp) && !strcmp(mo->id, m->defid);
}

static const llm_model **ms_active(msel *m, int *n)
{
    *n = m->scoped ? m->nsc : m->nall;
    return m->scoped ? m->sc : m->all;
}

/* getModelSearchText: provider, provider/id, provider id, the name */
static void model_search_text(const llm_model *mo, char *t, long cap)
{
    cl_copy(t, mo->provider, cap);
    cl_cat(t, " ", cap);
    cl_cat(t, mo->provider, cap);
    cl_cat(t, "/", cap);
    cl_cat(t, mo->id, cap);
    cl_cat(t, " ", cap);
    cl_cat(t, mo->provider, cap);
    cl_cat(t, " ", cap);
    cl_cat(t, mo->id, cap);
    if (mo->name && mo->name[0]) {
        cl_cat(t, " ", cap);
        cl_cat(t, mo->name, cap);
    }
}

/* getModelSelectorSearchText plus " default" for the default model */
static const char *ms_text(void *u, int i)
{
    static char t[700];
    msel *m = (msel *)u;
    int n;
    const llm_model **a = ms_active(m, &n);
    const llm_model *mo = a[i];
    model_search_text(mo, t, sizeof(t));
    if (ms_is_default(m, mo))
        cl_cat(t, " default", sizeof(t));
    return t;
}

/* filterModels: the matches (a search for "default" puts the default model first), the cursor on the best match, or kept when the query is empty */
static void ms_filter(msel *m, int keep_sel)
{
    int n, i, nv, j;
    const llm_model **a = ms_active(m, &n);
    const char *q = m->pk.query;
    (void)a;
    nv = fuzzy_order(q, n, ms_text, m, m->vis, m->score);
    {
        /* isDefaultSearch: the trimmed, lower-cased query is a non-empty start of "default" */
        char low[40];
        long l = 0, k = 0;
        const char *w = q;
        while (*w == ' ')
            w++;
        for (; w[k] && l < (long)sizeof(low) - 1; k++, l++)
            low[l] = (char)(w[k] >= 'A' && w[k] <= 'Z' ? w[k] + 32 : w[k]);
        low[l] = 0;
        while (l > 0 && low[l - 1] == ' ')
            low[--l] = 0;
        if (l > 0 && !strncmp("default", low, (size_t)l)) {
            int tmp[MS_MAX], nt = 0;
            for (i = 0; i < n; i++)
                if (ms_is_default(m, a[i]))
                    tmp[nt++] = i;
            for (i = 0; i < nv && nt < MS_MAX; i++) {
                int dup = 0;
                for (j = 0; j < nt; j++)
                    if (tmp[j] == m->vis[i])
                        dup = 1;
                if (!dup)
                    tmp[nt++] = m->vis[i];
            }
            memcpy(m->vis, tmp, sizeof(int) * (size_t)nt);
            nv = nt;
        }
    }
    m->pk.n = nv;
    if (q[0])
        m->pk.sel = 0;
    else if (!keep_sel || m->pk.sel > nv - 1)
        m->pk.sel = nv ? (keep_sel ? nv - 1 : 0) : 0;
}

/* the list at its start: the cursor on the session's model */
static void ms_reset_sel(msel *m)
{
    int n, i;
    const llm_model **a = ms_active(m, &n);
    m->pk.sel = 0;
    ms_filter(m, 1);
    for (i = 0; i < m->pk.n; i++)
        if (ms_same(a[m->vis[i]], &m->p->s->model)) {
            m->pk.sel = i;
            break;
        }
}

static void ms_row(void *u, int i, int selected, char *out, long cap)
{
    msel *m = (msel *)u;
    int n;
    const llm_model *mo = ms_active(m, &n)[m->vis[i]];
    int cur = ms_same(mo, &m->p->s->model);
    char b[200];
    out[0] = 0;
    if (selected)
        cat_role(m->p, PR_ACCENT, "\342\206\222 ", out, cap);
    else
        cl_cat(out, "  ", cap);
    if (cur)
        cat_role(m->p, PR_ACCENT, "\342\234\223 ", out, cap);
    else
        cl_cat(out, "  ", cap);
    if (selected)
        cat_role(m->p, PR_ACCENT, mo->id, out, cap);
    else
        cl_cat(out, mo->id, cap);
    cl_cat(out, " ", cap);
    cl_copy(b, "[", sizeof(b));
    cl_cat(b, mo->provider, sizeof(b));
    cl_cat(b, "]", sizeof(b));
    cat_role(m->p, PR_MUTED, b, out, cap);
    if (ms_is_default(m, mo))
        cat_role(m->p, PR_MUTED, " \302\267 default", out, cap);
}

/* the scope lines or the hint above the search box (the frame adds the blank rows round them) */
static void ms_top(msel *m)
{
    char sgr[48];
    m->top[0] = '\n';
    m->top[1] = 0;
    if (m->nsc > 0) {
        cat_role(m->p, PR_MUTED, "Scope: ", m->top, sizeof(m->top));
        cat_role(m->p, m->scoped ? PR_MUTED : PR_ACCENT, "all", m->top, sizeof(m->top));
        cat_role(m->p, PR_MUTED, " | ", m->top, sizeof(m->top));
        cat_role(m->p, m->scoped ? PR_ACCENT : PR_MUTED, "scoped", m->top, sizeof(m->top));
        cl_cat(m->top, "\n", sizeof(m->top));
        /* keyHint("tui.input.tab", "scope"): the key dim, the words muted, then " (all/scoped)" muted */
        cat_role(m->p, PR_DIM, "tab", m->top, sizeof(m->top));
        cat_role(m->p, PR_MUTED, " scope", m->top, sizeof(m->top));
        cat_role(m->p, PR_MUTED, " (all/scoped)", m->top, sizeof(m->top));
    } else
        cat_role(m->p, PR_WARNING, "Only showing models from configured providers. Use /login to add providers.", m->top, sizeof(m->top));
    (void)sgr;
    cl_cat(m->top, "\n", sizeof(m->top));
    m->pk.top = m->top;
}

static void ms_tail(void *u, char *out, long cap)
{
    msel *m = (msel *)u;
    int n;
    const llm_model *cur;
    out[0] = 0;
    pick_scroll(m->p, &m->pk, 10, out, cap);
    if (m->err[0]) {
        const char *a = m->err;
        for (;;) {
            const char *nl = strchr(a, '\n');
            char row[300];
            long l = nl ? (long)(nl - a) : (long)strlen(a);
            if (l >= (long)sizeof(row))
                l = (long)sizeof(row) - 1;
            memcpy(row, a, (size_t)l);
            row[l] = 0;
            cat_role(m->p, PR_ERROR, row, out, cap);
            cl_cat(out, "\n", cap);
            if (!nl)
                break;
            a = nl + 1;
        }
    } else if (!m->pk.n) {
        cat_role(m->p, PR_MUTED, "  No matching models", out, cap);
        cl_cat(out, "\n", cap);
    } else {
        char b[400];
        cur = ms_active(m, &n)[m->vis[m->pk.sel]];
        cl_cat(out, "\n", cap);
        cl_copy(b, "  Model Name: ", sizeof(b));
        cl_cat(b, cur->name && cur->name[0] ? cur->name : cur->id, sizeof(b));
        cat_role(m->p, PR_MUTED, b, out, cap);
        cl_cat(out, "\n", cap);
    }
    if (m->refresh[0]) {
        char b[140];
        cl_cat(out, "\n", cap);
        cl_copy(b, "  ", sizeof(b));
        cl_cat(b, m->refresh, sizeof(b));
        cat_role(m->p, m->err[0] ? PR_MUTED : PR_SUCCESS, b, out, cap);
        cl_cat(out, "\n", cap);
    }
    cl_cat(out, "\n", cap);
    cat_role(m->p, PR_DIM, "  Enter to select \302\267 Ctrl+S to set as default \302\267 Escape/Ctrl+C to cancel", out, cap);
}

static int ms_key(void *u, const cl_key *key, int act)
{
    msel *m = (msel *)u;
    if (key->k == K_TAB) {
        if (m->nsc > 0) {
            m->scoped = !m->scoped;
            m->pk.sel = 0;
            ms_reset_sel(m);
            ms_top(m);
            ms_filter(m, 1);
        }
        return 1;
    }
    if (key->k == K_CTRL && key->ch == 's') {
        m->save = 1;
        m->pk.done = 1;
        return 1;
    }
    if (pick_keeps(key, act))
        return 0;
    if (tui_pick_qkey(&m->pk, key))
        ms_filter(m, 0);
    return 1;
}

/* localeCompare of two provider names, enough for ASCII: case-insensitive first, lower case before upper on a tie */
static int ms_pcmp(const char *a, const char *b)
{
    const char *x = a, *y = b;
    for (; *x && *y; x++, y++) {
        int cx = *x >= 'A' && *x <= 'Z' ? *x + 32 : *x, cy = *y >= 'A' && *y <= 'Z' ? *y + 32 : *y;
        if (cx != cy)
            return cx - cy;
    }
    if (*x || *y)
        return *x ? 1 : -1;
    return -strcmp(a, b);
}

/* showModelSelector. search: the text the search box starts with (/model <text>) */
static void model_selector(pi_ui *p, cl_tui *t, const char *search)
{
    static msel m;
    pi_store *st = (pi_store *)p->s->command_u;
    pi_models *ms = st->models;
    char *set;
    long sn = 0;
    int i, j, res, rank_n = 0, timed_out = 0;
    int rank[MS_MAX];
    char rerr[300];
    memset(&m, 0, sizeof(m));
    m.p = p;
    m.ms = ms;
    rerr[0] = 0;
    if (p->s->auth_u) {
        pi_auth_deadline((pi_auth *)p->s->auth_u, pi_model_refresh_limit_ms);   /* model-selector.ts:184-189: the same 15 s */
        pi_radius_refresh((pi_auth *)p->s->auth_u, 0, rerr, sizeof(rerr));
        timed_out = pi_auth_timed_out((pi_auth *)p->s->auth_u);
        pi_auth_deadline((pi_auth *)p->s->auth_u, 0);
    }
    set = pi_store_read(ms->sys, ms->agent_dir, "settings.json", &sn);
    if (set) {
        char *dp = pi_jobj_str(set, sn, "defaultProvider"), *dm = pi_jobj_str(set, sn, "defaultModel");
        if (dp && dm && dp[0] && dm[0]) {
            cl_copy(m.defp, dp, sizeof(m.defp));
            cl_copy(m.defid, dm, sizeof(m.defid));
            m.has_def = 1;
        }
        free(dp);
        free(dm);
        free(set);
    }
    for (i = 0; i < ms->reg.n && m.nall < MS_MAX; i++)
        if (pi_model_usable(ms, &ms->reg.m[i]))
            m.all[m.nall++] = &ms->reg.m[i];
    for (i = 0; i < m.nall; i++)
        rank[i] = ms_same(m.all[i], &p->s->model) ? 0 : ms_is_default(&m, m.all[i]) ? 1 : 2;
    /* current first, the default second, then by provider (a stable sort) */
    for (i = 1; i < m.nall; i++) {
        const llm_model *x = m.all[i];
        int rx = rank[i];
        for (j = i; j > 0 && (rank[j - 1] > rx || (rank[j - 1] == rx && rx == 2 && ms_pcmp(m.all[j - 1]->provider, x->provider) > 0)); j--) {
            m.all[j] = m.all[j - 1];
            rank[j] = rank[j - 1];
        }
        m.all[j] = x;
        rank[j] = rx;
    }
    (void)rank_n;
    {
        int idx[MS_MAX];
        int ns = pi_models_scoped(ms, idx, MS_MAX);
        for (i = 0; i < ns; i++)
            m.sc[m.nsc++] = &ms->reg.m[idx[i]];
    }
    m.scoped = m.nsc > 0;
    cl_copy(m.pk.query, search ? search : "", sizeof(m.pk.query));
    m.pk.qcur1 = m.pk.query[0] ? 1 : 0;  /* Input.setValue leaves the cursor at the start (tui input.ts:66) */
    if (timed_out) {
        cl_copy(m.err, "Model refresh timed out; showing cached models.", sizeof(m.err));
    } else if (rerr[0]) {
        cl_copy(m.err, "Could not refresh radius; showing cached models.", sizeof(m.err));
    } else
        cl_copy(m.refresh, "Model catalogs refreshed.", sizeof(m.refresh));
    ms_top(&m);
    m.pk.maxrows = 10;
    m.pk.row = ms_row;
    m.pk.key = ms_key;
    m.pk.tail = ms_tail;
    m.pk.u = &m;
    m.pk.layout = TUI_PICK_FRAME;
    m.pk.search = 1;
    ms_reset_sel(&m);
    res = tui_pick_run(t, &m.pk);
    if (res >= 0) {
        int n;
        const llm_model *to = ms_active(&m, &n)[m.vis[res]];
        char err[200], line[300];
        if (pi_model_switch(ms, p->s, to, err, sizeof(err))) {
            pshow_error(&p->ps, err);
        } else {
            if (m.save) {
                pi_store_set_member(ms->sys, ms->agent_dir, "defaultProvider", to->provider, "defaultModel", to->id);
                pi_scope_add_default(ms, to, ms->sys, ms->agent_dir);       /* _addPersistedDefaultToNonEmptyScope */
            }
            if (m.save) {
                cl_copy(line, "Default model: ", sizeof(line));
                cl_cat(line, to->provider, sizeof(line));
                cl_cat(line, "/", sizeof(line));
            } else
                cl_copy(line, "Model: ", sizeof(line));
            cl_cat(line, to->id, sizeof(line));
            pshow_note(&p->ps, PR_DIM, line);
        }
    }
    status(p);
}

/* the models /model <text> is matched against: the scope when there is one, else every usable model (the available snapshot). 1 when scoped */
static int model_candidates(pi_models *ms, const llm_model **list, int cap, int *n)
{
    int i, idx[MS_MAX], ns = pi_models_scoped(ms, idx, MS_MAX);
    *n = 0;
    if (ns > 0) {
        for (i = 0; i < ns && *n < cap; i++)
            list[(*n)++] = &ms->reg.m[idx[i]];
        return 1;
    }
    for (i = 0; i < ms->reg.n && *n < cap; i++)
        if (pi_model_usable(ms, &ms->reg.m[i]))
            list[(*n)++] = &ms->reg.m[i];
    return 0;
}

/* findExactModelMatch's refresh (interactive-mode.ts:5229-5261): every catalog once, each provider on its own so a failure names it; pi aborts at 15 s */
long pi_model_cmd_refreshes;
long pi_model_refresh_limit_ms = 15000;       /* pi's AbortSignal.timeout(15000); a test shortens it */

static void model_refresh_all(pi_ui *p, pi_models *ms)
{
    char names[400], e[300], line[500];
    int timed_out;
    int i, nrad = ms->reg.nrad;
    char ids[LLM_RADIUS_MAX + 1][64];
    names[0] = 0;
    for (i = 0; i < nrad; i++)
        cl_copy(ids[i], ms->reg.rad[i].id, sizeof(ids[i]));
    cl_copy(ids[nrad], LLM_OPENROUTER, sizeof(ids[nrad]));
    pi_model_cmd_refreshes++;
    pshow_note(&p->ps, PR_DIM, "Refreshing model catalogs\342\200\246");
    pi_auth_deadline((pi_auth *)p->s->auth_u, pi_model_refresh_limit_ms);
    for (i = 0; i <= nrad && !pi_auth_timed_out((pi_auth *)p->s->auth_u); i++) {
        e[0] = 0;
        pi_radius_refresh((pi_auth *)p->s->auth_u, ids[i], e, sizeof(e));
        if (e[0]) {
            if (names[0])
                cl_cat(names, ", ", sizeof(names));
            cl_cat(names, ids[i], sizeof(names));
        }
    }
    timed_out = pi_auth_timed_out((pi_auth *)p->s->auth_u);
    pi_auth_deadline((pi_auth *)p->s->auth_u, 0);
    if (timed_out)
        pshow_note(&p->ps, PR_WARNING, "Warning: Model refresh timed out; searching cached models.");
    else if (names[0]) {
        cl_copy(line, "Warning: Could not refresh ", sizeof(line));
        cl_cat(line, names, sizeof(line));
        cl_cat(line, "; searching cached models.", sizeof(line));
        pshow_note(&p->ps, PR_WARNING, line);
    }
}

/* handleModelCommand: no text opens the selector; an exact provider/id or id switches, else (no scope) every catalog is refreshed and the
 * match tried again; anything else opens the selector searching for it */
static void model_command(pi_ui *p, cl_tui *t, const char *arg)
{
    pi_store *st = (pi_store *)p->s->command_u;
    pi_models *ms = st->models;
    if (!ms) {
        pshow_error(&p->ps, "models cannot be switched here");
        return;
    }
    if (arg[0]) {
        const llm_model *list[MS_MAX], *to;
        int n, scoped = model_candidates(ms, list, MS_MAX, &n);
        to = exact_model(ms, arg, list, n);
        if (!to && !scoped) {
            if (p->s->auth_u)
                model_refresh_all(p, ms);
            else
                pshow_note(&p->ps, PR_DIM, "Refreshing model catalogs\342\200\246");
            model_candidates(ms, list, MS_MAX, &n);
            to = exact_model(ms, arg, list, n);
        }
        if (to) {
            char err[200], line[300];
            if (pi_model_switch(ms, p->s, to, err, sizeof(err)))
                pshow_error(&p->ps, err);
            else {
                cl_copy(line, "Model: ", sizeof(line));
                cl_cat(line, to->id, sizeof(line));
                pshow_note(&p->ps, PR_DIM, line);
            }
            status(p);
            return;
        }
    }
    model_selector(p, t, arg);
}

/* ---- shared by the settings screens ---- */

/* the byte offset after `cells` cells of UTF-8 text (one cell a code point), the end of the text when it is shorter */
static long cell_off(const char *p, int cells)
{
    long i = 0;
    int w = 0;
    while (p[i] && w < cells) {
        i++;
        while (((unsigned char)p[i] & 0xc0) == 0x80)
            i++;
        w++;
    }
    return i;
}

/* wrapTextWithAnsi for plain text: lines of at most `width` cells, broken at the last space that fits (a longer word is cut), the
 * spaces at a break dropped; '\n' between the lines in out */
static void wrap_plain(const char *text, int width, char *out, long cap)
{
    const char *p = text;
    int first = 1;
    out[0] = 0;
    if (width < 1)
        width = 1;
    for (;;) {
        long lim = cell_off(p, width), take = lim, brk = lim, i;
        char line[400];
        if (p[lim]) {
            if (p[lim] != ' ') {
                for (i = lim; i > 0 && p[i] != ' '; i--)
                    ;
                if (i > 0) {
                    take = i;
                    brk = i;
                }
            }
        }
        if (take >= (long)sizeof(line))
            take = (long)sizeof(line) - 1;
        memcpy(line, p, (size_t)take);
        line[take] = 0;
        while (take > 0 && line[take - 1] == ' ')
            line[--take] = 0;
        if (!first)
            cl_cat(out, "\n", cap);
        cl_cat(out, line, cap);
        first = 0;
        p += brk;
        while (*p == ' ')
            p++;
        if (!*p)
            break;
    }
}

/* ---- /scoped-models ---- */

#define SM_MAX 1400

typedef struct smsel {
    pi_ui *p;
    pi_scope sc;
    int vis[SM_MAX];
    long score[SM_MAX];
    int dirty;
    char status[100];
    char refresh[160];
    int refresh_ok;
    char top[400];
    tui_pick pk;
} smsel;

static const char *sm_text(void *u, int i)
{
    static char t[900];
    smsel *s = (smsel *)u;
    const pi_scope_item *it = &s->sc.item[i];
    if (it->m)
        model_search_text(it->m, t, sizeof(t));
    else
        pi_scope_item_id(it, t, sizeof(t));
    return t;
}

/* refresh(): the list again from the state, the filter applied, the selection kept in range */
static void sm_refresh(smsel *s)
{
    s->pk.n = fuzzy_order(s->pk.query, s->sc.nitem, sm_text, s, s->vis, s->score);
    if (s->pk.sel > s->pk.n - 1)
        s->pk.sel = s->pk.n - 1;
    if (s->pk.sel < 0)
        s->pk.sel = 0;
}

static void sm_row(void *u, int i, int selected, char *out, long cap)
{
    smsel *s = (smsel *)u;
    const pi_scope_item *it = &s->sc.item[s->vis[i]];
    char id[400];
    out[0] = 0;
    if (selected)
        cat_role(s->p, PR_ACCENT, "\342\206\222 ", out, cap);
    else
        cl_cat(out, "  ", cap);
    if (it->m && it->enabled)
        cat_role(s->p, PR_ACCENT, "\342\234\223 ", out, cap);
    else
        cl_cat(out, "  ", cap);
    if (it->m)
        cl_copy(id, it->m->id, sizeof(id));
    else
        pi_scope_item_id(it, id, sizeof(id));
    if (!it->m) {                                   /* theme.strikethrough */
        char b[420];
        cl_copy(b, "\033[9m", sizeof(b));
        cl_cat(b, id, sizeof(b));
        cl_cat(b, "\033[29m", sizeof(b));
        cl_copy(id, b, sizeof(id));
    }
    if (selected)
        cat_role(s->p, PR_ACCENT, id, out, cap);
    else
        cl_cat(out, id, cap);
    if (it->m) {
        char b[200];
        cl_copy(b, " [", sizeof(b));
        cl_cat(b, it->m->provider, sizeof(b));
        cl_cat(b, "]", sizeof(b));
        cat_role(s->p, PR_MUTED, b, out, cap);
    } else
        cat_role(s->p, PR_MUTED, " [unavailable]", out, cap);
}

static void sm_tail(void *u, char *out, long cap)
{
    smsel *s = (smsel *)u;
    char foot[700], count[120], wrapped[900], *line, *nl;
    int cols = s->p->ui.tui->cols, k;
    static const char *const part[] = { "Enter toggle", "Ctrl+A all", "Ctrl+X clear", "Ctrl+P provider",
                                        "Option+Up/Option+Down reorder", "Ctrl+S save" };
    out[0] = 0;
    pick_scroll(s->p, &s->pk, 8, out, cap);
    if (!s->pk.n)
        cat_role(s->p, PR_MUTED, "  No matching models", out, cap);
    else {
        const pi_scope_item *it = &s->sc.item[s->vis[s->pk.sel]];
        char b[500];
        cl_cat(out, "\n", cap);
        if (it->m) {
            cl_copy(b, "  Model Name: ", sizeof(b));
            cl_cat(b, it->m->name && it->m->name[0] ? it->m->name : it->m->id, sizeof(b));
        } else
            cl_copy(b, "  Model unavailable", sizeof(b));
        cat_role(s->p, PR_MUTED, b, out, cap);
    }
    cl_cat(out, "\n\n", cap);
    if (s->refresh[0]) {
        char b[200];
        cl_copy(b, "  ", sizeof(b));
        cl_cat(b, s->refresh, sizeof(b));
        cat_role(s->p, s->refresh_ok ? PR_SUCCESS : PR_WARNING, b, out, cap);
        cl_cat(out, "\n", cap);
    }
    cl_copy(foot, "  ", sizeof(foot));
    for (k = 0; k < 6; k++) {
        cl_cat(foot, part[k], sizeof(foot));
        cl_cat(foot, " \302\267 ", sizeof(foot));
    }
    pi_scope_count_text(&s->sc, count, sizeof(count));
    cl_cat(foot, count, sizeof(foot));
    if (s->dirty)
        cl_cat(foot, " (unsaved)", sizeof(foot));
    wrap_plain(foot, cols, wrapped, sizeof(wrapped));
    for (line = wrapped;; line = nl + 1) {
        char row[300], *un;
        nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        un = strstr(line, "(unsaved)");
        if (un) {
            *un = 0;
            cl_copy(row, line, sizeof(row));
            cat_role(s->p, PR_DIM, row, out, cap);
            cat_role(s->p, PR_WARNING, "(unsaved)", out, cap);
        } else
            cat_role(s->p, PR_DIM, line, out, cap);
        if (!nl)
            break;
        cl_cat(out, "\n", cap);
    }
}

/* every filtered item's id, for the search-limited "all" and "clear" */
static int sm_targets(smsel *s, const char **ids, char (*store)[400])
{
    int i;
    for (i = 0; i < s->pk.n; i++) {
        pi_scope_item_id(&s->sc.item[s->vis[i]], store[i], 400);
        ids[i] = store[i];
    }
    return s->pk.n;
}

static int sm_changed(smsel *s)
{
    s->dirty = 1;
    pi_scope_apply(&s->sc);
    sm_refresh(s);
    return 1;
}

static int sm_key(void *u, const cl_key *key, int act)
{
    smsel *s = (smsel *)u;
    char id[400];
    (void)act;
    if (key->k == K_UP || key->k == K_DOWN) {
        if (!s->pk.n)
            return 1;
        if ((key->mods & KM_ALT) && s->sc.nitem) {                 /* app.models.reorderUp / reorderDown */
            int delta = key->k == K_UP ? -1 : 1;
            const pi_scope_item *it = &s->sc.item[s->vis[s->pk.sel]];
            if (!s->sc.en_null && it->enabled) {
                int at;
                pi_scope_item_id(it, id, sizeof(id));
                for (at = 0; at < s->sc.nen && strcmp(s->sc.en[at], id); at++)
                    ;
                if (at + delta >= 0 && at + delta < s->sc.nen) {
                    pi_scope_move(&s->sc, id, delta);
                    s->pk.sel += delta;
                    sm_changed(s);
                }
            }
            return 1;
        }
        if (key->k == K_UP)
            s->pk.sel = s->pk.sel == 0 ? s->pk.n - 1 : s->pk.sel - 1;
        else
            s->pk.sel = s->pk.sel == s->pk.n - 1 ? 0 : s->pk.sel + 1;
        return 1;
    }
    if (key->k == K_ENTER) {
        if (s->pk.n) {
            pi_scope_item_id(&s->sc.item[s->vis[s->pk.sel]], id, sizeof(id));
            pi_scope_toggle(&s->sc, id);
            sm_changed(s);
        }
        return 1;
    }
    if (key->k == K_CTRL && (key->ch == 'a' || key->ch == 'x')) {
        const char **ids = 0;
        char (*store)[400] = 0;
        int n = -1;
        if (s->pk.query[0]) {
            ids = (const char **)malloc(sizeof(char *) * (size_t)(s->pk.n + 1));
            store = (char (*)[400])malloc(400 * (size_t)(s->pk.n + 1));
            if (ids && store)
                n = sm_targets(s, ids, store);
        }
        if (key->ch == 'a')
            pi_scope_enable(&s->sc, ids, n);
        else
            pi_scope_clear(&s->sc, ids, n);
        free(ids);
        free(store);
        return sm_changed(s);
    }
    if (key->k == K_CTRL && key->ch == 'p') {
        if (s->pk.n && s->sc.item[s->vis[s->pk.sel]].m) {
            pi_scope_provider(&s->sc, s->sc.item[s->vis[s->pk.sel]].m->provider);
            sm_changed(s);
        }
        return 1;
    }
    if (key->k == K_CTRL && key->ch == 's') {
        pi_store *st = (pi_store *)s->p->s->command_u;
        if (!pi_scope_save(&s->sc, st->models->sys, st->models->agent_dir))
            cl_copy(s->status, "Model selection saved to settings", sizeof(s->status));
        s->dirty = 0;
        return 1;
    }
    if (key->k == K_CTRL && key->ch == 'c') {
        if (s->pk.query[0]) {
            s->pk.query[0] = 0;
            sm_refresh(s);
        } else
            s->pk.done = 1;
        return 1;
    }
    if (key->k == K_ESC) {
        s->pk.done = 1;
        return 1;
    }
    if (tui_pick_qkey(&s->pk, key))
        sm_refresh(s);
    return 1;
}

/* showModelsSelector */
static void scoped_selector(pi_ui *p, cl_tui *t)
{
    static smsel s;
    pi_store *st = (pi_store *)p->s->command_u;
    char rerr[300];
    memset(&s, 0, sizeof(s));
    s.p = p;
    rerr[0] = 0;
    if (p->s->auth_u)
        pi_radius_refresh((pi_auth *)p->s->auth_u, 0, rerr, sizeof(rerr));
    if (pi_scope_open(&s.sc, st->models)) {
        pshow_error(&p->ps, "out of memory");
        pi_scope_free(&s.sc);
        return;
    }
    if (rerr[0]) {
        cl_copy(s.refresh, "Could not refresh radius; showing cached models.", sizeof(s.refresh));
        s.refresh_ok = 0;
    } else {
        cl_copy(s.refresh, "Model catalogs refreshed.", sizeof(s.refresh));
        s.refresh_ok = 1;
    }
    s.top[0] = '\n';
    s.top[1] = 0;
    {
        char b[160];
        cl_copy(b, "\033[1m", sizeof(b));
        cat_role(p, PR_ACCENT, "Model Configuration", b, sizeof(b));
        cl_cat(s.top, b, sizeof(s.top));
        cl_cat(s.top, "\033[22m\n", sizeof(s.top));
        cat_role(p, PR_MUTED, "Session-only. Ctrl+S to save to settings.", s.top, sizeof(s.top));
        cl_cat(s.top, "\n", sizeof(s.top));
    }
    s.pk.top = s.top;
    s.pk.search = 1;
    s.pk.maxrows = 8;
    s.pk.row = sm_row;
    s.pk.key = sm_key;
    s.pk.tail = sm_tail;
    s.pk.u = &s;
    s.pk.layout = TUI_PICK_FRAME;
    sm_refresh(&s);
    tui_pick_run(t, &s.pk);
    if (s.status[0])
        pshow_note(&p->ps, PR_DIM, s.status);
    pi_scope_free(&s.sc);
    status(p);
}

/* ---- /thinking ---- */

typedef struct tsel {
    pi_ui *p;
    int lv[LT_N], n_all;         /* the levels the model supports, in order */
    int vis[LT_N];
    long score[LT_N];
    int cur, def, save;
    char desc[LT_N][80], text[LT_N][100];
    tui_pick pk;
} tsel;

static const char *const level_desc[LT_N] = { "No reasoning", "Very brief reasoning (~1k tokens)", "Light reasoning (~2k tokens)",
                                              "Moderate reasoning (~8k tokens)", "Deep reasoning (~16k tokens)",
                                              "Extra-high reasoning (~32k tokens)", "Maximum reasoning" };

static const char *ts_text(void *u, int i)
{
    return ((tsel *)u)->text[i];
}

static void ts_filter(tsel *t, int keep)
{
    int prev = t->pk.n ? t->vis[t->pk.sel] : -1, i;
    t->pk.n = fuzzy_order(t->pk.query, t->n_all, ts_text, t, t->vis, t->score);
    t->pk.sel = 0;
    if (keep)
        for (i = 0; i < t->pk.n; i++)
            if (t->vis[i] == prev)
                t->pk.sel = i;
}

static void ts_row(void *u, int i, int selected, char *out, long cap)
{
    tsel *t = (tsel *)u;
    int k = t->vis[i], w = 0, j, widest = 0;
    char label[64], pad[40];
    cl_copy(label, t->lv[k] == t->cur ? "\342\234\223 " : "  ", sizeof(label));
    cl_cat(label, llm_think_name(t->lv[k]), sizeof(label));
    for (j = 0; j < t->pk.n; j++) {
        int wl = 2 + (int)strlen(llm_think_name(t->lv[t->vis[j]]));
        if (wl > widest)
            widest = wl;
    }
    widest += 2;                                    /* PRIMARY_COLUMN_GAP */
    if (widest < 12)
        widest = 12;
    if (widest > 32)
        widest = 32;
    w = 2 + (int)strlen(llm_think_name(t->lv[k]));
    j = widest - w;
    if (j < 1)
        j = 1;
    memset(pad, ' ', (size_t)j);
    pad[j] = 0;
    out[0] = 0;
    if (selected) {
        char b[200];
        cl_copy(b, "\342\206\222 ", sizeof(b));
        cl_cat(b, label, sizeof(b));
        cl_cat(b, pad, sizeof(b));
        cl_cat(b, t->desc[k], sizeof(b));
        cat_role(t->p, PR_ACCENT, b, out, cap);
    } else {
        char b[200];
        cl_cat(out, "  ", cap);
        cl_cat(out, label, cap);
        cl_copy(b, pad, sizeof(b));
        cl_cat(b, t->desc[k], sizeof(b));
        cat_role(t->p, PR_DIM, b, out, cap);
    }
}

static void ts_tail(void *u, char *out, long cap)
{
    tsel *t = (tsel *)u;
    out[0] = 0;
    cl_cat(out, "\n", cap);
    cat_role(t->p, PR_DIM, "  Enter to select \302\267 Ctrl+S to set as default \302\267 Escape/Ctrl+C to cancel", out, cap);
}

static int ts_key(void *u, const cl_key *key, int act)
{
    tsel *t = (tsel *)u;
    if (key->k == K_CTRL && key->ch == 's') {
        t->save = 1;
        t->pk.done = 1;
        return 1;
    }
    if (pick_keeps(key, act))
        return 0;
    if (tui_pick_qkey(&t->pk, key))
        ts_filter(t, 1);
    return 1;
}

/* the status line of a thinking level change (selectThinkingLevel) */
static void thinking_set(pi_ui *p, int lv, int persist)
{
    pi_store *st = (pi_store *)p->s->command_u;
    char line[100];
    if (pi_thinking_set(st->models, p->s, lv) < 0) {
        pshow_error(&p->ps, "the thinking level change could not be recorded");
        return;
    }
    if (persist)
        pi_store_set_member(st->models->sys, st->models->agent_dir, "defaultThinkingLevel", llm_think_name(lv), 0, 0);
    cl_copy(line, persist ? "Default thinking level: " : "Thinking level: ", sizeof(line));
    cl_cat(line, llm_think_name(lv), sizeof(line));
    pshow_note(&p->ps, PR_DIM, line);
    status(p);
}

static void thinking_selector(pi_ui *p, cl_tui *t)
{
    static tsel s;
    pi_store *st = (pi_store *)p->s->command_u;
    unsigned av = llm_think_levels(&p->s->model);
    char *set;
    long sn = 0;
    int i, res;
    char top[300], key[48];
    memset(&s, 0, sizeof(s));
    s.p = p;
    s.cur = p->s->thinking;
    s.def = LT_OFF;
    set = pi_store_read(st->models->sys, st->models->agent_dir, "settings.json", &sn);
    if (set) {
        char *d = pi_jobj_str(set, sn, "defaultThinkingLevel");
        if (d && llm_think_of(d) >= 0)
            s.def = llm_think_of(d);
        free(d);
        free(set);
    }
    for (i = 0; i < LT_N; i++)
        if (av & (1u << i)) {
            s.lv[s.n_all] = i;
            cl_copy(s.desc[s.n_all], level_desc[i], sizeof(s.desc[0]));
            if (i == s.def)
                cl_cat(s.desc[s.n_all], " \302\267 default", sizeof(s.desc[0]));
            cl_copy(s.text[s.n_all], llm_think_name(i), sizeof(s.text[0]));
            cl_cat(s.text[s.n_all], " ", sizeof(s.text[0]));
            cl_cat(s.text[s.n_all], s.desc[s.n_all], sizeof(s.text[0]));
            s.n_all++;
        }
    cl_copy(top, "\nThinking Level\n\n", sizeof(top));
    key[0] = 0;
    km_key_text(&t->km, KC_CHAT, KA_THINK_CYCLE, key, sizeof(key));
    if (!key[0])
        cl_copy(key, "Shift+Tab", sizeof(key));
    else {
        /* keyDisplayText: "shift+tab" is shown as "Shift+Tab" */
        int k;
        for (k = 0; key[k]; k++)
            if ((k == 0 || key[k - 1] == '+') && key[k] >= 'a' && key[k] <= 'z')
                key[k] = (char)(key[k] - 32);
    }
    cl_cat(top, key, sizeof(top));
    cl_cat(top, " cycles thinking levels in-session\n", sizeof(top));
    s.pk.top = top;
    s.pk.search = 1;
    s.pk.maxrows = LT_N;
    s.pk.row = ts_row;
    s.pk.key = ts_key;
    s.pk.tail = ts_tail;
    s.pk.u = &s;
    s.pk.layout = TUI_PICK_FRAME;
    s.pk.n = fuzzy_order("", s.n_all, ts_text, &s, s.vis, s.score);
    for (i = 0; i < s.pk.n; i++)
        if (s.lv[s.vis[i]] == s.cur)
            s.pk.sel = i;
    res = tui_pick_run(t, &s.pk);
    if (res >= 0)
        thinking_set(p, s.lv[s.vis[res]], s.save);
    status(p);
}

/* handleThinkingCommand */
static void thinking_command(pi_ui *p, cl_tui *t, const char *arg)
{
    pi_store *st = (pi_store *)p->s->command_u;
    unsigned av = llm_think_levels(&p->s->model);
    int i, lv;
    char norm[40], msg[300];
    long l;
    if (!st->models) {
        pshow_error(&p->ps, "the thinking level cannot be changed here");
        return;
    }
    if (!arg[0]) {
        thinking_selector(p, t);
        return;
    }
    while (*arg == ' ')
        arg++;
    for (l = 0; arg[l] && l < (long)sizeof(norm) - 1; l++)
        norm[l] = (char)(arg[l] >= 'A' && arg[l] <= 'Z' ? arg[l] + 32 : arg[l]);
    norm[l] = 0;
    while (l > 0 && norm[l - 1] == ' ')
        norm[--l] = 0;
    lv = llm_think_of(norm);
    if (lv < 0 || !(av & (1u << lv))) {
        cl_copy(msg, "Unknown thinking level \"", sizeof(msg));
        cl_cat(msg, arg, sizeof(msg));
        cl_cat(msg, "\". Available levels: ", sizeof(msg));
        for (i = 0, l = 0; i < LT_N; i++)
            if (av & (1u << i)) {
                if (l++)
                    cl_cat(msg, ", ", sizeof(msg));
                cl_cat(msg, llm_think_name(i), sizeof(msg));
            }
        cl_cat(msg, ".", sizeof(msg));
        pshow_error(&p->ps, msg);
        return;
    }
    thinking_set(p, lv, 0);
}

/* ---- /settings ---- */

/* keyDisplayText: the first key bound to an action as pi shows it ("shift+tab" is Shift+Tab, "alt+enter" Option+Enter) */
static void key_display(cl_tui *t, int act, const char *dflt, char *out, long cap)
{
    char raw[48];
    int k, o = 0, start = 1;
    raw[0] = 0;
    km_key_text(&t->km, KC_CHAT, act, raw, sizeof(raw));
    if (!raw[0])
        cl_copy(raw, dflt, sizeof(raw));
    for (k = 0; raw[k] && o < cap - 8; k++) {
        if (start) {
            if (!strncmp(raw + k, "alt", 3) && (raw[k + 3] == '+' || !raw[k + 3])) {      /* macOS names the Alt key Option */
                memcpy(out + o, "Option", 6);
                o += 6;
                k += 2;
            } else
                out[o++] = (char)(raw[k] >= 'a' && raw[k] <= 'z' ? raw[k] - 32 : raw[k]);
            start = 0;
        } else {
            out[o++] = raw[k];
            start = raw[k] == '+';
        }
    }
    out[o] = 0;
}

#define ST_MAX 700
enum { SV_MAIN, SV_THEME, SV_AUTO, SV_CHOOSE, SV_WARN, SV_MODELS, SV_LEVELS };

typedef struct sitem {
    char label[200];            /* the text shown (plain) */
    char desc[240];
    char val[100];              /* a settings row's value */
    int tag;                    /* the table row, the model, the level */
    int mut;                    /* byte offset in label where a muted suffix starts, -1 none */
} sitem;

typedef struct sset {
    pi_ui *p;
    pi_store *st;
    cl_sys *sys;
    const char *dir;
    int images;                 /* the terminal draws images: the Show images and Image width rows are listed */
    sitem *it;
    int nit, cap;               /* the rows of the view on show, and how many the buffer holds */
    int vis[ST_MAX];
    long score[ST_MAX];
    int view, stack[6], sel_at[6], depth;
    char query_at[6][96];       /* the search text of each view below the one on show (an Input keeps its text) */
    char top[900];
    char title[120];
    int select_style;           /* a SelectList (rows "→ label  description") rather than a SettingsList */
    int searchable;
    int dirty_theme;
    char arrow[48];             /* the accent of the SettingsList cursor, fixed when the list is made (getSettingsListTheme) */
    /* the theme menus (ThemeSubmenu) */
    char names[40][64];
    int nnames;
    char orig[160];
    int tauto;                  /* the automatic menu is the one in use */
    char tsingle[64], tlight[64], tdark[64];
    int choose;                 /* SV_CHOOSE: 0 the light theme, 1 the dark one */
    /* the per-model thinking levels */
    int mt_model;               /* index into mods[] of the model being configured */
    const llm_model *mods[ST_MAX];
    int nmods;
    tui_pick pk;
} sset;

static const pi_ui_themes *ui_themes_src;

void pi_ui_set_themes(const pi_ui_themes *src)
{
    ui_themes_src = src;
}

static void ss_push(sset *s, int view)
{
    s->stack[s->depth] = s->view;
    cl_copy(s->query_at[s->depth], s->pk.query, sizeof(s->query_at[0]));
    s->pk.query[0] = 0;
    s->sel_at[s->depth++] = s->pk.sel;
    s->view = view;
}

/* ---- the rows of each view ---- */

static const char *ss_text(void *u, int i)
{
    static char t[500];
    sset *s = (sset *)u;
    cl_copy(t, s->it[i].label, sizeof(t));
    if (s->view == SV_MODELS || s->view == SV_LEVELS) {         /* fuzzyFilter(label + " " + description) */
        cl_cat(t, " ", sizeof(t));
        cl_cat(t, s->it[i].desc, sizeof(t));
    }
    return t;
}

/* the filter of the view's search box (pi's fuzzyFilter, an Input that does not move the cursor) */
static void ss_filter(sset *s, int keep)
{
    int prev = s->pk.n ? s->vis[s->pk.sel] : -1, i;
    s->pk.n = fuzzy_order(s->searchable ? s->pk.query : "", s->nit, ss_text, s, s->vis, s->score);
    s->pk.sel = 0;
    if (keep)
        for (i = 0; i < s->pk.n; i++)
            if (s->vis[i] == prev)
                s->pk.sel = i;
    s->pk.maxrows = s->view == SV_MAIN ? 10 : (s->pk.n < 10 ? (s->pk.n ? s->pk.n : 1) : 10);
}

/* the longest label of all the items, at most 36 (SettingsList.renderMainList) */
static int ss_label_width(const sset *s)
{
    int i, w = 0;
    for (i = 0; i < s->nit; i++) {
        int l = vw_width(s->it[i].label, (long)strlen(s->it[i].label));
        if (l > w)
            w = l;
    }
    return w > 36 ? 36 : w;
}

static void ss_row(void *u, int i, int selected, char *out, long cap)
{
    sset *s = (sset *)u;
    const sitem *it = &s->it[s->vis[i]];
    int cols = s->p->ui.tui->cols;
    out[0] = 0;
    if (!s->select_style) {
        int lw = ss_label_width(s), pad, vmax;
        char lab[200], val[120];
        cl_copy(lab, it->label, sizeof(lab));
        pad = lw - vw_width(lab, (long)strlen(lab));
        while (pad-- > 0)
            cl_cat(lab, " ", sizeof(lab));
        vmax = cols - (2 + lw + 2) - 2;
        cl_copy(val, it->val, sizeof(val));
        if (vw_width(val, (long)strlen(val)) > vmax && vmax > 0)
            val[cell_off(val, vmax)] = 0;
        if (selected) {
            cl_cat(out, s->arrow, cap);
            cl_cat(out, "\342\206\222 \033[0m", cap);
            cat_role(s->p, PR_ACCENT, lab, out, cap);
        } else {
            cl_cat(out, "  ", cap);
            cl_cat(out, lab, cap);
        }
        cl_cat(out, "  ", cap);
        cat_role(s->p, selected ? PR_ACCENT : PR_MUTED, val, out, cap);
    } else {
        /* SelectList: the primary column as wide as the widest label + 2, within the layout's bounds */
        int j, widest = 0, minw = s->view == SV_MODELS ? 12 : 12, maxw = s->view == SV_MODELS ? 46 : 32, w, gap;
        char pad[48];
        for (j = 0; j < s->pk.n; j++) {
            int lw = vw_width(s->it[s->vis[j]].label, (long)strlen(s->it[s->vis[j]].label)) + 2;
            if (lw > widest)
                widest = lw;
        }
        widest = widest < minw ? minw : widest > maxw ? maxw : widest;
        w = vw_width(it->label, (long)strlen(it->label));
        gap = it->desc[0] ? widest - w : 0;
        if (it->desc[0] && gap < 1)
            gap = 1;
        memset(pad, ' ', (size_t)(gap > 0 ? gap : 0));
        pad[gap > 0 ? gap : 0] = 0;
        if (selected) {
            char head[300];
            cl_copy(head, "\342\206\222 ", sizeof(head));
            if (it->mut >= 0) {                  /* the muted suffix of the label ends the accent: the rest is the default colour */
                char a[200], m[100];
                cl_copy(a, it->label, sizeof(a));
                a[it->mut] = 0;
                cl_copy(m, it->label + it->mut, sizeof(m));
                cl_cat(head, a, sizeof(head));
                cat_role(s->p, PR_ACCENT, head, out, cap);
                cat_role(s->p, PR_MUTED, m, out, cap);
                cl_cat(out, pad, cap);
                cl_cat(out, it->desc, cap);
            } else {
                cl_cat(head, it->label, sizeof(head));
                cl_cat(head, pad, sizeof(head));
                cl_cat(head, it->desc, sizeof(head));
                cat_role(s->p, PR_ACCENT, head, out, cap);
            }
        } else {
            char d[300];
            cl_cat(out, "  ", cap);
            if (it->mut >= 0) {
                char a[200], m[100];
                cl_copy(a, it->label, sizeof(a));
                a[it->mut] = 0;
                cl_copy(m, it->label + it->mut, sizeof(m));
                cl_cat(out, a, cap);
                cat_role(s->p, PR_MUTED, m, out, cap);
            } else
                cl_cat(out, it->label, cap);
            if (it->desc[0]) {
                cl_copy(d, pad, sizeof(d));
                cl_cat(d, it->desc, sizeof(d));
                cat_role(s->p, PR_MUTED, d, out, cap);
            }
        }
    }
}

static void ss_tail(void *u, char *out, long cap)
{
    sset *s = (sset *)u;
    int cols = s->p->ui.tui->cols;
    out[0] = 0;
    if (!s->select_style) {
        const char *hint = s->searchable ? "  Type to search \302\267 Enter/Space to change \302\267 Esc to cancel"
                                         : "  Enter/Space to change \302\267 Esc to cancel";
        int st, en;
        if (!s->pk.n) {
            cat_role(s->p, PR_DIM, "  No matching settings", out, cap);
            cl_cat(out, "\n\n", cap);
            cat_role(s->p, PR_DIM, hint, out, cap);
            return;
        }
        pick_window(&s->pk, s->pk.maxrows, &st, &en);
        if (st > 0 || en < s->pk.n) {
            char b[40], num[16];
            cl_copy(b, "  (", sizeof(b));
            cl_ltoa(s->pk.sel + 1, num);
            cl_cat(b, num, sizeof(b));
            cl_cat(b, "/", sizeof(b));
            cl_ltoa(s->pk.n, num);
            cl_cat(b, num, sizeof(b));
            cl_cat(b, ")", sizeof(b));
            cat_role(s->p, PR_DIM, b, out, cap);
            cl_cat(out, "\n", cap);
        }
        if (s->it[s->vis[s->pk.sel]].desc[0]) {
            char wr[900], *line, *nl, row[300];
            cl_cat(out, "\n", cap);
            wrap_plain(s->it[s->vis[s->pk.sel]].desc, cols - 4, wr, sizeof(wr));
            for (line = wr;; line = nl + 1) {
                nl = strchr(line, '\n');
                if (nl)
                    *nl = 0;
                cl_copy(row, "  ", sizeof(row));
                cl_cat(row, line, sizeof(row));
                cat_role(s->p, PR_DIM, row, out, cap);
                cl_cat(out, "\n", cap);
                if (!nl)
                    break;
            }
        }
        cl_cat(out, "\n", cap);
        cat_role(s->p, PR_DIM, hint, out, cap);
    } else {
        if (!s->pk.n)
            cat_role(s->p, PR_MUTED, "  No matching commands", out, cap);
        else
            pick_scroll(s->p, &s->pk, s->pk.maxrows, out, cap);
        cl_cat(out, "\n", cap);
        cat_role(s->p, PR_DIM, s->searchable ? "  Type to filter \302\267 Enter to select \302\267 Esc to go back"
                                              : "  Enter to select \302\267 Esc to go back", out, cap);
    }
}

/* the top lines of a SelectSubmenu: the title in bold and the accent colour, a blank row, the muted description, a blank row */
static void ss_head(sset *s, const char *title, const char *desc)
{
    char b[300];
    cl_copy(b, "\033[1m", sizeof(b));
    cat_role(s->p, PR_ACCENT, title, b, sizeof(b));
    cl_cat(b, "\033[22m", sizeof(b));
    cl_copy(s->top, b, sizeof(s->top));
    cl_cat(s->top, "\n", sizeof(s->top));
    if (desc && desc[0]) {
        cl_cat(s->top, "\n", sizeof(s->top));
        cat_role(s->p, PR_MUTED, desc, s->top, sizeof(s->top));
        cl_cat(s->top, "\n", sizeof(s->top));
    }
    s->pk.top = s->top;
}

static void ss_item(sset *s, const char *label, const char *desc, const char *val, int tag, int mut)
{
    sitem *it;
    if (s->nit >= s->cap)
        return;
    it = &s->it[s->nit++];
    cl_copy(it->label, label, sizeof(it->label));
    cl_copy(it->desc, desc ? desc : "", sizeof(it->desc));
    cl_copy(it->val, val ? val : "", sizeof(it->val));
    it->tag = tag;
    it->mut = mut;
}

/* ---- the settings the screen applies at once (the others take effect where the table says) ---- */

static void ss_themes_load(sset *s)
{
    s->nnames = ui_themes_src && ui_themes_src->list ? ui_themes_src->list(ui_themes_src->u, s->names, 40) : 0;
    if (s->nnames == 0) {
        cl_copy(s->names[0], "system", 64);
        s->nnames = 1;
    }
}

static int ss_theme_known(const sset *s, const char *name)
{
    int i;
    for (i = 0; i < s->nnames; i++)
        if (!strcmp(s->names[i], name))
            return 1;
    return 0;
}

/* preferredTheme */
static void ss_preferred(const sset *s, const char *pref, const char *fallback, char *out, long cap)
{
    if (pref && pref[0] && ss_theme_known(s, pref))
        cl_copy(out, pref, cap);
    else if (ss_theme_known(s, fallback))
        cl_copy(out, fallback, cap);
    else
        cl_copy(out, s->names[0], cap);
}

/* parseAutoThemeSetting: "light/dark", both non-empty, one slash */
static int ss_parse_auto(const char *setting, char *light, char *dark, long cap)
{
    const char *sl = strchr(setting, '/');
    char l[64], d[64];
    long n;
    if (!sl || strchr(sl + 1, '/'))
        return 0;
    n = (long)(sl - setting);
    if (n <= 0 || n >= (long)sizeof(l) || !sl[1])
        return 0;
    memcpy(l, setting, (size_t)n);
    l[n] = 0;
    cl_copy(d, sl + 1, sizeof(d));
    cl_copy(light, l, cap);
    cl_copy(dark, d, cap);
    return 1;
}

static void ss_theme_setting(const sset *s, char *out, long cap)
{
    if (s->tauto) {
        cl_copy(out, s->tlight, cap);
        cl_cat(out, "/", cap);
        cl_cat(out, s->tdark, cap);
    } else
        cl_copy(out, s->tsingle, cap);
}

/* onThemePreview: the screen's colours for a theme setting, nothing kept */
static void ss_theme_preview(sset *s, const char *setting)
{
    if (ui_themes_src && ui_themes_src->load && !ui_themes_src->load(ui_themes_src->u, setting, &s->p->theme)) {
        pshow_theme(&s->p->ps, &s->p->theme);
        s->dirty_theme = 1;
    }
}

/* ---- building the views ---- */

static void ss_build_main(sset *s, int keep_sel)
{
    long n = 0;
    char *text = s->dir ? pi_store_read(s->sys, s->dir, "settings.json", &n) : 0;
    int i, sel = s->pk.sel;
    char fk[40], tk[40];
    s->nit = 0;
    s->select_style = 0;
    s->searchable = 1;
    s->pk.search = 1;
    s->top[0] = 0;
    s->pk.top = s->top;
    key_display(s->p->ui.tui, KA_FOLLOW_UP, "alt+enter", fk, sizeof(fk));
    key_display(s->p->ui.tui, KA_THINK_CYCLE, "shift+tab", tk, sizeof(tk));
    for (i = 0; i < pi_settings_count(); i++) {
        const pi_setting *d = pi_settings_at(i);
        char desc[300], val[100];
        const char *src = d->desc;
        long o = 0;
        if ((d->flags & PSF_IMAGES) && !s->images)
            continue;
        desc[0] = 0;
        for (; src && *src && o < (long)sizeof(desc) - 40; src++) {
            if (src[0] == '%' && (src[1] == 'F' || src[1] == 'T')) {
                cl_cat(desc, src[1] == 'F' ? fk : tk, sizeof(desc));
                o = (long)strlen(desc);
                src++;
            } else {
                desc[o++] = *src;
                desc[o] = 0;
            }
        }
        if (d->kind == PSK_MENU) {
            if (!strcmp(d->id, "theme")) {
                pi_set_str(s->sys, s->dir, "theme", "system", val, sizeof(val));
            } else if (!strcmp(d->id, "model-thinking")) {
                jv root, sub, k, v;
                jit ji;
                long cnt = 0;
                if (text && !json_parse(text, n, &root) && json_get(root, "modelThinkingLevels", &sub) && json_type(sub) == J_OBJ) {
                    json_iter(sub, &ji);
                    while (json_next(&ji, &k, &v))
                        cnt++;
                }
                if (cnt) {
                    cl_ltoa(cnt, val);
                    cl_cat(val, " configured", sizeof(val));
                } else
                    cl_copy(val, "none", sizeof(val));
            } else
                cl_copy(val, d->dflt, sizeof(val));
        } else
            pi_setting_shown(d, text, n, val, sizeof(val));
        ss_item(s, d->label, desc, val, i, -1);
    }
    free(text);
    ss_filter(s, 0);
    if (keep_sel && sel < s->pk.n)
        s->pk.sel = sel;
}

static void ss_build_theme_single(sset *s)
{
    int i;
    char lab[100];
    s->nit = 0;
    s->select_style = 1;
    s->searchable = 0;
    s->pk.search = 0;
    ss_head(s, "Theme", "Select a theme, or choose automatic to follow terminal appearance.");
    for (i = 0; i < s->nnames; i++)
        if (!strcmp(s->names[i], "system")) {
            cl_copy(lab, !strcmp(s->names[i], s->tsingle) ? "\342\234\223 " : "  ", sizeof(lab));
            cl_cat(lab, "system", sizeof(lab));
            ss_item(s, lab, "Theme created from your terminal's colors", "system", i, -1);
        }
    ss_item(s, "  automatic", "Use separate themes for light and dark terminal appearance", "/", -1, -1);
    for (i = 0; i < s->nnames; i++)
        if (strcmp(s->names[i], "system")) {
            cl_copy(lab, !strcmp(s->names[i], s->tsingle) ? "\342\234\223 " : "  ", sizeof(lab));
            cl_cat(lab, s->names[i], sizeof(lab));
            ss_item(s, lab, "", s->names[i], i, -1);
        }
}

static void ss_build_theme_auto(sset *s)
{
    char b[300];
    s->nit = 0;
    s->select_style = 0;
    s->searchable = 0;
    s->pk.search = 0;
    cl_copy(s->top, "\033[1m", sizeof(s->top));
    cat_role(s->p, PR_ACCENT, "Automatic Theme", s->top, sizeof(s->top));
    cl_cat(s->top, "\033[22m\n\n", sizeof(s->top));
    cat_role(s->p, PR_MUTED, "Choose themes for terminal light and dark appearance.", s->top, sizeof(s->top));
    cl_cat(s->top, "\n", sizeof(s->top));
    cat_role(s->p, PR_MUTED, "Light/dark detection requires terminal support.", s->top, sizeof(s->top));
    cl_cat(s->top, "\n", sizeof(s->top));
    s->pk.top = s->top;
    (void)b;
    ss_item(s, "Light theme", "Theme to use in automatic mode when the terminal is light", s->tlight, 0, -1);
    ss_item(s, "Dark theme", "Theme to use in automatic mode when the terminal is dark", s->tdark, 1, -1);
    ss_item(s, "Apply", "Save and go back", "save and go back", 2, -1);
    ss_item(s, "Change mode", "Switch to one theme for light and dark", "switch to single theme", 3, -1);
}

static void ss_build_choose(sset *s)
{
    int i;
    const char *cur = s->choose ? s->tdark : s->tlight;
    s->nit = 0;
    s->select_style = 1;
    s->searchable = 0;
    s->pk.search = 0;
    ss_head(s, s->choose ? "Dark Theme" : "Light Theme",
            s->choose ? "Select the theme to use for dark terminal appearance" : "Select the theme to use for light terminal appearance");
    for (i = 0; i < s->nnames; i++) {
        char lab[100];
        cl_copy(lab, !strcmp(s->names[i], cur) ? "\342\234\223 " : "  ", sizeof(lab));
        cl_cat(lab, s->names[i], sizeof(lab));
        ss_item(s, lab, !strcmp(s->names[i], "system") ? "Theme created from your terminal's colors" : "", s->names[i], i, -1);
    }
}

static void ss_build_warn(sset *s)
{
    s->nit = 0;
    s->select_style = 0;
    s->searchable = 0;
    s->pk.search = 0;
    s->top[0] = 0;
    s->pk.top = s->top;
    ss_item(s, "Anthropic extra usage", "Warn when Anthropic subscription auth may use paid extra usage",
            pi_set_bool(s->sys, s->dir, "warnings.anthropicExtraUsage", 1) ? "true" : "false", 0, -1);
}

/* the model of "provider/id" being the session's or the default */
static void ss_build_models(sset *s)
{
    int i, rank[ST_MAX], j;
    char defp[100], defid[200], cur[300], key[300], lvl[40];
    pi_models *ms = s->st->models;
    long n = 0;
    char *text = pi_store_read(s->sys, s->dir, "settings.json", &n);
    s->nit = 0;
    s->select_style = 1;
    s->searchable = 1;
    s->pk.search = 1;
    ss_head(s, "Per-Model Thinking Level", "Step 1/2 \302\267 Select a model to configure");
    s->nmods = 0;
    defp[0] = defid[0] = 0;
    pi_set_str(s->sys, s->dir, "defaultProvider", "", defp, sizeof(defp));
    pi_set_str(s->sys, s->dir, "defaultModel", "", defid, sizeof(defid));
    pi_scope_id(&s->p->s->model, cur, sizeof(cur));
    for (i = 0; i < ms->reg.n && s->nmods < ST_MAX; i++)
        if (pi_model_usable(ms, &ms->reg.m[i]))
            s->mods[s->nmods++] = &ms->reg.m[i];
    for (i = 0; i < s->nmods; i++) {
        pi_scope_id(s->mods[i], key, sizeof(key));
        rank[i] = !strcmp(key, cur) ? 0 : (defp[0] && !strcmp(s->mods[i]->provider, defp) && !strcmp(s->mods[i]->id, defid)) ? 1 : 2;
    }
    /* the current model first, the default second, then by provider (a stable sort) */
    for (i = 1; i < s->nmods; i++) {
        const llm_model *x = s->mods[i];
        int rx = rank[i];
        for (j = i; j > 0 && (rank[j - 1] > rx || (rank[j - 1] == rx && rx == 2 && ms_pcmp(s->mods[j - 1]->provider, x->provider) > 0)); j--) {
            s->mods[j] = s->mods[j - 1];
            rank[j] = rank[j - 1];
        }
        s->mods[j] = x;
        rank[j] = rx;
    }
    if (!s->nmods)
        ss_item(s, "No models available", "Log in to a provider or configure an API key first", "__none__", -1, -1);
    for (i = 0; i < s->nmods; i++) {
        char lab[300];
        jv root, sub, v;
        pi_scope_id(s->mods[i], key, sizeof(key));
        lvl[0] = 0;
        if (text && !json_parse(text, n, &root) && json_get(root, "modelThinkingLevels", &sub) && json_type(sub) == J_OBJ) {
            jit ji;
            jv k;
            json_iter(sub, &ji);
            while (json_next(&ji, &k, &v)) {
                char kk[300];
                json_str(k, kk, sizeof(kk));
                if (!strcmp(kk, key) && json_type(v) == J_STR)
                    json_str(v, lvl, sizeof(lvl));
            }
        }
        cl_copy(lab, s->mods[i]->id, sizeof(lab));
        cl_cat(lab, " [", sizeof(lab));
        cl_cat(lab, s->mods[i]->provider, sizeof(lab));
        cl_cat(lab, "]", sizeof(lab));
        ss_item(s, lab, lvl, key, i, (int)strlen(s->mods[i]->id) + 1);
    }
    free(text);
    ss_filter(s, 0);
    /* preselect: currentModelKey ?? currentDefaultModelKey */
    for (i = 0; i < s->pk.n; i++)
        if (s->it[s->vis[i]].tag >= 0 && !strcmp(s->it[s->vis[i]].val, cur)) {
            s->pk.sel = i;
            break;
        }
}

static void ss_build_levels(sset *s)
{
    const llm_model *m = s->mods[s->mt_model];
    unsigned av = llm_think_levels(m);
    int i, active = -1;
    char key[300], t[200], lvl[40], gdef[40];
    long n = 0;
    char *text = pi_store_read(s->sys, s->dir, "settings.json", &n);
    jv root, sub, v;
    s->nit = 0;
    s->select_style = 1;
    s->searchable = 0;
    s->pk.search = 0;
    pi_scope_id(m, key, sizeof(key));
    cl_copy(t, "Thinking Level for ", sizeof(t));
    cl_cat(t, m->id, sizeof(t));
    cl_cat(t, " [", sizeof(t));
    cl_cat(t, m->provider, sizeof(t));
    cl_cat(t, "]", sizeof(t));
    ss_head(s, t, "Step 2/2 \302\267 Select default thinking level for this model");
    lvl[0] = 0;
    if (text && !json_parse(text, n, &root) && json_get(root, "modelThinkingLevels", &sub) && json_type(sub) == J_OBJ) {
        jit ji;
        jv k;
        json_iter(sub, &ji);
        while (json_next(&ji, &k, &v)) {
            char kk[300];
            json_str(k, kk, sizeof(kk));
            if (!strcmp(kk, key) && json_type(v) == J_STR)
                json_str(v, lvl, sizeof(lvl));
        }
    }
    free(text);
    if (lvl[0])
        active = llm_think_of(lvl);
    for (i = 0; i < LT_N; i++)
        if (av & (1u << i)) {
            char lab[60];
            cl_copy(lab, i == active ? "\342\234\223 " : "  ", sizeof(lab));
            cl_cat(lab, llm_think_name(i), sizeof(lab));
            ss_item(s, lab, level_desc[i], llm_think_name(i), i, -1);
        }
    if (lvl[0]) {
        char d[120];
        pi_set_str(s->sys, s->dir, "defaultThinkingLevel", "medium", gdef, sizeof(gdef));
        cl_copy(d, "Revert to global default (", sizeof(d));
        cl_cat(d, gdef, sizeof(d));
        cl_cat(d, ")", sizeof(d));
        ss_item(s, "  (clear override)", d, "__clear__", -1, -1);
    }
    ss_filter(s, 0);
    for (i = 0; i < s->pk.n; i++)
        if (s->it[s->vis[i]].tag == active && active >= 0)
            s->pk.sel = i;
}

/* the view the stack's top names, built afresh */
static void ss_build(sset *s)
{
    switch (s->view) {
    case SV_MAIN:
        ss_build_main(s, 0);
        break;
    case SV_THEME:
        ss_build_theme_single(s);
        ss_filter(s, 0);
        break;
    case SV_AUTO:
        ss_build_theme_auto(s);
        ss_filter(s, 0);
        break;
    case SV_CHOOSE:
        ss_build_choose(s);
        ss_filter(s, 0);
        break;
    case SV_WARN:
        ss_build_warn(s);
        ss_filter(s, 0);
        break;
    case SV_MODELS:
        ss_build_models(s);
        break;
    case SV_LEVELS:
        ss_build_levels(s);
        break;
    }
}

/* back one view (Esc): the parent, built again, its selection restored */
static void ss_pop(sset *s)
{
    int sel;
    if (!s->depth) {
        s->pk.done = 1;
        return;
    }
    s->view = s->stack[--s->depth];
    sel = s->sel_at[s->depth];
    cl_copy(s->pk.query, s->query_at[s->depth], sizeof(s->pk.query));
    ss_build(s);
    if (s->view != SV_MODELS && sel < s->pk.n)       /* the model step comes back with its own preselection (SteppedSubmenu.buildStep) */
        s->pk.sel = sel;
}

/* ---- the keys ---- */

static void ss_preview_selection(sset *s)
{
    if (s->view == SV_THEME && s->pk.n) {
        const char *v = s->it[s->vis[s->pk.sel]].val;
        if (!strcmp(v, "/")) {
            char a[160];
            cl_copy(a, s->tlight, sizeof(a));
            cl_cat(a, "/", sizeof(a));
            cl_cat(a, s->tdark, sizeof(a));
            ss_theme_preview(s, a);
        } else
            ss_theme_preview(s, v);
    } else if (s->view == SV_CHOOSE && s->pk.n)
        ss_theme_preview(s, s->it[s->vis[s->pk.sel]].val);
}

/* SettingsList.activateItem and the callbacks of SettingsSelectorComponent */
static void ss_apply_value(sset *s, const pi_setting *d, const char *shown)
{
    pi_setting_write(s->sys, s->dir, d, shown);
    settings_live(s->p, s->st, d->id);
}

static void ss_theme_done(sset *s, const char *setting)
{
    /* onDone(setting): the main list's Theme row takes it and onThemeChange saves it */
    s->dirty_theme = 1;
    if (setting) {
        const pi_setting *d = pi_settings_find("theme");
        jw v;
        jw_init(&v);
        jw_strz(&v, setting);
        pi_store_set_path(s->sys, s->dir, d->path, v.p);
        jw_free(&v);
        settings_live(s->p, s->st, "theme");
    } else
        ss_theme_preview(s, s->orig);
    cl_copy(s->pk.query, s->query_at[0], sizeof(s->pk.query));
    s->depth = 0;
    s->view = SV_MAIN;
    ss_build_main(s, 0);
    {
        int i;
        for (i = 0; i < s->pk.n; i++)
            if (!strcmp(pi_settings_at(s->it[s->vis[i]].tag)->id, "theme"))
                s->pk.sel = i;
    }
}

static void ss_activate(sset *s)
{
    const sitem *it;
    if (!s->pk.n)
        return;
    it = &s->it[s->vis[s->pk.sel]];
    switch (s->view) {
    case SV_MAIN: {
        const pi_setting *d = pi_settings_at(it->tag);
        char next[100];
        if (d->kind == PSK_MENU) {
            if (!strcmp(d->id, "theme")) {
                char cur[160], l[64], dk[64];
                ss_themes_load(s);
                cl_copy(cur, it->val, sizeof(cur));
                cl_copy(s->orig, cur, sizeof(s->orig));
                if (ss_parse_auto(cur, l, dk, 64)) {
                    s->tauto = 1;
                    ss_preferred(s, l, "system", s->tlight, sizeof(s->tlight));
                    ss_preferred(s, dk, "system", s->tdark, sizeof(s->tdark));
                    ss_preferred(s, dk, "system", s->tsingle, sizeof(s->tsingle));      /* the terminal reports no appearance: dark */
                } else {
                    s->tauto = 0;
                    ss_preferred(s, cur, "system", s->tsingle, sizeof(s->tsingle));
                    ss_preferred(s, strchr(cur, '/') ? "" : cur, "system", s->tlight, sizeof(s->tlight));
                    cl_copy(s->tdark, s->tlight, sizeof(s->tdark));
                }
                ss_push(s, s->tauto ? SV_AUTO : SV_THEME);
                ss_build(s);
                if (s->view == SV_THEME) {
                    int i;
                    for (i = 0; i < s->pk.n; i++)
                        if (!strcmp(s->it[s->vis[i]].val, s->tsingle))
                            s->pk.sel = i;
                }
            } else if (!strcmp(d->id, "warnings")) {
                ss_push(s, SV_WARN);
                ss_build(s);
            } else {
                ss_push(s, SV_MODELS);
                ss_build(s);
            }
            return;
        }
        pi_setting_next(d, it->val, next, sizeof(next));
        ss_apply_value(s, d, next);
        ss_build_main(s, 1);
        return;
    }
    case SV_THEME:
        if (!strcmp(it->val, "/")) {
            s->tauto = 1;
            {
                char a[160];
                cl_copy(a, s->tlight, sizeof(a));
                cl_cat(a, "/", sizeof(a));
                cl_cat(a, s->tdark, sizeof(a));
                ss_theme_preview(s, a);
            }
            s->view = SV_AUTO;
            ss_build(s);
        } else {
            cl_copy(s->tsingle, it->val, sizeof(s->tsingle));
            ss_theme_done(s, s->tsingle);
        }
        return;
    case SV_AUTO:
        if (it->tag == 0 || it->tag == 1) {
            s->choose = it->tag;
            ss_push(s, SV_CHOOSE);
            ss_build(s);
            {
                int i;
                const char *cur = s->choose ? s->tdark : s->tlight;
                for (i = 0; i < s->pk.n; i++)
                    if (!strcmp(s->it[s->vis[i]].val, cur))
                        s->pk.sel = i;
            }
        } else if (it->tag == 2) {
            char a[160];
            ss_theme_setting(s, a, sizeof(a));
            ss_theme_done(s, a);
        } else {
            s->tauto = 0;
            cl_copy(s->tsingle, s->tdark, sizeof(s->tsingle));              /* getActiveAutomaticTheme: dark */
            ss_theme_preview(s, s->tsingle);
            s->view = SV_THEME;
            ss_build(s);
            {
                int i;
                for (i = 0; i < s->pk.n; i++)
                    if (!strcmp(s->it[s->vis[i]].val, s->tsingle))
                        s->pk.sel = i;
            }
        }
        return;
    case SV_CHOOSE:
        if (s->choose)
            cl_copy(s->tdark, it->val, sizeof(s->tdark));
        else
            cl_copy(s->tlight, it->val, sizeof(s->tlight));
        {
            char a[160];
            ss_theme_setting(s, a, sizeof(a));
            ss_theme_preview(s, a);
        }
        ss_pop(s);
        return;
    case SV_WARN: {
        int on = !strcmp(it->val, "true");
        pi_store_set_path(s->sys, s->dir, "warnings.anthropicExtraUsage", on ? "false" : "true");
        ss_build_warn(s);
        ss_filter(s, 0);
        return;
    }
    case SV_MODELS:
        if (it->tag < 0)
            return;
        s->mt_model = it->tag;
        ss_push(s, SV_LEVELS);
        ss_build(s);
        return;
    case SV_LEVELS: {
        char key[300];
        pi_scope_id(s->mods[s->mt_model], key, sizeof(key));
        if (!strcmp(it->val, "__clear__"))
            pi_store_set_sub(s->sys, s->dir, "modelThinkingLevels", key, 0);
        else {
            jw v;
            jw_init(&v);
            jw_strz(&v, it->val);
            pi_store_set_sub(s->sys, s->dir, "modelThinkingLevels", key, v.p);
            jw_free(&v);
        }
        /* loop: true -- the first step again with the new summary */
        s->depth = s->depth > 0 ? s->depth - 1 : 0;
        s->view = SV_MODELS;
        ss_build(s);
        return;
    }
    }
}

static int ss_key(void *u, const cl_key *key, int act)
{
    sset *s = (sset *)u;
    (void)act;
    if (key->k == K_UP || key->k == K_DOWN) {
        if (!s->pk.n)
            return 1;
        if (key->k == K_UP)
            s->pk.sel = s->pk.sel == 0 ? s->pk.n - 1 : s->pk.sel - 1;
        else
            s->pk.sel = s->pk.sel == s->pk.n - 1 ? 0 : s->pk.sel + 1;
        ss_preview_selection(s);
        return 1;
    }
    if (key->k == K_ENTER || (key->k == K_CHAR && key->ch == ' ' && (s->view == SV_MAIN || s->view == SV_AUTO || s->view == SV_WARN) &&
                              (!s->searchable || !s->pk.query[0]))) {
        ss_activate(s);
        return 1;
    }
    if (key->k == K_ESC || (key->k == K_CTRL && key->ch == 'c')) {
        switch (s->view) {
        case SV_MAIN:
            s->pk.done = 1;
            break;
        case SV_THEME:
        case SV_AUTO:
            ss_theme_done(s, 0);
            break;
        case SV_CHOOSE: {
            char a[160];
            ss_theme_setting(s, a, sizeof(a));
            ss_theme_preview(s, a);
            ss_pop(s);
            break;
        }
        case SV_LEVELS:
            ss_pop(s);
            break;
        case SV_MODELS:
        case SV_WARN:
            ss_pop(s);
            ss_build_main(s, 1);
            break;
        }
        return 1;
    }
    if (s->searchable && tui_pick_qkey(&s->pk, key))
        ss_filter(s, 0);
    return 1;
}

/* showSettingsSelector */
static void settings_command(pi_ui *p, cl_tui *t)
{
    static sset s;
    memset(&s, 0, sizeof(s));
    s.p = p;
    s.st = (pi_store *)p->s->command_u;
    s.sys = s.st->models ? s.st->models->sys : 0;
    s.dir = s.st->models ? s.st->models->agent_dir : 0;
    s.images = ui_images;
    {
        /* room for the settings and for every model the per-model menu lists */
        int i, nm = 0;
        if (s.st->models)
            for (i = 0; i < s.st->models->reg.n; i++)
                if (pi_model_usable(s.st->models, &s.st->models->reg.m[i]))
                    nm++;
        s.cap = 48 + (nm < ST_MAX - 48 ? nm : ST_MAX - 48);
    }
    s.it = (sitem *)calloc((size_t)s.cap, sizeof(sitem));
    if (!s.it || !s.sys || !s.dir) {
        free(s.it);
        pshow_error(&p->ps, "settings cannot be changed here");
        return;
    }
    s.view = SV_MAIN;
    pitheme_sgr(&p->theme, PR_ACCENT, s.arrow);
    s.pk.row = ss_row;
    s.pk.key = ss_key;
    s.pk.tail = ss_tail;
    s.pk.u = &s;
    s.pk.layout = TUI_PICK_FRAME;
    ss_build_main(&s, 0);
    tui_pick_run(t, &s.pk);
    free(s.it);
    if (s.dirty_theme || p->redraw_after)
        redraw_session(p);
    p->redraw_after = 0;
    status(p);
}

/* the settings the editor's first frame already draws by (editor padding, menu rows, double Esc, hardware cursor): read before tui_start as well */
static void tui_settings_live(cl_tui *t, cl_sys *sys, const char *dir, const char *id)
{
    char v[40];
    if ((!id || !strcmp(id, "editor-padding"))) {
        long x = pi_set_num(sys, dir, "editorPaddingX", 0);
        t->pi_edpad = x < 0 ? 0 : x > 3 ? 3 : (int)x;
    }
    if ((!id || !strcmp(id, "autocomplete-max-visible"))) {
        long x = pi_set_num(sys, dir, "autocompleteMaxVisible", 5);
        t->pi_menu_max = x < 3 ? 3 : x > 20 ? 20 : (int)x;
    }
    if ((!id || !strcmp(id, "double-escape-action"))) {
        pi_set_str(sys, dir, "doubleEscapeAction", "tree", v, sizeof(v));
        t->pi_dbl_esc = !strcmp(v, "fork") ? 1 : !strcmp(v, "none") ? 2 : 0;
    }
    if ((!id || !strcmp(id, "show-hardware-cursor")))
        t->pi_hwcursor = pi_set_bool(sys, dir, "showHardwareCursor", 0);
}

/* what a changed setting does to the running screen and session (id: the table's id of the row, 0 every one: the start); the settings the table
 * calls "next start" are read where they start */
static void settings_live(pi_ui *p, pi_store *st, const char *id)
{
    cl_sys *sys = st->models->sys;
    const char *dir = st->models->agent_dir;
    cl_tui *t = p->ui.tui;
    pi_settings_session(sys, dir, p->s, st, id);
    if (!id || !strcmp(id, "autocompact"))
        status(p);
    if (t)
        tui_settings_live(t, sys, dir, id);
    if (id && !strcmp(id, "skill-commands")) {          /* setupAutocompleteProvider: the skills read again, the menu's /skill:name rows follow */
        char err[200];
        reload_resources(p, err, sizeof(err));
    }
    if (id && !strcmp(id, "output-padding")) {
        long x = pi_set_num(sys, dir, "outputPad", 1);
        pshow_set_pad(&p->ps, x == 0 ? 0 : 1);
        p->redraw_after = 1;
    }
    if (id && !strcmp(id, "theme")) {
        char th[160];
        pi_set_str(sys, dir, "theme", "system", th, sizeof(th));
        if (ui_themes_src && ui_themes_src->load && !ui_themes_src->load(ui_themes_src->u, th, &p->theme)) {
            pshow_theme(&p->ps, &p->theme);
            status(p);
        }
    }
}

/* ---- /fork ---- */

typedef struct fsel {
    pi_ui *p;
    int *idx;                    /* tree indexes of the user messages on the active path, oldest first */
    char (*text)[300];           /* each message as one line */
    int n;
    tui_pick pk;
} fsel;

static void fs_row(void *u, int i, int selected, char *out, long cap)
{
    fsel *f = (fsel *)u;
    const char *tx = f->text[i];
    int width = f->p->ui.tui->cols - 2;
    char cut[320], b[100], num[16];
    long k = 0, w = 0;
    out[0] = 0;
    /* truncateToWidth(text, width - 2): cut at the column, "…" in place of the last when it does not fit */
    if (tui_width(tx, (long)strlen(tx)) <= width - 2)
        cl_copy(cut, tx, sizeof(cut));
    else {
        long used = 0;
        while (tx[k]) {
            long l = 1;
            int cw;
            while ((tx[k + l] & 0xc0) == 0x80)
                l++;
            cw = tui_width(tx + k, l);
            if (used + cw > width - 2 - 1)
                break;
            used += cw;
            k += l;
        }
        memcpy(cut, tx, (size_t)k);
        cut[k] = 0;
        cl_cat(cut, "\342\200\246", sizeof(cut));
    }
    (void)w;
    if (selected) {
        char bold[300];
        cat_role(f->p, PR_ACCENT, "\342\200\272 ", out, cap);
        cl_copy(bold, "\033[1m", sizeof(bold));
        cl_cat(bold, cut, sizeof(bold));
        cl_cat(bold, "\033[22m", sizeof(bold));
        cl_cat(out, bold, cap);
    } else {
        cl_cat(out, "  ", cap);
        cl_cat(out, cut, cap);
    }
    cl_cat(out, "\n", cap);
    cl_copy(b, "  Message ", sizeof(b));
    cl_ltoa(i + 1, num);
    cl_cat(b, num, sizeof(b));
    cl_cat(b, " of ", sizeof(b));
    cl_ltoa(f->n, num);
    cl_cat(b, num, sizeof(b));
    cat_role(f->p, PR_MUTED, b, out, cap);
    cl_cat(out, "\n", cap);
}

static void fs_tail(void *u, char *out, long cap)
{
    fsel *f = (fsel *)u;
    out[0] = 0;
    pick_scroll(f->p, &f->pk, 10, out, cap);
}

/* showUserMessageSelector (user-message-selector.ts) and the fork it asks for: /fork N of cmds.c with the chat drawn again */
static void fork_command(pi_ui *p, cl_tui *t)
{
    static fsel f;
    pi_store *st = (pi_store *)p->s->command_u;
    pi_s3 *s = &st->s3;
    int *path = 0, np, i, res;
    char top[600], cols_desc[400];
    memset(&f, 0, sizeof(f));
    f.p = p;
    np = s->n;                                  /* getUserMessagesForForking: every user message of the file, the other branches too */
    path = (int *)malloc(sizeof(int) * (size_t)(np ? np : 1));
    for (i = 0; i < np && path; i++)
        path[i] = i;
    f.idx = (int *)malloc(sizeof(int) * (size_t)(np ? np : 1));
    f.text = (char (*)[300])malloc(sizeof(char[300]) * (size_t)(np ? np : 1));
    if (!f.idx || !f.text) {
        free(f.idx);
        free(f.text);
        free(path);
        return;
    }
    for (i = 0; i < np; i++)
        if (s->e[path[i]].type == S3_MESSAGE && s->e[path[i]].role == S3R_USER) {
            jw w;
            long j, k = 0;
            jw_init(&w);
            pi_user_text(s, path[i], &w);
            /* text.replace(/\n/g, " ").trim() */
            for (j = 0; j < w.n && k < 299; j++)
                f.text[f.n][k++] = w.p[j] == '\n' ? ' ' : w.p[j];
            f.text[f.n][k] = 0;
            while (k > 0 && (f.text[f.n][k - 1] == ' ' || f.text[f.n][k - 1] == '\t'))
                f.text[f.n][--k] = 0;
            for (j = 0; f.text[f.n][j] == ' ' || f.text[f.n][j] == '\t'; j++)
                ;
            if (j)
                memmove(f.text[f.n], f.text[f.n] + j, strlen(f.text[f.n] + j) + 1);
            f.idx[f.n++] = path[i];
            jw_free(&w);
        }
    free(path);
    if (!f.n) {
        pshow_note(&p->ps, PR_DIM, "No messages to fork from");
        free(f.idx);
        free(f.text);
        return;
    }
    {
        /* Spacer, the title (padding 1), the muted description wrapped at the width less 2, Spacer, rule, Spacer */
        char bold[60], mut[48], wrapped[500];
        const char *d = "Select a user message to copy the active path up to that point into a new session";
        int width = t->cols - 2, col = 0;
        long k = 0;
        const char *w = d;
        wrapped[0] = 0;
        top[0] = 2;
        top[1] = 0;
        cl_cat(top, "\n \033[1mFork from Message\033[22m\n", sizeof(top));
        (void)bold;
        cl_copy(cols_desc, " ", sizeof(cols_desc));
        while (*w) {
            const char *e = w;
            long wl;
            while (*e && *e != ' ')
                e++;
            wl = (long)(e - w);
            if (col && col + 1 + wl > width) {
                cl_cat(cols_desc, "\n ", sizeof(cols_desc));
                col = 0;
            } else if (col) {
                cl_cat(cols_desc, " ", sizeof(cols_desc));
                col++;
            }
            memcpy(wrapped, w, (size_t)wl);
            wrapped[wl] = 0;
            cl_cat(cols_desc, wrapped, sizeof(cols_desc));
            col += (int)wl;
            w = *e ? e + 1 : e;
        }
        (void)k;
        (void)mut;
        {
            char dsc[500];
            role_sg(p, PR_MUTED, mut);
            /* the colour is set on each wrapped row: a row of its own ends it */
            dsc[0] = 0;
            {
                const char *a = cols_desc;
                for (;;) {
                    const char *nl = strchr(a, '\n');
                    long l = nl ? (long)(nl - a) : (long)strlen(a);
                    char one[200];
                    memcpy(one, a, (size_t)l);
                    one[l] = 0;
                    cl_cat(dsc, " ", sizeof(dsc));                  /* the padding is not in the colour */
                    cat_role(p, PR_MUTED, one + (one[0] == ' '), dsc, sizeof(dsc));
                    if (!nl)
                        break;
                    cl_cat(dsc, "\n", sizeof(dsc));
                    a = nl + 1;
                }
            }
            cl_cat(top, dsc, sizeof(top));
        }
        cl_cat(top, "\n\n\001\n", sizeof(top));
    }
    f.pk.top = top;
    f.pk.search = 0;
    f.pk.maxrows = 10;
    f.pk.row = fs_row;
    f.pk.tail = fs_tail;
    f.pk.u = &f;
    f.pk.layout = TUI_PICK_FRAME;
    f.pk.n = f.n;
    f.pk.sel = f.n - 1;
    res = tui_pick_run(t, &f.pk);
    if (res >= 0) {
        jw reply, editor;
        int rc;
        jw_init(&reply);
        jw_init(&editor);
        rc = pi_store_fork_at(st, f.idx[res], &reply, &editor);
        if (rc > 0)
            ;                                   /* an extension cancelled it */
        else if (rc < 0 && reply.p && strstr(reply.p, "[ERROR]"))
            pshow_error(&p->ps, strstr(reply.p, "[ERROR]") + 8);
        else {
            pshow_clear(&p->ps);
            redraw_session(p);
            tui_set_text(t, editor.n ? editor.p : "");
            pshow_note(&p->ps, PR_DIM, "Forked to new session");
        }
        jw_free(&reply);
        jw_free(&editor);
    }
    free(f.idx);
    free(f.text);
    status(p);
}

/* /changelog (interactive-mode.ts handleChangelogCommand): every entry, oldest first, in the What's New block */
static void changelog_command(pi_ui *p)
{
    jw all;
    int n = 0;
    jw_init(&all);
    if (ui_cl_file)
        n = pi_cl_all(ui_cl_file, ui_cl_file_n, &all);
    if (!n) {
        jw_reset(&all);
        jw_rawz(&all, "No changelog entries found.");
    }
    if (p->look)
        pshow_news(&p->ps, 2, all.p, all.n);
    else
        ui_line(&p->ui, all.p);
    jw_free(&all);
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
    if (p->look && t && p->s->command) {
        /* the selectors of pi's look: /model and /thinking with or without a word, /fork alone (a number still forks, as in the line mode) */
        if (!strcmp(line, "/model") || !strncmp(line, "/model ", 7)) {
            model_command(p, t, line[6] ? line + 7 : line + 6);
            return 1;
        }
        if (!strcmp(line, "/thinking") || !strncmp(line, "/thinking ", 10)) {
            thinking_command(p, t, line[9] ? line + 10 : line + 9);
            return 1;
        }
        if (!strcmp(line, "/fork")) {
            fork_command(p, t);
            return 1;
        }
        if (!strcmp(line, "/scoped-models") && ((pi_store *)p->s->command_u)->models) {
            scoped_selector(p, t);
            return 1;
        }
        if (!strcmp(line, "/settings") && ((pi_store *)p->s->command_u)->models) {
            settings_command(p, t);
            return 1;
        }
    }
    if (!strcmp(line, "/changelog")) {
        changelog_command(p);
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
        redraw_session(&p);
    if (ui_cl_news && ui_cl_news_n) {
        /* showStartupNoticesIfNeeded: the new entries once, after the loaded resources */
        if (p.look)
            pshow_news(&p.ps, ui_cl_collapsed ? 1 : 0, ui_cl_news, ui_cl_news_n);
        else if (ui_cl_collapsed) {
            char ver[24], l[100];
            pi_cl_latest(ui_cl_news, ver, sizeof(ver));
            cl_copy(l, "Updated to v", sizeof(l));
            cl_cat(l, ver, sizeof(l));
            cl_cat(l, ". Use /changelog to view full changelog.", sizeof(l));
            ui_line(&p.ui, l);
        } else {
            ui_line(&p.ui, "What's New");
            ui_line(&p.ui, ui_cl_news);
        }
    }            /* a resumed session shows its conversation, as pi's renderInitialMessages */
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
