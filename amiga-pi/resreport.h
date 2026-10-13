/* resreport -- the lines pi's showLoadedResources (interactive-mode.ts:1772-1930) writes about skills and prompt templates, one
 * implementation for both: the compact list ("  a, b"), the list grouped by scope that Ctrl+O shows instead (buildScopeGroups /
 * formatScopeGroups: the scope in the accent colour, the items sorted by path below it), and the conflicts block (formatDiagnostics:
 * a name two files have, with the winner marked and the files skipped, then warnings and errors with the file they are about).
 * Each line is handed to a callback with its kind and the form it belongs to. Portable C89, host-tested (tests/test_pi_tui.c,
 * test_pi_resources.c, test_pi_templates.c). */
#ifndef PI_RESREPORT_H
#define PI_RESREPORT_H

#include "pkgres.h"

/* how a line is drawn */
enum { RRL_ITEM, RRL_WARN, RRL_ERROR, RRL_COLL_HEAD, RRL_COLL_WIN, RRL_COLL_LOSE, RRL_SCOPE };
/* in which form of the block a line is: both, the compact one only, the expanded one only */
enum { RRM_BOTH, RRM_COLLAPSED, RRM_EXPANDED };

typedef void (*rr_fn)(void *u, int kind, int mode, const char *text);

/* a loaded resource: name for the compact list ("review", or "/review" for a template), shown for the expanded one (0: the path,
 * a home directory written as ~) */
typedef struct rr_item {
    const char *name;
    const char *path;
    const char *shown;
    int scope, kind;            /* PMS_*, PMK_* */
    const char *psrc;           /* PMK_PACKAGE: the package's source string */
} rr_item;

enum { RRD_WARNING, RRD_ERROR, RRD_COLLISION };

typedef struct rr_diag {
    int type;                   /* RRD_* */
    const char *msg;
    const char *path;           /* the file (a collision: the loser) */
    const char *winner;         /* a collision: the file that kept the name */
    const char *name;           /* a collision: the name */
} rr_diag;

/* what `~` stands for in a shown path (the home directory; 0 or "" none, as on the Amiga without HOME) */
void rr_set_home(const char *home);

/* pi's getDisplaySourceInfo label of a resource: "user", "project", "path (temp)", "auto (user)", or a package's source string
 * with its scope */
void rr_label(int scope, int kind, const char *psrc, char *out, long cap);

/* the compact list (one line, RRM_COLLAPSED) and the grouped list (RRM_EXPANDED); n == 0 writes nothing */
void rr_listing(const rr_item *it, int n, rr_fn fn, void *u);

/* the conflicts block: the collisions grouped by name, then the other diagnostics; the number of lines */
int rr_diagnostics(const rr_diag *d, int nd, const rr_item *it, int n, rr_fn fn, void *u);

#endif
