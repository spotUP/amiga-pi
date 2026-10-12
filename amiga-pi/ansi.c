/* see ansi.h */
#include <string.h>
#include "ansi.h"

#define ESC 0x1b

/* the introducer of a CSI at p (ESC, or U+009B as UTF-8): its length, 0 if none */
static int intro(const char *p, long n)
{
    if (n >= 1 && (unsigned char)p[0] == ESC)
        return 1;
    if (n >= 2 && (unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0x9B)
        return 2;
    return 0;
}

/* the length of a string terminator at p: BEL, ESC \ or U+009C; 0 if none */
static int st_len(const char *p, long n)
{
    if (n >= 1 && p[0] == 7)
        return 1;
    if (n >= 2 && (unsigned char)p[0] == ESC && p[1] == '\\')
        return 2;
    if (n >= 2 && (unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0x9C)
        return 2;
    return 0;
}

/* CSI_START's class [[\]()#;?] */
static int is_class(char c)
{
    return c == '[' || c == ']' || c == '(' || c == ')' || c == '#' || c == ';' || c == '?';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* CSI_FINAL [\dA-PR-TZcf-nq-uy=><~] */
static int is_final(char c)
{
    return is_digit(c) || (c >= 'A' && c <= 'P') || (c >= 'R' && c <= 'T') || c == 'Z' || c == 'c' ||
           (c >= 'f' && c <= 'n') || (c >= 'q' && c <= 'u') || c == 'y' || c == '=' || c == '>' || c == '<' || c == '~';
}

/* the optional parameters \d{1,4}([;:]\d{0,4})* greedy from p: their length (0 when there are none) */
static long params(const char *p, long n)
{
    long i = 0, d = 0;
    while (i < n && d < 4 && is_digit(p[i])) {
        i++;
        d++;
    }
    if (!d)
        return 0;
    while (i < n && (p[i] == ';' || p[i] == ':')) {
        i++;
        d = 0;
        while (i < n && d < 4 && is_digit(p[i])) {
            i++;
            d++;
        }
    }
    return i;
}

/* the length of the complete CSI sequence at p (introducer, class run, parameters, final byte), 0 if there is none; the
 * parameters give back characters when no final byte follows them (a digit is a final byte too) */
static long csi_complete(const char *p, long n)
{
    int in = intro(p, n);
    long i, c, e, q;
    if (!in)
        return 0;
    i = in;
    while (i < n && is_class(p[i]))
        i++;
    c = i;
    e = c + params(p + c, n - c);
    if (e < n && is_final(p[e]))
        return e + 1;
    for (q = e - 1; q >= c; q--)
        if (is_digit(p[q]))
            return q + 1;
    return 0;
}

/* the length of the complete OSC sequence at p (ESC ] up to the first terminator), 0 if there is none */
static long osc_complete(const char *p, long n)
{
    long i;
    if (n < 2 || (unsigned char)p[0] != ESC || p[1] != ']')
        return 0;
    for (i = 2; i < n; i++) {
        int s = st_len(p + i, n - i);
        if (s)
            return i + s;
    }
    return 0;
}

/* whether in[j..n) is, to its end, an unfinished sequence: OSC without a terminator (an ESC at the very end may start
 * ESC \), or a CSI start without its final byte (unfinishedAnsiAtEndRegex) */
static int unfinished_from(const char *p, long n)
{
    int in = intro(p, n);
    long i, e;
    if (!in)
        return 0;
    if (in == 1 && n >= 2 && p[1] == ']') {
        for (i = 2; i < n; i++) {
            if (p[i] == 7 || (i + 1 < n && (unsigned char)p[i] == 0xC2 && (unsigned char)p[i + 1] == 0x9C))
                break;
            if ((unsigned char)p[i] == ESC && i + 1 < n && p[i + 1] == '\\')
                break;
        }
        if (i >= n)
            return 1;                       /* no terminator in the rest */
    }
    i = in;
    while (i < n && is_class(p[i]))
        i++;
    if (i == n)
        return 1;
    e = i + params(p + i, n - i);
    return e == n;
}

long pi_ansi_split(const char *in, long n)
{
    long start = n > PI_ANSI_MAX_PENDING ? n - PI_ANSI_MAX_PENDING : 0, j;
    int has = 0;
    for (j = 0; j < n; j++)
        if ((unsigned char)in[j] == ESC || ((unsigned char)in[j] == 0xC2 && j + 1 < n && (unsigned char)in[j + 1] == 0x9B))
            has = 1;
    if (!has)
        return n;
    for (j = start; j < n; j++)
        if (intro(in + j, n - j) && unfinished_from(in + j, n - j))
            return j;
    return n;
}

void pi_ansi_strip(const char *in, long n, jw *out)
{
    long i = 0, a = 0;
    while (i < n) {
        long m = 0;
        if (intro(in + i, n - i)) {
            m = osc_complete(in + i, n - i);
            if (!m)
                m = csi_complete(in + i, n - i);
        }
        if (m) {
            jw_raw(out, in + a, i - a);
            i += m;
            a = i;
        } else
            i++;
    }
    jw_raw(out, in + a, n - a);
}

void pi_ansi_clean(const char *in, long n, jw *out)
{
    jw tmp;
    long i;
    jw_init(&tmp);
    pi_ansi_strip(in, n, &tmp);
    for (i = 0; i < tmp.n; i++) {
        unsigned char c = (unsigned char)tmp.p[i];
        if (c <= 8 || c == 0x0B || c == 0x0C || c == 0x0D || (c >= 0x0E && c <= 0x1F))
            continue;
        if (c == 0xEF && i + 2 < tmp.n && (unsigned char)tmp.p[i + 1] == 0xBF &&
            (unsigned char)tmp.p[i + 2] >= 0xB9 && (unsigned char)tmp.p[i + 2] <= 0xBB) {
            i += 2;
            continue;
        }
        jw_raw(out, tmp.p + i, 1);
    }
    jw_free(&tmp);
}
