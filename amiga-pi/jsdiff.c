/* see jsdiff.h. The names in the comments are those of jsdiff 8.0.4 (libesm/diff/base.js, line.js, word.js, util/string.js). */
#include <stdlib.h>
#include <string.h>
#include "jsdiff.h"
#include "view/vw_text.h"

#define JD_MAX_NODES 400000L    /* the walk's components (24 bytes each): past this the diff is not made (jsdiff has no bound) */

typedef struct jtok {
    const char *p;
    long n;
} jtok;

typedef struct jnode {          /* a component of a path: shared by the paths that grew from it */
    long count;
    long prev;
    int added, removed;
} jnode;

typedef struct jpath {
    long old;                   /* oldPos */
    long last;                  /* lastComponent, -1 none */
    int ok;
} jpath;

enum { JM_LINE, JM_WORD };

/* \s of JavaScript (and String.prototype.trim): WhiteSpace and LineTerminator */
static int js_ws(unsigned long c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

/* extendedWordChars of word.js */
static int word_char(unsigned long c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == 0xAD ||
           (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2C6) || (c >= 0x2C8 && c <= 0x2D7) ||
           (c >= 0x2DE && c <= 0x2FF) || (c >= 0x1E00 && c <= 0x1EFF);
}

/* the length of the leading / trailing whitespace of s[0..n) */
static long lead_ws(const char *s, long n)
{
    long i = 0;
    while (i < n) {
        unsigned long c;
        int l = vw_char(s + i, n - i, &c);
        if (!js_ws(c))
            break;
        i += l;
    }
    return i;
}

static long trail_ws(const char *s, long n)
{
    long i = 0, a = 0;          /* a: where the whitespace after the last non-whitespace starts */
    while (i < n) {
        unsigned long c;
        int l = vw_char(s + i, n - i, &c);
        i += l;
        if (!js_ws(c))
            a = i;
    }
    return n - a;
}

long jd_leading_ws(const char *s, long n)
{
    return lead_ws(s, n);
}

/* ---- the Myers walk (base.js diffWithOptionsObj) ---- */

typedef struct jwalk {
    const jtok *ot, *nt;
    long on, nn;
    int mode;
    jnode *node;
    long nnode, capnode;
} jwalk;

static void trimmed(const jtok *t, const char **p, long *n)
{
    long l = lead_ws(t->p, t->n);
    if (l == t->n) {
        *p = t->p;
        *n = 0;
        return;
    }
    *p = t->p + l;
    *n = t->n - l - trail_ws(t->p, t->n);
}

static int tok_equal(int mode, const jtok *a, const jtok *b)
{
    if (mode == JM_LINE)
        return a->n == b->n && !memcmp(a->p, b->p, (size_t)a->n);
    {
        /* WordDiff.equals: left.trim() === right.trim() */
        const char *pa, *pb;
        long na, nb;
        trimmed(a, &pa, &na);
        trimmed(b, &pb, &nb);
        return na == nb && !memcmp(pa, pb, (size_t)na);
    }
}

static long node_new(jwalk *w, long count, long prev, int added, int removed)
{
    if (w->nnode == w->capnode) {
        long cap = w->capnode ? w->capnode * 2 : 256;
        jnode *n;
        if (cap > JD_MAX_NODES + 256)
            return -2;
        n = (jnode *)realloc(w->node, (size_t)cap * sizeof(*n));
        if (!n)
            return -2;
        w->node = n;
        w->capnode = cap;
    }
    if (w->nnode >= JD_MAX_NODES)
        return -2;
    w->node[w->nnode].count = count;
    w->node[w->nnode].prev = prev;
    w->node[w->nnode].added = added;
    w->node[w->nnode].removed = removed;
    return w->nnode++;
}

/* extractCommon: the common run after base's position on diagonal d; the new position, -2 when out of nodes */
static long extract_common(jwalk *w, jpath *base, long d)
{
    long oldp = base->old, newp = oldp - d, common = 0;
    while (newp + 1 < w->nn && oldp + 1 < w->on && tok_equal(w->mode, &w->ot[oldp + 1], &w->nt[newp + 1])) {
        newp++;
        oldp++;
        common++;
    }
    if (common) {
        long k = node_new(w, common, base->last, 0, 0);
        if (k == -2)
            return -2;
        base->last = k;
    }
    base->old = oldp;
    return newp;
}

/* addToPath */
static int add_to_path(jwalk *w, const jpath *path, int added, int removed, long inc, jpath *out)
{
    long last = path->last, k;
    if (last >= 0 && w->node[last].added == added && w->node[last].removed == removed)
        k = node_new(w, w->node[last].count + 1, w->node[last].prev, added, removed);
    else
        k = node_new(w, 1, last, added, removed);
    if (k == -2)
        return -1;
    out->old = path->old + inc;
    out->last = k;
    out->ok = 1;
    return 0;
}

/* the components in order into comp (count, added, removed): the number, -1 out of memory / bound, -3 identical (still a list) */
static long walk(jwalk *w, jnode **comp)
{
    long newlen = w->nn, oldlen = w->on, edit = 1, maxedit = newlen + oldlen, off = maxedit + 2, np, last = -1, n = 0, i;
    long mind = -(maxedit + 5), maxd = maxedit + 5;
    jpath *best;
    jpath first;
    best = (jpath *)calloc((size_t)(2 * maxedit + 5), sizeof(jpath));
    if (!best)
        return -1;
    first.old = -1;
    first.last = -1;
    first.ok = 1;
    np = extract_common(w, &first, 0);
    if (np == -2) {
        free(best);
        return -1;
    }
    best[off] = first;
    if (first.old + 1 >= oldlen && np + 1 >= newlen) {
        last = first.last;
        goto done;
    }
    while (edit <= maxedit) {
        long d, lo = mind > -edit ? mind : -edit, hi = maxd < edit ? maxd : edit;
        for (d = lo; d <= hi; d += 2) {
            jpath base;
            jpath *rem = &best[off + d - 1], *add = &best[off + d + 1];
            int can_add = 0, can_rem;
            long addnew;
            jpath remc = *rem, addc = *add;      /* (the clearing below must not change what this step reads) */
            if (rem->ok)
                rem->ok = 0;
            if (addc.ok) {
                addnew = addc.old - d;
                can_add = 0 <= addnew && addnew < newlen;
            }
            can_rem = remc.ok && remc.old + 1 < oldlen;
            if (!can_add && !can_rem) {
                best[off + d].ok = 0;
                continue;
            }
            if (!can_rem || (can_add && remc.old < addc.old)) {
                if (add_to_path(w, &addc, 1, 0, 0, &base)) {
                    free(best);
                    return -1;
                }
            } else if (add_to_path(w, &remc, 0, 1, 1, &base)) {
                free(best);
                return -1;
            }
            np = extract_common(w, &base, d);
            if (np == -2) {
                free(best);
                return -1;
            }
            if (base.old + 1 >= oldlen && np + 1 >= newlen) {
                last = base.last;
                goto done;
            }
            best[off + d] = base;
            if (base.old + 1 >= oldlen && d - 1 < maxd)
                maxd = d - 1;
            if (np + 1 >= newlen && d + 1 > mind)
                mind = d + 1;
        }
        edit++;
    }
    free(best);
    return -1;
done:
    free(best);
    for (i = last; i >= 0; i = w->node[i].prev)
        n++;
    *comp = (jnode *)malloc((size_t)(n ? n : 1) * sizeof(jnode));
    if (!*comp)
        return -1;
    for (i = last, np = n; i >= 0; i = w->node[i].prev)
        (*comp)[--np] = w->node[i];
    return n;
}

/* ---- tokenizers ---- */

static int tok_add(jtok **t, long *n, long *cap, const char *p, long len)
{
    if (len <= 0)
        return 0;               /* removeEmpty */
    if (*n == *cap) {
        long c = *cap ? *cap * 2 : 64;
        jtok *x = (jtok *)realloc(*t, (size_t)c * sizeof(**t));
        if (!x)
            return -1;
        *t = x;
        *cap = c;
    }
    (*t)[*n].p = p;
    (*t)[(*n)++].n = len;
    return 0;
}

/* line.js tokenize: each line with its "\n" or "\r\n" */
static int tok_lines(const char *s, long n, jtok **t, long *nt)
{
    long a = 0, i, cap = 0;
    *t = 0;
    *nt = 0;
    for (i = 0; i < n; i++)
        if (s[i] == '\n') {
            if (tok_add(t, nt, &cap, s + a, i + 1 - a))
                return -1;
            a = i + 1;
        }
    if (a < n && tok_add(t, nt, &cap, s + a, n - a))
        return -1;
    return 0;
}

/* word.js tokenize: runs of word characters, of whitespace, single other characters, whitespace stitched on */
static int tok_words(const char *s, long n, jtok **t, long *nt)
{
    long i = 0, cap = 0, prev_s = -1, prev_n = 0;
    int prev_ws = 0;
    *t = 0;
    *nt = 0;
    while (i < n) {
        unsigned long c;
        int l = vw_char(s + i, n - i, &c);
        long st = i, len;
        int ws = js_ws(c);
        i += l;
        if (word_char(c)) {
            while (i < n) {
                int l2 = vw_char(s + i, n - i, &c);
                if (!word_char(c))
                    break;
                i += l2;
            }
        } else if (ws) {
            while (i < n) {
                int l2 = vw_char(s + i, n - i, &c);
                if (!js_ws(c))
                    break;
                i += l2;
            }
        }
        len = i - st;
        if (ws) {
            if (prev_s < 0) {
                if (tok_add(t, nt, &cap, s + st, len))
                    return -1;
            } else
                (*t)[*nt - 1].n += len;
        } else if (prev_s >= 0 && prev_ws) {
            if ((*t)[*nt - 1].p == s + prev_s && (*t)[*nt - 1].n == prev_n)
                (*t)[*nt - 1].n += len;
            else if (tok_add(t, nt, &cap, s + prev_s, prev_n + len))
                return -1;
        } else if (tok_add(t, nt, &cap, s + st, len))
            return -1;
        prev_s = st;
        prev_n = len;
        prev_ws = ws;
    }
    return 0;
}

/* ---- values ---- */

static int build(jwalk *w, jnode *comp, long nc, jd_res *r)
{
    long i, np = 0, op = 0, k;
    r->p = (jd_part *)calloc((size_t)(nc ? nc : 1), sizeof(jd_part));
    r->n = 0;
    if (!r->p)
        return -1;
    for (i = 0; i < nc; i++) {
        const jtok *tk = comp[i].removed ? w->ot + op : w->nt + np;
        long total = 0;
        char *v;
        for (k = 0; k < comp[i].count; k++)
            total += tk[k].n;
        v = (char *)malloc((size_t)total + 1);
        if (!v)
            return -1;
        total = 0;
        for (k = 0; k < comp[i].count; k++) {
            const char *p = tk[k].p;
            long len = tk[k].n;
            if (k && w->mode == JM_WORD) {          /* WordDiff.join: no leading whitespace but on the first */
                long l = lead_ws(p, len);
                p += l;
                len -= l;
            }
            memcpy(v + total, p, (size_t)len);
            total += len;
        }
        v[total] = 0;
        r->p[i].v = v;
        r->p[i].n = total;
        r->p[i].added = comp[i].added;
        r->p[i].removed = comp[i].removed;
        r->n++;
        if (comp[i].removed)
            op += comp[i].count;
        else {
            np += comp[i].count;
            if (!comp[i].added)
                op += comp[i].count;
        }
    }
    return 0;
}

void jd_free(jd_res *r)
{
    long i;
    for (i = 0; r->p && i < r->n; i++)
        free(r->p[i].v);
    free(r->p);
    r->p = 0;
    r->n = 0;
}

/* ---- word.js postProcess: dedupeWhitespaceInChangeObjects ---- */

static char *sdup(const char *p, long n)
{
    char *s = (char *)malloc((size_t)n + 1);
    if (s) {
        memcpy(s, p, (size_t)n);
        s[n] = 0;
    }
    return s;
}

/* the part's text replaced by head + (v[from..n) with to bytes cut at its end) ... as small steps: set(part, new text) */
static int part_set(jd_part *p, const char *a, long an, const char *b, long bn)
{
    char *s = (char *)malloc((size_t)(an + bn) + 1);
    if (!s)
        return -1;
    memcpy(s, a, (size_t)an);
    memcpy(s + an, b, (size_t)bn);
    s[an + bn] = 0;
    free(p->v);
    p->v = s;
    p->n = an + bn;
    return 0;
}

/* longestCommonPrefix / Suffix in whole characters */
static long common_prefix(const char *a, long an, const char *b, long bn)
{
    long i = 0;
    while (i < an && i < bn && a[i] == b[i])
        i++;
    while (i > 0 && i < an && ((unsigned char)a[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static long common_suffix(const char *a, long an, const char *b, long bn)
{
    long i = 0;
    while (i < an && i < bn && a[an - 1 - i] == b[bn - 1 - i])
        i++;
    while (i > 0 && ((unsigned char)a[an - i] & 0xC0) == 0x80)
        i--;
    return i;
}

/* replacePrefix(part.v, old of oldn bytes, new) / replaceSuffix: the old text is the part's prefix / suffix (jsdiff throws if not) */
static int replace_prefix(jd_part *p, long oldn, const char *nw, long nn)
{
    char *keep = sdup(p->v + oldn, p->n - oldn);
    int rc;
    if (!keep)
        return -1;
    rc = part_set(p, nw, nn, keep, p->n - oldn);
    free(keep);
    return rc;
}

static int replace_suffix(jd_part *p, long oldn, const char *nw, long nn)
{
    char *keep = sdup(p->v, p->n - oldn);
    int rc;
    if (!keep)
        return -1;
    rc = part_set(p, keep, p->n - oldn, nw, nn);
    free(keep);
    return rc;
}

/* maximumOverlap(s1, s2): util/string.js overlapCount, on bytes */
static long overlap(const char *a, long an, const char *b, long bn)
{
    long start_a = 0, end_b = bn, k = 0, j, i;
    long *map;
    if (an > bn)
        start_a = an - bn;
    if (an < bn)
        end_b = an;
    if (end_b <= 0)
        return 0;
    map = (long *)calloc((size_t)end_b + 1, sizeof(long));
    if (!map)
        return 0;
    map[0] = 0;
    for (j = 1; j < end_b; j++) {
        map[j] = b[j] == b[k] ? map[k] : k;
        while (k > 0 && b[j] != b[k])
            k = map[k];
        if (b[j] == b[k])
            k++;
    }
    k = 0;
    for (i = start_a; i < an; i++) {
        while (k > 0 && (k >= bn || a[i] != b[k]))      /* (b[bn] is undefined in JavaScript: no match) */
            k = map[k];
        if (k < bn && a[i] == b[k])
            k++;
    }
    free(map);
    return k;
}

static int dedupe(jd_part *sk, jd_part *del, jd_part *ins, jd_part *ek)
{
    int rc = 0;
    if (del && ins) {
        long ol = lead_ws(del->v, del->n), orr = trail_ws(del->v, del->n), nl = lead_ws(ins->v, ins->n), nr = trail_ws(ins->v, ins->n);
        char *ows = sdup(del->v, ol), *owe = sdup(del->v + del->n - orr, orr), *nws = sdup(ins->v, nl), *nwe = sdup(ins->v + ins->n - nr, nr);
        if (!ows || !owe || !nws || !nwe)
            rc = -1;
        else {
            if (sk) {
                long c = common_prefix(ows, ol, nws, nl);
                rc |= replace_suffix(sk, nl, nws, c);       /* replaceSuffix(startKeep, newWsPrefix, commonWsPrefix) */
                rc |= replace_prefix(del, c, "", 0);
                rc |= replace_prefix(ins, c, "", 0);
            }
            if (ek) {
                long c = common_suffix(owe, orr, nwe, nr);
                rc |= replace_prefix(ek, nr, nwe + nr - c, c);
                rc |= replace_suffix(del, c, "", 0);
                rc |= replace_suffix(ins, c, "", 0);
            }
        }
        free(ows);
        free(owe);
        free(nws);
        free(nwe);
    } else if (ins) {
        if (sk) {
            long ws = lead_ws(ins->v, ins->n);
            rc |= replace_prefix(ins, ws, "", 0);
        }
        if (ek) {
            long ws = lead_ws(ek->v, ek->n);
            rc |= replace_prefix(ek, ws, "", 0);
        }
    } else if (sk && ek) {
        long full = lead_ws(ek->v, ek->n), ds = lead_ws(del->v, del->n), de = trail_ws(del->v, del->n);
        char *nfull = sdup(ek->v, full), *dstart = sdup(del->v, ds), *dend = sdup(del->v + del->n - de, de);
        if (!nfull || !dstart || !dend)
            rc = -1;
        else {
            long ns = common_prefix(nfull, full, dstart, ds), ne;
            rc |= replace_prefix(del, ns, "", 0);
            ne = common_suffix(nfull + ns, full - ns, dend, de);
            rc |= replace_suffix(del, ne, "", 0);
            rc |= replace_prefix(ek, full, nfull + full - ne, ne);
            rc |= replace_suffix(sk, full, nfull, full - ne);
        }
        free(nfull);
        free(dstart);
        free(dend);
    } else if (ek) {
        long pre = lead_ws(ek->v, ek->n), suf = trail_ws(del->v, del->n);
        long ov = overlap(del->v + del->n - suf, suf, ek->v, pre);
        rc |= replace_suffix(del, ov, "", 0);
    } else if (sk) {
        long suf = trail_ws(sk->v, sk->n), pre = lead_ws(del->v, del->n);
        long ov = overlap(sk->v + sk->n - suf, suf, del->v, pre);
        rc |= replace_prefix(del, ov, "", 0);
    }
    return rc ? -1 : 0;
}

static int post_words(jd_res *r)
{
    jd_part *keep = 0, *ins = 0, *del = 0;
    long i;
    for (i = 0; i < r->n; i++) {
        jd_part *c = &r->p[i];
        if (c->added)
            ins = c;
        else if (c->removed)
            del = c;
        else {
            if ((ins || del) && dedupe(keep, del, ins, c))
                return -1;
            keep = c;
            ins = 0;
            del = 0;
        }
    }
    if ((ins || del) && dedupe(keep, del, ins, 0))
        return -1;
    return 0;
}

static int run(const char *a, long an, const char *b, long bn, int mode, jd_res *r)
{
    jtok *ot = 0, *nt = 0;
    long on = 0, nn = 0, nc;
    jwalk w;
    jnode *comp = 0;
    int rc = -1;
    r->p = 0;
    r->n = 0;
    memset(&w, 0, sizeof(w));
    if ((mode == JM_LINE ? tok_lines(a, an, &ot, &on) : tok_words(a, an, &ot, &on)) ||
        (mode == JM_LINE ? tok_lines(b, bn, &nt, &nn) : tok_words(b, bn, &nt, &nn)))
        goto out;
    w.ot = ot;
    w.nt = nt;
    w.on = on;
    w.nn = nn;
    w.mode = mode;
    nc = walk(&w, &comp);
    if (nc < 0)
        goto out;
    if (build(&w, comp, nc, r)) {
        jd_free(r);
        goto out;
    }
    if (mode == JM_WORD && post_words(r)) {
        jd_free(r);
        goto out;
    }
    rc = 0;
out:
    free(ot);
    free(nt);
    free(comp);
    free(w.node);
    return rc;
}

int jd_lines(const char *a, long an, const char *b, long bn, jd_res *r)
{
    return run(a, an, b, bn, JM_LINE, r);
}

int jd_words(const char *a, long an, const char *b, long bn, jd_res *r)
{
    return run(a, an, b, bn, JM_WORD, r);
}

/* ---- what pi builds on them (core/tools/edit-diff.ts) ---- */

/* the lines of a part's value split at "\n", the empty last one dropped: the start of each, count (value.split("\n") then pop) */
static long part_lines(const jd_part *p)
{
    long k = 1, i;
    for (i = 0; i < p->n; i++)
        k += p->v[i] == '\n';
    if (p->n == 0 || p->v[p->n - 1] == '\n')
        k--;                    /* the last element is "" */
    return k;
}

static const char *line_at(const jd_part *p, long idx, long *len)
{
    const char *s = p->v;
    long i = 0, a = 0, k = 0;
    for (;; i++) {
        if (i == p->n || p->v[i] == '\n') {
            if (k == idx) {
                *len = i - a;
                return s + a;
            }
            k++;
            a = i + 1;
            if (i == p->n)
                break;
        }
    }
    *len = 0;
    return s;
}

static void put_num(jw *o, long v, int width, char pad)
{
    char d[24];
    int n = 0, i;
    long x = v;
    do {
        d[n++] = (char)('0' + x % 10);
        x /= 10;
    } while (x);
    for (i = n; i < width; i++)
        jw_raw(o, &pad, 1);
    while (n)
        jw_raw(o, &d[--n], 1);
}

static long digits(long v)
{
    long n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

static long split_count(const char *s, long n)
{
    long k = 1, i;
    for (i = 0; i < n; i++)
        k += s[i] == '\n';
    return k;
}

int pi_edit_diff(const char *a, long an, const char *b, long bn, jw *diff, long *first)
{
    enum { CTX = 4 };
    jd_res r;
    long i, ol = 1, nl = 1, ow, w, oc = split_count(a, an), nc = split_count(b, bn), nout = 0;
    int last_change = 0;
    *first = 0;
    if (jd_lines(a, an, b, bn, &r))
        return -1;
    w = digits(oc > nc ? oc : nc);
    ow = w;
    (void)ow;
#define OUT_LINE(sign, num, s, len)                \
    do {                                           \
        if (nout++)                                \
            jw_raw(diff, "\n", 1);                 \
        jw_raw(diff, sign, 1);                     \
        put_num(diff, num, (int)w, ' ');           \
        jw_raw(diff, " ", 1);                      \
        jw_raw(diff, s, len);                      \
    } while (0)
#define OUT_GAP()                                  \
    do {                                           \
        long q;                                    \
        if (nout++)                                \
            jw_raw(diff, "\n", 1);                 \
        jw_raw(diff, " ", 1);                      \
        for (q = 0; q < w; q++)                    \
            jw_raw(diff, " ", 1);                  \
        jw_rawz(diff, " ...");                     \
    } while (0)
    for (i = 0; i < r.n; i++) {
        const jd_part *p = &r.p[i];
        long cnt = part_lines(p), k, len;
        const char *ln;
        if (p->added || p->removed) {
            if (!*first)
                *first = nl;
            for (k = 0; k < cnt; k++) {
                ln = line_at(p, k, &len);
                if (p->added) {
                    OUT_LINE("+", nl, ln, len);
                    nl++;
                } else {
                    OUT_LINE("-", ol, ln, len);
                    ol++;
                }
            }
            last_change = 1;
        } else {
            int next_change = i < r.n - 1 && (r.p[i + 1].added || r.p[i + 1].removed);
            if (last_change && next_change) {
                if (cnt <= CTX * 2) {
                    for (k = 0; k < cnt; k++) {
                        ln = line_at(p, k, &len);
                        OUT_LINE(" ", ol, ln, len);
                        ol++;
                        nl++;
                    }
                } else {
                    long skipped = cnt - 2 * CTX;
                    for (k = 0; k < CTX; k++) {
                        ln = line_at(p, k, &len);
                        OUT_LINE(" ", ol, ln, len);
                        ol++;
                        nl++;
                    }
                    OUT_GAP();
                    ol += skipped;
                    nl += skipped;
                    for (k = cnt - CTX; k < cnt; k++) {
                        ln = line_at(p, k, &len);
                        OUT_LINE(" ", ol, ln, len);
                        ol++;
                        nl++;
                    }
                }
            } else if (last_change) {
                long shown = cnt < CTX ? cnt : CTX, skipped = cnt - shown;
                for (k = 0; k < shown; k++) {
                    ln = line_at(p, k, &len);
                    OUT_LINE(" ", ol, ln, len);
                    ol++;
                    nl++;
                }
                if (skipped > 0) {
                    OUT_GAP();
                    ol += skipped;
                    nl += skipped;
                }
            } else if (next_change) {
                long skipped = cnt - CTX > 0 ? cnt - CTX : 0;
                if (skipped > 0) {
                    OUT_GAP();
                    ol += skipped;
                    nl += skipped;
                }
                for (k = skipped; k < cnt; k++) {
                    ln = line_at(p, k, &len);
                    OUT_LINE(" ", ol, ln, len);
                    ol++;
                    nl++;
                }
            } else {
                ol += cnt;
                nl += cnt;
            }
            last_change = 0;
        }
    }
#undef OUT_LINE
#undef OUT_GAP
    jd_free(&r);
    return diff->oom ? -1 : 0;
}

/* structuredPatch / formatPatch (patch/create.js) with 4 lines of context, FILE_HEADERS_ONLY */

/* the lines of a part with their "\n" (splitLines): count, start of line k, its length */
static long sl_count(const jd_part *p)
{
    long k = 0, i;
    if (p->n == 0)
        return 1;               /* "".split("\n") is [""]: one line "" without a newline */
    for (i = 0; i < p->n; i++)
        k += p->v[i] == '\n';
    if (p->v[p->n - 1] != '\n')
        k++;
    return k;
}

static const char *sl_at(const jd_part *p, long idx, long *len)
{
    long i = 0, a = 0, k = 0;
    for (; i < p->n; i++)
        if (p->v[i] == '\n') {
            if (k == idx) {
                *len = i + 1 - a;
                return p->v + a;
            }
            k++;
            a = i + 1;
        }
    *len = p->n - a;
    return p->v + a;
}

typedef struct jhunk {
    long os, ol, ns, nl;
    jw lines;                   /* the hunk's lines, each with its terminating "\n", "\ No newline at end of file" lines in place */
} jhunk;

/* one line (with or without its newline) into a hunk: the sign, the line, and the marker line when it lacked the newline */
static void hunk_line(jw *o, char sign, const char *s, long n)
{
    jw_raw(o, &sign, 1);
    if (n > 0 && s[n - 1] == '\n')
        jw_raw(o, s, n);
    else {
        jw_raw(o, s, n);
        jw_rawz(o, "\n\\ No newline at end of file\n");
    }
}

int pi_edit_patch(const char *path, const char *a, long an, const char *b, long bn, jw *patch)
{
    enum { CTX = 4 };
    jd_res r;
    jhunk *h = 0;
    long nh = 0, i, k, old_start = 0, new_start = 0, old_line = 1, new_line = 1, ci;
    int open = 0, rc = -1;
    jw cur;
    if (jd_lines(a, an, b, bn, &r))
        return -1;
    jw_init(&cur);
    for (i = 0; i <= r.n; i++) {
        jd_part empty;
        const jd_part *p;
        long cnt;
        if (i == r.n) {
            empty.added = empty.removed = 0;
            empty.v = (char *)"";
            empty.n = 0;
            p = &empty;
        } else
            p = &r.p[i];
        cnt = i == r.n ? 0 : sl_count(p);       /* (the closing empty value has lines [] ) */
        if (p->added || p->removed) {
            if (!open) {
                long ctxn = 0;
                open = 1;
                old_start = old_line;
                new_start = new_line;
                jw_reset(&cur);
                if (i > 0) {
                    const jd_part *pv = &r.p[i - 1];
                    long pc = sl_count(pv), from = pc > CTX ? pc - CTX : 0;
                    for (k = from; k < pc; k++) {
                        long len;
                        const char *s = sl_at(pv, k, &len);
                        hunk_line(&cur, ' ', s, len);
                        ctxn++;
                    }
                    old_start -= ctxn;
                    new_start -= ctxn;
                }
            }
            for (k = 0; k < cnt; k++) {
                long len;
                const char *s = sl_at(p, k, &len);
                hunk_line(&cur, p->added ? '+' : '-', s, len);
            }
            if (p->added)
                new_line += cnt;
            else
                old_line += cnt;
        } else {
            if (open) {
                if (cnt <= CTX * 2 && i < r.n - 1) {
                    for (k = 0; k < cnt; k++) {
                        long len;
                        const char *s = sl_at(p, k, &len);
                        hunk_line(&cur, ' ', s, len);
                    }
                } else {
                    jhunk *nhp;
                    long size = cnt < CTX ? cnt : CTX;
                    for (k = 0; k < size; k++) {
                        long len;
                        const char *s = sl_at(p, k, &len);
                        hunk_line(&cur, ' ', s, len);
                    }
                    nhp = (jhunk *)realloc(h, (size_t)(nh + 1) * sizeof(*h));
                    if (!nhp)
                        goto out;
                    h = nhp;
                    h[nh].os = old_start;
                    h[nh].ol = old_line - old_start + size;
                    h[nh].ns = new_start;
                    h[nh].nl = new_line - new_start + size;
                    jw_init(&h[nh].lines);
                    jw_raw(&h[nh].lines, cur.p ? cur.p : "", cur.n);
                    nh++;
                    open = 0;
                    jw_reset(&cur);
                }
            }
            old_line += cnt;
            new_line += cnt;
        }
    }
    jw_rawz(patch, "--- ");
    jw_rawz(patch, path);
    jw_rawz(patch, "\n+++ ");
    jw_rawz(patch, path);
    jw_rawz(patch, "\n");
    for (ci = 0; ci < nh; ci++) {
        long os = h[ci].os, ns = h[ci].ns;
        if (h[ci].ol == 0)
            os -= 1;
        if (h[ci].nl == 0)
            ns -= 1;
        jw_rawz(patch, "@@ -");
        put_num(patch, os, 0, ' ');
        jw_rawz(patch, ",");
        put_num(patch, h[ci].ol, 0, ' ');
        jw_rawz(patch, " +");
        put_num(patch, ns, 0, ' ');
        jw_rawz(patch, ",");
        put_num(patch, h[ci].nl, 0, ' ');
        jw_rawz(patch, " @@\n");
        jw_raw(patch, h[ci].lines.p ? h[ci].lines.p : "", h[ci].lines.n);
    }
    rc = patch->oom ? -1 : 0;
out:
    for (ci = 0; ci < nh; ci++)
        jw_free(&h[ci].lines);
    free(h);
    jw_free(&cur);
    jd_free(&r);
    return rc;
}
