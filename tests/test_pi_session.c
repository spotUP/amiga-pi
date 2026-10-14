/* amiga-pi's session file on the host (plan phase E: E1 writer, E2 reader and
 * index, E6 branched sessions, E8 names, E9 timestamps).
 *
 * The expectations are pi's own: tools/pi_oracle/gen.ts runs pi's SessionManager
 * (f10993bc) over the fixtures in tests/pi/sessions/ and writes <name>.expected,
 * and over writer.scn for writer.expected; this suite does the same with
 * session3.c and compares byte for byte. Nothing here updates an expectation. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/sys_posix.h"
#include "../amiga-pi/session3.h"
#include "../amiga-pi/context.h"
#include "../amiga-pi/resume_pi.h"
#include "../amiga-pi/compact.h"
#include "../amiga-pi/cp_prompts.h"

static char root[400];
static sys_posix sp;
static cl_sys sys;
static long clock_s;

static llm_ts tick(void *u)
{
    llm_ts t;
    (void)u;
    t.sec = 1733234400L + clock_s++;
    t.ms = 123;
    return t;
}

static void path_of(char *out, const char *dir, const char *name)
{
    strcpy(out, dir);
    strcat(out, "/");
    strcat(out, name);
}

static char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long len;
    if (!f)
        return 0;
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

static void copy_file(const char *from, const char *to)
{
    long n;
    char *b = slurp(from, &n);
    FILE *f = fopen(to, "wb");
    if (b && f)
        fwrite(b, 1, (size_t)n, f);
    if (f)
        fclose(f);
    free(b);
}

/* ---- the text normaliser: the same rules as gen.ts ---- */
/* the values of "id", "parentId", "targetId", "firstKeptEntryId", "fromId" become i1, i2, ... in order
 * of first appearance (a 36-character "id" is SID); an entry's "timestamp" string is T, a 13-digit
 * number N, a parentSession path P */
static void norm_text(const char *s, jw *out)
{
    static const char *const keys[] = { "\"id\":\"", "\"parentId\":\"", "\"targetId\":\"", "\"firstKeptEntryId\":\"",
                                        "\"fromId\":\"" };
    char seen[256][48];
    int nseen = 0;
    while (*s) {
        int k, hit = 0;
        if (!strncmp(s, "\"parentSession\":\"", 17)) {
            const char *e = strchr(s + 17, '"');
            jw_rawz(out, "\"parentSession\":\"P\"");
            s = e ? e + 1 : s + strlen(s);
            continue;
        }
        if (!strncmp(s, "\"timestamp\":\"", 13)) {
            const char *e = strchr(s + 13, '"');
            jw_rawz(out, "\"timestamp\":\"T\"");
            s = e ? e + 1 : s + strlen(s);
            continue;
        }
        if (!strncmp(s, "\"timestamp\":", 12)) {
            int d = 0;
            while (s[12 + d] >= '0' && s[12 + d] <= '9')
                d++;
            if (d == 13) {
                jw_rawz(out, "\"timestamp\":N");
                s += 25;
                continue;
            }
        }
        for (k = 0; k < 5 && !hit; k++) {
            long kl = (long)strlen(keys[k]);
            if (!strncmp(s, keys[k], (size_t)kl)) {
                const char *e = strchr(s + kl, '"');
                char v[48];
                long vl = e ? (long)(e - (s + kl)) : 0;
                int j;
                if (!e || vl >= (long)sizeof(v))
                    break;
                memcpy(v, s + kl, (size_t)vl);
                v[vl] = 0;
                jw_raw(out, s, kl);
                if (k == 0 && vl == 36)
                    jw_rawz(out, "SID");
                else {
                    char lab[16];
                    for (j = 0; j < nseen && strcmp(seen[j], v); j++)
                        ;
                    if (j == nseen && nseen < 256)
                        strcpy(seen[nseen++], v);
                    sprintf(lab, "i%d", j + 1);
                    jw_rawz(out, lab);
                }
                jw_raw(out, "\"", 1);
                s = e + 1;
                hit = 1;
            }
        }
        if (!hit) {
            jw_raw(out, s, 1);
            s++;
        }
    }
}

/* want against got: a failed check naming the first line that differs */
static void same_as_pi(const char *name, const char *want, const char *got)
{
    const char *a = want ? want : "", *b = got;
    int line = 1;
    h_checks++;
    if (want && !strcmp(want, got))
        return;
    h_failures++;
    while (*a && *b) {
        const char *ea = strchr(a, '\n'), *eb = strchr(b, '\n');
        long la = ea ? (long)(ea - a) : (long)strlen(a), lb = eb ? (long)(eb - b) : (long)strlen(b);
        if (la != lb || memcmp(a, b, (size_t)la))
            break;
        a += la + (ea ? 1 : 0);
        b += lb + (eb ? 1 : 0);
        line++;
        if (!ea || !eb)
            break;
    }
    printf("  FAIL %s differs from pi at line %d:\n    pi:   %.400s\n    here: %.400s\n", name, line, a, b);
}


/* ---- compaction: prepare and compact with a model that answers "SUMMARY n" (gen.ts's fake) ---- */
typedef struct mock {
    int n;
    int branch;                 /* answer "BRANCH SUMMARY" with the usage of a first call, whatever n is */
    int fail;                   /* 0 ok, LS_ERROR, LS_LENGTH */
    jw log;                     /* the requests: "request maxTokens=.. system=.. prompt=.." lines */
} mock;

static int mock_complete(void *u, const char *prompt, long max_tokens, cp_reply *r)
{
    mock *m = (mock *)u;
    jw sys, t;
    llm_usage *us = &r->usage;
    int n;
    char num[48];
    m->n++;
    n = m->branch ? 1 : m->n;
    jw_init(&sys);
    {
        const char *const *l;
        for (l = cp_summarization_system_prompt; *l; l++)
            jw_rawz(&sys, *l);
    }
    jw_init(&t);
    jw_rawz(&m->log, "request maxTokens=");
    sprintf(num, "%ld", max_tokens);
    jw_rawz(&m->log, num);
    jw_rawz(&m->log, " system=");
    jw_str(&m->log, sys.p, sys.n);
    jw_rawz(&m->log, " prompt=");
    jw_str(&m->log, prompt, (long)strlen(prompt));
    jw_rawz(&m->log, "\n");
    if (m->branch)
        strcpy(num, "BRANCH SUMMARY");
    else
        sprintf(num, "SUMMARY %d", n);
    jw_rawz(&r->text, num);
    memset(us, 0, sizeof(*us));
    us->input = 100L * n;
    us->output = 10L * n;
    us->cache_read = n;
    us->cache_write = 2L * n;
    us->cost[0] = 500000L * n;
    us->cost[1] = 250000L * n;
    r->stop = m->fail ? m->fail : LS_STOP;
    if (m->fail == LS_ERROR)
        strcpy(r->error, "boom");
    jw_free(&sys);
    jw_free(&t);
    return 0;
}

static void json_list(jw *w, const cp_strs *l)
{
    int i;
    jw_rawz(w, "[");
    for (i = 0; i < l->n; i++) {
        if (i)
            jw_rawz(w, ",");
        jw_strz(w, l->s[i]);
    }
    jw_rawz(w, "]");
}

static void json_msgs(jw *w, const pi_ctx *c)
{
    int i;
    jw_rawz(w, "[");
    for (i = 0; i < c->n; i++) {
        if (i)
            jw_rawz(w, ",");
        jw_raw(w, c->msg[i], c->msg_n[i]);
    }
    jw_rawz(w, "]");
}

static void dump_compaction(pi_s3 *st, int leaf, const pi_ctx *ctx, jw *w)
{
    static const long keeps[] = { 20000, 5000, 5 };
    size_t k;
    int i;
    char num[48];
    for (k = 0; k < sizeof(keeps) / sizeof(keeps[0]); k++) {
        pi_prep p;
        cp_result r;
        mock m;
        char err[200];
        int rc = cp_prepare(st, leaf, keeps[k], &p);
        sprintf(num, "%ld", keeps[k]);
        jw_rawz(w, "prepare keep=");
        jw_rawz(w, num);
        jw_rawz(w, " ");
        if (rc != 1) {
            jw_rawz(w, "none\n");
            continue;
        }
        jw_rawz(w, "{\"firstKeptEntryId\":");
        jw_strz(w, s3_id(st, p.first_kept));
        jw_rawz(w, ",\"messagesToSummarize\":");
        json_msgs(w, &p.hist);
        jw_rawz(w, ",\"turnPrefixMessages\":");
        json_msgs(w, &p.prefix);
        jw_rawz(w, p.split ? ",\"isSplitTurn\":true,\"tokensBefore\":" : ",\"isSplitTurn\":false,\"tokensBefore\":");
        jw_long(w, p.tokens_before);
        if (p.prev_summary) {
            jw_rawz(w, ",\"previousSummary\":");
            jw_strz(w, p.prev_summary);
        }
        jw_rawz(w, ",\"fileOps\":{\"read\":");
        json_list(w, &p.read);
        jw_rawz(w, ",\"written\":");
        json_list(w, &p.written);
        jw_rawz(w, ",\"edited\":");
        json_list(w, &p.edited);
        jw_rawz(w, "}}\n");
        memset(&m, 0, sizeof(m));
        jw_init(&m.log);
        rc = cp_compact(&p, CP_RESERVE, 20000, "focus on tests", mock_complete, &m, &r, err, sizeof(err));
        CHECK_INT(rc, 0);
        if (!rc) {
            jw_rawz(w, "compact keep=");
            jw_rawz(w, num);
            jw_rawz(w, " {\"summary\":");
            jw_str(w, r.summary.p, r.summary.n);
            jw_rawz(w, ",\"firstKeptEntryId\":");
            jw_strz(w, s3_id(st, r.first_kept));
            jw_rawz(w, ",\"tokensBefore\":");
            jw_long(w, r.tokens_before);
            jw_rawz(w, ",\"usage\":");
            llm_w_usage(w, &r.usage);
            jw_rawz(w, ",\"details\":");
            jw_raw(w, r.details.p, r.details.n);
            jw_rawz(w, "}\n");
            jw_raw(w, m.log.p, m.log.n);
            cp_result_free(&r);
        }
        jw_free(&m.log);
        if (keeps[k] == 5) {
            /* the model fails: pi's words, nothing persisted */
            static const int stops[] = { LS_ERROR, LS_LENGTH };
            static const char *const names[] = { "error", "length" };
            for (i = 0; i < 2; i++) {
                memset(&m, 0, sizeof(m));
                jw_init(&m.log);
                m.fail = stops[i];
                rc = cp_compact(&p, CP_RESERVE, 100, 0, mock_complete, &m, &r, err, sizeof(err));
                jw_rawz(w, "failure ");
                jw_rawz(w, names[i]);
                jw_rawz(w, " ");
                jw_rawz(w, rc ? err : "none");
                jw_rawz(w, "\n");
                if (!rc)
                    cp_result_free(&r);
                jw_free(&m.log);
            }
        }
        cp_prep_free(&p);
    }
    (void)ctx;
}


/* ---- branch summaries: the same selection of targets as gen.ts's branchDump ---- */
static int on_branch(const int *path, int np, int i)
{
    int k;
    for (k = 0; k < np; k++)
        if (path[k] == i)
            return 1;
    return 0;
}

static void dump_branches(pi_s3 *st, jw *w)
{
    int targets[4], nt = 0, i, *path = 0, np, t;
    if (st->n == 0)
        return;
    np = s3_path(st, st->leaf, &path);
    targets[nt++] = 0;
    if (st->n > 1)
        targets[nt++] = st->n / 2;
    for (i = 0; i < st->n && nt < 4; i++)
        if (!on_branch(path, np, i))
            targets[nt++] = i;
    free(path);
    for (t = 0; t < nt; t++) {
        int *ent, anc, n, k, b;
        static const long budgets[] = { 0, 40 };
        mock m;
        cp_branch r;
        char err[200], num[48];
        const char *instr = t == 1 ? "focus on tests" : t == 2 ? "Only say hi" : 0;
        n = cp_collect(st, st->leaf, targets[t], &ent, &anc);
        CHECK(n >= 0);
        if (n < 0)
            continue;
        jw_rawz(w, "branch target=");
        jw_rawz(w, s3_id(st, targets[t]));
        jw_rawz(w, " ancestor=");
        jw_rawz(w, anc >= 0 ? s3_id(st, anc) : "-");
        jw_rawz(w, " entries=");
        for (k = 0; k < n; k++) {
            if (k)
                jw_rawz(w, ",");
            jw_rawz(w, s3_id(st, ent[k]));
        }
        jw_rawz(w, "\n");
        for (b = 0; b < 2; b++) {
            cp_bprep p;
            CHECK_INT(cp_branch_prepare(st, ent, n, budgets[b], &p), 0);
            sprintf(num, "%ld", budgets[b]);
            jw_rawz(w, "branchprep budget=");
            jw_rawz(w, num);
            jw_rawz(w, " {\"messages\":");
            json_msgs(w, &p.msgs);
            jw_rawz(w, ",\"fileOps\":{\"read\":");
            json_list(w, &p.read);
            jw_rawz(w, ",\"written\":");
            json_list(w, &p.written);
            jw_rawz(w, ",\"edited\":");
            json_list(w, &p.edited);
            jw_rawz(w, "},\"totalTokens\":");
            jw_long(w, p.total);
            jw_rawz(w, "}\n");
            cp_bprep_free(&p);
        }
        memset(&m, 0, sizeof(m));
        m.branch = 1;
        jw_init(&m.log);
        CHECK_INT(cp_branch_summary(st, ent, n, t == 1 ? CP_RESERVE + 40 : 0, CP_RESERVE, 20000, instr, t == 2,
                                    mock_complete, &m, &r, err, sizeof(err)),
                  0);
        jw_rawz(w, "branchsum {\"summary\":");
        jw_str(w, r.summary.p, r.summary.n);
        if (r.has_usage) {
            jw_rawz(w, ",\"usage\":");
            llm_w_usage(w, &r.usage);
            /* r.details is {"readFiles":[..],"modifiedFiles":[..]}: its members join the result's */
            jw_rawz(w, ",");
            jw_raw(w, r.details.p + 1, r.details.n - 2);
        }
        jw_rawz(w, "}\n");
        jw_raw(w, m.log.p, m.log.n);
        jw_free(&m.log);
        cp_branch_free(&r);
        free(ent);
    }
}

/* ---- the dump of an opened session: gen.ts's dump(), line for line ---- */
typedef struct idmap {
    char id[256][48];
    int n, on;
} idmap;

static const char *mp(idmap *m, const char *id, char *buf)
{
    int i;
    if (!id)
        return "-";
    if (!m->on)
        return id;
    for (i = 0; i < m->n && strcmp(m->id[i], id); i++)
        ;
    if (i == m->n && m->n < 256)
        strcpy(m->id[m->n++], id);
    sprintf(buf, "i%d", i + 1);
    return buf;
}

/* a string member of an entry's line, "" absent */
static void member(pi_s3 *st, int i, const char *key, char *out, long cap)
{
    long n;
    const char *t = s3_text(st, i, &n);
    jv o, v;
    out[0] = 0;
    if (t && !json_parse(t, n, &o) && json_get(o, key, &v) && json_type(v) == J_STR)
        json_str(v, out, cap);
}

static void dump_session(pi_s3 *st, const char *path, long size_before, int norm, int skip_branch, jw *w)
{
    idmap *m = (idmap *)calloc(1, sizeof(*m));
    char b1[64], b2[64], b3[64], tmp[300], type[64];
    int i, *p, k;
    jv h, v;
    struct stat sb;
    char *file;
    long n;
    m->on = norm;
    jw_rawz(w, "open ok\n");
    json_parse(st->head, (long)strlen(st->head), &h);
    sprintf(tmp, "version %ld\n", json_get(h, "version", &v) ? json_long(v, 0) : 0);
    jw_rawz(w, tmp);
    jw_rawz(w, "cwd ");
    if (json_get(h, "cwd", &v))
        json_str(v, tmp, sizeof(tmp));
    else
        tmp[0] = 0;
    jw_rawz(w, tmp);
    jw_rawz(w, "\nparent ");
    if (json_get(h, "parentSession", &v) && json_str(v, tmp, sizeof(tmp)) >= 0)
        jw_rawz(w, tmp);
    else
        jw_rawz(w, "-");
    sprintf(tmp, "\nentries %d\n", st->n);
    jw_rawz(w, tmp);
    for (i = 0; i < st->n; i++) {
        char id[48], pid[48], x[48];
        long tn;
        const char *t = s3_text(st, i, &tn);
        jv o;
        member(st, i, "id", id, sizeof(id));
        jw_rawz(w, mp(m, id, b1));
        jw_rawz(w, " ");
        pid[0] = 0;
        if (t && !json_parse(t, tn, &o) && json_get(o, "parentId", &v) && json_type(v) == J_STR)
            json_str(v, pid, sizeof(pid));
        jw_rawz(w, pid[0] ? mp(m, pid, b2) : "-");
        member(st, i, "type", type, sizeof(type));
        jw_rawz(w, " ");
        jw_rawz(w, type);
        if (!strcmp(type, "label") || !strcmp(type, "context_edit")) {
            member(st, i, "targetId", x, sizeof(x));
            jw_rawz(w, " target=");
            jw_rawz(w, mp(m, x, b3));
        }
        if (!strcmp(type, "compaction")) {
            member(st, i, "firstKeptEntryId", x, sizeof(x));
            jw_rawz(w, " kept=");
            jw_rawz(w, mp(m, x, b3));
        }
        if (!strcmp(type, "branch_summary")) {
            member(st, i, "fromId", x, sizeof(x));
            jw_rawz(w, " from=");
            jw_rawz(w, mp(m, x, b3));
        }
        jw_rawz(w, "\n");
    }
    jw_rawz(w, "leaf ");
    jw_rawz(w, st->leaf >= 0 ? mp(m, s3_id(st, st->leaf), b1) : "-");
    jw_rawz(w, "\n");
    for (i = 0; i < st->n; i++)
        if (s3_label_of(st, i)) {
            jw_rawz(w, "label ");
            jw_rawz(w, mp(m, s3_id(st, i), b1));
            jw_rawz(w, " ");
            jw_rawz(w, s3_label_of(st, i));
            jw_rawz(w, "\n");
        }
    jw_rawz(w, "name ");
    jw_rawz(w, s3_name(st, tmp, sizeof(tmp)) ? tmp : "-");
    jw_rawz(w, "\nbranch ");
    k = s3_path(st, st->leaf, &p);
    for (i = 0; i < k; i++) {
        if (i)
            jw_rawz(w, ",");
        jw_rawz(w, mp(m, s3_id(st, p[i]), b1));
    }
    free(p);
    jw_rawz(w, "\n");
    if (!norm) {
        /* what the model sees (E3): the context, convertToLlm, the replayed system message and its text */
        pi_ctx c;
        jw sysm;
        ctx_init(&c);
        CHECK_INT(ctx_build(st, st->leaf, &c), 0);
        jw_rawz(w, "context ");
        ctx_json(&c, w);
        jw_rawz(w, "\nllm ");
        ctx_to_llm(&c, w);
        jw_rawz(w, "\nsystem ");
        jw_init(&sysm);
        if (ctx_system(&c, &sysm) == 1) {
            jw_rawz(w, sysm.p);
            jw_rawz(w, "\nsystemtext ");
            {
                jw txt;
                jw_init(&txt);
                ctx_system_text(sysm.p, sysm.n, &txt);
                jw_str(w, txt.p ? txt.p : "", txt.n);
                jw_free(&txt);
            }
        } else
            jw_rawz(w, "-\nsystemtext -");
        jw_rawz(w, "\n");
        jw_free(&sysm);
        dump_compaction(st, st->leaf, &c, w);
        if (!skip_branch)
            dump_branches(st, w);
        {
            int ti;
            char num[24];
            jw_rawz(w, "tokens [");
            for (ti = 0; ti < c.n; ti++) {
                if (ti)
                    jw_rawz(w, ",");
                sprintf(num, "%ld", cp_estimate(c.msg[ti], c.msg_n[ti]));
                jw_rawz(w, num);
            }
            jw_rawz(w, "]\n");
        }
        ctx_free(&c);
    }
    free(m);
    (void)sb;
    (void)path;
    (void)size_before;
    (void)file;
    (void)n;
}

static void dump_file(const char *path, long size_before, int norm, jw *w)
{
    struct stat sb;
    long n = 0;
    char *file = slurp(path, &n);
    jw_rawz(w, stat(path, &sb) == 0 && sb.st_size == size_before ? "size same\nfile\n" : "size changed\nfile\n");
    if (file) {
        if (norm)
            norm_text(file, w);
        else
            jw_rawz(w, file);
    }
    free(file);
}

static void test_fixture(const char *name, int norm)
{
    char src[600], dst[600], ex[600], tn[100];
    pi_s3 st;
    jw w;
    struct stat sb;
    long size;
    char *want;
    int rc;
    strcpy(tn, name);
    strcat(tn, ".jsonl");
    strcpy(src, PI_TESTS "/pi/sessions/");
    strcat(src, tn);
    path_of(dst, root, tn);
    copy_file(src, dst);
    stat(dst, &sb);
    size = (long)sb.st_size;
    s3_init(&st, &sys, tick, 0, 1, 7);
    rc = s3_open(&st, dst);
    jw_init(&w);
    if (rc)
        jw_rawz(&w, "open error\n");
    else
        dump_session(&st, dst, size, norm, !strcmp(name, "bad_content"), &w);
    dump_file(dst, size, norm, &w);
    strcpy(ex, PI_TESTS "/pi/sessions/");
    strcat(ex, name);
    strcat(ex, ".expected");
    want = slurp(ex, 0);
    same_as_pi(name, want, w.p);
    free(want);
    jw_free(&w);
    s3_free(&st);
}


/* ---- the writer: writer.scn applied through session3.c, the file compared with pi's ---- */
static char *raw_of(jv v)
{
    char *z = (char *)malloc((size_t)v.n + 1);
    if (z) {
        memcpy(z, v.p, (size_t)v.n);
        z[v.n] = 0;
    }
    return z;
}

static char *str_of(jv o, const char *key)
{
    jv v;
    long n;
    return json_get(o, key, &v) && json_type(v) == J_STR ? json_strdup(v, &n) : 0;
}

/* one scenario line: its entry's index, -2 for a leaf move, -1 failed */
static int apply(pi_s3 *st, const char *line, long n)
{
    jv o, v;
    char *op, *a, *b, *c, *d;
    int r = -1, ref;
    llm_usage u;
    if (json_parse(line, n, &o))
        return -1;
    op = str_of(o, "op");
    if (!op)
        return -1;
    if (!strcmp(op, "message") && json_get(o, "message", &v))
        r = s3_message(st, v.p, v.n);
    else if (!strcmp(op, "thinking") && (a = str_of(o, "level"))) {
        r = s3_thinking(st, a);
        free(a);
    } else if (!strcmp(op, "model")) {
        a = str_of(o, "provider");
        b = str_of(o, "modelId");
        r = a && b ? s3_model(st, a, b) : -1;
        free(a);
        free(b);
    } else if (!strcmp(op, "usage")) {
        a = str_of(o, "kind");
        b = str_of(o, "provider");
        c = str_of(o, "model");
        d = str_of(o, "note");
        memset(&u, 0, sizeof(u));
        if (json_get(o, "usage", &v))
            llm_r_usage(v, &u);
        r = a && b && c ? s3_usage(st, a, b, c, &u, d) : -1;
        free(a);
        free(b);
        free(c);
        free(d);
    } else if (!strcmp(op, "compaction")) {
        long tb = json_get(o, "tokens", &v) ? json_long(v, 0) : 0;
        int hook = json_get(o, "fromHook", &v) ? (json_type(v) == J_TRUE) : -1;
        a = str_of(o, "summary");
        b = json_get(o, "details", &v) ? raw_of(v) : 0;
        c = json_get(o, "usage", &v) ? raw_of(v) : 0;
        ref = json_get(o, "first", &v) && json_type(v) == J_NUM ? (int)json_long(v, 0) - 1 : -1;
        {
            /* pi's appendCompaction: the system message replayed from the projection */
            pi_ctx cx;
            jw sysj;
            ctx_init(&cx);
            jw_init(&sysj);
            ctx_build(st, st->leaf, &cx);
            r = a ? s3_compaction(st, a, ref, tb, b, c, hook, ctx_system(&cx, &sysj) == 1 ? sysj.p : 0) : -1;
            jw_free(&sysj);
            ctx_free(&cx);
        }
        free(a);
        free(b);
        free(c);
    } else if (!strcmp(op, "custom")) {
        a = str_of(o, "customType");
        b = json_get(o, "data", &v) ? raw_of(v) : 0;
        r = a ? s3_custom(st, a, b) : -1;
        free(a);
        free(b);
    } else if (!strcmp(op, "custom_message")) {
        a = str_of(o, "customType");
        b = json_get(o, "content", &v) ? raw_of(v) : 0;
        c = json_get(o, "details", &v) ? raw_of(v) : 0;
        r = a && b ? s3_custom_message(st, a, b, json_get(o, "display", &v) && json_type(v) == J_TRUE, c) : -1;
        free(a);
        free(b);
        free(c);
    } else if (!strcmp(op, "context_edit")) {
        ref = json_get(o, "target", &v) ? (int)json_long(v, 0) - 1 : -1;
        b = json_get(o, "replacement", &v) && json_type(v) != J_NULL ? raw_of(v) : 0;
        r = s3_context_edit(st, ref, b);
        free(b);
    } else if (!strcmp(op, "label")) {
        ref = json_get(o, "target", &v) ? (int)json_long(v, 0) - 1 : -1;
        a = str_of(o, "label");
        r = s3_label(st, ref, a);
        free(a);
    } else if (!strcmp(op, "session_info")) {
        a = str_of(o, "name");
        r = a ? s3_session_info(st, a) : -1;
        free(a);
    } else if (!strcmp(op, "branch")) {
        ref = json_get(o, "to", &v) && json_type(v) == J_NUM ? (int)json_long(v, 0) - 1 : -1;
        s3_set_leaf(st, ref);
        r = -2;
    } else if (!strcmp(op, "branched")) {
        ref = json_get(o, "leaf", &v) ? (int)json_long(v, 0) - 1 : -1;
        r = s3_branched(st, ref) ? -1 : -2;
    } else if (!strcmp(op, "branch_summary")) {
        ref = json_get(o, "to", &v) && json_type(v) == J_NUM ? (int)json_long(v, 0) - 1 : -1;
        a = str_of(o, "summary");
        b = json_get(o, "details", &v) ? raw_of(v) : 0;
        c = json_get(o, "usage", &v) ? raw_of(v) : 0;
        r = a ? s3_branch_summary(st, ref, a, b, c, json_get(o, "fromHook", &v) ? (json_type(v) == J_TRUE) : -1) : -1;
        free(a);
        free(b);
        free(c);
    }
    free(op);
    return r;
}

static void run_scn(const char *name)
{
    char scn[600], dir[600], *text, *want, *p, *eol;
    pi_s3 st;
    long n, at;
    int line = 0, ok = 1;
    jw w;
    strcpy(scn, PI_TESTS "/pi/sessions/");
    strcat(scn, name);
    strcat(scn, ".scn");
    text = slurp(scn, &n);
    path_of(dir, root, name);
    s3_init(&st, &sys, tick, 0, 1, 99);
    CHECK_INT(s3_new(&st, dir, "/work/dir", 0, 0, 1), 0);
    CHECK(text != 0);
    for (p = text; text && *p; p = eol + 1) {
        int r;
        eol = strchr(p, '\n');
        if (!eol)
            break;
        r = apply(&st, p, (long)(eol - p));
        line++;
        if (r == -1) {
            ok = 0;
            printf("  FAIL %s.scn line %d: %s\n", name, line, st.err);
        }
        /* pi creates the file when the first user or assistant message exists, not before */
        if (line == 3 && !strcmp(name, "writer"))
            CHECK_INT(st.flushed, 0);
        if (line == 4 && !strcmp(name, "writer")) {
            CHECK_INT(st.flushed, 1);
            CHECK_INT(st.appends, 5);   /* the header and the four entries so far, written at once */
        }
    }
    CHECK(ok);
    CHECK(st.file[0] != 0);
    jw_init(&w);
    {
        char *f = slurp(st.file, &at);
        if (f)
            norm_text(f, &w);
        free(f);
    }
    strcpy(scn, PI_TESTS "/pi/sessions/");
    strcat(scn, name);
    strcat(scn, ".expected");
    want = slurp(scn, 0);
    same_as_pi(name, want, w.p);
    free(want);
    jw_free(&w);
    free(text);
    {
        /* PI_SESSION_OUT=<dir>: the file kept for tools/pi_oracle/crosscheck.sh (pi's own SessionManager opens it) */
        const char *out = getenv("PI_SESSION_OUT");
        if (out && *out) {
            char to[700];
            strcpy(to, out);
            strcat(to, "/");
            strcat(to, name);
            strcat(to, ".jsonl");
            copy_file(st.file, to);
        }
    }
    s3_free(&st);
}

/* ---- pi's refusals (appendContextEdit, appendLabelChange) and "wx" ---- */
static const char *const USER_MSG = "{\"role\":\"user\",\"content\":\"hi\",\"timestamp\":1}";
static const char *const ASST_MSG = "{\"role\":\"assistant\",\"content\":[],\"timestamp\":2}";

static void test_refusals(void)
{
    pi_s3 st;
    char dir[600], pre[600];
    int u, a, m, ce;
    long before;
    path_of(dir, root, "refuse");
    s3_init(&st, &sys, tick, 0, 1, 5);
    CHECK_INT(s3_new(&st, dir, "/c", 0, 0, 1), 0);
    u = s3_message(&st, USER_MSG, (long)strlen(USER_MSG));
    a = s3_message(&st, ASST_MSG, (long)strlen(ASST_MSG));
    m = s3_model(&st, "p", "m");
    CHECK_INT(u, 0);
    CHECK_INT(a, 1);
    before = st.n;
    /* an entry that carries no model content cannot be edited */
    CHECK_INT(s3_context_edit(&st, m, "{\"content\":\"x\"}"), -1);
    CHECK(strstr(st.err, "does not contribute editable model content") != 0);
    /* a replacement must be null or hold string/array content */
    CHECK_INT(s3_context_edit(&st, u, "{\"content\":5}"), -1);
    CHECK(strstr(st.err, "must be null or contain string/array content") != 0);
    CHECK_INT(s3_context_edit(&st, u, "{\"text\":\"x\"}"), -1);
    CHECK_INT(s3_context_edit(&st, 77, "{\"content\":\"x\"}"), -1);
    CHECK_INT(s3_label(&st, 77, "x"), -1);
    /* an entry off the active branch cannot be edited */
    s3_set_leaf(&st, u);
    CHECK_INT(s3_context_edit(&st, a, "{\"content\":\"x\"}"), -1);
    CHECK(strstr(st.err, "is not on the active branch") != 0);
    CHECK_INT(st.n, before);
    /* none of that reached the file: it still holds the header and the four... three entries */
    {
        long n;
        char *f = slurp(st.file, &n);
        int lines = 0;
        char *q;
        for (q = f; f && *q; q++)
            lines += *q == '\n';
        CHECK_INT(lines, 4);
        free(f);
    }
    ce = s3_context_edit(&st, u, "{\"content\":\"edited\"}");
    CHECK(ce > 0);
    s3_free(&st);

    /* pi writes the new file with "wx": a file already there is not overwritten */
    path_of(pre, root, "wx");
    s3_init(&st, &sys, tick, 0, 1, 6);
    CHECK_INT(s3_new(&st, pre, "/c", 0, 0, 1), 0);
    {
        FILE *f;
        mkdir(pre, 0777);
        f = fopen(st.file, "wb");
        CHECK(f != 0);
        if (f) {
            fputs("precious\n", f);
            fclose(f);
        }
    }
    CHECK_INT(s3_message(&st, USER_MSG, (long)strlen(USER_MSG)), -1);
    {
        char *f = slurp(st.file, 0);
        CHECK_STR(f ? f : "", "precious\n");
        free(f);
    }
    s3_free(&st);

    /* --no-session: nothing is written, the tree still works */
    path_of(pre, root, "none");
    s3_init(&st, &sys, tick, 0, 1, 8);
    CHECK_INT(s3_new(&st, pre, "/c", 0, 0, 0), 0);
    CHECK_INT(s3_message(&st, USER_MSG, (long)strlen(USER_MSG)), 0);
    CHECK_INT(sys.kind(sys.u, pre), 0);
    CHECK_INT(st.appends, 0);
    s3_free(&st);
}

/* ---- E2: a 2,000-entry file keeps only the active branch's lines in RAM ---- */
static long held(const pi_s3 *st, int *entries)
{
    long b = 0;
    int i;
    *entries = 0;
    for (i = 0; i < st->n; i++)
        if (st->e[i].text) {
            b += st->e[i].len;
            (*entries)++;
        }
    return b;
}

/* Q7: combineUsage with usage.reasoning -- the sums below are what pi 1.1.0's combineUsage (usage-totals.ts, run on the same
 * operands) gives */
static void test_usage_sum(void)
{
    static const char *const first[3] = {
        "{\"input\":1,\"output\":5,\"cacheRead\":2,\"cacheWrite\":3,\"cacheWrite1h\":1,\"reasoning\":2,\"totalTokens\":11,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}",
        "{\"input\":1,\"output\":5,\"cacheRead\":0,\"cacheWrite\":0,\"reasoning\":2,\"totalTokens\":6,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}",
        "{\"input\":1,\"output\":5,\"cacheRead\":0,\"cacheWrite\":0,\"totalTokens\":6,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}"
    };
    static const char *const second =
        "{\"input\":4,\"output\":6,\"cacheRead\":0,\"cacheWrite\":0,\"reasoning\":3,\"totalTokens\":10,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}";
    static const char *const want[3] = {
        "{\"input\":5,\"output\":11,\"cacheRead\":2,\"cacheWrite\":3,\"cacheWrite1h\":1,\"reasoning\":5,\"totalTokens\":21,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}",
        "{\"input\":5,\"output\":11,\"cacheRead\":0,\"cacheWrite\":0,\"reasoning\":5,\"totalTokens\":16,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}",
        "{\"input\":5,\"output\":11,\"cacheRead\":0,\"cacheWrite\":0,\"reasoning\":3,\"totalTokens\":16,"
        "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}"
    };
    int i;
    for (i = 0; i < 3; i++) {
        jv v;
        llm_usage a, b;
        jw o;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        CHECK(json_parse(first[i], (long)strlen(first[i]), &v) == 0);
        llm_r_usage(v, &a);
        CHECK(json_parse(second, (long)strlen(second), &v) == 0);
        llm_r_usage(v, &b);
        cp_usage_add(&a, &b);
        jw_init(&o);
        llm_w_usage(&o, &a);
        CHECK_STR(o.p ? o.p : "", want[i]);
        jw_free(&o);
    }
}

/* Q6a: where the last answer stands for the overflow cases of _checkCompaction (assistantIsProjected,
 * hasPostAssistantContextEdit, assistantRetainedForExplicitRecovery) and the decision on top of it */
static void test_overflow_state(void)
{
    pi_s3 st;
    char dir[600];
    int u, a, k;
    cp_asst as;
    long n = (long)strlen(ASST_MSG);
    path_of(dir, root, "ovstate");
    s3_init(&st, &sys, tick, 0, 1, 15);
    CHECK_INT(s3_new(&st, dir, "/c", 0, 0, 1), 0);
    u = s3_message(&st, USER_MSG, (long)strlen(USER_MSG));
    a = s3_message(&st, ASST_MSG, n);
    CHECK(u >= 0 && a > u);
    CHECK_INT(cp_assistant_state(&st, st.leaf, ASST_MSG, n, &as), 0);
    CHECK(as.projected && !as.after_edit && as.retained);
    /* an answer the session does not hold: pi has no persisted id, the checks stand */
    CHECK_INT(cp_assistant_state(&st, st.leaf, "{\"role\":\"assistant\"}", 20, &as), 0);
    CHECK(as.projected && !as.after_edit && as.retained);
    /* an edit of another entry after it: the usage no longer matches the projection, the answer is still retained */
    CHECK(s3_context_edit(&st, u, "{\"content\":\"edited\"}") > 0);
    CHECK_INT(cp_assistant_state(&st, st.leaf, ASST_MSG, n, &as), 0);
    CHECK(as.projected && as.after_edit && as.retained);
    /* its own replacement: still in the context, still retained */
    CHECK(s3_context_edit(&st, a, "{\"content\":\"short\"}") > 0);
    CHECK_INT(cp_assistant_state(&st, st.leaf, ASST_MSG, n, &as), 0);
    CHECK(as.projected && as.after_edit && as.retained);
    /* omitted (the recovery's context_edit without a replacement): not projected, not retained */
    CHECK(s3_context_edit(&st, a, 0) > 0);
    CHECK_INT(cp_assistant_state(&st, st.leaf, ASST_MSG, n, &as), 0);
    CHECK(!as.projected && as.after_edit && !as.retained);
    s3_free(&st);

    /* a compaction after it: no longer retained for the explicit case */
    path_of(dir, root, "ovstate2");
    s3_init(&st, &sys, tick, 0, 1, 16);
    CHECK_INT(s3_new(&st, dir, "/c", 0, 0, 1), 0);
    s3_message(&st, USER_MSG, (long)strlen(USER_MSG));
    a = s3_message(&st, ASST_MSG, n);
    CHECK(s3_compaction(&st, "summary", -1, 10, 0, 0, -1, 0) > 0);
    CHECK_INT(cp_assistant_state(&st, st.leaf, ASST_MSG, n, &as), 0);
    CHECK(!as.projected && !as.after_edit && !as.retained);
    s3_free(&st);

    /* the decision: pi's table. as = {projected, after_edit, retained} */
    for (k = 0; k < 8; k++) {
        cp_asst x;
        x.projected = k & 1;
        x.after_edit = (k >> 1) & 1;
        x.retained = (k >> 2) & 1;
        /* an overflow error: retained is the condition; or the usage matching the projection */
        CHECK_INT(cp_overflow(LS_ERROR, 1, 0, 0, 1000, 0, &x) & CP_OV_DEEP, (x.retained || (x.projected && !x.after_edit)) ? CP_OV_DEEP : 0);
        /* a successful answer over the window: the projection decides */
        CHECK_INT(cp_overflow(LS_STOP, 0, 1500, 10, 1000, 0, &x) & CP_OV_DEEP, (x.projected && !x.after_edit) ? CP_OV_DEEP : 0);
        /* a length stop below the output limit: projected only */
        CHECK_INT(cp_overflow(LS_LENGTH, 0, 10, 5, 1000, 100, &x), x.projected ? CP_OV_LENGTH : 0);
        /* any other error is neither */
        CHECK_INT(cp_overflow(LS_ERROR, 0, 0, 0, 1000, 0, &x), 0);
    }
}

static void test_big(void)
{
    char path[600];
    FILE *f;
    int i;
    pi_s3 st;
    long file_bytes, bytes;
    int ents;
    long n;
    const char *t;
    path_of(path, root, "big.jsonl");
    f = fopen(path, "wb");
    CHECK(f != 0);
    if (!f)
        return;
    fputs("{\"type\":\"session\",\"version\":3,\"id\":\"0193a1b2-c3d4-7e5f-8a6b-7c8d9e0f1a2b\","
          "\"timestamp\":\"2024-12-03T14:00:00.000Z\",\"cwd\":\"/big\"}\n", f);
    /* entries 1..1000 in a chain, 1001..2000 a second branch from entry 500: 1,500 on the active branch */
    for (i = 1; i <= 2000; i++) {
        int parent = i == 1 ? 0 : i == 1001 ? 500 : i - 1;
        char pid[24];
        if (parent)
            sprintf(pid, "\"%08x\"", parent);
        else
            strcpy(pid, "null");
        fprintf(f, "{\"type\":\"message\",\"id\":\"%08x\",\"parentId\":%s,\"timestamp\":\"2024-12-03T14:00:00.000Z\","
                   "\"message\":{\"role\":\"%s\",\"content\":\"line %d of the big session, padded: %.60s\","
                   "\"timestamp\":%d}}\n",
                i, pid, i % 2 ? "user" : "assistant", i,
                "................................................................", i);
    }
    fclose(f);
    {
        struct stat sb;
        stat(path, &sb);
        file_bytes = (long)sb.st_size;
    }
    s3_init(&st, &sys, tick, 0, 1, 3);
    CHECK_INT(s3_open(&st, path), 0);
    CHECK_INT(st.n, 2000);
    CHECK_INT(st.leaf, 1999);
    bytes = held(&st, &ents);
    CHECK_INT(ents, 1500);
    CHECK(bytes < file_bytes * 80 / 100);
    /* an off-branch line is read again when asked for, and the count of such reads is exact */
    CHECK_INT(st.reads, 1500);          /* open read the active branch's lines, no others */
    t = s3_text(&st, 700, &n);
    CHECK(t != 0 && strstr(t, "line 701 of the big session") != 0);
    CHECK_INT(st.reads, 1501);
    /* the leaf moved there: the lines of the branch left are dropped from RAM, the new one's (1..500
     * and the one just read) stay and the rest are read when something asks */
    s3_set_leaf(&st, 999);
    bytes = held(&st, &ents);
    CHECK_INT(ents, 501);
    CHECK(st.e[1500].text == 0 && st.e[1999].text == 0);
    (void)bytes;
    s3_free(&st);
}

/* ---- E8, E9: the names, and the clock ---- */
static int longest(cl_sys *sy, const char *dir, int depth);
typedef struct walk {
    cl_sys *sy;
    const char *dir;
    int longest, depth;
} walk;

static int walk_fn(void *c, const cl_dirent *e)
{
    walk *w = (walk *)c;
    int l = (int)strlen(e->name);
    if (l > w->longest)
        w->longest = l;
    if (e->dir && w->depth < 8) {
        char p[700];
        strcpy(p, w->dir);
        strcat(p, "/");
        strcat(p, e->name);
        l = longest(w->sy, p, w->depth + 1);
        if (l > w->longest)
            w->longest = l;
    }
    return 0;
}

static int longest(cl_sys *sy, const char *dir, int depth)
{
    walk w;
    w.sy = sy;
    w.dir = dir;
    w.longest = 0;
    w.depth = depth;
    sy->list(sy->u, dir, walk_fn, &w);
    return w.longest;
}

static void test_names(void)
{
    char d1[300], d2[300], d3[300], base[600], dir[600], out[100];
    pi_s3 st;
    int k, i;
    const char *name;
    llm_ts t;
    /* pi's directory: "--" + the cwd without its leading separator, / \ : as -, "--" */
    CHECK_INT(s3_dir("UP-Term:var/amiga-pi", "/work/vtcon", d1, sizeof(d1)), 0);
    CHECK_STR(d1, "UP-Term:var/amiga-pi/sessions/--work-vtcon--");
    CHECK_INT(s3_dir("UP-Term:var/amiga-pi", "Work:Projects/amiga-pi", d1, sizeof(d1)), 0);
    CHECK_STR(d1, "UP-Term:var/amiga-pi/sessions/--Work-Projects-amiga-pi--");
    /* a long one is cut to 30 characters by C:Claude's rule, two long ones differ */
    CHECK_INT(s3_dir("D", "/work/vtcon/some/very/long/path/to/a/project", d1, sizeof(d1)), 0);
    CHECK_INT(s3_dir("D", "/work/vtcon/some/very/long/path/to/another/project", d2, sizeof(d2)), 0);
    CHECK_INT(s3_dir("D", "/work/vtcon/some/very/long/path/to/a/project", d3, sizeof(d3)), 0);
    CHECK_INT((int)strlen(d1) - (int)strlen("D/sessions/"), 30);
    CHECK_INT((int)strlen(d2) - (int)strlen("D/sessions/"), 30);
    CHECK(strcmp(d1, d2) != 0);
    CHECK_STR(d1, d3);
    /* the file: <YYYYMMDDTHHMMSS>_<8 hex>.jsonl is 30 characters and every name created is at most 30 */
    path_of(base, root, "names");
    CHECK_INT(s3_dir(base, "/work/vtcon/some/very/long/path/to/a/project", dir, sizeof(dir)), 0);
    s3_init(&st, &sys, tick, 0, 1, 11);
    CHECK_INT(s3_new(&st, dir, "/work/vtcon/some/very/long/path/to/a/project", 0, 0, 1), 0);
    CHECK_INT(s3_message(&st, USER_MSG, (long)strlen(USER_MSG)), 0);
    name = strrchr(st.file, '/') + 1;
    CHECK_INT((int)strlen(name), 30);
    CHECK(name[8] == 'T' && name[15] == '_' && !strcmp(name + 24, ".jsonl"));
    for (k = 0; k < 8; k++)
        CHECK(name[k] >= '0' && name[k] <= '9');
    for (k = 16; k < 24; k++)
        CHECK((name[k] >= '0' && name[k] <= '9') || (name[k] >= 'a' && name[k] <= 'f'));
    CHECK_INT(s3_branched(&st, 0), 0);
    CHECK_INT(s3_message(&st, ASST_MSG, (long)strlen(ASST_MSG)), 1);
    CHECK(longest(&sys, base, 0) <= 30);
    CHECK(longest(&sys, base, 0) >= 30);
    s3_free(&st);

    /* the clock (E9): ISO 8601 with milliseconds; Z on a UTC clock, no offset on the Amiga's local one */
    t.sec = 1733234400L;
    t.ms = 5;
    s3_iso(t, 1, out);
    CHECK_STR(out, "2024-12-03T14:00:00.005Z");
    s3_iso(t, 0, out);
    CHECK_STR(out, "2024-12-03T14:00:00.005");
    {
        llm_ts b;
        CHECK_INT(s3_iso_read("2024-12-03T14:00:00.005Z", &b), 0);
        CHECK(b.sec == t.sec && b.ms == 5);
        CHECK_INT(s3_iso_read("2024-12-03T14:00:00", &b), 0);
        CHECK(b.sec == t.sec && b.ms == 0);
        CHECK_INT(s3_iso_read("yesterday", &b), -1);
    }
    for (i = 0; i < 3; i++) {
        t.sec = 1733234400L + 86400L * 366 * i;
        s3_iso(t, 0, out);
        {
            llm_ts b;
            CHECK_INT(s3_iso_read(out, &b), 0);
            CHECK(b.sec == t.sec);
        }
    }
    /* a session made on the Amiga's clock carries no Z in its header or its entries */
    path_of(base, root, "local");
    s3_init(&st, &sys, tick, 0, 0, 12);
    CHECK_INT(s3_new(&st, base, "/c", 0, 0, 1), 0);
    CHECK_INT(s3_message(&st, USER_MSG, (long)strlen(USER_MSG)), 0);
    {
        char *f = slurp(st.file, 0);
        CHECK(f && strstr(f, "Z\"") == 0);
        CHECK(f && strstr(f, "\"timestamp\":\"2024-12-03T14:00:") != 0);
        free(f);
    }
    s3_free(&st);
}

/* ---- finding sessions: --continue, --session <id prefix>, --resume's list ---- */
typedef struct lst {
    int n;
    char path[4][600], id[4][48], title[4][100];
} lst;

static void list_fn(void *c, const char *path, const char *id, const char *title)
{
    lst *l = (lst *)c;
    if (l->n < 4) {
        strcpy(l->path[l->n], path);
        strcpy(l->id[l->n], id);
        strcpy(l->title[l->n], title);
    }
    l->n++;
}

static void test_find(void)
{
    char dir[600], found[600], first[600];
    pi_s3 st;
    int k;
    lst l;
    char ids[3][48];
    static const char *const texts[3] = { "first question", "second question", "third question" };
    path_of(dir, root, "find");
    memset(&l, 0, sizeof(l));
    CHECK_INT(s3_recent(&sys, dir, found, sizeof(found)), -1);
    for (k = 0; k < 3; k++) {
        char msg[200];
        s3_init(&st, &sys, tick, 0, 1, (unsigned long)(100 + k));
        CHECK_INT(s3_new(&st, dir, "/c", 0, 0, 1), 0);
        sprintf(msg, "{\"role\":\"user\",\"content\":\"%s\",\"timestamp\":1}", texts[k]);
        CHECK_INT(s3_message(&st, msg, (long)strlen(msg)), 0);
        strcpy(ids[k], st.id);
        if (k == 0)
            strcpy(first, st.file);
        if (k == 1)
            CHECK_INT(s3_session_info(&st, "named one"), 1);
        s3_free(&st);
    }
    /* a file that is not a session is never picked, whatever its time */
    {
        char junk[700];
        FILE *f;
        strcpy(junk, dir);
        strcat(junk, "/99999999T999999_zzzzzzzz.jsonl");
        f = fopen(junk, "wb");
        if (f) {
            fputs("{\"type\":\"message\"}\n", f);
            fclose(f);
        }
    }
    CHECK_INT(s3_recent(&sys, dir, found, sizeof(found)), 0);
    {
        pi_s3 chk;
        s3_init(&chk, &sys, tick, 0, 1, 1);
        CHECK_INT(s3_open(&chk, found), 0);
        CHECK_STR(chk.id, ids[2]);
        s3_free(&chk);
    }
    /* by id: exact, or the newest whose id starts with the prefix */
    CHECK_INT(s3_by_id(&sys, dir, ids[0], found, sizeof(found)), 0);
    CHECK_STR(found, first);
    ids[1][8] = 0;
    CHECK_INT(s3_by_id(&sys, dir, ids[1], found, sizeof(found)), 0);
    CHECK(strcmp(found, first) != 0);
    CHECK_INT(s3_by_id(&sys, dir, "nosuchid", found, sizeof(found)), -1);
    /* the list, newest first, titled by the name or the first user message */
    CHECK_INT(s3_list(&sys, dir, 10, list_fn, &l), 3);
    CHECK_STR(l.title[0], "third question");
    CHECK_STR(l.title[1], "named one");
    CHECK_STR(l.title[2], "first question");
    CHECK_INT(s3_list(&sys, dir, 2, list_fn, &l), 2);
}

/* U2: a volume root as the working directory ("BOOTX:", "DH0:"): the sessions run there are listed again by -r */
static void test_volume_root_list(void)
{
    static const char *const cwds[] = { "BOOTX:", "DH0:", "Work:dir", "/" };
    char data[600];
    int i;
    path_of(data, root, "volroot");
    for (i = 0; i < 4; i++) {
        char dir[700], dir2[700];
        pi_s3 st;
        lst l;
        memset(&l, 0, sizeof(l));
        CHECK_INT(s3_dir(data, cwds[i], dir, sizeof(dir)), 0);
        s3_init(&st, &sys, tick, 0, 1, (unsigned long)(300 + i));
        CHECK_INT(s3_new(&st, dir, cwds[i], 0, 0, 1), 0);
        CHECK_INT(s3_message(&st, USER_MSG, (long)strlen(USER_MSG)), 0);
        s3_free(&st);
        CHECK_INT(s3_dir(data, cwds[i], dir2, sizeof(dir2)), 0);
        CHECK_STR(dir, dir2);
        CHECK_INT(s3_list(&sys, dir2, 10, list_fn, &l), 1);
    }
}

/* U2: pi's SessionInfo as -r's selector reads it (buildSessionInfo): the count of "message" entries of every role, the
 * last activity of a user or assistant message by its own millisecond number, the latest name, the parent; the age text */
static void test_infos(void)
{
    char dir[700], path[800], age[16];
    FILE *f;
    s3_info *a;
    path_of(dir, root, "infos");
    {
        char cmd[800];
        strcpy(cmd, "mkdir -p '");
        strcat(cmd, dir);
        strcat(cmd, "'");
        CHECK_INT(system(cmd), 0);
    }
    path_of(path, dir, "20240101T000000_aaaaaaaa.jsonl");
    f = fopen(path, "wb");
    CHECK(f != 0);
    if (!f)
        return;
    fputs("{\"type\":\"session\",\"version\":3,\"id\":\"i1\",\"timestamp\":\"2024-01-01T00:00:00.000Z\",\"cwd\":\"BOOTX:\",\"parentSession\":\"P\"}\n", f);
    fputs("{\"type\":\"message\",\"id\":\"1\",\"parentId\":null,\"timestamp\":\"2024-01-01T00:00:01.000Z\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hello\"},{\"type\":\"text\",\"text\":\"there\"}],\"timestamp\":1704067201000}}\n", f);
    fputs("{\"type\":\"message\",\"id\":\"2\",\"parentId\":\"1\",\"timestamp\":\"2024-01-01T00:00:02.000Z\",\"message\":{\"role\":\"assistant\",\"content\":\"ok\",\"timestamp\":1704067205000}}\n", f);
    fputs("{\"type\":\"message\",\"id\":\"3\",\"parentId\":\"2\",\"timestamp\":\"2024-01-01T00:00:09.000Z\",\"message\":{\"role\":\"bashExecution\",\"content\":\"x\",\"timestamp\":1704067209000}}\n", f);
    fputs("{\"type\":\"session_info\",\"id\":\"4\",\"parentId\":\"3\",\"timestamp\":\"2024-01-01T00:00:10.000Z\",\"name\":\"  named  \"}\n", f);
    fclose(f);
    CHECK_INT(s3_infos(&sys, dir, 10, &a), 1);
    if (a) {
        CHECK_STR(a[0].id, "i1");
        CHECK_STR(a[0].cwd, "BOOTX:");
        CHECK_STR(a[0].parent, "P");
        CHECK_STR(a[0].name, "named");
        CHECK_STR(a[0].first, "hello there");
        CHECK_INT(a[0].count, 3);                          /* every role counts, the activity is the last user/assistant message */
        CHECK(a[0].modified == 1704067205000.0);
        CHECK(strstr(a[0].text, "hello there ok") != 0);
    }
    free(a);
    pi_resume_age(1000000.0, 1000000.0 + 30000.0, age, sizeof(age));
    CHECK_STR(age, "now");
    pi_resume_age(0.0, 5 * 60000.0, age, sizeof(age));
    CHECK_STR(age, "5m");
    pi_resume_age(0.0, 3 * 3600000.0, age, sizeof(age));
    CHECK_STR(age, "3h");
    pi_resume_age(0.0, 2 * 86400000.0, age, sizeof(age));
    CHECK_STR(age, "2d");
    pi_resume_age(0.0, 15 * 86400000.0, age, sizeof(age));
    CHECK_STR(age, "2w");
    pi_resume_age(0.0, 70 * 86400000.0, age, sizeof(age));
    CHECK_STR(age, "2mo");
    pi_resume_age(0.0, 800 * 86400000.0, age, sizeof(age));
    CHECK_STR(age, "2y");
}

static void set_mtime(const char *path, long t)
{
    struct utimbuf u;
    u.actime = u.modtime = (time_t)t;
    CHECK_INT(utime(path, &u), 0);
}

/* --resume's list reads the files and nothing else (pi's buildSessionInfo): a
 * 0-byte file a crash left, or a last line without its newline, keeps its bytes
 * and its time, so --continue afterwards still resumes the real newest session */
static void test_list_reads_only(void)
{
    char dir[600], empty[700], torn[700], newest[600], found[600];
    char *before, *after;
    long nb = 0, na = 0, now = (long)time(0);
    struct stat a, b;
    pi_s3 st;
    lst l;
    int k;
    FILE *f;
    path_of(dir, root, "listro");
    for (k = 0; k < 2; k++) {
        static const char *const msg = "{\"role\":\"user\",\"content\":\"real\",\"timestamp\":1}";
        s3_init(&st, &sys, tick, 0, 1, (unsigned long)(200 + k));
        CHECK_INT(s3_new(&st, dir, "/c", 0, 0, 1), 0);
        CHECK_INT(s3_message(&st, msg, (long)strlen(msg)), 0);
        strcpy(newest, st.file);
        /* the second session is the newest */
        set_mtime(st.file, now - 1000 + k);
        s3_free(&st);
    }
    strcpy(empty, dir);
    strcat(empty, "/00000000T000000_crashed.jsonl");
    f = fopen(empty, "wb");
    CHECK(f != 0);
    if (f)
        fclose(f);
    set_mtime(empty, now - 5000);
    strcpy(torn, dir);
    strcat(torn, "/00000000T000001_torn.jsonl");
    f = fopen(torn, "wb");
    CHECK(f != 0);
    if (f) {
        fputs("{\"type\":\"session\",\"version\":3,\"id\":\"torn\",\"timestamp\":\"2024-12-03T14:00:00.000Z\",\"cwd\":\"/c\"}\n"
              "{\"type\":\"message\",\"id\":\"a1\",\"parentId\":null,\"timestamp\":\"2024-12-03T14:00:01.000Z\","
              "\"message\":{\"role\":\"user\",\"content\":\"torn one\",\"timestamp\":1}}",
              f);
        fclose(f);
    }
    set_mtime(torn, now - 4000);
    before = slurp(torn, &nb);
    memset(&l, 0, sizeof(l));
    /* the two sessions and the torn one; the empty file is not a session */
    CHECK_INT(s3_list(&sys, dir, 10, list_fn, &l), 3);
    CHECK_STR(l.title[2], "torn one");
    CHECK_INT(stat(empty, &a), 0);
    CHECK_INT((long)a.st_size, 0);
    CHECK_INT((long)a.st_mtime, now - 5000);
    CHECK_INT(stat(torn, &b), 0);
    CHECK_INT((long)b.st_mtime, now - 4000);
    after = slurp(torn, &na);
    CHECK_INT(na, nb);
    CHECK(before && after && nb == na && !memcmp(before, after, (size_t)nb));
    free(before);
    free(after);
    CHECK_INT(s3_recent(&sys, dir, found, sizeof(found)), 0);
    CHECK_STR(found, newest);
}

static void test_writer(void)
{
    run_scn("writer");
    run_scn("branched");
}

static void test_fixtures(void)
{
    static const char *const names[] = { "bad_lines", "bad_no_final_newline", "bad_not_session", "doc_all_types",
                                         "doc_branch", "doc_compaction", "doc_compaction_retain_none",
                                         "doc_context_edit", "doc_parent", "mig_v1", "mig_v2", "doc_system",
                                         "doc_compaction_system", "bad_content", "cut_cases", "cut_prev",
                                         "cut_hook" };
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        test_fixture(names[i], !strcmp(names[i], "mig_v1"));
}

void suite_pi_session(void)
{
    const char *base = getenv("TMPDIR");
    strcpy(root, base && *base ? base : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_session_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    sys_posix_init(&sp, &sys);
    test_fixtures();
    test_writer();
    test_refusals();
    test_big();
    test_names();
    test_find();
    test_volume_root_list();
    test_infos();
    test_list_reads_only();
    test_usage_sum();
    test_overflow_state();      /* last: the tests above count on the clock's ticks */
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
