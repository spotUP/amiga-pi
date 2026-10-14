/* see scoped.h */
#include <stdlib.h>
#include <string.h>
#include "scoped.h"
#include "authstore.h"
#include "claude/json.h"
#include "claude/util.h"

void pi_scope_id(const llm_model *m, char *out, long cap)
{
    cl_copy(out, m->provider, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, m->id, cap);
}

static const llm_model *find_model(const pi_scope *s, const char *id)
{
    char f[400];
    int i;
    for (i = 0; i < s->nall; i++) {
        pi_scope_id(s->all[i], f, sizeof(f));
        if (!strcmp(f, id))
            return s->all[i];
    }
    return 0;
}

static int en_find(const pi_scope *s, const char *id)
{
    int i;
    for (i = 0; i < s->nen; i++)
        if (!strcmp(s->en[i], id))
            return i;
    return -1;
}

static int en_add(pi_scope *s, const char *id)
{
    char *c;
    if (s->nen == s->encap) {
        int nc = s->encap ? s->encap * 2 : 16;
        char **n = (char **)realloc(s->en, sizeof(char *) * (size_t)nc);
        if (!n)
            return -1;
        s->en = n;
        s->encap = nc;
    }
    c = (char *)malloc(strlen(id) + 1);
    if (!c)
        return -1;
    strcpy(c, id);
    s->en[s->nen++] = c;
    return 0;
}

static void en_remove_at(pi_scope *s, int i)
{
    free(s->en[i]);
    memmove(s->en + i, s->en + i + 1, sizeof(char *) * (size_t)(s->nen - i - 1));
    s->nen--;
}

static void en_reset(pi_scope *s)
{
    while (s->nen)
        en_remove_at(s, s->nen - 1);
}

/* the list the selector shows (getSortedIds) */
static void rebuild(pi_scope *s)
{
    int i, k = 0;
    char f[400];
    free(s->item);
    s->item = (pi_scope_item *)calloc((size_t)(s->nall + s->nen + 1), sizeof(pi_scope_item));
    s->nitem = 0;
    if (!s->item)
        return;
    if (s->en_null) {
        for (i = 0; i < s->nall; i++) {
            pi_scope_id(s->all[i], f, sizeof(f));
            s->item[k].m = s->all[i];
            s->item[k].id = 0;
            s->item[k].enabled = 1;
            k++;
        }
    } else {
        for (i = 0; i < s->nen; i++) {
            s->item[k].m = find_model(s, s->en[i]);
            s->item[k].id = s->en[i];
            s->item[k].enabled = 1;
            k++;
        }
        for (i = 0; i < s->nall; i++) {
            pi_scope_id(s->all[i], f, sizeof(f));
            if (en_find(s, f) < 0) {
                s->item[k].m = s->all[i];
                s->item[k].id = 0;
                s->item[k].enabled = 0;
                k++;
            }
        }
    }
    s->nitem = k;
}

void pi_scope_item_id(const pi_scope_item *it, char *out, long cap)
{
    if (it->id)
        cl_copy(out, it->id, cap);
    else
        pi_scope_id(it->m, out, cap);
}

int pi_scope_open(pi_scope *s, pi_models *ms)
{
    int i, n = 0, *idx = 0;
    char f[400];
    memset(s, 0, sizeof(*s));
    s->ms = ms;
    s->all = (const llm_model **)malloc(sizeof(llm_model *) * (size_t)(ms->reg.n + 1));
    if (!s->all)
        return -1;
    for (i = 0; i < ms->reg.n; i++)
        if (pi_model_usable(ms, &ms->reg.m[i]))
            s->all[s->nall++] = &ms->reg.m[i];
    s->en_null = 1;
    if (ms->scope && ms->scope[0]) {
        const char *p = ms->scope;
        idx = (int *)malloc(sizeof(int) * (size_t)(ms->reg.n + 1));
        if (!idx)
            return -1;
        s->en_null = 0;
        while (*p) {
            char pat[300];
            const char *nl = strchr(p, '\n');
            long l = nl ? (long)(nl - p) : (long)strlen(p);
            if (l >= (long)sizeof(pat))
                l = (long)sizeof(pat) - 1;
            memcpy(pat, p, (size_t)l);
            pat[l] = 0;
            p = nl ? nl + 1 : p + strlen(p);
            if (!pat[0])
                continue;
            n = pi_models_pattern(ms, pat, idx, ms->reg.n);
            if (!n) {
                if (en_find(s, pat) < 0 && en_add(s, pat))
                    return -1;
            } else {
                int k;
                for (k = 0; k < n; k++) {
                    pi_scope_id(&ms->reg.m[idx[k]], f, sizeof(f));
                    if (en_find(s, f) < 0 && en_add(s, f))
                        return -1;
                }
            }
        }
        free(idx);
    }
    rebuild(s);
    return s->item ? 0 : -1;
}

void pi_scope_free(pi_scope *s)
{
    en_reset(s);
    free(s->en);
    free(s->all);
    free(s->item);
    memset(s, 0, sizeof(*s));
}

int pi_scope_is_enabled(const pi_scope *s, const char *id)
{
    return s->en_null || en_find(s, id) >= 0;
}

/* normalizeEnabled: a list that covers every available model, and only those, is null */
static void normalize(pi_scope *s)
{
    int i;
    char f[400];
    if (s->en_null || s->nen != s->nall)
        return;
    for (i = 0; i < s->nen; i++)
        if (!find_model(s, s->en[i]))
            return;
    (void)f;
    en_reset(s);
    s->en_null = 1;
}

void pi_scope_toggle(pi_scope *s, const char *id)
{
    if (s->en_null) {
        int i;
        char f[400];
        s->en_null = 0;
        for (i = 0; i < s->nall; i++) {
            pi_scope_id(s->all[i], f, sizeof(f));
            if (strcmp(f, id))
                en_add(s, f);
        }
    } else {
        int at = en_find(s, id);
        if (at >= 0)
            en_remove_at(s, at);
        else {
            en_add(s, id);
            normalize(s);
        }
    }
    rebuild(s);
}

void pi_scope_enable(pi_scope *s, const char *const *ids, int n)
{
    int i;
    char f[400];
    if (s->en_null)
        return;
    if (n < 0) {
        for (i = 0; i < s->nall; i++) {
            pi_scope_id(s->all[i], f, sizeof(f));
            if (en_find(s, f) < 0)
                en_add(s, f);
        }
    } else
        for (i = 0; i < n; i++)
            if (en_find(s, ids[i]) < 0)
                en_add(s, ids[i]);
    normalize(s);
    rebuild(s);
}

void pi_scope_clear(pi_scope *s, const char *const *ids, int n)
{
    int i;
    if (s->en_null) {
        char f[400];
        s->en_null = 0;
        if (n >= 0)
            for (i = 0; i < s->nall; i++) {
                int hit = 0, k;
                pi_scope_id(s->all[i], f, sizeof(f));
                for (k = 0; k < n; k++)
                    if (!strcmp(ids[k], f))
                        hit = 1;
                if (!hit)
                    en_add(s, f);
            }
    } else if (n < 0)
        en_reset(s);
    else
        for (i = 0; i < n; i++) {
            int at = en_find(s, ids[i]);
            if (at >= 0)
                en_remove_at(s, at);
        }
    rebuild(s);
}

void pi_scope_move(pi_scope *s, const char *id, int delta)
{
    int at, to;
    char *t;
    if (s->en_null)
        return;
    at = en_find(s, id);
    to = at + delta;
    if (at < 0 || to < 0 || to >= s->nen)
        return;
    t = s->en[at];
    s->en[at] = s->en[to];
    s->en[to] = t;
    rebuild(s);
}

void pi_scope_provider(pi_scope *s, const char *provider)
{
    const char **ids;
    int i, n = 0, all_on = 1;
    char f[400];
    ids = (const char **)malloc(sizeof(char *) * (size_t)(s->nall + 1));
    if (!ids)
        return;
    for (i = 0; i < s->nall; i++)
        if (!strcmp(s->all[i]->provider, provider)) {
            char *c;
            pi_scope_id(s->all[i], f, sizeof(f));
            c = (char *)malloc(strlen(f) + 1);
            if (!c)
                continue;
            strcpy(c, f);
            ids[n++] = c;
            if (!pi_scope_is_enabled(s, f))
                all_on = 0;
        }
    if (all_on)
        pi_scope_clear(s, ids, n);
    else
        pi_scope_enable(s, ids, n);
    for (i = 0; i < n; i++)
        free((void *)ids[i]);
    free((void *)ids);
}

void pi_scope_count_text(const pi_scope *s, char *out, long cap)
{
    int i, have = 0, gone = 0;
    char num[16];
    if (s->en_null) {
        cl_copy(out, "all enabled", cap);
        return;
    }
    for (i = 0; i < s->nen; i++) {
        if (find_model(s, s->en[i]))
            have++;
        else
            gone++;
    }
    cl_ltoa(have, num);
    cl_copy(out, num, cap);
    cl_cat(out, "/", cap);
    cl_ltoa(s->nall, num);
    cl_cat(out, num, cap);
    cl_cat(out, " enabled", cap);
    if (gone) {
        cl_cat(out, " \302\267 ", cap);
        cl_ltoa(gone, num);
        cl_cat(out, num, cap);
        cl_cat(out, " unavailable", cap);
    }
}

int pi_scope_apply(pi_scope *s)
{
    int i, has = 0, every = 1;
    char f[400];
    if (!s->en_null) {
        for (i = 0; i < s->nen; i++)
            if (find_model(s, s->en[i]))
                has = 1;
        for (i = 0; i < s->nall; i++) {
            pi_scope_id(s->all[i], f, sizeof(f));
            if (en_find(s, f) < 0)
                every = 0;
        }
    }
    if (!s->en_null && has && !every) {
        jw w;
        int rc;
        jw_init(&w);
        for (i = 0; i < s->nen; i++) {
            if (i)
                jw_rawz(&w, "\n");
            jw_rawz(&w, s->en[i]);
        }
        rc = pi_models_set_scope(s->ms, w.p ? w.p : "");
        jw_free(&w);
        return rc;
    }
    return pi_models_set_scope(s->ms, "");
}

int pi_scope_save(pi_scope *s, cl_sys *sys, const char *dir)
{
    int i, every = 1, rc;
    char f[400];
    jw w;
    if (!s->en_null && s->nen == s->nall) {
        for (i = 0; i < s->nen; i++)
            if (!find_model(s, s->en[i]))
                every = 0;
    } else
        every = 0;
    jw_init(&w);
    if (s->en_null || every) {
        rc = pi_store_set_path(sys, dir, "enabledModels", 0);
    } else {
        jw_rawz(&w, "[");
        for (i = 0; i < s->nen; i++) {
            if (i)
                jw_rawz(&w, ",");
            jw_strz(&w, s->en[i]);
        }
        jw_rawz(&w, "]");
        rc = w.oom ? -1 : pi_store_set_path(sys, dir, "enabledModels", w.p);
    }
    (void)f;
    jw_free(&w);
    return rc;
}


int pi_scope_add_default(pi_models *ms, const llm_model *to, cl_sys *sys, const char *dir)
{
    int *idx, n, i, rc = 0;
    char ref[400], *joined, *text, lower[400];
    long tn = 0;
    jv root, arr, e;
    jit it;
    idx = (int *)malloc(sizeof(int) * (size_t)(ms->reg.n + 1));
    if (!idx)
        return -1;
    n = pi_models_scoped(ms, idx, ms->reg.n);
    for (i = 0; i < n; i++)
        if (!strcmp(ms->reg.m[idx[i]].provider, to->provider) && !strcmp(ms->reg.m[idx[i]].id, to->id)) {
            free(idx);
            return 0;                                   /* already in the scope */
        }
    free(idx);
    if (!n)
        return 0;                                       /* no scope: nothing to join */
    pi_scope_id(to, ref, sizeof(ref));
    joined = (char *)malloc(strlen(ms->scope) + strlen(ref) + 2);
    if (!joined)
        return -1;
    strcpy(joined, ms->scope);
    strcat(joined, "\n");
    strcat(joined, ref);
    if (pi_models_set_scope(ms, joined))
        rc = -1;
    free(joined);
    text = dir && dir[0] && sys ? pi_store_read(sys, dir, "settings.json", &tn) : 0;
    if (text && !json_parse(text, tn, &root) && json_get(root, "enabledModels", &arr) && json_type(arr) == J_ARR && json_count(arr) > 0) {
        jw w;
        int have = 0, k;
        for (k = 0; ref[k] && k < (int)sizeof(lower) - 1; k++)
            lower[k] = (char)(ref[k] >= 'A' && ref[k] <= 'Z' ? ref[k] + 32 : ref[k]);
        lower[k] = 0;
        jw_init(&w);
        jw_rawz(&w, "[");
        json_iter(arr, &it);
        while (json_next(&it, 0, &e)) {
            char b[400], lb[400];
            if (w.n > 1)
                jw_rawz(&w, ",");
            jw_raw(&w, e.p, e.n);
            if (json_type(e) == J_STR) {
                json_str(e, b, sizeof(b));
                for (k = 0; b[k] && k < (int)sizeof(lb) - 1; k++)
                    lb[k] = (char)(b[k] >= 'A' && b[k] <= 'Z' ? b[k] + 32 : b[k]);
                lb[k] = 0;
                if (!strcmp(lb, lower))
                    have = 1;
            }
        }
        if (!have) {
            jw_rawz(&w, ",");
            jw_strz(&w, ref);
            jw_rawz(&w, "]");
            if (w.oom || pi_store_set_path(sys, dir, "enabledModels", w.p))
                rc = -1;
        }
        jw_free(&w);
    }
    free(text);
    return rc;
}
