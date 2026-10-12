/* see app.h */
#include "okcolor.h"
#include "themes_builtin.h"
#include <string.h>
#include "app.h"
#include "args.h"
#include "prompt.h"
#include "ui_pi.h"
#include "claude/tui.h"
#include "tools_pi.h"
#include "ext_gate.h"
#include "store.h"
#include "models.h"
#include "mcp.h"
#include "resources.h"
#include "mode_json.h"
#include "mode_rpc.h"
#include "claude/util.h"
#include "claude/path.h"
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

/* <agent dir>/settings.json read: 0, -1 none (b is CL_FREEd by the caller) */
static int settings_text(const pi_platform *pl, char **b, long *n, char *path, long cap)
{
    *b = 0;
    *n = 0;
    if (!pl->sys || !pl->agent_dir || !pl->agent_dir[0])
        return -1;
    cl_copy(path, pl->agent_dir, cap);
    if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", cap);
    cl_cat(path, "settings.json", cap);
    if (pl->sys->kind(pl->sys->u, path) != 1 || pl->sys->read(pl->sys->u, path, 256L * 1024, b, n))
        return -1;
    return 0;
}

/* settings.json's defaultProjectTrust ("ask" | "always" | "never"): into out, "ask" when absent */
static void trust_setting(const pi_platform *pl, char *out, long cap)
{
    char *b, path[300];
    long n;
    jv o, v;
    cl_copy(out, "ask", cap);
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return;
    if (!json_parse(b, n, &o) && json_get(o, "defaultProjectTrust", &v) && json_type(v) == J_STR)
        json_str(v, out, cap);
    CL_FREE(b);
}

/* settings.json's quietStartup: true hides the start header ("header" keeps it: it has no resource listing to hide) */
static int quiet_setting(const pi_platform *pl)
{
    char *b, path[300];
    long n;
    jv o, v;
    int quiet = 0;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return 0;
    if (!json_parse(b, n, &o) && json_get(o, "quietStartup", &v) && json_type(v) == J_TRUE)
        quiet = 1;
    CL_FREE(b);
    return quiet;
}

/* settings.json's defaultTools (an array of strings): joined by commas into out, 1 when the setting is there (an empty
 * array too), 0 when absent. pi 1.1.0 settings-manager.ts getDefaultTools; only the global file is read */
static int default_tools_setting(const pi_platform *pl, char *out, long cap)
{
    char *b, path[300], e[64];
    long n;
    jv o, a, x;
    jit it;
    int present = 0;
    out[0] = 0;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return 0;
    if (!json_parse(b, n, &o) && json_get(o, "defaultTools", &a) && json_type(a) == J_ARR) {
        present = 1;
        json_iter(a, &it);
        while (json_next(&it, 0, &x))
            if (json_type(x) == J_STR) {
                json_str(x, e, sizeof(e));
                if (out[0])
                    cl_cat(out, ",", cap);
                cl_cat(out, e, cap);
            }
    }
    CL_FREE(b);
    return present;
}

/* settings.json's outputPad (pi 1.1.0 getOutputPad): 0 gives 0, anything else 1 */
static int output_pad_setting(const pi_platform *pl)
{
    char *b, path[300];
    long n;
    jv o, v;
    int pad = 1;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return 1;
    if (!json_parse(b, n, &o) && json_get(o, "outputPad", &v) && json_type(v) == J_NUM && v.n == 1 && v.p[0] == '0')
        pad = 0;
    CL_FREE(b);
    return pad;
}

/* settings.json's tuiMode ("regular" | "fullscreen", pi's default), fullscreenExitOutput ("transcript" | "resume-hint")
 * and fullscreenScrollbar ("auto" | "always" | "hidden"; "auto" shows the bar on a pointer's hover only, and there is
 * no pointer here): into the arguments of pi_ui_set_tui. fullscreenCopyOnSelect and fullscreenWheelScrollLines are
 * read by pi for its own mouse handling; amiga-pi takes no mouse (COMPATIBILITY.md, pi-tui group). cli: --tui-mode */
static void tui_settings(const pi_platform *pl, int cli, int *regular, int *hint_only, int *bar)
{
    char *b, path[300], v[32];
    long n;
    jv o, x;
    *regular = *hint_only = *bar = 0;
    if (!settings_text(pl, &b, &n, path, sizeof(path))) {
        if (!json_parse(b, n, &o)) {
            if (json_get(o, "tuiMode", &x) && json_type(x) == J_STR && (json_str(x, v, sizeof(v)), !strcmp(v, "regular")))
                *regular = 1;
            if (json_get(o, "fullscreenExitOutput", &x) && json_type(x) == J_STR &&
                (json_str(x, v, sizeof(v)), !strcmp(v, "resume-hint")))
                *hint_only = 1;
            if (json_get(o, "fullscreenScrollbar", &x) && json_type(x) == J_STR &&
                (json_str(x, v, sizeof(v)), !strcmp(v, "always")))
                *bar = 1;
        }
        CL_FREE(b);
    }
    if (cli)
        *regular = cli == 2;
}

/* pi's resume line: the command that opens this session again, only when its file exists */
static const char *resume_cmd(void *u)
{
    static char cmd[400];
    pi_store *st = (pi_store *)u;
    if (!st->s3.persist || !st->s3.flushed || !st->s3.id[0])
        return "";
    cl_copy(cmd, "amiga-pi --session ", sizeof(cmd));
    cl_cat(cmd, st->s3.id, sizeof(cmd));
    return cmd;
}

/* settings.json's steeringMode / followUpMode: 1 for "all", 0 for "one-at-a-time" (the default) */
static int queue_mode_all(const pi_platform *pl, const char *key)
{
    char *b, path[300], v[32];
    long n;
    jv o, x;
    int all = 0;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return 0;
    if (!json_parse(b, n, &o) && json_get(o, key, &x) && json_type(x) == J_STR) {
        json_str(x, v, sizeof(v));
        all = !strcmp(v, "all");
    }
    CL_FREE(b);
    return all;
}

/* settings.json's enabledModels (an array of patterns): joined by newlines into out, "" when absent */
static void enabled_models(const pi_platform *pl, char *out, long cap)
{
    char *b, path[300], pat[200];
    long n;
    jv o, a, e;
    jit it;
    out[0] = 0;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return;
    if (!json_parse(b, n, &o) && json_get(o, "enabledModels", &a) && json_type(a) == J_ARR) {
        json_iter(a, &it);
        while (json_next(&it, 0, &e))
            if (json_type(e) == J_STR) {
                json_str(e, pat, sizeof(pat));
                if (out[0])
                    cl_cat(out, "\n", cap);
                cl_cat(out, pat, cap);
            }
    }
    CL_FREE(b);
}

/* the theme settings.json names ("theme"; "system" when absent): a file <data dir>/themes/<name>.json or
 * <agent dir>/themes/<name>.json, else pi's built-in dark or light; a theme that is invalid or missing is told and
 * the system theme used (pi: the same fallback) */
static void load_theme(const pi_platform *pl, cl_pitheme *th)
{
    char *b, path[300], name[80], err[800], *txt = 0;
    long n, tn = 0;
    jv o, v;
    int i, own = 0;
    pitheme_system(th);
    name[0] = 0;
    if (!settings_text(pl, &b, &n, path, sizeof(path))) {
        if (!json_parse(b, n, &o) && json_get(o, "theme", &v) && json_type(v) == J_STR)
            json_str(v, name, sizeof(name));
        CL_FREE(b);
    }
    if (!name[0] || !strcmp(name, "system"))
        return;
    for (i = 0; i < 2 && !txt; i++) {
        const char *dir = i ? pl->agent_dir : pl->data_dir;
        if (!dir || !dir[0] || strchr(name, '/') || !pl->sys)
            continue;
        cl_copy(path, dir, sizeof(path));
        if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
            cl_cat(path, "/", sizeof(path));
        cl_cat(path, "themes/", sizeof(path));
        cl_cat(path, name, sizeof(path));
        cl_cat(path, ".json", sizeof(path));
        if (pl->sys->kind(pl->sys->u, path) == 1 && !pl->sys->read(pl->sys->u, path, 256L * 1024, &txt, &tn))
            own = 1;
        else
            txt = 0;
    }
    if (!txt) {
        const char *bt = !strcmp(name, "dark") ? pi_theme_dark : !strcmp(name, "light") ? pi_theme_light : 0;
        if (!bt) {
            say(pl->io, "[WARN] Theme not found: ", name);
            return;
        }
        if (pitheme_load(th, bt, (long)strlen(bt), pi_okcolor, err, sizeof(err)))
            pitheme_system(th);
        return;
    }
    if (pitheme_load(th, txt, tn, pi_okcolor, err, sizeof(err))) {
        say(pl->io, "[WARN] ", err);
        pitheme_system(th);
    }
    if (own)
        CL_FREE(txt);
}

/* A loaded theme is drawn in the depth pi would pick on this terminal (pi-tui 1.0.4: truecolor when COLORTERM or
 * TERM says so, else the 256 palette; UP-Term tells COLORTERM through /colors rgb), never the fixed 16 that turned a
 * dark theme's #343541 into ANSI 8. The system theme is indexed colours: its depth does not matter. */
static void theme_depth(const pi_platform *pl, cl_pitheme *th)
{
    char term[48], ct[24], pt[8];
    term[0] = ct[0] = pt[0] = 0;
    if (strcmp(th->name, "system") == 0)
        return;
    if (pl->getenv) {
        pl->getenv("TERM", term, sizeof(term));
        pl->getenv("COLORTERM", ct, sizeof(ct));
        pl->getenv("PI_TRUE_COLOR", pt, sizeof(pt));
    }
    th->depth = pitheme_depth(term, ct, pt);
}

/* <agent dir>/keybindings.json read (pi's shape): the text, CL_FREEd by the caller; 0 none */
static char *keybindings_text(const pi_platform *pl, long *n)
{
    char *b = 0, path[300];
    if (!pl->sys || !pl->agent_dir || !pl->agent_dir[0])
        return 0;
    cl_copy(path, pl->agent_dir, sizeof(path));
    if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", sizeof(path));
    cl_cat(path, "keybindings.json", sizeof(path));
    if (pl->sys->kind(pl->sys->u, path) != 1 || pl->sys->read(pl->sys->u, path, 256L * 1024, &b, n))
        return 0;
    return b;
}

/* pi's trust question on the console: the title, the answers numbered, a number typed (0 or none: no answer) */
static int trust_ask(void *u, const char *title, const char *const *labels, int n)
{
    const pi_platform *pl = (const pi_platform *)u;
    char line[160], num[8], buf[16];
    int i;
    if (pl->io->read && pl->io->raw) {
        /* a console that answers raw mode: pi's selector (the question before the screen is up, no footer under it) */
        static cl_tui t;
        static cl_pitheme th;
        if (!tui_init(&t, pl->io)) {
            int pick;
            pitheme_system(&th);
            t.look = TUI_LOOK_PI;
            t.pith = &th;
            t.pi_nofoot = 1;
            if (!tui_start(&t)) {
                pick = tui_menu(&t, title, 0, labels, n, 0, -1);
                tui_stop(&t);
                tui_free(&t);
                return pick;
            }
            tui_free(&t);
        }
    }
    say(pl->io, title, 0);
    for (i = 0; i < n; i++) {
        num[0] = (char)('1' + i);
        num[1] = 0;
        cl_copy(line, "  ", sizeof(line));
        cl_cat(line, num, sizeof(line));
        cl_cat(line, "  ", sizeof(line));
        cl_cat(line, labels[i], sizeof(line));
        say(pl->io, line, 0);
    }
    say(pl->io, "Number of your answer (anything else: do not trust this time):", 0);
    if (!pl->io->read_line || pl->io->read_line(pl->io->u, buf, sizeof(buf)) != 1 || buf[0] < '1' || buf[0] >= '1' + n)
        return -1;
    return buf[0] - '1';
}

/* <agent dir>/settings.json: the gate's rules taken; 1 when it switches the gate on */
static int gate_setting(const pi_platform *pl, pi_gate *g)
{
    char *b, path[300];
    long n;
    int on = -1;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
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
                "[--gate|--no-gate] [--approve|--no-approve] [--no-mcp] [-c] [--session path-or-id] [-r] [--no-session] [-n name] [prompt]", 0);
    return 2;
}

/* <agent dir>/settings.json: the compaction settings taken by the store */
static void store_settings(const pi_platform *pl, pi_store *st)
{
    char *b, path[300];
    long n;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
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
    if (settings_text(pl, &b, &n, path, sizeof(path)))
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

/* the runtime: one per process (the pieces are large and the Amiga's stack is not) */
static pi_args a;
static pi_session s;
static pi_models models;
static pi_tools tools;
static pi_gate gate;
static pi_store store;
static mcp mc;
static pi_res res;

/* json and rpc mode: stdout is the protocol's, so the program's own messages go to the platform's diagnostic channel */
static void diag_write(void *u, const char *text, long n)
{
    pi_rt *rt = (pi_rt *)u;
    char line[400];
    long i, k = 0;
    if (!rt->pl.diag)
        return;
    for (i = 0; i < n && k < (long)sizeof(line) - 1; i++)
        if (text[i] != '\n')
            line[k++] = text[i];
    line[k] = 0;
    if (k)
        rt->pl.diag(line);
}

/* settings.json's "retry" object: enabled, maxRetries, baseDelayMs, maxAgentDelayMs (pi's settings.md) */
static void retry_settings(const pi_platform *pl, pi_session *ses)
{
    char *b, path[300];
    long n;
    jv o, r, v;
    if (settings_text(pl, &b, &n, path, sizeof(path)))
        return;
    if (!json_parse(b, n, &o) && json_get(o, "retry", &r) && json_type(r) == J_OBJ) {
        if (json_get(r, "enabled", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
            ses->retry_on = json_type(v) == J_TRUE;
        if (json_get(r, "maxRetries", &v) && json_type(v) == J_NUM)
            ses->retry_max = (int)json_long(v, 3);
        if (json_get(r, "baseDelayMs", &v) && json_type(v) == J_NUM)
            ses->retry_base = json_long(v, 2000);
        if (json_get(r, "maxAgentDelayMs", &v) && json_type(v) == J_NUM)
            ses->retry_max_delay = json_long(v, 60000);
    }
    CL_FREE(b);
}

int pi_rt_open(pi_rt *rt, const pi_platform *plat, int argc, char **argv)
{
    const pi_platform *pl;
    llm_model m;
    char err[160], picked[320];
    char dflt[16];
    int mode, level0 = 0, trusted = 0;
    memset(rt, 0, sizeof(*rt));
    rt->pl = *plat;
    rt->io = plat->io;
    pl = &rt->pl;
    if (pi_args_parse(&a, argc, argv))
        return usage(pl, &a);
    rt->mode = a.mode;
    if (a.mode != PI_MODE_TEXT) {
        rt->diag.u = rt;
        rt->diag.write = diag_write;
        rt->pl.io = &rt->diag;
    }
    if (a.list_models) {
        pi_models_init(&models, pl->sys, pl->agent_dir, pl->getenv);
        if (models.warn[0])
            say(pl->io, "[WARN] ", models.warn);
        list_models(pl, &models, a.list_search);
        pi_models_free(&models);
        return -1;
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
    if (pl->agent_dir)
        cl_copy(store.agent_dir, pl->agent_dir, sizeof(store.agent_dir));
    {
        char dprov[64], did[128], warn[200];
        int dlevel, plevel, want;
        char lvl[32];
        pi_models_init(&models, pl->sys, pl->agent_dir, pl->getenv);
        if (models.warn[0])
            say(pl->io, "[WARN] ", models.warn);
        default_model_settings(pl, dprov, sizeof(dprov), did, sizeof(did), &dlevel);
        models.default_level = dlevel;
        {
            char scope[2048];
            enabled_models(pl, scope, sizeof(scope));
            pi_models_set_scope(&models, scope);
        }
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
    s.steer_all = queue_mode_all(pl, "steeringMode");
    s.follow_all = queue_mode_all(pl, "followUpMode");
    retry_settings(pl, &s);
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
        int tools_ok;
        /* pi's tools on C:Claude's, and the gate (off unless asked for) */
        cl_copy(err, "out of memory", sizeof(err));
        tools_ok = pi_tools_init(&tools, pl->sys, pl->cwd, pl->tmp, pl->shell) == 0;
        if (tools_ok) {
            /* the default selection (defaultTools, --no-tools) and --tools through the one applyToolModifiers */
            char dt[256];
            pi_tools_defaults(&tools, default_tools_setting(pl, dt, sizeof(dt)) ? dt : 0, a.no_tools);
        }
        if (!tools_ok || pi_tools_add(&tools, &s, a.tools, err, sizeof(err))) {
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
    /* project trust (resources.h): -a / -na, a saved decision, defaultProjectTrust, else the question (not in print
     * mode); then the context files, SYSTEM.md and APPEND_SYSTEM.md for the prompt */
    memset(&res, 0, sizeof(res));
    if (pl->sys) {
        trust_setting(pl, dflt, sizeof(dflt));
        trusted = pi_trust_decide(pl->sys, pl->agent_dir, pl->cwd, a.approve, dflt,
                                  a.print || rt->mode != PI_MODE_TEXT || (!pl->io->read_line && !(pl->io->read && pl->io->raw)) ? 0 : trust_ask, (void *)pl);
        if (trusted < 0) {
            /* pi stops with the trust store's error (readTrustFile / setMany throw) */
            char terr[400];
            pi_trust_why(pl->sys, pl->agent_dir, terr, sizeof(terr));
            say(pl->io, "amiga-pi: ", terr);
            pi_models_free(&models);
            pi_store_free(&store);
            pi_session_free(&s);
            pi_gate_free(&gate);
            pi_tools_free(&tools);
            return 2;
        }
        pi_res_load(&res, pl->sys, pl->agent_dir, pl->cwd, trusted);
        s.res = &res;
    }
    /* MCP servers from <agent dir>/mcp.json (pi: ~/.pi/agent/mcp.json), then the project's <cwd>/.pi/mcp.json for a
     * trusted project (pi: guarded by trust) */
    mcp_init(&mc, pl->sys, pl->mcp_pool ? pl->mcp_pool : pl->pool, &s.h, pl->getenv, pl->cwd, pl->tmp);
    if (pl->sys && !a.no_mcp) {
        char path[300];
        long i, st = 0;
        if (pl->agent_dir && pl->agent_dir[0] && path_join(pl->agent_dir, "mcp.json", path, sizeof(path)) == 0)
            mcp_load(&mc, path, 0);
        if (trusted && path_join(pl->cwd, ".pi/mcp.json", path, sizeof(path)) == 0)
            mcp_load(&mc, path, 1);
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
        pi_res_free(&res);
        return 1;
    }
    if (a.name[0])
        s3_session_info(&store.s3, a.name);
    rt->a = &a;
    rt->s = &s;
    rt->models = &models;
    rt->tools = &tools;
    rt->gate = &gate;
    rt->store = &store;
    rt->mc = &mc;
    rt->res = &res;
    rt->trusted = trusted;
    return 0;
}

int pi_rt_run(pi_rt *rt)
{
    const pi_platform *pl = &rt->pl;
    int rc;
    if (rt->mode == PI_MODE_JSON)
        return pi_mode_json(&s, &store, rt->io, (const char *const *)a.msg, a.nmsg);
    if (rt->mode == PI_MODE_RPC)
        return pi_mode_rpc(rt);
    if (a.print)
        rc = pi_ui_print(&s, pl->io, a.prompt);
    else {
        static cl_pitheme th;
        static char *kb;
        long kn = 0;
        pi_ui_set_cwd(pl->cwd);
        pi_ui_set_quiet(quiet_setting(pl));
        pi_ui_set_output_pad(output_pad_setting(pl));
        {
            int reg, hint, bar;
            tui_settings(pl, a.tui_mode, &reg, &hint, &bar);
            pi_ui_set_tui(reg, hint, bar);
            pi_ui_set_resume(resume_cmd, &store);
        }
        load_theme(pl, &th);
        theme_depth(pl, &th);
        pi_ui_set_theme(&th);
        kb = keybindings_text(pl, &kn);
        pi_ui_set_keybindings(kb, kn);
        rc = pi_ui_run(&s, pl->io, a.prompt[0] ? a.prompt : 0);
    }
    return rc;
}

void pi_rt_close(pi_rt *rt)
{
    pi_store_free(&store);
    pi_session_free(&s);
    mcp_free(&mc);
    pi_res_free(&res);
    if (rt->pl.sys) {
        pi_gate_free(&gate);
        pi_tools_free(&tools);
    }
    memset(models.cfg.runtime_key, 0, sizeof(models.cfg.runtime_key));
    pi_models_free(&models);
}

int pi_app(int argc, char **argv, const pi_platform *pl)
{
    static pi_rt rt;
    int rc = pi_rt_open(&rt, pl, argc, argv);
    if (rc)
        return rc < 0 ? 0 : rc;
    rc = pi_rt_run(&rt);
    pi_rt_close(&rt);
    return rc;
}
