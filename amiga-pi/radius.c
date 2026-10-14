/* radius -- see radius.h. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "radius.h"
#include "authstore.h"
#include "llm/radius.h"
#include "claude/util.h"

static int is_default(const llm_rad *r)
{
    char g[200];
    llm_radius_gateway(r->gateway, 0, g, sizeof(g));
    return !strcmp(g, LLM_RADIUS_GATEWAY);
}

/* the provider's catalog from models-store.json: the "models" array as malloc'ed text, 0 none. A store that is not an object, or
 * an entry without the array, is none */
static char *stored_models(pi_models *m, const char *id, long *n)
{
    long tn = 0;
    char *t = pi_store_read(m->sys, m->agent_dir, PI_MODELS_STORE, &tn), *r = 0;
    jv root, e, a;
    if (!t)
        return 0;
    if (!json_parse(t, tn, &root) && json_type(root) == J_OBJ && json_get(root, id, &e) && json_type(e) == J_OBJ &&
        json_get(e, "models", &a) && json_type(a) == J_ARR)
    {
        r = (char *)malloc((size_t)a.n + 1);
        if (r) {
            memcpy(r, a.p, (size_t)a.n);
            r[a.n] = 0;
            *n = a.n;
        }
    }
    free(t);
    return r;
}

/* the OAuth entry's gatewayConfig of auth.json (OAuthCredential & { gatewayConfig }): the models it describes, 0 none */
static int legacy_models(pi_models *m, const char *id, jw *out)
{
    jv root, e, t, c;
    int rc = -1;
    if (!m->auth || json_parse(m->auth, m->auth ? (long)strlen(m->auth) : 0, &root) || json_type(root) != J_OBJ ||
        !json_get(root, id, &e) || json_type(e) != J_OBJ || !json_get(e, "type", &t) || !json_streq(t, "oauth") ||
        !json_get(e, "gatewayConfig", &c) || json_type(c) != J_OBJ)
        return -1;
    jw_init(out);
    rc = llm_radius_models(id, c.p, c.n, out);
    if (rc)
        jw_free(out);
    return rc;
}

/* models-store.json with the provider's entry set */
static void store_entry(pi_models *m, const char *id, const char *models, long n, llm_ts now)
{
    long tn = 0;
    char *t = pi_store_read(m->sys, m->agent_dir, PI_MODELS_STORE, &tn), err[200];
    jw entry, out;
    jw_init(&entry);
    jw_init(&out);
    jw_rawz(&entry, "{\"models\":");
    jw_raw(&entry, models, n);
    jw_rawz(&entry, ",\"checkedAt\":");
    oa_write_ms(&entry, now.sec, now.ms);
    jw_rawz(&entry, "}");
    jw_raw(&entry, "", 1);                      /* terminated for pi_jobj_set's C string */
    if (!entry.oom && !pi_jobj_set(t ? t : "", t ? tn : 0, id, entry.p, &out, err, sizeof(err)))
        pi_store_write(m->sys, m->agent_dir, PI_MODELS_STORE, out.p, out.n, err, sizeof(err));
    jw_free(&entry);
    jw_free(&out);
    free(t);
}

/* OpenRouter's per-key list kept in the store ("openrouter": {models, checkedAt}) narrows the shipped catalog with no network; an empty
 * or missing list leaves the shipped models (withPersonalizedCatalog) */
static void or_restore(pi_models *m)
{
    long n = 0;
    char *st;
    llm_or_entry *e;
    int k;
    if (!llm_reg_has_provider(&m->reg, LLM_OPENROUTER))
        return;
    st = stored_models(m, LLM_OPENROUTER, &n);
    if (!st)
        return;
    e = (llm_or_entry *)malloc(LLM_OR_MAX * sizeof(*e));
    if (e) {
        k = llm_or_stored(st, n, e, LLM_OR_MAX);
        if (k > 0)
            llm_or_apply(&m->reg, e, k);
        free(e);
    }
    free(st);
}

void pi_radius_restore(pi_models *m)
{
    int i;
    char err[200];
    for (i = 0; i < m->reg.nrad; i++) {
        const llm_rad *r = &m->reg.rad[i];
        long n = 0;
        char *st = stored_models(m, r->id, &n);
        jw lg;
        if (st) {
            llm_radius_apply(&m->reg, r->id, st, n, err, sizeof(err));
            free(st);
        } else if (!legacy_models(m, r->id, &lg)) {
            if (!llm_radius_apply(&m->reg, r->id, lg.p, lg.n, err, sizeof(err))) {
                llm_ts z;
                z.sec = (long)time(0);          /* persist: {models, checkedAt: Date.now()} */
                z.ms = 0;
                store_entry(m, r->id, lg.p, lg.n, z);
            }
            jw_free(&lg);
        } else if (!is_default(r))
            llm_reg_drop_provider(&m->reg, r->id);    /* a custom gateway does not inherit the public catalog */
    }
    or_restore(m);
}

/* loadRadiusGatewayConfig's failure words */
static void trunc512(const char *p, long n, char *out, long cap)
{
    long s = 0, e = n;
    while (s < e && (p[s] == ' ' || p[s] == '\n' || p[s] == '\r' || p[s] == '\t'))
        s++;
    while (e > s && (p[e - 1] == ' ' || p[e - 1] == '\n' || p[e - 1] == '\r' || p[e - 1] == '\t'))
        e--;
    n = e - s;
    if (n > 512) {
        n = 512;
        cl_copy(out, "", cap);
        if (n < cap - 4) {
            memcpy(out, p + s, (size_t)n);
            out[n] = 0;
            cl_cat(out, "\xe2\x80\xa6", cap);
        }
        return;
    }
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p + s, (size_t)n);
    out[n] = 0;
}

static int refresh_one(pi_auth *a, const llm_rad *r, char *err, long cap)
{
    oa_io io;
    jw resp, models;
    char url[300], hdrs[4400], key[4200], num[16], body[600];
    static char gw[200];
    int status = 0, rc = -1;
    llm_model t;
    pi_models *m = a->m;
        if (pi_auth_provider_token(a, r->id, key, sizeof(key)) != 0) {
        llm_model_init(&t);
        t.provider = r->id;
        t.api = "pi-messages";
        t.id = "";
        key[0] = 0;
        llm_cfg_key(&m->cfg, &t, key, sizeof(key), 0);
    }
    if (!key[0])
        return 0;                                /* no credential: no network (models.ts refresh) */
    if (strncmp(r->gateway, "https://", 8))
        return 0;                                /* plain http: the credential stays home */
    llm_radius_gateway(r->gateway, 0, gw, sizeof(gw));
    cl_copy(url, gw, sizeof(url));
    cl_cat(url, "/v1/config", sizeof(url));
    cl_copy(hdrs, "accept: application/json\r\nauthorization: Bearer ", sizeof(hdrs));
    cl_cat(hdrs, key, sizeof(hdrs));
    cl_cat(hdrs, "\r\n", sizeof(hdrs));
    memset(key, 0, sizeof(key));
    pi_auth_io(a, &io);
    jw_init(&resp);
    jw_init(&models);
    if (io.http(io.u, "GET", url, hdrs, 0, 0, &status, &resp, err, cap)) {
        memset(hdrs, 0, sizeof(hdrs));
        jw_free(&resp);
        return -1;
    }
    memset(hdrs, 0, sizeof(hdrs));
    if (status < 200 || status > 299) {
        trunc512(resp.p ? resp.p : "", resp.n, body, sizeof(body));
        cl_ltoa(status, num);
        cl_copy(err, "Could not load Radius config from ", cap);
        cl_cat(err, gw, cap);
        cl_cat(err, ": ", cap);
        cl_cat(err, num, cap);
        cl_cat(err, ": ", cap);
        cl_cat(err, body, cap);
    } else if (!resp.p || llm_radius_models(r->id, resp.p, resp.n, &models)) {
        cl_copy(err, "Invalid Radius config from ", cap);
        cl_cat(err, gw, cap);
    } else {
        char e2[200];
        if (llm_radius_apply(&m->reg, r->id, models.p, models.n, e2, sizeof(e2))) {
            cl_copy(err, e2, cap);
        } else {
            pi_models_relay(m);
            store_entry(m, r->id, models.p, models.n, io.now(io.u));
            rc = 1;
        }
    }
    jw_free(&resp);
    jw_free(&models);
    return rc;
}

/* OpenRouter: GET <baseUrl>/models/user with the key narrows the shipped catalog to the chat models the key may use, with their limits
 * and prices (personalizeOpenRouterCatalog); 401, 403 and 404 keep the shipped catalog; any other failure leaves the models as they are.
 * The answer is read as a stream (llm_or_user), whatever its size. 1 refreshed, 0 nothing to do, -1 failed (words in err) */
typedef struct or_sink {
    llm_or_user *p;
    int oom;
} or_sink;

static void or_body(void *u, const char *s, long n)
{
    or_sink *k = (or_sink *)u;
    if (llm_or_user_feed(k->p, s, n))
        k->oom = 1;
}

static int or_refresh(pi_auth *a, char *err, long cap)
{
    oa_io io;
    char url[300], hdrs[4400], key[4200], num[16], base[240];
    int status = 0, rc = -1, i;
    llm_model t;
    pi_models *m = a->m;
    or_sink k;
    llm_or_entry *e;
    jw models;
    const llm_model *first = 0;
    if (!llm_reg_has_provider(&m->reg, LLM_OPENROUTER))
        return 0;
    if (pi_auth_provider_token(a, LLM_OPENROUTER, key, sizeof(key)) != 0) {
        llm_model_init(&t);
        t.provider = LLM_OPENROUTER;
        t.api = "openai-completions";
        t.id = "";
        key[0] = 0;
        llm_cfg_key(&m->cfg, &t, key, sizeof(key), 0);
    }
    if (!key[0])
        return 0;                                /* no credential: the shipped catalog stands */
    for (i = 0; i < m->reg.n && !first; i++)
        if (!strcmp(m->reg.m[i].provider, LLM_OPENROUTER))
            first = &m->reg.m[i];
    cl_copy(base, first && first->base_url && *first->base_url ? first->base_url : LLM_OPENROUTER_BASE, sizeof(base));
    i = (int)strlen(base);
    while (i > 0 && base[i - 1] == '/')
        base[--i] = 0;
    if (strncmp(base, "https://", 8))
        return 0;                                /* plain http: the credential stays home */
    cl_copy(url, base, sizeof(url));
    cl_cat(url, "/models/user", sizeof(url));
    cl_copy(hdrs, "accept: application/json\r\nauthorization: Bearer ", sizeof(hdrs));
    cl_cat(hdrs, key, sizeof(hdrs));
    cl_cat(hdrs, "\r\n", sizeof(hdrs));
    memset(key, 0, sizeof(key));
    pi_auth_io(a, &io);
    if (!io.http_stream)
        return 0;
    k.p = llm_or_user_new();
    k.oom = 0;
    if (!k.p) {
        cl_copy(err, "out of memory", cap);
        return -1;
    }
    if (io.http_stream(io.u, "GET", url, hdrs, &status, or_body, &k, err, cap)) {
        memset(hdrs, 0, sizeof(hdrs));
        llm_or_user_free(k.p);
        return -1;
    }
    memset(hdrs, 0, sizeof(hdrs));
    if (status == 401 || status == 403 || status == 404) {
        llm_or_reset(&m->reg);                   /* the catalog returned unchanged, saved all the same */
        rc = 1;
    } else if (status < 200 || status > 299) {
        cl_ltoa(status, num);
        cl_copy(err, "OpenRouter model list request failed: ", cap);
        cl_cat(err, num, cap);
    } else if (k.oom || llm_or_user_end(k.p)) {
        cl_copy(err, k.oom ? "out of memory" : "Invalid OpenRouter model list response", cap);
    } else {
        llm_or_reset(&m->reg);
        e = (llm_or_entry *)malloc(LLM_OR_MAX * sizeof(*e));
        if (e) {
            for (i = 0; i < llm_or_user_count(k.p); i++)
                e[i] = *llm_or_user_get(k.p, i);
            llm_or_apply(&m->reg, e, llm_or_user_count(k.p));
            free(e);
            rc = 1;
        } else
            cl_copy(err, "out of memory", cap);
    }
    if (rc == 1) {
        jw_init(&models);
        llm_or_store(&m->reg, &models);
        pi_models_relay(m);
        store_entry(m, LLM_OPENROUTER, models.p, models.n, io.now(io.u));
        jw_free(&models);
    }
    llm_or_user_free(k.p);
    return rc;
}

int pi_radius_refresh(pi_auth *a, const char *only, char *err, long cap)
{
    int i, ok = 0;
    char e[400];
    err[0] = 0;
    if (a->getenv && a->getenv("PI_OFFLINE", e, sizeof(e)) >= 0)
        return 0;                                /* PI_OFFLINE set (to anything): no model network at all (model-runtime.ts) */
    for (i = 0; i < a->m->reg.nrad; i++) {
        const llm_rad *r = &a->m->reg.rad[i];
        int rc;
        if (only && strcmp(only, r->id))
            continue;
        e[0] = 0;
        rc = refresh_one(a, r, e, sizeof(e));
        if (rc > 0)
            ok++;
        else if (rc < 0)
            cl_copy(err, e, cap);
    }
    if (!only || !strcmp(only, LLM_OPENROUTER)) {
        int rc;
        e[0] = 0;
        rc = or_refresh(a, e, sizeof(e));
        if (rc > 0)
            ok++;
        else if (rc < 0)
            cl_copy(err, e, cap);
    }
    return ok;
}
