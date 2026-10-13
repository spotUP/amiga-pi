/* amiga-pi's Lua extensions (plan L3; ext_lua.h): the runtime's own rules, with small Lua files as the extensions. What
 * an extension is handed and what its answer does is pi's (the payloads and decisions are ext_payload.c's, compared with
 * pi 1.1.0 in test_pi_modes.c, scenarios ext_lua_*); this suite holds what pi's goldens cannot show -- a file that does
 * not load, a handler that throws, discovery and trust, the limits, the environment, the order of the list.
 *   block      tool_call { block, reason } and a throwing handler's text
 *   mutate     event.input changed in place reaches the tool and the next extension; untouched input stays byte for byte
 *   result     tool_result's patch; input's transform and handled
 *   errors     a load error and a handler error: pi's texts, the extension disabled, the others go on
 *   discover   <cwd>/.pi/extensions only when trusted, then the user's, by name; index.lua in a directory
 *   limits     instructions, time, memory; an extension that catches the limit's error is still stopped
 *   env        no io / os / require / bytecode; json; ctx; print; unsubscribe
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

static void at(char *out, const char *name)
{
    strcpy(out, root);
    strcat(out, "/");
    strcat(out, name);
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
    /* a handler that throws: the call is blocked with the message, the extension is disabled, a second one carries on */
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
        at(path, "throw.lua");
        CHECK(strstr(told[0], "Extension \"") == told[0] && strstr(told[0], path) != 0);
        CHECK(strstr(told[0], "\" error: kaboom (the extension is disabled)") != 0);
        CHECK_INT(lx.x[0].L == 0, 1);
        CHECK_INT(pi_lua_active(&lx), 1);
        /* disabled: not called again; the other extension still decides */
        calls = pi_lua_calls;
        CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "bash", &args, reason, sizeof(reason)), 0);
        CHECK_INT(pi_exts_tool_call(&ses.ext, "c", "edit", &args, reason, sizeof(reason)), 1);
        CHECK_STR(reason, "good");
        CHECK_INT(pi_lua_calls - calls, 2);             /* the good extension's two, none of the disabled one's */
        CHECK_INT(lx.errors, 1);
        CHECK_STR(run_input("a"), "a+");                /* its input handler (that would throw) is gone too */
    }
    down();
    /* an error value that is no string, in an input handler: the text goes on unchanged, the session continues */
    up(0);
    CHECK_INT(load("tbl.lua", "pi.on('input', function() error({}) end)"), 0);
    CHECK_STR(run_input("keep"), "keep");
    CHECK(strstr(told[0], "error: the error value is a table (the extension is disabled)") != 0);
    CHECK_STR(run_input("keep"), "keep");
    CHECK_INT(told_n, 1);
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
    CHECK(strstr(told[1], "busy.lua\" error: instruction limit exceeded (the extension is disabled)") != 0);
    CHECK_INT(pi_lua_active(&lx), 0);
    down();
    /* an extension that catches the limit's error and loops on is stopped all the same */
    up(0);
    lx.instr_cap = 200000;
    CHECK_INT(load("catch.lua", "pi.on('input', function()\n  while true do pcall(function() while true do end end) end\nend)"), 0);
    CHECK_STR(run_input("go"), "go");
    CHECK(strstr(told[0], "limit exceeded") != 0);
    CHECK_INT(pi_lua_active(&lx), 0);
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
    CHECK_INT(pi_lua_active(&lx), 0);
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
              "integerfloatintegerfloat");
    CHECK_STR(eval("json.encode(json.decode('1760000000000'))"), "1.76e+12");   /* 32-bit Lua: a float of 24 bits */
    CHECK_STR(eval("json.encode(0/0) .. json.encode(1/0)"), "nullnull");
    CHECK_STR(eval("select(2, pcall(json.decode, '{oops'))"), "json.decode: invalid JSON");
    CHECK_STR(eval("select(2, pcall(json.encode, { f = print }))"), "json.encode: cannot encode a function, thread or userdata");
    CHECK_STR(eval("select(2, pcall(json.encode, { [1] = 'a', x = 'b' }))"), "json.encode: object keys must be strings");
    CHECK_STR(eval("(function() local t = {}; t.t = t; return select(2, pcall(json.encode, t)) end)()"),
              "json.encode: table nested too deeply (a cycle?)");
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
    static const pi_ext first = { "c-first", (void *)"1", 0, c_call, 0, 0, 0, 0 };
    static const pi_ext last = { "c-last", (void *)"3", 0, c_call, 0, 0, 0, 0 };
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

/* L8: the order is read from the list. The gate, a Lua extension and the hook commands announce their ranks and are added in
 * the wrong order (gate first, hooks last): pi_ext_add puts each in place, so the list is hooks, Lua, gate; an extension with no
 * rank goes to the end in the order it was added */
static void test_rank(void)
{
    static const pi_ext gate = { "gate", (void *)"g", 0, c_call, 0, 0, 0, PI_EXT_GATE };
    static const pi_ext lua2 = { "lua2", (void *)"L", 0, c_call, 0, 0, 0, PI_EXT_LUA };
    static const pi_ext lua1 = { "lua1", (void *)"l", 0, c_call, 0, 0, 0, PI_EXT_LUA };
    static const pi_ext plain = { "plain", (void *)"p", 0, c_call, 0, 0, 0, 0 };
    static const pi_ext hooks = { "hooks", (void *)"h", 0, c_call, 0, 0, 0, PI_EXT_HOOKS };
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

/* pi_app -p in root/w with the agent directory root/agent, the message hello, one more flag (0 none) */
static int app_run(const char *flag)
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
    argv[a++] = "hello";
    argv[a] = 0;
    return pi_app(a, argv, &pl);
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
    CHECK(strstr(out.p, "b_throw.lua\" error: oops (the extension is disabled)") != 0);
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
    test_events_and_api();
    test_order();
    test_rank();
    jw_init(&out);
    test_app();
    jw_free(&out);
    {
        char cmd[600];
        strcpy(cmd, "rm -rf ");
        strcat(cmd, root);
        CHECK_INT(system(cmd), 0);
    }
}
