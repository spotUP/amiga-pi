/* see ui_pi.h */
#include <stdlib.h>
#include <string.h>
#include "ui_pi.h"
#include "claude/tui.h"
#include "claude/show.h"
#include "claude/util.h"
#include "show_pi.h"
#include "claude/tools.h"
#include "compact.h"
#include "tree_pi.h"
#include "store.h"

typedef struct pi_ui {
    cl_ui ui;
    cl_render render;
    pi_session *s;
    long text_index;            /* the streaming answer's text block, -1 none */
    int print;                  /* -p: nothing shown while it runs */
    int look;                   /* the screen runs in pi's look (show_pi.c writes the transcript) */
    pi_show ps;
    cl_pitheme theme;
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
    { "/quit", "Quit amiga-pi" }
};
static const cl_cmd *ui_cmds = default_cmds;
static int ui_ncmds = (int)(sizeof(default_cmds) / sizeof(default_cmds[0]));

void pi_ui_set_commands(const cl_cmd *cmds, int n)
{
    ui_cmds = cmds ? cmds : default_cmds;
    ui_ncmds = cmds ? n : (int)(sizeof(default_cmds) / sizeof(default_cmds[0]));
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
    ui_quiet = quiet;
}

void pi_ui_set_cwd(const char *cwd)
{
    cl_copy(ui_cwd, cwd ? cwd : "", sizeof(ui_cwd));
}

static void status(pi_ui *p);
static void user_text(const char *json, long n, jw *out);
static void sync_queue(pi_ui *p);

static int u_poll(void *u)
{
    return ui_poll(&((pi_ui *)u)->ui);
}

static void on_event(void *u, const pi_event *e)
{
    pi_ui *p = (pi_ui *)u;
    if (p->print && !(e->type == PE_COMPACTION_END && e->is_error))
        return;                 /* print mode shows the answer, and a compaction that failed */
    switch (e->type) {
    case PE_AGENT_START:
        if (p->look)
            cl_copy(p->ui.tui->pi_work, "Working", sizeof(p->ui.tui->pi_work));
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
        if (e->role == LR_USER && p->look) {
            /* the user's message is in the conversation now (a typed prompt, or a queued one delivered): it
             * leaves the waiting list and shows in the transcript */
            jw txt;
            jw_init(&txt);
            user_text(e->msg, e->msg_n, &txt);
            sync_queue(p);
            pshow_user(&p->ps, txt.p ? txt.p : "");
            jw_free(&txt);
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
                cl_copy(p->ui.tui->pi_work, "Working", sizeof(p->ui.tui->pi_work));
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
            pshow_tool_start(&p->ps, e->tool_name, e->args, e->args_n);
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
    case PE_TOOL_END:
        if (p->look) {
            pshow_tool_end(&p->ps, e->result, e->result_n, e->is_error);
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
        c = tui_menu(p->ui.tui, title, 0, opt, n, 0, n - 1);
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
    } else
        s->ask = 0;             /* print mode: nobody answers the gate */
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

static void footer(pi_ui *p)
{
    cl_tui *t = p->ui.tui;
    pi_session *s = p->s;
    char n[24], *l = t->pi_left;
    long tenths, t_in, t_out, t_cr, t_cw, t_cost;
    int level = s->model.reasoning ? s->thinking : 0;
    totals(p, &t_in, &t_out, &t_cr, &t_cw, &t_cost);
    cl_copy(t->pi_cwd, ui_cwd, sizeof(t->pi_cwd));
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
    if (s->model.context_window > 0) {
        /* pi's getContextUsage: the last answer's usage plus an estimate of what came after it */
        int last;
        long ctx = s->nmsg ? cp_estimate_context(s->msg, s->msg_n, s->nmsg, &last) : 0;
        tenths = (ctx * 1000 + s->model.context_window / 2) / s->model.context_window;
        cl_ltoa(tenths / 10, t->pi_ctx);
        cl_cat(t->pi_ctx, ".", sizeof(t->pi_ctx));
        cl_ltoa(tenths % 10, n);
        cl_cat(t->pi_ctx, n, sizeof(t->pi_ctx));
        cl_cat(t->pi_ctx, "%/", sizeof(t->pi_ctx));
        fmt_tokens(s->model.context_window, n);
        cl_cat(t->pi_ctx, n, sizeof(t->pi_ctx));
        cl_cat(t->pi_ctx, " (auto)", sizeof(t->pi_ctx));
        if (tenths > 900)
            t->pi_ctx_role = PR_ERROR + 1;
        else if (tenths > 700)
            t->pi_ctx_role = PR_WARNING + 1;
    } else {
        cl_copy(t->pi_ctx, "?/0 (auto)", sizeof(t->pi_ctx));
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
    return stop == LS_STOP ? 0 : 1;
}

/* C:Claude's screen without its REPL (claude/repl.c repl_screen's part that is the screen's), in pi's look */
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
    t->pith = &p->theme;
    t->cmds = ui_cmds;
    t->ncmds = ui_ncmds;
    p->look = 1;
    show_init(sh, t);
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
    t->qu = p;
    km_free(&t->km);
    if (ui_kb)
        km_load_pi(&t->km, ui_kb, ui_kb_n);
    else
        km_init_pi(&t->km);
    pshow_init(&p->ps, t, &p->theme);
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
            tui_clear(t);
            p->ps.any = 0;
            pshow_replay(&p->ps, p->s);
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
    int excl = line[1] == '!', i, is_error = 0, stopped, code = 0;
    const char *cmd = line + (excl ? 2 : 1);
    pi_tool *tool = 0;
    jw args, content, text;
    char *tail;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (!*cmd)
        return 0;
    for (i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i]->name, "bash"))
            tool = s->tools[i];
    if (!tool) {
        pshow_error(&p->ps, "The bash tool is not available");
        return 1;
    }
    jw_init(&args);
    jw_init(&content);
    jw_init(&text);
    jw_rawz(&args, "{\"command\":");
    jw_strz(&args, cmd);
    jw_raw(&args, "}", 1);
    stopped = tool->run(tool->u, s, args.p, args.n, &content, &is_error);
    pshow_result_text(content.p ? content.p : "[]", content.n, &text);
    jw_rawz(&text, "");
    /* the tool appends "Command exited with code N" to the output of a failed command: pi keeps the code apart */
    tail = text.p ? strstr(text.p, "Command exited with code ") : 0;
    if (tail && !strchr(tail, '\n')) {
        code = (int)strtol(tail + 25, 0, 10);
        while (tail > text.p && tail[-1] == '\n')
            tail--;
        *tail = 0;
    } else if (is_error && !stopped)
        code = 1;
    pshow_bash(&p->ps, cmd, text.p ? text.p : "", code, stopped, excl);
    if (s->command_u && s->command)
        pi_store_bash((pi_store *)s->command_u, cmd, text.p ? text.p : "", code, stopped, excl);
    jw_free(&args);
    jw_free(&content);
    jw_free(&text);
    status(p);
    return 1;
}

static int run_command(pi_ui *p, cl_tui *t, const char *line)
{
    jw reply, editor;
    int rc;
    char *a, *nl;
    if (p->look && t && !strcmp(line, "/tree") && p->s->command) {
        tree_command(p, t);
        return 1;
    }
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
    if (p.look && !ui_quiet)
        pshow_header(&p.ps, "amiga-pi", ui_version[0] ? ui_version : "(pi 1.0.4)");
    else if (!p.look) {
        cl_copy(hello, "amiga-pi in UP-Term. Model ", sizeof(hello));
        cl_cat(hello, s->model.provider, sizeof(hello));
        cl_cat(hello, "/", sizeof(hello));
        cl_cat(hello, s->model.id, sizeof(hello));
        cl_cat(hello, ". Type /exit to leave.", sizeof(hello));
        ui_line(&p.ui, hello);
    }
    if (p.look && s->nmsg)
        pshow_replay(&p.ps, s);        /* a resumed session shows its conversation, as pi's renderInitialMessages */
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
    return 0;
}
