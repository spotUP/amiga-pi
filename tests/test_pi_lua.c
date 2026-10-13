/* amiga-pi's Lua extensions (plan L3; ext_lua.h): the runtime's own rules, with small Lua files as the extensions. What
 * an extension is handed and what its answer does is pi's (the payloads and decisions are ext_payload.c's, compared with
 * pi 1.1.0 in test_pi_modes.c, scenarios ext_lua_*); this suite holds what pi's goldens cannot show -- a file that does
 * not load, a handler that throws, discovery and trust, the limits, the environment, the order of the list.
 *   block      tool_call { block, reason } and a throwing handler's text
 *   mutate     event.input changed in place reaches the tool and the next extension; untouched input stays byte for byte
 *   result     tool_result's patch; input's transform and handled
 *   errors     a load error and a handler error: pi's texts, the extension keeps running (L14), the others too
 *   discover   <cwd>/.pi/extensions only when trusted, then the user's, by name; index.lua in a directory
 *   limits     instructions, time, memory; each call is limited anew, the extension keeps running
 *   env        no io / os / require / bytecode; json; ctx; print; unsubscribe
 *   ui         ctx.hasUI and ctx.ui: pi's defaults without a user, the mode's dialogs with one, a long wait is not the
 *              handler's time (L5)
 *   order      one list with C extensions: the order they were added in */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/sys_posix.h"
#include "pi_stub.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/app.h"
#include "../amiga-pi/ext_lua.h"
#include "../amiga-pi/ui_pi.h"
#include "../amiga-pi/ext_events.h"

static char root[400];
static sys_posix sp;
static cl_sys sys;
static llm_model model;
static llm_hooks hooks;
static pi_session ses;
static pi_lua lx;
static char told[8][700];
static long told_n;
static unsigned long clk, clk_step;

static void on_notice(void *u, const char *text)
{
    (void)u;
    if (told_n < 8)
        cl_copy(told[told_n], text, sizeof(told[0]));
    told_n++;
}

static unsigned long on_ms(void *u)
{
    (void)u;
    clk += clk_step;
    return clk;
}

static void mkdirs(const char *path)
{
    char b[600];
    char *p;
    cl_copy(b, path, sizeof(b));
    for (p = b + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(b, 0777);
            *p = '/';
        }
    mkdir(b, 0777);
}

/* root/name = text (directories made) */
static void put(const char *name, const char *text)
{
    char path[600], dir[600], *sl;
    FILE *f;
    strcpy(path, root);
    strcat(path, "/");
    strcat(path, name);
    strcpy(dir, path);
    sl = strrchr(dir, '/');
    *sl = 0;
    mkdirs(dir);
    f = fopen(path, "wb");
    CHECK(f != 0);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void rm_file(const char *name);

static void at(char *out, const char *name)
{
    strcpy(out, root);
    strcat(out, "/");
    strcat(out, name);
}

static void rm_file(const char *name)
{
    char path[600];
    strcpy(path, root);
    strcat(path, "/");
    strcat(path, name);
    unlink(path);
}

static void up(int trusted)
{
    memset(&model, 0, sizeof(model));
    memset(&hooks, 0, sizeof(hooks));
    hooks.ms = on_ms;
    clk = 0;
    clk_step = 0;
    pi_session_init(&ses, &model, 0, 0, &hooks);
    ses.h.notice = on_notice;
    told_n = 0;
    pi_lua_init(&lx, &ses, &sys, root, trusted);
}

static void down(void)
{
    pi_lua_free(&lx);
    pi_session_free(&ses);
}

/* a file x.lua with the source, loaded */
static int load(const char *file, const char *src)
{
    char path[600];
    put(file, src);
    at(path, file);
    return pi_lua_load(&lx, path);
}

/* the input event through the list: the text a handler produces for "x" ("<handled>" when one consumed it) */
static const char *run_input(const char *text)
{
    static jw w;
    jw_free(&w);
    jw_init(&w);
    jw_rawz(&w, text);
    if (pi_exts_input(&ses.ext, &w))
        return "<handled>";
    return w.p;
}

/* the value of a Lua expression, as an input handler's text (event, ctx in scope) */
static const char *eval(const char *expr)
{
    char src[1200];
    strcpy(src, "pi.on('input', function(event, ctx) return { action = 'transform', text = tostring(");
    strcat(src, expr);
    strcat(src, ") } end)\n");
    CHECK_INT(load("eval.lua", src), 0);
    {
        const char *r = run_input("x");
        pi_lua_free(&lx);
        pi_lua_init(&lx, &ses, &sys, root, 0);
        ses.ext.n = 0;
        return r;
    }
}

static void test_block(void)
{
    jw args;
    char reason[300];
    jw_init(&args);
    up(0);
    CHECK_INT(load("block.lua",
                   "pi.on('tool_call', function(event, ctx)\n"
                   "  if event.toolName == 'bash' then return { block = true, reason = 'no shell today' } end\n"
                   "end)\n"), 0);
    CHECK_INT(ses.ext.n, 1);
    jw_rawz(&args, "{\"command\":\"ls\"}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "call_1", "bash", &args, reason, sizeof(reason)), 1);
    CHECK_STR(reason, "no shell today");
    CHECK_STR(args.p, "{\"command\":\"ls\"}");
    CHECK_INT(ses.ext.blocks, 1);
    jw_reset(&args);
    jw_rawz(&args, "{\"path\":\"a\"}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "call_2", "read", &args, reason, sizeof(reason)), 0);
    down();
    /* {block = true} alone: blocked, the reason is empty (the loop says pi's "Tool execution was blocked") */
    up(0);
    CHECK_INT(load("block2.lua", "pi.on('tool_call', function() return { block = true } end)"), 0);
    jw_reset(&args);
    jw_rawz(&args, "{}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 1);
    CHECK_STR(reason, "");
    down();
    /* a handler that returns nothing, or something that is no decision, blocks nothing */
    up(0);
    CHECK_INT(load("quiet.lua", "pi.on('tool_call', function() end)\npi.on('tool_call', function() return 5 end)\n"
                   "pi.on('tool_call', function() return { block = false } end)"), 0);
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 0);
    down();
    jw_free(&args);
}

static void test_mutate(void)
{
    jw args;
    char reason[300];
    jw_init(&args);
    up(0);
    CHECK_INT(load("m1.lua", "pi.on('tool_call', function(event) event.input.command = event.input.command .. ' -l' end)"), 0);
    CHECK_INT(load("m2.lua", "pi.on('tool_call', function(event)\n"
                   "  if event.input.command == 'ls -l' then event.input.timeout = 5 end\n"
                   "  if event.toolName ~= 'bash' then return { block = true, reason = 'x' } end\nend)"), 0);
    jw_rawz(&args, "{\"command\":\"ls\"}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 0);
    /* the second extension saw the first one's change, the tool gets both (keys come out sorted) */
    CHECK_STR(args.p, "{\"command\":\"ls -l\",\"timeout\":5}");
    down();
    /* arguments nobody changed are the model's own text, key order and spacing kept */
    up(0);
    CHECK_INT(load("m3.lua", "pi.on('tool_call', function(event) local c = event.input.command; local n = #c end)"), 0);
    jw_reset(&args);
    jw_rawz(&args, "{\"timeout\": 3,\"command\":\"ls\",  \"z\":[1,2]}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 0);
    CHECK_STR(args.p, "{\"timeout\": 3,\"command\":\"ls\",  \"z\":[1,2]}");
    down();
    /* L13: numbers are JS's. A 13-digit millisecond timestamp and 2^53-1 pass through a handler exactly (Lua integers of 64 bits);
     * the handler's own arithmetic past 2^53 is written as the double JS would hold; a float is JSON.stringify's text */
    up(0);
    CHECK_INT(load("m4.lua", "pi.on('tool_call', function(event)\n"
                   "  local i = event.input\n"
                   "  i.t2 = i.t + 1\n"
                   "  i.m2 = i.m\n"
                   "  i.p = i.m + 1\n"
                   "  i.q = i.m + 2\n"
                   "  i.f = i.x * 3\n"
                   "  i.k = math.type(i.t) .. math.type(i.x) .. math.type(i.big)\n"
                   "end)"), 0);
    jw_reset(&args);
    jw_rawz(&args, "{\"t\":1760000000123,\"m\":9007199254740991,\"x\":0.1,\"big\":18446744073709551616}");
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 0);
    CHECK_STR(args.p, "{\"big\":18446744073709552000,\"f\":0.30000000000000004,\"k\":\"integerfloatfloat\",\"m\":9007199254740991,"
                      "\"m2\":9007199254740991,\"p\":9007199254740992,\"q\":9007199254740992,\"t\":1760000000123,\"t2\":1760000000124,"
                      "\"x\":0.1}");
    down();
    jw_free(&args);
}

static void test_result_and_input(void)
{
    jw content;
    int is_error = 0;
    jw_init(&content);
    up(0);
    CHECK_INT(load("r.lua",
                   "pi.on('tool_result', function(event)\n"
                   "  if event.toolName == 'bash' and not event.isError then\n"
                   "    return { content = { { type = 'text', text = 'redacted' } }, isError = true }\n"
                   "  end\nend)\n"), 0);
    jw_rawz(&content, "[{\"type\":\"text\",\"text\":\"secret\"}]");
    pi_exts_tool_result(&ses.ext, "c", "bash", "{}", 2, &content, &is_error);
    CHECK_STR(content.p, "[{\"type\":\"text\",\"text\":\"redacted\"}]");
    CHECK_INT(is_error, 1);
    /* now an error result: the handler leaves it alone */
    jw_reset(&content);
    jw_rawz(&content, "[{\"type\":\"text\",\"text\":\"boom\"}]");
    pi_exts_tool_result(&ses.ext, "c", "bash", "{}", 2, &content, &is_error);
    CHECK_STR(content.p, "[{\"type\":\"text\",\"text\":\"boom\"}]");
    down();
    jw_free(&content);

    up(0);
    CHECK_INT(load("i.lua",
                   "pi.on('input', function(event)\n"
                   "  if event.text:sub(1, 1) == '!' then return { action = 'handled' } end\n"
                   "  return { action = 'transform', text = event.text .. ' (' .. event.source .. ')' }\n"
                   "end)\n"
                   "pi.on('input', function(event) return { action = 'transform', text = event.text .. '!' } end)\n"), 0);
    CHECK_STR(run_input("fix"), "fix (interactive)!");
    ses.ext.source = "rpc";
    CHECK_STR(run_input("fix"), "fix (rpc)!");
    CHECK_STR(run_input("!stop"), "<handled>");
    down();
    /* streamingBehavior is in the event only for a queued message */
    up(0);
    CHECK_INT(load("s.lua", "pi.on('input', function(event) return { action = 'transform', text = tostring(event.streamingBehavior) } end)"), 0);
    CHECK_STR(run_input("a"), "nil");
    ses.ext.streaming = "steer";
    CHECK_STR(run_input("a"), "steer");
    down();
}

static void test_errors(void)
{
    jw args;
    char reason[400], path[600];
    jw_init(&args);
    jw_rawz(&args, "{}");
    up(0);
    /* a syntax error and an error while the file runs: pi's text, nothing listed */
    CHECK_INT(load("bad.lua", "pi.on('tool_call', function() +++ end)"), -1);
    CHECK_INT(load("boom.lua", "pi.on('input', function() end)\nerror('no way')"), -1);
    CHECK_INT(ses.ext.n, 0);
    CHECK_INT(lx.n, 0);
    CHECK_INT(lx.errors, 2);
    at(path, "bad.lua");
    CHECK(strstr(told[0], "Failed to load extension \"") == told[0] && strstr(told[0], path) != 0);
    CHECK(strstr(told[0], "\": Failed to load extension: bad.lua:1:") != 0);
    CHECK(strstr(told[1], "Failed to load extension: boom.lua:2: no way") != 0);
    CHECK_STR(run_input("same"), "same");               /* the handler registered before the error never runs */
    {
        char missing[600];
        at(missing, "nothere.lua");
        CHECK_INT(pi_lua_load(&lx, missing), -1);
        CHECK(strstr(told[2], "the file cannot be read") != 0);
    }
    /* a handler that throws: the call is blocked with the message (runner.ts emitToolCall has no catch), the extension is
     * called again by the next event (pi has no disabling), a second one carries on */
    CHECK_INT(load("good.lua", "pi.on('tool_call', function(event) if event.toolName == 'edit' then return { block = true, reason = 'good' } end end)"), 0);
    down();
    up(0);
    CHECK_INT(load("throw.lua", "pi.on('tool_call', function() error('kaboom', 0) end)\npi.on('input', function() error({}) end)"), 0);
    CHECK_INT(load("good.lua", "pi.on('tool_call', function(event) if event.toolName == 'edit' then return { block = true, reason = 'good' } end end)\n"
                   "pi.on('input', function(event) return { action = 'transform', text = event.text .. '+' } end)"), 0);
    {
        long calls = pi_lua_calls;
        CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 1);
        CHECK_STR(reason, "kaboom");                    /* pi: the throwing handler blocks with its message */
        CHECK_INT(pi_lua_calls - calls, 1);
        CHECK_INT(told_n, 0);                           /* the error is the tool result, not a showExtensionError line */
        CHECK_INT(lx.x[0].L != 0, 1);
        CHECK_INT(pi_lua_active(&lx), 2);
        /* not disabled: called again, and blocks again; the other extension is never reached (the throw ends the dispatch) */
        calls = pi_lua_calls;
        CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "edit", &args, reason, sizeof(reason)), 1);
        CHECK_STR(reason, "kaboom");
        CHECK_INT(pi_lua_calls - calls, 1);
        /* its input handler throws too: told as showExtensionError, the next handler (the other extension's) still runs */
        CHECK_STR(run_input("a"), "a+");
        at(path, "throw.lua");
        CHECK(strstr(told[0], "Extension \"") == told[0] && strstr(told[0], path) != 0);
        CHECK_STR(strstr(told[0], "\" error: ") + 9, "the error value is a table");
        CHECK_STR(run_input("a"), "a+");                /* and the next input: still called, still told */
        CHECK_INT(told_n, 2);
        CHECK_INT(lx.errors, 2);
    }
    down();
    /* an error value that is no string, in an input handler: the text goes on unchanged, the session continues */
    up(0);
    CHECK_INT(load("tbl.lua", "pi.on('input', function() error({}) end)"), 0);
    CHECK_STR(run_input("keep"), "keep");
    CHECK(strstr(told[0], "error: the error value is a table") != 0 && strstr(told[0], "disabled") == 0);
    CHECK_STR(run_input("keep"), "keep");
    CHECK_INT(told_n, 2);                               /* called again, told again (runner.ts emitError only reports) */
    CHECK_INT(pi_lua_active(&lx), 1);
    down();
    /* a result that cannot become JSON (a function in it) is the handler's error */
    up(0);
    CHECK_INT(load("fn.lua", "pi.on('input', function() return { action = 'transform', text = print } end)"), 0);
    CHECK_STR(run_input("keep"), "keep");
    CHECK(strstr(told[0], "the handler's result cannot be used") != 0);
    down();
    jw_free(&args);
}

static void test_discover(void)
{
    char a[600], d[600];
    up(0);
    put("agent/extensions/b.lua", "pi.on('input', function() end)");
    put("agent/extensions/a.lua", "pi.on('input', function() end)");
    put("agent/extensions/notes.txt", "not an extension");
    put("agent/extensions/.hidden.lua", "error('never loaded')");
    put("agent/extensions/dir/index.lua", "pi.on('input', function() end)");
    put("agent/extensions/empty/readme.txt", "no index");
    put("agent/extensions/broken.lua", "this is not lua");
    put("w/.pi/extensions/proj.lua", "pi.on('input', function() end)");
    put("data/extensions/d.lua", "pi.on('input', function() end)");
    pi_lua_free(&lx);
    pi_lua_init(&lx, &ses, &sys, root, 0);
    strcpy(d, root);
    strcat(d, "/w");
    lx.cwd = d;
    at(a, "agent");
    {
        char data[600];
        at(data, "data");
        /* not trusted: the project's directory is never read */
        CHECK_INT(pi_lua_discover(&lx, a, data), 4);
        CHECK_INT(lx.n, 4);
        CHECK(strstr(lx.x[0].path, "/agent/extensions/a.lua") != 0);
        CHECK(strstr(lx.x[1].path, "/agent/extensions/b.lua") != 0);
        CHECK(strstr(lx.x[2].path, "/agent/extensions/dir/index.lua") != 0);
        CHECK(strstr(lx.x[3].path, "/data/extensions/d.lua") != 0);
        CHECK_INT(lx.errors, 1);                        /* broken.lua: reported, left out */
        CHECK(strstr(told[0], "broken.lua") != 0);
        CHECK_INT(ses.ext.n, 4);
        down();
        /* trusted: the project's extensions come first (loader.ts: project, then global) */
        up(1);
        strcpy(d, root);
        strcat(d, "/w");
        lx.cwd = d;
        CHECK_INT(pi_lua_discover(&lx, a, data), 5);
        CHECK(strstr(lx.x[0].path, "/w/.pi/extensions/proj.lua") != 0);
        CHECK(strstr(lx.x[1].path, "/agent/extensions/a.lua") != 0);
        /* the data directory equal to the agent directory is read once */
        down();
        up(1);
        strcpy(d, root);
        strcat(d, "/w");
        lx.cwd = d;
        CHECK_INT(pi_lua_discover(&lx, a, a), 4);
    }
    down();
    /* more than PI_LUA_MAX: the rest is reported and left out */
    up(0);
    {
        int i;
        char name[80], agent[600];
        for (i = 0; i < PI_LUA_MAX + 1; i++) {
            sprintf(name, "many/extensions/e%d.lua", i);
            put(name, "-- empty");
        }
        at(agent, "many");
        CHECK_INT(pi_lua_discover(&lx, agent, 0), PI_LUA_MAX);
        CHECK(strstr(told[0], "too many extensions") != 0);
    }
    down();
}

static void test_limits(void)
{
    up(0);
    lx.instr_cap = 200000;
    CHECK_INT(load("loop.lua", "while true do end"), -1);
    CHECK(strstr(told[0], "Failed to load extension: instruction limit exceeded") != 0);
    CHECK_INT(load("busy.lua",
                   "pi.on('input', function() while true do end end)\n"
                   "pi.on('tool_call', function() local n = 0; for i = 1, 1000 do n = n + i end end)"), 0);
    CHECK_STR(run_input("go"), "go");                   /* the runaway handler ended, the session goes on */
    CHECK(strstr(told[1], "busy.lua\" error: instruction limit exceeded") != 0 && strstr(told[1], "disabled") == 0);
    CHECK_INT(pi_lua_active(&lx), 1);
    CHECK_STR(run_input("go"), "go");                   /* the limit is per call: the extension runs again, and ends again */
    CHECK_INT(told_n, 3);
    down();
    /* an extension that catches the limit's error and loops on is ended by the same limit (the hook raises on every instruction) */
    up(0);
    lx.instr_cap = 200000;
    CHECK_INT(load("catch.lua", "pi.on('input', function()\n  while true do pcall(function() while true do end end) end\nend)"), 0);
    CHECK_STR(run_input("go"), "go");
    CHECK(strstr(told[0], "limit exceeded") != 0);
    CHECK_INT(pi_lua_active(&lx), 1);
    down();
    /* the limit is per call: a handler that stays under it runs as often as it is asked */
    up(0);
    lx.instr_cap = 200000;
    CHECK_INT(load("under.lua", "pi.on('input', function(e) local n = 0; for i = 1, 20000 do n = n + 1 end; return { action = 'transform', text = e.text .. '.' } end)"), 0);
    CHECK_STR(run_input("a"), "a.");
    CHECK_STR(run_input("a"), "a.");
    CHECK_STR(run_input("a"), "a.");
    CHECK_INT(pi_lua_active(&lx), 1);
    down();
    /* the clock: 1 s per look, 2.5 s allowed */
    up(0);
    lx.ms_cap = 2500;
    CHECK_INT(load("slow.lua", "pi.on('input', function() while true do end end)"), 0);
    clk_step = 1000;
    CHECK_STR(run_input("go"), "go");
    CHECK(strstr(told[0], "time limit exceeded") != 0);
    down();
    /* memory */
    up(0);
    lx.mem_cap = 1024L * 1024;
    CHECK_INT(load("mem.lua", "pi.on('input', function() local t = {}; while true do t[#t + 1] = string.rep('x', 100000) end end)"), 0);
    CHECK_STR(run_input("go"), "go");
    CHECK(strstr(told[0], "not enough memory") != 0);
    CHECK_INT(pi_lua_active(&lx), 1);
    down();
    up(0);
    lx.mem_cap = 1024L * 1024;
    CHECK_INT(load("mem2.lua", "local t = {}; for i = 1, 100 do t[i] = string.rep('x', 100000) end"), -1);
    CHECK(strstr(told[0], "not enough memory") != 0);
    down();
}

static void test_env(void)
{
    up(0);
    CHECK_STR(eval("io"), "nil");
    CHECK_STR(eval("os"), "nil");
    CHECK_STR(eval("require"), "nil");
    CHECK_STR(eval("package"), "nil");
    CHECK_STR(eval("debug"), "nil");
    CHECK_STR(eval("dofile"), "nil");
    CHECK_STR(eval("loadfile"), "nil");
    CHECK_STR(eval("type(string.format) .. type(table.concat) .. type(math.floor) .. type(utf8.char) .. type(coroutine.wrap)"),
              "functionfunctionfunctionfunctionfunction");
    CHECK_STR(eval("load('return 1 + 1')()"), "2");
    CHECK_STR(eval("select('#', load(string.dump(function() end)))"), "2");     /* nil and the message: no bytecode */
    {
        char expr[500];
        strcpy(expr, "ctx.cwd == '");
        strcat(expr, root);
        strcat(expr, "'");
        CHECK_STR(eval(expr), "true");
    }
    CHECK_STR(eval("ctx.hasUI"), "false");
    CHECK_STR(eval("ctx.isProjectTrusted()"), "false");
    CHECK_STR(eval("pi.on ~= nil and type(pi.on)"), "function");
    CHECK_STR(eval("string.format('%d %s %.2f', 7, 'a', 3.14159)"), "7 a 3.14");
    CHECK_STR(eval("tonumber('3.5') * 2"), "7.0");
    down();
    up(1);
    CHECK_STR(eval("ctx.isProjectTrusted()"), "true");
    down();
    /* json */
    up(0);
    CHECK_STR(eval("json.encode({ b = 1, a = { 1, 2, 3 }, c = {}, d = json.decode('[]'), e = 'x\\ny', f = true })"),
              "{\"a\":[1,2,3],\"b\":1,\"c\":{},\"d\":[],\"e\":\"x\\ny\",\"f\":true}");
    CHECK_STR(eval("json.encode(json.decode('{\"x\":[1,2.5,{\"y\":null,\"z\":\"\\\\u00e9\"}],\"s\":\"a\\\\nb\"}'))"),
              "{\"s\":\"a\\nb\",\"x\":[1,2.5,{\"z\":\"\xc3\xa9\"}]}");
    CHECK_STR(eval("json.encode({ json.null, 1, json.null })"), "[null,1,null]");
    CHECK_STR(eval("json.encode(json.decode('[[],{},[[]]]'))"), "[[],{},[[]]]");
    CHECK_STR(eval("json.encode(2147483647) .. json.encode(-2147483647) .. json.encode(0.5)"), "2147483647-21474836470.5");
    CHECK_STR(eval("math.type(json.decode('2147483647')) .. math.type(json.decode('2147483648')) .. math.type(json.decode('-5')) .. math.type(json.decode('1e2'))"),
              "integerintegerintegerfloat");
    CHECK_STR(eval("json.encode(json.decode('1760000000000')) .. json.encode(json.decode('-9007199254740991')) .. json.encode(json.decode('9007199254740993'))"),
              "1760000000000-90071992547409919007199254740992");
    CHECK_STR(eval("json.encode(1e21) .. ' ' .. json.encode(2^53) .. ' ' .. json.encode(-0.0) .. ' ' .. json.encode(1e-7) .. ' ' .. json.encode(1/3)"),
              "1e+21 9007199254740992 0 1e-7 0.3333333333333333");
    CHECK_STR(eval("json.encode(0/0) .. json.encode(1/0)"), "nullnull");
    CHECK_STR(eval("select(2, pcall(json.decode, '{oops'))"), "json.decode: invalid JSON");
    CHECK_STR(eval("select(2, pcall(json.encode, { f = print }))"), "json.encode: cannot encode a function, thread or userdata");
    CHECK_STR(eval("select(2, pcall(json.encode, { [1] = 'a', x = 'b' }))"), "json.encode: object keys must be strings");
    CHECK_STR(eval("(function() local t = {}; t.t = t; return select(2, pcall(json.encode, t)) end)()"),
              "json.encode: table nested too deeply (a cycle?)");
    down();
}

/* ---- L5: ctx.hasUI and ctx.ui ---- */

static struct {
    char title[80], message[80], placeholder[80], type[16], opt[4][16];
    int n, calls;
    long timeout;
    int yes, pick;              /* confirm's answer, select's (-1: cancelled) */
    const char *text;           /* input's answer, 0 cancelled */
    unsigned long wait_ms;      /* how long the dialog "waits" (the session clock goes on) */
} fui;

static int fui_select(void *u, const char *title, const char *const *opt, int n, long timeout_ms, char *value, long cap)
{
    int i;
    (void)u;
    fui.calls++;
    cl_copy(fui.title, title, sizeof(fui.title));
    fui.n = n;
    for (i = 0; i < n && i < 4; i++)
        cl_copy(fui.opt[i], opt[i], sizeof(fui.opt[i]));
    fui.timeout = timeout_ms;
    clk += fui.wait_ms;
    if (fui.pick < 0)
        return 0;
    cl_copy(value, opt[fui.pick], cap);
    return 1;
}

static int fui_confirm(void *u, const char *title, const char *message, long timeout_ms)
{
    (void)u;
    fui.calls++;
    cl_copy(fui.title, title, sizeof(fui.title));
    cl_copy(fui.message, message, sizeof(fui.message));
    fui.timeout = timeout_ms;
    clk += fui.wait_ms;
    return fui.yes;
}

static int fui_input(void *u, const char *title, const char *placeholder, long timeout_ms, char *value, long cap)
{
    (void)u;
    fui.calls++;
    cl_copy(fui.title, title, sizeof(fui.title));
    cl_copy(fui.placeholder, placeholder ? placeholder : "<none>", sizeof(fui.placeholder));
    fui.timeout = timeout_ms;
    clk += fui.wait_ms;
    if (!fui.text)
        return 0;
    cl_copy(value, fui.text, cap);
    return 1;
}

static void fui_notify(void *u, const char *message, const char *type)
{
    (void)u;
    fui.calls++;
    cl_copy(fui.message, message, sizeof(fui.message));
    cl_copy(fui.type, type ? type : "<none>", sizeof(fui.type));
}

static pi_ext_ui fake_ui = { 0, fui_select, fui_confirm, fui_input, fui_notify };

static void test_ui(void)
{
    /* no user (print, json): pi's noOpUIContext -- hasUI false, select and input undefined, confirm false, notify nothing */
    up(0);
    CHECK_STR(eval("ctx.hasUI"), "false");
    CHECK_STR(eval("tostring(ctx.ui.select('T', { 'a', 'b' })) .. tostring(ctx.ui.confirm('T', 'm')) .. tostring(ctx.ui.input('T', 'p'))"),
              "nilfalsenil");
    CHECK_STR(eval("tostring((ctx.ui.notify('hello', 'error')))"), "nil");
    down();
    /* a user: hasUI is read when asked, so a mode that binds after the extensions loaded still shows */
    up(0);
    memset(&fui, 0, sizeof(fui));
    CHECK_INT(load("ui.lua",
                   "pi.on('input', function(event, ctx)\n"
                   "  return { action = 'transform', text = tostring(ctx.hasUI) }\n"
                   "end)"), 0);
    CHECK_STR(run_input("x"), "false");
    ses.ext.ui = &fake_ui;
    CHECK_STR(run_input("x"), "true");
    down();
    /* the four calls reach the mode with pi's arguments and give its answers back */
    up(0);
    memset(&fui, 0, sizeof(fui));
    ses.ext.ui = &fake_ui;
    fui.pick = 1;
    CHECK_STR(eval("ctx.ui.select('Pick', { 'one', 'two', 'three' })"), "two");
    CHECK_STR(fui.title, "Pick");
    CHECK_INT(fui.n, 3);
    CHECK_STR(fui.opt[2], "three");
    CHECK_INT((int)fui.timeout, 0);
    CHECK_STR(eval("ctx.ui.select('Pick', { 'one', 'two' }, { timeout = 250 })"), "two");
    CHECK_INT((int)fui.timeout, 250);
    fui.pick = -1;
    CHECK_STR(eval("tostring(ctx.ui.select('Pick', { 'one' }))"), "nil");            /* cancelled and timed out: undefined */
    CHECK_STR(eval("tostring(ctx.ui.select('Pick', {}))"), "nil");                     /* nothing to choose from: no dialog */
    CHECK_INT(fui.calls, 3);
    fui.yes = 1;
    CHECK_STR(eval("tostring(ctx.ui.confirm('Sure?', 'It runs.', { timeout = 9 }))"), "true");
    CHECK_STR(fui.title, "Sure?");
    CHECK_STR(fui.message, "It runs.");
    CHECK_INT((int)fui.timeout, 9);
    fui.yes = 0;
    CHECK_STR(eval("tostring(ctx.ui.confirm('Sure?', 'It runs.'))"), "false");
    fui.text = "because";
    CHECK_STR(eval("ctx.ui.input('Why?', 'a reason')"), "because");
    CHECK_STR(fui.placeholder, "a reason");
    CHECK_STR(eval("ctx.ui.input('Why?')"), "because");
    CHECK_STR(fui.placeholder, "<none>");
    fui.text = 0;
    CHECK_STR(eval("tostring(ctx.ui.input('Why?', 'a reason'))"), "nil");
    CHECK_STR(eval("tostring((ctx.ui.notify('careful', 'warning')))"), "nil");
    CHECK_STR(fui.message, "careful");
    CHECK_STR(fui.type, "warning");
    CHECK_STR(eval("tostring((ctx.ui.notify('plain')))"), "nil");
    CHECK_STR(fui.type, "<none>");
    /* a wrong call is the handler's error, as any Lua call with a missing argument (the extension keeps running) */
    CHECK(strstr(eval("select(2, pcall(ctx.ui.select, 'Pick'))"), "table expected") != 0);
    down();
    /* a dialog waits for the user as long as it takes: a minute on the session clock is not the handler's, whose limit is 10 s */
    up(0);
    memset(&fui, 0, sizeof(fui));
    ses.ext.ui = &fake_ui;
    fui.pick = 0;
    fui.wait_ms = 60000;
    CHECK_INT(load("wait.lua",
                   "pi.on('input', function(event, ctx)\n"
                   "  local v = ctx.ui.select('Pick', { 'a', 'b' })\n"
                   "  local n = 0\n"
                   "  for i = 1, 20000 do n = n + 1 end\n"
                   "  return { action = 'transform', text = v .. n }\n"
                   "end)"), 0);
    CHECK_STR(run_input("x"), "a20000");
    CHECK_INT((int)told_n, 0);
    down();
}

static void test_events_and_api(void)
{
    pi_event e;
    up(0);
    CHECK_INT(load("ev.lua",
                   "pi.on('agent_start', function(e) print(e.type) end)\n"
                   "pi.on('turn_start', function(e) print(e.type, e.turnIndex) end)\n"
                   "pi.on('session_start', function(e) print(e.type, e.reason, e.previousSessionFile) end)\n"
                   "pi.on('never_dispatched', function() error('x') end)\n"), 0);
    memset(&e, 0, sizeof(e));
    e.type = PE_AGENT_START;
    pi_exts_event(&ses.ext, &e);
    CHECK_INT(told_n, 1);
    CHECK_STR(told[0], "agent_start");
    e.type = PE_TURN_START;
    pi_exts_event(&ses.ext, &e);
    CHECK_STR(told[1], "turn_start\t0");
    e.type = PE_TURN_END;
    pi_exts_event(&ses.ext, &e);
    e.type = PE_TURN_START;
    pi_exts_event(&ses.ext, &e);
    CHECK_STR(told[2], "turn_start\t1");
    pi_exts_session_start(&ses.ext, "resume", "/s/x.jsonl");
    CHECK_STR(told[3], "session_start\tresume\t/s/x.jsonl");
    pi_exts_session_start(&ses.ext, "startup", 0);
    CHECK_STR(told[4], "session_start\tstartup\tnil");
    down();
    /* pi.on returns the function that removes the handler */
    up(0);
    CHECK_INT(load("off.lua",
                   "local off = pi.on('input', function(e) return { action = 'transform', text = e.text .. '1' } end)\n"
                   "pi.on('input', function(e) off(); return { action = 'transform', text = e.text .. '2' } end)\n"
                   "pi.on('input', function(e) return { action = 'transform', text = e.text .. '3' } end)\n"), 0);
    CHECK_STR(run_input("a"), "a123");
    CHECK_STR(run_input("a"), "a23");
    down();
    /* pi.on checks its arguments, as a call with the wrong types is an error in the file */
    up(0);
    CHECK_INT(load("args.lua", "pi.on('input')"), -1);
    CHECK(strstr(told[0], "bad argument #2 to 'on'") != 0);
    down();
}

/* ---- L7: the events beyond the goldens' reach (tree navigation is the terminal's, a cycle needs two usable models) ---- */

static char drafts[4][200];
static int ndrafts, can_continue;

static int fake_boundary(void *u, pi_session *s, int settle, jw *ids, jw *ctx)
{
    (void)u;
    (void)s;
    (void)settle;
    if (ids)
        jw_rawz(ids, "\"messageEntryId\":\"m1\",\"toolResultEntryIds\":[]");
    jw_rawz(ctx, can_continue ? "{\"contextEntries\":[],\"canContinue\":true}" : "{\"contextEntries\":[],\"canContinue\":false}");
    return 0;
}

static int fake_draft(void *u, pi_session *s, const char *text, long n)
{
    (void)u;
    (void)s;
    if (ndrafts < 4) {
        memcpy(drafts[ndrafts], text, (size_t)(n < 199 ? n : 199));
        drafts[ndrafts][n < 199 ? n : 199] = 0;
    }
    ndrafts++;
    return 0;
}

static char got_kind[4][16], got_path[4][80], got_from[4][40];
static int got_n;

static void fake_discovered(void *u, const char *kind, const char *path, const pi_ext *from)
{
    (void)u;
    if (kind && got_n < 4) {
        cl_copy(got_kind[got_n], kind, sizeof(got_kind[0]));
        cl_copy(got_path[got_n], path, sizeof(got_path[0]));
        cl_copy(got_from[got_n], from && from->name ? from->name : "", sizeof(got_from[0]));
        got_n++;
    }
}

static void test_l7_events(void)
{
    pi_tree_dec dec;
    llm_model other;
    up(0);
    CHECK_INT(load("t.lua",
                   "for _, n in ipairs({'session_before_tree','session_tree','model_select','thinking_level_select',\n"
                   "  'ui_prompt_start'}) do\n"
                   "  pi.on(n, function(e)\n"
                   "    print(json.encode(e))\n"
                   "    if e.type == 'session_before_tree' then return { customInstructions = 'be short', label = 'L', summary = { summary = 'by hand' } } end\n"
                   "  end)\n"
                   "end\n"), 0);
    CHECK_INT(load("v.lua", "pi.on('turn_end', function(e) print(json.encode(e)) return { entries = { { type = 'custom', customType = 'k', "
                            "data = { n = 1 } } }, continue = true } end)\n"), 0);
    CHECK_INT(load("u.lua", "pi.on('resources_discover', function(e) return { skillPaths = { '/s/one' }, promptPaths = { '/p/two' }, "
                            "themePaths = { '/t' } } end)\n"), 0);
    /* session_before_tree: the preparation as types.ts has it, the answers kept */
    pi_tree_dec_init(&dec);
    CHECK_INT(pi_ev_before_tree(&ses, "t1", "o1", 0, "[{\"id\":\"x\"}]", 1, 0, 0, 0, 0, &dec), 0);
    CHECK(strstr(told[0], "\"preparation\"") != 0 && strstr(told[0], "\"targetId\":\"t1\"") != 0);
    CHECK(strstr(told[0], "\"userWantsSummary\":true") != 0);
    CHECK(dec.has_instr && !strcmp(dec.instr.p, "be short"));
    CHECK(dec.has_label && !strcmp(dec.label.p, "L"));
    CHECK(dec.has_summary && !strcmp(dec.summary.p, "by hand"));
    pi_tree_dec_free(&dec);
    /* session_tree: the leaf ids, the summary entry and fromExtension only with one */
    pi_ev_tree(&ses, "n1", "o1", "{\"id\":\"b\"}", 10, 1);
    CHECK(strstr(told[1], "\"newLeafId\":\"n1\"") != 0 && strstr(told[1], "\"fromExtension\":true") != 0);
    pi_ev_tree(&ses, 0, "o1", 0, 0, 0);
    CHECK(strstr(told[2], "\"fromExtension\"") == 0 && strstr(told[2], "\"summaryEntry\"") == 0);
    /* model_select: not for the same model; a cycle says so */
    memset(&other, 0, sizeof(other));
    model.provider = "p";
    model.id = "m1";
    model.api = "a";
    other = model;
    other.id = "m2";
    told_n = 3;
    pi_ev_model_select(&ses, &model, &model, "cycle");
    CHECK_INT(told_n, 3);
    pi_ev_model_select(&ses, &other, &model, "cycle");
    CHECK_INT(told_n, 4);
    CHECK(strstr(told[3], "\"source\":\"cycle\"") != 0);
    pi_ev_thinking(&ses, "high", "off");
    CHECK(strstr(told[4], "\"previousLevel\":\"off\"") != 0);
    /* turn_end: the drafts reach the session, the continuation is honoured only with a request to make */
    ses.boundary = fake_boundary;
    ses.draft = fake_draft;
    can_continue = 1;
    ndrafts = 0;
    CHECK_INT(pi_ev_turn_end(&ses, "{\"role\":\"assistant\"}", 20, "[]", 2), 1);
    CHECK_INT(ndrafts, 1);
    CHECK(strstr(drafts[0], "\"customType\":\"k\"") != 0);
    can_continue = 0;
    told_n = 6;
    CHECK_INT(pi_ev_turn_end(&ses, "{\"role\":\"assistant\"}", 20, "[]", 2), 0);
    CHECK(strstr(told[6], "turn_end") != 0);
    CHECK(strstr(told[7], "requested continuation without runnable model context") != 0);
    /* resources_discover: every path named, with the extension; the theme paths are left out */
    ses.ext.discovered = fake_discovered;
    got_n = 0;
    pi_exts_session_start(&ses.ext, "reload", 0);
    CHECK_INT(got_n, 2);
    CHECK(!strcmp(got_kind[0], "skill") && !strcmp(got_path[0], "/s/one"));
    CHECK(!strcmp(got_kind[1], "prompt") && !strcmp(got_path[1], "/p/two"));
    CHECK(got_from[0][0] != 0);
    /* ui_prompt: told for the outermost dialog only */
    told_n = 0;
    pi_ev_ui_prompt(&ses, 1, "select", "T");
    CHECK_INT(told_n, 1);
    CHECK(strstr(told[0], "\"kind\":\"select\"") != 0 && strstr(told[0], "\"title\":\"T\"") != 0);
    down();
}

/* ---- one list with the C extensions ---- */

static char seen[16];
static int c_call(void *u, const char *id, const char *name, jw *args, char *reason, long cap)
{
    (void)id;
    (void)name;
    (void)args;
    (void)reason;
    (void)cap;
    strcat(seen, (const char *)u);
    return 0;
}

static void test_order(void)
{
    static const pi_ext first = { "c-first", (void *)"1", 0, c_call, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const pi_ext last = { "c-last", (void *)"3", 0, c_call, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    jw args;
    char reason[100];
    jw_init(&args);
    jw_rawz(&args, "{}");
    up(0);
    pi_ext_add(&ses.ext, &first);
    CHECK_INT(load("o1.lua", "pi.on('tool_call', function() print('lua-a') end)"), 0);
    CHECK_INT(load("o2.lua", "pi.on('tool_call', function(e) if e.toolName == 'bash' then return { block = true, reason = 'b' } end end)"), 0);
    pi_ext_add(&ses.ext, &last);
    seen[0] = 0;
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "read", &args, reason, sizeof(reason)), 0);
    CHECK_STR(seen, "13");
    CHECK_STR(told[0], "lua-a");
    seen[0] = 0;
    told_n = 0;
    CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 1);
    CHECK_STR(seen, "1");                               /* the blocker stops the list: the C extension after it never ran */
    CHECK_INT(told_n, 1);
    CHECK_INT(ses.ext.n, 4);
    down();
    jw_free(&args);
}

/* ---- L4, L6: tools, commands and entries an extension registers ---- */

static pi_tool *tool_named(const char *name)
{
    int i;
    for (i = 0; i < ses.ntools; i++)
        if (!strcmp(ses.tools[i]->name, name))
            return ses.tools[i];
    return 0;
}

/* the tool's run with args, as the agent loop calls it (call id tc1): the content text, the error flag in *err */
static const char *run_tool(const char *name, const char *args, int *err)
{
    static jw c;
    pi_tool *t = tool_named(name);
    jw_free(&c);
    jw_init(&c);
    *err = -1;
    if (!t)
        return "<no such tool>";
    ses.call_id = "tc1";
    ses.result_error = 0;
    jw_reset(&ses.details);
    jw_reset(&ses.structured);
    t->run(t->u, &ses, args, (long)strlen(args), &c, err);
    ses.call_id = 0;
    return c.p ? c.p : "";
}

static int deny_x(void *u, const char *name)
{
    (void)u;
    return strcmp(name, "x") != 0;
}

static int builtin_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    (void)u;
    (void)s;
    (void)args;
    (void)an;
    (void)content;
    *is_error = 0;
    return 0;
}

static void test_register_tools(void)
{
    static pi_tool builtin = { "read", "the built-in", "{\"type\":\"object\"}", 0, 0, builtin_run, 0, 0, 0 };
    int err;
    long k0;
    const char *r;
    pi_tool *t;
    char src[1200];
    up(0);
    strcpy(src, "pi.registerTool({ name = 'echo', label = 'Echo', description = 'e', promptSnippet = '  Echo\\n   it  ',\n"
                "  promptGuidelines = { ' Say it. ', 'Say it.', '', 'Twice.' },\n"
                "  parameters = { type = 'object', properties = { s = { type = 'string' } }, required = { 's' } },\n"
                "  execute = function(id, p, sig, up, ctx)\n");
    strcat(src, "    if p.s == 'soft' then return { content = { { type = 'text', text = 'no' } }, isError = true } end\n"
                "    if p.s == 'raise' then error('bad', 0) end\n"
                "    if p.s == 'none' then return {} end\n");
    strcat(src, "    return { content = { { type = 'text', text = id .. ':' .. p.s .. ':' .. tostring(sig) .. ':' .. ctx.cwd } },\n"
                "             details = { n = 1 }, structuredContent = { ok = true } }\n"
                "  end })");
    CHECK_INT(load("t1.lua", src), 0);
    CHECK_INT(ses.ntools, 1);
    t = tool_named("echo");
    CHECK(t != 0);
    if (!t) {
        down();
        return;
    }
    CHECK_STR(t->description, "e");
    CHECK_STR(t->parameters, "{\"type\":\"object\",\"properties\":{\"s\":{\"type\":\"string\"}},\"required\":[\"s\"]}");
    CHECK_STR(t->snippet, "Echo it");                     /* _normalizePromptSnippet: one line, white space collapsed */
    CHECK(t->guide && !strcmp(t->guide[0], "Say it.") && !strcmp(t->guide[1], "Twice.") && !t->guide[2]);   /* trimmed, once */
    k0 = pi_lua_tool_calls;
    r = run_tool("echo", "{\"s\":\"hi\"}", &err);
    CHECK_INT(err, 0);
    CHECK(strstr(r, "tc1:hi:nil:") != 0);                   /* the call id, the arguments, no signal, ctx */
    CHECK_INT(pi_lua_tool_calls - k0, 1);
    CHECK_STR(ses.details.p, "{\"n\":1}");
    CHECK_STR(ses.structured.p, "{\"ok\":true}");
    CHECK_INT(ses.result_error, 0);
    /* a result with isError is no thrown error: the loop keeps its details as they are */
    r = run_tool("echo", "{\"s\":\"soft\"}", &err);
    CHECK_INT(err, 1);
    CHECK_STR(r, "[{\"type\":\"text\",\"text\":\"no\"}]");
    CHECK_INT(ses.result_error, 1);
    CHECK_INT(ses.details.n, 0);
    /* an error raised by execute is the tool's error result, the extension goes on */
    r = run_tool("echo", "{\"s\":\"raise\"}", &err);
    CHECK_INT(err, 1);
    CHECK_STR(r, "[{\"type\":\"text\",\"text\":\"bad\"}]");
    CHECK_INT(ses.result_error, 0);
    CHECK_INT(pi_lua_active(&lx), 1);
    r = run_tool("echo", "{\"s\":\"none\"}", &err);        /* no content: an empty array */
    CHECK_INT(err, 0);
    CHECK_STR(r, "[]");
    /* the arguments are checked against the schema before execute runs */
    k0 = pi_lua_tool_calls;
    r = run_tool("echo", "{}", &err);
    CHECK_INT(err, 1);
    CHECK(strstr(r, "Validation failed for tool \\\"echo\\\"") != 0);
    r = run_tool("echo", "[1]", &err);
    CHECK_INT(err, 1);
    CHECK(strstr(r, "the arguments are not a JSON object") != 0);
    CHECK_INT(pi_lua_tool_calls - k0, 0);
    /* a definition pi refuses, or no name: the load fails, nothing is registered */
    CHECK_INT(load("t2.lua", "pi.registerTool({ name = 'x', parameters = { 1 }, execute = function() end })"), -1);
    CHECK(strstr(told[0], "Tool \"x\" registered by extension \"") != 0 && strstr(told[0], "must define an object parameter schema.") != 0);
    CHECK_INT(load("t2b.lua", "pi.registerTool({ parameters = { type = 'object' } })"), -1);
    CHECK_INT(load("t2c.lua", "pi.registerTool({ name = 'q', parameters = { type = 'object' }, promptGuidelines = { 1 } })"), -1);
    /* what an extension registers and then fails to load with is not registered (loader.ts: a failed load is discarded) */
    CHECK_INT(load("t3.lua", "pi.registerTool({ name = 'gone', parameters = { type = 'object' } })\n"
                             "pi.registerCommand('gonecmd', { handler = function() end })\nerror('late', 0)"), -1);
    CHECK(tool_named("gone") == 0);
    CHECK_INT(ses.ext.ncmd, 0);
    CHECK_INT(ses.ntools, 1);
    /* --tools: the host says which names may join */
    lx.tool_allowed = deny_x;
    CHECK_INT(load("t4.lua", "pi.registerTool({ name = 'x', parameters = { type = 'object' }, execute = function() end })\n"
                             "pi.registerTool({ name = 'y', parameters = { type = 'object' }, execute = function() end })"), 0);
    CHECK(tool_named("x") == 0 && tool_named("y") != 0);
    /* a built-in tool of that name is replaced where it stands; the first extension to register a name keeps it */
    pi_tool_add(&ses, &builtin);
    CHECK_INT(load("t5.lua", "pi.registerTool({ name = 'read', description = 'mine', parameters = { type = 'object' }, execute = function() end })"), 0);
    CHECK_STR(tool_named("read")->description, "mine");
    CHECK(tool_named("read") != &builtin);
    CHECK_INT(load("t6.lua", "pi.registerTool({ name = 'read', description = 'second', parameters = { type = 'object' }, execute = function() end })"), 0);
    CHECK_STR(tool_named("read")->description, "mine");
    /* a tool registered by a handler, after the load, joins at once */
    CHECK_INT(load("t7.lua", "pi.on('input', function() pi.registerTool({ name = 'late', parameters = { type = 'object' }, execute = function() end }) end)"), 0);
    CHECK(tool_named("late") == 0);
    run_input("x");
    CHECK(tool_named("late") != 0);
    /* an extension whose handler threw: its tool still runs (pi has no disabling) */
    CHECK_INT(load("t8.lua", "pi.registerTool({ name = 'stops', parameters = { type = 'object' }, execute = function() return { content = {} } end })\n"
                             "pi.on('input', function() error('stop', 0) end)"), 0);
    run_input("x");
    r = run_tool("stops", "{}", &err);
    CHECK_INT(err, 0);
    CHECK(strstr(r, "disabled") == 0);
    down();
}

static void test_register_commands(void)
{
    pi_ext_rcmd rc[PI_EXT_CMDS];
    int n;
    up(0);
    CHECK_INT(load("c1.lua",
        "pi.registerCommand('hello', { description = 'Say hello', handler = function(args, ctx) print('hello:' .. args .. ':' .. ctx.cwd) end })\n"
        "pi.registerCommand('plain', { handler = function(args) print('plain:' .. args) end })"), 0);
    CHECK_INT(load("c2.lua", "pi.registerCommand('hello', { description = 'again', handler = function(args) print('second:' .. args) end })"), 0);
    CHECK_INT(load("c3.lua", "pi.registerCommand('boom', { handler = function() error('x', 0) end })"), 0);
    n = pi_exts_commands(&ses.ext, rc, PI_EXT_CMDS);
    /* runner.ts resolveRegisteredCommands: extension by extension, a name two extensions register is name:1, name:2 */
    CHECK_INT(n, 4);
    CHECK_STR(rc[0].invocation, "hello:1");
    CHECK_STR(rc[1].invocation, "plain");
    CHECK_STR(rc[2].invocation, "hello:2");
    CHECK_STR(rc[3].invocation, "boom");
    CHECK_STR(rc[0].c->description, "Say hello");
    CHECK(rc[1].c->description == 0);
    CHECK_INT(pi_exts_run_command(&ses.ext, "/hello:2 two words"), 1);
    CHECK_STR(told[0], "second:two words");
    CHECK_INT(pi_exts_run_command(&ses.ext, "/plain"), 1);              /* no space: the args are "" */
    CHECK_STR(told[1], "plain:");
    CHECK_INT(pi_exts_run_command(&ses.ext, "/hello:1  x"), 1);         /* the text after the first space, as typed */
    CHECK(strstr(told[2], "hello: x:") == told[2]);
    CHECK_INT(pi_exts_run_command(&ses.ext, "/hello x"), 0);            /* two extensions have it: only the numbered names run */
    CHECK_INT(pi_exts_run_command(&ses.ext, "/nosuch x"), 0);
    CHECK_INT(pi_exts_run_command(&ses.ext, "plain"), 0);
    CHECK_INT(pi_exts_run_command(&ses.ext, "/"), 0);
    /* a handler that throws: told as pi tells it (agent-session.ts emitError, extensionPath command:<name>), the extension stays */
    told_n = 0;
    CHECK_INT(pi_exts_run_command(&ses.ext, "/boom"), 1);
    CHECK_STR(told[0], "Extension \"command:boom\" error: x");
    CHECK_INT(pi_lua_active(&lx), 3);                   /* the extension stays */
    CHECK_INT(pi_exts_run_command(&ses.ext, "/boom"), 1);
    CHECK_STR(told[1], "Extension \"command:boom\" error: x");    /* and runs again */
    /* definitions pi refuses */
    CHECK_INT(load("c4.lua", "pi.registerCommand('', { handler = function() end })"), -1);
    CHECK(strstr(told[2], "must have a non-empty string name. Use pi.registerCommand(\"name\", { description, handler }).") != 0);
    CHECK_INT(load("c5.lua", "pi.registerCommand('nohandler', { description = 'd' })"), -1);
    CHECK(strstr(told[3], "Command \"/nohandler\" registered by extension \"") != 0 && strstr(told[3], "must define handler().") != 0);
    CHECK_INT(ses.ext.ncmd, 4);
    down();
}

static char ent_type[8][40], ent_data[8][80];
static int ent_n;

static int rec_entry(void *u, pi_session *s, const char *type, const char *data)
{
    (void)u;
    (void)s;
    if (ent_n < 8) {
        strcpy(ent_type[ent_n], type);
        strcpy(ent_data[ent_n], data ? data : "<none>");
    }
    ent_n++;
    return 0;
}

static void test_append_entry(void)
{
    up(0);
    ses.append_entry = rec_entry;
    ent_n = 0;
    /* not while the extension loads (loader.ts notInitialized) */
    CHECK_INT(load("e1.lua", "pi.appendEntry('early', {})"), -1);
    CHECK(strstr(told[0], "Extension runtime not initialized. Action methods cannot be called during extension loading.") != 0);
    CHECK_INT(ent_n, 0);
    CHECK_INT(load("e2.lua", "pi.on('input', function()\n  pi.appendEntry('t1', { a = 1, b = { 'x' } })\n  pi.appendEntry('t2')\n"
                             "  pi.appendEntry('t3', json.null)\n  pi.appendEntry('t4', 'text')\nend)"), 0);
    run_input("x");
    CHECK_INT(ent_n, 4);
    CHECK_STR(ent_type[0], "t1");
    CHECK_STR(ent_data[0], "{\"a\":1,\"b\":[\"x\"]}");
    CHECK_STR(ent_type[1], "t2");
    CHECK_STR(ent_data[1], "<none>");                       /* undefined data: no "data" key (JSON.stringify drops it) */
    CHECK_STR(ent_data[2], "null");
    CHECK_STR(ent_data[3], "\"text\"");
    /* a function cannot be written: the handler fails, the extension stays */
    CHECK_INT(load("e3.lua", "pi.on('input', function() pi.appendEntry('t5', { f = print }) end)"), 0);
    run_input("x");
    CHECK(strstr(told[0], "pi.appendEntry: ") != 0 || strstr(told[1], "pi.appendEntry: ") != 0);
    down();
}

/* the slash menu's rows: the project's extensions first, then the user's; "[p]" and "[u]" with the description, a command
 * named like a built-in one left out (interactive-mode.ts:793-802) */
static void test_ext_menu(void)
{
    cl_cmd *rows;
    char *text, cwd[400], agent[400];
    int n;
    up(1);
    at(cwd, "m/w");
    at(agent, "m/agent");
    put("m/agent/extensions/a.lua", "pi.registerCommand('hello', { description = 'Say hello', handler = function() end })\n"
                                    "pi.registerCommand('tree', { description = 'mine', handler = function() end })");
    put("m/w/.pi/extensions/p.lua", "pi.registerCommand('proj', { handler = function() end })");
    pi_lua_init(&lx, &ses, &sys, cwd, 1);
    CHECK_INT(pi_lua_discover(&lx, agent, 0), 2);
    n = pi_ui_ext_menu(&ses.ext, &rows, &text);
    CHECK_INT(n, 2);
    if (n == 2) {
        CHECK_STR(rows[0].name, "/proj");
        CHECK_STR(rows[0].help, "[p]");
        CHECK_STR(rows[1].name, "/hello");
        CHECK_STR(rows[1].help, "[u] Say hello");
    }
    free(rows);
    free(text);
    down();
    up(0);
    CHECK_INT(pi_ui_ext_menu(&ses.ext, &rows, &text), 0);
    CHECK(rows == 0 && text == 0);
    down();
}

/* L8: the order is read from the list. The gate, a Lua extension and the hook commands announce their ranks and are added in
 * the wrong order (gate first, hooks last): pi_ext_add puts each in place, so the list is hooks, Lua, gate; an extension with no
 * rank goes to the end in the order it was added */
static void test_rank(void)
{
    static const pi_ext gate = { "gate", (void *)"g", 0, c_call, 0, 0, 0, PI_EXT_GATE, 0, 0, 0, 0, 0, 0 };
    static const pi_ext lua2 = { "lua2", (void *)"L", 0, c_call, 0, 0, 0, PI_EXT_LUA, 0, 0, 0, 0, 0, 0 };
    static const pi_ext lua1 = { "lua1", (void *)"l", 0, c_call, 0, 0, 0, PI_EXT_LUA, 0, 0, 0, 0, 0, 0 };
    static const pi_ext plain = { "plain", (void *)"p", 0, c_call, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const pi_ext hooks = { "hooks", (void *)"h", 0, c_call, 0, 0, 0, PI_EXT_HOOKS, 0, 0, 0, 0, 0, 0 };
    pi_exts x;
    jw args;
    char reason[100];
    pi_exts_init(&x);
    jw_init(&args);
    jw_rawz(&args, "{}");
    pi_ext_add(&x, &gate);
    pi_ext_add(&x, &plain);
    pi_ext_add(&x, &lua1);
    pi_ext_add(&x, &lua2);
    pi_ext_add(&x, &hooks);
    seen[0] = 0;
    CHECK_INT(pi_exts_tool_call(&x, "c", "read", &args, reason, sizeof(reason)), 0);
    CHECK_STR(seen, "hlLgp");                           /* hooks, Lua in the order loaded, gate, the unranked one last */
    CHECK_INT(x.n, 5);
    jw_free(&args);
}

/* ---- through pi_app: discovery, trust and -ne on the real command line ---- */

static jw out;

static llm_ts adv_now(void *u)
{
    llm_ts t;
    static long clock_s;
    (void)u;
    t.sec = 1760100000L + clock_s++;
    t.ms = 7;
    return t;
}

static long no_env(const char *name, char *o, long cap)
{
    (void)name;
    (void)o;
    (void)cap;
    return -1;
}

static void l_write(void *u, const char *s, long n)
{
    (void)u;
    jw_raw(&out, s, n);
}

static int l_brk(void *u)
{
    (void)u;
    return 0;
}

/* pi_app -p in root/w with the agent directory root/agent, the message msg, one more flag (0 none) */
static int app_run_msg(const char *flag, const char *msg)
{
    cl_io io;
    pi_platform pl;
    char cwd[400], agent[400], tmp[420], *argv[16];
    int a = 0;
    at(cwd, "app/w");
    at(agent, "app/agent");
    mkdirs(cwd);
    cl_copy(tmp, root, sizeof(tmp));
    cl_cat(tmp, "/", sizeof(tmp));
    memset(&io, 0, sizeof(io));
    io.write = l_write;
    io.brk = l_brk;
    jw_free(&out);
    jw_init(&out);
    ps_reset();
    ps_pool_up(LLM_POOL);
    ps_sse("../pi/openai_text.sse", 0);
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = cwd;
    pl.sys = &sys;
    pl.tmp = tmp;
    pl.utc = 1;
    pl.agent_dir = agent;
    argv[a++] = "amiga-pi";
    argv[a++] = "-p";
    argv[a++] = "--no-session";
    argv[a++] = "--no-mcp";
    if (flag)
        argv[a++] = (char *)flag;
    argv[a++] = "--model";
    argv[a++] = "fixture/qwen2.5-coder:7b";
    argv[a++] = "--base-url";
    argv[a++] = "http://192.168.1.10:11434/v1";
    argv[a++] = (char *)msg;
    argv[a] = 0;
    return pi_app(a, argv, &pl);
}

static int app_run(const char *flag)
{
    return app_run_msg(flag, "hello");
}

static int sent(const char *needle)
{
    return ps.nreq >= 1 && strstr(ps.body[0], needle) != 0;
}

/* the input event of the extensions found by pi_app: the project's (only when trusted: -a) then the user's, each
 * appending its mark to the text the model gets; -ne finds none */
static void test_app(void)
{
    long c0;
    put("app/agent/extensions/u.lua", "pi.on('input', function(e) return { action = 'transform', text = e.text .. ' [user]' } end)");
    put("app/w/.pi/extensions/p.lua", "pi.on('input', function(e) return { action = 'transform', text = e.text .. ' [project]' } end)");
    c0 = pi_lua_calls;
    CHECK_INT(app_run("-a"), 0);
    CHECK(sent("hello [project] [user]"));
    CHECK_INT(pi_lua_calls - c0, 2);                    /* the sentinel: both handlers ran */
    CHECK_INT(app_run("-na"), 0);
    CHECK(sent("hello [user]") && !sent("[project]"));
    CHECK_INT(app_run(0), 0);                           /* print mode does not trust the project */
    CHECK(sent("hello [user]") && !sent("[project]"));
    c0 = pi_lua_calls;
    CHECK_INT(app_run("--no-extensions"), 0);
    CHECK(sent("hello") && !sent("[user]") && !sent("[project]"));
    CHECK_INT(pi_lua_calls - c0, 0);
    CHECK_INT(app_run("-ne"), 0);
    CHECK(!sent("[user]"));
    /* an extension that does not load or throws is told on the console and the prompt still goes out */
    put("app/agent/extensions/a_bad.lua", "this is not lua");
    put("app/agent/extensions/b_throw.lua", "pi.on('input', function() error('oops', 0) end)");
    CHECK_INT(app_run(0), 0);
    CHECK(sent("hello [user]"));
    CHECK(strstr(out.p, "Failed to load extension \"") != 0 && strstr(out.p, "a_bad.lua") != 0);
    /* print mode tells a handler's error as print-mode.ts:101 does (stderr, which the test's platform folds into its console) */
    CHECK(strstr(out.p, "Extension error (") != 0 && strstr(out.p, "b_throw.lua): oops") != 0);
    ps_reset();
}


/* L16: line mode (a console without pi's screen) reads an extension dialog's answer with the time the extension gave it
 * (ExtensionUIDialogOptions.timeout): the console's timed read is polled until the time is up and the dialog answers as pi's
 * does when it times out. The first line typed is the prompt; the extension's input handler asks. With the time given the
 * clock below passes it, so the handler sees "false" (confirm) or nil (select, input); a console without a timed read gets
 * its line read as before. */
static int lm_lines, lm_timed_reads;
static unsigned long lm_clock;

static long lm_read_line(void *u, char *buf, long cap)
{
    (void)u;
    if (lm_lines++ == 0) {
        cl_copy(buf, "hello", cap);
        return 5;
    }
    if (lm_lines == 2) {
        cl_copy(buf, "y", cap);               /* the answer a blocking read would take */
        return 1;
    }
    return -1;
}

static long lm_read(void *u, char *buf, long cap, long timeout_ms)
{
    (void)u;
    (void)buf;
    (void)cap;
    (void)timeout_ms;
    lm_timed_reads++;
    return 0;                                  /* nothing typed */
}

static unsigned long lm_ms(void *u)
{
    (void)u;
    lm_clock += 100;
    return lm_clock;
}

static int lm_sleep(void *u, long ms)
{
    (void)u;
    (void)ms;
    return 0;
}

static int app_line(int timed)
{
    cl_io io;
    pi_platform pl;
    char cwd[400], agent[400], tmp[420], *argv[16];
    int a = 0;
    at(cwd, "app/w");
    at(agent, "app/agent");
    mkdirs(cwd);
    cl_copy(tmp, root, sizeof(tmp));
    cl_cat(tmp, "/", sizeof(tmp));
    memset(&io, 0, sizeof(io));
    io.read_line = lm_read_line;
    io.write = l_write;
    io.brk = l_brk;
    io.ms = lm_ms;
    io.sleep = lm_sleep;
    if (timed)
        io.read = lm_read;
    lm_lines = lm_timed_reads = 0;
    jw_free(&out);
    jw_init(&out);
    ps_reset();
    ps_pool_up(LLM_POOL);
    ps_sse("../pi/openai_text.sse", 0);
    memset(&pl, 0, sizeof(pl));
    pl.io = &io;
    pl.pool = &ps_pool;
    pl.h.poll = ps_poll;
    pl.h.sleep = ps_sleep;
    pl.h.ms = ps_ms;
    pl.h.resolve = ps_resolve;
    pl.now = adv_now;
    pl.getenv = no_env;
    pl.cwd = cwd;
    pl.sys = &sys;
    pl.tmp = tmp;
    pl.utc = 1;
    pl.agent_dir = agent;
    argv[a++] = "amiga-pi";
    argv[a++] = "--no-session";
    argv[a++] = "--no-mcp";
    argv[a++] = "-na";                          /* no trust question: the first line is the prompt */
    argv[a++] = "--model";
    argv[a++] = "fixture/qwen2.5-coder:7b";
    argv[a++] = "--base-url";
    argv[a++] = "http://192.168.1.10:11434/v1";
    argv[a] = 0;
    return pi_app(a, argv, &pl);
}

static void test_line_timeout(void)
{
    put("app/agent/extensions/ask.lua",
        "pi.on('input', function(event, ctx)\n"
        "  local ok = ctx.ui.confirm('Sure?', 'It runs.', { timeout = 300 })\n"
        "  return { action = 'transform', text = event.text .. (ok and ' [answered]' or ' [timed out]') }\n"
        "end)\n");
    CHECK_INT(app_line(1), 0);
    CHECK(sent("hello [timed out]"));
    CHECK(lm_timed_reads >= 2);                 /* polled in slices until the 300 ms were up */
    CHECK_INT(app_line(0), 0);                  /* no timed read: the line is read as before */
    CHECK(sent("hello [answered]"));
    rm_file("app/agent/extensions/ask.lua");
    ps_reset();
}

/* L4 through pi_app: an extension in <agent dir>/extensions registers a command and a tool; "/hello a b" on the command line runs the
 * handler (the sentinel counts that one call) and sends nothing to the model, and the tool is among the tools of an ordinary
 * request unless --tools leaves it out or --no-tools */
static void test_app_register(void)
{
    long c0;
    put("app/agent/extensions/u.lua",
        "pi.registerCommand('hello', { description = 'Say hello', handler = function(args) print('hello ran with ' .. args) end })\n"
        "pi.registerTool({ name = 'greet', description = 'Greets', parameters = { type = 'object' }, execute = function() return { content = {} } end })");
    rm_file("app/agent/extensions/a_bad.lua");
    rm_file("app/agent/extensions/b_throw.lua");
    c0 = pi_lua_calls;
    CHECK_INT(app_run_msg(0, "/hello a b"), 0);
    CHECK_INT(pi_lua_calls - c0, 1);
    CHECK(strstr(out.p, "hello ran with a b") != 0);
    CHECK_INT(ps.nreq, 0);
    CHECK_INT(app_run(0), 0);
    CHECK(sent("\"name\":\"greet\""));
    CHECK_INT(app_run("--no-tools"), 0);
    CHECK(!sent("\"name\":\"greet\""));
    ps_reset();
}

void suite_pi_lua(void)
{
    const char *tmp = getenv("TMPDIR");
    strcpy(root, tmp && tmp[0] ? tmp : "/tmp");
    if (root[strlen(root) - 1] == '/')
        root[strlen(root) - 1] = 0;
    strcat(root, "/pi_lua_XXXXXX");
    if (!mkdtemp(root)) {
        CHECK(0);
        return;
    }
    sys_posix_init(&sp, &sys);
    test_block();
    test_mutate();
    test_result_and_input();
    test_errors();
    test_discover();
    test_limits();
    test_env();
    test_ui();
    test_events_and_api();
    test_l7_events();
    test_order();
    test_rank();
    test_register_tools();
    test_register_commands();
    test_append_entry();
    test_ext_menu();
    jw_init(&out);
    test_app();
    test_app_register();
    test_line_timeout();
    jw_free(&out);
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
