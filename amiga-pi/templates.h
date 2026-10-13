/* templates -- prompt templates (plan phase J; pi 1.1.0 packages/coding-agent/src/core/prompt-templates.ts).
 *
 * The .md files of <agent dir>/prompts and, for a trusted project, of <cwd>/.pi/prompts (not recursive, names starting
 * with "." skipped, ignore files honoured, as package-manager.ts collectAutoPromptEntries), of the "prompts" entries and
 * packages of settings.json (pkgres.h) and of --prompt-template paths become /name. The project's come first, so a
 * name in both is the project's (resource-loader dedupePrompts keeps the first). A file's frontmatter gives
 * description and argument-hint; with no description the first non-empty line, cut at 60 characters and "..."
 * after it. The body is what follows the frontmatter, trimmed; a file with none is taken as it is.
 *
 * Typing /name args expands to the body with the arguments substituted (expandPromptTemplate): pi_prompt does it
 * for every front end (agent-session.ts prompt()), so a line the UI, RPC or the SDK sends is expanded alike.
 *
 * The substitution is one pure function (pi_tmpl_subst), the argument split another (pi_tmpl_args); neither
 * touches the disk. claude/commands.c's expansion has another syntax ($0 is the first argument, $ARGUMENTS[N],
 * \$, named arguments), so the two share nothing; the frontmatter is read by frontmatter.h, as skills do.
 * Portable C89 over claude/sys.h, host-tested (tests/test_pi_templates.c). */
#ifndef PI_TEMPLATES_H
#define PI_TEMPLATES_H

#include "claude/sys.h"
#include "claude/json.h"
#include "claude/tui.h"
#include "pkgres.h"

/* pi's parseCommandArgs: the words of s, quotes ' and " group (and are dropped), JavaScript's whitespace splits,
 * a word that is empty is not a word. *out is an array of *n malloc'ed strings (0 when none): 0, -1 out of memory */
int pi_tmpl_args(const char *s, char ***out, int *n);
void pi_tmpl_args_free(char **a, int n);

/* pi's substituteArgs: content with $1.. $@ $ARGUMENTS ${N:-d} ${@:-d} ${ARGUMENTS:-d} ${@:N} ${@:N:L} replaced in
 * one pass (an argument or a default holding "$1" is not substituted again), appended to out */
void pi_tmpl_subst(jw *out, const char *content, char *const *args, int nargs);

enum { TD_WARNING, TD_ERROR, TD_COLLISION };    /* pi's ResourceDiagnostic.type */

typedef struct pi_tmpl_diag {
    int type;                   /* TD_* */
    char *msg;
    char path[300];             /* the file (a collision: the loser) */
    char winner[300];           /* a collision: the file that kept the name */
    char *name;                 /* a collision: the name, without the slash */
} pi_tmpl_diag;

typedef struct pi_tmpl {
    char name[108];
    char *description;          /* the frontmatter's or the first line's, "" none */
    char *hint;                 /* argument-hint, "" none */
    char *content;
    char path[300];
    int scope;                  /* sourceInfo.scope: PMS_USER, PMS_PROJECT, PMS_TEMP (pkgres.h) */
    int kind;                   /* sourceInfo.source: PMK_AUTO ("auto"), PMK_LOCAL ("local"), PMK_PACKAGE (the package's source string) */
    char base[300];             /* sourceInfo.baseDir ("" none) */
    char psrc[300];             /* PMK_PACKAGE: the package's source string */
} pi_tmpl;

typedef struct pi_tmpls {
    pi_tmpl *t;
    int n, cap;
    pi_tmpl_diag *d;            /* warnings (a file that cannot be read, YAML that is refused), errors (a path that is not there), collisions */
    int nd, capd;
    cl_cmd *menu;               /* the slash menu's rows, one per template (pi_tmpl_menu) */
    char **menu_text;           /* what menu points to: name then help, each its own allocation */
} pi_tmpls;

/* files read and templates expanded since the start (the reachability tests' sentinels) */
extern long pi_tmpl_reads, pi_tmpl_expansions;

typedef struct pi_tmpl_opts {
    const char *agent_dir;
    const char *cwd;
    int trusted;                /* <cwd>/.pi/prompts and the project's settings.json are used */
    int no_defaults;            /* --no-prompt-templates: only the paths */
    const char *home;           /* what ~ is in a path (pkgres.h) */
    int host;                   /* host paths rather than AmigaDOS paths */
    char (*paths)[300];         /* --prompt-template: files or directories, relative to cwd */
    int npaths;
} pi_tmpl_opts;

/* pi's loadPromptTemplates over what DefaultPackageManager.resolve() finds (pkgres.h: settings.json "prompts" entries and
 * packages, <agent dir>/prompts, <cwd>/.pi/prompts, in precedence order), then the paths (resource-loader.ts mergePaths: a
 * file once by its canonical name); the first template of a name keeps it (dedupePrompts) and the others are collisions.
 * 0, -1 out of memory */
int pi_tmpl_load(pi_tmpls *ts, cl_sys *sys, const pi_tmpl_opts *o);
void pi_tmpl_free(pi_tmpls *ts);

/* pi's expandPromptTemplate: 1 when text is /name [args] of a template (the expansion appended to out), 0 else */
int pi_tmpl_expand(const pi_tmpls *ts, const char *text, jw *out);

/* the rows of the slash menu as interactive-mode.ts builds them from promptTemplates: name, description
 * prefixed by the scope's tag ([u] user, [p] project), the argument hint and " \342\200\224 " before it */
const cl_cmd *pi_tmpl_menu(pi_tmpls *ts, int *n);

/* the templates as get_commands lists them (rpc-mode.ts:690-696): [{"name":"review","description":...,"source":"prompt","sourceInfo":{...}}, ...]
 * appended to out, without the brackets and with a comma before each but the first; n templates added */
int pi_tmpls_commands(const pi_tmpls *ts, jw *out);

#endif
