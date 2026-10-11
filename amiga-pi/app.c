/* see app.h */
#include <string.h>
#include "app.h"
#include "args.h"
#include "prompt.h"
#include "ui_pi.h"
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

int pi_app(int argc, char **argv, const pi_platform *pl)
{
    static pi_args a;
    static pi_session s;
    static char key[512];
    llm_model m;
    jw sys;
    char kn[64];
    int rc;
    if (pi_args_parse(&a, argc, argv) || pi_args_model(&a, &m)) {
        say(pl->io, "amiga-pi: ", a.err);
        say(pl->io, "usage: amiga-pi [-p] [--model provider/id] [--base-url URL] [prompt]", 0);
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
    jw_free(&sys);
    memset(key, 0, sizeof(key));
    return rc;
}
