/* see models.h */
#include <stdlib.h>
#include <string.h>
#include "models.h"
#include "store.h"
#include "claude/util.h"

static pi_models *self;         /* the callbacks have no user pointer in cl_sys's run; one registry per process */

static long cfg_getenv(void *u, const char *name, char *out, long cap)
{
    pi_models *m = (pi_models *)u;
    long n = m->getenv ? m->getenv(name, out, cap) : -1;
    return n > 0 ? n : -1;
}

/* pi runs the command with a 10 second limit and takes its trimmed standard output */
long pi_cfg_run(cl_sys *sys, const char *cmd, char *out, long cap)
{
    long n = 0, rc = -1, s = 0;
    if (!sys || !sys->run)
        return -1;
    if (sys->run(sys->u, cmd, 10, out, cap - 1, &n, &rc) || rc != 0)
        return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t'))
        n--;
    while (s < n && (out[s] == '\n' || out[s] == '\r' || out[s] == ' ' || out[s] == '\t'))
        s++;
    if (s)
        memmove(out, out + s, (size_t)(n - s));
    n -= s;
    out[n] = 0;
    return n > 0 ? n : -1;
}

static long cfg_run(void *u, const char *cmd, char *out, long cap)
{
    return pi_cfg_run(((pi_models *)u)->sys, cmd, out, cap);
}

static char *read_text(cl_sys *sys, const char *dir, const char *name, long *n)
{
    char path[300], *b = 0, *c;
    long len = 0;
    if (!sys || !dir || !dir[0])
        return 0;
    cl_copy(path, dir, sizeof(path));
    if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/')
        cl_cat(path, "/", sizeof(path));
    cl_cat(path, name, sizeof(path));
    if (sys->kind(sys->u, path) != 1 || sys->read(sys->u, path, 1024L * 1024, &b, &len))
        return 0;
    c = (char *)malloc((size_t)len + 1);
    if (c) {
        memcpy(c, b, (size_t)len);
        c[len] = 0;
        *n = len;
    }
    CL_FREE(b);
    return c;
}

int pi_models_init(pi_models *m, cl_sys *sys, const char *agent_dir, long (*getenv_)(const char *, char *, long))
{
    char *txt;
    long n = 0;
    memset(m, 0, sizeof(*m));
    m->sys = sys;
    m->default_level = -1;
    m->getenv = getenv_;
    llm_reg_init(&m->reg);
    llm_cfg_init(&m->cfg);
    m->cfg.u = m;
    m->cfg.getenv = cfg_getenv;
    m->cfg.run = cfg_run;
    self = m;
    if (llm_reg_builtin(&m->reg)) {
        cl_copy(m->warn, m->reg.err[0] ? m->reg.err : "out of memory", sizeof(m->warn));
        return -1;
    }
    txt = read_text(sys, agent_dir, "models.json", &n);
    if (txt) {
        if (llm_reg_overlay(&m->reg, txt, n)) {
            cl_copy(m->warn, m->reg.err, sizeof(m->warn));
            cl_cat(m->warn, " (models.json is ignored)", sizeof(m->warn));
        }
        free(txt);
    }
    m->auth = read_text(sys, agent_dir, "auth.json", &n);
    m->cfg.auth = m->auth;
    m->cfg.auth_n = m->auth ? n : 0;
    return 0;
}

void pi_models_free(pi_models *m)
{
    llm_cfg_free(&m->cfg);
    llm_reg_free(&m->reg);
    free(m->auth);
    m->auth = 0;
    if (self == m)
        self = 0;
}

void pi_models_attach(pi_models *m, pi_session *s, struct pi_store *st)
{
    s->cfg = &m->cfg;
    m->store = st;
    s->thinking = llm_think_clamp(&s->model, s->thinking);
}

int pi_models_start_level(const llm_model *model, int want)
{
    if (!model->reasoning)
        return LT_OFF;
    if (want < 0)
        want = LT_MEDIUM;
    return llm_think_clamp(model, want);
}

void pi_model_label(const llm_model *mo, char *out, long cap)
{
    cl_copy(out, mo->provider, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, mo->id, cap);
}

int pi_thinking_set(pi_models *m, pi_session *s, int level)
{
    int eff = llm_think_clamp(&s->model, level);
    if (eff != s->thinking) {
        if (m->store && pi_store_thinking(m->store, llm_think_name(eff)) < 0)
            return -1;          /* the level in effect is unchanged */
        s->thinking = eff;
    }
    return s->thinking;
}

/* checkAuth: a credential for the provider, or an endpoint that wants none (a LAN server on plain http) */
static int usable(pi_models *m, const llm_model *mo)
{
    static char key[512];
    if (mo->base_url && !strncmp(mo->base_url, "http://", 7))
        return 1;
    return llm_cfg_key(&m->cfg, mo, key, sizeof(key), 0) > 0;
}

/* pi's setModel (agent-session.js :1942): the level for the new model is the settings default, else the one the session has
 * (_getThinkingLevelForModelSwitch), clamped to the model. The session keeps its model and level when the session file
 * cannot take the entries. */
int pi_model_switch(pi_models *m, pi_session *s, const llm_model *to, char *err, long cap)
{
    char label[200];
    llm_model was = s->model;
    int level = m->default_level >= 0 ? m->default_level : s->thinking;
    if (!usable(m, to)) {
        pi_model_label(to, label, sizeof(label));
        cl_copy(err, "No API key for ", cap);
        cl_cat(err, label, cap);
        return -1;
    }
    s->model = *to;
    if (m->store && pi_store_model_change(m->store, to->provider, to->id) < 0) {
        s->model = was;
        cl_copy(err, "the model change could not be recorded", cap);
        return -1;
    }
    if (pi_thinking_set(m, s, level) < 0) {
        s->model = was;
        cl_copy(err, "the thinking level change could not be recorded", cap);
        return -1;
    }
    return 0;
}

void pi_models_list(pi_models *m, const pi_session *s, jw *out)
{
    int i, n = 0;
    char line[200], num[16];
    for (i = 0; i < m->reg.n; i++) {
        const llm_model *mo = &m->reg.m[i];
        if (!usable(m, mo))
            continue;
        n++;
        cl_ltoa(n, num);
        cl_copy(line, !strcmp(mo->provider, s->model.provider) && !strcmp(mo->id, s->model.id) ? "* " : "  ", sizeof(line));
        cl_cat(line, num, sizeof(line));
        cl_cat(line, ". ", sizeof(line));
        cl_cat(line, mo->provider, sizeof(line));
        cl_cat(line, "/", sizeof(line));
        cl_cat(line, mo->id, sizeof(line));
        if (mo->reasoning)
            cl_cat(line, "  (thinking)", sizeof(line));
        jw_rawz(out, line);
        jw_rawz(out, "\n");
    }
    if (!n)
        jw_rawz(out, "No models available: set a provider key (for example ANTHROPIC_API_KEY) or add models to models.json.\n");
}

const llm_model *pi_models_nth(pi_models *m, const pi_session *s, int nth)
{
    int i, n = 0;
    (void)s;
    for (i = 0; i < m->reg.n; i++)
        if (usable(m, &m->reg.m[i]) && ++n == nth)
            return &m->reg.m[i];
    return 0;
}
