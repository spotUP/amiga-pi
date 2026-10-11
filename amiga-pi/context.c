/* see context.h */
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "claude/util.h"

#define COMPACTION_PREFIX "The conversation history before this point was compacted into the following summary:\n\n<summary>\n"
#define COMPACTION_SUFFIX "\n</summary>"
#define BRANCH_PREFIX "The following is a summary of a branch that this conversation came back from:\n\n<summary>\n"
#define BRANCH_SUFFIX "</summary>"

void ctx_init(pi_ctx *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->thinking, "off");
}

void ctx_free(pi_ctx *c)
{
    int i;
    for (i = 0; i < c->n; i++)
        free(c->msg[i]);
    free(c->msg);
    free(c->msg_n);
    free(c->esrc);
    free(c->efirst);
    free(c->ecnt);
    ctx_init(c);
}

int ctx_push(pi_ctx *c, const char *s, long n)
{
    char *z;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 32;
        char **m = (char **)realloc(c->msg, (size_t)nc * sizeof(*m));
        long *l;
        if (!m) {
            c->oom = 1;
            return -1;
        }
        c->msg = m;
        l = (long *)realloc(c->msg_n, (size_t)nc * sizeof(*l));
        if (!l) {
            c->oom = 1;
            return -1;
        }
        c->msg_n = l;
        c->cap = nc;
    }
    z = (char *)malloc((size_t)n + 1);
    if (!z) {
        c->oom = 1;
        return -1;
    }
    memcpy(z, s, (size_t)n);
    z[n] = 0;
    c->msg[c->n] = z;
    c->msg_n[c->n++] = n;
    return 0;
}

static int ctx_entry_add(pi_ctx *c, int src, int first, int cnt)
{
    if (c->ne == c->ecap) {
        int nc = c->ecap ? c->ecap * 2 : 32;
        int *a = (int *)realloc(c->esrc, (size_t)nc * sizeof(int));
        int *b, *d;
        if (!a) {
            c->oom = 1;
            return -1;
        }
        c->esrc = a;
        b = (int *)realloc(c->efirst, (size_t)nc * sizeof(int));
        if (!b) {
            c->oom = 1;
            return -1;
        }
        c->efirst = b;
        d = (int *)realloc(c->ecnt, (size_t)nc * sizeof(int));
        if (!d) {
            c->oom = 1;
            return -1;
        }
        c->ecnt = d;
        c->ecap = nc;
    }
    c->esrc[c->ne] = src;
    c->efirst[c->ne] = first;
    c->ecnt[c->ne++] = cnt;
    return 0;
}

static int ctx_add_jw(pi_ctx *c, jw *w)
{
    int rc = w->oom ? -1 : ctx_push(c, w->p, w->n);
    jw_reset(w);
    return rc;
}

/* an entry's time as pi's new Date(iso).getTime(): milliseconds, "null" (NaN) when it does not parse */
static void ts_ms(jv o, jw *w)
{
    jv v;
    char s[64];
    llm_ts t;
    if (json_get(o, "timestamp", &v) && json_type(v) == J_STR && json_str(v, s, sizeof(s)) >= 0 && !s3_iso_read(s, &t)) {
        s3_ms(t, w);
    } else
        jw_rawz(w, "null");
}

/* ,"key":<member of o as it is>, when o has it; comma only when !first */
static int put_member(jw *w, jv o, const char *key, const char *as, int *first)
{
    jv v;
    if (!json_get(o, key, &v))
        return 0;
    if (!*first)
        jw_raw(w, ",", 1);
    *first = 0;
    jw_raw(w, "\"", 1);
    jw_rawz(w, as ? as : key);
    jw_rawz(w, "\":");
    jw_raw(w, v.p, v.n);
    return 1;
}

static void put_key(jw *w, const char *key, int *first)
{
    if (!*first)
        jw_raw(w, ",", 1);
    *first = 0;
    jw_raw(w, "\"", 1);
    jw_rawz(w, key);
    jw_rawz(w, "\":");
}

/* sessionEntryToContextMessages for entry i: its messages appended to out (a leading system message
 * of a compaction first) as separate JSON texts into c. 0, -1 */
static int entry_messages(pi_s3 *st, int i, pi_ctx *c)
{
    long n;
    const char *t = s3_text(st, i, &n);
    jv o, m, v;
    jw w;
    int type = st->e[i].type, rc = 0, first = 1;
    if (!t || json_parse(t, n, &o))
        return -1;
    jw_init(&w);
    if (type == S3_MESSAGE) {
        char role[32];
        if (!json_get(o, "message", &m) || json_type(m) != J_OBJ) {
            jw_free(&w);
            return 0;
        }
        role[0] = 0;
        if (json_get(m, "role", &v) && json_type(v) == J_STR)
            json_str(v, role, sizeof(role));
        /* a file written elsewhere can hold a message with null or missing content */
        if (!json_get(m, "content", &v) || json_type(v) == J_NULL) {
            if (!strcmp(role, "system") && json_get(m, "content", &v) && json_type(v) == J_NULL)
                rc = s3_obj_put(m, &w, "content", "\"\"", 0);
            else if (!strcmp(role, "system"))
                rc = -2;        /* absent: pi leaves it as it is */
            else if (!strcmp(role, "user") || !strcmp(role, "assistant") || !strcmp(role, "toolResult"))
                rc = s3_obj_put(m, &w, "content", "[]", 0);
            else
                rc = -2;
        } else
            rc = -2;
        if (rc == -2)
            rc = ctx_push(c, m.p, m.n);
        else
            rc = rc ? -1 : ctx_add_jw(c, &w);
    } else if (type == S3_CUSTOM_MESSAGE) {
        jw_rawz(&w, "{\"role\":\"custom\"");
        first = 0;
        put_member(&w, o, "customType", 0, &first);
        put_key(&w, "content", &first);
        if (json_get(o, "content", &v) && json_type(v) != J_NULL)
            jw_raw(&w, v.p, v.n);
        else
            jw_rawz(&w, "[]");
        put_member(&w, o, "display", 0, &first);
        put_member(&w, o, "details", 0, &first);
        put_key(&w, "timestamp", &first);
        ts_ms(o, &w);
        jw_raw(&w, "}", 1);
        rc = ctx_add_jw(c, &w);
    } else if (type == S3_BRANCH_SUMMARY) {
        char *s = json_get(o, "summary", &v) && json_type(v) == J_STR ? json_strdup(v, &n) : 0;
        if (s && *s) {
            jw_rawz(&w, "{\"role\":\"branchSummary\"");
            first = 0;
            put_member(&w, o, "summary", 0, &first);
            put_member(&w, o, "fromId", 0, &first);
            put_key(&w, "timestamp", &first);
            ts_ms(o, &w);
            jw_raw(&w, "}", 1);
            rc = ctx_add_jw(c, &w);
        }
        free(s);
    } else if (type == S3_COMPACTION) {
        if (json_get(o, "systemMessage", &v) && json_type(v) != J_NULL && ctx_push(c, v.p, v.n))
            rc = -1;
        jw_rawz(&w, "{\"role\":\"compactionSummary\"");
        first = 0;
        put_member(&w, o, "summary", 0, &first);
        put_member(&w, o, "tokensBefore", 0, &first);
        put_key(&w, "timestamp", &first);
        ts_ms(o, &w);
        jw_raw(&w, "}", 1);
        if (!rc)
            rc = ctx_add_jw(c, &w);
    }
    jw_free(&w);
    return rc;
}

/* one message with a context edit's replacement applied (projectContextEntry); the edit entry j */
static int edit_message(pi_s3 *st, int j, pi_ctx *c, int from)
{
    long n;
    const char *t = s3_text(st, j, &n);
    jv o, r, rc_, mv, v;
    int k;
    if (!t || json_parse(t, n, &o))
        return -1;
    if (!json_get(o, "replacement", &r) || json_type(r) == J_NULL) {
        /* null omits every message of the entry */
        for (k = from; k < c->n; k++)
            free(c->msg[k]);
        c->n = from;
        return 0;
    }
    if (!json_get(r, "content", &rc_))
        return 0;
    for (k = from; k < c->n; k++) {
        char role[32];
        jw w;
        int rc, str;
        char *m = c->msg[k];
        long mn = c->msg_n[k];
        if (json_parse(m, mn, &mv))
            continue;
        role[0] = 0;
        if (json_get(mv, "role", &v) && json_type(v) == J_STR)
            json_str(v, role, sizeof(role));
        if (strcmp(role, "user") && strcmp(role, "assistant") && strcmp(role, "toolResult") && strcmp(role, "custom"))
            continue;
        str = json_type(rc_) == J_STR && (!strcmp(role, "assistant") || !strcmp(role, "toolResult"));
        jw_init(&w);
        if (str) {
            char *z;
            long zn;
            jw blocks;
            jw_init(&blocks);
            z = json_strdup(rc_, &zn);
            jw_rawz(&blocks, "[{\"type\":\"text\",\"text\":");
            jw_str(&blocks, z ? z : "", z ? zn : 0);
            jw_rawz(&blocks, "}]");
            free(z);
            rc = blocks.oom ? -1 : s3_obj_put(mv, &w, "content", blocks.p, 0);
            jw_free(&blocks);
        } else {
            char *raw = (char *)malloc((size_t)rc_.n + 1);
            if (!raw)
                return -1;
            memcpy(raw, rc_.p, (size_t)rc_.n);
            raw[rc_.n] = 0;
            rc = s3_obj_put(mv, &w, "content", raw, 0);
            free(raw);
        }
        if (rc || w.oom) {
            jw_free(&w);
            return -1;
        }
        free(c->msg[k]);
        c->msg[k] = w.p;
        c->msg_n[k] = w.n;
    }
    return 0;
}

int ctx_build(pi_s3 *st, int leaf, pi_ctx *c)
{
    int *path = 0, np, i, comp = -1, *ent = 0, ne = 0, rc = -1;
    int *edit = 0;
    ctx_init(c);
    if (leaf < 0)
        return 0;
    np = s3_path(st, leaf, &path);
    if (np < 0)
        return -1;
    /* thinking level and model: the whole path */
    for (i = 0; i < np; i++) {
        s3_ent *e = &st->e[path[i]];
        long n;
        const char *t;
        jv o, v, m;
        if (e->type == S3_THINKING || e->type == S3_MODEL || (e->type == S3_MESSAGE && e->role == S3R_ASSISTANT)) {
            t = s3_text(st, path[i], &n);
            if (!t || json_parse(t, n, &o))
                goto out;
            if (e->type == S3_THINKING && json_get(o, "thinkingLevel", &v) && json_type(v) == J_STR)
                json_str(v, c->thinking, sizeof(c->thinking));
            else if (e->type == S3_MODEL) {
                c->has_model = 1;
                c->provider[0] = c->model[0] = 0;
                if (json_get(o, "provider", &v) && json_type(v) == J_STR)
                    json_str(v, c->provider, sizeof(c->provider));
                if (json_get(o, "modelId", &v) && json_type(v) == J_STR)
                    json_str(v, c->model, sizeof(c->model));
            } else if (e->type == S3_MESSAGE && json_get(o, "message", &m)) {
                c->has_model = 1;
                c->provider[0] = c->model[0] = 0;
                if (json_get(m, "provider", &v) && json_type(v) == J_STR)
                    json_str(v, c->provider, sizeof(c->provider));
                if (json_get(m, "model", &v) && json_type(v) == J_STR)
                    json_str(v, c->model, sizeof(c->model));
            }
        }
    }
    /* buildContextEntries */
    for (i = 0; i < np; i++)
        if (st->e[path[i]].type == S3_COMPACTION)
            comp = i;
    ent = (int *)malloc((size_t)(np + 1) * sizeof(int));
    edit = (int *)malloc((size_t)(np + 1) * sizeof(int));
    if (!ent || !edit)
        goto out;
    if (comp < 0) {
        for (i = 0; i < np; i++)
            ent[ne++] = path[i];
    } else {
        long n;
        const char *t = s3_text(st, path[comp], &n);
        jv o, v;
        char kept[48];
        int found = 0;
        if (!t || json_parse(t, n, &o))
            goto out;
        kept[0] = 0;
        if (json_get(o, "firstKeptEntryId", &v) && json_type(v) == J_STR)
            json_str(v, kept, sizeof(kept));
        ent[ne++] = path[comp];
        for (i = 0; i < comp; i++) {
            s3_ent *e = &st->e[path[i]];
            if (!found && !strcmp(s3_id(st, path[i]), kept))
                found = 1;
            if (found && !(e->type == S3_MESSAGE && e->role == S3R_SYSTEM))
                ent[ne++] = path[i];
        }
        for (i = comp + 1; i < np; i++)
            ent[ne++] = path[i];
    }
    /* the latest context_edit per target among the context entries */
    for (i = 0; i < ne; i++)
        edit[i] = -1;
    for (i = 0; i < ne; i++)
        if (st->e[ent[i]].type == S3_CONTEXT_EDIT && st->e[ent[i]].target >= 0) {
            int k;
            for (k = 0; k < ne; k++)
                if (ent[k] == st->e[ent[i]].target)
                    edit[k] = ent[i];
        }
    for (i = 0; i < ne; i++) {
        int before = c->n;
        /* an older compaction kept inside the range contributes nothing */
        if (!(st->e[ent[i]].type == S3_COMPACTION && i > 0)) {
            if (entry_messages(st, ent[i], c))
                goto out;
            if (edit[i] >= 0 && edit_message(st, edit[i], c, before))
                goto out;
        }
        if (ctx_entry_add(c, ent[i], before, c->n - before))
            goto out;
    }
    rc = 0;
out:
    free(path);
    free(ent);
    free(edit);
    if (rc)
        ctx_free(c);
    return rc;
}

int ctx_entry_count(pi_s3 *st, int i)
{
    long n;
    const char *t;
    jv o, v;
    int type = st->e[i].type;
    if (type == S3_MESSAGE || type == S3_CUSTOM_MESSAGE)
        return 1;
    if (type != S3_BRANCH_SUMMARY && type != S3_COMPACTION)
        return 0;
    t = s3_text(st, i, &n);
    if (!t || json_parse(t, n, &o))
        return 0;
    if (type == S3_BRANCH_SUMMARY) {
        char *z = json_get(o, "summary", &v) && json_type(v) == J_STR ? json_strdup(v, &n) : 0;
        int any = z && *z;
        free(z);
        return any;
    }
    return json_get(o, "systemMessage", &v) && json_type(v) != J_NULL ? 2 : 1;
}

void ctx_json(const pi_ctx *c, jw *out)
{
    int i;
    jw_rawz(out, "{\"messages\":[");
    for (i = 0; i < c->n; i++) {
        if (i)
            jw_raw(out, ",", 1);
        jw_raw(out, c->msg[i], c->msg_n[i]);
    }
    jw_rawz(out, "],\"thinkingLevel\":");
    jw_strz(out, c->thinking);
    jw_rawz(out, ",\"model\":");
    if (!c->has_model)
        jw_rawz(out, "null");
    else {
        int first = 1;
        jw_raw(out, "{", 1);
        if (c->provider[0]) {
            jw_rawz(out, "\"provider\":");
            jw_strz(out, c->provider);
            first = 0;
        }
        if (c->model[0]) {
            if (!first)
                jw_raw(out, ",", 1);
            jw_rawz(out, "\"modelId\":");
            jw_strz(out, c->model);
        }
        jw_raw(out, "}", 1);
    }
    jw_raw(out, "}", 1);
}

/* bashExecutionToText */
static void bash_text(jv m, jw *w)
{
    jv v;
    char *cmd = 0, *outp = 0, *path = 0;
    long n;
    jw t;
    jw_init(&t);
    if (json_get(m, "command", &v) && json_type(v) == J_STR)
        cmd = json_strdup(v, &n);
    if (json_get(m, "output", &v) && json_type(v) == J_STR)
        outp = json_strdup(v, &n);
    jw_rawz(&t, "Ran `");
    jw_rawz(&t, cmd ? cmd : "undefined");
    jw_rawz(&t, "`\n");
    if (outp && *outp) {
        jw_rawz(&t, "```\n");
        jw_rawz(&t, outp);
        jw_rawz(&t, "\n```");
    } else
        jw_rawz(&t, "(no output)");
    if (json_get(m, "cancelled", &v) && json_type(v) == J_TRUE)
        jw_rawz(&t, "\n\n(command cancelled)");
    else if (json_get(m, "exitCode", &v) && json_type(v) == J_NUM && json_long(v, 0) != 0) {
        jw_rawz(&t, "\n\nCommand exited with code ");
        jw_long(&t, json_long(v, 0));
    }
    if (json_get(m, "truncated", &v) && json_type(v) == J_TRUE && json_get(m, "fullOutputPath", &v) &&
        json_type(v) == J_STR && (path = json_strdup(v, &n)) != 0 && *path) {
        jw_rawz(&t, "\n\n[Output truncated. Full output: ");
        jw_rawz(&t, path);
        jw_rawz(&t, "]");
    }
    jw_str(w, t.p ? t.p : "", t.n);
    jw_free(&t);
    free(cmd);
    free(outp);
    free(path);
}

int ctx_msg_to_llm(const char *json, long n, jw *out)
{
    jv m, v;
    char role[32];
    int first;
    if (json_parse(json, n, &m) || json_type(m) != J_OBJ)
        return 0;
    role[0] = 0;
    if (json_get(m, "role", &v) && json_type(v) == J_STR)
        json_str(v, role, sizeof(role));
    if (!strcmp(role, "system") || !strcmp(role, "user") || !strcmp(role, "assistant") || !strcmp(role, "toolResult")) {
        jw_raw(out, json, n);
        return 1;
    }
    if (!strcmp(role, "bashExecution") && json_get(m, "excludeFromContext", &v) && json_type(v) == J_TRUE)
        return 0;
    if (!strcmp(role, "bashExecution") || !strcmp(role, "custom") || !strcmp(role, "branchSummary") ||
        !strcmp(role, "compactionSummary")) {
        jw_rawz(out, "{\"role\":\"user\",\"content\":");
        if (!strcmp(role, "bashExecution")) {
            jw_rawz(out, "[{\"type\":\"text\",\"text\":");
            bash_text(m, out);
            jw_rawz(out, "}]");
        } else if (!strcmp(role, "custom")) {
            if (json_get(m, "content", &v) && json_type(v) == J_STR) {
                jw_rawz(out, "[{\"type\":\"text\",\"text\":");
                jw_raw(out, v.p, v.n);
                jw_rawz(out, "}]");
            } else if (json_get(m, "content", &v))
                jw_raw(out, v.p, v.n);
            else
                jw_rawz(out, "null");
        } else {
            int br = !strcmp(role, "branchSummary");
            char *s = json_get(m, "summary", &v) && json_type(v) == J_STR ? json_strdup(v, &n) : 0;
            jw t;
            jw_init(&t);
            jw_rawz(&t, br ? BRANCH_PREFIX : COMPACTION_PREFIX);
            jw_rawz(&t, s ? s : "undefined");
            jw_rawz(&t, br ? BRANCH_SUFFIX : COMPACTION_SUFFIX);
            jw_rawz(out, "[{\"type\":\"text\",\"text\":");
            jw_str(out, t.p, t.n);
            jw_rawz(out, "}]");
            jw_free(&t);
            free(s);
        }
        first = 0;
        if (json_get(m, "timestamp", &v)) {
            put_key(out, "timestamp", &first);
            jw_raw(out, v.p, v.n);
        }
        jw_raw(out, "}", 1);
        return 1;
    }
    return 0;
}

int ctx_to_llm(const pi_ctx *c, jw *out)
{
    int i, any = 0;
    jw_raw(out, "[", 1);
    for (i = 0; i < c->n; i++) {
        jw one;
        jw_init(&one);
        if (ctx_msg_to_llm(c->msg[i], c->msg_n[i], &one)) {
            if (any)
                jw_raw(out, ",", 1);
            jw_raw(out, one.p, one.n);
            any = 1;
        }
        jw_free(&one);
    }
    jw_raw(out, "]", 1);
    return out->oom ? -1 : 0;
}

/* ---- the system message replay ---- */
typedef struct named {
    char *name;
    char *raw;                  /* a section's JSON string, a tool's JSON object */
} named;

typedef struct nlist {
    named *a;
    int n, cap;
    int oom;
} nlist;

static void nl_free(nlist *l)
{
    int i;
    for (i = 0; i < l->n; i++) {
        free(l->a[i].name);
        free(l->a[i].raw);
    }
    free(l->a);
    memset(l, 0, sizeof(*l));
}

static void nl_del(nlist *l, const char *name)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->a[i].name, name)) {
            free(l->a[i].name);
            free(l->a[i].raw);
            memmove(l->a + i, l->a + i + 1, (size_t)(l->n - i - 1) * sizeof(named));
            l->n--;
            return;
        }
}

/* Map.set: an existing name keeps its place */
static void nl_set(nlist *l, const char *name, const char *raw, long rn)
{
    int i;
    char *r = (char *)malloc((size_t)rn + 1);
    if (!r) {
        l->oom = 1;
        return;
    }
    memcpy(r, raw, (size_t)rn);
    r[rn] = 0;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->a[i].name, name)) {
            free(l->a[i].raw);
            l->a[i].raw = r;
            return;
        }
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        named *g = (named *)realloc(l->a, (size_t)nc * sizeof(*g));
        if (!g) {
            free(r);
            l->oom = 1;
            return;
        }
        l->a = g;
        l->cap = nc;
    }
    l->a[l->n].name = (char *)malloc(strlen(name) + 1);
    if (!l->a[l->n].name) {
        free(r);
        l->oom = 1;
        return;
    }
    strcpy(l->a[l->n].name, name);
    l->a[l->n++].raw = r;
}

/* contentText: a string, or the text blocks joined by a newline; appended raw to out */
static void content_text(jv c, jw *out)
{
    long n;
    if (json_type(c) == J_STR) {
        char *z = json_strdup(c, &n);
        if (z)
            jw_raw(out, z, n);
        free(z);
    } else if (json_type(c) == J_ARR) {
        jit it;
        jv b, t, x;
        int any = 0;
        json_iter(c, &it);
        while (json_next(&it, 0, &b))
            if (json_type(b) == J_OBJ && json_get(b, "type", &t) && json_streq(t, "text")) {
                char *z = json_get(b, "text", &x) && json_type(x) == J_STR ? json_strdup(x, &n) : 0;
                if (any)
                    jw_raw(out, "\n", 1);
                any = 1;
                if (z)
                    jw_raw(out, z, n);
                free(z);
            }
    }
}

int ctx_system(const pi_ctx *c, jw *out)
{
    nlist sec, tools;
    jw content;
    int i, have_ts = 0, any_text = 0, rc;
    jv ts_v;
    memset(&sec, 0, sizeof(sec));
    memset(&tools, 0, sizeof(tools));
    jw_init(&content);
    ts_v.p = 0;
    ts_v.n = 0;
    for (i = 0; i < c->n; i++) {
        jv m, v, k, val;
        jit it;
        char role[16];
        jw part;
        if (json_parse(c->msg[i], c->msg_n[i], &m))
            continue;
        role[0] = 0;
        if (json_get(m, "role", &v) && json_type(v) == J_STR)
            json_str(v, role, sizeof(role));
        if (strcmp(role, "system"))
            continue;
        if (!have_ts && json_get(m, "timestamp", &v)) {
            have_ts = 1;
            ts_v = v;
        }
        jw_init(&part);
        if (json_get(m, "content", &v))
            content_text(v, &part);
        if (part.n > 0) {
            if (any_text)
                jw_rawz(&content, "\n\n");
            jw_raw(&content, part.p, part.n);
            any_text = 1;
        }
        jw_free(&part);
        if (json_get(m, "sections", &v) && json_type(v) == J_OBJ) {
            json_iter(v, &it);
            while (json_next(&it, &k, &val)) {
                char *name = json_strdup(k, 0);
                if (name) {
                    if (json_type(val) == J_NULL)
                        nl_del(&sec, name);
                    else
                        nl_set(&sec, name, val.p, val.n);
                    free(name);
                }
            }
        }
        if (json_get(m, "toolsRemoved", &v) && json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &val)) {
                jv nm;
                char *name = json_get(val, "name", &nm) && json_type(nm) == J_STR ? json_strdup(nm, 0) : 0;
                if (name)
                    nl_del(&tools, name);
                free(name);
            }
        }
        if (json_get(m, "toolsAdded", &v) && json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &val)) {
                jv nm;
                char *name = json_get(val, "name", &nm) && json_type(nm) == J_STR ? json_strdup(nm, 0) : 0;
                if (name)
                    nl_set(&tools, name, val.p, val.n);
                free(name);
            }
        }
    }
    rc = 0;
    if (!have_ts && tools.n == 0)
        goto done;
    jw_rawz(out, "{\"role\":\"system\",\"content\":");
    jw_str(out, content.p ? content.p : "", content.n);
    if (sec.n > 0) {
        jw_rawz(out, ",\"sections\":{");
        for (i = 0; i < sec.n; i++) {
            if (i)
                jw_raw(out, ",", 1);
            jw_strz(out, sec.a[i].name);
            jw_raw(out, ":", 1);
            jw_rawz(out, sec.a[i].raw);
        }
        jw_raw(out, "}", 1);
    }
    if (tools.n > 0) {
        jw_rawz(out, ",\"toolsAdded\":[");
        for (i = 0; i < tools.n; i++) {
            if (i)
                jw_raw(out, ",", 1);
            jw_rawz(out, tools.a[i].raw);
        }
        jw_raw(out, "]", 1);
    }
    jw_rawz(out, ",\"timestamp\":");
    if (have_ts)
        jw_raw(out, ts_v.p, ts_v.n);
    else
        jw_raw(out, "0", 1);
    jw_raw(out, "}", 1);
    rc = 1;
done:
    nl_free(&sec);
    nl_free(&tools);
    jw_free(&content);
    return out->oom ? -1 : rc;
}

int ctx_system_text(const char *json, long n, jw *out)
{
    jv m, v, k, val;
    jit it;
    jw part;
    int any = 0;
    if (json_parse(json, n, &m))
        return -1;
    jw_init(&part);
    if (json_get(m, "content", &v))
        content_text(v, &part);
    if (part.n > 0) {
        jw_raw(out, part.p, part.n);
        any = 1;
    }
    jw_free(&part);
    if (json_get(m, "sections", &v) && json_type(v) == J_OBJ) {
        json_iter(v, &it);
        while (json_next(&it, &k, &val)) {
            char *z;
            long zn;
            if (json_type(val) != J_STR)
                continue;
            z = json_strdup(val, &zn);
            if (z && zn > 0) {
                if (any)
                    jw_rawz(out, "\n\n");
                jw_raw(out, z, zn);
                any = 1;
            }
            free(z);
        }
    }
    return out->oom ? -1 : 0;
}
