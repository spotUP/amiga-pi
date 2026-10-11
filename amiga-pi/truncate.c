/* see truncate.h */
#include <string.h>
#include "truncate.h"
#include "claude/util.h"

/* the length of s[0..n) with the final newline (one) left out: pi splits
 * the text at '\n' and drops the empty piece after a closing newline */
static long body_len(const char *s, long n)
{
    return n > 0 && s[n - 1] == '\n' ? n - 1 : n;
}

void pi_truncate(const char *s, long n, int tail, long max_lines, long max_bytes, pi_trunc *r)
{
    long e = body_len(s, n), i, lines = 0;
    memset(r, 0, sizeof(*r));
    if (n > 0)
        for (lines = 1, i = 0; i < e; i++)
            lines += s[i] == '\n';
    r->total_lines = lines;
    r->n = n;
    r->out_lines = lines;
    if (lines <= max_lines && n <= max_bytes)
        return;
    r->by = PT_LINES;
    if (!tail) {
        /* whole lines from the front */
        long at = 0, kept = 0, bytes = 0;
        while (at <= e && kept < max_lines) {
            long le = at, add;
            while (le < e && s[le] != '\n')
                le++;
            add = (le - at) + (kept ? 1 : 0);
            if (bytes + add > max_bytes) {
                r->by = PT_BYTES;
                if (!kept)
                    r->first_over = 1;
                break;
            }
            bytes += add;
            kept++;
            if (le >= e)
                break;
            at = le + 1;
        }
        r->off = 0;
        r->n = bytes;
        r->out_lines = kept;
        if (kept >= max_lines && bytes <= max_bytes)
            r->by = PT_LINES;
    } else {
        /* whole lines from the end; one line over the limit keeps its end */
        long end = e, kept = 0, bytes = 0, start = e;
        while (end >= 0 && kept < max_lines) {
            long ls = end, add;
            while (ls > 0 && s[ls - 1] != '\n')
                ls--;
            add = (end - ls) + (kept ? 1 : 0);
            if (bytes + add > max_bytes) {
                r->by = PT_BYTES;
                if (!kept) {
                    long from = end - max_bytes;
                    while (from < end && ((unsigned char)s[from] & 0xc0) == 0x80)
                        from++;         /* not inside a UTF-8 character */
                    start = from;
                    bytes = end - from;
                    kept = 1;
                    r->partial = 1;
                }
                break;
            }
            bytes += add;
            kept++;
            start = ls;
            if (ls == 0)
                break;
            end = ls - 1;
        }
        r->off = start;
        r->n = bytes;
        r->out_lines = kept;
        if (kept >= max_lines && bytes <= max_bytes)
            r->by = PT_LINES;
    }
}

void pi_size(long b, char *out, long cap)
{
    char num[16];
    long tenths;
    const char *unit;
    if (b < 1024) {
        cl_ltoa(b, num);
        cl_copy(out, num, cap);
        cl_cat(out, "B", cap);
        return;
    }
    if (b < 1024L * 1024) {
        tenths = (b * 10 + 512) / 1024;
        unit = "KB";
    } else {
        tenths = (b / 1024 * 10 + 512) / 1024;
        unit = "MB";
    }
    cl_ltoa(tenths / 10, num);
    cl_copy(out, num, cap);
    cl_cat(out, ".", cap);
    cl_ltoa(tenths % 10, num);
    cl_cat(out, num, cap);
    cl_cat(out, unit, cap);
}

void pi_cut_path(const char *tmp, const char *id, char *out, long cap)
{
    long k;
    cl_copy(out, tmp ? tmp : "", cap);
    cl_cat(out, "amiga-pi-", cap);
    k = (long)strlen(out);
    for (; id && *id && k < cap - 5 && k < 200; id++)
        out[k++] = (*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') ||
                           *id == '-' || *id == '_'
                       ? *id
                       : '_';
    out[k] = 0;
    cl_cat(out, ".txt", cap);
}

int pi_cut(cl_sys *sys, const char *tmp, const char *id, const char *text, long n, int tail, jw *out)
{
    pi_trunc r;
    char path[300], a[32], b[32], num[16];
    int saved;
    pi_truncate(text, n, tail, PI_MAX_LINES, PI_MAX_BYTES, &r);
    if (r.by == PT_NONE) {
        jw_raw(out, text, n);
        return 0;
    }
    pi_cut_path(tmp, id, path, sizeof(path));
    saved = sys && sys->write && sys->write(sys->u, path, text, n) == 0;
    jw_raw(out, text + r.off, r.n);
    if (r.n)
        jw_rawz(out, "\n\n");
    jw_rawz(out, "[");
    if (r.first_over) {
        pi_size(n, a, sizeof(a));
        jw_rawz(out, "Line 1 is over the ");
        pi_size(PI_MAX_BYTES, b, sizeof(b));
        jw_rawz(out, b);
        jw_rawz(out, " limit (the text is ");
        jw_rawz(out, a);
        jw_rawz(out, ")");
    } else if (r.partial) {
        pi_size(r.n, a, sizeof(a));
        jw_rawz(out, "Showing last ");
        jw_rawz(out, a);
        jw_rawz(out, " of line ");
        cl_ltoa(r.total_lines, num);
        jw_rawz(out, num);
    } else {
        long first = tail ? r.total_lines - r.out_lines + 1 : 1;
        jw_rawz(out, "Showing lines ");
        cl_ltoa(first, num);
        jw_rawz(out, num);
        jw_rawz(out, "-");
        cl_ltoa(first + r.out_lines - 1, num);
        jw_rawz(out, num);
        jw_rawz(out, " of ");
        cl_ltoa(r.total_lines, num);
        jw_rawz(out, num);
        if (r.by == PT_BYTES) {
            pi_size(PI_MAX_BYTES, b, sizeof(b));
            jw_rawz(out, " (");
            jw_rawz(out, b);
            jw_rawz(out, " limit)");
        }
    }
    if (saved) {
        jw_rawz(out, ". Full output: ");
        jw_rawz(out, path);
    } else
        jw_rawz(out, ". The full output could not be kept");
    jw_rawz(out, "]");
    return 1;
}
