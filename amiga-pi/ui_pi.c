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
    if (p->print)
        return;
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
    }
}

static void say_end(pi_ui *p)
{
    char line[300];
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
        if (!n)
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
