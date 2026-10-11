/* resources -- what the system prompt takes from the disk (plan phase G), as pi
 * f10993bc core/resource-loader.ts and core/trust-manager.ts load it:
 *
 * Context files: the agent directory's first, then the working directory's
 * ancestors from the volume down and the working directory itself; in each
 * directory the first of AGENTS.override.md, AGENTS.md, AGENTS.MD, CLAUDE.md,
 * CLAUDE.MD that is a file (C:Claude's walk, claude/memory.c mem_walk, with
 * pi's candidates); each file once, its text as it is (a UTF-8 BOM dropped).
 * They load whether the project is trusted or not (pi's docs/security.md).
 *
 * SYSTEM.md and APPEND_SYSTEM.md: <cwd>/.pi/<name> when the project is trusted
 * and it is there, else <agent dir>/<name>. SYSTEM.md replaces the default
 * prompt's prefix, APPEND_SYSTEM.md becomes the "addendum" section (prompt.c).
 *
 * Project trust: needed when <cwd>/.pi holds one of pi's protected names
 * (settings.json, mcp.json, extensions, skills, prompts, themes, SYSTEM.md,
 * APPEND_SYSTEM.md) or the working directory or an ancestor holds
 * .agents/skills. Decided by -a / -na, else the nearest saved decision in
 * <agent dir>/trust.json (claude/trust.c trust_near), else settings.json's
 * defaultProjectTrust (ask, always, never), else the question when there is
 * someone to ask (pi's five answers; print mode: not trusted).
 * Portable C89 over claude/sys.h, host-tested (tests/test_pi_resources.c). */
#ifndef PI_RESOURCES_H
#define PI_RESOURCES_H

#include "claude/sys.h"
#include "claude/json.h"

#define PI_CTX_FILES 16
#define PI_RES_MAX (256L * 1024)        /* one file at most */

typedef struct pi_ctxfile {
    char path[300];                     /* as pi names it: the directory joined with the candidate's name */
    char *text;
    long n;
} pi_ctxfile;

typedef struct pi_res {
    pi_ctxfile f[PI_CTX_FILES];
    int nf;
    char *system;                       /* SYSTEM.md's text, 0 none */
    char system_path[300];
    char *append;                       /* APPEND_SYSTEM.md's text, 0 none */
    char append_path[300];
    int trusted;                        /* the project's .pi/ resources were loaded */
    long reads;                         /* files read (the tests' sentinel) */
} pi_res;

/* The resources for cwd (canonical: NameFromLock) and the agent directory (0 none); trusted as
 * decided (pi_trust_decide). 0, -1 out of memory. */
int pi_res_load(pi_res *r, cl_sys *sys, const char *agent_dir, const char *cwd, int trusted);
void pi_res_free(pi_res *r);

/* pi's hasTrustRequiringProjectResources: 1 when cwd has something trust guards */
int pi_trust_needed(cl_sys *sys, const char *cwd);

/* the answers pi offers (getProjectTrustOptions with the session-only ones) */
enum { PI_TRUST_YES, PI_TRUST_PARENT, PI_TRUST_SESSION, PI_TRUST_NO, PI_TRUST_NO_SESSION };

/* the question: title (pi's text), the answers' labels (n of them, PI_TRUST_* order with
 * "Trust parent folder" left out when cwd has no parent); the index picked, -1 none */
typedef int (*pi_trust_ask_fn)(void *u, const char *title, const char *const *labels, int n);

/* pi's resolveProjectTrusted: override 1 / 0 (-a, -na), -1 none; dflt settings.json's
 * defaultProjectTrust ("ask" when 0); ask 0 when nobody can answer. An answer that is kept is
 * written to <agent dir>/trust.json. 1 trusted, 0 not. */
int pi_trust_decide(cl_sys *sys, const char *agent_dir, const char *cwd, int override, const char *dflt,
                    pi_trust_ask_fn ask, void *ask_u);

/* /trust (pi: "Save project trust decision for future sessions"): pick 0 lists the answers that can
 * be kept (Trust, Trust parent folder, Do not trust) and the decision in force into out; pick 1.. saves
 * that one and says so. 0, -1 (a bad pick, or trust.json could not be written: out says why) */
int pi_trust_command(cl_sys *sys, const char *agent_dir, const char *cwd, int pick, jw *out);

#endif
