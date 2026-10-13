/* fuzzy -- pi-tui's fuzzy matcher (packages/tui/src/fuzzy.ts, pi 1.1.0) and the filter of its slash-command autocomplete
 * (autocomplete.ts:343-372), for the slash menu of amiga-pi's screen (tui.h slash_filter). Scores are pi's, in tenths
 * (pi adds i * 0.1 to a double; here the sum is exact). Names and queries are UTF-8 (lower_tab.h, tools/pi_oracle/gen_lower.mjs). Portable C89. */
#ifndef PI_FUZZY_H
#define PI_FUZZY_H

#include "claude/tui.h"

/* fuzzyMatch(query, text): 1 when every character of query is in text in order; *score (tenths, lower is better) */
int pi_fuzzy_match(const char *query, long qn, const char *text, long tn, long *score);
/* fuzzyFilter's test of one item: query split at whitespace and '/', every token must match; *score the sum.
 * 1 also when the query holds no token (fuzzyFilter then returns every item) */
int pi_fuzzy_item(const char *query, long qn, const char *text, long tn, long *score);
/* the menu of the slash commands for the box (len bytes) with the cursor at byte cur: cl_tui.slash_filter. Like pi's provider
 * it looks at the first line's text before the cursor only (leading whitespace dropped; it must start with '/' and hold no
 * space), else the menu is empty; *from the offset of that '/', the text [*from, cur) is what a chosen command replaces.
 * Names without their '/' are matched, a `skill:` name by what follows it first, the skills that did not match that way by
 * their whole name after them; each group in order of score, ties in menu order. Text and names are UTF-8, matched as
 * pi-tui does (lowercased, UTF-16 positions) */
int pi_slash_filter(void *u, const cl_cmd *cmds, int n, const char *box, long len, long cur, long *from, int *idx);

#endif
