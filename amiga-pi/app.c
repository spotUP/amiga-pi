/* see app.h */
#include <string.h>
#include "app.h"
#include "args.h"
#include "prompt.h"
#include "ui_pi.h"
#include "tools_pi.h"
#include "ext_gate.h"
#include "store.h"
#include "models.h"
#include "mcp.h"
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
                "[--gate|--no-gate] [--no-mcp] [-c] [--session path-or-id] [-r] [--no-session] [-n name] [prompt]", 0);
    return 2;
}

/* <agent dir>/settings.json: the compaction settings taken by the store */
static void store_settings(const pi_platform *pl, pi_store *st)
{
    char path[300], *b = 0;
    long n = 0;
    if (!pl->sys || !pl->agent_dir || !pl->agent_dir[0])
        return;
    cl_copy(path, pl->agent_dir, sizeof(path));
    if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", sizeof(path));
    cl_cat(path, "settings.json", sizeof(path));
    if (pl->sys->kind(pl->sys->u, path) != 1 || pl->sys->read(pl->sys->u, path, 256L * 1024, &b, &n))
        return;
    pi_store_settings(st, b, n);
    CL_FREE(b);
}

/* settings.json's defaultProvider, defaultModel, defaultThinkingLevel (pi's settings.md) */
static void default_model_settings(const pi_platform *pl, char *prov, long pc, char *id, long ic, int *level)
{
    char path[300], *b = 0;
    long n = 0;
    jv root, v;
    prov[0] = id[0] = 0;
    *level = -1;
    if (!pl->sys || !pl->agent_dir || !pl->agent_dir[0])
        return;
    cl_copy(path, pl->agent_dir, sizeof(path));
    if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", sizeof(path));
    cl_cat(path, "settings.json", sizeof(path));
    if (pl->sys->kind(pl->sys->u, path) != 1 || pl->sys->read(pl->sys->u, path, 256L * 1024, &b, &n))
        return;
    if (!json_parse(b, n, &root) && json_type(root) == J_OBJ) {
        if (json_get(root, "defaultProvider", &v))
            json_str(v, prov, pc);
        if (json_get(root, "defaultModel", &v))
            json_str(v, id, ic);
        if (json_get(root, "defaultThinkingLevel", &v) && json_type(v) == J_STR) {
            char t[16];
            json_str(v, t, sizeof(t));
            *level = llm_think_of(t);
        }
    }
    CL_FREE(b);
}

/* --list-models [search]: the registry, one line each */
static void list_models(const pi_platform *pl, pi_models *ms, const char *search)
{
    int i;
    char line[300], num[16];
    for (i = 0; i < ms->reg.n; i++) {
        const llm_model *m = &ms->reg.m[i];
        char label[200];
        pi_model_label(m, label, sizeof(label));
        if (search[0] && !strstr(label, search))
            continue;
        cl_copy(line, label, sizeof(line));
        cl_cat(line, "  ", sizeof(line));
        cl_cat(line, m->api, sizeof(line));
        cl_cat(line, "  context ", sizeof(line));
        cl_ltoa(m->context_window, num);
        cl_cat(line, num, sizeof(line));
        if (m->reasoning)
            cl_cat(line, "  thinking", sizeof(line));
        if (m->image)
            cl_cat(line, "  images", sizeof(line));
        say(pl->io, line, 0);
    }
}

/* the model this run starts with: --model (pi's resolveCliModel; a provider the registry does not know is the old
 * provider/id --base-url form), else the session's, else settings.json's default, else C:Claude's default. 0, -1 with
 * a->err; *level the :thinking of the pattern or -1 */
static int pick_model(pi_args *a, pi_models *ms, pi_store *st, const char *dprov, const char *did, llm_model *m, int *level,
                      char *warn)
{
    char err[200];
    *level = -1;
    warn[0] = 0;
    if (a->pattern[0]) {
        if (!llm_reg_resolve(&ms->reg, a->cli_provider[0] ? a->cli_provider : 0, a->pattern, m, level, err, sizeof(err)))
            goto done;
        if (!a->provider[0] || a->cli_provider[0] || llm_reg_has_provider(&ms->reg, a->provider)) {
            cl_copy(a->err, err, sizeof(a->err));
            return -1;
        }
        return pi_args_model(a, m);     /* an unknown provider: the --base-url form */
    }
    {
        char p[64], id[128];
        const llm_model *f;
        if (pi_store_model(st, p, sizeof(p), id, sizeof(id))) {
            f = llm_reg_find(&ms->reg, p, id);
            if (f) {
                *m = *f;
                goto done;
            }
            cl_copy(a->provider, p, sizeof(a->provider));
            cl_copy(a->id, id, sizeof(a->id));
            return pi_args_model(a, m);
        }
    }
    if (dprov[0] && did[0]) {
        const llm_model *f = llm_reg_find(&ms->reg, dprov, did);
        if (f) {
            *m = *f;
            goto done;
        }
    }
    {
        const llm_model *f = llm_reg_find(&ms->reg, "anthropic", CL_DEFAULT_MODEL);
        if (!f) {
            cl_copy(a->err, "no default model in the catalog", sizeof(a->err));
            return -1;
        }
        *m = *f;
    }
done:
    if (a->base_url[0])
        m->base_url = a->base_url;
    return 0;
}

int pi_app(int argc, char **argv, const pi_platform *pl)
{
    static pi_args a;
    static pi_session s;
    static pi_models models;
    static pi_tools tools;
    static pi_gate gate;
    static pi_store store;
    static mcp mc;
    llm_model m;
    char err[160], picked[320];
    int rc, mode, level0 = 0;
    if (pi_args_parse(&a, argc, argv))
        return usage(pl, &a);
    if (a.list_models) {
        pi_models_init(&models, pl->sys, pl->agent_dir, pl->getenv);
        if (models.warn[0])
            say(pl->io, "[WARN] ", models.warn);
        list_models(pl, &models, a.list_search);
        pi_models_free(&models);
        return 0;
    }
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
    {
        char dprov[64], did[128], warn[200];
        int dlevel, plevel, want;
        char lvl[32];
        pi_models_init(&models, pl->sys, pl->agent_dir, pl->getenv);
        if (models.warn[0])
            say(pl->io, "[WARN] ", models.warn);
        default_model_settings(pl, dprov, sizeof(dprov), did, sizeof(did), &dlevel);
        if (pick_model(&a, &models, &store, dprov, did, &m, &plevel, warn)) {
            pi_models_free(&models);
            pi_store_free(&store);
            return usage(pl, &a);
        }
        /* the thinking level: --thinking, the pattern's :level, the session's, settings.json's, pi's "medium" */
        want = a.thinking >= 0 ? a.thinking : plevel;
        if (want < 0 && store.resumed && pi_store_level(&store, lvl, sizeof(lvl)))
            want = llm_think_of(lvl);
        if (want < 0)
            want = dlevel;
        level0 = pi_models_start_level(&m, want);
    }
    pi_session_init(&s, &m, 0, pl->pool, &pl->h);
    s.thinking = level0;
    if (a.api_key[0]) {
        cl_copy(models.cfg.runtime_key, a.api_key, sizeof(models.cfg.runtime_key));
        cl_copy(models.cfg.runtime_provider, m.provider, sizeof(models.cfg.runtime_provider));
    }
    pi_models_attach(&models, &s, &store);
    store.models = &models;
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
            pi_models_free(&models);
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
    /* MCP servers from <agent dir>/mcp.json (pi: ~/.pi/agent/mcp.json); the project's .pi/mcp.json is read
     * only for a trusted project, and amiga-pi has no trust yet (phase G3) */
    mcp_init(&mc, pl->sys, pl->mcp_pool ? pl->mcp_pool : pl->pool, &s.h, pl->getenv, pl->cwd, pl->tmp);
    if (pl->sys && !a.no_mcp && pl->agent_dir && pl->agent_dir[0]) {
        char path[300];
        long i, st = 0;
        cl_copy(path, pl->agent_dir, sizeof(path));
        if (path[0] && path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
            cl_cat(path, "/", sizeof(path));
        cl_cat(path, "mcp.json", sizeof(path));
        mcp_load(&mc, path, 0);
        if (mc.n)
            mcp_connect(&mc);
        for (i = 0; i < mc.errors.n; i++)
            if (mc.errors.p[i] == '\n') {
                mc.errors.p[i] = 0;
                say(pl->io, "amiga-pi: MCP: ", mc.errors.p + st);
                st = i + 1;
            }
        if (mcp_attach(&mc, &s))
            say(pl->io, "amiga-pi: MCP: too many tools; some are not available", 0);
    }
    if (pl->ready)
        pl->ready(&s);
    store_settings(pl, &store);
    if (pi_store_attach(&store, &s)) {
        say(pl->io, "amiga-pi: the session could not be loaded", 0);
        pi_store_free(&store);
        pi_session_free(&s);
        pi_models_free(&models);
        mcp_free(&mc);
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
    mcp_free(&mc);
    if (pl->sys) {
        pi_gate_free(&gate);
        pi_tools_free(&tools);
    }
    memset(models.cfg.runtime_key, 0, sizeof(models.cfg.runtime_key));
    pi_models_free(&models);
    return rc;
}
