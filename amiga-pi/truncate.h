/* truncate -- pi's output limits for every tool (plan D5; pi f10993bc
 * packages/coding-agent/src/core/tools/truncate.ts): at most PI_MAX_LINES
 * lines and PI_MAX_BYTES bytes reach the model. read, grep, find and ls
 * keep the head (whole lines only), bash the tail (the last line may be
 * cut at its front). When text is cut, all of it is kept in
 * <tmp>amiga-pi-<call id>.txt (T: on the Amiga) and the result names that
 * file. One helper for every tool; MCP (phase K) uses it too.
 * Portable C89 over cl_sys, host-tested (tests/test_pi_tools.c). */
#ifndef PI_TRUNCATE_H
#define PI_TRUNCATE_H

#include "claude/json.h"
#include "claude/sys.h"

#define PI_MAX_LINES 2000
#define PI_MAX_BYTES (50L * 1024)

enum { PT_NONE, PT_LINES, PT_BYTES };

typedef struct pi_trunc {
    long off, n;                /* what is kept: text[off .. off+n) */
    long total_lines, out_lines;
    int by;                     /* PT_*: what cut it */
    int first_over;             /* head: the first line alone is over the byte limit (nothing kept) */
    int partial;                /* tail: the one kept line is cut at its front */
} pi_trunc;

/* truncateHead (tail 0) / truncateTail (tail 1) of text[0..n) */
void pi_truncate(const char *text, long n, int tail, long max_lines, long max_bytes, pi_trunc *r);
/* pi's formatSize: "512B", "50.0KB", "1.2MB" */
void pi_size(long bytes, char *out, long cap);
/* The tool's text as the model gets it, appended to out: text itself when
 * it fits; else the kept part and pi's notice ("[Showing lines 1-2000 of
 * 5000. Full output: T:amiga-pi-call_1.txt]"), the whole text written to
 * that file first. 1 when cut, 0 not cut. */
int pi_cut(cl_sys *sys, const char *tmp, const char *id, const char *text, long n, int tail, jw *out);
/* pi's ending of grep, find and ls (no temp file): s[0..n) cut to
 * PI_MAX_BYTES (whole lines, no line limit) into out, then the notices
 * that apply joined by ". " in one "\n\n[...]": first (the limit's, or 0),
 * "50.0KB limit reached" when cut, last (grep's long lines, or 0) */
void pi_head(const char *s, long n, const char *first, const char *last, jw *out);
/* the file pi_cut keeps text in for call id: <tmp>amiga-pi-<id>.txt, the id's
 * characters other than letters, digits, '-' and '_' as '_' */
void pi_cut_path(const char *tmp, const char *id, char *out, long cap);
/* the same with another extension (".png", ".bin"; MCP's binary resources), the name at most 30 */
void pi_tmp_path(const char *tmp, const char *id, const char *ext, char *out, long cap);

#endif
