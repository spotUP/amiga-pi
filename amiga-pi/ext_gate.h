/* ext_gate -- C:Claude's ask-before-acting gate as a built-in extension
 * (plan D6, decision D6: pi never asks, so the gate is off by default).
 * On every tool_call it decides with C:Claude's own code:
 *   the permission rules of settings.json (claude/config.c: Tool or
 *   Tool(pattern) under permissions.allow / ask / deny, deny over ask over
 *   allow), on the call mapped to C:Claude's tool and input
 *   (pi_tools_claude: read is Read, edit Edit, bash Bash, ls and find Glob);
 *   no rule: C:Claude's default (perm_must_ask) -- reads inside the working
 *   directory run, everything else asks; "yes, this session" keeps that
 *   tool allowed; "yes, always in this project" keeps C:Claude's rule for
 *   the call (cfg_rule_for: "Bash(make *)", "Edit") in the project's file
 *   under the data directory: <data dir>/permissions/--<cwd>--.json (the
 *   working directory's name as pi names its session directories: '/', '\'
 *   and ':' as '-'), read back as a local settings file at the next start.
 * The question is the session's ask (the prompt's ui_ask; print mode has
 * none, so a call that would ask is refused). Switched by
 * "permissionGate": true in ENVARC:amiga-pi/settings.json, --gate and
 * --no-gate on the command line (the flag wins).
 * Portable C89, host-tested (tests/test_pi_tools.c). */
#ifndef PI_EXT_GATE_H
#define PI_EXT_GATE_H

#include "claude/config.h"
#include "tools_pi.h"

typedef struct pi_gate {
    pi_ext ext;
    pi_tools *tools;            /* the working directory, C:Claude's permission state */
    pi_session *s;              /* its ask */
    cl_settings cfg;            /* the rules */
    char proj[400];             /* the project's kept rules (settings.json form), "" none */
    int on;
    long questions, blocks, kept;   /* the tests' sentinels (kept: rules written for the project) */
} pi_gate;

void pi_gate_init(pi_gate *g, pi_tools *tools, pi_session *s);
void pi_gate_free(pi_gate *g);
/* the project's file for cwd under data_dir named and its rules taken
 * (when it is there): 0, -1 the name too long */
int pi_gate_project(pi_gate *g, const char *data_dir, const char *cwd);
/* the file name of cwd's kept rules: <data_dir>/permissions/--<cwd>--.json */
int pi_gate_project_file(const char *data_dir, const char *cwd, char *out, long cap);
/* a settings.json text: its permissions rules taken, "permissionGate"
 * read (-1 when absent, 0, 1); 0, -1 not a JSON object */
int pi_gate_settings(pi_gate *g, const char *json, long n, const char *name, int *gate);

#endif
