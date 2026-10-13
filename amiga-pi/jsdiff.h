/* jsdiff -- the text diffs pi's edit tool shows, as the `diff` package (jsdiff 8.0.4, the version of pi 1.1.0) makes them:
 * Diff.diffLines and Diff.diffWords (the Myers walk of libesm/diff/base.js, the same tie-breaks, the tokenizers of line.js and
 * word.js, word.js's whitespace clean-up), and what pi builds on them: generateDiffString and generateUnifiedPatch
 * (core/tools/edit-diff.ts:365-480). Portable C89; tests/test_pi_tools.c holds jsdiff's own cases from Node (tests/pi/diff). */
#ifndef PI_JSDIFF_H
#define PI_JSDIFF_H

#include "claude/json.h"

typedef struct jd_part {
    int added, removed;         /* both 0: common to both texts */
    char *v;                    /* the part's text (zero-terminated) */
    long n;
} jd_part;

typedef struct jd_res {
    jd_part *p;
    long n;
} jd_res;

/* Diff.diffLines(a, b) / Diff.diffWords(a, b): 0, or -1 when memory (or the walk's bound, JD_MAX_NODES) runs out */
int jd_lines(const char *a, long an, const char *b, long bn, jd_res *r);
int jd_words(const char *a, long an, const char *b, long bn, jd_res *r);
void jd_free(jd_res *r);

/* generateDiffString(oldContent, newContent) with 4 lines of context: the diff text ("+12 new", "-12 old", " 12 same",
 * "    ..." ) joined by newlines into diff; *first is firstChangedLine, 0 when there is none. 0, -1 out of memory */
int pi_edit_diff(const char *a, long an, const char *b, long bn, jw *diff, long *first);
/* generateUnifiedPatch(path, a, b): "--- path", "+++ path" and the hunks, each line ending in a newline */
int pi_edit_patch(const char *path, const char *a, long an, const char *b, long bn, jw *patch);

#endif
