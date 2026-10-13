/* see fuzzy.h */
#include <stdlib.h>
#include <string.h>
#include "fuzzy.h"
#include "lower_tab.h"

/* ---- UTF-16 units, as JavaScript sees the strings (fuzzy.ts indexes, compares and measures gaps in code units) ---- */

typedef unsigned short u16;

/* the code point at s[i] (n bytes in all) and its length; a byte that starts no sequence is U+FFFD of length 1, as TextDecoder */
static int dec1(const char *s, long i, long n, unsigned long *out)
{
    unsigned c = (unsigned char)s[i];
    int need = c >= 0xf0 && c < 0xf5 ? 3 : c >= 0xe0 && c < 0xf0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : 0;
    unsigned long v = need == 3 ? c & 7 : need == 2 ? c & 15 : need == 1 ? c & 31 : c;
    int j;
    if (c >= 0x80 && need == 0)
        need = -1;
    for (j = 0; j < need; j++) {
        if (i + 1 + j >= n || ((unsigned char)s[i + 1 + j] & 0xc0) != 0x80) {
            need = -1;
            break;
        }
        v = (v << 6) | ((unsigned char)s[i + 1 + j] & 0x3f);
    }
    if (need < 0 || (need == 2 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (need == 3 && (v < 0x10000 || v > 0x10ffff))) {
        *out = 0xfffd;
        return 1;
    }
    *out = v;
    return 1 + need;
}

/* the code points of a UTF-8 string; *out malloc'd, *cnt their count */
static int decode(const char *s, long n, unsigned long **out, long *cnt)
{
    unsigned long *cp = (unsigned long *)malloc(sizeof(unsigned long) * ((size_t)n + 1));
    long i = 0, k = 0;
    if (!cp)
        return 0;
    while (i < n)
        i += dec1(s, i, n, &cp[k++]);
    *out = cp;
    *cnt = k;
    return 1;
}

static int in_ranges(const lw_range *r, int n, unsigned long c)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (c < r[m].first)
            hi = m - 1;
        else if (c > r[m].last)
            lo = m + 1;
        else
            return 1;
    }
    return 0;
}

static unsigned long lower_cp(unsigned long c)
{
    int lo = 0, hi = (int)(sizeof(lw_runs) / sizeof(lw_runs[0])) - 1;
    if (c < 0x80)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (c < lw_runs[m].first)
            hi = m - 1;
        else if (c > lw_runs[m].last)
            lo = m + 1;
        else
            return (c - lw_runs[m].first) % lw_runs[m].step ? c : (unsigned long)((long)c + lw_runs[m].delta);
    }
    return c;
}

/* String.prototype.toLowerCase in UTF-16 units: the one-to-one mappings, U+0130 as i and U+0307, and the Final_Sigma rule
 * (U+03A3 is U+03C2 after a cased letter and before none, case-ignorable characters skipped, else U+03C3) */
static int lower_cps(const unsigned long *cp, long cnt, u16 **out, long *units)
{
    u16 *u;
    long i, k = 0;
    u = (u16 *)malloc(sizeof(u16) * ((size_t)cnt * 2 + 1));
    if (!u)
        return 0;
    for (i = 0; i < cnt; i++) {
        unsigned long c = cp[i];
        if (c == 0x130) {
            u[k++] = 'i';
            c = 0x307;
        } else if (c == 0x3a3) {
            long a = i - 1, b = i + 1;
            while (a >= 0 && in_ranges(lw_ignorable, (int)(sizeof(lw_ignorable) / sizeof(lw_ignorable[0])), cp[a]))
                a--;
            while (b < cnt && in_ranges(lw_ignorable, (int)(sizeof(lw_ignorable) / sizeof(lw_ignorable[0])), cp[b]))
                b++;
            c = a >= 0 && in_ranges(lw_cased, (int)(sizeof(lw_cased) / sizeof(lw_cased[0])), cp[a]) &&
                !(b < cnt && in_ranges(lw_cased, (int)(sizeof(lw_cased) / sizeof(lw_cased[0])), cp[b])) ? 0x3c2 : 0x3c3;
        } else {
            c = lower_cp(c);
        }
        if (c >= 0x10000) {
            u[k++] = (u16)(0xd800 + ((c - 0x10000) >> 10));
            u[k++] = (u16)(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            u[k++] = (u16)c;
        }
    }
    *out = u;
    *units = k;
    return 1;
}

static int to_lower(const char *s, long n, u16 **out, long *units)
{
    unsigned long *cp;
    long cnt;
    int ok;
    if (!decode(s, n, &cp, &cnt))
        return 0;
    ok = lower_cps(cp, cnt, out, units);
    free(cp);
    return ok;
}

/* JavaScript's \s (and trim()'s set) */
static int js_space(unsigned long c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

/* matchQuery (fuzzy.ts:17-67), on lowercased UTF-16 */
static int match_query(const u16 *q, long qn, const u16 *t, long tn, long *score)
{
    long qi = 0, sc = 0, last = -1, consec = 0, i;
    if (qn == 0) {
        *score = 0;
        return 1;
    }
    if (qn > tn)
        return 0;
    while (qi < qn) {
        int boundary, c;
        for (i = last + 1; i < tn && t[i] != q[qi]; i++)
            ;
        if (i >= tn)
            break;
        c = i ? t[i - 1] : 0;
        boundary = i == 0 || js_space((unsigned long)c) || c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
        if (last == i - 1) {            /* (also at the first character: -1 === -1) */
            consec++;
            sc -= consec * 50;
        } else {
            consec = 0;
            if (last >= 0)
                sc += (i - last - 1) * 20;
        }
        if (boundary)
            sc -= 100;
        sc += i;                        /* slight penalty for later matches: i * 0.1 */
        last = i;
        qi++;
    }
    if (qi < qn)
        return 0;
    if (qn == tn && !memcmp(q, t, sizeof(u16) * (size_t)tn))
        sc -= 1000;
    *score = sc;
    return 1;
}

static int is_letter(int c)
{
    return c >= 'a' && c <= 'z';
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* fuzzyMatch on lowercased units */
static int match_units(const u16 *q, long qn, const u16 *t, long tn, long *score)
{
    u16 *sw;
    long a = 0, b;
    int ok;
    if (match_query(q, qn, t, tn, score))
        return 1;
    /* fuzzy.ts:69-84: a query of letters then digits is tried as digits then letters (and the other way round), 5.0 worse */
    while (a < qn && is_letter(q[a]))
        a++;
    for (b = a; b < qn && is_digit(q[b]); b++)
        ;
    if (!(a > 0 && b > a && b == qn)) {
        for (a = 0; a < qn && is_digit(q[a]); a++)
            ;
        for (b = a; b < qn && is_letter(q[b]); b++)
            ;
        if (!(a > 0 && b > a && b == qn))
            return 0;
    }
    sw = (u16 *)malloc(sizeof(u16) * (size_t)qn);
    if (!sw)
        return 0;
    memcpy(sw, q + a, sizeof(u16) * (size_t)(qn - a));
    memcpy(sw + (qn - a), q, sizeof(u16) * (size_t)a);
    ok = match_query(sw, qn, t, tn, score);
    free(sw);
    if (!ok)
        return 0;
    *score += 50;
    return 1;
}

int pi_fuzzy_match(const char *query, long qn, const char *text, long tn, long *score)
{
    u16 *q, *t;
    long ql, tl;
    int ok = 0;
    if (!to_lower(query, qn, &q, &ql))
        return 0;
    if (to_lower(text, tn, &t, &tl)) {
        ok = match_units(q, ql, t, tl, score);
        free(t);
    }
    free(q);
    return ok;
}

int pi_fuzzy_item(const char *query, long qn, const char *text, long tn, long *score)
{
    unsigned long *cp;
    u16 *t;
    long cnt, tl, i, a = 0, total = 0;
    int ok = 1;
    *score = 0;
    if (!to_lower(text, tn, &t, &tl))
        return 0;
    if (!decode(query, qn, &cp, &cnt)) {
        free(t);
        return 0;
    }
    /* the tokens: runs between whitespace and '/' (fuzzyFilter), each lowercased by itself (toLowerCase's Final_Sigma
     * looks at the token only) and matched */
    for (i = 0; i <= cnt && ok; i++) {
        if (i < cnt && !js_space(cp[i]) && cp[i] != '/')
            continue;
        if (i > a) {
            u16 *q;
            long ql, sc;
            if (!lower_cps(cp + a, i - a, &q, &ql)) {
                ok = 0;
                break;
            }
            ok = match_units(q, ql, t, tl, &sc);
            free(q);
            if (ok)
                total += sc;
        }
        a = i + 1;
    }
    free(cp);
    free(t);
    if (!ok)
        return 0;
    *score = total;
    return 1;
}

/* whether the query holds a token (fuzzyFilter returns every item when it does not) */
static int has_token(const char *q, long qn)
{
    unsigned long *cp;
    long cnt, i;
    int r = 0;
    if (!decode(q, qn, &cp, &cnt))
        return 0;
    for (i = 0; i < cnt && !r; i++)
        r = !js_space(cp[i]) && cp[i] != '/';
    free(cp);
    return r;
}

/* the items of `want` (flagged 1 in in[]) appended to idx by score, the ties in menu order (Array.sort is stable) */
static int append_sorted(int *idx, int n_out, const long *score, const char *want, int n)
{
    int i, j, start = n_out;
    for (i = 0; i < n; i++) {
        if (!want[i])
            continue;
        j = n_out++;
        while (j > start && score[idx[j - 1]] > score[i]) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = i;
    }
    return n_out;
}

int pi_slash_filter(void *u, const cl_cmd *cmds, int n, const char *box, long len, long cur, long *from, int *idx)
{
    const char *q;
    long ql, st = 0, i;
    long *score;
    char *bare, *full;
    int k = 0;
    (void)u;
    if (n <= 0 || cur < 0 || cur > len)
        return 0;
    /* CombinedAutocompleteProvider.getSuggestions (autocomplete.ts:316-372) for the editor's first line (editor.ts:2188):
     * the text before the cursor, without its leading whitespace, starts with '/' and holds no space */
    for (i = 0; i < cur; i++)
        if (box[i] == '\n')
            return 0;
    while (st < cur) {
        unsigned long c;
        int l = dec1(box, st, cur, &c);
        if (!js_space(c))
            break;
        st += l;
    }
    if (st >= cur || box[st] != '/' || memchr(box + st, ' ', (size_t)(cur - st)))
        return 0;
    *from = st;
    q = box + st + 1;
    ql = cur - st - 1;
    if (!has_token(q, ql)) {
        for (i = 0; i < n; i++)
            idx[i] = (int)i;
        return n;
    }
    score = (long *)malloc(sizeof(long) * (size_t)n);
    bare = (char *)calloc((size_t)n, 2);
    if (!score || !bare) {
        free(score);
        free(bare);
        return 0;
    }
    full = bare + n;
    for (i = 0; i < n; i++) {
        const char *name = cmds[i].name[0] == '/' ? cmds[i].name + 1 : cmds[i].name;
        int skill = !strncmp(name, "skill:", 6);
        long s;
        if (pi_fuzzy_item(q, ql, skill ? name + 6 : name, (long)strlen(skill ? name + 6 : name), &s))
            bare[i] = 1, score[i] = s;
        else if (skill && pi_fuzzy_item(q, ql, name, (long)strlen(name), &s))
            full[i] = 1, score[i] = s;
    }
    k = append_sorted(idx, 0, score, bare, n);
    k = append_sorted(idx, k, score, full, n);
    free(score);
    free(bare);
    return k;
}
