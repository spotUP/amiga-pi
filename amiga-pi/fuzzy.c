/* see fuzzy.h */
#include <stdlib.h>
#include <string.h>
#include "fuzzy.h"

static int low(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* matchQuery (fuzzy.ts:17-67) */
static int match_query(const char *q, long qn, const char *t, long tn, long *score)
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
        for (i = last + 1; i < tn && low((unsigned char)t[i]) != low((unsigned char)q[qi]); i++)
            ;
        if (i >= tn)
            break;
        c = i ? low((unsigned char)t[i - 1]) : 0;
        boundary = i == 0 || is_space(c) || c == '-' || c == '_' || c == '.' || c == '/' || c == ':';
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
    if (qn == tn) {
        for (i = 0; i < tn && low((unsigned char)q[i]) == low((unsigned char)t[i]); i++)
            ;
        if (i == tn)
            sc -= 1000;
    }
    *score = sc;
    return 1;
}

static int is_letter(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

int pi_fuzzy_match(const char *query, long qn, const char *text, long tn, long *score)
{
    char sw[128];
    long a = 0, b;
    if (match_query(query, qn, text, tn, score))
        return 1;
    /* fuzzy.ts:69-84: a query of letters then digits is tried as digits then letters (and the other way round), 5.0 worse */
    if (qn <= 0 || qn >= (long)sizeof(sw))
        return 0;
    while (a < qn && is_letter((unsigned char)query[a]))
        a++;
    for (b = a; b < qn && is_digit((unsigned char)query[b]); b++)
        ;
    if (a > 0 && b > a && b == qn) {
        memcpy(sw, query + a, (size_t)(qn - a));
        memcpy(sw + (qn - a), query, (size_t)a);
    } else {
        for (a = 0; a < qn && is_digit((unsigned char)query[a]); a++)
            ;
        for (b = a; b < qn && is_letter((unsigned char)query[b]); b++)
            ;
        if (!(a > 0 && b > a && b == qn))
            return 0;
        memcpy(sw, query + a, (size_t)(qn - a));
        memcpy(sw + (qn - a), query, (size_t)a);
    }
    if (!match_query(sw, qn, text, tn, score))
        return 0;
    *score += 50;
    return 1;
}

int pi_fuzzy_item(const char *query, long qn, const char *text, long tn, long *score)
{
    long a = 0, i, total = 0;
    *score = 0;
    for (i = 0; i <= qn; i++) {
        if (i < qn && !is_space((unsigned char)query[i]) && query[i] != '/')
            continue;
        if (i > a) {
            long s;
            if (!pi_fuzzy_match(query + a, i - a, text, tn, &s))
                return 0;
            total += s;
        }
        a = i + 1;
    }
    *score = total;
    return 1;
}

/* whether the query holds a token (fuzzyFilter returns every item when it does not) */
static int has_token(const char *q, long qn)
{
    long i;
    for (i = 0; i < qn; i++)
        if (!is_space((unsigned char)q[i]) && q[i] != '/')
            return 1;
    return 0;
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

int pi_slash_filter(void *u, const cl_cmd *cmds, int n, const char *text, long len, int *idx)
{
    const char *q = text + 1;
    long ql = len - 1;
    long *score;
    char *bare, *full;
    int i, k = 0;
    (void)u;
    if (n <= 0)
        return 0;
    if (!has_token(q, ql)) {
        for (i = 0; i < n; i++)
            idx[i] = i;
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
