/* tree_pi -- /tree as pi's selector (plan N3; pi tree-selector.js): the session index (session3.h) drawn with
 * its branches, the active path marked, labels, the filters (default, no-tools, user-only, labeled-only, all),
 * search by typing, folding of branch segments; Enter moves the leaf (cmds.c navigate, with pi's "Summarize
 * branch?" question). Rows come out of the index, never from a second copy. Portable C89. */
#ifndef PI_TREE_H
#define PI_TREE_H

#include "claude/tui.h"
#include "store.h"

/* the filters, in pi's cycle order */
enum { TF_DEFAULT, TF_NO_TOOLS, TF_USER, TF_LABELED, TF_ALL, TF_COUNT };

/* the selector over st's session; at the start the cursor is on the entry `select` (-1: the leaf). Returns the
 * entry chosen (a tree index), -1 cancelled or nothing to show; *filter is kept across calls */
int pi_tree_pick(pi_store *st, cl_tui *t, int select, int *filter);

/* the text of an entry as the tree shows it, SGR kept (selected: bold); the tests read it */
void pi_tree_entry_text(pi_store *st, int idx, const cl_pitheme *th, int selected, jw *out);

#endif
