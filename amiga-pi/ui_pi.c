/* see ui_pi.h */
#include <stdlib.h>
#include <string.h>
#include "ui_pi.h"
#include "claude/tui.h"
#include "claude/show.h"
#include "claude/util.h"

typedef struct pi_ui {
    cl_ui ui;
    cl_render render;
    pi_session *s;
    long text_index;            /* the streaming answer's text block, -1 none */
    int print;                  /* -p: nothing shown while it runs */
} pi_ui;

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
        ui_busy(&p->ui, 1);
        break;
    case PE_AGENT_END:
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
        if (e->role == LR_ASSISTANT && p->text_index >= 0) {
            p->render.end(p->render.u);
            p->text_index = -1;
        }
        break;
    case PE_COMPACTION_START:
        ui_line(&p->ui, "* compacting the conversation");
        break;
    case PE_COMPACTION_END:
        {
            char line[300];
            cl_copy(line, e->is_error ? "  [ERROR] " : "* ", sizeof(line));
            cl_cat(line, e->msg ? e->msg : "", sizeof(line));
            ui_line(&p->ui, line);
        }
        break;
    case PE_TOOL_START:
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

/* the gate's question (ext_gate.h) as C:Claude asks it */
static int u_ask(void *u, int tool, const char *name, const char *what, int outside)
{
    pi_ui *p = (pi_ui *)u;
    char line[360];
    cl_copy(line, "* ", sizeof(line));
    cl_cat(line, name, sizeof(line));
    cl_cat(line, " ", sizeof(line));
    cl_cat(line, what, sizeof(line));
    ui_line(&p->ui, line);
    return ui_ask(&p->ui, tool, name, what, outside);
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
    tui_frame(t);
}

static void say_end(pi_ui *p)
{
    char line[300];
    status(p);
    if (p->s->stop != LS_ERROR && p->s->stop != LS_ABORTED)
        return;
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

/* C:Claude's screen without its REPL (claude/repl.c repl_screen's part that is the screen's) */
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
    show_init(sh, t);
    if (tui_start(t)) {
        show_free(sh);
        tui_free(t);
        free(t);
        free(sh);
        return 0;
    }
    p->ui.tui = t;
    p->ui.show = sh;
    show_render(sh, &p->render);
    tui_frame(t);
    return t;
}

/* a slash command the store knows: its lines shown, a message taken back kept for the next input.
 * 1 when handled */
static char prefill[4096];

static int run_command(pi_ui *p, cl_tui *t, const char *line)
{
    jw reply, editor;
    int rc;
    char *a, *nl;
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
            ui_line(&p->ui, a);
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
    cl_copy(hello, "amiga-pi in UP-Term. Model ", sizeof(hello));
    cl_cat(hello, s->model.provider, sizeof(hello));
    cl_cat(hello, "/", sizeof(hello));
    cl_cat(hello, s->model.id, sizeof(hello));
    cl_cat(hello, ". Type /exit to leave.", sizeof(hello));
    ui_line(&p.ui, hello);
    if (first && *first) {
        ui_user(&p.ui, first);
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
        if (run_command(&p, t, line))
            continue;
        ui_user(&p.ui, line);
        pi_prompt(s, line);
        say_end(&p);
    }
    if (t) {
        tui_stop(t);
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
