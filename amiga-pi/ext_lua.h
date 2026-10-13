/* ext_lua -- Lua 5.4 extensions (plan L3, decision D4; vendor/lua-5.4).
 *
 * pi's extensions are TypeScript modules that get an ExtensionAPI (core/extensions/types.ts:1564, `pi`) and register
 * handlers with pi.on(event, handler); amiga-pi's are Lua files that get the same `pi` and do the same:
 *
 *   pi.on("tool_call", function(event, ctx)
 *       if event.toolName == "bash" then return { block = true, reason = "no shell today" } end
 *   end)
 *
 * The event is pi's event object as a table (the payload ext_payload.c builds, the same JSON the hook commands get),
 * the handler's result is pi's result shape as a table, and the rules that apply it are ext_payload.c's, the same as
 * for hook commands. This batch implements pi.on for the events amiga-pi dispatches (ext.h): session_start, input,
 * agent_start, turn_start, message_start, message_end, tool_call, tool_execution_start / _update / _end, tool_result,
 * agent_end, agent_settled; any other name is accepted and never fires (COMPATIBILITY.md, extension events). tool_call:
 * mutate event.input in place or return { block = true, reason }; tool_result: return { content, details,
 * structuredContent, isError }; input: return { action = "transform", text } or { action = "handled" }.
 * Also in the extension's environment: `ctx.cwd`, `ctx.hasUI` (false until L5), `ctx.isProjectTrusted()`, `json.encode`
 * / `json.decode` (JS's JSON; objects encode with the keys in a fixed order -- type, text, data, mimeType, then sorted --, decoded arrays carry the metatable that makes the empty
 * table an array), `print` (a notice). The libraries: base (no dofile / loadfile; load accepts text only), coroutine,
 * table, string, math, utf8. No io, os, package, debug: files and processes come with pi.exec (L4).
 *
 * Each file is its own Lua state and its own entry in the session's single ordered extension list (ext.h): project
 * extensions (<cwd>/.pi/extensions, only when the project is trusted), then the user's (<agent dir>/extensions, the data
 * dir's too), in file name order -- pi's order (loader.ts discoverAndLoadExtensions: project, then global). A load error
 * or a handler error is reported as pi reports an extension error (the load: main.ts:802, the handler: interactive-mode.ts
 * showExtensionError) and disables that extension for the rest of the session; the session goes on. (pi keeps calling
 * a handler that threw; amiga-pi stops, so a runaway or broken extension costs one error, not one per event.)
 *
 * Limits per extension (a runaway extension must not hang a 68020 or eat the machine's memory): 4 MB of Lua memory,
 * 5,000,000 VM instructions and 10 seconds per call (the load counts as a call), checked every 1000 instructions; past
 * the limit the call fails with an error that the extension's own pcall cannot swallow (the hook fires on every
 * instruction after it). Lua numbers are 32-bit (LUA_32BITS, A5): a JSON integer outside +-2147483647 becomes a float
 * with 24 bits of precision (turn_start's 13-digit timestamp).
 * Portable C89, host-tested (tests/test_pi_lua.c). */
#ifndef PI_EXT_LUA_H
#define PI_EXT_LUA_H

#include "ext.h"
#include "agent.h"
#include "claude/sys.h"

struct lua_State;

#define PI_LUA_MAX 8                    /* extensions loaded; a ninth is reported and left out */
#define PI_LUA_MEM (4L * 1024 * 1024)   /* bytes of Lua memory per extension */
#define PI_LUA_INSTR 5000000L           /* VM instructions per call */
#define PI_LUA_MS 10000L                /* milliseconds per call (when the session has a clock) */

struct pi_lua;

typedef struct pi_lua_ext {
    pi_ext ext;                         /* the entry in the session's list (first member) */
    struct pi_lua *host;
    struct lua_State *L;                /* 0 once the extension is disabled */
    char path[300];
    int disabled;
    int over;                           /* the call went past a limit: the hook raises on every instruction */
    long mem;                           /* bytes of Lua memory now */
    long ticks;                         /* count-hook ticks (1000 instructions) in this call */
    unsigned long t0;                   /* the clock when the call began */
} pi_lua_ext;

typedef struct pi_lua {
    pi_session *s;
    cl_sys *sys;
    const char *cwd;
    int trusted;
    pi_lua_ext x[PI_LUA_MAX];
    int n;
    long errors;                        /* extension errors reported (load and handler) */
    /* the limits (the defaults above; the tests lower them) */
    long mem_cap, instr_cap;
    unsigned long ms_cap;
} pi_lua;

/* the host on the session; nothing is loaded or listed yet */
void pi_lua_init(pi_lua *h, pi_session *s, cl_sys *sys, const char *cwd, int trusted);
/* discovery: <cwd>/.pi/extensions (when trusted), then agent_dir/extensions, then data_dir/extensions (when another
 * directory): each *.lua file and each directory holding index.lua, by name; every one loaded in that order and added to
 * the session's list. Returns the number loaded (a failure is reported and left out). */
int pi_lua_discover(pi_lua *h, const char *agent_dir, const char *data_dir);
/* one extension file: 0 loaded and listed, -1 failed (reported) */
int pi_lua_load(pi_lua *h, const char *path);
/* the extensions that are still enabled */
int pi_lua_active(const pi_lua *h);
void pi_lua_free(pi_lua *h);

extern long pi_lua_calls;               /* handler calls made (the reachability tests' sentinel) */

#endif
