/* skills -- Agent Skills for the system prompt and /skill:name (plan phase I), as pi 1.1.0 loads them
 * (packages/coding-agent/src/core/skills.ts, core/resource-loader.ts, core/package-manager.ts addAutoDiscoveredResources
 * and resolveLocalEntries, core/agent-session.ts _expandSkillCommand):
 *
 * Where skills come from, in pi's order (the first of two with one name wins, the other is a "collision"):
 *   settings.json "skills" paths (the project's <cwd>/.pi/settings.json first, when trusted; then <agent dir>/settings.json),
 *   <cwd>/.pi/skills (trusted), <dir>/.agents/skills for the working directory and every ancestor up to the one holding
 *   .git (all the way up without one; trusted), <agent dir>/skills, <agent dir>/agents-skills (pi's ~/.agents/skills: the
 *   Amiga has no home), then --skill paths. --no-skills leaves out everything but --skill.
 * A directory holding SKILL.md is a skill and is not searched further; else its .md files count (only the root's, in
 * .pi/skills style; only the sub-directories', in .agents/skills style) and its sub-directories are searched. Names starting
 * with "." and node_modules are skipped; .gitignore, .ignore and .fdignore of each directory below the root hide files.
 * A skill needs a description (else it is skipped, with a warning for a SKILL.md); name (else the directory's) and
 * description are checked against the Agent Skills spec and only warn.
 *
 * The prompt section, /skill:name args and the startup report are built here; prompt.c, agent.c and ui_pi.c call them.
 * Portable C89 over claude/sys.h, host-tested (tests/test_pi_skills.c). */
#ifndef PI_SKILLS_H
#define PI_SKILLS_H

#include "claude/sys.h"
#include "claude/json.h"

enum { SKS_USER, SKS_PROJECT, SKS_TEMP };       /* pi's SourceScope: user, project, temporary (--skill) */
enum { SKO_AUTO, SKO_LOCAL };                   /* pi's SourceInfo.source: "auto" (a default directory), "local" (settings, --skill) */
enum { SKD_WARNING, SKD_ERROR, SKD_COLLISION }; /* pi's ResourceDiagnostic.type */

typedef struct pi_skill {
    char *name, *desc;
    char path[300];             /* filePath */
    char base[300];             /* baseDir: the skill's directory */
    int hidden;                 /* disable-model-invocation: true */
    int source, scope;          /* SKO_*, SKS_* */
    char sbase[300];            /* sourceInfo.baseDir: the .pi directory, the agent directory, the .agents directory, or a --skill skill's own; "" none */
} pi_skill;

typedef struct pi_skill_diag {
    int type;                   /* SKD_* */
    char *msg;
    char path[300];             /* the file (a collision: the loser) */
    char winner[300];           /* a collision: the file that kept the name */
    char *name;                 /* a collision: the name */
} pi_skill_diag;

typedef struct pi_skills {
    pi_skill *s;
    int n, cap;
    pi_skill_diag *d;
    int nd, capd;
    int commands;               /* settings.json enableSkillCommands (default 1): the skills are in the slash menu */
    long reads;                 /* skill files read by this load */
    cl_sys *sys;                /* how /skill:name reads a skill again (the load's) */
} pi_skills;

/* skill files read since the start, over every load and /skill expansion (the reachability tests' sentinel) */
extern long pi_skills_reads;

typedef struct pi_skill_opts {
    const char *agent_dir;      /* skills/, agents-skills/ and settings.json below it (0 none) */
    const char *data_dir;       /* its skills/ too, after the agent directory's, when it is another directory (ARCHITECTURE.md D10); 0 none */
    const char *cwd;
    int trusted;                /* the project's .pi/ and .agents/skills are used */
    int none;                   /* --no-skills */
    const char *const *paths;   /* --skill: files or directories */
    int npaths;
} pi_skill_opts;

/* all of the above: 0, -1 out of memory */
int pi_skills_load(pi_skills *k, cl_sys *sys, const pi_skill_opts *o);
void pi_skills_free(pi_skills *k);

/* pi's formatSkillsForPrompt(skills, reader).trim(): reader "read", "bash" or 0 (indirect); "" when no skill is visible
 * (disable-model-invocation hides one). The text of the "skills" prompt section. 0, -1 */
int pi_skills_format(const pi_skills *k, const char *reader, jw *out);

/* pi's _expandSkillCommand: text "/skill:name args" becomes the <skill> block (+ "\n\n" + args) into out: 1 expanded, 0 not
 * a skill command, an unknown skill or a file that cannot be read (the text goes on as typed); -1 out of memory */
int pi_skill_expand(const pi_skills *k, const char *text, jw *out);

/* the slash menu's entry for skill i: name "skill:<name>" and description "[u|p|t] <description>" (pi's
 * prefixAutocompleteDescription) */
void pi_skill_command(const pi_skill *s, char *name, long ncap, char *desc, long dcap);

/* pi's formatPathWithSource label of a skill's source: "auto (user)", "auto (project)", "user", "project" or "path (temp)" */
void pi_skill_label(const pi_skill *s, char *out, long cap);

/* the skills as get_commands lists them (rpc-mode.ts:701-708): [{"name":"skill:x","description":...,"source":"skill","sourceInfo":{...}}, ...]
 * appended to out, without the brackets and with a comma before each; n skills added */
int pi_skills_commands(const pi_skills *k, jw *out);

/* the startup report of pi's interactive mode (showLoadedResources): lines of text, each tagged with how it is drawn */
enum { SKL_LISTING, SKL_WARN, SKL_ERROR, SKL_COLL_HEAD, SKL_COLL_WIN, SKL_COLL_LOSE };
typedef void (*pi_skl_fn)(void *u, int kind, const char *text);
/* the "[Skills]" compact list ("  a, b": SKL_LISTING, one line; none without skills) */
void pi_skills_listing(const pi_skills *k, pi_skl_fn fn, void *u);
/* the "[Skill conflicts]" lines (formatDiagnostics); the number of lines */
int pi_skills_report(const pi_skills *k, pi_skl_fn fn, void *u);

#endif
