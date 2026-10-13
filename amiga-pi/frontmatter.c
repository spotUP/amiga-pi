/* see frontmatter.h */
#include <stdlib.h>
#include <string.h>
#include "frontmatter.h"

int pi_js_space_len(const unsigned char *p)
{
    if (p[0] == ' ' || (p[0] >= 9 && p[0] <= 13))
        return 1;
    if (p[0] == 0xC2 && p[1] == 0xA0)
        return 2;
    if (p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80)
        return 3;
    if (p[0] == 0xE2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xA8 || p[2] == 0xA9 || p[2] == 0xAF))
        return 3;
    if (p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F)
        return 3;
    if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80)
        return 3;
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        return 3;
    return 0;
}

int pi_js_space_back(const unsigned char *s, const unsigned char *e)
{
    int k;
    for (k = 1; k <= 3 && e - k >= s; k++)
        if (pi_js_space_len(e - k) == k)
            return k;
    return 0;
}

int pi_fm_parse(const char *content, long n, const char *const *keys, int nkeys, pi_yv *vals, char **body, long *bodylen, char *err,
                long errcap)
{
    char *norm = (char *)malloc((size_t)n + 4);
    long i = 0, o = 0, end = -1, bs = 0;
    int rc = 0, k;
    for (k = 0; k < nkeys; k++) {
        vals[k].kind = YV_NONE;
        vals[k].s = 0;
    }
    if (body)
        *body = 0;
    if (bodylen)
        *bodylen = 0;
    if (err && errcap > 0)
        err[0] = 0;
    if (!norm)
        return -1;
    /* stripBom, then \r\n and \r to \n (utils/frontmatter.ts normalizeNewlines) */
    if (n >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
        i = 3;
    for (; i < n; i++) {
        if (content[i] == '\r') {
            norm[o++] = '\n';
            if (i + 1 < n && content[i + 1] == '\n')
                i++;
        } else
            norm[o++] = content[i];
    }
    norm[o] = 0;
    /* extractFrontmatter: starts with "---", closed by the first "\n---" from index 3 */
    if (o >= 3 && !strncmp(norm, "---", 3)) {
        long j;
        for (j = 3; j + 3 < o + 0 && j < o; j++)
            if (norm[j] == '\n' && norm[j + 1] == '-' && norm[j + 2] == '-' && norm[j + 3] == '-') {
                end = j;
                break;
            }
    }
    if (end >= 0) {
        long yn = end > 4 ? end - 4 : 0;
        bs = end + 4;
        if (yn > 0) {
            rc = pi_yaml_keys(norm + 4, yn, keys, nkeys, vals, err, errcap);
            if (rc) {
                free(norm);
                return rc;
            }
        }
        if (body) {
            const unsigned char *b = (const unsigned char *)norm + bs, *e = (const unsigned char *)norm + o;
            char *t;
            while (b < e && pi_js_space_len(b))
                b += pi_js_space_len(b);
            while (e > b && pi_js_space_back(b, e))
                e -= pi_js_space_back(b, e);
            t = (char *)malloc((size_t)(e - b) + 1);
            if (!t) {
                free(norm);
                pi_yv_free(vals, nkeys);
                return -1;
            }
            memcpy(t, b, (size_t)(e - b));
            t[e - b] = 0;
            *body = t;
            if (bodylen)
                *bodylen = (long)(e - b);
        }
        free(norm);
        return 0;
    }
    if (body) {
        *body = norm;
        if (bodylen)
            *bodylen = o;
        return 0;
    }
    free(norm);
    return 0;
}
