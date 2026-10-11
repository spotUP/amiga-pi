/* amiga-pi's models (plan phase F): the registry and its bundled catalog (F1), thinking levels, models.json and
 * config values (F2), the handoff between models (F3), thinking on the wire (F3b), and the switch itself through
 * pi_prompt (F5, in test_pi_model_reach.c).
 *
 * The expectations of the handoff, the thinking levels, the model patterns, the config values and shortHash are
 * pi's own: tools/pi_oracle/gen_model.ts runs pi's code (f10993bc) and writes tests/pi/model/NAME.expected; the cases
 * are in the files next to them. Nothing here updates an expectation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/conv.h"
#include "llm/registry.h"
#include "llm/creds.h"
#include "llm/handoff.h"

static char *slurp(const char *name, long *n)
{
    char path[600];
    FILE *f;
    char *b;
    long len;
    strcpy(path, PI_TESTS "/pi/model/");
    strcat(path, name);
    f = fopen(path, "rb");
    if (!f) {
        h_checks++;
        h_failures++;
        printf("  FAIL cannot open %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)len + 1);
    len = b ? (long)fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    if (b)
        b[len] = 0;
    if (n)
        *n = len;
    return b;
}

/* the next line of a text (terminated in place), 0 at the end */
static char *next_line(char **p)
{
    char *s = *p, *e;
    if (!s || !*s)
        return 0;
    e = strchr(s, '\n');
    if (e) {
        *e = 0;
        *p = e + 1;
    } else
        *p = s + strlen(s);
    return s;
}

/* ---- F1: the bundled catalog ---- */

static void test_catalog(void)
{
    llm_reg r;
    const llm_model *m;
    cl_price p;
    static const char *const ids[] = { "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5", "claude-haiku-4-5" };
    int i;
    llm_reg_init(&r);
    CHECK_INT(llm_reg_builtin(&r), 0);
    CHECK(r.n >= 7);
    /* the cost of every Anthropic model is C:Claude's conv_price table (micro-dollars per million tokens) */
    for (i = 0; i < 4; i++) {
        m = llm_reg_find(&r, "anthropic", ids[i]);
        CHECK(m != 0);
        if (!m)
            continue;
        CHECK_INT(conv_price(ids[i], &p), 1);
        CHECK_INT(m->cost[0], p.in * 10000L);
        CHECK_INT(m->cost[1], p.out * 10000L);
        CHECK_INT(m->cost[2], p.cread * 10000L);
        CHECK_INT(m->cost[3], p.cwrite * 10000L);
        CHECK_STR(m->api, "anthropic-messages");
        CHECK(m->image == 1 && m->reasoning == 1);
    }
    m = llm_reg_find(&r, "anthropic", "claude-opus-5-5");
    CHECK(m && m->adaptive == 1 && m->context_window == 1000000L);
    m = llm_reg_find(&r, "anthropic", "claude-haiku-4-5");
    CHECK(m && m->adaptive == 0 && m->context_window == 200000L);
    m = llm_reg_find(&r, "openai", "gpt-5.5");
    CHECK(m != 0);
    CHECK(m && !strcmp(m->api, "openai-completions") && !strcmp(m->base_url, "https://api.openai.com/v1"));
    CHECK(m && m->cost[0] == 1250000L && m->cost[2] == 125000L);
    m = llm_reg_find(&r, "ollama", "qwen2.5-coder:7b");
    CHECK(m && m->key == 0 && m->reasoning == 0 && m->context_window == 32768L);
    CHECK(llm_reg_find(&r, "openai", "nope") == 0);
    CHECK(llm_reg_has_provider(&r, "ollama") && !llm_reg_has_provider(&r, "nope"));
    llm_reg_free(&r);
}

/* ---- F2: models.json laid over the catalog ---- */

static void test_overlay(void)
{
    llm_reg r;
    const llm_model *m;
    llm_reg_init(&r);
    CHECK_INT(llm_reg_builtin(&r), 0);
    /* a provider block (BOM, comments), a new model, a replaced one, an override, an unknown override id */
    {
        char t[1400];
        strcpy(t, "\xEF\xBB\xBF{ // the LAN box\n"
                  "  \"providers\": {\n"
                  "    \"ollama\": { \"baseUrl\": \"http://192.168.1.10:11434/v1\", /* kept */ "
                  "\"headers\": {\"X-Team\": \"$TEAM\"},\n"
                  "      \"models\": [ {\"id\": \"llama3.1:8b\", \"contextWindow\": 131072, \"reasoning\": false},\n"
                  "                    {\"id\": \"qwen2.5-coder:7b\", \"name\": \"Qwen\", \"maxTokens\": 4096} ] },\n");
        strcat(t, "    \"openai\": { \"modelOverrides\": { \"gpt-5.5\": { \"cost\": {\"output\": 12}, "
                  "\"thinkingLevelMap\": {\"minimal\": null},\n"
                  "                                      \"compat\": {\"supportsDeveloperRole\": false} },\n"
                  "                                      \"gpt-9\": { \"name\": \"ignored\" } } },\n"
                  "    \"anthropic\": { \"apiKey\": \"!pass show anthropic\", \"authHeader\": true }\n"
                  "  } }");
        CHECK_INT(llm_reg_overlay(&r, t, (long)strlen(t)), 0);
        CHECK_STR(r.err, "");
    }
    m = llm_reg_find(&r, "ollama", "llama3.1:8b");
    CHECK(m != 0);
    CHECK(m && !strcmp(m->api, "openai-completions") && !strcmp(m->base_url, "http://192.168.1.10:11434/v1"));
    CHECK(m && m->context_window == 131072L && m->max_tokens == 16384L);    /* pi's defaults for a new model */
    CHECK(m && m->headers && !strcmp(m->headers, "{\"X-Team\":\"$TEAM\"}"));
    m = llm_reg_find(&r, "ollama", "qwen2.5-coder:7b");
    CHECK(m && !strcmp(m->name, "Qwen") && m->max_tokens == 4096L && m->context_window == 128000L);   /* replaced, not merged */
    CHECK(m && m->key == 0);
    m = llm_reg_find(&r, "openai", "gpt-5.5");
    CHECK(m && m->cost[0] == 1250000L && m->cost[1] == 12000000L);            /* merged per key */
    CHECK(m && m->thinking_map[LT_MINIMAL].kind == LTM_NULL && m->compat.developer_role == 0);
    CHECK(llm_reg_find(&r, "openai", "gpt-9") == 0);
    m = llm_reg_find(&r, "anthropic", "claude-opus-5-5");
    CHECK(m && m->auth_header == 1 && !strcmp(m->key, "!pass show anthropic"));
    /* a refused file leaves the registry as it was, and says why */
    {
        int n0 = r.n;
        static const char bad[] = "{\"providers\":{\"ollama\":{\"models\":[{\"id\":\"x\",\"contextWindow\":-1}]}}}";
        CHECK_INT(llm_reg_overlay(&r, bad, (long)strlen(bad)), -1);
        CHECK(strstr(r.err, "providers.ollama") && strstr(r.err, "contextWindow"));
        CHECK_INT(r.n, n0);
        CHECK(llm_reg_find(&r, "ollama", "x") == 0);
        CHECK_INT(llm_reg_overlay(&r, "{\"providers\":", 13), -1);
        CHECK(strstr(r.err, "parse") != 0);
        {
            static const char noapi[] = "{\"providers\":{\"newp\":{\"baseUrl\":\"http://h/v1\",\"models\":[{\"id\":\"m\"}]}}}";
            CHECK_INT(llm_reg_overlay(&r, noapi, (long)strlen(noapi)), -1);
            CHECK(strstr(r.err, "no \"api\"") != 0);
            CHECK_INT(r.n, n0);
        }
        {
            static const char empty[] = "{\"providers\":{\"openai\":{}}}";
            CHECK_INT(llm_reg_overlay(&r, empty, (long)strlen(empty)), -1);
            CHECK(strstr(r.err, "must specify") != 0);
        }
    }
    llm_reg_free(&r);
}

/* ---- thinking levels: pi's getSupportedThinkingLevels and clampThinkingLevel ---- */

static void test_think(void)
{
    llm_reg r;
    char *models, *exp, *p, *l;
    long n, en;
    int lines = 0;
    llm_reg_init(&r);
    models = slurp("think.models", &n);
    exp = slurp("think.expected", &en);
    if (!models || !exp)
        return;
    CHECK_INT(llm_reg_overlay(&r, models, n), 0);
    p = exp;
    while ((l = next_line(&p)) != 0) {
        char id[64], kind[16], rest[96];
        const llm_model *m;
        if (sscanf(l, "%63s %15s %95[^\n]", id, kind, rest) != 3)
            continue;
        m = llm_reg_find(&r, "t", id);
        CHECK(m != 0);
        if (!m)
            continue;
        lines++;
        if (!strcmp(kind, "supported")) {
            char got[96];
            unsigned mask = llm_think_levels(m);
            int i;
            got[0] = 0;
            for (i = 0; i < LT_N; i++)
                if (mask & (1u << i)) {
                    if (got[0])
                        strcat(got, ",");
                    strcat(got, llm_think_name(i));
                }
            CHECK_STR(got, rest);
        } else {
            char lv[16], want[16];
            sscanf(rest, "%15s %15s", lv, want);
            CHECK_STR(llm_think_name(llm_think_clamp(m, llm_think_of(lv))), want);
        }
    }
    CHECK(lines > 40);
    CHECK_INT(llm_think_of("nope"), -1);
    free(models);
    free(exp);
    llm_reg_free(&r);
}

/* ---- model patterns: pi's parseModelPattern; a known provider with an unknown id is pi's fallback model ---- */

static void test_resolve(void)
{
    llm_reg r;
    char *models, *exp, *p, *l;
    long n, en;
    int lines = 0;
    llm_reg_init(&r);
    models = slurp("resolve.models", &n);
    exp = slurp("resolve.expected", &en);
    if (!models || !exp)
        return;
    CHECK_INT(llm_reg_overlay(&r, models, n), 0);
    p = exp;
    while ((l = next_line(&p)) != 0) {
        char *pat = l, *want = strchr(l, '\t'), *lvl;
        llm_model m;
        char err[160], got[160];
        int level, rc;
        if (!want)
            continue;
        *want++ = 0;
        lvl = strchr(want, '\t');
        *lvl++ = 0;
        lines++;
        rc = llm_reg_resolve(&r, 0, pat, &m, &level, err, sizeof(err));
        if (strcmp(want, "-")) {
            CHECK_INT(rc, 0);
            if (rc)
                continue;
            strcpy(got, m.provider);
            strcat(got, "/");
            strcat(got, m.id);
            CHECK_STR(got, want);
            CHECK_STR(level < 0 ? "-" : llm_think_name(level), lvl);
        } else {
            /* pi finds nothing: with a known provider prefix the model is made up under that id, else an error */
            const char *sl = strchr(pat, '/');
            int known = sl && (!strncmp(pat, "anthropic/", 10) || !strncmp(pat, "openai/", 7) || !strncmp(pat, "ollama/", 7));
            CHECK_INT(rc, known ? 0 : -1);
            if (known && !rc) {
                CHECK_STR(m.id, sl + 1);
                CHECK(m.api != 0);
            } else if (rc)
                CHECK(strstr(err, "not found") != 0);
        }
    }
    CHECK(lines >= 20);
    /* --provider names the provider; an unknown one is refused in pi's words */
    {
        llm_model m;
        char err[160];
        int level;
        CHECK_INT(llm_reg_resolve(&r, "OpenAI", "gpt-4.1", &m, &level, err, sizeof(err)), 0);
        CHECK_STR(m.provider, "openai");
        CHECK_INT(llm_reg_resolve(&r, "nope", "gpt", &m, &level, err, sizeof(err)), -1);
        CHECK_STR(err, "Unknown provider \"nope\". Use --list-models to see available providers/models.");
    }
    free(models);
    free(exp);
    llm_reg_free(&r);
}

/* ---- F2: config values, pi's resolveConfigValue; commands run once ---- */

typedef struct envtab {
    char name[3][48], val[3][48];
    int n;
    long cmds;
} envtab;
static envtab et;

static long t_getenv(void *u, const char *name, char *out, long cap)
{
    int i;
    (void)u;
    for (i = 0; i < et.n; i++)
        if (!strcmp(et.name[i], name) && (long)strlen(et.val[i]) < cap) {
            strcpy(out, et.val[i]);
            return (long)strlen(out);
        }
    return -1;
}

/* a command's output: the host shell, trimmed (the agent runs vsh/C: on the Amiga) */
static long t_run(void *u, const char *cmd, char *out, long cap)
{
    FILE *f;
    long n;
    char line[1024];
    (void)u;
    et.cmds++;
    strcpy(line, cmd);
    f = popen(line, "r");
    if (!f)
        return -1;
    n = (long)fread(out, 1, (size_t)cap - 1, f);
    if (pclose(f) != 0)
        return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\r'))
        n--;
    {
        long s = 0;
        while (s < n && (out[s] == ' ' || out[s] == '\n'))
            s++;
        memmove(out, out + s, (size_t)(n - s));
        n -= s;
    }
    out[n] = 0;
    return n > 0 ? n : -1;
}

/* the env of one case, from its JSON: {"PI_T_A":"1"} */
static void load_env(jv env)
{
    jit it;
    jv k, v;
    et.n = 0;
    json_iter(env, &it);
    while (json_next(&it, &k, &v) && et.n < 3) {
        json_str(k, et.name[et.n], sizeof(et.name[0]));
        json_str(v, et.val[et.n], sizeof(et.val[0]));
        et.n++;
    }
}

static void test_cfg(void)
{
    char *cases, *exp, *pc, *pe, *lc, *le;
    long n, en;
    int lines = 0;
    llm_cfg c;
    cases = slurp("cfg.cases", &n);
    exp = slurp("cfg.expected", &en);
    if (!cases || !exp)
        return;
    pc = cases;
    pe = exp;
    while ((lc = next_line(&pc)) != 0 && (le = next_line(&pe)) != 0) {
        jv o, v;
        char name[64], value[200], got[300], want[300], *tab;
        long r;
        json_parse(lc, (long)strlen(lc), &o);
        json_get(o, "name", &v);
        json_str(v, name, sizeof(name));
        json_get(o, "value", &v);
        json_str(v, value, sizeof(value));
        json_get(o, "env", &v);
        load_env(v);
        llm_cfg_init(&c);
        c.getenv = t_getenv;
        c.run = t_run;
        r = llm_cfg_value(&c, value, got, sizeof(got));
        tab = strchr(le, '\t');
        strcpy(want, tab ? tab + 1 : "");
        if (r < 0) {
            CHECK_STR("UNRESOLVED", want);
        } else {
            /* the oracle prints JSON.stringify of the value */
            jw w;
            jw_init(&w);
            jw_strz(&w, got);
            CHECK_STR(w.p, want);
            jw_free(&w);
        }
        lines++;
        llm_cfg_free(&c);
    }
    CHECK(lines == 15);
    free(cases);
    free(exp);

    /* a command is run once per process however many requests resolve it, a failure is cached too */
    et.n = 0;
    et.cmds = 0;
    llm_cfg_init(&c);
    c.getenv = t_getenv;
    c.run = t_run;
    {
        char out[64];
        int i;
        for (i = 0; i < 3; i++)
            CHECK_INT(llm_cfg_value(&c, "!echo sk-from-command", out, sizeof(out)), 15);
        CHECK_STR(out, "sk-from-command");
        CHECK_INT(et.cmds, 1);
        CHECK_INT(c.runs, 1);
        for (i = 0; i < 2; i++)
            CHECK_INT(llm_cfg_value(&c, "!exit 1", out, sizeof(out)), -1);
        CHECK_INT(et.cmds, 2);
    }
    llm_cfg_free(&c);
}

/* ---- F2: the credential order --api-key > auth.json > models.json > environment ---- */

static void test_cred_order(void)
{
    llm_cfg c;
    llm_model m;
    char out[128];
    const char *src;
    static const char auth[] = "{\"openai\":{\"type\":\"api_key\",\"key\":\"$PI_T_AUTHKEY\"},"
                               "\"ollama\":{\"type\":\"oauth\",\"access\":\"x\"}}";
    llm_model_init(&m);
    m.provider = "openai";
    m.id = "gpt-5.5";
    m.key = "models-json-key";
    llm_cfg_init(&c);
    c.getenv = t_getenv;
    c.run = t_run;
    et.n = 2;
    strcpy(et.name[0], "OPENAI_API_KEY");
    strcpy(et.val[0], "env-key");
    strcpy(et.name[1], "PI_T_AUTHKEY");
    strcpy(et.val[1], "auth-key");
    c.auth = auth;
    c.auth_n = (long)strlen(auth);
    strcpy(c.runtime_key, "cli-key");
    strcpy(c.runtime_provider, "openai");
    CHECK(llm_cfg_key(&c, &m, out, sizeof(out), &src) > 0);
    CHECK_STR(out, "cli-key");
    CHECK_STR(src, "--api-key");
    c.runtime_key[0] = 0;
    CHECK(llm_cfg_key(&c, &m, out, sizeof(out), &src) > 0);
    CHECK_STR(out, "auth-key");
    CHECK_STR(src, "auth.json");
    c.auth = 0;
    CHECK(llm_cfg_key(&c, &m, out, sizeof(out), &src) > 0);
    CHECK_STR(out, "models-json-key");
    CHECK_STR(src, "models.json");
    m.key = 0;
    CHECK(llm_cfg_key(&c, &m, out, sizeof(out), &src) > 0);
    CHECK_STR(out, "env-key");
    CHECK_STR(src, "environment");
    et.n = 0;
    CHECK_INT(llm_cfg_key(&c, &m, out, sizeof(out), &src), 0);
    CHECK(src == 0);
    /* --api-key belongs to the provider it was given for */
    strcpy(c.runtime_key, "cli-key");
    strcpy(c.runtime_provider, "anthropic");
    CHECK_INT(llm_cfg_key(&c, &m, out, sizeof(out), &src), 0);
    /* an unresolved models.json value falls through to the environment */
    m.key = "$PI_T_NOT_SET";
    et.n = 1;
    strcpy(et.name[0], "OPENAI_API_KEY");
    strcpy(et.val[0], "env-key");
    c.runtime_key[0] = 0;
    CHECK(llm_cfg_key(&c, &m, out, sizeof(out), &src) > 0);
    CHECK_STR(src, "environment");
    llm_cfg_free(&c);
}

static void test_headers(void)
{
    llm_cfg c;
    llm_model m;
    jw w;
    llm_model_init(&m);
    m.provider = "x";
    m.headers = "{\"X-Team\":\"$PI_T_TEAM\",\"X-Fixed\":\"1\"}";
    m.auth_header = 1;
    llm_cfg_init(&c);
    c.getenv = t_getenv;
    et.n = 1;
    strcpy(et.name[0], "PI_T_TEAM");
    strcpy(et.val[0], "amiga");
    jw_init(&w);
    CHECK_INT(llm_cfg_headers(&c, &m, "k1", &w), 0);
    CHECK_STR(w.p, "X-Team: amiga\r\nX-Fixed: 1\r\nAuthorization: Bearer k1\r\n");
    jw_reset(&w);
    et.n = 0;
    CHECK_INT(llm_cfg_headers(&c, &m, "k1", &w), -1);
    CHECK_INT(w.n, 0);
    jw_free(&w);
    llm_cfg_free(&c);
}

/* ---- shortHash ---- */

static void test_hash(void)
{
    char *exp, *p, *l;
    int lines = 0;
    exp = slurp("hash.expected", 0);
    if (!exp)
        return;
    p = exp;
    while ((l = next_line(&p)) != 0) {
        char *tab = strchr(l, '\t'), h[32];
        if (!tab)
            continue;
        *tab++ = 0;
        llm_short_hash(l, h, sizeof(h));
        CHECK_STR(h, tab);
        lines++;
    }
    CHECK_INT(lines, 5);
    free(exp);
}

/* ---- F3: the handoff, line by line against pi's transformMessages ---- */

/* the value with object keys sorted, as the oracle writes it */
static void canon(jv v, jw *o)
{
    jit it;
    jv k, x;
    if (json_type(v) == J_ARR) {
        int n = 0;
        jw_raw(o, "[", 1);
        json_iter(v, &it);
        while (json_next(&it, 0, &x)) {
            jw_rawz(o, n++ ? "," : "");
            canon(x, o);
        }
        jw_raw(o, "]", 1);
    } else if (json_type(v) == J_OBJ) {
        jv keys[64], vals[64];
        int n = 0, i, j;
        json_iter(v, &it);
        while (json_next(&it, &k, &x) && n < 64) {
            for (i = 0; i < n; i++)
                if (strncmp(keys[i].p, k.p, (size_t)(keys[i].n < k.n ? keys[i].n : k.n)) > 0)
                    break;
            for (j = n; j > i; j--) {
                keys[j] = keys[j - 1];
                vals[j] = vals[j - 1];
            }
            keys[i] = k;
            vals[i] = x;
            n++;
        }
        jw_raw(o, "{", 1);
        for (i = 0; i < n; i++) {
            jw_rawz(o, i ? "," : "");
            jw_raw(o, keys[i].p, keys[i].n);
            jw_raw(o, ":", 1);
            canon(vals[i], o);
        }
        jw_raw(o, "}", 1);
    } else
        jw_raw(o, v.p, v.n);
}

static void target_model(jv t, llm_model *m, char *prov, char *api, char *id)
{
    jv v, e;
    jit it;
    llm_model_init(m);
    json_get(t, "provider", &v);
    json_str(v, prov, 64);
    json_get(t, "api", &v);
    json_str(v, api, 64);
    json_get(t, "id", &v);
    json_str(v, id, 96);
    m->provider = prov;
    m->api = api;
    m->id = id;
    json_get(t, "input", &v);
    json_iter(v, &it);
    while (json_next(&it, 0, &e))
        if (json_streq(e, "image"))
            m->image = 1;
}

static void test_handoff(void)
{
    char *cases, *exp, *pc, *pe, *lc, *le;
    int lines = 0;
    cases = slurp("handoff.cases", 0);
    exp = slurp("handoff.expected", 0);
    if (!cases || !exp)
        return;
    pc = cases;
    pe = exp;
    while ((lc = next_line(&pc)) != 0 && (le = next_line(&pe)) != 0) {
        jv o, v, msgs, mm, tg;
        jit it;
        char name[64], prov[64], api[64], id[96];
        const char *ms[64];
        long mn[64];
        int count = 0, i;
        llm_model m;
        llm_view view;
        llm_ts ts;
        jw got;
        char *tab = strchr(le, '\t');
        json_parse(lc, (long)strlen(lc), &o);
        json_get(o, "name", &v);
        json_str(v, name, sizeof(name));
        json_get(o, "target", &tg);
        target_model(tg, &m, prov, api, id);
        json_get(o, "messages", &msgs);
        json_iter(msgs, &it);
        while (json_next(&it, 0, &mm) && count < 64) {
            ms[count] = mm.p;
            mn[count++] = mm.n;
        }
        ts.sec = 1760100000L;
        ts.ms = 0;
        CHECK_INT(llm_prepare(&m, ms, mn, count, ts, &view), 0);
        jw_init(&got);
        jw_raw(&got, "[", 1);
        for (i = 0; i < view.count; i++) {
            jv mv;
            jw_rawz(&got, i ? "," : "");
            json_parse(view.m[i], view.n[i], &mv);
            canon(mv, &got);
        }
        jw_raw(&got, "]", 1);
        CHECK(tab != 0);
        if (tab && strcmp(got.p, tab + 1)) {
            h_checks++;
            h_failures++;
            printf("  FAIL handoff %s differs:\n--- pi\n%s\n--- got\n%s\n", name, tab + 1, got.p);
        } else
            h_checks++;
        jw_free(&got);
        llm_view_free(&view);
        lines++;
    }
    CHECK_INT(lines, 12);
    free(cases);
    free(exp);
}

void suite_pi_model(void)
{
    test_catalog();
    test_overlay();
    test_think();
    test_resolve();
    test_cfg();
    test_cred_order();
    test_headers();
    test_hash();
    test_handoff();
}
