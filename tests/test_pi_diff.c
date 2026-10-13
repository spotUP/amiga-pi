/* jsdiff (U1b): Diff.diffLines / generateDiffString / generateUnifiedPatch and Diff.diffWords against what pi 1.1.0's own packages make of
 * 1500+ pairs of texts (tests/pi/diff/cases.json, tools/pi_oracle/gen_diff.mjs): the edit tool's diff and patch word for word, the
 * word parts of the intra-line diff one by one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/json.h"
#include "../amiga-pi/jsdiff.h"

#ifndef PI_TESTS
#define PI_TESTS "tests"
#endif

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
    *n = len;
    return b;
}

static char *str_of(jv v, long *n)
{
    return json_strdup(v, n);
}

void suite_pi_diff(void)
{
    long n = 0, nlines = 0, nwords = 0, bad = 0;
    char *cases = slurp(PI_TESTS "/pi/diff/cases.json", &n);
    jv arr, row;
    jit it;
    CHECK(cases != 0);
    if (!cases)
        return;
    CHECK_INT(json_parse(cases, n, &arr), 0);
    json_iter(arr, &it);
    while (json_next(&it, 0, &row)) {
        jit in;
        jv m[6];
        int cnt = 0, ok = 1;
        char *sa, *sb;
        long la, lb;
        json_iter(row, &in);
        while (cnt < 6 && json_next(&in, 0, &m[cnt]))
            cnt++;
        if (cnt < 4)
            continue;
        sa = str_of(m[1], &la);
        sb = str_of(m[2], &lb);
        if (json_long(m[0], 0) == 0) {
            long first = 0, wn, pn;
            char *want = str_of(m[3], &wn), *wpatch = str_of(m[5], &pn);
            jw diff, patch;
            nlines++;
            jw_init(&diff);
            jw_init(&patch);
            pi_edit_diff(sa, la, sb, lb, &diff, &first);
            pi_edit_patch("f.txt", sa, la, sb, lb, &patch);
            jw_rawz(&diff, "");
            jw_rawz(&patch, "");
            if (strcmp(diff.p ? diff.p : "", want ? want : "") || first != json_long(m[4], 0) || strcmp(patch.p ? patch.p : "", wpatch ? wpatch : "")) {
                ok = 0;
                if (bad < 3)
                    printf("  lines [%.60s] -> [%.60s]\n   want first %ld: %.200s\n   got  first %ld: %.200s\n   patch want %.160s\n   patch got  %.160s\n", sa, sb,
                           json_long(m[4], 0), want, first, diff.p, wpatch ? wpatch : "", patch.p);
            }
            jw_free(&diff);
            jw_free(&patch);
            free(want);
            free(wpatch);
        } else {
            jd_res r;
            jit parts;
            jv p;
            long k = 0;
            nwords++;
            if (jd_words(sa, la, sb, lb, &r)) {
                ok = 0;
            } else {
                json_iter(m[3], &parts);
                while (json_next(&parts, 0, &p)) {
                    jit pi;
                    jv fl, val;
                    char *want;
                    long lc;
                    json_iter(p, &pi);
                    json_next(&pi, 0, &fl);
                    json_next(&pi, 0, &val);
                    want = str_of(val, &lc);
                    if (k >= r.n || r.p[k].added != (json_long(fl, 0) == 1) || r.p[k].removed != (json_long(fl, 0) == 2) || r.p[k].n != lc ||
                        memcmp(r.p[k].v, want, (size_t)lc))
                        ok = 0;
                    free(want);
                    k++;
                }
                if (k != r.n)
                    ok = 0;
                if (!ok && bad < 3) {
                    long q;
                    printf("  words [%s] -> [%s]\n   got:", sa, sb);
                    for (q = 0; q < r.n; q++)
                        printf(" (%d)[%s]", r.p[q].added ? 1 : r.p[q].removed ? 2 : 0, r.p[q].v);
                    printf("\n");
                }
                jd_free(&r);
            }
        }
        if (!ok)
            bad++;
        free(sa);
        free(sb);
    }
    CHECK_INT(bad, 0);
    CHECK(nlines > 700 && nwords > 1500);
    free(cases);
}
