/* pkgres -- where skills and prompt templates come from: pi 1.1.0's DefaultPackageManager.resolve() (core/package-manager.ts) for
 * those two resource types, the one place skills.c and templates.c ask. In pi's order:
 *   the "skills" / "prompts" entries of settings.json (the project's <cwd>/.pi/settings.json when the project is trusted, then
 *   <agent dir>/settings.json): paths (a file, or a directory searched for resources), and patterns that switch resources off
 *   or on (`!glob`, `+path`, `-path`, bare globs); the "packages" of both settings files that are local paths (a directory with
 *   a package.json "pi" manifest, or with skills/ and prompts/ directories; an object entry filters what it brings); the
 *   directories searched without being asked for (<dir>/skills and <dir>/prompts of the project's .pi and of the agent
 *   directory, .agents/skills of the working directory and each ancestor up to the one holding .git, the user's agents-skills);
 *   every resource is then ranked as resourcePrecedenceRank does (project settings entry, project auto, user settings entry,
 *   user auto, package) and a file reached twice counts once.
 * Names, paths and patterns: resolvePath(input, base, {homeDir, trim}) (utils/paths.ts: trimmed, `~` and `~/` the home
 * directory, file:// URLs); patterns are minimatch's (case of letters counts), the ignore files (.gitignore, .ignore,
 * .fdignore) are the ignore package's (case does not count).
 * Not done: packages from npm and git (they need a JavaScript runtime and the net); extglob patterns, and minimatch's rule
 * that `*` does not match a name starting with a dot.
 * Portable C89 over claude/sys.h, host-tested (tests/test_pi_resources.c, test_pi_templates.c). */
#ifndef PI_PKGRES_H
#define PI_PKGRES_H

#include "claude/sys.h"

enum { PM_SKILLS, PM_PROMPTS };                 /* the resource type */
enum { PMS_USER, PMS_PROJECT, PMS_TEMP };       /* pi's SourceScope */
enum { PMK_AUTO, PMK_LOCAL, PMK_PACKAGE };      /* PathMetadata.source: "auto", "local", or the package's source string (origin "package") */

typedef struct pm_res {
    char path[300];             /* the file */
    int enabled;                /* settings.json patterns switch resources off: such a file is listed, not enabled */
    int scope, kind;            /* PMS_*, PMK_* */
    char base[300];             /* PathMetadata.baseDir: the .pi directory, the agent directory, the .agents directory, the package; "" none */
    char src[300];              /* PMK_PACKAGE: the package's source string as settings.json has it */
} pm_res;

typedef struct pm_list {
    pm_res *e;
    int n, cap;
} pm_list;

typedef struct pm_opts {
    const char *agent_dir;      /* skills/, prompts/, agents-skills/ and settings.json below it (0 none) */
    const char *data_dir;       /* skills/ below it too (skills only), when it is another directory (ARCHITECTURE.md D10); 0 none */
    const char *cwd;
    int trusted;                /* the project's .pi/ and .agents/skills are used */
    const char *home;           /* what `~` is; 0: "SYS:" (C:Claude's ~/ without HOME) */
    int host;                   /* host paths: a leading "/" is the root; else AmigaDOS paths, where it is the parent directory */
} pm_opts;

/* pi's resolve() for one resource type: every resource in precedence order, a file once, enabled or not. *commands gets
 * settings.json's enableSkillCommands (the project's over the user's; -1 neither says). 0, -1 out of memory */
int pm_resolve(cl_sys *sys, const pm_opts *o, int rtype, pm_list *out, int *commands);
void pm_free(pm_list *l);

/* pi's resolvePath(input, base, {homeDir, trim}): the entry trimmed (when trim), `~` and `~/x` from home, file:// as a path, else against
 * base (a path with a colon or, for host paths, a leading "/" is absolute). 0, -1 too long */
int pm_path(const pm_opts *o, const char *input, const char *base, int trim, char *out, long cap);

/* settings.json's enableSkillCommands alone (the project's over the user's), -1 when neither says: *commands */
void pm_commands(cl_sys *sys, const pm_opts *o, int *commands);

/* the skill files of a directory given to --skill (loadSkillsFromDirInternal: SKILL.md ends the search, else the .md files of the
 * directory and SKILL.md below it, with the ignore files), each to fn. 0, -1 */
typedef void (*pm_path_fn)(void *u, const char *path);
int pm_skill_dir(cl_sys *sys, const char *dir, pm_path_fn fn, void *u);

/* dedupe helper for callers: the canonical name of a path (symlinks resolved), the path itself when the host cannot */
void pm_canon(cl_sys *sys, const char *path, char *out, long cap);

#endif
