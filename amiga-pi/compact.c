/* see compact.h */
#include <stdlib.h>
#include <string.h>
#include "compact.h"
#include "cp_prompts.h"
#include "claude/util.h"

#define IMAGE_CHARS 4800L

/* ---- JavaScript's string length: UTF-16 units of UTF-8 text ---- */
static long u16(const char *s, long n)
{
    long i, k = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xc0) != 0x80)             /* a lead byte (or ASCII) starts a character */
            k += c >= 0xf0 ? 2 : 1;
    }
    return k;
}

static long str_len(jv v)
{
    long n;
    char *z;
    if (json_type(v) != J_STR)
        return 0;
    z = json_strdup(v, &n);
    n = z ? u16(z, n) : 0;
    free(z);
    return n;
}

/* a string member's decoded text (malloc'ed), 0 absent or not a string */
static char *member_str(jv o, const char *key, long *n)
{
    jv v;
    return json_get(o, key, &v) && json_type(v) == J_STR ? json_strdup(v, n) : 0;
}

static void role_of(jv m, char *role, long cap)
{
    jv v;
    role[0] = 0;
    if (json_get(m, "role", &v) && json_type(v) == J_STR)
        json_str(v, role, cap);
}

/* estimateTextAndImageContentChars */
static long text_image_chars(jv c)
{
    long chars = 0;
    jit it;
    jv b, t, x;
    if (json_type(c) == J_STR)
        return str_len(c);
    if (json_type(c) != J_ARR)
        return 0;
    json_iter(c, &it);
    while (json_next(&it, 0, &b)) {
        if (json_type(b) != J_OBJ || !json_get(b, "type", &t))
            continue;
        if (json_streq(t, "text") && json_get(b, "text", &x))
            chars += str_len(x);
        else if (json_streq(t, "image"))
            chars += IMAGE_CHARS;
    }
    return chars;
}

long cp_estimate(const char *msg, long n)
{
    jv m, v, k, val, b, t;
    jit it;
    char role[32];
    long chars = 0;
    if (json_parse(msg, n, &m) || json_type(m) != J_OBJ)
        return 0;
    role_of(m, role, sizeof(role));
    if (!strcmp(role, "system")) {
        if (json_get(m, "content", &v))
            chars = text_image_chars(v);
        if (json_get(m, "sections", &v) && json_type(v) == J_OBJ) {
            json_iter(v, &it);
            while (json_next(&it, &k, &val))
                chars += str_len(val);
        }
        if (json_get(m, "toolsAdded", &v))
            chars += u16(v.p, v.n);
    } else if (!strcmp(role, "user") || !strcmp(role, "custom") || !strcmp(role, "toolResult")) {
        if (json_get(m, "content", &v))
            chars = text_image_chars(v);
    } else if (!strcmp(role, "assistant")) {
        if (json_get(m, "content", &v) && json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &b)) {
                if (json_type(b) != J_OBJ || !json_get(b, "type", &t))
                    continue;
                if (json_streq(t, "text") && json_get(b, "text", &val))
                    chars += str_len(val);
                else if (json_streq(t, "thinking") && json_get(b, "thinking", &val))
                    chars += str_len(val);
                else if (json_streq(t, "toolCall")) {
                    if (json_get(b, "name", &val))
                        chars += str_len(val);
                    if (json_get(b, "arguments", &val))
                        chars += u16(val.p, val.n);
                }
            }
        }
    } else if (!strcmp(role, "bashExecution")) {
        if (json_get(m, "command", &v))
            chars += str_len(v);
        if (json_get(m, "output", &v))
            chars += str_len(v);
    } else if (!strcmp(role, "branchSummary") || !strcmp(role, "compactionSummary")) {
        if (json_get(m, "summary", &v))
            chars = str_len(v);
    } else
        return 0;
    return (chars + 3) / 4;
}

/* ---- the context's size ---- */

long cp_usage_total(const char *msg, long n)
{
    jv m, u, x;
    long total;
    if (json_parse(msg, n, &m) || json_type(m) != J_OBJ || !json_get(m, "usage", &u) || json_type(u) != J_OBJ)
        return 0;
    total = json_get(u, "totalTokens", &x) ? json_long(x, 0) : 0;
    if (!total) {
        total = (json_get(u, "input", &x) ? json_long(x, 0) : 0) + (json_get(u, "output", &x) ? json_long(x, 0) : 0) +
                (json_get(u, "cacheRead", &x) ? json_long(x, 0) : 0) +
                (json_get(u, "cacheWrite", &x) ? json_long(x, 0) : 0);
    }
    return total > 0 ? total : 0;
}

/* the tokens an answer's usage reports, 0 when it is not usable (aborted, error, all zero) */
static long usage_tokens(const char *msg, long n)
{
    jv m, v;
    char role[32];
    if (json_parse(msg, n, &m) || json_type(m) != J_OBJ)
        return 0;
    role_of(m, role, sizeof(role));
    if (strcmp(role, "assistant"))
        return 0;
    if (json_get(m, "stopReason", &v) && (json_streq(v, "aborted") || json_streq(v, "error")))
        return 0;
    return cp_usage_total(msg, n);
}

long cp_estimate_context(char *const *msg, const long *msg_n, int n, int *last_usage)
{
    int i, last = -1;
    long t = 0;
    for (i = n - 1; i >= 0 && last < 0; i--)
        if (usage_tokens(msg[i], msg_n[i]) > 0)
            last = i;
    if (last >= 0)
        t = usage_tokens(msg[last], msg_n[last]);
    for (i = last + 1; i < n; i++)
        t += cp_estimate(msg[i], msg_n[i]);
    if (last_usage)
        *last_usage = last;
    return t;
}

long cp_context_tokens(pi_s3 *st, int leaf, const pi_ctx *c)
{
    int i, last = -1, e, *path = 0, np, usage_entry = -1, inval = -1, k;
    long tokens = 0, sys = 0, t = cp_estimate_context(c->msg, c->msg_n, c->n, &last);
    if (last >= 0) {
        np = s3_path(st, leaf, &path);
        for (e = 0; e < c->ne; e++)
            if (last >= c->efirst[e] && last < c->efirst[e] + c->ecnt[e]) {
                usage_entry = c->esrc[e];
                break;
            }
        if (np > 0) {
            int ue = -1;
            for (k = 0; k < np; k++) {
                if (path[k] == usage_entry)
                    ue = k;
                if (st->e[path[k]].type == S3_CONTEXT_EDIT || st->e[path[k]].type == S3_COMPACTION)
                    inval = k;
            }
            free(path);
            if (ue > inval)
                return t;
        } else
            free(path);
    }
    /* not trusted after an edit or a compaction: the replayed system message and the rest, estimated */
    {
        jw sm;
        jw_init(&sm);
        if (ctx_system(c, &sm) == 1)
            sys = cp_estimate(sm.p, sm.n);
        jw_free(&sm);
    }
    tokens = sys;
    for (i = 0; i < c->n; i++) {
        jv m;
        char role[32];
        if (!json_parse(c->msg[i], c->msg_n[i], &m)) {
            role_of(m, role, sizeof(role));
            if (strcmp(role, "system"))
                tokens += cp_estimate(c->msg[i], c->msg_n[i]);
        }
    }
    return tokens;
}

long cp_context_usage(pi_s3 *st, int leaf, const pi_ctx *c)
{
    int *path = 0, np = s3_path(st, leaf, &path), k, comp = -1, e, j, seen = 0;
    for (k = 0; k < np; k++)
        if (st->e[path[k]].type == S3_COMPACTION)
            comp = k;
    /* the branch entries after the compaction: does one project an answer with usable usage */
    for (k = comp + 1; comp >= 0 && k < np && !seen; k++)
        for (e = 0; e < c->ne && !seen; e++)
            if (c->esrc[e] == path[k])
                for (j = c->efirst[e]; j < c->efirst[e] + c->ecnt[e] && !seen; j++)
                    seen = usage_tokens(c->msg[j], c->msg_n[j]) > 0;
    free(path);
    if (comp >= 0 && !seen)
        return -1;
    return cp_context_tokens(st, leaf, c);
}

int cp_should(long tokens, long window, long reserve)
{
    return tokens > window - reserve;
}

/* ---- cut points ---- */

static int msg_role_is(const pi_ctx *c, int i, const char *const *roles)
{
    jv m;
    char role[32];
    int k;
    if (json_parse(c->msg[i], c->msg_n[i], &m))
        return 0;
    role_of(m, role, sizeof(role));
    for (k = 0; roles[k]; k++)
        if (!strcmp(role, roles[k]))
            return 1;
    return 0;
}

static const char *const cut_roles[] = { "user", "assistant", "bashExecution", "custom", "branchSummary",
                                         "compactionSummary", 0 };
static const char *const turn_roles[] = { "user", "bashExecution", "custom", "branchSummary", "compactionSummary", 0 };

static int any_msg(const pi_ctx *c, int e, const char *const *roles)
{
    int k;
    for (k = 0; k < c->ecnt[e]; k++)
        if (msg_role_is(c, c->efirst[e] + k, roles))
            return 1;
    return 0;
}

static int is_turn_start(pi_s3 *st, const pi_ctx *c, int e)
{
    return st->e[c->esrc[e]].type != S3_COMPACTION && any_msg(c, e, turn_roles);
}

static int turn_start_index(pi_s3 *st, const pi_ctx *c, int from, int start)
{
    int i;
    for (i = from; i >= start; i--)
        if (is_turn_start(st, c, i))
            return i;
    return -1;
}

static int intrinsically_visible(pi_s3 *st, int src)
{
    return st->e[src].type != S3_CONTEXT_EDIT && ctx_entry_count(st, src) > 0;
}

static int is_omitted(pi_s3 *st, const pi_ctx *c, int e)
{
    return intrinsically_visible(st, c->esrc[e]) && c->ecnt[e] == 0;
}

/* findProjectedCutPoint: first kept entry, turn start (-1 none), split */
static void find_cut(pi_s3 *st, const pi_ctx *c, int start, int end, long keep, int *first, int *turn, int *split)
{
    int i, ncut = 0, *cuts, cut, exceeded = 0, k;
    long acc = 0;
    cuts = (int *)malloc((size_t)(end > start ? end - start : 1) * sizeof(int));
    *first = start;
    *turn = -1;
    *split = 0;
    if (!cuts)
        return;
    for (i = start; i < end; i++)
        if (st->e[c->esrc[i]].type != S3_COMPACTION && any_msg(c, i, cut_roles))
            cuts[ncut++] = i;
    if (!ncut) {
        free(cuts);
        return;
    }
    cut = cuts[0];
    for (i = end - 1; i >= start; i--) {
        long t = 0;
        for (k = 0; k < c->ecnt[i]; k++)
            t += cp_estimate(c->msg[c->efirst[i] + k], c->msg_n[c->efirst[i] + k]);
        if (t == 0)
            continue;
        acc += t;
        if (acc >= keep) {
            exceeded = 1;
            cut = cuts[ncut - 1];
            for (k = 0; k < ncut; k++)
                if (cuts[k] >= i) {
                    cut = cuts[k];
                    break;
                }
            break;
        }
    }
    /* a recovery attempt's omitted answer and its edits trail the last visible input: the cut moves past them */
    {
        int has_ext = 0, has_omitted_asst = 0, every = 1;
        for (i = cut + 1; i < end; i++) {
            int src = c->esrc[i];
            if (st->e[src].type == S3_CONTEXT_EDIT) {
                long n;
                const char *t = s3_text(st, src, &n);
                jv o, v;
                if (t && !json_parse(t, n, &o) && json_get(o, "replacement", &v) && json_type(v) != J_NULL) {
                    /* replacement not null: external unless its target is an omitted entry of the suffix */
                    int tg = st->e[src].target, ext = 1, j;
                    for (j = cut + 1; j < end; j++)
                        if (c->esrc[j] == tg && is_omitted(st, c, j))
                            ext = 0;
                    has_ext |= ext;
                }
            }
            if (st->e[src].type == S3_MESSAGE && st->e[src].role == S3R_ASSISTANT && is_omitted(st, c, i))
                has_omitted_asst = 1;
            if (st->e[src].type == S3_COMPACTION || (intrinsically_visible(st, src) && !is_omitted(st, c, i)))
                every = 0;
        }
        if (exceeded && !has_ext && has_omitted_asst && every)
            cut++;
    }
    while (cut > start) {
        int pe = cut - 1;
        if (st->e[c->esrc[pe]].type == S3_COMPACTION || c->ecnt[pe] > 0)
            break;
        cut--;
    }
    *first = cut;
    if (!is_turn_start(st, c, cut)) {
        *turn = turn_start_index(st, c, cut, start);
        *split = *turn != -1;
    }
    free(cuts);
}

/* ---- sets of paths ---- */

static void strs_add(cp_strs *l, const char *s, long n)
{
    int i;
    char *z;
    for (i = 0; i < l->n; i++)
        if ((long)strlen(l->s[i]) == n && !memcmp(l->s[i], s, (size_t)n))
            return;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        char **g = (char **)realloc(l->s, (size_t)nc * sizeof(*g));
        if (!g)
            return;
        l->s = g;
        l->cap = nc;
    }
    z = (char *)malloc((size_t)n + 1);
    if (!z)
        return;
    memcpy(z, s, (size_t)n);
    z[n] = 0;
    l->s[l->n++] = z;
}

static void strs_free(cp_strs *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->s[i]);
    free(l->s);
    memset(l, 0, sizeof(*l));
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* extractFileOpsFromMessage */
static void file_ops(const char *msg, long n, pi_prep *p)
{
    jv m, c, b, t, nm, a, path;
    jit it;
    char role[32], name[16];
    if (json_parse(msg, n, &m))
        return;
    role_of(m, role, sizeof(role));
    if (strcmp(role, "assistant") || !json_get(m, "content", &c) || json_type(c) != J_ARR)
        return;
    json_iter(c, &it);
    while (json_next(&it, 0, &b)) {
        char *pz;
        long pn;
        if (json_type(b) != J_OBJ || !json_get(b, "type", &t) || !json_streq(t, "toolCall") ||
            !json_get(b, "arguments", &a) || !json_get(b, "name", &nm) || json_type(nm) != J_STR)
            continue;
        json_str(nm, name, sizeof(name));
        if (json_type(a) != J_OBJ || !json_get(a, "path", &path) || json_type(path) != J_STR)
            continue;
        pz = json_strdup(path, &pn);
        if (pz && pn > 0) {
            if (!strcmp(name, "read"))
                strs_add(&p->read, pz, pn);
            else if (!strcmp(name, "write"))
                strs_add(&p->written, pz, pn);
            else if (!strcmp(name, "edit"))
                strs_add(&p->edited, pz, pn);
        }
        free(pz);
    }
}

void cp_prep_free(pi_prep *p)
{
    ctx_free(&p->hist);
    ctx_free(&p->prefix);
    free(p->prev_summary);
    strs_free(&p->read);
    strs_free(&p->written);
    strs_free(&p->edited);
    memset(p, 0, sizeof(*p));
}

/* the messages of entries [a, b) except those of a compaction and system messages (getMessagesFromProjectedEntryForCompaction) */
static int collect(pi_s3 *st, const pi_ctx *c, int a, int b, pi_ctx *out)
{
    int e, k;
    for (e = a; e < b; e++) {
        if (st->e[c->esrc[e]].type == S3_COMPACTION)
            continue;
        for (k = 0; k < c->ecnt[e]; k++) {
            int mi = c->efirst[e] + k;
            jv m;
            char role[32];
            if (json_parse(c->msg[mi], c->msg_n[mi], &m))
                continue;
            role_of(m, role, sizeof(role));
            if (!strcmp(role, "system"))
                continue;
            if (ctx_push(out, c->msg[mi], c->msg_n[mi]))
                return -1;
        }
    }
    return 0;
}

int cp_prepare(pi_s3 *st, int leaf, long keep_recent, pi_prep *p)
{
    pi_ctx c;
    int prev = -1, start = 0, e, cut, turn, split, hist_end, i, rc = -1;
    memset(p, 0, sizeof(*p));
    ctx_init(&p->hist);
    ctx_init(&p->prefix);
    ctx_init(&c);
    if (leaf < 0 || st->e[leaf].type == S3_COMPACTION)
        return 0;
    if (ctx_build(st, leaf, &c))
        goto out;
    for (e = 0; e < c.ne; e++)
        if (st->e[c.esrc[e]].type == S3_COMPACTION && c.ecnt[e] > 0) {
            prev = e;
            break;
        }
    if (prev >= 0) {
        long n;
        const char *t = s3_text(st, c.esrc[prev], &n);
        jv o;
        if (t && !json_parse(t, n, &o))
            p->prev_summary = member_str(o, "summary", &n);
        start = prev + 1;
    }
    p->tokens_before = cp_context_tokens(st, leaf, &c);
    find_cut(st, &c, start, c.ne, keep_recent, &cut, &turn, &split);
    if (cut < 0 || cut >= c.ne) {
        rc = 0;
        goto out;
    }
    p->first_kept = c.esrc[cut];
    p->split = split;
    hist_end = split ? turn : cut;
    if (collect(st, &c, start, hist_end, &p->hist) || (split && collect(st, &c, turn, cut, &p->prefix)))
        goto out;
    if (p->hist.n == 0 && p->prefix.n == 0) {
        rc = 0;
        goto out;
    }
    /* the earlier compaction's file lists (only when pi wrote it, not an extension) */
    if (prev >= 0) {
        long n;
        const char *t = s3_text(st, c.esrc[prev], &n);
        jv o, d, v, x;
        jit it;
        if (t && !json_parse(t, n, &o)) {
            int hook = json_get(o, "fromHook", &v) && json_type(v) == J_TRUE;
            if (!hook && json_get(o, "details", &d) && json_type(d) == J_OBJ) {
                if (json_get(d, "readFiles", &v) && json_type(v) == J_ARR) {
                    json_iter(v, &it);
                    while (json_next(&it, 0, &x)) {
                        char *z = json_type(x) == J_STR ? json_strdup(x, &n) : 0;
                        if (z)
                            strs_add(&p->read, z, n);
                        free(z);
                    }
                }
                if (json_get(d, "modifiedFiles", &v) && json_type(v) == J_ARR) {
                    json_iter(v, &it);
                    while (json_next(&it, 0, &x)) {
                        char *z = json_type(x) == J_STR ? json_strdup(x, &n) : 0;
                        if (z)
                            strs_add(&p->edited, z, n);
                        free(z);
                    }
                }
            }
        }
    }
    for (i = 0; i < p->hist.n; i++)
        file_ops(p->hist.msg[i], p->hist.msg_n[i], p);
    for (i = 0; i < p->prefix.n; i++)
        file_ops(p->prefix.msg[i], p->prefix.msg_n[i], p);
    rc = 1;
out:
    ctx_free(&c);
    if (rc != 1)
        cp_prep_free(p);
    return rc;
}

/* ---- serialisation for the summary prompt ---- */

#define TOOL_RESULT_MAX 2000L

/* contentText over a pi-ai content value: a string, or the text blocks joined by sep */
static void content_text(jv c, const char *sep, jw *out)
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
                    jw_rawz(out, sep);
                any = 1;
                if (z)
                    jw_raw(out, z, n);
                free(z);
            }
    }
}

/* the first max UTF-16 units of s (n bytes) as a byte count */
static long u16_prefix(const char *s, long n, long max)
{
    long i = 0, k = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        long len = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        long units = c >= 0xf0 ? 2 : 1;
        if (k + units > max)
            break;
        k += units;
        i += len;
    }
    return i < n ? i : n;
}

void cp_serialize(const pi_ctx *msgs, jw *out)
{
    int i, any = 0;
    for (i = 0; i < msgs->n; i++) {
        jw one, part;
        jv m, c, b, t, x, args, k, val;
        jit it, it2;
        char role[32];
        jw_init(&one);
        if (!ctx_msg_to_llm(msgs->msg[i], msgs->msg_n[i], &one) || json_parse(one.p, one.n, &m)) {
            jw_free(&one);
            continue;
        }
        role_of(m, role, sizeof(role));
        jw_init(&part);
        if (!strcmp(role, "user")) {
            if (json_get(m, "content", &c))
                content_text(c, "", &part);
            if (part.n) {
                if (any++)
                    jw_rawz(out, "\n\n");
                jw_rawz(out, "[User]: ");
                jw_raw(out, part.p, part.n);
            }
        } else if (!strcmp(role, "assistant")) {
            jw thinking, calls;
            int nthink = 0, ncalls = 0, has_text = 0;
            jw_init(&thinking);
            jw_init(&calls);
            if (json_get(m, "content", &c) && json_type(c) == J_ARR) {
                json_iter(c, &it);
                while (json_next(&it, 0, &b)) {
                    if (json_type(b) != J_OBJ || !json_get(b, "type", &t))
                        continue;
                    if (json_streq(t, "thinking")) {
                        long zn;
                        char *z = json_get(b, "thinking", &x) && json_type(x) == J_STR ? json_strdup(x, &zn) : 0;
                        if (nthink++)
                            jw_rawz(&thinking, "\n");
                        if (z)
                            jw_raw(&thinking, z, zn);
                        free(z);
                    } else if (json_streq(t, "toolCall")) {
                        long zn;
                        char *z = json_get(b, "name", &x) && json_type(x) == J_STR ? json_strdup(x, &zn) : 0;
                        if (ncalls++)
                            jw_rawz(&calls, "; ");
                        if (z)
                            jw_raw(&calls, z, zn);
                        free(z);
                        jw_rawz(&calls, "(");
                        if (json_get(b, "arguments", &args) && json_type(args) == J_OBJ) {
                            int first = 1;
                            json_iter(args, &it2);
                            while (json_next(&it2, &k, &val)) {
                                char *kz = json_strdup(k, &zn);
                                if (!first)
                                    jw_rawz(&calls, ", ");
                                first = 0;
                                if (kz)
                                    jw_raw(&calls, kz, zn);
                                free(kz);
                                jw_rawz(&calls, "=");
                                jw_raw(&calls, val.p, val.n);
                            }
                        }
                        jw_rawz(&calls, ")");
                    } else if (json_streq(t, "text"))
                        has_text = 1;
                }
                if (has_text)
                    content_text(c, "\n", &part);
            }
            if (nthink) {
                if (any++)
                    jw_rawz(out, "\n\n");
                jw_rawz(out, "[Assistant thinking]: ");
                jw_raw(out, thinking.p, thinking.n);
            }
            if (has_text) {
                if (any++)
                    jw_rawz(out, "\n\n");
                jw_rawz(out, "[Assistant]: ");
                jw_raw(out, part.p, part.n);
            }
            if (ncalls) {
                if (any++)
                    jw_rawz(out, "\n\n");
                jw_rawz(out, "[Assistant tool calls]: ");
                jw_raw(out, calls.p, calls.n);
            }
            jw_free(&thinking);
            jw_free(&calls);
        } else if (!strcmp(role, "toolResult")) {
            if (json_get(m, "content", &c))
                content_text(c, "", &part);
            if (part.n) {
                long units = u16(part.p, part.n);
                if (any++)
                    jw_rawz(out, "\n\n");
                jw_rawz(out, "[Tool result]: ");
                if (units <= TOOL_RESULT_MAX)
                    jw_raw(out, part.p, part.n);
                else {
                    long cut = u16_prefix(part.p, part.n, TOOL_RESULT_MAX);
                    jw_raw(out, part.p, cut);
                    jw_rawz(out, "\n\n[... ");
                    jw_long(out, units - TOOL_RESULT_MAX);
                    jw_rawz(out, " more characters truncated]");
                }
            }
        }
        jw_free(&part);
        jw_free(&one);
    }
}

/* ---- the summary ---- */

static void cat_lines(jw *w, const char *const *lines)
{
    for (; *lines; lines++)
        jw_rawz(w, *lines);
}

static void add_usage(llm_usage *a, const llm_usage *b)
{
    int i;
    a->input += b->input;
    a->output += b->output;
    a->cache_read += b->cache_read;
    a->cache_write += b->cache_write;
    if (a->has_1h || b->has_1h) {   /* pi's combineUsage: the sum has cacheWrite1h when either has it, in its place */
        a->cache_write_1h += b->cache_write_1h;
        a->has_1h = a->combined = 1;
    }
    for (i = 0; i < 4; i++)
        a->cost[i] += b->cost[i];
}

/* one summarization call: its text, or the failure in pi's words */
static int summarize(cp_complete fn, void *u, const char *label, const jw *prompt, long max_tokens, jw *text,
                     llm_usage *usage, char *err, long errcap)
{
    cp_reply r;
    int rc;
    memset(&r, 0, sizeof(r));
    jw_init(&r.text);
    rc = fn(u, prompt->p, max_tokens, &r);
    if (rc) {
        cl_copy(err, "out of memory", errcap);
    } else if (r.stop == LS_ABORTED) {
        cl_copy(err, label, errcap);
        cl_cat(err, " aborted", errcap);
        rc = 1;
    } else if (r.stop == LS_ERROR) {
        cl_copy(err, label, errcap);
        cl_cat(err, " failed: ", errcap);
        cl_cat(err, r.error[0] ? r.error : "Unknown error", errcap);
        rc = -1;
    } else if (r.stop == LS_LENGTH) {
        cl_copy(err, label, errcap);
        cl_cat(err, " failed: generation hit the token cap and the summary is incomplete", errcap);
        rc = -1;
    } else if (r.tool_call) {
        cl_copy(err, label, errcap);
        cl_cat(err, " attempted to call a tool", errcap);
        rc = -1;
    } else {
        jw_raw(text, r.text.p ? r.text.p : "", r.text.n);
        *usage = r.usage;
    }
    jw_free(&r.text);
    return rc;
}

/* formatFileOperations: the <read-files> and <modified-files> blocks */
static void file_lists_text(jw *summary, const cp_strs *ro, const cp_strs *mod)
{
    int i, sec = 0;
    if (!ro->n && !mod->n)
        return;
    jw_rawz(summary, "\n\n");
    if (ro->n) {
        jw_rawz(summary, "<read-files>\n");
        for (i = 0; i < ro->n; i++) {
            if (i)
                jw_rawz(summary, "\n");
            jw_rawz(summary, ro->s[i]);
        }
        jw_rawz(summary, "\n</read-files>");
        sec = 1;
    }
    if (mod->n) {
        if (sec)
            jw_rawz(summary, "\n\n");
        jw_rawz(summary, "<modified-files>\n");
        for (i = 0; i < mod->n; i++) {
            if (i)
                jw_rawz(summary, "\n");
            jw_rawz(summary, mod->s[i]);
        }
        jw_rawz(summary, "\n</modified-files>");
    }
}

static void file_lists_json(jw *d, const cp_strs *ro, const cp_strs *mod)
{
    int i;
    jw_rawz(d, "{\"readFiles\":[");
    for (i = 0; i < ro->n; i++) {
        if (i)
            jw_rawz(d, ",");
        jw_strz(d, ro->s[i]);
    }
    jw_rawz(d, "],\"modifiedFiles\":[");
    for (i = 0; i < mod->n; i++) {
        if (i)
            jw_rawz(d, ",");
        jw_strz(d, mod->s[i]);
    }
    jw_rawz(d, "]}");
}

static long min_tokens(long a, long model_max)
{
    return model_max > 0 && model_max < a ? model_max : a;
}

int cp_compact(const pi_prep *p, long reserve, long model_max_tokens, const char *instructions, cp_complete fn,
               void *u, cp_result *out, char *err, long errcap)
{
    jw summary, history, prefix_text;
    llm_usage usage, hu, pu;
    int rc = 0, i;
    memset(out, 0, sizeof(*out));
    jw_init(&out->summary);
    jw_init(&out->details);
    jw_init(&summary);
    jw_init(&history);
    jw_init(&prefix_text);
    memset(&usage, 0, sizeof(usage));
    memset(&hu, 0, sizeof(hu));
    memset(&pu, 0, sizeof(pu));
    err[0] = 0;
    if (p->split && p->prefix.n > 0) {
        int have_hist = 0;
        if (p->hist.n > 0) {
            jw conv, prompt;
            jw_init(&conv);
            jw_init(&prompt);
            cp_serialize(&p->hist, &conv);
            jw_rawz(&prompt, "<conversation>\n");
            jw_raw(&prompt, conv.p ? conv.p : "", conv.n);
            jw_rawz(&prompt, "\n</conversation>\n\n");
            if (p->prev_summary) {
                jw_rawz(&prompt, "<previous-summary>\n");
                jw_rawz(&prompt, p->prev_summary);
                jw_rawz(&prompt, "\n</previous-summary>\n\n");
                jw_rawz(&prompt, "The messages above are NEW conversation messages to incorporate into the existing "
                                 "summary provided in <previous-summary> tags.\n\n");
                cat_lines(&prompt, cp_update_summarization_instructions);
            } else
                cat_lines(&prompt, cp_summarization_prompt);
            if (instructions && *instructions) {
                jw_rawz(&prompt, "\n\nAdditional focus: ");
                jw_rawz(&prompt, instructions);
            }
            rc = summarize(fn, u, "Summarization", &prompt, min_tokens(reserve * 8 / 10, model_max_tokens), &history, &hu,
                           err, errcap);
            have_hist = !rc;
            jw_free(&conv);
            jw_free(&prompt);
        }
        if (!rc) {
            jw conv, prompt;
            jw_init(&conv);
            jw_init(&prompt);
            cp_serialize(&p->prefix, &conv);
            jw_rawz(&prompt, "# Conversation\n");
            jw_raw(&prompt, conv.p ? conv.p : "", conv.n);
            jw_rawz(&prompt, "\n\n# Instructions\n");
            cat_lines(&prompt, cp_turn_prefix_summarization_prompt);
            rc = summarize(fn, u, "Turn prefix summarization", &prompt, min_tokens(reserve / 2, model_max_tokens),
                           &prefix_text, &pu, err, errcap);
            jw_free(&conv);
            jw_free(&prompt);
        }
        if (!rc) {
            if (have_hist)
                jw_raw(&summary, history.p ? history.p : "", history.n);
            else
                jw_rawz(&summary, p->prev_summary ? p->prev_summary : "No prior history.");
            jw_rawz(&summary, "\n\n---\n\n**Turn Context (split turn):**\n\n");
            jw_raw(&summary, prefix_text.p ? prefix_text.p : "", prefix_text.n);
            usage = pu;
            if (have_hist)
                add_usage(&usage, &hu);
        }
    } else {
        jw conv, prompt;
        jw_init(&conv);
        jw_init(&prompt);
        cp_serialize(&p->hist, &conv);
        jw_rawz(&prompt, "<conversation>\n");
        jw_raw(&prompt, conv.p ? conv.p : "", conv.n);
        jw_rawz(&prompt, "\n</conversation>\n\n");
        if (p->prev_summary) {
            jw_rawz(&prompt, "<previous-summary>\n");
            jw_rawz(&prompt, p->prev_summary);
            jw_rawz(&prompt, "\n</previous-summary>\n\n");
            jw_rawz(&prompt, "The messages above are NEW conversation messages to incorporate into the existing "
                             "summary provided in <previous-summary> tags.\n\n");
            cat_lines(&prompt, cp_update_summarization_instructions);
        } else
            cat_lines(&prompt, cp_summarization_prompt);
        if (instructions && *instructions) {
            jw_rawz(&prompt, "\n\nAdditional focus: ");
            jw_rawz(&prompt, instructions);
        }
        rc = summarize(fn, u, "Summarization", &prompt, min_tokens(reserve * 8 / 10, model_max_tokens), &summary, &usage,
                       err, errcap);
        jw_free(&conv);
        jw_free(&prompt);
    }
    if (!rc) {
        /* the file lists: read-only files and modified ones, each sorted */
        cp_strs mod, ro;
        memset(&mod, 0, sizeof(mod));
        memset(&ro, 0, sizeof(ro));
        for (i = 0; i < p->edited.n; i++)
            strs_add(&mod, p->edited.s[i], (long)strlen(p->edited.s[i]));
        for (i = 0; i < p->written.n; i++)
            strs_add(&mod, p->written.s[i], (long)strlen(p->written.s[i]));
        for (i = 0; i < p->read.n; i++) {
            int j, in_mod = 0;
            for (j = 0; j < mod.n; j++)
                in_mod |= !strcmp(mod.s[j], p->read.s[i]);
            if (!in_mod)
                strs_add(&ro, p->read.s[i], (long)strlen(p->read.s[i]));
        }
        if (ro.n > 1)
            qsort(ro.s, (size_t)ro.n, sizeof(char *), cmp_str);
        if (mod.n > 1)
            qsort(mod.s, (size_t)mod.n, sizeof(char *), cmp_str);
        file_lists_text(&summary, &ro, &mod);
        file_lists_json(&out->details, &ro, &mod);
        strs_free(&mod);
        strs_free(&ro);
        jw_raw(&out->summary, summary.p ? summary.p : "", summary.n);
        out->usage = usage;
        out->tokens_before = p->tokens_before;
        out->first_kept = p->first_kept;
    }
    jw_free(&summary);
    jw_free(&history);
    jw_free(&prefix_text);
    if (rc)
        cp_result_free(out);
    return rc;
}

void cp_result_free(cp_result *r)
{
    jw_free(&r->summary);
    jw_free(&r->details);
}

/* ---- branch summaries ---- */

int cp_collect(pi_s3 *st, int old_leaf, int target, int **out, int *ancestor)
{
    int *oldp = 0, *tp = 0, no, nt, i, j, cur, n = 0;
    int *res;
    *out = 0;
    *ancestor = -1;
    if (old_leaf < 0)
        return 0;
    no = s3_path(st, old_leaf, &oldp);
    nt = s3_path(st, target, &tp);
    if (no < 0 || nt < 0) {
        free(oldp);
        free(tp);
        return -1;
    }
    for (i = nt - 1; i >= 0 && *ancestor < 0; i--)
        for (j = 0; j < no; j++)
            if (oldp[j] == tp[i]) {
                *ancestor = tp[i];
                break;
            }
    res = (int *)malloc((size_t)(no + 1) * sizeof(int));
    if (!res) {
        free(oldp);
        free(tp);
        return -1;
    }
    for (cur = old_leaf; cur >= 0 && cur != *ancestor; cur = st->e[cur].parent)
        res[n++] = cur;
    for (i = 0; i < n / 2; i++) {
        int t = res[i];
        res[i] = res[n - 1 - i];
        res[n - 1 - i] = t;
    }
    free(oldp);
    free(tp);
    *out = res;
    return n;
}

void cp_bprep_free(cp_bprep *p)
{
    ctx_free(&p->msgs);
    strs_free(&p->read);
    strs_free(&p->written);
    strs_free(&p->edited);
    memset(p, 0, sizeof(*p));
}

static void details_files(jv d, cp_strs *read, cp_strs *edited)
{
    jv v, x;
    jit it;
    long n;
    if (json_get(d, "readFiles", &v) && json_type(v) == J_ARR) {
        json_iter(v, &it);
        while (json_next(&it, 0, &x)) {
            char *z = json_type(x) == J_STR ? json_strdup(x, &n) : 0;
            if (z)
                strs_add(read, z, n);
            free(z);
        }
    }
    if (json_get(d, "modifiedFiles", &v) && json_type(v) == J_ARR) {
        json_iter(v, &it);
        while (json_next(&it, 0, &x)) {
            char *z = json_type(x) == J_STR ? json_strdup(x, &n) : 0;
            if (z)
                strs_add(edited, z, n);
            free(z);
        }
    }
}

int cp_branch_prepare(pi_s3 *st, const int *ent, int n, long budget, cp_bprep *p)
{
    int i, rc = -1, k;
    pi_ctx rev, tmp;
    pi_prep fo;
    memset(p, 0, sizeof(*p));
    ctx_init(&p->msgs);
    ctx_init(&rev);
    memset(&fo, 0, sizeof(fo));
    /* first pass: the file lists of the branch summaries already in it (pi's own, not an extension's) */
    for (i = 0; i < n; i++)
        if (st->e[ent[i]].type == S3_BRANCH_SUMMARY) {
            long tn;
            const char *t = s3_text(st, ent[i], &tn);
            jv o, v, d;
            if (t && !json_parse(t, tn, &o) && !(json_get(o, "fromHook", &v) && json_type(v) == J_TRUE) &&
                json_get(o, "details", &d) && json_type(d) == J_OBJ)
                details_files(d, &p->read, &p->edited);
        }
    /* second pass: newest to oldest until the budget is spent */
    for (i = n - 1; i >= 0; i--) {
        long tokens;
        ctx_init(&tmp);
        if (ctx_branch_message(st, ent[i], &tmp)) {
            ctx_free(&tmp);
            goto out;
        }
        if (tmp.n == 0) {
            ctx_free(&tmp);
            continue;
        }
        fo.read = p->read;
        fo.written = p->written;
        fo.edited = p->edited;
        file_ops(tmp.msg[0], tmp.msg_n[0], &fo);
        p->read = fo.read;
        p->written = fo.written;
        p->edited = fo.edited;
        tokens = cp_estimate(tmp.msg[0], tmp.msg_n[0]);
        if (budget > 0 && p->total + tokens > budget) {
            int t = st->e[ent[i]].type;
            if ((t == S3_COMPACTION || t == S3_BRANCH_SUMMARY) && p->total * 10 < budget * 9) {
                if (ctx_push(&rev, tmp.msg[0], tmp.msg_n[0])) {
                    ctx_free(&tmp);
                    goto out;
                }
                p->total += tokens;
            }
            ctx_free(&tmp);
            break;
        }
        if (ctx_push(&rev, tmp.msg[0], tmp.msg_n[0])) {
            ctx_free(&tmp);
            goto out;
        }
        p->total += tokens;
        ctx_free(&tmp);
    }
    for (k = rev.n - 1; k >= 0; k--)
        if (ctx_push(&p->msgs, rev.msg[k], rev.msg_n[k]))
            goto out;
    rc = 0;
out:
    ctx_free(&rev);
    if (rc)
        cp_bprep_free(p);
    return rc;
}

void cp_branch_free(cp_branch *b)
{
    jw_free(&b->summary);
    jw_free(&b->details);
}

int cp_branch_summary(pi_s3 *st, const int *ent, int n, long window, long reserve, long model_max_tokens,
                      const char *instr, int replace, cp_complete fn, void *u, cp_branch *out, char *err, long errcap)
{
    cp_bprep p;
    jw conv, prompt, text, summary;
    llm_usage usage;
    int rc, i;
    cp_strs mod, ro;
    memset(out, 0, sizeof(*out));
    jw_init(&out->summary);
    jw_init(&out->details);
    err[0] = 0;
    if (cp_branch_prepare(st, ent, n, (window > 0 ? window : 128000L) - reserve, &p)) {
        cl_copy(err, "out of memory", errcap);
        return -1;
    }
    if (p.msgs.n == 0) {
        jw_rawz(&out->summary, "No content to summarize");
        jw_rawz(&out->details, "{\"readFiles\":[],\"modifiedFiles\":[]}");
        cp_bprep_free(&p);
        return 0;
    }
    jw_init(&conv);
    jw_init(&prompt);
    jw_init(&text);
    jw_init(&summary);
    memset(&usage, 0, sizeof(usage));
    cp_serialize(&p.msgs, &conv);
    jw_rawz(&prompt, "<conversation>\n");
    jw_raw(&prompt, conv.p ? conv.p : "", conv.n);
    jw_rawz(&prompt, "\n</conversation>\n\n");
    if (replace && instr && *instr)
        jw_rawz(&prompt, instr);
    else {
        cat_lines(&prompt, cp_branch_summary_prompt);
        if (instr && *instr) {
            jw_rawz(&prompt, "\n\nAdditional focus: ");
            jw_rawz(&prompt, instr);
        }
    }
    rc = summarize(fn, u, "Branch summarization", &prompt, min_tokens(4096, model_max_tokens), &text, &usage, err,
                   errcap);
    if (!rc) {
        cat_lines(&summary, cp_branch_summary_preamble);
        jw_raw(&summary, text.p ? text.p : "", text.n);
        memset(&mod, 0, sizeof(mod));
        memset(&ro, 0, sizeof(ro));
        for (i = 0; i < p.edited.n; i++)
            strs_add(&mod, p.edited.s[i], (long)strlen(p.edited.s[i]));
        for (i = 0; i < p.written.n; i++)
            strs_add(&mod, p.written.s[i], (long)strlen(p.written.s[i]));
        for (i = 0; i < p.read.n; i++) {
            int j, in_mod = 0;
            for (j = 0; j < mod.n; j++)
                in_mod |= !strcmp(mod.s[j], p.read.s[i]);
            if (!in_mod)
                strs_add(&ro, p.read.s[i], (long)strlen(p.read.s[i]));
        }
        if (ro.n > 1)
            qsort(ro.s, (size_t)ro.n, sizeof(char *), cmp_str);
        if (mod.n > 1)
            qsort(mod.s, (size_t)mod.n, sizeof(char *), cmp_str);
        file_lists_text(&summary, &ro, &mod);
        file_lists_json(&out->details, &ro, &mod);
        strs_free(&mod);
        strs_free(&ro);
        jw_raw(&out->summary, summary.p ? summary.p : "", summary.n);
        out->usage = usage;
        out->has_usage = 1;
    }
    jw_free(&conv);
    jw_free(&prompt);
    jw_free(&text);
    jw_free(&summary);
    cp_bprep_free(&p);
    if (rc)
        cp_branch_free(out);
    return rc;
}
