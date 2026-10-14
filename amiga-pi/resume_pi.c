/* see resume_pi.h */
#include <stdlib.h>
#include <string.h>
#include "resume_pi.h"
#include "session3.h"
#include "fuzzy.h"
#include "claude/util.h"
#include "claude/regex.h"
#include "view/vw_text.h"

#define RS_MAX 200              /* sessions read per scope: the newest (pi reads them all) */
#define RS_PAGE 10              /* SessionList.maxVisible */
#define RS_TOK 12

enum { SORT_THREADED, SORT_RECENT, SORT_RELEVANCE };

typedef struct rs {
    cl_tui *t;
    cl_sys *sys;
    const cl_pitheme *th;
    double now_ms;
    const char *cwd, *dir, *all;
    int all_flat, filter_cwd;
    s3_info *info[2];
    int ninfo[2], loaded[2];
    int scope, sort, named, path;
    int *vis, nvis;             /* indices into info[scope] in display order */
    char (*pre)[64];            /* the tree prefix of each visible row */
    int del;                    /* the visible row whose deletion waits for Enter, -1 none */
    char msg[200];
    int msg_err;
    char head[900], hints[1200];
    tui_pick pk;
} rs;

void pi_resume_age(double modified, double now_ms, char *out, long cap)
{
    double d = now_ms - modified;
    long mins = (long)(d / 60000.0), hours = (long)(d / 3600000.0), days = (long)(d / 86400000.0);
    char num[16];
    if (d < 0)
        mins = (long)(-(-d / 60000.0));          /* Math.floor of a negative: below zero is "now" too */
    if (mins < 1) {
        cl_copy(out, "now", cap);
        return;
    }
    if (mins < 60)
        cl_ltoa(mins, num);
    else if (hours < 24)
        cl_ltoa(hours, num);
    else if (days < 7)
        cl_ltoa(days, num);
    else if (days < 30)
        cl_ltoa(days / 7, num);
    else if (days < 365)
        cl_ltoa(days / 30, num);
    else
        cl_ltoa(days / 365, num);
    cl_copy(out, num, cap);
    cl_cat(out, mins < 60 ? "m" : hours < 24 ? "h" : days < 7 ? "d" : days < 30 ? "w" : days < 365 ? "mo" : "y", cap);
}

static const char *role(const rs *v, int r, char *b)
{
    pitheme_sgr(v->th, r, b);
    return b;
}

/* pi's theme.fg(): a role with no colour of its own (the system theme's dim) still sets the default foreground, so
 * it ends an enclosing colour */
static const char *fgrole(const rs *v, int r, char *b)
{
    pitheme_sgr(v->th, r, b);
    if (strlen(b) <= 4)
        cl_cat(b, "\033[39m", 48);
    return b;
}

/* ---- the search: pi's session-selector-search.ts ---- */

typedef struct query {
    int regex, err, ntok;
    cl_re *re;
    char tok[RS_TOK][100];
    int phrase[RS_TOK];
} query;

static void lower_norm(const char *s, char *out, long cap)
{
    long k = 0;
    int sp = 1;
    for (; *s && k + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!sp && k + 1 < cap)
                out[k++] = ' ';
            sp = 1;
        } else {
            out[k++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            sp = 0;
        }
    }
    while (k > 0 && out[k - 1] == ' ')
        k--;
    out[k] = 0;
}

static void query_add(query *q, const char *buf, long n, int phrase)
{
    long a = 0, b = n;
    while (a < b && (buf[a] == ' ' || buf[a] == '\t'))
        a++;
    while (b > a && (buf[b - 1] == ' ' || buf[b - 1] == '\t'))
        b--;
    if (b <= a || q->ntok >= RS_TOK)
        return;
    if (b - a >= (long)sizeof(q->tok[0]))
        b = a + (long)sizeof(q->tok[0]) - 1;
    memcpy(q->tok[q->ntok], buf + a, (size_t)(b - a));
    q->tok[q->ntok][b - a] = 0;
    q->phrase[q->ntok++] = phrase;
}

/* parseSearchQuery: "re:<pattern>", else tokens split at white space with "quoted phrases"; unbalanced quotes: plain
 * white-space tokens */
static void query_parse(query *q, const char *s)
{
    char buf[100], err[80];
    long n = 0, i, len;
    int in = 0, unclosed = 0, ntok0;
    memset(q, 0, sizeof(*q));
    while (*s == ' ')
        s++;
    len = (long)strlen(s);
    while (len > 0 && s[len - 1] == ' ')
        len--;
    if (!len)
        return;
    if (len >= 3 && !strncmp(s, "re:", 3)) {
        char pat[100];
        long a = 3;
        while (a < len && s[a] == ' ')
            a++;
        q->regex = 1;
        if (a >= len || len - a >= (long)sizeof(pat)) {
            q->err = 1;
            return;
        }
        memcpy(pat, s + a, (size_t)(len - a));
        pat[len - a] = 0;
        q->re = re_compile(pat, RE_ICASE, err, sizeof(err));
        if (!q->re)
            q->err = 1;
        return;
    }
    for (i = 0; i < len; i++) {
        if (s[i] == '"') {
            query_add(q, buf, n, in);
            n = 0;
            in = !in;
            continue;
        }
        if (!in && (s[i] == ' ' || s[i] == '\t')) {
            query_add(q, buf, n, 0);
            n = 0;
            continue;
        }
        if (n + 1 < (long)sizeof(buf))
            buf[n++] = s[i];
    }
    unclosed = in;
    ntok0 = q->ntok;
    if (unclosed) {
        q->ntok = 0;
        n = 0;
        for (i = 0; i <= len; i++)
            if (i == len || s[i] == ' ' || s[i] == '\t') {
                query_add(q, buf, n, 0);
                n = 0;
            } else if (n + 1 < (long)sizeof(buf))
                buf[n++] = s[i];
        (void)ntok0;
        return;
    }
    query_add(q, buf, n, 0);
}

/* matchSession: 1 when it matches, *score in tenths (lower is better) */
static int query_match(const query *q, const s3_info *in, long *score)
{
    char *text;
    long tn, i, total = 0;
    int ok = 1;
    tn = (long)strlen(in->id) + (long)strlen(in->name) + (long)strlen(in->text) + (long)strlen(in->cwd) + 4;
    text = (char *)malloc((size_t)tn);
    if (!text)
        return 0;
    cl_copy(text, in->id, tn);
    cl_cat(text, " ", tn);
    cl_cat(text, in->name, tn);
    cl_cat(text, " ", tn);
    cl_cat(text, in->text, tn);
    cl_cat(text, " ", tn);
    cl_cat(text, in->cwd, tn);
    tn = (long)strlen(text);
    if (q->regex) {
        long ms, me;
        ok = q->re && re_search(q->re, text, tn, 0, &ms, &me) == 1;
        total = ok ? ms : 0;
    } else {
        char *norm = 0;
        for (i = 0; i < q->ntok && ok; i++) {
            if (q->phrase[i]) {
                char ph[100], *at;
                if (!norm) {
                    norm = (char *)malloc((size_t)tn + 1);
                    if (!norm) {
                        ok = 0;
                        break;
                    }
                    lower_norm(text, norm, tn + 1);
                }
                lower_norm(q->tok[i], ph, sizeof(ph));
                if (!ph[0])
                    continue;
                at = strstr(norm, ph);
                if (!at)
                    ok = 0;
                else
                    total += at - norm;
            } else {
                long sc = 0;
                if (!pi_fuzzy_match(q->tok[i], (long)strlen(q->tok[i]), text, tn, &sc))
                    ok = 0;
                else
                    total += sc;
            }
        }
        free(norm);
    }
    free(text);
    *score = total;
    return ok;
}

/* ---- the list: filterSessions (session-selector.ts:365) ---- */

static const s3_info *at(const rs *v, int row)
{
    return &v->info[v->scope][v->vis[row]];
}

/* one subtree in threaded order (flattenSessionTree's walk): node i at depth, anc the continuation flag of each level
 * above it, last whether no sibling follows; children by the latest activity below them, newest first */
static void emit(rs *v, const int *parent, const double *latest, int i, int depth, const char *anc, int last)
{
    int n = v->ninfo[v->scope], j, k, nc = 0, *kids;
    char prefix[64], sub[40];
    prefix[0] = 0;
    if (depth > 0) {
        int d;
        for (d = 0; d < depth && d < 12; d++)
            cl_cat(prefix, anc[d] ? "\342\224\202  " : "   ", sizeof(prefix));
        cl_cat(prefix, last ? "\342\224\224\342\224\200 " : "\342\224\234\342\224\200 ", sizeof(prefix));
    }
    cl_copy(v->pre[v->nvis], prefix, 64);
    v->vis[v->nvis++] = i;
    if (depth >= 12)
        return;
    kids = (int *)malloc((size_t)n * sizeof(int));
    if (!kids)
        return;
    for (j = 0; j < n; j++)
        if (parent[j] == i)
            kids[nc++] = j;
    for (j = 1; j < nc; j++)
        for (k = j; k > 0 && latest[kids[k]] > latest[kids[k - 1]]; k--) {
            int t = kids[k];
            kids[k] = kids[k - 1];
            kids[k - 1] = t;
        }
    memcpy(sub, anc, (size_t)depth);
    sub[depth] = (char)(depth > 0 ? !last : 0);
    for (j = 0; j < nc; j++)
        emit(v, parent, latest, kids[j], depth + 1, sub, j == nc - 1);
    free(kids);
}

static void threaded(rs *v)
{
    const s3_info *a = v->info[v->scope];
    int n = v->ninfo[v->scope], i, j, steps, nr = 0, *parent, *roots;
    double *latest;
    char cont[40];
    parent = (int *)malloc((size_t)n * sizeof(int));
    latest = (double *)malloc((size_t)n * sizeof(double));
    roots = (int *)malloc((size_t)n * sizeof(int));
    if (!parent || !latest || !roots)
        goto out;
    for (i = 0; i < n; i++) {
        parent[i] = -1;
        latest[i] = a[i].modified;
        if (a[i].parent[0])
            for (j = 0; j < n; j++)
                if (j != i && !strcmp(a[j].path, a[i].parent)) {
                    parent[i] = j;
                    break;
                }
    }
    for (i = 0; i < n; i++)
        for (j = parent[i], steps = 0; j >= 0 && steps < n; j = parent[j], steps++)
            if (a[i].modified > latest[j])
                latest[j] = a[i].modified;
    for (i = 0; i < n; i++) {
        /* a cycle of parents has no root: its members are shown flat */
        int up = i;
        for (steps = 0; parent[up] >= 0 && steps <= n; steps++)
            up = parent[up];
        if (parent[up] >= 0)
            parent[i] = -1;
        if (parent[i] < 0)
            roots[nr++] = i;
    }
    for (i = 1; i < nr; i++)
        for (j = i; j > 0 && latest[roots[j]] > latest[roots[j - 1]]; j--) {
            int t = roots[j];
            roots[j] = roots[j - 1];
            roots[j - 1] = t;
        }
    memset(cont, 0, sizeof(cont));
    for (i = 0; i < nr; i++)
        emit(v, parent, latest, roots[i], 0, cont, i == nr - 1);
out:
    free(parent);
    free(latest);
    free(roots);
}

static void refilter(rs *v)
{
    const s3_info *a = v->info[v->scope];
    int n = v->ninfo[v->scope], i, j;
    long *score = 0;
    query q;
    memset(&q, 0, sizeof(q));
    v->nvis = 0;
    v->del = -1;
    if (!n)
        goto done;
    query_parse(&q, v->pk.query);
    if (v->sort == SORT_THREADED && !q.ntok && !q.regex && !q.err) {
        /* threaded without a search: the tree (named only: the sessions with a name, parents or not) */
        if (v->named) {
            for (i = 0; i < n; i++)
                if (a[i].name[0]) {
                    v->pre[v->nvis][0] = 0;
                    v->vis[v->nvis++] = i;
                }
        } else
            threaded(v);
        goto done;
    }
    if (q.err)
        goto done;
    score = (long *)malloc((size_t)n * sizeof(long));
    if (!score)
        goto done;
    for (i = 0; i < n; i++) {
        long sc = 0;
        if (v->named && !a[i].name[0])
            continue;
        if ((q.ntok || q.regex) && !query_match(&q, &a[i], &sc))
            continue;
        score[v->nvis] = sc;
        v->pre[v->nvis][0] = 0;
        v->vis[v->nvis++] = i;
    }
    /* recent: the order of the list; else by score, then the later activity (the list is newest first) */
    if (v->sort != SORT_RECENT && (q.ntok || q.regex))
        for (i = 1; i < v->nvis; i++)
            for (j = i; j > 0 && score[j] < score[j - 1]; j--) {
                long ts = score[j];
                int ti = v->vis[j];
                score[j] = score[j - 1];
                score[j - 1] = ts;
                v->vis[j] = v->vis[j - 1];
                v->vis[j - 1] = ti;
            }
    free(score);
done:
    if (q.re)
        re_free(q.re);
    v->pk.n = v->nvis;
    if (v->pk.sel > v->nvis - 1)
        v->pk.sel = v->nvis > 0 ? v->nvis - 1 : 0;
}

/* ---- loading ---- */

static int load(rs *v, int scope)
{
    if (v->loaded[scope])
        return 0;
    v->loaded[scope] = 1;
    if (scope == 0) {
        int i, k = 0;
        v->ninfo[0] = s3_infos(v->sys, v->dir, RS_MAX, &v->info[0]);
        if (v->filter_cwd)
            for (i = 0; i < v->ninfo[0]; i++)
                if (v->info[0][i].cwd[0] && !strcmp(v->info[0][i].cwd, v->cwd))
                    v->info[0][k++] = v->info[0][i];
        if (v->filter_cwd)
            v->ninfo[0] = k;
    } else if (v->all_flat)
        v->ninfo[1] = s3_infos(v->sys, v->all, RS_MAX, &v->info[1]);
    else
        v->ninfo[1] = s3_infos_all(v->sys, v->all, RS_MAX, &v->info[1]);
    return 0;
}

/* ---- drawing ---- */

static void put_fit(char *out, long cap, const char *s, int cols)
{
    long n = vw_fit(s, (long)strlen(s), cols);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, (size_t)n);
    out[n] = 0;
}

/* a key as pi's keyHint: the key dim, the words muted */
static void hint(const rs *v, char *out, long cap, const char *key, const char *words)
{
    char b[48];
    cl_cat(out, fgrole(v, PR_DIM, b), cap);
    cl_cat(out, key, cap);
    cl_cat(out, "\033[39m", cap);
    cl_cat(out, fgrole(v, PR_MUTED, b), cap);
    cl_cat(out, " ", cap);
    cl_cat(out, words, cap);
    cl_cat(out, "\033[39;22m", cap);
}

static void compose(rs *v)
{
    char b[48], b2[48], left[200], right[400], plain[300], title[80];
    int cols = v->t->cols, rw, lw, avail, sp;
    const char *sortname = v->sort == SORT_THREADED ? "Threaded" : v->sort == SORT_RECENT ? "Recent" : "Fuzzy";
    long n;
    cl_copy(title, v->scope == 0 ? "Resume Session (Current Folder)" : "Resume Session (All)", sizeof(title));
    cl_copy(plain, v->scope == 0 ? "\342\227\211 Current Folder | \342\227\213 All" : "\342\227\213 Current Folder | \342\227\211 All",
            sizeof(plain));
    cl_cat(plain, "  Name: ", sizeof(plain));
    cl_cat(plain, v->named ? "Named" : "All", sizeof(plain));
    cl_cat(plain, "  Sort: ", sizeof(plain));
    cl_cat(plain, sortname, sizeof(plain));
    rw = vw_width(plain, (long)strlen(plain));
    /* the right side, coloured as pi's header: the scope in the accent, the rest muted, the labels muted */
    right[0] = 0;
    if (v->scope == 0) {
        cl_cat(right, role(v, PR_ACCENT, b), sizeof(right));
        cl_cat(right, "\342\227\211 Current Folder\033[39m", sizeof(right));
        cl_cat(right, role(v, PR_MUTED, b), sizeof(right));
        cl_cat(right, " | \342\227\213 All\033[39;22m", sizeof(right));
    } else {
        cl_cat(right, role(v, PR_MUTED, b), sizeof(right));
        cl_cat(right, "\342\227\213 Current Folder | \033[39;22m", sizeof(right));
        cl_cat(right, role(v, PR_ACCENT, b), sizeof(right));
        cl_cat(right, "\342\227\211 All\033[39m", sizeof(right));
    }
    cl_cat(right, "  ", sizeof(right));
    cl_cat(right, role(v, PR_MUTED, b), sizeof(right));
    cl_cat(right, "Name: \033[39;22m", sizeof(right));
    cl_cat(right, role(v, PR_ACCENT, b2), sizeof(right));
    cl_cat(right, v->named ? "Named" : "All", sizeof(right));
    cl_cat(right, "\033[39m  ", sizeof(right));
    cl_cat(right, role(v, PR_MUTED, b), sizeof(right));
    cl_cat(right, "Sort: \033[39;22m", sizeof(right));
    cl_cat(right, role(v, PR_ACCENT, b2), sizeof(right));
    cl_cat(right, sortname, sizeof(right));
    cl_cat(right, "\033[39m", sizeof(right));
    avail = cols - rw - 1;
    if (avail < 0)
        avail = 0;
    put_fit(left, sizeof(left), title, avail);
    lw = vw_width(left, (long)strlen(left));
    sp = cols - lw - rw;
    if (sp < 0)
        sp = 0;
    cl_copy(v->head, "\033[1m", sizeof(v->head));
    cl_cat(v->head, left, sizeof(v->head));
    cl_cat(v->head, "\033[22m", sizeof(v->head));
    for (n = 0; n < sp && (long)strlen(v->head) + 2 < (long)sizeof(v->head); n++)
        cl_cat(v->head, " ", sizeof(v->head));
    cl_cat(v->head, right, sizeof(v->head));
    /* the hint rows, or the question, or the last message */
    v->hints[0] = 0;
    if (v->del >= 0) {
        cl_cat(v->hints, role(v, PR_ERROR, b), sizeof(v->hints));
        cl_cat(v->hints, "Delete session? ", sizeof(v->hints));
        {
            char h[200];
            h[0] = 0;
            hint(v, h, sizeof(h), "enter", "confirm");
            cl_cat(v->hints, h, sizeof(v->hints));
            cl_cat(v->hints, " \302\267 ", sizeof(v->hints));
            h[0] = 0;
            hint(v, h, sizeof(h), "escape/ctrl+c", "cancel");
            cl_cat(v->hints, h, sizeof(v->hints));
        }
        cl_cat(v->hints, "\033[39m\n", sizeof(v->hints));      /* the second hint row is empty */
    } else if (v->msg[0]) {
        cl_cat(v->hints, role(v, v->msg_err ? PR_ERROR : PR_ACCENT, b), sizeof(v->hints));
        cl_cat(v->hints, v->msg, sizeof(v->hints));
        cl_cat(v->hints, "\033[39m\n", sizeof(v->hints));
    } else {
        char sep[80], h[200];
        cl_copy(sep, role(v, PR_MUTED, b), sizeof(sep));
        cl_cat(sep, " \302\267 \033[39;22m", sizeof(sep));
        hint(v, v->hints, sizeof(v->hints), "tab", "scope");
        cl_cat(v->hints, sep, sizeof(v->hints));
        cl_cat(v->hints, role(v, PR_MUTED, b), sizeof(v->hints));
        cl_cat(v->hints, "re:<pattern> regex \302\267 \"phrase\" exact\033[39;22m\n", sizeof(v->hints));
        hint(v, v->hints, sizeof(v->hints), "ctrl+s", "sort");
        cl_cat(v->hints, sep, sizeof(v->hints));
        h[0] = 0;
        hint(v, h, sizeof(h), "ctrl+n", "named");
        cl_cat(v->hints, h, sizeof(v->hints));
        cl_cat(v->hints, sep, sizeof(v->hints));
        h[0] = 0;
        hint(v, h, sizeof(h), "ctrl+d", "delete");
        cl_cat(v->hints, h, sizeof(v->hints));
        cl_cat(v->hints, sep, sizeof(v->hints));
        h[0] = 0;
        hint(v, h, sizeof(h), "ctrl+p", v->path ? "path (on)" : "path (off)");
        cl_cat(v->hints, h, sizeof(v->hints));
    }
}

static void row_text(void *u, int i, int selected, char *out, long cap)
{
    rs *v = (rs *)u;
    const s3_info *in = at(v, i);
    char b[48], right[420], age[16], msg[260], num[16], shown[260], plain[760];
    const char *disp = in->name[0] ? in->name : in->first;
    long k, n;
    int cols = v->t->cols, rw, pw, avail, lw, sp, confirming = v->del == i;
    const char *pre = v->pre[i];
    /* the message: control characters to spaces, trimmed */
    for (k = 0, n = 0; disp[k] && n + 1 < (long)sizeof(msg); k++)
        msg[n++] = (unsigned char)disp[k] < 0x20 || disp[k] == 0x7f ? ' ' : disp[k];
    msg[n] = 0;
    {
        long a = 0, e = n;
        while (a < e && msg[a] == ' ')
            a++;
        while (e > a && msg[e - 1] == ' ')
            e--;
        memmove(msg, msg + a, (size_t)(e - a));
        msg[e - a] = 0;
    }
    pi_resume_age(in->modified, v->now_ms, age, sizeof(age));
    cl_ltoa(in->count, num);
    cl_copy(right, num, sizeof(right));
    cl_cat(right, " ", sizeof(right));
    cl_cat(right, age, sizeof(right));
    if (v->scope == 1 && in->cwd[0]) {
        cl_copy(plain, in->cwd, sizeof(plain));
        cl_cat(plain, " ", sizeof(plain));
        cl_cat(plain, right, sizeof(plain));
        cl_copy(right, plain, sizeof(right));
    }
    if (v->path) {
        cl_copy(plain, in->path, sizeof(plain));
        cl_cat(plain, " ", sizeof(plain));
        cl_cat(plain, right, sizeof(plain));
        cl_copy(right, plain, sizeof(right));
    }
    rw = vw_width(right, (long)strlen(right));
    pw = vw_width(pre, (long)strlen(pre));
    avail = cols - 2 - pw - (rw + 2);
    put_fit(shown, sizeof(shown), msg, avail > 10 ? avail : 10);
    if ((long)strlen(shown) < (long)strlen(msg)) {
        /* truncateToWidth with an ellipsis: one column less, then "..." as a single character */
        put_fit(shown, sizeof(shown), msg, (avail > 10 ? avail : 10) - 1);
        cl_cat(shown, "\342\200\246", sizeof(shown));
    }
    out[0] = 0;
    if (selected) {
        cl_cat(out, role(v, PR_SELECTED_BG, b), cap);
        cl_cat(out, role(v, PR_ACCENT, b), cap);
        cl_cat(out, "\342\200\272 \033[39m", cap);
    } else
        cl_cat(out, "  ", cap);
    if (pre[0]) {
        cl_cat(out, "\033[2m", cap);
        cl_cat(out, pre, cap);
        cl_cat(out, "\033[22m", cap);
    }
    {
        int r = confirming ? PR_ERROR : in->name[0] ? PR_WARNING : -1;
        if (selected)
            cl_cat(out, "\033[1m", cap);
        if (r >= 0)
            cl_cat(out, role(v, r, b), cap);
        cl_cat(out, shown, cap);
        if (r >= 0)
            cl_cat(out, "\033[39m", cap);
        if (selected)
            cl_cat(out, "\033[22m", cap);
    }
    lw = 2 + pw + vw_width(shown, (long)strlen(shown));
    sp = cols - lw - rw;
    if (sp < 1)
        sp = 1;
    for (k = 0; k < sp && (long)strlen(out) + 2 < cap; k++)
        cl_cat(out, " ", cap);
    cl_cat(out, confirming ? role(v, PR_ERROR, b) : "\033[2m", cap);
    cl_cat(out, right, cap);
    cl_cat(out, confirming ? "\033[39m" : "\033[22m", cap);
}

static void redraw_texts(rs *v)
{
    compose(v);
    v->pk.title = v->head;
    v->pk.help = v->hints;
    v->pk.empty = v->nvis ? "" :
        v->named ? (v->scope == 1 ? "  No named sessions found. Press ctrl+n to show all." :
                                     "  No named sessions in current folder. Press ctrl+n to show all, or Tab to view all.") :
        v->scope == 1 ? "  No sessions found" : "  No sessions in current folder. Press Tab to view all.";
}

static int key(void *u, const cl_key *k, int act)
{
    rs *v = (rs *)u;
    int changed = 0;
    (void)act;
    v->msg[0] = 0;
    if (v->del >= 0) {
        /* the question: Enter deletes, Escape asks no more, every other key is ignored */
        if (k->k == K_ENTER) {
            const s3_info *in = at(v, v->del);
            char path[360];
            int j;
            cl_copy(path, in->path, sizeof(path));
            v->del = -1;
            if (!v->sys->remove || v->sys->remove(v->sys->u, path)) {
                cl_copy(v->msg, "Failed to delete: ", sizeof(v->msg));
                cl_cat(v->msg, v->sys->err ? v->sys->err(v->sys->u) : "cannot remove the file", sizeof(v->msg));
                v->msg_err = 1;
            } else {
                int s;
                for (s = 0; s < 2; s++)
                    for (j = 0; j < v->ninfo[s]; j++)
                        if (!strcmp(v->info[s][j].path, path)) {
                            memmove(&v->info[s][j], &v->info[s][j + 1], (size_t)(v->ninfo[s] - j - 1) * sizeof(s3_info));
                            v->ninfo[s]--;
                            j--;
                        }
                cl_copy(v->msg, "Session deleted", sizeof(v->msg));
                v->msg_err = 0;
                refilter(v);
            }
        } else if (k->k == K_ESC || (k->k == K_CTRL && k->ch == 'c'))
            v->del = -1;
        redraw_texts(v);
        return 1;
    }
    if (k->k == K_TAB) {
        v->scope = !v->scope;
        load(v, v->scope);
        refilter(v);
    } else if (k->k == K_CTRL && k->ch == 's') {
        v->sort = (v->sort + 1) % 3;
        refilter(v);
    } else if (k->k == K_CTRL && k->ch == 'n') {
        v->named = !v->named;
        refilter(v);
    } else if (k->k == K_CTRL && k->ch == 'p')
        v->path = !v->path;
    else if (k->k == K_CTRL && k->ch == 'd') {
        if (v->nvis)
            v->del = v->pk.sel;
    } else if (k->k == K_UP)
        v->pk.sel = v->pk.sel > 0 ? v->pk.sel - 1 : 0;
    else if (k->k == K_DOWN)
        v->pk.sel = v->pk.sel < v->nvis - 1 ? v->pk.sel + 1 : v->pk.sel;
    else if (k->k == K_PGUP)
        v->pk.sel = v->pk.sel - RS_PAGE > 0 ? v->pk.sel - RS_PAGE : 0;
    else if (k->k == K_PGDN)
        v->pk.sel = v->pk.sel + RS_PAGE < v->nvis - 1 ? v->pk.sel + RS_PAGE : v->nvis > 0 ? v->nvis - 1 : 0;
    else if (tui_pick_qkey(&v->pk, k))
        changed = 1;
    else if (k->k == K_ENTER || k->k == K_ESC || (k->k == K_CTRL && k->ch == 'c'))
        return 0;                           /* the picker's own: accept, cancel */
    if (changed)
        refilter(v);
    redraw_texts(v);
    return 1;
}

int pi_resume_pick(cl_io *io, cl_sys *sys, const cl_pitheme *th, llm_ts now, const char *cwd, const char *dir,
                   const char *all, int all_flat, int filter_cwd, const char *kb, long kbn, char *out, long cap)
{
    static cl_tui t;
    rs *v;
    int res = 0, row;
    if (!io->read || !io->raw || tui_init(&t, io))
        return -1;
    v = (rs *)calloc(1, sizeof(rs));
    if (!v) {
        tui_free(&t);
        return -1;
    }
    v->t = &t;
    v->sys = sys;
    v->th = th;
    v->now_ms = (double)now.sec * 1000.0 + now.ms;
    v->cwd = cwd;
    v->dir = dir;
    v->all = all;
    v->all_flat = all_flat;
    v->filter_cwd = filter_cwd;
    v->del = -1;
    km_free(&t.km);
    if (kb)
        km_load_pi(&t.km, kb, kbn);
    else
        km_init_pi(&t.km);
    t.look = TUI_LOOK_PI;
    t.pith = th;
    t.screen = TUI_SCREEN_REGULAR;
    t.pi_nofoot = 1;
    if (tui_start(&t)) {
        tui_free(&t);
        free(v);
        return -1;
    }
    load(v, 0);
    v->vis = (int *)malloc(sizeof(int) * (size_t)(RS_MAX * 2 + 1));
    v->pre = (char (*)[64])malloc(64 * (size_t)(RS_MAX * 2 + 1));
    if (!v->vis || !v->pre) {
        res = -1;
        goto out;
    }
    v->pk.layout = TUI_PICK_SESSION;
    v->pk.maxrows = RS_PAGE;
    v->pk.row = row_text;
    v->pk.key = key;
    v->pk.u = v;
    refilter(v);
    redraw_texts(v);
    row = tui_pick_run(&t, &v->pk);
    if (row >= 0 && row < v->nvis) {
        cl_copy(out, at(v, row)->path, cap);
        res = 1;
    }
out:
    tui_stop(&t);
    tui_free(&t);
    free(v->vis);
    free(v->pre);
    free(v->info[0]);
    free(v->info[1]);
    free(v);
    return res;
}
