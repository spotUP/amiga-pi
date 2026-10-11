/* see app.h */
#include <string.h>
#include "app.h"
#include "args.h"
#include "prompt.h"
#include "ui_pi.h"
#include "tools_pi.h"
#include "ext_gate.h"
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

int pi_app(int argc, char **argv, const pi_platform *pl)
{
    static pi_args a;
    static pi_session s;
    static char key[512];
    static pi_tools tools;
    static pi_gate gate;
    llm_model m;
    jw sys;
    char kn[64], err[160];
    int rc;
    if (pi_args_parse(&a, argc, argv) || pi_args_model(&a, &m)) {
        say(pl->io, "amiga-pi: ", a.err);
        say(pl->io, "usage: amiga-pi [-p] [--model provider/id] [--base-url URL] [--tools list] [--no-tools] "
                    "[--gate|--no-gate] [prompt]", 0);
        return 2;
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
        if (a.gate >= 0)
            gate.on = a.gate;
        pi_ext_add(&s.ext, &gate.ext);
    }
    if (pl->ready)
        pl->ready(&s);
    jw_init(&sys);
    pi_system_prompt(&s, pl->cwd, &sys);
    s.system = sys.p;
    if (a.print)
        rc = pi_ui_print(&s, pl->io, a.prompt);
    else
        rc = pi_ui_run(&s, pl->io, a.prompt[0] ? a.prompt : 0);
    pi_session_free(&s);
    if (pl->sys) {
        pi_gate_free(&gate);
        pi_tools_free(&tools);
    }
    jw_free(&sys);
    memset(key, 0, sizeof(key));
    return rc;
}
