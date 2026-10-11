/* see session3.h */
#include <stdlib.h>
#include <string.h>
#include "session3.h"
#include "claude/util.h"
#include "claude/session.h"     /* sess_fit, sess_hash: C:Claude's 30-character rule */
#include "claude/tasks.h"       /* cron_civil */

#define S3_CHUNK 8192
#define S3_1978 252460800L      /* 1970-01-01 to 1978-01-01 in seconds */

static const char *const tnames[] = { "message", "thinking_level_change", "model_change", "usage", "compaction",
                                      "context_edit", "branch_summary", "custom", "custom_message", "label",
                                      "session_info" };

int s3_type_of(const char *name)
{
    int i;
    for (i = 0; i < S3_UNKNOWN; i++)
        if (!strcmp(name, tnames[i]))
            return i;
    return S3_UNKNOWN;
}

static int role_of(const char *r)
{
    static const char *const rn[] = { "user", "assistant", "toolResult", "system", "custom", "bashExecution" };
    int i;
    for (i = 0; i < 6; i++)
        if (!strcmp(r, rn[i]))
            return S3R_USER + i;
    return S3R_OTHER;
}

/* ---- time ---- */

static void two(char *o, int v)
{
    o[0] = (char)('0' + v / 10 % 10);
    o[1] = (char)('0' + v % 10);
}

void s3_iso(llm_ts t, int utc, char *out)
{
    cron_tm tm;
    long s = t.sec - S3_1978;
    if (s < 0)
        s = 0;
    cron_civil(s, &tm);
    two(out, tm.year / 100);
    two(out + 2, tm.year % 100);
    out[4] = '-';
    two(out + 5, tm.mon);
    out[7] = '-';
    two(out + 8, tm.day);
    out[10] = 'T';
    two(out + 11, tm.hour);
    out[13] = ':';
    two(out + 14, tm.min);
    out[16] = ':';
    two(out + 17, (int)(s % 60));
    out[19] = '.';
    out[20] = (char)('0' + t.ms / 100 % 10);
    out[21] = (char)('0' + t.ms / 10 % 10);
    out[22] = (char)('0' + t.ms % 10);
    out[23] = 0;
    if (utc) {
        out[23] = 'Z';
        out[24] = 0;
    }
}

void s3_ms(llm_ts t, jw *w)
{
    char ms[4];
    ms[0] = (char)('0' + t.ms / 100 % 10);
    ms[1] = (char)('0' + t.ms / 10 % 10);
    ms[2] = (char)('0' + t.ms % 10);
    ms[3] = 0;
    /* a long is 32 bits on the Amiga: the seconds and the three digits of milliseconds joined as text */
    if (t.sec == 0)
        jw_long(w, t.ms);
    else {
        jw_long(w, t.sec);
        jw_rawz(w, ms);
    }
}

static long digits(const char *s, int n)
{
    long v = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* days from 1970-01-01 to y-m-d (proleptic Gregorian) */
static long days_of(long y, long m, long d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int s3_iso_read(const char *s, llm_ts *t)
{
    long y, mo, d, h, mi, se, ms = 0;
    if (strlen(s) < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' ||
        s[16] != ':')
        return -1;
    y = digits(s, 4);
    mo = digits(s + 5, 2);
    d = digits(s + 8, 2);
    h = digits(s + 11, 2);
    mi = digits(s + 14, 2);
    se = digits(s + 17, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || h < 0 || mi < 0 || se < 0)
        return -1;
    if (s[19] == '.') {
        int k = 0;
        const char *p = s + 20;
        while (*p >= '0' && *p <= '9') {
            if (k < 3)
                ms = ms * 10 + (*p - '0');
            k++;
            p++;
        }
        while (k > 0 && k < 3) {
            ms *= 10;
            k++;
        }
    }
    t->sec = days_of(y, mo, d) * 86400L + h * 3600L + mi * 60L + se;
    t->ms = (int)ms;
    return 0;
}

static llm_ts now_of(pi_s3 *st)
{
    llm_ts t;
    if (st->now)
        return st->now(st->u);
    t.sec = 0;
    t.ms = 0;
    return t;
}

/* ---- the pool and the index ---- */

static long pool_add(pi_s3 *st, const char *s, long n)
{
    long at;
    if (st->pn + n + 1 > st->pcap) {
        long nc = st->pcap ? st->pcap * 2 : 1024;
        char *g;
        while (nc < st->pn + n + 1)
            nc *= 2;
        g = (char *)realloc(st->pool, (size_t)nc);
        if (!g) {
            st->oom = 1;
            return -1;
        }
        st->pool = g;
        st->pcap = nc;
    }
    at = st->pn;
    memcpy(st->pool + at, s, (size_t)n);
    st->pool[at + n] = 0;
    st->pn += n + 1;
    return at;
}

static int grow(pi_s3 *st)
{
    if (st->n == st->cap) {
        int nc = st->cap ? st->cap * 2 : 64;
        s3_ent *g = (s3_ent *)realloc(st->e, (size_t)nc * sizeof(*g));
        if (!g) {
            st->oom = 1;
            return -1;
        }
        st->e = g;
        st->cap = nc;
    }
    return 0;
}

const char *s3_id(const pi_s3 *st, int i)
{
    return i >= 0 && i < st->n ? st->pool + st->e[i].id : "";
}

const char *s3_label_of(const pi_s3 *st, int i)
{
    return i >= 0 && i < st->n && st->e[i].label >= 0 ? st->pool + st->e[i].label : 0;
}

int s3_find(const pi_s3 *st, const char *id)
{
    int i;
    /* newest first: a duplicated id (a hand-made file) is the last one, as pi's Map */
    for (i = st->n - 1; i >= 0; i--)
        if (!strcmp(st->pool + st->e[i].id, id))
            return i;
    return -1;
}

void s3_init(pi_s3 *st, cl_sys *sys, llm_ts (*now)(void *u), void *u, int utc, unsigned long seed)
{
    memset(st, 0, sizeof(*st));
    st->sys = sys;
    st->now = now;
    st->u = u;
    st->utc = utc;
    st->seed = seed;
    st->leaf = -1;
}

static void clear(pi_s3 *st)
{
    int i;
    for (i = 0; i < st->n; i++)
        free(st->e[i].text);
    free(st->e);
    free(st->pool);
    free(st->head);
    st->e = 0;
    st->pool = 0;
    st->head = 0;
    st->n = st->cap = 0;
    st->pn = st->pcap = 0;
    st->leaf = -1;
    st->flushed = 0;
}

void s3_free(pi_s3 *st)
{
    clear(st);
}

/* pi's generateId: 8 hex digits not yet used (a whole UUID after 100 tries) */
static void new_id(pi_s3 *st, char *out)
{
    char u[40];
    int k;
    for (k = 0; k < 100; k++) {
        cl_uuid(&st->seed, u);
        u[8] = 0;
        if (s3_find(st, u) < 0) {
            cl_copy(out, u, 40);
            return;
        }
    }
    cl_uuid(&st->seed, out);
}

/* pi's uuidv7: 48 bits of milliseconds, version 7, the rest random */
static void uuid7(pi_s3 *st, llm_ts t, char *out)
{
    static const char hex[] = "0123456789abcdef";
    unsigned long a = (unsigned long)t.sec >> 16, b = (unsigned long)t.sec & 0xffffUL, lo, mid, hi;
    int i;
    lo = b * 1000UL + (unsigned long)t.ms;
    mid = a * 1000UL + (lo >> 16);
    lo &= 0xffffUL;
    hi = mid >> 16;
    mid &= 0xffffUL;
    cl_uuid(&st->seed, out);
    for (i = 0; i < 4; i++) {
        out[i] = hex[(hi >> (12 - 4 * i)) & 15];
        out[4 + i] = hex[(mid >> (12 - 4 * i)) & 15];
        out[9 + i] = hex[(lo >> (12 - 4 * i)) & 15];
    }
    out[14] = '7';
}

/* an entry added to the index from its parsed line: its index, -1 */
static int index_line(pi_s3 *st, jv o, const char *text, long n, long off, int keep)
{
    jv v, x;
    char buf[64];
    s3_ent *e;
    if (grow(st))
        return -1;
    e = &st->e[st->n];
    memset(e, 0, sizeof(*e));
    e->parent = e->target = -1;
    e->pid = e->label = -1;
    e->off = off;
    e->len = n;
    e->type = S3_UNKNOWN;
    if (json_get(o, "type", &v) && json_type(v) == J_STR) {
        json_str(v, buf, sizeof(buf));
        e->type = (signed char)s3_type_of(buf);
    }
    e->id = -1;
    if (json_get(o, "id", &v) && json_type(v) == J_STR) {
        char *z = json_strdup(v, &n);
        if (z) {
            e->id = pool_add(st, z, (long)strlen(z));
            free(z);
        }
    }
    if (e->id < 0)
        e->id = pool_add(st, "", 0);
    if (json_get(o, "parentId", &v) && json_type(v) == J_STR) {
        char *z = json_strdup(v, &n);
        if (z) {
            e->pid = pool_add(st, z, (long)strlen(z));
            free(z);
        }
    }
    if (e->type == S3_MESSAGE && json_get(o, "message", &v) && json_get(v, "role", &x) && json_type(x) == J_STR) {
        json_str(x, buf, sizeof(buf));
        e->role = (signed char)role_of(buf);
    }
    if (keep) {
        e->text = (char *)malloc((size_t)e->len + 1);
        if (!e->text) {
            st->oom = 1;
            return -1;
        }
        memcpy(e->text, text, (size_t)e->len);
        e->text[e->len] = 0;
    }
    if (st->oom)
        return -1;
    return st->n++;
}

/* parents, targets and labels from the ids (after a load or a rewrite) */
static void resolve(pi_s3 *st)
{
    int i;
    for (i = 0; i < st->n; i++) {
        s3_ent *e = &st->e[i];
        e->parent = e->pid >= 0 ? s3_find(st, st->pool + e->pid) : -1;
        if (e->parent == i)
            e->parent = -1;     /* pi's getTree: its own parent is a root */
        e->label = -1;
    }
    for (i = 0; i < st->n; i++) {
        s3_ent *e = &st->e[i];
        if (e->type == S3_LABEL || e->type == S3_CONTEXT_EDIT) {
            long n;
            const char *t = s3_text(st, i, &n);
            jv o, v;
            e->target = -1;
            if (t && !json_parse(t, n, &o) && json_get(o, "targetId", &v) && json_type(v) == J_STR) {
                char *z = json_strdup(v, &n);
                if (z) {
                    e->target = s3_find(st, z);
                    free(z);
                }
                if (e->type == S3_LABEL && e->target >= 0) {
                    s3_ent *g = &st->e[e->target];
                    g->label = -1;
                    if (json_get(o, "label", &v) && json_type(v) == J_STR) {
                        char *z2 = json_strdup(v, &n);
                        if (z2 && *z2)
                            g->label = pool_add(st, z2, (long)strlen(z2));
                        free(z2);
                    }
                }
            }
        }
    }
}

/* ---- writing ---- */

static int has_conversation(const pi_s3 *st)
{
    int i;
    for (i = 0; i < st->n; i++)
        if (st->e[i].type == S3_MESSAGE && (st->e[i].role == S3R_USER || st->e[i].role == S3R_ASSISTANT))
            return 1;
    return 0;
}

/* dir made one level at a time (mkdir is one level) */
static int make_dirs(cl_sys *sys, const char *dir)
{
    char p[320];
    long i, n;
    if (!sys->mkdir)
        return -1;
    cl_copy(p, dir, sizeof(p));
    n = (long)strlen(p);
    for (i = 1; i <= n; i++)
        if (i == n || p[i] == '/') {
            char c = p[i];
            p[i] = 0;
            if (i > 0 && p[i - 1] != ':' && sys->kind(sys->u, p) != 2 && sys->mkdir(sys->u, p))
                return -1;
            p[i] = c;
        }
    return 0;
}

/* the header, then every entry, as one new file: 0, -1 */
static int write_all(pi_s3 *st)
{
    jw w;
    int i, rc;
    long at;
    jw_init(&w);
    jw_rawz(&w, st->head);
    jw_raw(&w, "\n", 1);
    for (i = 0; i < st->n; i++) {
        long n;
        const char *t = s3_text(st, i, &n);
        if (!t) {
            jw_free(&w);
            return -1;
        }
        st->e[i].off = w.n;
        jw_raw(&w, t, n);
        jw_raw(&w, "\n", 1);
    }
    if (w.oom) {
        jw_free(&w);
        st->oom = 1;
        return -1;
    }
    at = w.n;
    rc = st->sys->write(st->sys->u, st->file, w.p, w.n);
    jw_free(&w);
    if (rc) {
        cl_copy(st->err, "cannot write ", sizeof(st->err));
        cl_cat(st->err, st->file, sizeof(st->err));
        return -1;
    }
    st->fsize = at;
    st->appends += st->n + 1;
    return 0;
}

/* pi's _persist for entry i */
static int persist(pi_s3 *st, int i)
{
    if (!st->persist || !st->file[0])
        return 0;
    if (!st->flushed) {
        if (!has_conversation(st))
            return 0;
        if (st->sys->kind(st->sys->u, st->file) != 0) {
            /* pi opens it "wx": an existing file is not overwritten */
            cl_copy(st->err, "session file exists: ", sizeof(st->err));
            cl_cat(st->err, st->file, sizeof(st->err));
            return -1;
        }
        if (st->dir[0] && make_dirs(st->sys, st->dir)) {
            cl_copy(st->err, "cannot make ", sizeof(st->err));
            cl_cat(st->err, st->dir, sizeof(st->err));
            return -1;
        }
        if (write_all(st))
            return -1;
        st->flushed = 1;
        return 0;
    } else {
        s3_ent *e = &st->e[i];
        char *line = (char *)malloc((size_t)e->len + 1);
        int rc;
        if (!line) {
            st->oom = 1;
            return -1;
        }
        memcpy(line, e->text, (size_t)e->len);
        line[e->len] = '\n';
        e->off = st->fsize;
        rc = st->sys->append ? st->sys->append(st->sys->u, st->file, line, e->len + 1) : -1;
        free(line);
        if (rc) {
            e->off = -1;
            cl_copy(st->err, "cannot append to ", sizeof(st->err));
            cl_cat(st->err, st->file, sizeof(st->err));
            return -1;
        }
        st->fsize += e->len + 1;
        st->appends++;
    }
    return 0;
}

static void file_name(pi_s3 *st, llm_ts t, char *out, long cap)
{
    char iso[32], f[40];
    s3_iso(t, 0, iso);
    /* 20261011T180512_<the uuid's last 8 hex>.jsonl: 30 characters */
    memcpy(f, iso, 4);
    memcpy(f + 4, iso + 5, 2);
    memcpy(f + 6, iso + 8, 2);
    f[8] = 'T';
    memcpy(f + 9, iso + 11, 2);
    memcpy(f + 11, iso + 14, 2);
    memcpy(f + 13, iso + 17, 2);
    f[15] = '_';
    memcpy(f + 16, st->id + strlen(st->id) - 8, 8);
    f[24] = 0;
    cl_cat(f, ".jsonl", sizeof(f));
    cl_copy(out, st->dir, cap);
    if (out[0] && out[strlen(out) - 1] != ':' && out[strlen(out) - 1] != '/')
        cl_cat(out, "/", cap);
    cl_cat(out, f, cap);
}

static int make_head(pi_s3 *st, const char *ts)
{
    jw w;
    jw_init(&w);
    jw_rawz(&w, "{\"type\":\"session\",\"version\":3,\"id\":");
    jw_strz(&w, st->id);
    jw_rawz(&w, ",\"timestamp\":");
    jw_strz(&w, ts);
    jw_rawz(&w, ",\"cwd\":");
    jw_strz(&w, st->cwd);
    if (st->parent[0]) {
        jw_rawz(&w, ",\"parentSession\":");
        jw_strz(&w, st->parent);
    }
    jw_rawz(&w, "}");
    free(st->head);
    st->head = w.p;
    if (w.oom || !w.p) {
        st->head = 0;
        jw_free(&w);
        st->oom = 1;
        return -1;
    }
    return 0;
}

int s3_new(pi_s3 *st, const char *dir, const char *cwd, const char *parent, const char *id, int persist_)
{
    llm_ts t = now_of(st);
    char ts[32];
    clear(st);
    st->persist = persist_;
    cl_copy(st->dir, dir ? dir : "", sizeof(st->dir));
    cl_copy(st->cwd, cwd ? cwd : "", sizeof(st->cwd));
    cl_copy(st->parent, parent ? parent : "", sizeof(st->parent));
    if (id && *id)
        cl_copy(st->id, id, sizeof(st->id));
    else
        uuid7(st, t, st->id);
    s3_iso(t, st->utc, ts);
    st->file[0] = 0;
    if (persist_ && st->dir[0])
        file_name(st, t, st->file, sizeof(st->file));
    return make_head(st, ts);
}

int s3_append(pi_s3 *st, const char *type, const char *pre, const char *post)
{
    jw w;
    char id[40], ts[32];
    int i, prev_leaf = st->leaf;
    jv o;
    new_id(st, id);
    s3_iso(st->at_set ? st->at : now_of(st), st->utc, ts);
    st->at_set = 0;
    jw_init(&w);
    jw_rawz(&w, "{\"type\":");
    jw_strz(&w, type);
    if (pre && *pre) {
        jw_raw(&w, ",", 1);
        jw_rawz(&w, pre);
    }
    jw_rawz(&w, ",\"id\":");
    jw_strz(&w, id);
    jw_rawz(&w, ",\"parentId\":");
    if (st->leaf >= 0)
        jw_strz(&w, s3_id(st, st->leaf));
    else
        jw_rawz(&w, "null");
    jw_rawz(&w, ",\"timestamp\":");
    jw_strz(&w, ts);
    if (post && *post) {
        jw_raw(&w, ",", 1);
        jw_rawz(&w, post);
    }
    jw_rawz(&w, "}");
    if (w.oom || json_parse(w.p, w.n, &o)) {
        jw_free(&w);
        cl_copy(st->err, "an entry that is not JSON", sizeof(st->err));
        return -1;
    }
    i = index_line(st, o, w.p, w.n, -1, 1);
    jw_free(&w);
    if (i < 0)
        return -1;
    st->e[i].parent = prev_leaf;
    st->leaf = i;
    if (persist(st, i))
        return -1;
    return i;
}

int s3_message(pi_s3 *st, const char *msg, long n)
{
    jw w;
    int i;
    jw_init(&w);
    jw_rawz(&w, "\"message\":");
    jw_raw(&w, msg, n);
    i = w.oom ? -1 : s3_append(st, "message", 0, w.p);
    jw_free(&w);
    return i;
}

static int append1(pi_s3 *st, const char *type, const char *k, const char *v)
{
    jw w;
    int i;
    jw_init(&w);
    jw_raw(&w, "\"", 1);
    jw_rawz(&w, k);
    jw_rawz(&w, "\":");
    jw_strz(&w, v);
    i = w.oom ? -1 : s3_append(st, type, 0, w.p);
    jw_free(&w);
    return i;
}

int s3_thinking(pi_s3 *st, const char *level)
{
    return append1(st, "thinking_level_change", "thinkingLevel", level);
}

int s3_model(pi_s3 *st, const char *provider, const char *model)
{
    jw w;
    int i;
    jw_init(&w);
    jw_rawz(&w, "\"provider\":");
    jw_strz(&w, provider);
    jw_rawz(&w, ",\"modelId\":");
    jw_strz(&w, model);
    i = w.oom ? -1 : s3_append(st, "model_change", 0, w.p);
    jw_free(&w);
    return i;
}

int s3_usage(pi_s3 *st, const char *kind, const char *provider, const char *model, const llm_usage *u,
             const char *note)
{
    jw w;
    int i;
    jw_init(&w);
    jw_rawz(&w, "\"kind\":");
    jw_strz(&w, kind);
    jw_rawz(&w, ",\"provider\":");
    jw_strz(&w, provider);
    jw_rawz(&w, ",\"model\":");
    jw_strz(&w, model);
    jw_rawz(&w, ",\"usage\":");
    llm_w_usage(&w, u);
    if (note && *note) {
        jw_rawz(&w, ",\"note\":");
        jw_strz(&w, note);
    }
    i = w.oom ? -1 : s3_append(st, "usage", 0, w.p);
    jw_free(&w);
    return i;
}

int s3_compaction(pi_s3 *st, const char *summary, int first_kept, long tokens_before, const char *details,
                  const char *usage, int from_hook, const char *system)
{
    jw w;
    int i;
    char self[40];
    /* the entry's own id is the next new one: retain-none points at itself */
    jw_init(&w);
    jw_rawz(&w, "\"summary\":");
    jw_strz(&w, summary);
    jw_rawz(&w, ",\"firstKeptEntryId\":");
    if (first_kept >= 0)
        jw_strz(&w, s3_id(st, first_kept));
    else {
        unsigned long keep = st->seed;
        new_id(st, self);
        st->seed = keep;        /* s3_append draws the same id */
        jw_strz(&w, self);
    }
    jw_rawz(&w, ",\"tokensBefore\":");
    jw_long(&w, tokens_before);
    if (details) {
        jw_rawz(&w, ",\"details\":");
        jw_rawz(&w, details);
    }
    if (usage) {
        jw_rawz(&w, ",\"usage\":");
        jw_rawz(&w, usage);
    }
    if (from_hook >= 0)
        jw_rawz(&w, from_hook ? ",\"fromHook\":true" : ",\"fromHook\":false");
    if (system) {
        /* pi: {...systemMessage, timestamp: the entry's own time} */
        jw ms, sysw;
        jv so;
        jw_init(&ms);
        jw_init(&sysw);
        st->at = now_of(st);
        st->at_set = 1;
        s3_ms(st->at, &ms);
        if (json_parse(system, (long)strlen(system), &so) || s3_obj_put(so, &sysw, "timestamp", ms.p, 0)) {
            jw_free(&ms);
            jw_free(&sysw);
            jw_free(&w);
            st->at_set = 0;
            cl_copy(st->err, "a system message that is not JSON", sizeof(st->err));
            return -1;
        }
        jw_rawz(&w, ",\"systemMessage\":");
        jw_raw(&w, sysw.p, sysw.n);
        jw_free(&ms);
        jw_free(&sysw);
    }
    i = w.oom ? -1 : s3_append(st, "compaction", 0, w.p);
    jw_free(&w);
    return i;
}

int s3_branch_summary(pi_s3 *st, int from, const char *summary, const char *details, const char *usage, int from_hook)
{
    jw w;
    int i;
    char old[40];
    cl_copy(old, st->leaf >= 0 ? s3_id(st, st->leaf) : "root", sizeof(old));
    s3_set_leaf(st, from);
    jw_init(&w);
    jw_rawz(&w, "\"fromId\":");
    jw_strz(&w, old);
    jw_rawz(&w, ",\"summary\":");
    jw_strz(&w, summary);
    if (details) {
        jw_rawz(&w, ",\"details\":");
        jw_rawz(&w, details);
    }
    if (usage) {
        jw_rawz(&w, ",\"usage\":");
        jw_rawz(&w, usage);
    }
    if (from_hook >= 0)
        jw_rawz(&w, from_hook ? ",\"fromHook\":true" : ",\"fromHook\":false");
    i = w.oom ? -1 : s3_append(st, "branch_summary", 0, w.p);
    jw_free(&w);
    return i;
}

int s3_custom(pi_s3 *st, const char *custom_type, const char *data)
{
    jw w;
    int i;
    jw_init(&w);
    jw_rawz(&w, "\"customType\":");
    jw_strz(&w, custom_type);
    if (data) {
        jw_rawz(&w, ",\"data\":");
        jw_rawz(&w, data);
    }
    i = w.oom ? -1 : s3_append(st, "custom", w.p, 0);
    jw_free(&w);
    return i;
}

int s3_custom_message(pi_s3 *st, const char *custom_type, const char *content, int display, const char *details)
{
    jw w;
    int i;
    jw_init(&w);
    jw_rawz(&w, "\"customType\":");
    jw_strz(&w, custom_type);
    jw_rawz(&w, ",\"content\":");
    jw_rawz(&w, content);
    jw_rawz(&w, display ? ",\"display\":true" : ",\"display\":false");
    if (details) {
        jw_rawz(&w, ",\"details\":");
        jw_rawz(&w, details);
    }
    i = w.oom ? -1 : s3_append(st, "custom_message", w.p, 0);
    jw_free(&w);
    return i;
}

static int on_path(const pi_s3 *st, int leaf, int i)
{
    int k = 0;
    while (leaf >= 0 && k++ <= st->n) {
        if (leaf == i)
            return 1;
        leaf = st->e[leaf].parent;
    }
    return 0;
}

int s3_context_edit(pi_s3 *st, int target, const char *replacement)
{
    jw w;
    int i, role;
    jv r, c;
    if (replacement && (json_parse(replacement, (long)strlen(replacement), &r) || json_type(r) != J_OBJ ||
                        !json_get(r, "content", &c) || (json_type(c) != J_STR && json_type(c) != J_ARR))) {
        cl_copy(st->err, "Context edit replacement must be null or contain string/array content",
                sizeof(st->err));
        return -1;
    }
    if (target < 0 || target >= st->n) {
        cl_copy(st->err, "Entry not found", sizeof(st->err));
        return -1;
    }
    if (!on_path(st, st->leaf, target)) {
        cl_copy(st->err, "Entry ", sizeof(st->err));
        cl_cat(st->err, s3_id(st, target), sizeof(st->err));
        cl_cat(st->err, " is not on the active branch", sizeof(st->err));
        return -1;
    }
    role = st->e[target].type == S3_CUSTOM_MESSAGE ? S3R_CUSTOM
         : st->e[target].type == S3_MESSAGE        ? st->e[target].role
                                                   : S3R_NONE;
    if (!(st->e[target].type == S3_CUSTOM_MESSAGE ||
          (st->e[target].type == S3_MESSAGE && (role == S3R_USER || role == S3R_ASSISTANT || role == S3R_TOOLRESULT)))) {
        cl_copy(st->err, "Entry ", sizeof(st->err));
        cl_cat(st->err, s3_id(st, target), sizeof(st->err));
        cl_cat(st->err, " does not contribute editable model content", sizeof(st->err));
        return -1;
    }
    jw_init(&w);
    jw_rawz(&w, "\"targetId\":");
    jw_strz(&w, s3_id(st, target));
    jw_rawz(&w, ",\"replacement\":");
    if (!replacement)
        jw_rawz(&w, "null");
    else if ((role == S3R_ASSISTANT || role == S3R_TOOLRESULT) && json_type(c) == J_STR) {
        /* a string for these roles becomes one text block */
        char *z;
        long zn;
        z = json_strdup(c, &zn);
        jw_rawz(&w, "{\"content\":[{\"type\":\"text\",\"text\":");
        jw_str(&w, z ? z : "", z ? zn : 0);
        jw_rawz(&w, "}]}");
        free(z);
    } else
        jw_rawz(&w, replacement);
    i = w.oom ? -1 : s3_append(st, "context_edit", 0, w.p);
    jw_free(&w);
    if (i >= 0)
        st->e[i].target = target;
    return i;
}

int s3_label(pi_s3 *st, int target, const char *label)
{
    jw w;
    int i;
    if (target < 0 || target >= st->n) {
        cl_copy(st->err, "Entry not found", sizeof(st->err));
        return -1;
    }
    jw_init(&w);
    jw_rawz(&w, "\"targetId\":");
    jw_strz(&w, s3_id(st, target));
    if (label && *label) {
        jw_rawz(&w, ",\"label\":");
        jw_strz(&w, label);
    }
    i = w.oom ? -1 : s3_append(st, "label", 0, w.p);
    jw_free(&w);
    if (i >= 0) {
        st->e[i].target = target;
        st->e[target].label = label && *label ? pool_add(st, label, (long)strlen(label)) : -1;
    }
    return i;
}

int s3_session_info(pi_s3 *st, const char *name)
{
    /* pi: line breaks to one space, trimmed */
    char *s = (char *)malloc(strlen(name) + 1);
    long k = 0, a, b;
    int i;
    const char *p;
    if (!s)
        return -1;
    for (p = name; *p;) {
        if (*p == '\r' || *p == '\n') {
            while (*p == '\r' || *p == '\n')
                p++;
            s[k++] = ' ';
        } else
            s[k++] = *p++;
    }
    s[k] = 0;
    for (a = 0; s[a] == ' ' || s[a] == '\t'; a++)
        ;
    for (b = k; b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'); b--)
        ;
    s[b] = 0;
    i = append1(st, "session_info", "name", s + a);
    free(s);
    return i;
}

/* ---- reading ---- */

const char *s3_text(pi_s3 *st, int i, long *n)
{
    s3_ent *e;
    long got = 0, k;
    char *b;
    if (i < 0 || i >= st->n)
        return 0;
    e = &st->e[i];
    *n = e->len;
    if (e->text)
        return e->text;
    if (e->off < 0 || !st->sys->pread)
        return 0;
    b = (char *)malloc((size_t)e->len + 1);
    if (!b) {
        st->oom = 1;
        return 0;
    }
    for (k = 0; k < e->len; k += got)
        if (st->sys->pread(st->sys->u, st->file, e->off + k, b + k, e->len - k, &got) || got <= 0) {
            free(b);
            return 0;
        }
    b[e->len] = 0;
    e->text = b;
    st->reads++;
    return b;
}

int s3_path(const pi_s3 *st, int i, int **out)
{
    int k = 0, len, j, *p;
    for (j = i; j >= 0 && k <= st->n; j = st->e[j].parent)
        k++;
    len = k;
    p = (int *)malloc((size_t)(k ? k : 1) * sizeof(int));
    *out = p;
    if (!p)
        return -1;
    for (j = i; j >= 0 && k > 0; j = st->e[j].parent)
        p[--k] = j;
    return len;
}

int s3_name(pi_s3 *st, char *out, long cap)
{
    int i;
    for (i = st->n - 1; i >= 0; i--)
        if (st->e[i].type == S3_SESSION_INFO) {
            long n;
            const char *t = s3_text(st, i, &n);
            jv o, v;
            char *z;
            long a, b, zn;
            out[0] = 0;
            if (!t || json_parse(t, n, &o) || !json_get(o, "name", &v) || json_type(v) != J_STR)
                return 0;
            z = json_strdup(v, &zn);
            if (!z)
                return 0;
            for (a = 0; z[a] == ' '; a++)
                ;
            for (b = zn; b > a && z[b - 1] == ' '; b--)
                ;
            z[b] = 0;
            cl_copy(out, z + a, cap);
            free(z);
            return out[0] != 0;
        }
    return 0;
}

/* ---- the tree ---- */

void s3_set_leaf(pi_s3 *st, int i)
{
    int k;
    st->leaf = i >= 0 && i < st->n ? i : -1;
    /* what is in the file and off the new branch leaves RAM (E2) */
    for (k = 0; k < st->n; k++)
        if (st->e[k].text && st->e[k].off >= 0 && st->flushed && !on_path(st, st->leaf, k)) {
            free(st->e[k].text);
            st->e[k].text = 0;
        }
}

/* a member set in an object: replaced where it is, else appended; drop a member named drop */
int s3_obj_put(jv o, jw *out, const char *key, const char *val, const char *drop)
{
    jit it;
    jv k, v;
    int first = 1, done = 0;
    jw_raw(out, "{", 1);
    json_iter(o, &it);
    while (json_next(&it, &k, &v)) {
        if (drop && json_streq(k, drop))
            continue;
        if (!first)
            jw_raw(out, ",", 1);
        first = 0;
        jw_raw(out, k.p, k.n);
        jw_raw(out, ":", 1);
        if (key && json_streq(k, key) && !done) {
            jw_rawz(out, val);
            done = 1;
        } else
            jw_raw(out, v.p, v.n);
    }
    if (key && !done) {
        if (!first)
            jw_raw(out, ",", 1);
        jw_raw(out, "\"", 1);
        jw_rawz(out, key);
        jw_rawz(out, "\":");
        jw_rawz(out, val);
    }
    jw_raw(out, "}", 1);
    return out->oom ? -1 : 0;
}

int s3_branched(pi_s3 *st, int leaf)
{
    int *path = 0, np, i, k, prev = -1;
    int *map;                   /* label entry -> the entry that follows it on the path */
    pi_s3 nw;
    llm_ts t = now_of(st);
    char ts[32];
    np = s3_path(st, leaf, &path);
    if (np <= 0) {
        free(path);
        cl_copy(st->err, "Entry not found", sizeof(st->err));
        return -1;
    }
    map = (int *)malloc((size_t)st->n * sizeof(int));
    if (!map) {
        free(path);
        return -1;
    }
    for (i = 0; i < st->n; i++)
        map[i] = -1;
    s3_init(&nw, st->sys, st->now, st->u, st->utc, st->seed);
    nw.persist = st->persist;
    cl_copy(nw.dir, st->dir, sizeof(nw.dir));
    cl_copy(nw.cwd, st->cwd, sizeof(nw.cwd));
    cl_copy(nw.parent, st->persist ? st->file : "", sizeof(nw.parent));
    uuid7(&nw, t, nw.id);
    s3_iso(t, st->utc, ts);
    if (st->persist && nw.dir[0])
        file_name(&nw, t, nw.file, sizeof(nw.file));
    if (make_head(&nw, ts))
        goto fail;
    {
        /* labels dropped and the path re-chained; a label's id stands for the next kept entry */
        int pend[64], npend = 0;
        for (k = 0; k < np; k++) {
            int j = path[k];
            long n;
            const char *tx;
            jv o, v;
            jw w, w2;
            char pid[48];
            if (st->e[j].type == S3_LABEL) {
                if (npend < 64)
                    pend[npend++] = j;
                continue;
            }
            for (i = 0; i < npend; i++)
                map[pend[i]] = j;
            npend = 0;
            tx = s3_text(st, j, &n);
            if (!tx || json_parse(tx, n, &o))
                goto fail;
            jw_init(&w);
            if (prev >= 0) {
                jw_init(&w2);
                jw_strz(&w2, s3_id(st, prev));
                cl_copy(pid, w2.p ? w2.p : "null", sizeof(pid));
                jw_free(&w2);
            } else
                cl_copy(pid, "null", sizeof(pid));
            if (s3_obj_put(o, &w, "parentId", pid, 0)) {
                jw_free(&w);
                goto fail;
            }
            if (st->e[j].type == S3_COMPACTION && json_get(o, "firstKeptEntryId", &v) && json_type(v) == J_STR) {
                char fk[48];
                int f;
                json_str(v, fk, sizeof(fk));
                f = s3_find(st, fk);
                if (strcmp(fk, s3_id(st, j)) && f >= 0 && st->e[f].type == S3_LABEL && map[f] >= 0) {
                    jv o2;
                    jw w3;
                    jw_init(&w2);
                    jw_strz(&w2, s3_id(st, map[f]));
                    jw_init(&w3);
                    if (json_parse(w.p, w.n, &o2) || s3_obj_put(o2, &w3, "firstKeptEntryId", w2.p, 0)) {
                        jw_free(&w);
                        jw_free(&w2);
                        jw_free(&w3);
                        goto fail;
                    }
                    jw_free(&w);
                    jw_free(&w2);
                    w = w3;
                }
            }
            if (json_parse(w.p, w.n, &o) || index_line(&nw, o, w.p, w.n, -1, 1) < 0) {
                jw_free(&w);
                goto fail;
            }
            jw_free(&w);
            prev = j;
        }
    }
    resolve(&nw);
    {
        /* the labels of entries on the path, after it, with their own times, in
         * the order pi's label Map holds them: a target goes in at its first set
         * (after a clear), and keeps that place when its label changes */
        int *ord = (int *)malloc((size_t)(st->n ? st->n : 1) * sizeof(int)), no = 0, j, q;
        int *lastset = (int *)malloc((size_t)(st->n ? st->n : 1) * sizeof(int));
        if (!ord || !lastset) {
            free(ord);
            free(lastset);
            goto fail;
        }
        for (j = 0; j < st->n; j++) {
            int tg = st->e[j].target, at = -1;
            if (st->e[j].type != S3_LABEL || tg < 0)
                continue;
            for (q = 0; q < no; q++)
                if (st->e[ord[q]].target == tg)
                    at = q;
            {
                long n;
                const char *tx = s3_text(st, j, &n);
                jv o, v;
                int set = tx && !json_parse(tx, n, &o) && json_get(o, "label", &v) && json_type(v) == J_STR &&
                          v.n > 2;
                if (set && at < 0) {
                    ord[no] = j;
                    lastset[no++] = j;
                } else if (set)
                    lastset[at] = j;
                else if (at >= 0) {
                    memmove(ord + at, ord + at + 1, (size_t)(no - at - 1) * sizeof(int));
                    memmove(lastset + at, lastset + at + 1, (size_t)(no - at - 1) * sizeof(int));
                    no--;
                }
            }
        }
        for (q = 0; q < no; q++) {
            int tg = st->e[ord[q]].target;
            long n;
            const char *tx;
            jv o, v;
            jw w;
            char id[40], lts[48];
            j = lastset[q];
            if (!s3_label_of(st, tg) || s3_find(&nw, s3_id(st, tg)) < 0)
                continue;
            tx = s3_text(st, j, &n);
            if (!tx || json_parse(tx, n, &o) || !json_get(o, "timestamp", &v))
                goto fail;
            json_str(v, lts, sizeof(lts));
            new_id(&nw, id);
            jw_init(&w);
            jw_rawz(&w, "{\"type\":\"label\",\"id\":");
            jw_strz(&w, id);
            jw_rawz(&w, ",\"parentId\":");
            if (nw.n)
                jw_strz(&w, s3_id(&nw, nw.n - 1));
            else
                jw_rawz(&w, "null");
            jw_rawz(&w, ",\"timestamp\":");
            jw_strz(&w, lts);
            jw_rawz(&w, ",\"targetId\":");
            jw_strz(&w, s3_id(st, tg));
            jw_rawz(&w, ",\"label\":");
            jw_strz(&w, s3_label_of(st, tg));
            jw_rawz(&w, "}");
            if (w.oom || json_parse(w.p, w.n, &o) || index_line(&nw, o, w.p, w.n, -1, 1) < 0) {
                jw_free(&w);
                free(ord);
                free(lastset);
                goto fail;
            }
            jw_free(&w);
        }
        free(ord);
        free(lastset);
    }
    resolve(&nw);
    nw.leaf = nw.n - 1;
    if (nw.persist && nw.file[0] && has_conversation(&nw)) {
        if (nw.dir[0] && make_dirs(nw.sys, nw.dir))
            goto fail;
        if (write_all(&nw))
            goto fail;
        nw.flushed = 1;
    }
    nw.appends += st->appends;
    nw.reads += st->reads;
    free(path);
    free(map);
    clear(st);
    *st = nw;
    return 0;
fail:
    if (!st->err[0])
        cl_copy(st->err, nw.err[0] ? nw.err : "out of memory", sizeof(st->err));
    s3_free(&nw);
    free(path);
    free(map);
    return -1;
}

/* ---- loading ---- */

typedef struct reader {
    pi_s3 *st;
    const char *path;
    long off;                   /* where the next chunk starts */
    char buf[S3_CHUNK];
    long bn, bi;
    int eof;
} reader;

/* the next line into w (without its newline): 1, 0 at the end; *at its offset, *nl 0 when it had none */
static int next_line(reader *r, jw *w, long *at, int *nl)
{
    jw_reset(w);
    *at = r->off - r->bn + r->bi;
    for (;;) {
        long i;
        if (r->bi >= r->bn) {
            long got = 0;
            if (r->eof)
                break;
            if (r->st->sys->pread(r->st->sys->u, r->path, r->off, r->buf, S3_CHUNK, &got) || got <= 0) {
                r->eof = 1;
                break;
            }
            r->off += got;
            r->bn = got;
            r->bi = 0;
        }
        for (i = r->bi; i < r->bn && r->buf[i] != '\n'; i++)
            ;
        jw_raw(w, r->buf + r->bi, i - r->bi);
        if (i < r->bn) {
            r->bi = i + 1;
            *nl = 1;
            return 1;
        }
        r->bi = r->bn;
    }
    *nl = 0;
    return w->n > 0;
}

static int blank(const char *s, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            return 0;
    return 1;
}

/* the line's own parse; 0 when it is not an object (pi skips it) */
static int parse_obj(const char *s, long n, jv *o)
{
    return !json_parse(s, n, o) && json_type(*o) == J_OBJ;
}

static int migrate(pi_s3 *st, const char *path);

int s3_open(pi_s3 *st, const char *path)
{
    reader *r;
    jw w;
    long at;
    int nl = 1, last_nl = 1, got_head = 0, version = 1, rc = 0;
    long size = 0;
    clear(st);
    st->persist = 1;
    st->err[0] = 0;
    cl_copy(st->file, path, sizeof(st->file));
    {
        /* the directory: the file's parent */
        long k = (long)strlen(path);
        while (k > 0 && path[k - 1] != '/' && path[k - 1] != ':')
            k--;
        cl_copy(st->dir, path, k + 1 > (long)sizeof(st->dir) ? (long)sizeof(st->dir) : k + 1);
        if (k > 1 && st->dir[k - 1] == '/')
            st->dir[k - 1] = 0;
    }
    if (st->sys->kind(st->sys->u, path) != 1) {
        /* pi: a new session kept at the path asked for */
        char keep[320], d[256], c[256];
        cl_copy(keep, path, sizeof(keep));
        cl_copy(d, st->dir, sizeof(d));
        cl_copy(c, st->cwd, sizeof(c));
        if (s3_new(st, d, c, 0, 0, 1))
            return -1;
        cl_copy(st->file, keep, sizeof(st->file));
        return 0;
    }
    if (!st->sys->pread) {
        cl_copy(st->err, "no ranged reads on this system", sizeof(st->err));
        return -1;
    }
    r = (reader *)malloc(sizeof(reader));
    if (!r)
        return -1;
    memset(r, 0, sizeof(*r));
    r->st = st;
    r->path = path;
    jw_init(&w);
    while (next_line(r, &w, &at, &nl)) {
        jv o, v;
        size = at + w.n + (nl ? 1 : 0);
        last_nl = nl;
        if (blank(w.p, w.n) || !parse_obj(w.p, w.n, &o))
            continue;
        if (!got_head) {
            char type[16];
            type[0] = 0;
            if (json_get(o, "type", &v))
                json_str(v, type, sizeof(type));
            if (strcmp(type, "session") || !json_get(o, "id", &v) || json_type(v) != J_STR) {
                rc = -2;
                break;
            }
            got_head = 1;
            json_str(v, st->id, sizeof(st->id));
            if (json_get(o, "cwd", &v) && json_type(v) == J_STR)
                json_str(v, st->cwd, sizeof(st->cwd));
            if (json_get(o, "parentSession", &v) && json_type(v) == J_STR)
                json_str(v, st->parent, sizeof(st->parent));
            if (json_get(o, "version", &v) && json_type(v) == J_NUM)
                version = (int)json_long(v, 1);
            st->head = (char *)malloc((size_t)w.n + 1);
            if (!st->head) {
                rc = -1;
                break;
            }
            memcpy(st->head, w.p, (size_t)w.n);
            st->head[w.n] = 0;
            continue;
        }
        if (index_line(st, o, w.p, w.n, at, 0) < 0) {
            rc = -1;
            break;
        }
    }
    jw_free(&w);
    free(r);
    if (rc == -2 || (!got_head && size > 0)) {
        clear(st);
        cl_copy(st->err, "Session file is not a valid pi session: ", sizeof(st->err));
        cl_cat(st->err, path, sizeof(st->err));
        return -1;
    }
    if (rc)
        return -1;
    if (!got_head) {
        /* an empty file: a new session written there */
        char keep[320], d[256], c[256];
        cl_copy(keep, path, sizeof(keep));
        cl_copy(d, st->dir, sizeof(d));
        cl_copy(c, st->cwd, sizeof(c));
        if (s3_new(st, d, c, 0, 0, 1))
            return -1;
        cl_copy(st->file, keep, sizeof(st->file));
        if (write_all(st))
            return -1;
        st->flushed = 1;
        return 0;
    }
    st->flushed = 1;
    st->fsize = size;
    if (!last_nl && size > 0 && st->sys->append) {
        /* pi repairs a last line without its newline */
        if (!st->sys->append(st->sys->u, path, "\n", 1))
            st->fsize++;
    }
    if (version < 3)
        return migrate(st, path);
    resolve(st);
    s3_set_leaf(st, st->n - 1);
    {
        /* the active branch read into RAM */
        int j;
        for (j = st->leaf; j >= 0; j = st->e[j].parent) {
            long n;
            if (!s3_text(st, j, &n))
                return -1;
        }
    }
    return 0;
}

/* pi's migrateV1ToV2 and migrateV2ToV3, then _rewriteFile: the whole file
 * once (an old file from a Mac's pi; rare) */
static int migrate(pi_s3 *st, const char *path)
{
    char *b = 0, *p, *e;
    long n = 0;
    jw out;
    int rc = -1, version = 1, nent = 0, cap = 0;
    char **ids = 0;             /* every parsed line's id (the header's ""), for firstKeptEntryIndex */
    char prev[48];
    clear(st);
    if (st->sys->read(st->sys->u, path, 64L * 1024 * 1024, &b, &n))
        return -1;
    jw_init(&out);
    prev[0] = 0;
    /* pass 1: the ids v1 gets (in order), so an index can name one */
    for (p = b; p < b + n; p = e + 1) {
        jv o, v;
        for (e = p; e < b + n && *e != '\n'; e++)
            ;
        if (blank(p, e - p) || !parse_obj(p, e - p, &o))
            continue;
        if (nent == cap) {
            int nc = cap ? cap * 2 : 64;
            char **g = (char **)realloc(ids, (size_t)nc * sizeof(*g));
            if (!g)
                goto done;
            ids = g;
            cap = nc;
        }
        ids[nent] = 0;
        if (json_get(o, "type", &v) && json_streq(v, "session")) {
            if (json_get(o, "version", &v) && json_type(v) == J_NUM)
                version = (int)json_long(v, 1);
        }
        nent++;
    }
    {
        int k = 0;
        for (p = b; p < b + n; p = e + 1) {
            jv o, v, m, r;
            jw a, c;
            int is_head;
            for (e = p; e < b + n && *e != '\n'; e++)
                ;
            if (blank(p, e - p) || !parse_obj(p, e - p, &o))
                continue;
            is_head = json_get(o, "type", &v) && json_streq(v, "session");
            jw_init(&a);
            if (is_head) {
                if (s3_obj_put(o, &a, "version", "3", 0))
                    goto fail1;
            } else {
                jw_raw(&a, p, e - p);
                if (version < 2) {
                    char id[40], val[60];
                    jw t;
                    jv o2;
                    new_id(st, id);
                    ids[k] = (char *)malloc(strlen(id) + 1);
                    if (!ids[k])
                        goto fail1;
                    strcpy(ids[k], id);
                    /* id, then parentId, appended as pi assigns them */
                    jw_init(&t);
                    jw_strz(&t, id);
                    cl_copy(val, t.p ? t.p : "\"\"", sizeof(val));
                    jw_free(&t);
                    jw_init(&c);
                    if (json_parse(a.p, a.n, &o2) || s3_obj_put(o2, &c, "id", val, 0))
                        goto fail2;
                    jw_reset(&a);
                    jw_init(&t);
                    if (prev[0])
                        jw_strz(&t, prev);
                    else
                        jw_rawz(&t, "null");
                    if (json_parse(c.p, c.n, &o2) || s3_obj_put(o2, &a, "parentId", t.p, 0)) {
                        jw_free(&t);
                        goto fail2;
                    }
                    jw_free(&t);
                    jw_free(&c);
                    cl_copy(prev, id, sizeof(prev));
                    if (json_get(o, "type", &v) && json_streq(v, "compaction") &&
                        json_get(o, "firstKeptEntryIndex", &v) && json_type(v) == J_NUM) {
                        long fi = json_long(v, -1);
                        jw_init(&c);
                        if (json_parse(a.p, a.n, &o2))
                            goto fail2;
                        if (fi >= 0 && fi < nent && fi < k && ids[fi]) {
                            jw_init(&t);
                            jw_strz(&t, ids[fi]);
                            if (s3_obj_put(o2, &c, "firstKeptEntryId", t.p, "firstKeptEntryIndex")) {
                                jw_free(&t);
                                goto fail2;
                            }
                            jw_free(&t);
                        } else if (s3_obj_put(o2, &c, 0, 0, "firstKeptEntryIndex"))
                            goto fail2;
                        jw_free(&a);
                        a = c;
                    }
                }
                /* v2 -> v3: the hookMessage role is custom */
                if (json_parse(a.p, a.n, &o) == 0 && json_get(o, "type", &v) && json_streq(v, "message") &&
                    json_get(o, "message", &m) && json_get(m, "role", &r) && json_streq(r, "hookMessage")) {
                    jw mm;
                    jw_init(&mm);
                    jw_init(&c);
                    if (s3_obj_put(m, &mm, "role", "\"custom\"", 0) || s3_obj_put(o, &c, "message", mm.p, 0)) {
                        jw_free(&mm);
                        goto fail2;
                    }
                    jw_free(&mm);
                    jw_free(&a);
                    a = c;
                }
            }
            jw_raw(&out, a.p, a.n);
            jw_raw(&out, "\n", 1);
            jw_free(&a);
            k++;
            continue;
        fail2:
            jw_free(&c);
        fail1:
            jw_free(&a);
            goto done;
        }
    }
    if (out.oom || st->sys->write(st->sys->u, path, out.p, out.n))
        goto done;
    rc = s3_open(st, path);
done:
    {
        int k;
        for (k = 0; k < nent; k++)
            free(ids ? ids[k] : 0);
    }
    free(ids);
    jw_free(&out);
    CL_FREE(b);
    return rc;
}

/* ---- names and finding sessions ---- */

int s3_dir(const char *data_dir, const char *cwd, char *out, long cap)
{
    char name[300];
    long k = 2;
    const char *c = cwd;
    cl_copy(out, data_dir, cap);
    k = (long)strlen(out);
    if (k && out[k - 1] != ':' && out[k - 1] != '/')
        cl_cat(out, "/", cap);
    cl_cat(out, "sessions/", cap);
    /* pi: "--" + the cwd without its leading separator, / \ : as - + "--" */
    name[0] = name[1] = '-';
    k = 2;
    if (*c == '/' || *c == '\\')
        c++;
    for (; *c && k < (long)sizeof(name) - 3; c++)
        name[k++] = *c == '/' || *c == '\\' || *c == ':' ? '-' : *c;
    name[k++] = '-';
    name[k++] = '-';
    name[k] = 0;
    sess_fit(name, sess_hash(name), 30);
    cl_cat(out, name, cap);
    return (long)strlen(out) >= cap - 1 ? -1 : 0;
}

typedef struct scan {
    cl_sys *sys;
    const char *dir;
    char (*names)[40];
    int n, cap;
} scan;

static int scan_fn(void *c, const cl_dirent *e)
{
    scan *s = (scan *)c;
    long n = (long)strlen(e->name);
    if (n < 7 || strcmp(e->name + n - 6, ".jsonl") || n >= 40)
        return 0;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 32;
        char (*g)[40] = (char (*)[40])realloc(s->names, (size_t)nc * 40);
        if (!g)
            return 1;
        s->names = g;
        s->cap = nc;
    }
    cl_copy(s->names[s->n++], e->name, 40);
    return 0;
}

static void join(const char *dir, const char *name, char *out, long cap)
{
    cl_copy(out, dir, cap);
    if (out[0] && out[strlen(out) - 1] != ':' && out[strlen(out) - 1] != '/')
        cl_cat(out, "/", cap);
    cl_cat(out, name, cap);
}

/* the header line of a file: its id (and 1), 0 not a session */
static int head_of(cl_sys *sys, const char *path, char *id, long cap)
{
    char buf[4096];
    long got = 0, i;
    jv o, v;
    if (!sys->pread || sys->pread(sys->u, path, 0, buf, sizeof(buf) - 1, &got) || got <= 0)
        return 0;
    for (i = 0; i < got && buf[i] != '\n'; i++)
        ;
    if (!parse_obj(buf, i, &o) || !json_get(o, "type", &v) || !json_streq(v, "session") || !json_get(o, "id", &v) ||
        json_type(v) != J_STR)
        return 0;
    json_str(v, id, cap);
    return 1;
}

/* the files of dir, newest first (mtime, then name descending) */
static int sorted(cl_sys *sys, const char *dir, scan *s)
{
    long *mt;
    int i, j;
    memset(s, 0, sizeof(*s));
    if (sys->list(sys->u, dir, scan_fn, s))
        return -1;
    mt = (long *)malloc((size_t)(s->n ? s->n : 1) * sizeof(long));
    if (!mt) {
        free(s->names);
        return -1;
    }
    for (i = 0; i < s->n; i++) {
        char p[360];
        join(dir, s->names[i], p, sizeof(p));
        mt[i] = sys->mtime ? sys->mtime(sys->u, p) : 0;
    }
    for (i = 1; i < s->n; i++)
        for (j = i; j > 0 && (mt[j] > mt[j - 1] || (mt[j] == mt[j - 1] && strcmp(s->names[j], s->names[j - 1]) > 0));
             j--) {
            char t[40];
            long m = mt[j];
            mt[j] = mt[j - 1];
            mt[j - 1] = m;
            memcpy(t, s->names[j], 40);
            memcpy(s->names[j], s->names[j - 1], 40);
            memcpy(s->names[j - 1], t, 40);
        }
    free(mt);
    return 0;
}

int s3_recent(cl_sys *sys, const char *dir, char *out, long cap)
{
    scan s;
    int i, rc = -1;
    if (sorted(sys, dir, &s))
        return -1;
    for (i = 0; i < s.n && rc; i++) {
        char id[48];
        join(dir, s.names[i], out, cap);
        if (head_of(sys, out, id, sizeof(id)))
            rc = 0;
    }
    free(s.names);
    return rc;
}

int s3_by_id(cl_sys *sys, const char *dir, const char *id, char *out, long cap)
{
    scan s;
    int i, pass, rc = -1;
    if (sorted(sys, dir, &s))
        return -1;
    /* the exact id first, then the newest whose id starts with it (pi's resolveSessionPath) */
    for (pass = 0; pass < 2 && rc; pass++)
        for (i = 0; i < s.n && rc; i++) {
            char h[48];
            join(dir, s.names[i], out, cap);
            if (head_of(sys, out, h, sizeof(h)) &&
                (pass ? !strncmp(h, id, strlen(id)) : !strcmp(h, id)))
                rc = 0;
        }
    free(s.names);
    return rc;
}

int s3_list(cl_sys *sys, const char *dir, int max, void (*fn)(void *c, const char *path, const char *id,
                                                                const char *title), void *c)
{
    scan s;
    int i, k = 0;
    if (sorted(sys, dir, &s))
        return 0;
    for (i = 0; i < s.n && k < max; i++) {
        pi_s3 st;
        char p[360], title[100];
        join(dir, s.names[i], p, sizeof(p));
        s3_init(&st, sys, 0, 0, 0, 1);
        if (s3_open(&st, p)) {
            s3_free(&st);
            continue;
        }
        if (!s3_name(&st, title, sizeof(title))) {
            /* the first user message's text */
            int j;
            title[0] = 0;
            for (j = 0; j < st.n && !title[0]; j++)
                if (st.e[j].type == S3_MESSAGE && st.e[j].role == S3R_USER) {
                    long n;
                    const char *t = s3_text(&st, j, &n);
                    jv o, m, cc, b, x;
                    jit it;
                    if (!t || json_parse(t, n, &o) || !json_get(o, "message", &m) || !json_get(m, "content", &cc))
                        continue;
                    if (json_type(cc) == J_STR)
                        json_str(cc, title, sizeof(title));
                    else {
                        json_iter(cc, &it);
                        while (!title[0] && json_next(&it, 0, &b))
                            if (json_get(b, "type", &x) && json_streq(x, "text") && json_get(b, "text", &x))
                                json_str(x, title, sizeof(title));
                    }
                }
        }
        fn(c, p, st.id, title);
        s3_free(&st);
        k++;
    }
    free(s.names);
    return k;
}
