/* see truncate.h */
#include <string.h>
#include "truncate.h"
#include "claude/session.h"
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
    r->total_bytes = n;
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

void pi_trunc_json(jw *o, const pi_trunc *r, const char *text, long max_lines)
{
    jw_rawz(o, "{\"content\":");
    jw_str(o, r->first_over ? "" : text + r->off, r->first_over ? 0 : r->n);
    jw_rawz(o, ",\"truncated\":true,\"truncatedBy\":\"");
    jw_rawz(o, r->by == PT_LINES ? "lines" : "bytes");
    jw_rawz(o, "\",\"totalLines\":");
    jw_long(o, r->total_lines);
    jw_rawz(o, ",\"totalBytes\":");
    jw_long(o, r->total_bytes);
    jw_rawz(o, ",\"outputLines\":");
    jw_long(o, r->out_lines);
    jw_rawz(o, ",\"outputBytes\":");
    jw_long(o, r->first_over ? 0 : r->n);
    jw_rawz(o, ",\"lastLinePartial\":");
    jw_rawz(o, r->partial ? "true" : "false");
    jw_rawz(o, ",\"firstLineExceedsLimit\":");
    jw_rawz(o, r->first_over ? "true" : "false");
    jw_rawz(o, ",\"maxLines\":");
    if (max_lines >= 0x7fffffffL)
        jw_rawz(o, "9007199254740991");     /* Number.MAX_SAFE_INTEGER, what ls, find and grep pass */
    else
        jw_long(o, max_lines);
    jw_rawz(o, ",\"maxBytes\":");
    jw_long(o, PI_MAX_BYTES);
    jw_rawz(o, "}");
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

void pi_tmp_path(const char *tmp, const char *id, const char *ext, char *out, long cap)
{
    char stem[240];
    long k, room = 30 - (long)strlen(ext);
    cl_copy(stem, "amiga-pi-", sizeof(stem));
    k = (long)strlen(stem);
    for (; id && *id && k < 200; id++)
        stem[k++] = (*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') ||
                            *id == '-' || *id == '_'
                        ? *id
                        : '_';
    stem[k] = 0;
    /* FFS names are 30 characters: the extension (".txt") leaves the rest for the stem; a longer id
     * (Anthropic's toolu_...) keeps its end and a hash, as C:Claude's session names do */
    sess_fit(stem, sess_hash(stem), room < 12 ? 12 : room);
    cl_copy(out, tmp ? tmp : "", cap);
    cl_cat(out, stem, cap);
    cl_cat(out, ext, cap);
}

void pi_cut_path(const char *tmp, const char *id, char *out, long cap)
{
    pi_tmp_path(tmp, id, ".txt", out, cap);
}

int pi_cut(cl_sys *sys, const char *tmp, const char *id, const char *text, long n, int tail, jw *out, jw *details)
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
    if (details) {
        jw_reset(details);
        jw_rawz(details, "{\"truncation\":");
        pi_trunc_json(details, &r, text, PI_MAX_LINES);
        if (saved) {
            jw_rawz(details, ",\"fullOutputPath\":");
            jw_str(details, path, (long)strlen(path));
        }
        jw_rawz(details, "}");
    }
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
        long ls = r.off;
        while (ls > 0 && text[ls - 1] != '\n')
            ls--;
        pi_size(r.n, a, sizeof(a));
        jw_rawz(out, "Showing last ");
        jw_rawz(out, a);
        jw_rawz(out, " of line ");
        cl_ltoa(r.total_lines, num);
        jw_rawz(out, num);
        /* pi: the size of that whole line */
        pi_size(r.off + r.n - ls, b, sizeof(b));
        jw_rawz(out, " (line is ");
        jw_rawz(out, b);
        jw_rawz(out, ")");
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

void pi_head(const char *s, long n, const char *first, const char *last, jw *out, pi_trunc *tr)
{
    pi_trunc r;
    char b[32];
    int k = 0;
    pi_truncate(s, n, 0, 0x7fffffffL, PI_MAX_BYTES, &r);
    if (tr)
        *tr = r;
    jw_raw(out, s + r.off, r.n);
    if (first) {
        jw_rawz(out, "\n\n[");
        jw_rawz(out, first);
        k = 1;
    }
    if (r.by != PT_NONE) {
        jw_rawz(out, k ? ". " : "\n\n[");
        pi_size(PI_MAX_BYTES, b, sizeof(b));
        jw_rawz(out, b);
        jw_rawz(out, " limit reached");
        k = 1;
    }
    if (last) {
        jw_rawz(out, k ? ". " : "\n\n[");
        jw_rawz(out, last);
        k = 1;
    }
    if (k)
        jw_rawz(out, "]");
}
