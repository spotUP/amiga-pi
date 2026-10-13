/* fuzzy -- pi-tui's fuzzy matcher (packages/tui/src/fuzzy.ts, pi 1.1.0) and the filter of its slash-command autocomplete
 * (autocomplete.ts:343-372), for the slash menu of amiga-pi's screen (tui.h slash_filter). Scores are pi's, in tenths
 * (pi adds i * 0.1 to a double; here the sum is exact). Names are ASCII. Portable C89. */
#ifndef PI_FUZZY_H
#define PI_FUZZY_H

#include "claude/tui.h"

/* fuzzyMatch(query, text): 1 when every character of query is in text in order; *score (tenths, lower is better) */
int pi_fuzzy_match(const char *query, long qn, const char *text, long tn, long *score);
/* fuzzyFilter's test of one item: query split at whitespace and '/', every token must match; *score the sum.
 * 1 also when the query holds no token (fuzzyFilter then returns every item) */
int pi_fuzzy_item(const char *query, long qn, const char *text, long tn, long *score);
/* the menu of the slash commands for the box's text (starting with '/'): cl_tui.slash_filter. Names without their '/' are
 * matched, a `skill:` name by what follows it first, the skills that did not match that way by their whole name after them;
 * each group in order of score, ties in menu order */
int pi_slash_filter(void *u, const cl_cmd *cmds, int n, const char *text, long len, int *idx);

#endif
