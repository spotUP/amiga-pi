/* changelog -- see changelog.h. pi: packages/coding-agent/src/utils/changelog.ts (parseChangelog, compareVersions, getNewEntries). */
#include <string.h>
#include "changelog.h"

#define CL_MAX 200

typedef struct entry {
    long a, b;                  /* the entry's text in the file: from its heading to the next */
    long maj, min, pat;
} entry;

/* "##\s+\[?(\d+)\.(\d+)\.(\d+)\]?" at the start of a line that starts with "## ": 1 and the numbers, else 0 */
static int heading(const char *s, long n, long *maj, long *min, long *pat)
{
    long i = 2, v[3], k;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i == 2)
        return 0;                       /* "##\s+": at least one blank after the hashes (the line began with "## " already) */
    if (i < n && s[i] == '[')
        i++;
    for (k = 0; k < 3; k++) {
        long x = 0, d = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            x = x * 10 + (s[i] - '0');
            i++;
            d++;
        }
        if (!d)
            return 0;
        v[k] = x;
        if (k < 2) {
            if (i >= n || s[i] != '.')
                return 0;
            i++;
        }
    }
    *maj = v[0];
    *min = v[1];
    *pat = v[2];
    return 1;
}

/* the entries in file order: a "## " line that is no version ends the entry before it and starts none */
static int parse(const char *t, long n, entry *e, int max)
{
    int ne = 0, open = 0;
    long a = 0;
    entry cur;
    memset(&cur, 0, sizeof(cur));
    while (a <= n) {
        long b = a;
        while (b < n && t[b] != '\n')
            b++;
        if (b - a >= 3 && t[a] == '#' && t[a + 1] == '#' && t[a + 2] == ' ') {
            if (open && ne < max) {
                cur.b = a;
                e[ne++] = cur;
            }
            open = 0;
            if (heading(t + a, b - a, &cur.maj, &cur.min, &cur.pat)) {
                cur.a = a;
                open = 1;
            }
        }
        if (b >= n)
            break;
        a = b + 1;
    }
    if (open && ne < max) {
        cur.b = n;
        e[ne++] = cur;
    }
    return ne;
}

/* the entry's text trimmed of whitespace at both ends, appended to out */
static void put(const char *t, const entry *e, jw *out)
{
    long a = e->a, b = e->b;
    while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\n' || t[b - 1] == '\r' || t[b - 1] == '\t'))
        b--;
    jw_raw(out, t + a, b - a);
}

int pi_cl_newer(const char *text, long n, const char *last, jw *out)
{
    entry e[CL_MAX];
    long l[3] = { 0, 0, 0 };
    int ne = parse(text, n, e, CL_MAX), i, k = 0, cnt = 0;
    const char *p = last;
    for (k = 0; k < 3 && p && *p; k++) {            /* lastVersion.split(".").map(Number): a part that is no number is 0 */
        long x = 0;
        int ok = 0;
        while (*p >= '0' && *p <= '9') {
            x = x * 10 + (*p - '0');
            p++;
            ok = 1;
        }
        l[k] = ok ? x : 0;
        while (*p && *p != '.')
            p++;
        if (*p == '.')
            p++;
    }
    for (i = 0; i < ne; i++) {
        long c = e[i].maj != l[0] ? e[i].maj - l[0] : e[i].min != l[1] ? e[i].min - l[1] : e[i].pat - l[2];
        if (c <= 0)
            continue;
        if (cnt++)
            jw_rawz(out, "\n\n");
        put(text, &e[i], out);
    }
    return cnt;
}

int pi_cl_all(const char *text, long n, jw *out)
{
    entry e[CL_MAX];
    int ne = parse(text, n, e, CL_MAX), i;
    for (i = ne - 1; i >= 0; i--) {
        put(text, &e[i], out);
        if (i)
            jw_rawz(out, "\n\n");
    }
    return ne;
}

void pi_cl_latest(const char *md, char *out, long cap)
{
    long n = (long)strlen(md), a = 0;
    out[0] = 0;
    while (a < n) {
        long b = a, maj, min, pat;
        const char *q;
        while (b < n && md[b] != '\n')
            b++;
        if (b - a >= 3 && md[a] == '#' && md[a + 1] == '#' && heading(md + a, b - a, &maj, &min, &pat)) {
            /* the match may sit anywhere in the text (pi: no ^ anchor): the first "##" followed by blanks and a version */
            q = md + a + 2;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '[')
                q++;
            {
                long k = 0;
                while (k < cap - 1 && ((q[k] >= '0' && q[k] <= '9') || q[k] == '.'))
                    k++;
                memcpy(out, q, (size_t)k);
                out[k] = 0;
            }
            return;
        }
        a = b + 1;
    }
}
