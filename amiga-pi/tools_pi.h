/* tools_pi -- pi's built-in tools (plan phase D) on C:Claude's tool code:
 * pi's names, descriptions and argument schemas (pi f10993bc
 * packages/coding-agent/src/core/tools/), each call mapped onto the
 * C:Claude tool that does the work (claude/tools.c through tools_run, on
 * one cl_tools with pi_rules set) and its tool_result turned back into pi
 * content blocks. No second implementation of a tool:
 *   read  {path, offset?, limit?}        -> Read
 *   write {path, content}                -> Write (the directories made)
 *   edit  {path, edits[{oldText,newText}]} -> tools_edit_orig (every oldText
 *                                          against the original, all or none)
 *   bash  {command, timeout?}            -> Bash: sys->run, i.e. vsh when it is
 *                                          installed, else the AmigaShell
 *   grep  {pattern, path?, glob?, ignoreCase?, literal?, context?, limit?} -> Grep
 *   find  {pattern, path?, limit?}       -> Glob
 *   ls    {path?, limit?}                -> sys->list (C:Claude has no ls)
 * pi's default set is read, bash, edit, write; grep, find and ls are off
 * until --tools names them. Every text result goes through pi_cut (2000
 * lines / 50 KB, the whole text kept in the temporary directory).
 * Permissions are not asked here (pi never asks); the gate is an extension
 * (ext_gate.h).
 * Portable C89 over cl_sys, host-tested (tests/test_pi_tools.c). */
#ifndef PI_TOOLS_PI_H
#define PI_TOOLS_PI_H

#include "claude/tools.h"
#include "agent.h"

enum { PT_READ, PT_WRITE, PT_EDIT, PT_BASH, PT_GREP, PT_FIND, PT_LS, PT_COUNT };

/* the shell the bash tool runs: vsh, the AmigaShell, the host's /bin/sh */
enum { PI_SH_VSH, PI_SH_AMIGASHELL, PI_SH_HOST };

#define PI_CAPTURE (512L * 1024)    /* a command's output kept for pi_cut (its tail beyond) */
#define PI_BASH_UPDATE_MS 100UL     /* pi renderers/bash.ts:19 BASH_UPDATE_THROTTLE_MS: the least between two tool_execution_update */

/* does an update go out now (bash.ts scheduleOutputUpdate): none yet, or PI_BASH_UPDATE_MS since the last one (ms clock) */
int pi_update_due(int has_last, unsigned long last, unsigned long now);

typedef struct pi_tools pi_tools;

typedef struct pi_tslot {
    pi_tools *p;
    int kind;                   /* PT_* */
} pi_tslot;

struct pi_tools {
    cl_tools t;                 /* C:Claude's tools, pi_rules set */
    char tmp[256];              /* where cut output is kept: "T:" on the Amiga */
    int shell;                  /* PI_SH_* */
    char bash_desc[600];        /* the bash tool's description, for that shell */
    char edit_schema[1100], grep_schema[1100];  /* the two schemas over C90's string length, joined */
    char defaults[256];         /* settings.json's defaultTools, joined by commas */
    int defaults_set;           /* the setting is there (an empty list is no tools) */
    int no_tools;               /* PI_NO_TOOLS_*: the default selection is empty (all: extension and MCP tools too) */
    char exclude[512];          /* --exclude-tools as given: names and '*' patterns, comma-separated */
    pi_tool tool[PT_COUNT];
    pi_tslot slot[PT_COUNT];
};

enum { PI_NO_TOOLS_NONE, PI_NO_TOOLS_ALL, PI_NO_TOOLS_BUILTIN };   /* main.ts:540-544 noTools: undefined, "all", "builtin" */

/* the tests' sentinels: calls of each tool that passed the argument check
 * and reached C:Claude's code (or ls's lister) */
extern long pi_tool_runs[PT_COUNT];

/* the tool set over sys, working directory root (canonical when it can
 * be), cut output kept in tmp ("T:", "/tmp/"): 0, -1 out of memory */
int pi_tools_init(pi_tools *p, cl_sys *sys, const char *root, const char *tmp, int shell);
void pi_tools_free(pi_tools *p);
/* The default selection, before pi_tools_add: settings.json's defaultTools as a comma-separated list (0: the setting is
 * absent: read, bash, edit, write; plain names replace the defaults, +name/-name entries change them; an empty list is
 * no tools) and --no-tools (the defaults are empty). pi 1.1.0 core/settings-manager.ts resolveDefaultTools. */
void pi_tools_defaults(pi_tools *p, const char *entries, int no_tools);
/* --exclude-tools (cli/args.ts:162): names and patterns removed from every tool source, after all other selection */
void pi_tools_exclude(pi_tools *p, const char *list);
/* pi's getToolListError for a --tools list: 0 when it is valid, else the problem (a literal, or buf for the one that
 * names an entry). A list is an allowlist of plain names ("*" all) or only +name/-name entries with exact names. */
const char *pi_tool_list_error(const char *list, char *buf, long cap);
/* The tools a --tools list selects added to the session: no list the default selection; plain names an allowlist
 * ("read,grep"); only +name/-name entries applyToolModifiers on the default selection ("-write" is read, bash, edit).
 * 0, -1 with the problem in err. */
int pi_tools_add(pi_tools *p, pi_session *s, const char *list, char *err, long cap);
/* toolPatternRegExp: 1 when name matches pattern, '*' any run of characters (--tools, --exclude-tools, MCP toolExposure) */
int pi_tool_glob(const char *pattern, const char *name);
/* whether an extension's tool of this name is registered under --tools list (0 none) and --no-tools, as the session
 * applies them to custom tools (sdk.ts:282-294, agent-session.ts _isAllowedTool): no list and no --no-tools allows it; a
 * list of plain names and patterns only the ones it names; only +name / -name entries allow every name, or with --no-tools
 * the ones they add; --no-tools alone none */
int pi_tools_ext_allowed(const pi_tools *p, const char *list, const char *name);
/* whether an MCP tool of this name (exposure direct or not) may be declared or loaded under --tools list, --no-tools and
 * --exclude-tools (agent-session.ts _isAllowedTool, _isActivatable): excluded never; no allowlist yes; named by the
 * allowlist yes; an allowlist that names mcp__ tools or is empty filters MCP tools; else a tool that is not direct, when
 * tool_search is allowed */
int pi_tools_mcp_ok(const pi_tools *p, const char *list, const char *name, int direct);
/* PT_*, -1 not one of these */
int pi_tools_kind(const char *name);
/* C:Claude's tool (T_*) and its input for a pi call (the gate decides on
 * these, with C:Claude's rules): 0, -1 no mapping (ls maps to Glob's
 * {path}, edit to Edit's {file_path}) */
int pi_tools_claude(int kind, const char *args, long an, int *tool, jw *in);
/* the shell sentence of the system prompt */
const char *pi_tools_shell_note(const pi_tools *p);
/* JavaScript's a.localeCompare(b) (ICU, English) for two UTF-8 or Latin-1 names: <0, 0, >0 (the order ls, find and the
 * skills listing sort in) */
int pi_locale_compare(const char *a, const char *b);

#endif
