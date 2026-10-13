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
 * Also in the extension's environment: `ctx.cwd`, `ctx.hasUI` (true when the mode has a user: ext.h pi_ext_ui) and `ctx.ui.select(title, options, {timeout})` / `confirm(title, message, {timeout})` / `input(title, placeholder, {timeout})` / `notify(message, type)` (L5: pi's ExtensionUIContext; the call waits for the answer and returns it -- the chosen string or nil, true or false, the text or nil --, and without a user gives pi's defaults nil, false, nil), `ctx.isProjectTrusted()`, `json.encode`
 * / `json.decode` (JS's JSON; objects encode with the keys in a fixed order -- type, text, data, mimeType, then sorted --, decoded arrays carry the metatable that makes the empty
 * table an array), `print` (a notice). The libraries: base (no dofile / loadfile; load accepts text only), coroutine,
 * table, string, math, utf8. No io, os, package, debug: files and processes come with pi.exec (L4).
 *
 * Also (L4, L6): `pi.registerTool(def)` -- def is pi's ToolDefinition (core/extensions/types.ts:573) as a table: name,
 * description, parameters (a JSON schema table, type "object"), promptSnippet, promptGuidelines (an array), constrainedSampling
 * and execute(toolCallId, params, signal, onUpdate, ctx), which returns { content, details, structuredContent, isError } or
 * raises an error (the model gets its message as the tool's error); `signal` is nil (nothing aborts a Lua call but Ctrl+C
 * before it starts), onUpdate({ content, details }) is a tool_execution_update. The tool joins the tools the model sees after
 * the built-in ones, as --tools / --no-tools allow it (pi_tools_ext_allowed). `pi.registerCommand(name, { description,
 * handler })` registers /name: handler(args, ctx) runs when the line "/name args" is entered, in the slash menu and in RPC
 * get_commands (ext.h). `pi.appendEntry(customType, data)` appends a custom entry to the session; it takes part in no request.
 * Not read: label, renderShell, prepareArguments, outputSchema, exposure, namespace, annotations, defaultActive,
 * prepareLoadout, executionMode, renderCall / renderResult, getArgumentCompletions (COMPATIBILITY.md).
 *
 * Each file is its own Lua state and its own entry in the session's single ordered extension list (ext.h): project
 * extensions (<cwd>/.pi/extensions, only when the project is trusted), then the user's (<agent dir>/extensions, the data
 * dir's too), in file name order -- pi's order (loader.ts discoverAndLoadExtensions: project, then global). A load error
 * or a handler error is reported as pi reports an extension error (the load: main.ts:802, the handler: interactive-mode.ts
 * showExtensionError); the extension is called again by the next event, as pi 1.1.0 does (runner.ts emitError only
 * reports), and the session goes on.
 *
 * Limits per extension (a runaway extension must not hang a 68020 or eat the machine's memory): 4 MB of Lua memory,
 * 5,000,000 VM instructions and 10 seconds per call (the load counts as a call), checked every 1000 instructions; past
 * the limit the call fails with an error that the extension's own pcall cannot swallow (the hook fires on every
 * instruction after it; each call is limited anew). Lua numbers are 64-bit integers and doubles (L13): a JSON integer to
 * 2^53 is a Lua integer, the rest a float, and a number is written as JSON.stringify writes a JS number (mcp.c pi_js_number).
 * Portable C89, host-tested (tests/test_pi_lua.c). */
#ifndef PI_EXT_LUA_H
#define PI_EXT_LUA_H

#include "ext.h"
#include "agent.h"
#include "claude/sys.h"

struct lua_State;

#define PI_LUA_MAX 8                    /* extensions loaded; a ninth is reported and left out */
#define PI_LUA_TOOLS 16                 /* tools one extension registers at most */
#define PI_LUA_CMDS 16                  /* commands one extension registers at most */
#define PI_LUA_MEM (4L * 1024 * 1024)   /* bytes of Lua memory per extension */
#define PI_LUA_INSTR 5000000L           /* VM instructions per call */
#define PI_LUA_MS 10000L                /* milliseconds per call (when the session has a clock) */

struct pi_lua;
struct pi_lua_ext;

/* a tool the extension registered: the session's pi_tool (first member) and the strings it points to */
typedef struct pi_lua_tool {
    pi_tool t;
    struct pi_lua_ext *x;
    int idx;                            /* its execute function is registry pi.tools[idx + 1] */
    char *name, *desc, *params, *snippet, *constrained;
    char **guide;                       /* the guidelines, a 0-ended array of strings, 0 none */
    int in_session;                     /* it was added to the session's tools */
} pi_lua_tool;

typedef struct pi_lua_ext {
    pi_ext ext;                         /* the entry in the session's list (first member) */
    struct pi_lua *host;
    struct lua_State *L;                /* the extension's state (0 while it is being loaded or when the load failed) */
    char path[300];
    int over;                           /* the call went past a limit: the hook raises on every instruction */
    long mem;                           /* bytes of Lua memory now */
    long ticks;                         /* count-hook ticks (1000 instructions) in this call */
    unsigned long t0;                   /* the clock when the call began */
    const char *scope;                  /* pi's SourceInfo scope of the file: "user", "project", "temporary" */
    char base[300];                     /* its baseDir */
    int loading;                        /* the factory is running: pi.appendEntry is not available yet, registrations wait */
    int loaded;                         /* registered in the session's list: what it registers now joins at once */
    pi_lua_tool tool[PI_LUA_TOOLS];
    int ntool;
    char *cmd[PI_LUA_CMDS], *cmd_desc[PI_LUA_CMDS];     /* registered commands: name and description (0 none); handler pi.commands[i + 1] */
    int ncmd;
} pi_lua_ext;

typedef struct pi_lua {
    pi_session *s;
    cl_sys *sys;
    const char *cwd;
    int trusted;
    pi_lua_ext x[PI_LUA_MAX];
    int n;
    long errors;                        /* extension errors reported (load and handler) */
    /* whether a tool of that name may join the session (--tools, --no-tools: pi_tools_ext_allowed); 0 allows every tool */
    int (*tool_allowed)(void *u, const char *name);
    void *tool_u;
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
extern long pi_lua_tool_calls;          /* execute functions of registered tools run */

#endif
