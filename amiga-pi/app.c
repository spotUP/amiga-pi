/* see app.h */
#include <string.h>
#include "app.h"
#include "args.h"
#include "prompt.h"
#include "ui_pi.h"
#include "tools_pi.h"
#include "ext_gate.h"
#include "store.h"
#include "claude/util.h"
#include "claude/repl.h"       /* cl_key_clean */

static void say(cl_io *io, const char *a, const char *b)
{
    io->write(io->u, a, (long)strlen(a));
    if (b)
        io->write(io->u, b, (long)strlen(b));
    io->write(io->u, "\n", 1);
}

/* the key-not-sent notice (llm_request) on the console */
static cl_io *note_io;

static void note(void *u, const char *text)
{
    (void)u;
    if (note_io)
        say(note_io, "[INFO] ", text);
}

static int has_tool(const pi_session *s, const char *name)
{
    int i;
    for (i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i]->name, name))
            return 1;
    return 0;
}

/* <agent dir>/settings.json: the gate's rules taken; 1 when it switches the gate on */
static int gate_setting(const pi_platform *pl, pi_gate *g)
{
    char path[300], *b = 0;
    long n = 0;
    int on = -1;
    if (!pl->agent_dir || !pl->agent_dir[0])
        return 0;
    cl_copy(path, pl->agent_dir, sizeof(path));
    if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", sizeof(path));
    cl_cat(path, "settings.json", sizeof(path));
    if (pl->sys->kind(pl->sys->u, path) != 1 || pl->sys->read(pl->sys->u, path, 256L * 1024, &b, &n))
        return 0;
    if (pi_gate_settings(g, b, n, path, &on))
        say(pl->io, "[WARN] not a JSON object, skipped: ", path);
    CL_FREE(b);
    return on == 1;
}

/* -r: the sessions of this directory listed, one picked by number (0 or no input: none) */
typedef struct pick {
    cl_io *io;
    int n;
    char path[9][320];
} pick;

static void pick_fn(void *c, const char *path, const char *id, const char *title)
{
    pick *p = (pick *)c;
    char line[160], num[8];
    if (p->n >= 9)
        return;
    cl_copy(p->path[p->n], path, sizeof(p->path[0]));
    num[0] = (char)('1' + p->n);
    num[1] = 0;
    cl_copy(line, "  ", sizeof(line));
    cl_cat(line, num, sizeof(line));
    cl_cat(line, "  ", sizeof(line));
    cl_copy(line + strlen(line), id, 9);
    cl_cat(line, "  ", sizeof(line));
    cl_cat(line, title, sizeof(line));
    say(p->io, line, 0);
    p->n++;
}

static int pick_session(const pi_platform *pl, const char *data_dir, char *out, long cap)
{
    pick p;
    char dir[300], buf[16];
    long n;
    p.io = pl->io;
    p.n = 0;
    if (s3_dir(data_dir, pl->cwd, dir, sizeof(dir)))
        return -1;
    say(pl->io, "Sessions in this directory (number to resume, anything else to start a new one):", 0);
    s3_list(pl->sys, dir, 9, pick_fn, &p);
    if (!p.n || !pl->io->read_line)
        return -1;
    n = pl->io->read_line(pl->io->u, buf, sizeof(buf));
    if (n != 1 || buf[0] < '1' || buf[0] >= '1' + p.n)
        return -1;
    cl_copy(out, p.path[buf[0] - '1'], cap);
    return 0;
}

static int usage(const pi_platform *pl, const pi_args *a)
{
    say(pl->io, "amiga-pi: ", a->err);
    say(pl->io, "usage: amiga-pi [-p] [--model provider/id] [--base-url URL] [--tools list] [--no-tools] "
                "[--gate|--no-gate] [-c] [--session path-or-id] [-r] [--no-session] [-n name] [prompt]", 0);
    return 2;
}

int pi_app(int argc, char **argv, const pi_platform *pl)
{
    static pi_args a;
    static pi_session s;
    static char key[512];
    static pi_tools tools;
    static pi_gate gate;
    static pi_store store;
    llm_model m;
    char kn[64], err[160], picked[320];
    int rc, mode;
    if (pi_args_parse(&a, argc, argv))
        return usage(pl, &a);
    /* the session first: the model it names is the default (pi restores it on resume) */
    mode = a.no_session || !pl->sys || !pl->data_dir || !pl->data_dir[0] ? PS_MODE_NONE : PS_MODE_NEW;
    picked[0] = 0;
    if (mode != PS_MODE_NONE && a.resume && !pick_session(pl, pl->data_dir, picked, sizeof(picked)))
        mode = PS_MODE_SESSION;
    else if (mode != PS_MODE_NONE && a.session[0]) {
        mode = PS_MODE_SESSION;
        cl_copy(picked, a.session, sizeof(picked));
    } else if (mode != PS_MODE_NONE && a.cont)
        mode = PS_MODE_CONTINUE;
    pi_store_init(&store, pl->sys, pl->now, pl->h.u, pl->utc);
    if (pi_store_open(&store, mode, pl->data_dir, pl->cwd, picked)) {
        say(pl->io, "amiga-pi: ", store.err);
        pi_store_free(&store);
        return 2;
    }
    if (!a.provider[0])
        pi_store_model(&store, a.provider, sizeof(a.provider), a.id, sizeof(a.id));
    if (pi_args_model(&a, &m)) {
        pi_store_free(&store);
        return usage(pl, &a);
    }
    pi_key_name(m.provider, kn, sizeof(kn));
    key[0] = 0;
    if (pl->getenv && pl->getenv(kn, key, sizeof(key)) > 0)
        if (cl_key_clean(key))
            key[0] = 0;         /* not a key (C:Claude's rule) */
    pi_session_init(&s, &m, key[0] ? key : 0, pl->pool, &pl->h);
    s.now = pl->now;
    note_io = pl->io;
    s.h.notice = note;
    if (pl->sys) {
        /* pi's tools on C:Claude's, and the gate (off unless asked for) */
        cl_copy(err, "out of memory", sizeof(err));
        if (pi_tools_init(&tools, pl->sys, pl->cwd, pl->tmp, pl->shell) ||
            pi_tools_add(&tools, &s, a.tools, err, sizeof(err))) {
            say(pl->io, "amiga-pi: ", err);
            pi_tools_free(&tools);
            pi_session_free(&s);
            return 2;
        }
        if (has_tool(&s, "bash"))
            s.shell_note = pi_tools_shell_note(&tools);
        pi_gate_init(&gate, &tools, &s);
        gate.on = gate_setting(pl, &gate);
        if (pl->data_dir && pl->data_dir[0])
            pi_gate_project(&gate, pl->data_dir, pl->cwd);
        if (a.gate >= 0)
            gate.on = a.gate;
        pi_ext_add(&s.ext, &gate.ext);
    }
    if (pl->ready)
        pl->ready(&s);
    if (pi_store_attach(&store, &s)) {
        say(pl->io, "amiga-pi: the session could not be loaded", 0);
        pi_store_free(&store);
        pi_session_free(&s);
        return 1;
    }
    if (a.name[0])
        s3_session_info(&store.s3, a.name);
    if (a.print)
        rc = pi_ui_print(&s, pl->io, a.prompt);
    else
        rc = pi_ui_run(&s, pl->io, a.prompt[0] ? a.prompt : 0);
    pi_store_free(&store);
    pi_session_free(&s);
    if (pl->sys) {
        pi_gate_free(&gate);
        pi_tools_free(&tools);
    }
    memset(key, 0, sizeof(key));
    return rc;
}
