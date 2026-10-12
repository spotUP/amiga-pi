/* templates -- prompt templates (plan phase J; pi 1.1.0 packages/coding-agent/src/core/prompt-templates.ts).
 *
 * The .md files of <agent dir>/prompts and, for a trusted project, of <cwd>/.pi/prompts (not recursive, names starting
 * with "." skipped, as package-manager.ts collectAutoPromptEntries) become /name. The project's come first, so a
 * name in both is the project's (resource-loader dedupePrompts keeps the first). A file's frontmatter gives
 * description and argument-hint; with no description the first non-empty line, cut at 60 characters and "..."
 * after it. The body is what follows the frontmatter, trimmed; a file with none is taken as it is.
 *
 * Typing /name args expands to the body with the arguments substituted (expandPromptTemplate): pi_prompt does it
 * for every front end (agent-session.ts prompt()), so a line the UI, RPC or the SDK sends is expanded alike.
 *
 * The substitution is one pure function (pi_tmpl_subst), the argument split another (pi_tmpl_args); neither
 * touches the disk. claude/commands.c's expansion has another syntax ($0 is the first argument, $ARGUMENTS[N],
 * \$, named arguments), so the two share only the frontmatter reader (defs_parse), not the substitution.
 * Portable C89 over claude/sys.h, host-tested (tests/test_pi_templates.c). */
#ifndef PI_TEMPLATES_H
#define PI_TEMPLATES_H

#include "claude/sys.h"
#include "claude/json.h"
#include "claude/tui.h"

/* pi's parseCommandArgs: the words of s, quotes ' and " group (and are dropped), JavaScript's whitespace splits,
 * a word that is empty is not a word. *out is an array of *n malloc'ed strings (0 when none): 0, -1 out of memory */
int pi_tmpl_args(const char *s, char ***out, int *n);
void pi_tmpl_args_free(char **a, int n);

/* pi's substituteArgs: content with $1.. $@ $ARGUMENTS ${N:-d} ${@:-d} ${ARGUMENTS:-d} ${@:N} ${@:N:L} replaced in
 * one pass (an argument or a default holding "$1" is not substituted again), appended to out */
void pi_tmpl_subst(jw *out, const char *content, char *const *args, int nargs);

enum { PI_TMPL_USER, PI_TMPL_PROJECT, PI_TMPL_PATH };    /* the agent directory, <cwd>/.pi/prompts, a --prompt-template path */

typedef struct pi_tmpl {
    char name[108];
    char *description;          /* the frontmatter's or the first line's, "" none */
    char *hint;                 /* argument-hint, "" none */
    char *content;
    char path[300];
    int scope;                  /* PI_TMPL_*: where it was found */
    char base[300];             /* PI_TMPL_PATH: the directory it was found in (sourceInfo.baseDir) */
} pi_tmpl;

typedef struct pi_tmpls {
    pi_tmpl *t;
    int n, cap;
    cl_cmd *menu;               /* the slash menu's rows, one per template (pi_tmpl_menu) */
    char **menu_text;           /* what menu points to: name then help, each its own allocation */
    char base_user[300];        /* sourceInfo.baseDir of the two scopes (rpc get_commands) */
    char base_project[300];
} pi_tmpls;

/* files read and templates expanded since the start (the reachability tests' sentinels) */
extern long pi_tmpl_reads, pi_tmpl_expansions;

/* The templates of the paths (--prompt-template: files or directories, relative to cwd) first, then, unless
 * no_defaults (--no-prompt-templates), the project's when trusted and the agent directory's (resource-loader.ts
 * mergePaths): 0, -1 out of memory */
int pi_tmpl_load(pi_tmpls *ts, cl_sys *sys, const char *agent_dir, const char *cwd, int trusted, int no_defaults,
                 char (*paths)[300], int npaths);
void pi_tmpl_free(pi_tmpls *ts);

/* pi's expandPromptTemplate: 1 when text is /name [args] of a template (the expansion appended to out), 0 else */
int pi_tmpl_expand(const pi_tmpls *ts, const char *text, jw *out);

/* the rows of the slash menu as interactive-mode.ts builds them from promptTemplates: name, description
 * prefixed by the scope's tag ([u] user, [p] project), the argument hint and " \342\200\224 " before it */
const cl_cmd *pi_tmpl_menu(pi_tmpls *ts, int *n);

#endif
