/* see ext_lua.h */
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "ext_lua.h"
#include "ext_payload.h"
#include "ext_events.h"
#include "mcp.h"
#include "claude/util.h"
#include "claude/path.h"
#include "claude/schema.h"

long pi_lua_calls;
long pi_lua_tool_calls;

#define K_HANDLERS "pi.handlers"        /* registry: event name -> array of handler functions */
#define K_CTX "pi.ctx"                  /* registry: the ctx table every handler gets */
#define K_ARRAY "pi.array"              /* registry: the metatable that marks a table as a JSON array */
#define K_RENDERERS "pi.renderers"      /* registry: customType -> the message renderer function */
#define K_TOOLS "pi.tools"              /* registry: tool slot + 1 -> its execute function */
#define K_CMDS "pi.commands"            /* registry: command slot + 1 -> its handler function */

static pi_lua_ext *self(lua_State *L)
{
    void *ud;
    lua_getallocf(L, &ud);
    return (pi_lua_ext *)ud;
}

/* ---- the limits ---- */

/* the memory cap: Lua asks for every block through here; a refused block is Lua's "not enough memory" error */
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    pi_lua_ext *x = (pi_lua_ext *)ud;
    long old = ptr ? (long)osize : 0;
    void *p;
    if (nsize == 0) {
        if (ptr) {
            x->mem -= (long)osize;
            free(ptr);
        }
        return 0;
    }
    if ((long)nsize > old && x->mem - old + (long)nsize > x->host->mem_cap)
        return 0;
    p = realloc(ptr, nsize);
    if (p)
        x->mem += (long)nsize - old;
    return p;
}

static void arm(pi_lua_ext *x)
{
    pi_session *s = x->host->s;
    x->ticks = 0;
    x->over = 0;
    x->t0 = s->h.ms ? s->h.ms(s->h.u) : 0;
}

/* every 1000 VM instructions: the instruction and the time limits. Past one, the hook fires on every instruction
 * (count 1), so an extension that catches the error with pcall and loops on is stopped by the next instruction. */
static void limit_hook(lua_State *L, lua_Debug *ar)
{
    pi_lua_ext *x = self(L);
    pi_lua *h = x->host;
    const char *why = 0;
    (void)ar;
    if (x->over)
        luaL_error(L, "extension stopped: limit exceeded");
    x->ticks++;
    if (x->ticks * 1000L > h->instr_cap)
        why = "instruction limit exceeded";
    else if (h->ms_cap && h->s->h.ms && (unsigned long)(h->s->h.ms(h->s->h.u) - x->t0) > h->ms_cap)
        why = "time limit exceeded";
    if (why) {
        x->over = 1;
        lua_sethook(L, limit_hook, LUA_MASKCOUNT, 1);
        luaL_error(L, "%s", why);
    }
}

/* ---- JSON <-> Lua ---- */

/* a JSON string's decoded bytes as a Lua string, in Lua's own memory (an error while building it leaks nothing) */
static void push_jstr(lua_State *L, jv v)
{
    luaL_Buffer b;
    long cap = v.n + 1, len;
    char *p = luaL_buffinitsize(L, &b, (size_t)cap);
    len = json_str(v, p, cap);
    luaL_pushresultsize(&b, (size_t)(len < 0 ? 0 : len));
}

static void push_jnum(lua_State *L, jv v)
{
    char t[64];
    long i, k = v.n < 63 ? v.n : 63;
    int is_int = 1, neg = v.p[0] == '-';
    long digits = 0;
    for (i = 0; i < k; i++) {
        char c = v.p[i];
        t[i] = c;
        if (c == '.' || c == 'e' || c == 'E')
            is_int = 0;
        else if (c >= '0' && c <= '9')
            digits++;
    }
    t[k] = 0;
    /* pi's numbers are JS doubles: an integer up to 2^53 is exact in both, so it is a Lua integer; the rest a float */
    if (is_int && v.n < 64 && (digits < 16 || (digits == 16 && strcmp(t + neg, "9007199254740992") <= 0))) {
        lua_Integer n = 0;
        for (i = neg; i < k; i++)
            n = n * 10 + (t[i] - '0');
        lua_pushinteger(L, neg ? -n : n);
    } else
        lua_pushnumber(L, (lua_Number)strtod(t, 0));
}

static void j2l(lua_State *L, jv v, int depth)
{
    luaL_checkstack(L, 6, "json nesting");
    switch (json_type(v)) {
    case J_OBJ: {
        jit it;
        jv k, e;
        lua_createtable(L, 0, 4);
        json_iter(v, &it);
        while (json_next(&it, &k, &e)) {
            if (json_type(e) == J_NULL)         /* JS drops an undefined member, a null one reads as nil */
                continue;
            push_jstr(L, k);
            j2l(L, e, depth + 1);
            lua_rawset(L, -3);
        }
        break;
    }
    case J_ARR: {
        jit it;
        jv e;
        long i = 0;
        lua_createtable(L, (int)json_count(v), 0);
        json_iter(v, &it);
        while (json_next(&it, 0, &e)) {
            j2l(L, e, depth + 1);
            lua_rawseti(L, -2, (lua_Integer)++i);
        }
        luaL_getmetatable(L, K_ARRAY);
        lua_setmetatable(L, -2);
        break;
    }
    case J_STR: push_jstr(L, v); break;
    case J_NUM: push_jnum(L, v); break;
    case J_TRUE: lua_pushboolean(L, 1); break;
    case J_FALSE: lua_pushboolean(L, 0); break;
    default: lua_pushlightuserdata(L, 0); break;    /* null inside an array: json.null */
    }
}

static int c_decode(lua_State *L)
{
    j2l(L, *(const jv *)lua_touserdata(L, 1), 0);
    return 1;
}

/* JSON text (validated here) as a value pushed on L; 0, or -1 (not JSON, or out of memory: nothing pushed) */
static int push_json(lua_State *L, const char *p, long n)
{
    jv v;
    if (json_parse(p, n, &v))
        return -1;
    lua_pushcfunction(L, c_decode);
    lua_pushlightuserdata(L, &v);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

typedef struct skey {
    const char *s;
    size_t n;
} skey;

/* a table has no key order; the output has one, fixed: the keys of pi's content blocks first in pi's order (type, text,
 * data, mimeType: {"type":"text","text":...} is what the model and the records show), the rest sorted */
static int key_rank(const skey *k)
{
    static const char *const first[] = { "type", "text", "data", "mimeType" };
    int i;
    for (i = 0; i < 4; i++)
        if (strlen(first[i]) == k->n && !memcmp(first[i], k->s, k->n))
            return i;
    return 4;
}

static int cmp_key(const void *a, const void *b)
{
    const skey *x = (const skey *)a, *y = (const skey *)b;
    size_t m = x->n < y->n ? x->n : y->n;
    int c = key_rank(x) - key_rank(y);
    if (c)
        return c;
    c = memcmp(x->s, y->s, m);
    if (c)
        return c;
    return x->n < y->n ? -1 : x->n > y->n;
}

static int is_array_mt(lua_State *L, int idx)
{
    int eq;
    if (!lua_getmetatable(L, idx))
        return 0;
    luaL_getmetatable(L, K_ARRAY);
    eq = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return eq;
}

static const char *l2j(lua_State *L, int idx, jw *w, int depth);

static const char *l2j_table(lua_State *L, int idx, jw *w, int depth)
{
    long n = (long)lua_rawlen(L, idx), count = 0, i, m = 0;
    int arr = is_array_mt(L, idx);
    const char *err = 0;
    skey *keys;
    if (!lua_checkstack(L, 8))
        return "stack overflow";
    if (!arr) {
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            count++;
            lua_pop(L, 1);
        }
        arr = n > 0 && count == n;
    }
    if (arr) {
        jw_rawz(w, "[");
        for (i = 1; i <= n && !err; i++) {
            if (i > 1)
                jw_rawz(w, ",");
            lua_rawgeti(L, idx, (lua_Integer)i);
            err = l2j(L, -1, w, depth + 1);
            lua_pop(L, 1);
        }
        jw_rawz(w, "]");
        return err;
    }
    if (count == 0) {
        jw_rawz(w, "{}");
        return 0;
    }
    /* an object: the keys in key_rank / sorted order (the output is deterministic). The array of them is Lua's
     * memory, so an error on the way frees it. */
    keys = (skey *)lua_newuserdatauv(L, sizeof(skey) * (size_t)count, 0);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 3);
            return "object keys must be strings";
        }
        keys[m].s = lua_tolstring(L, -2, &keys[m].n);
        m++;
        lua_pop(L, 1);
    }
    qsort(keys, (size_t)count, sizeof(skey), cmp_key);
    jw_rawz(w, "{");
    for (i = 0; i < count && !err; i++) {
        if (i)
            jw_rawz(w, ",");
        jw_str(w, keys[i].s, (long)keys[i].n);
        jw_rawz(w, ":");
        lua_pushlstring(L, keys[i].s, keys[i].n);
        lua_rawget(L, idx);
        err = l2j(L, -1, w, depth + 1);
        lua_pop(L, 1);
    }
    jw_rawz(w, "}");
    lua_pop(L, 1);                              /* the key array */
    return err;
}

/* the value at idx as JSON; 0, or the reason it has none (a function, a cycle, a key that is not a string) */
static const char *l2j(lua_State *L, int idx, jw *w, int depth)
{
    idx = lua_absindex(L, idx);
    if (depth > JSON_DEPTH)
        return "table nested too deeply (a cycle?)";
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        jw_rawz(w, "null");
        return 0;
    case LUA_TBOOLEAN:
        jw_rawz(w, lua_toboolean(L, idx) ? "true" : "false");
        return 0;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, idx) == 0) {
            jw_rawz(w, "null");
            return 0;
        }
        return "cannot encode a userdata";
    case LUA_TNUMBER:
        /* JSON.stringify of a JS number: a Lua integer is the double JS would hold (exact to 2^53), written by mcp.c */
        pi_js_double(w, lua_isinteger(L, idx) ? (double)lua_tointeger(L, idx) : (double)lua_tonumber(L, idx));
        return 0;
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        jw_str(w, s, (long)n);
        return 0;
    }
    case LUA_TTABLE:
        return l2j_table(L, idx, w, depth);
    }
    return "cannot encode a function, thread or userdata";
}

static int c_encode(lua_State *L)
{
    jw *w = (jw *)lua_touserdata(L, 2);
    const char *err = l2j(L, 1, w, 0);
    if (err)
        lua_pushstring(L, err);
    else
        lua_pushnil(L);
    return 1;
}

/* the value at idx as JSON into w, in a protected call (building it can run out of Lua memory); 0, or -1 with the
 * reason in err */
static int enc_protected(lua_State *L, int idx, jw *w, char *err, long cap)
{
    lua_pushcfunction(L, c_encode);
    lua_pushvalue(L, idx < 0 ? idx - 1 : idx);
    lua_pushlightuserdata(L, w);
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        cl_copy(err, lua_tostring(L, -1) ? lua_tostring(L, -1) : "out of memory", cap);
        lua_pop(L, 1);
        return -1;
    }
    if (!lua_isnil(L, -1)) {
        cl_copy(err, lua_tostring(L, -1), cap);
        lua_pop(L, 1);
        return -1;
    }
    lua_pop(L, 1);
    if (w->oom) {
        cl_copy(err, "out of memory", cap);
        return -1;
    }
    return 0;
}

/* are two values equal, tables member by member? (no allocation) */
static int deep_eq(lua_State *L, int a, int b, int depth)
{
    long na = 0, nb = 0;
    a = lua_absindex(L, a);
    b = lua_absindex(L, b);
    if (lua_type(L, a) != LUA_TTABLE || lua_type(L, b) != LUA_TTABLE)
        return lua_rawequal(L, a, b);
    if (lua_rawequal(L, a, b))
        return 1;
    if (depth > JSON_DEPTH || !lua_checkstack(L, 6))
        return 0;
    lua_pushnil(L);
    while (lua_next(L, a)) {
        int same;
        lua_pushvalue(L, -2);
        lua_rawget(L, b);
        same = !lua_isnil(L, -1) && deep_eq(L, -2, -1, depth + 1);
        lua_pop(L, 2);
        if (!same) {
            lua_pop(L, 1);
            return 0;
        }
        na++;
    }
    lua_pushnil(L);
    while (lua_next(L, b)) {
        nb++;
        lua_pop(L, 1);
    }
    return na == nb;
}

/* ---- the extension's environment ---- */

static int l_json_encode(lua_State *L)
{
    jw w;
    char err[160];
    luaL_checkany(L, 1);
    jw_init(&w);
    if (enc_protected(L, 1, &w, err, sizeof(err))) {
        jw_free(&w);
        return luaL_error(L, "json.encode: %s", err);
    }
    lua_pushlstring(L, w.p ? w.p : "null", (size_t)(w.p ? w.n : 4));
    jw_free(&w);
    return 1;
}

static int l_json_decode(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    jv v;
    if (json_parse(s, (long)n, &v))
        return luaL_error(L, "json.decode: invalid JSON");
    j2l(L, v, 0);
    return 1;
}

/* print: a line of the session's notices (stdout is the screen's, or a protocol's) */
static int l_print(lua_State *L)
{
    int n = lua_gettop(L), i;
    luaL_Buffer b;
    pi_lua_ext *x = self(L);
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++) {
        luaL_tolstring(L, i, 0);
        if (i > 1)
            luaL_addchar(&b, '\t');
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    if (x->host->s->h.notice)
        x->host->s->h.notice(x->host->s->h.u, lua_tostring(L, -1));
    return 0;
}

/* pi.on(event, handler): handler(event, ctx) after the ones registered before it; returns the function that removes it
 * (types.ts: `on(...): () => void`) */
static int pi_off(lua_State *L)
{
    long i, n;
    lua_getfield(L, LUA_REGISTRYINDEX, K_HANDLERS);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_rawget(L, -2);
    if (!lua_istable(L, -1))
        return 0;
    n = (long)lua_rawlen(L, -1);
    for (i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i);
        if (lua_rawequal(L, -1, lua_upvalueindex(2))) {
            long j;
            lua_pop(L, 1);
            for (j = i; j < n; j++) {
                lua_rawgeti(L, -1, (lua_Integer)(j + 1));
                lua_rawseti(L, -2, (lua_Integer)j);
            }
            lua_pushnil(L);
            lua_rawseti(L, -2, (lua_Integer)n);
            return 0;
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int pi_on(lua_State *L)
{
    long n;
    luaL_checktype(L, 1, LUA_TSTRING);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, K_HANDLERS);         /* 3 */
    lua_pushvalue(L, 1);
    lua_rawget(L, 3);                                       /* 4: the list */
    if (!lua_istable(L, 4)) {
        lua_pop(L, 1);
        lua_createtable(L, 4, 0);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, -2);
        lua_rawset(L, 3);
    }
    n = (long)lua_rawlen(L, 4);
    lua_pushvalue(L, 2);
    lua_rawseti(L, 4, (lua_Integer)(n + 1));
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, pi_off, 2);
    return 1;
}

static int ctx_trusted(lua_State *L)
{
    lua_pushboolean(L, self(L)->host->trusted);
    return 1;
}

/* ---- ctx.hasUI and ctx.ui (L5): pi's ExtensionUIContext dialogs (types.ts:149-160) ---- */

#define UI_VALUE 4096                   /* the longest answer of select and input (cut there) */
#define UI_OPTIONS 256                  /* options of one select */

/* ctx.hasUI is read when it is asked: the mode binds its dialogs after the extensions load (runner.ts hasUI()) */
static int ctx_index(lua_State *L)
{
    const char *k = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "";
    if (!strcmp(k, "hasUI")) {
        lua_pushboolean(L, self(L)->host->s->ext.ui != 0);
        return 1;
    }
    return 0;
}

/* the options table's timeout (ExtensionUIDialogOptions.timeout, milliseconds; signal has no meaning here), 0 none */
static long ui_timeout(lua_State *L, int idx)
{
    long ms = 0;
    if (lua_istable(L, idx)) {
        lua_getfield(L, idx, "timeout");
        if (lua_isnumber(L, -1)) {
            lua_Number n = lua_tonumber(L, -1);
            ms = n > 0 && n < 2147483647.0 ? (long)n : 0;
        }
        lua_pop(L, 1);
    }
    return ms;
}

/* a dialog waits for the user as long as it takes: that time is not the handler's (the clock limit counts the handler) */
static unsigned long ui_clock(pi_lua_ext *x)
{
    pi_session *s = x->host->s;
    return s->h.ms ? s->h.ms(s->h.u) : 0;
}

static void ui_waited(pi_lua_ext *x, unsigned long t0)
{
    x->t0 += ui_clock(x) - t0;
}

/* a dialog opens or closes: ui_prompt_start / ui_prompt_end for the outermost one (runner.ts withUIPrompt) */
static void prompt_event(pi_lua_ext *x, int start, const char *kind, const char *title)
{
    pi_exts *e = &x->host->s->ext;
    if (start ? e->prompt_depth++ > 0 : --e->prompt_depth > 0)
        return;
    pi_ev_ui_prompt(x->host->s, start, kind, title);
}

/* ctx.ui.select(title, options, { timeout }) -> the chosen string, or nil (cancelled, timed out, no UI) */
static int ui_select(lua_State *L)
{
    pi_lua_ext *x = self(L);
    const pi_ext_ui *ui = x->host->s->ext.ui;
    const char *title = luaL_checkstring(L, 1);
    const char **opt;
    char *value;
    long n, i, ms = ui_timeout(L, 3);
    unsigned long t0;
    int ok = 0;
    luaL_checktype(L, 2, LUA_TTABLE);
    n = (long)lua_rawlen(L, 2);
    if (n > UI_OPTIONS)
        n = UI_OPTIONS;
    if (!ui || !ui->select || n < 1) {
        lua_pushnil(L);
        return 1;
    }
    luaL_checkstack(L, (int)n + 4, "too many options");
    opt = (const char **)lua_newuserdatauv(L, (size_t)n * sizeof(char *), 0);      /* collected with the call */
    for (i = 0; i < n; i++) {
        lua_rawgeti(L, 2, (lua_Integer)(i + 1));
        opt[i] = luaL_tolstring(L, -1, 0);                  /* kept on the stack until the call ends */
        lua_remove(L, -2);
    }
    value = (char *)malloc(UI_VALUE);
    if (!value)
        return luaL_error(L, "not enough memory");
    value[0] = 0;
    prompt_event(x, 1, "select", title);
    t0 = ui_clock(x);
    ok = ui->select(ui->u, title, opt, (int)n, ms, value, UI_VALUE);
    ui_waited(x, t0);
    prompt_event(x, 0, "select", title);
    if (ok)
        lua_pushstring(L, value);
    else
        lua_pushnil(L);
    free(value);
    return 1;
}

/* ctx.ui.confirm(title, message, { timeout }) -> true or false (declined, cancelled, timed out, no UI) */
static int ui_confirm(lua_State *L)
{
    pi_lua_ext *x = self(L);
    const pi_ext_ui *ui = x->host->s->ext.ui;
    const char *title = luaL_checkstring(L, 1), *message = luaL_checkstring(L, 2);
    long ms = ui_timeout(L, 3);
    unsigned long t0;
    int yes = 0;
    if (ui && ui->confirm) {
        prompt_event(x, 1, "confirm", title);
        t0 = ui_clock(x);
        yes = ui->confirm(ui->u, title, message, ms);
        ui_waited(x, t0);
        prompt_event(x, 0, "confirm", title);
    }
    lua_pushboolean(L, yes);
    return 1;
}

/* ctx.ui.input(title, placeholder, { timeout }) -> the text, or nil */
static int ui_input(lua_State *L)
{
    pi_lua_ext *x = self(L);
    const pi_ext_ui *ui = x->host->s->ext.ui;
    const char *title = luaL_checkstring(L, 1);
    const char *ph = lua_isnoneornil(L, 2) ? 0 : luaL_checkstring(L, 2);
    long ms = ui_timeout(L, 3);
    unsigned long t0;
    char *value;
    int ok;
    if (!ui || !ui->input) {
        lua_pushnil(L);
        return 1;
    }
    value = (char *)malloc(UI_VALUE);
    if (!value)
        return luaL_error(L, "not enough memory");
    value[0] = 0;
    prompt_event(x, 1, "input", title);
    t0 = ui_clock(x);
    ok = ui->input(ui->u, title, ph, ms, value, UI_VALUE);
    ui_waited(x, t0);
    prompt_event(x, 0, "input", title);
    if (ok)
        lua_pushstring(L, value);
    else
        lua_pushnil(L);
    free(value);
    return 1;
}

/* ctx.ui.notify(message, type): type "info", "warning" or "error"; nothing happens without a UI */
static int ui_notify(lua_State *L)
{
    const pi_ext_ui *ui = self(L)->host->s->ext.ui;
    const char *message = luaL_checkstring(L, 1);
    const char *type = lua_isnoneornil(L, 2) ? 0 : luaL_checkstring(L, 2);
    if (ui && ui->notify)
        ui->notify(ui->u, message, type);
    return 0;
}

static void ext_error_at(pi_lua_ext *x, const char *shown, const char *msg);
static void settle(pi_lua_ext *x, int top);

/* ---- tools, commands and entries an extension registers (L4, L6) ---- */

static void tell(pi_lua *h, const char *m);

static char *dupn(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}

/* agent-session.ts _normalizePromptSnippet: the text on one line, white space collapsed; 0 when nothing is left */
static char *normal_snippet(const char *s)
{
    size_t n = strlen(s), i, k = 0;
    char *p = (char *)malloc(n + 1);
    int space = 0;
    if (!p)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = k > 0;
            continue;
        }
        if (space)
            p[k++] = ' ';
        space = 0;
        p[k++] = c;
    }
    p[k] = 0;
    if (!k) {
        free(p);
        return 0;
    }
    return p;
}

static void free_tool(pi_lua_tool *t)
{
    int i;
    free(t->name);
    free(t->desc);
    free(t->params);
    free(t->snippet);
    free(t->constrained);
    for (i = 0; t->guide && t->guide[i]; i++)
        free(t->guide[i]);
    free(t->guide);
    t->name = t->desc = t->params = t->snippet = t->constrained = 0;
    t->guide = 0;
}

static void free_registered(pi_lua_ext *x)
{
    int i;
    for (i = 0; i < x->ntool; i++)
        free_tool(&x->tool[i]);
    for (i = 0; i < x->ncmd; i++) {
        free(x->cmd[i]);
        free(x->cmd_desc[i]);
    }
    x->ntool = x->ncmd = 0;
}

static int lt_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error);

/* the tool joins the session's tools (a tool of that name from a built-in is replaced as agent-session.ts
 * _refreshToolRegistry does, an earlier extension's wins as runner.ts getAllRegisteredTools), when --tools allows it */
static void add_tool(pi_lua_ext *x, pi_lua_tool *t)
{
    pi_lua *h = x->host;
    pi_session *s = h->s;
    int i;
    if (t->in_session || (h->tool_allowed && !h->tool_allowed(h->tool_u, t->name)))
        return;
    for (i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i]->name, t->name)) {
            if (s->tools[i]->run != lt_run)
                s->tools[i] = &t->t;
            else
                return;
            t->in_session = 1;
            return;
        }
    if (pi_tool_add(s, &t->t))
        tell(h, "Too many tools: the extension's tool is not available");
    else
        t->in_session = 1;
}

/* the commands and tools an extension registered while it loaded, put in the session's lists once it loaded */
static void commit(pi_lua_ext *x)
{
    int i;
    x->loaded = 1;
    for (i = 0; i < x->ncmd; i++)
        pi_exts_add_command(&x->host->s->ext, &x->ext, x->cmd[i], x->cmd_desc[i], i);
    for (i = 0; i < x->ntool; i++)
        add_tool(x, &x->tool[i]);
}

/* the field key of the table at idx as a string, 0 when it is not one (the pointer is valid while the value stays in the table) */
static const char *str_field(lua_State *L, int idx, const char *key)
{
    const char *r;
    lua_getfield(L, idx, key);
    r = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : 0;
    lua_pop(L, 1);
    return r;
}

/* the value at idx encoded into w (a table that is empty or an array of blocks); 0 or the reason */
static int enc_to(lua_State *L, int idx, jw *w, char *err, long cap)
{
    jw_reset(w);
    return enc_protected(L, idx, w, err, cap);
}

/* pi.registerTool(def): types.ts ToolDefinition as a table (loader.ts:289-301) */
static int pi_register_tool(lua_State *L)
{
    pi_lua_ext *x = self(L);
    pi_lua_tool *t;
    const char *name, *desc, *snip;
    char err[200], msg[400];
    jw params, cons;
    char **guide = 0;
    int i, n = 0, ng = 0, rc = 0;
    luaL_checktype(L, 1, LUA_TTABLE);
    name = str_field(L, 1, "name");
    if (!name || !*name)
        return luaL_error(L, "pi.registerTool: the tool needs a name");
    lua_getfield(L, 1, "parameters");                       /* 2 */
    if (lua_type(L, 2) != LUA_TTABLE || lua_rawlen(L, 2) > 0) {
        cl_copy(msg, "Tool \"", sizeof(msg));
        cl_cat(msg, name, sizeof(msg));
        cl_cat(msg, "\" registered by extension \"", sizeof(msg));
        cl_cat(msg, x->path, sizeof(msg));
        cl_cat(msg, "\" must define an object parameter schema.", sizeof(msg));
        return luaL_error(L, "%s", msg);
    }
    lua_getfield(L, 1, "promptGuidelines");                 /* 3 */
    if (lua_type(L, 3) == LUA_TTABLE) {
        ng = (int)lua_rawlen(L, 3);
        for (i = 1; i <= ng; i++) {
            lua_rawgeti(L, 3, (lua_Integer)i);
            if (lua_type(L, -1) != LUA_TSTRING)
                return luaL_error(L, "pi.registerTool: promptGuidelines must be an array of strings");
            lua_pop(L, 1);
        }
    }
    if (x->ntool == PI_LUA_TOOLS) {
        for (i = 0; i < x->ntool; i++)
            if (!strcmp(x->tool[i].name, name))
                break;
        if (i == x->ntool)
            return luaL_error(L, "pi.registerTool: too many tools (at most %d per extension)", PI_LUA_TOOLS);
    }
    jw_init(&params);
    jw_init(&cons);
    if (enc_to(L, 2, &params, err, sizeof(err))) {
        jw_free(&params);
        jw_free(&cons);
        return luaL_error(L, "pi.registerTool: parameters: %s", err);
    }
    lua_getfield(L, 1, "constrainedSampling");              /* 4 */
    if (lua_type(L, 4) == LUA_TTABLE && enc_to(L, 4, &cons, err, sizeof(err))) {
        jw_free(&params);
        jw_free(&cons);
        return luaL_error(L, "pi.registerTool: constrainedSampling: %s", err);
    }
    desc = str_field(L, 1, "description");
    snip = str_field(L, 1, "promptSnippet");
    /* nothing below raises a Lua error: what is allocated is freed on the way out */
    for (i = 0; i < x->ntool; i++)
        if (!strcmp(x->tool[i].name, name))
            break;
    n = i;
    t = &x->tool[n];
    if (n < x->ntool)
        free_tool(t);                                       /* the extension registers the name again: it replaces the first */
    else {
        memset(t, 0, sizeof(*t));
        x->ntool++;
    }
    t->x = x;
    t->idx = n;
    t->name = dupn(name, strlen(name));
    t->desc = dupn(desc ? desc : "", desc ? strlen(desc) : 0);
    t->params = dupn(params.p ? params.p : "{}", params.p ? (size_t)params.n : 2);
    t->snippet = snip ? normal_snippet(snip) : 0;
    t->constrained = cons.n ? dupn(cons.p, (size_t)cons.n) : 0;
    guide = (char **)calloc((size_t)ng + 1, sizeof(char *));
    if (guide) {
        int k = 0, j;
        for (i = 1; i <= ng; i++) {
            const char *g, *e;
            size_t len;
            lua_rawgeti(L, 3, (lua_Integer)i);
            g = lua_tostring(L, -1);
            while (*g == ' ' || *g == '\t' || *g == '\r' || *g == '\n')
                g++;
            e = g + strlen(g);
            while (e > g && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
                e--;
            len = (size_t)(e - g);
            for (j = 0; j < k; j++)                         /* _normalizePromptGuidelines: trimmed, empty ones and repeats dropped */
                if (strlen(guide[j]) == len && !memcmp(guide[j], g, len))
                    break;
            if (len && j == k && (guide[k] = dupn(g, len)) != 0)
                k++;
            lua_pop(L, 1);
        }
        if (!k) {
            free(guide);
            guide = 0;
        }
    }
    t->guide = guide;
    jw_free(&params);
    jw_free(&cons);
    if (!t->name || !t->desc || !t->params) {
        rc = 1;
    } else {
        memset(&t->t, 0, sizeof(t->t));
        t->t.name = t->name;
        t->t.description = t->desc;
        t->t.parameters = t->params;
        t->t.snippet = t->snippet;
        t->t.guide = (const char *const *)t->guide;
        t->t.constrained = t->constrained;
        t->t.u = t;
        t->t.run = lt_run;
        lua_getfield(L, LUA_REGISTRYINDEX, K_TOOLS);
        lua_getfield(L, 1, "execute");
        lua_rawseti(L, -2, (lua_Integer)(n + 1));
        lua_pop(L, 1);
        if (x->loaded)
            add_tool(x, t);
    }
    if (rc)
        return luaL_error(L, "pi.registerTool: out of memory");
    return 0;
}

/* pi.registerCommand(name, { description, handler }) (loader.ts:303-322) */
static int pi_register_command(lua_State *L)
{
    pi_lua_ext *x = self(L);
    size_t n;
    const char *name, *desc;
    char msg[500];
    int i;
    if (lua_type(L, 1) != LUA_TSTRING || !(name = lua_tolstring(L, 1, &n)) || n == 0) {
        cl_copy(msg, "Command registered by extension \"", sizeof(msg));
        cl_cat(msg, x->path, sizeof(msg));
        cl_cat(msg, "\" must have a non-empty string name. Use pi.registerCommand(\"name\", { description, handler }).", sizeof(msg));
        return luaL_error(L, "%s", msg);
    }
    if (lua_type(L, 2) != LUA_TTABLE) {
        lua_pushnil(L);
        lua_replace(L, 2);
    } else
        lua_getfield(L, 2, "handler");                      /* 3 */
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        cl_copy(msg, "Command \"/", sizeof(msg));
        cl_cat(msg, name, sizeof(msg));
        cl_cat(msg, "\" registered by extension \"", sizeof(msg));
        cl_cat(msg, x->path, sizeof(msg));
        cl_cat(msg, "\" must define handler().", sizeof(msg));
        return luaL_error(L, "%s", msg);
    }
    desc = lua_type(L, 2) == LUA_TTABLE ? str_field(L, 2, "description") : 0;
    for (i = 0; i < x->ncmd; i++)
        if (!strcmp(x->cmd[i], name))
            break;
    if (i == x->ncmd && x->ncmd == PI_LUA_CMDS)
        return luaL_error(L, "pi.registerCommand: too many commands (at most %d per extension)", PI_LUA_CMDS);
    {
        char *nm = i < x->ncmd ? x->cmd[i] : dupn(name, n), *ds = desc ? dupn(desc, strlen(desc)) : 0;
        if (!nm || (desc && !ds)) {
            if (i == x->ncmd)
                free(nm);
            free(ds);
            return luaL_error(L, "pi.registerCommand: out of memory");
        }
        free(x->cmd_desc[i]);
        x->cmd[i] = nm;
        x->cmd_desc[i] = ds;
        if (i == x->ncmd)
            x->ncmd++;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, K_CMDS);
    lua_pushvalue(L, 3);
    lua_rawseti(L, -2, (lua_Integer)(i + 1));
    lua_pop(L, 1);
    if (x->loaded)
        pi_exts_add_command(&x->host->s->ext, &x->ext, x->cmd[i], x->cmd_desc[i], i);
    return 0;
}

/* pi.appendEntry(customType, data): not while the extension loads (loader.ts:163 notInitialized); data nil leaves the key out,
 * as JSON.stringify does for undefined, json.null writes null */
static int pi_append_entry(lua_State *L)
{
    pi_lua_ext *x = self(L);
    pi_session *s = x->host->s;
    const char *type = luaL_checkstring(L, 1);
    jw w;
    char err[200];
    int rc;
    if (x->loading)
        return luaL_error(L, "Extension runtime not initialized. Action methods cannot be called during extension loading.");
    jw_init(&w);
    if (!lua_isnoneornil(L, 2) && enc_protected(L, 2, &w, err, sizeof(err))) {
        jw_free(&w);
        return luaL_error(L, "pi.appendEntry: %s", err);
    }
    rc = s->append_entry ? s->append_entry(s->append_u, s, type, w.n ? w.p : 0) : 0;
    jw_free(&w);
    if (rc)
        return luaL_error(L, "pi.appendEntry: the entry could not be written");
    return 0;
}

/* onUpdate({ content, details }) of a running tool: a tool_execution_update (agent-loop.ts emitToolExecutionUpdate) */
static int lt_update(lua_State *L)
{
    pi_lua_tool *t = (pi_lua_tool *)lua_touserdata(L, lua_upvalueindex(1));
    pi_session *s = t->x->host->s;
    pi_event e;
    jw c, d;
    char err[200];
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!s->call_id)
        return 0;
    jw_init(&c);
    jw_init(&d);
    lua_getfield(L, 1, "content");
    if (lua_istable(L, -1)) {
        if (enc_to(L, -1, &c, err, sizeof(err))) {
            jw_free(&c);
            jw_free(&d);
            return luaL_error(L, "onUpdate: content: %s", err);
        }
        if (c.n == 2 && !memcmp(c.p, "{}", 2))
            jw_reset(&c);
    }
    lua_pop(L, 1);
    if (!c.n)
        jw_rawz(&c, "[]");
    lua_getfield(L, 1, "details");
    if (!lua_isnil(L, -1) && enc_to(L, -1, &d, err, sizeof(err))) {
        jw_free(&c);
        jw_free(&d);
        return luaL_error(L, "onUpdate: details: %s", err);
    }
    lua_pop(L, 1);
    pi_event_init(&e, PE_TOOL_UPDATE);
    e.tool_id = s->call_id;
    e.tool_name = t->name;
    e.args = s->call_args ? s->call_args : "{}";
    e.args_n = s->call_args ? s->call_args_n : 2;
    e.result = c.p;
    e.result_n = c.n;
    e.structured = d.n ? d.p : 0;
    e.structured_n = d.n;
    pi_emit(&s->em, &e);
    jw_free(&c);
    jw_free(&d);
    return 0;
}

/* a failed call's text as the tool's error result (agent-loop.ts createErrorToolResult) */
static int tool_fail(jw *content, const char *text)
{
    jw_reset(content);
    jw_rawz(content, "[");
    llm_b_text(content, text, (long)strlen(text), 0);
    jw_rawz(content, "]");
    return 0;
}

/* the tool's call: arguments checked against its schema (the same check as the built-in tools and MCP: claude/schema.c),
 * execute(toolCallId, params, signal, onUpdate, ctx); its result {content, details, structuredContent, isError} or its error */
static int lt_run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    pi_lua_tool *t = (pi_lua_tool *)u;
    pi_lua_ext *x = t->x;
    lua_State *L = x->L;
    jv a, sch;
    char err[300];
    int top, rc;
    *is_error = 1;
    if (json_parse(args, an, &a) || json_type(a) != J_OBJ) {
        cl_copy(err, "Validation failed for tool \"", sizeof(err));
        cl_cat(err, t->name, sizeof(err));
        cl_cat(err, "\": the arguments are not a JSON object", sizeof(err));
        return tool_fail(content, err);
    }
    if (!json_parse(t->params, (long)strlen(t->params), &sch) && schema_check(sch, a, err, sizeof(err))) {
        char m[500];
        cl_copy(m, "Validation failed for tool \"", sizeof(m));
        cl_cat(m, t->name, sizeof(m));
        cl_cat(m, "\":\n  - ", sizeof(m));
        cl_cat(m, err, sizeof(m));
        return tool_fail(content, m);
    }
    top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, K_TOOLS);
    lua_rawgeti(L, -1, (lua_Integer)(t->idx + 1));
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return tool_fail(content, "tool.execute is not a function");
    }
    lua_pushstring(L, s && s->call_id ? s->call_id : "call");
    if (push_json(L, args, an)) {
        lua_settop(L, top);
        settle(x, top);
        return tool_fail(content, "out of memory");
    }
    lua_pushnil(L);                                         /* signal */
    lua_pushlightuserdata(L, t);
    lua_pushcclosure(L, lt_update, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, K_CTX);
    arm(x);
    pi_lua_tool_calls++;
    rc = lua_pcall(L, 5, 1, 0);
    if (rc != LUA_OK) {
        /* a thrown error is the tool's error result with details {} (the loop's `thrown`) */
        cl_copy(err, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "the error value is not a string", sizeof(err));
        lua_settop(L, top);
        return tool_fail(content, err);
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return tool_fail(content, "the tool must return a table with content");
    }
    {
        jw w;
        int ok = 1;
        jw_init(&w);
        lua_getfield(L, -1, "content");
        if (lua_istable(L, -1) && enc_to(L, -1, &w, err, sizeof(err)) == 0) {
            if (w.n == 2 && !memcmp(w.p, "{}", 2))
                jw_reset(&w);
            jw_reset(content);
            jw_raw(content, w.n ? w.p : "[]", w.n ? w.n : 2);
        } else if (lua_isnil(L, -1)) {
            jw_reset(content);
            jw_rawz(content, "[]");
        } else
            ok = 0;
        lua_pop(L, 1);
        if (ok && s) {
            lua_getfield(L, -1, "details");
            if (!lua_isnil(L, -1) && enc_to(L, -1, &w, err, sizeof(err)) == 0) {
                jw_reset(&s->details);
                jw_raw(&s->details, w.p, w.n);
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "structuredContent");
            if (!lua_isnil(L, -1) && enc_to(L, -1, &w, err, sizeof(err)) == 0) {
                jw_reset(&s->structured);
                jw_raw(&s->structured, w.p, w.n);
            }
            lua_pop(L, 1);
        }
        jw_free(&w);
        if (!ok) {
            lua_settop(L, top);
            return tool_fail(content, "the tool's content must be an array of blocks");
        }
    }
    if (s) {
        jw w;
        char e2[200];
        jw_init(&w);
        lua_getfield(L, -1, "usage");
        if (lua_istable(L, -1) && enc_to(L, -1, &w, e2, sizeof(e2)) == 0) {
            jv uo;
            jw_reset(&s->usage);
            if (!json_parse(w.p, w.n, &uo) && json_type(uo) == J_OBJ)
                pi_ext_usage_order(uo, &s->usage);
            else
                jw_raw(&s->usage, w.p, w.n);
        }
        lua_pop(L, 1);
        jw_free(&w);
        lua_getfield(L, -1, "terminate");
        s->call_terminate = lua_toboolean(L, -1);           /* AgentToolResult.terminate: the batch may end the run */
        lua_pop(L, 1);
    }
    lua_getfield(L, -1, "isError");
    *is_error = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (*is_error && s)
        s->result_error = 1;                                /* the result carries isError: it is no thrown error */
    lua_settop(L, top);
    return 0;
}

/* a registered command run: handler(args, ctx) (agent-session.ts _tryExecuteExtensionCommand); an error is told as pi tells it,
 * "command:<name>" the extension; the extension keeps running (pi has no disabling) */
static int lx_command(void *u, int handle, const char *args)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    int top, rc;
    char err[300], shown[140];
    top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, K_CMDS);
    lua_rawgeti(L, -1, (lua_Integer)(handle + 1));
    lua_remove(L, -2);
    lua_pushstring(L, args);
    lua_getfield(L, LUA_REGISTRYINDEX, K_CTX);
    arm(x);
    pi_lua_calls++;
    rc = lua_pcall(L, 2, 0, 0);
    if (rc != LUA_OK) {
        cl_copy(err, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "the error value is not a string", sizeof(err));
        cl_copy(shown, "command:", sizeof(shown));
        cl_cat(shown, x->cmd[handle], sizeof(shown));
        x->event = "command";
        ext_error_at(x, shown, err);
    }
    settle(x, top);
    return rc == LUA_OK ? 0 : -1;
}

/* pi.sendMessage(message, { triggerTurn, deliverAs }) (agent-session.ts sendCustomMessage): message {customType, content (a string or
 * blocks), display, details} */
static int pi_send_message(lua_State *L)
{
    pi_lua_ext *x = self(L);
    pi_session *s = x->host->s;
    jw w;
    char err[200];
    int trigger = -1, rc;
    const char *deliver = 0;
    if (x->loading)
        return luaL_error(L, "Extension runtime not initialized. Action methods cannot be called during extension loading.");
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "triggerTurn");
        if (!lua_isnil(L, -1))
            trigger = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "deliverAs");
        if (lua_type(L, -1) == LUA_TSTRING)
            deliver = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    jw_init(&w);
    if (enc_protected(L, 1, &w, err, sizeof(err))) {
        jw_free(&w);
        return luaL_error(L, "pi.sendMessage: %s", err);
    }
    rc = pi_session_send_custom(s, w.p, trigger, deliver);
    jw_free(&w);
    if (rc)
        return luaL_error(L, "pi.sendMessage: the message could not be sent");
    return 0;
}

/* pi.registerMessageRenderer(customType, function(message, options, theme)) -> a string or an array of lines (pi's returns a Component) */
static int pi_register_renderer(lua_State *L)
{
    luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, K_RENDERERS);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    return 0;
}

/* theme.fg(name, text), theme.bg(name, text) and the text styles: pi's Theme wraps the text in the colour's escape sequence */
static int theme_color(lua_State *L, int bg)
{
    const pi_ext_render *r = (const pi_ext_render *)lua_touserdata(L, lua_upvalueindex(1));
    const char *name = luaL_checkstring(L, 1), *text = luaL_checkstring(L, 2);
    char sgr[64];
    luaL_Buffer b;
    if (!r->sgr || !r->sgr(r->u, name, bg, sgr, sizeof(sgr)))
        return luaL_error(L, "Unknown theme color: %s", name);
    luaL_buffinit(L, &b);
    luaL_addstring(&b, sgr);
    luaL_addstring(&b, text);
    luaL_addstring(&b, bg ? "\033[49m" : "\033[39m");
    luaL_pushresult(&b);
    return 1;
}

static int theme_fg(lua_State *L)
{
    return theme_color(L, 0);
}

static int theme_bg(lua_State *L)
{
    return theme_color(L, 1);
}

static int theme_style(lua_State *L)
{
    const char *on = lua_tostring(L, lua_upvalueindex(1)), *off = lua_tostring(L, lua_upvalueindex(2));
    const char *text = luaL_checkstring(L, 1);
    lua_pushfstring(L, "%s%s%s", on, text, off);
    return 1;
}

static void theme_new(lua_State *L, const pi_ext_render *r)
{
    static const char *const style[][3] = { { "bold", "\033[1m", "\033[22m" }, { "italic", "\033[3m", "\033[23m" },
                                            { "underline", "\033[4m", "\033[24m" }, { "inverse", "\033[7m", "\033[27m" },
                                            { "strikethrough", "\033[9m", "\033[29m" } };
    int i;
    lua_newtable(L);
    lua_pushlightuserdata(L, (void *)r);
    lua_pushcclosure(L, theme_fg, 1);
    lua_setfield(L, -2, "fg");
    lua_pushlightuserdata(L, (void *)r);
    lua_pushcclosure(L, theme_bg, 1);
    lua_setfield(L, -2, "bg");
    for (i = 0; i < 5; i++) {
        lua_pushstring(L, style[i][1]);
        lua_pushstring(L, style[i][2]);
        lua_pushcclosure(L, theme_style, 2);
        lua_setfield(L, -2, style[i][0]);
    }
}

/* the renderer of customType (pi.registerMessageRenderer): called with the message, { expanded, outputPad } and the theme; the lines it
 * returns (a string split at line feeds, or an array of strings) are drawn instead of the default box. An error or nothing back is the
 * default drawing (custom-message.ts: the catch falls through) */
static int lx_render(void *u, const char *type, const char *msg, long n, const pi_ext_render *r, jw *lines)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    int top, got = 0;
    if (!L)
        return 0;
    top = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, K_RENDERERS);
    lua_getfield(L, -1, type);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return 0;
    }
    x->event = "render";
    if (push_json(L, msg, n)) {
        lua_settop(L, top);
        return 0;
    }
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, r->expanded);
    lua_setfield(L, -2, "expanded");
    lua_pushinteger(L, r->pad);
    lua_setfield(L, -2, "outputPad");
    theme_new(L, r);
    arm(x);
    pi_lua_calls++;
    if (lua_pcall(L, 3, 1, 0) == LUA_OK) {
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *z = lua_tostring(L, -1);
            jw_rawz(lines, z);
            jw_rawz(lines, "\n");
            got = 1;
        } else if (lua_istable(L, -1)) {
            long i, cnt = (long)lua_rawlen(L, -1);
            for (i = 1; i <= cnt; i++) {
                lua_rawgeti(L, -1, (lua_Integer)i);
                if (lua_type(L, -1) == LUA_TSTRING) {
                    jw_rawz(lines, lua_tostring(L, -1));
                    jw_rawz(lines, "\n");
                    got = 1;
                }
                lua_pop(L, 1);
            }
        }
    } else {
        char err[300];
        cl_copy(err, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "the error value is not a string", sizeof(err));
        x->host->errors++;
        (void)err;                            /* pi swallows a renderer's error and draws the default */
    }
    settle(x, top);
    return got;
}

/* the state's contents, in a protected call (memory errors raise) */
static int c_setup(lua_State *L)
{
    static const luaL_Reg libs[] = {
        { "_G", luaopen_base }, { LUA_COLIBNAME, luaopen_coroutine }, { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string }, { LUA_MATHLIBNAME, luaopen_math }, { LUA_UTF8LIBNAME, luaopen_utf8 }, { 0, 0 }
    };
    const luaL_Reg *l;
    pi_lua_ext *x = self(L);
    for (l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    /* no files, no bytecode: the host decides what an extension can reach (io and os are not opened, L4 adds pi.exec) */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    if (luaL_dostring(L, "local ld = load\nload = function(chunk, name, mode, ...) return ld(chunk, name, 't', ...) end"))
        return lua_error(L);
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    luaL_newmetatable(L, K_ARRAY);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, K_HANDLERS);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, K_TOOLS);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, K_CMDS);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, K_RENDERERS);
    /* json */
    lua_newtable(L);
    lua_pushcfunction(L, l_json_encode);
    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, l_json_decode);
    lua_setfield(L, -2, "decode");
    lua_pushlightuserdata(L, 0);
    lua_setfield(L, -2, "null");
    lua_setglobal(L, "json");
    /* ctx */
    lua_newtable(L);
    lua_pushstring(L, x->host->cwd ? x->host->cwd : "");
    lua_setfield(L, -2, "cwd");
    lua_pushcfunction(L, ctx_trusted);
    lua_setfield(L, -2, "isProjectTrusted");
    lua_newtable(L);
    lua_pushcfunction(L, ui_select);
    lua_setfield(L, -2, "select");
    lua_pushcfunction(L, ui_confirm);
    lua_setfield(L, -2, "confirm");
    lua_pushcfunction(L, ui_input);
    lua_setfield(L, -2, "input");
    lua_pushcfunction(L, ui_notify);
    lua_setfield(L, -2, "notify");
    lua_setfield(L, -2, "ui");
    lua_newtable(L);                    /* hasUI answers for the mode as it is now */
    lua_pushcfunction(L, ctx_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, K_CTX);
    /* pi */
    lua_newtable(L);
    lua_pushcfunction(L, pi_on);
    lua_setfield(L, -2, "on");
    lua_pushcfunction(L, pi_register_tool);
    lua_setfield(L, -2, "registerTool");
    lua_pushcfunction(L, pi_register_command);
    lua_setfield(L, -2, "registerCommand");
    lua_pushcfunction(L, pi_append_entry);
    lua_setfield(L, -2, "appendEntry");
    lua_pushcfunction(L, pi_send_message);
    lua_setfield(L, -2, "sendMessage");
    lua_pushcfunction(L, pi_register_renderer);
    lua_setfield(L, -2, "registerMessageRenderer");
    lua_setglobal(L, "pi");
    return 0;
}

/* ---- reporting ---- */

static void tell(pi_lua *h, const char *m)
{
    h->errors++;
    if (h->s->h.notice)
        h->s->h.notice(h->s->h.u, m);
}

/* pi's text for an extension that failed: interactive-mode.ts showExtensionError; the extension is called again (runner.ts
 * emitError only reports); shown is the
 * path pi names (the file's, or "command:<name>" for a command's handler: agent-session.ts _tryExecuteExtensionCommand) */
static void ext_error_at(pi_lua_ext *x, const char *shown, const char *msg)
{
    char m[900];
    cl_copy(m, msg, sizeof(m) - 40);
    x->host->errors++;
    pi_ev_error(x->host->s, shown, x->event ? x->event : "", m);       /* each mode tells it its own way */
}

static void ext_error(pi_lua_ext *x, const char *msg)
{
    ext_error_at(x, x->path, msg);
}

/* after a dispatch, back on the stack's base */
static void settle(pi_lua_ext *x, int top)
{
    lua_settop(x->L, top);
}

/* ---- dispatch ---- */

/* a snapshot of the handler list of event on the stack (and its length), as pi's snapshotEventHandlers: a handler that
 * unsubscribes one does not change the dispatch under way; 0 and nothing pushed when there is none */
static long handlers_of(lua_State *L, const char *event)
{
    long n, i;
    lua_getfield(L, LUA_REGISTRYINDEX, K_HANDLERS);
    lua_getfield(L, -1, event);
    lua_remove(L, -2);
    if (lua_type(L, -1) != LUA_TTABLE || (n = (long)lua_rawlen(L, -1)) == 0) {
        lua_pop(L, 1);
        return 0;
    }
    lua_createtable(L, (int)n, 0);
    for (i = 1; i <= n; i++) {
        lua_rawgeti(L, -2, (lua_Integer)i);
        lua_rawseti(L, -2, (lua_Integer)i);
    }
    lua_remove(L, -2);
    return n;
}

/* handler i of the list at lidx called with a copy of the event at eidx and ctx: 0 and its result pushed, or -1 and
 * nothing pushed (the error told when report is set, err holds its text); the extension stays in the list */
static int call_one(pi_lua_ext *x, int lidx, long i, int eidx, int report, char *err, long cap)
{
    lua_State *L = x->L;
    int rc;
    lua_rawgeti(L, lidx, (lua_Integer)i);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 0;
    }
    lua_pushvalue(L, eidx);
    lua_getfield(L, LUA_REGISTRYINDEX, K_CTX);
    arm(x);
    pi_lua_calls++;
    rc = lua_pcall(L, 2, 1, 0);
    if (rc != LUA_OK) {
        if (lua_type(L, -1) == LUA_TSTRING)
            cl_copy(err, lua_tostring(L, -1), cap);
        else {
            cl_copy(err, "the error value is a ", cap);
            cl_cat(err, luaL_typename(L, -1), cap);
        }
        lua_pop(L, 1);
        if (report)
            ext_error(x, err);
        return -1;
    }
    return 0;
}

/* the event's payload built into b as JSON and pushed as a table; 0, or -1 (out of Lua memory: reported) */
static int push_event(pi_lua_ext *x, const jw *b)
{
    if (!b->p || push_json(x->L, b->p, b->n)) {
        ext_error(x, "out of memory");
        return -1;
    }
    return 0;
}

/* a handler's result, when it is a table, as the JSON object ext_payload.c's decision rules read; 0 no decision (nil, a
 * number, ...), 1 parsed into o (the text in w), -1 the table cannot be encoded (reported) */
static int result_json(pi_lua_ext *x, jw *w, jv *o)
{
    char err[200], m[260];
    if (lua_type(x->L, -1) != LUA_TTABLE)
        return 0;
    jw_reset(w);
    if (enc_protected(x->L, -1, w, err, sizeof(err))) {
        cl_copy(m, "the handler's result cannot be used: ", sizeof(m));
        cl_cat(m, err, sizeof(m));
        ext_error(x, m);
        return -1;
    }
    return json_parse(w->p, w->n, o) == 0 && json_type(*o) == J_OBJ;
}

static void lx_event(void *u, const pi_event *e)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    const char *name = pi_ext_event_name(e->type);
    lua_State *L = x->L;
    long n, i;
    int top, lidx, eidx;
    jw b;
    char err[300];
    if (!L || !name)
        return;
    top = lua_gettop(L);
    n = handlers_of(L, name);
    x->event = name;
    if (!n)
        return;
    lidx = lua_gettop(L);
    jw_init(&b);
    if (pi_ext_payload_event(x->host->s, e, &b) && push_event(x, &b) == 0) {
        eidx = lua_gettop(L);
        for (i = 1; i <= (long)lua_rawlen(L, lidx); i++) {
            if (call_one(x, lidx, i, eidx, 1, err, sizeof(err)) == 0)
                lua_pop(L, 1);
        }
    }
    jw_free(&b);
    settle(x, top);
}

/* tool_call: handlers share one event whose input they may change in place (types.ts ToolCallEvent: "mutate
 * `event.input` in place"); what they changed is written back into args after each handler, so the next extension
 * and the tool see it; { block = true, reason } stops the dispatch. A handler that throws blocks the call with the
 * error's message (runner.ts emitToolCall has no catch; agent-session.ts:679 rethrows; agent-loop.ts), told to nobody
 * else, and the extension is called again by the next call. */
static int lx_tool_call(void *u, const char *id, const char *name, jw *args, char *reason, long cap)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    long n, i;
    int top, lidx, eidx, iidx, oidx, blocked = 0;
    jw b, r;
    char err[300];
    if (!L)
        return 0;
    top = lua_gettop(L);
    n = handlers_of(L, "tool_call");
    x->event = "tool_call";
    if (!n)
        return 0;
    lidx = lua_gettop(L);
    jw_init(&b);
    jw_init(&r);
    pi_ext_payload_call(&b, name, id, args);
    if (push_event(x, &b) == 0) {
        eidx = lua_gettop(L);
        lua_getfield(L, eidx, "input");
        iidx = lua_gettop(L);
        if (!lua_istable(L, iidx))
            iidx = 0;
        /* a second decode of the same arguments: what the handlers did to the first shows against it */
        if (push_json(L, args->p ? args->p : "{}", args->p ? args->n : 2) == 0) {
            oidx = lua_gettop(L);
            for (i = 1; i <= (long)lua_rawlen(L, lidx) && !blocked; i++) {
                jv o;
                int rc;
                if (call_one(x, lidx, i, eidx, 0, err, sizeof(err)) != 0) {
                    cl_copy(reason, err, cap);
                    blocked = 1;
                    break;
                }
                if (iidx && !deep_eq(L, iidx, oidx, 0)) {
                    jw_reset(&r);
                    if (enc_protected(L, iidx, &r, err, sizeof(err)) == 0) {
                        jw_reset(args);
                        jw_raw(args, r.p, r.n);
                    } else
                        ext_error(x, err);
                }
                rc = result_json(x, &r, &o);
                if (rc < 0) {
                    cl_copy(reason, "Extension failed, blocking execution: the result cannot be used", cap);
                    blocked = 1;
                } else if (rc > 0 && pi_ext_decide_call(o, args, reason, cap, &x->host->s->ext.terminate))
                    blocked = 1;
                lua_pop(L, 1);
            }
        } else
            ext_error(x, "out of memory");
    }
    jw_free(&b);
    jw_free(&r);
    settle(x, top);
    return blocked;
}

static void lx_tool_result(void *u, const char *id, const char *name, const char *args, long an, jw *content,
                           int *is_error)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    long n, i;
    int top, lidx;
    jw b, r;
    char err[300];
    if (!L)
        return;
    top = lua_gettop(L);
    n = handlers_of(L, "tool_result");
    x->event = "tool_result";
    if (!n)
        return;
    lidx = lua_gettop(L);
    jw_init(&b);
    jw_init(&r);
    for (i = 1; i <= (long)lua_rawlen(L, lidx); i++) {
        jv o;
        jw_reset(&b);
        /* each handler sees what the ones before it changed (runner.ts emitToolResult) */
        pi_ext_payload_result(&b, &x->host->s->ext, name, id, args, an, content, *is_error);
        if (push_event(x, &b))
            break;
        if (call_one(x, lidx, i, lua_gettop(L), 1, err, sizeof(err)) == 0) {
            if (result_json(x, &r, &o) > 0)
                pi_ext_decide_result(o, &x->host->s->ext, content, is_error);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);                          /* the event */
    }
    jw_free(&b);
    jw_free(&r);
    settle(x, top);
}

static int lx_input(void *u, const char *source, const char *streaming, jw *text)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    long n, i;
    int top, lidx, handled = 0;
    jw b, r;
    char err[300];
    if (!L)
        return 0;
    top = lua_gettop(L);
    n = handlers_of(L, "input");
    x->event = "input";
    if (!n)
        return 0;
    lidx = lua_gettop(L);
    jw_init(&b);
    jw_init(&r);
    for (i = 1; i <= (long)lua_rawlen(L, lidx) && !handled; i++) {
        jv o;
        jw_reset(&b);
        pi_ext_payload_input(&b, text, x->host->s->ext.images, source, streaming);
        if (push_event(x, &b))
            break;
        if (call_one(x, lidx, i, lua_gettop(L), 1, err, sizeof(err)) == 0) {
            if (result_json(x, &r, &o) > 0 && pi_ext_decide_input(o, text, x->host->s->ext.images))
                handled = 1;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    jw_free(&b);
    jw_free(&r);
    settle(x, top);
    return handled;
}

static void lx_session_start(void *u, const char *reason, const char *prev)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    long n, i;
    int top, lidx;
    jw b;
    char err[300];
    if (!L)
        return;
    top = lua_gettop(L);
    n = handlers_of(L, "session_start");
    x->event = "session_start";
    if (!n)
        return;
    lidx = lua_gettop(L);
    jw_init(&b);
    pi_ext_payload_start(&b, reason, prev);
    if (push_event(x, &b) == 0) {
        int eidx = lua_gettop(L);
        for (i = 1; i <= (long)lua_rawlen(L, lidx); i++)
            if (call_one(x, lidx, i, eidx, 1, err, sizeof(err)) == 0)
                lua_pop(L, 1);
    }
    jw_free(&b);
    settle(x, top);
}

/* ---- the other events (pi_ext.has / pi_ext.emit) ---- */

static int lx_has(void *u, const char *type)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    int has;
    if (!L)
        return 0;
    lua_getfield(L, LUA_REGISTRYINDEX, K_HANDLERS);
    lua_getfield(L, -1, type);
    has = lua_type(L, -1) == LUA_TTABLE && lua_rawlen(L, -1) > 0;
    lua_pop(L, 2);
    return has;
}

static int lx_emit(void *u, const char *type, jw *payload, int flags, pi_ext_apply fn, void *fu)
{
    pi_lua_ext *x = (pi_lua_ext *)u;
    lua_State *L = x->L;
    long n, i;
    int top, lidx, stop = 0;
    jw before, after, r;
    char err[300];
    if (!L)
        return 0;
    top = lua_gettop(L);
    n = handlers_of(L, type);
    x->event = type;
    if (!n)
        return 0;
    lidx = lua_gettop(L);
    jw_init(&before);
    jw_init(&after);
    jw_init(&r);
    for (i = 1; i <= (long)lua_rawlen(L, lidx) && !stop; i++) {
        jv ans, aft;
        int eidx, inplace = (flags & PI_EMIT_INPLACE) != 0;
        ans.p = aft.p = 0;
        ans.n = aft.n = 0;
        if (push_event(x, payload))
            break;
        eidx = lua_gettop(L);
        jw_reset(&before);
        if (inplace && enc_protected(L, eidx, &before, err, sizeof(err)))
            inplace = 0;
        if (call_one(x, lidx, i, eidx, 1, err, sizeof(err)) == 0) {
            if (result_json(x, &r, &ans) <= 0)
                ans.p = 0;
            lua_pop(L, 1);
        }
        if (inplace) {
            jw_reset(&after);
            if (enc_protected(L, eidx, &after, err, sizeof(err)) == 0 &&
                (after.n != before.n || memcmp(after.p, before.p, (size_t)after.n)) && json_parse(after.p, after.n, &aft))
                aft.p = 0;
        }
        if (fn && (ans.p || aft.p) && fn(fu, ans, aft, payload))
            stop = 1;
        lua_settop(L, lidx);
    }
    jw_free(&before);
    jw_free(&after);
    jw_free(&r);
    settle(x, top);
    return stop;
}

/* ---- loading ---- */

void pi_lua_init(pi_lua *h, pi_session *s, cl_sys *sys, const char *cwd, int trusted)
{
    memset(h, 0, sizeof(*h));
    h->s = s;
    h->sys = sys;
    h->cwd = cwd;
    h->trusted = trusted;
    h->mem_cap = PI_LUA_MEM;
    h->instr_cap = PI_LUA_INSTR;
    h->ms_cap = PI_LUA_MS;
}

/* pi's text for an extension that did not load (main.ts:802 around loader.ts:655) */
static void load_failed(pi_lua *h, const char *path, const char *why)
{
    char m[900];
    cl_copy(m, "Failed to load extension \"", sizeof(m));
    cl_cat(m, path, sizeof(m));
    cl_cat(m, "\": Failed to load extension: ", sizeof(m));
    cl_cat(m, why, sizeof(m));
    tell(h, m);
}

static const char *base_name(const char *path)
{
    const char *sl = strrchr(path, '/'), *co = strrchr(path, ':');
    return sl && (!co || sl > co) ? sl + 1 : co ? co + 1 : path;
}

/* one extension file of the given SourceInfo scope and baseDir (loader.ts createSourceInfo): 0 loaded and listed, -1 failed */
static int load_file(pi_lua *h, const char *path, const char *scope, const char *base)
{
    pi_lua_ext *x;
    char *src = 0, err[400], chunk[120];
    long n = 0;
    int rc;
    lua_State *L;
    if (h->n == PI_LUA_MAX) {
        load_failed(h, path, "too many extensions");
        return -1;
    }
    if (h->sys->read(h->sys->u, path, 262144L, &src, &n) != 0) {
        load_failed(h, path, "the file cannot be read");
        return -1;
    }
    x = &h->x[h->n];
    memset(x, 0, sizeof(*x));
    x->host = h;
    cl_copy(x->path, path, sizeof(x->path));
    x->ext.name = base_name(x->path);
    x->ext.rank = PI_EXT_LUA;
    x->scope = scope;
    cl_copy(x->base, base ? base : "", sizeof(x->base));
    x->ext.path = x->path;
    x->ext.scope = scope;
    x->ext.base_dir = x->base[0] ? x->base : 0;
    x->ext.command = lx_command;
    x->ext.u = x;
    x->ext.event = lx_event;
    x->ext.tool_call = lx_tool_call;
    x->ext.tool_result = lx_tool_result;
    x->ext.input = lx_input;
    x->ext.session_start = lx_session_start;
    x->ext.has = lx_has;
    x->ext.render = lx_render;
    x->ext.emit = lx_emit;
    L = lua_newstate(l_alloc, x);
    if (!L) {
        CL_FREE(src);
        load_failed(h, path, "not enough memory");
        return -1;
    }
    x->L = L;
    arm(x);
    lua_sethook(L, limit_hook, LUA_MASKCOUNT, 1000);
    lua_pushcfunction(L, c_setup);
    rc = lua_pcall(L, 0, 0, 0);
    if (rc == LUA_OK) {
        cl_copy(chunk, "@", sizeof(chunk));
        cl_cat(chunk, x->ext.name, sizeof(chunk));
        rc = luaL_loadbufferx(L, src, (size_t)n, chunk, "t");
        if (rc == LUA_OK) {
            /* the file is the extension's factory: `pi` is its global and its first argument (types.ts: the factory
             * receives the ExtensionAPI) */
            lua_getglobal(L, "pi");
            x->loading = 1;
            rc = lua_pcall(L, 1, 0, 0);
            x->loading = 0;
        }
    }
    CL_FREE(src);
    if (rc != LUA_OK) {
        cl_copy(err, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "the error value is not a string", sizeof(err));
        load_failed(h, path, err);
        lua_close(L);
        x->L = 0;
        free_registered(x);
        return -1;
    }
    lua_settop(L, 0);
    if (pi_ext_add(&h->s->ext, &x->ext)) {
        load_failed(h, path, "the extension list is full");
        lua_close(L);
        x->L = 0;
        free_registered(x);
        return -1;
    }
    h->n++;
    commit(x);                                  /* the tools and commands it registered while loading join the session now */
    return 0;
}

int pi_lua_load(pi_lua *h, const char *path)
{
    return load_file(h, path, "temporary", 0);
}

typedef struct names {
    char (*n)[108];
    int count, cap, oom;
} names;

static int collect(void *c, const cl_dirent *e)
{
    names *ns = (names *)c;
    if (e->name[0] == '.' || e->name[0] == 0)
        return 0;
    if (!e->dir) {
        size_t l = strlen(e->name);
        if (l < 5 || strcmp(e->name + l - 4, ".lua"))
            return 0;
    }
    if (ns->count == ns->cap) {
        char (*g)[108] = (char (*)[108])realloc(ns->n, 108 * (size_t)(ns->cap ? ns->cap * 2 : 16));
        if (!g) {
            ns->oom = 1;
            return 1;
        }
        ns->n = g;
        ns->cap = ns->cap ? ns->cap * 2 : 16;
    }
    memcpy(ns->n[ns->count++], e->name, 108);
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* loader.ts discoverExtensionsInDir: the files, and the directories with an index (resolveExtensionEntries) */
static int load_dir(pi_lua *h, const char *dir, const char *scope, const char *base)
{
    names ns;
    int i, loaded = 0;
    char p[420], q[440];
    memset(&ns, 0, sizeof(ns));
    if (h->sys->kind(h->sys->u, dir) != 2)
        return 0;
    h->sys->list(h->sys->u, dir, collect, &ns);
    if (ns.count > 1)
        qsort(ns.n, (size_t)ns.count, 108, cmp_name);
    for (i = 0; i < ns.count; i++) {
        if (path_join(dir, ns.n[i], p, sizeof(p)))
            continue;
        if (h->sys->kind(h->sys->u, p) == 2) {
            if (path_join(p, "index.lua", q, sizeof(q)) || h->sys->kind(h->sys->u, q) != 1)
                continue;
            cl_copy(p, q, sizeof(p));
        }
        if (load_file(h, p, scope, base) == 0)
            loaded++;
    }
    free(ns.n);
    return loaded;
}

int pi_lua_discover(pi_lua *h, const char *agent_dir, const char *data_dir)
{
    char d[400];
    int loaded = 0;
    char base[400];
    /* the files' SourceInfo: a project's extensions are scope "project" with baseDir <cwd>/.pi, the user's "user" with the
     * agent directory (the data directory for its own) */
    if (h->trusted && h->cwd && h->cwd[0] && !path_join(h->cwd, ".pi/extensions", d, sizeof(d)) &&
        !path_join(h->cwd, ".pi", base, sizeof(base)))
        loaded += load_dir(h, d, "project", base);
    if (agent_dir && agent_dir[0] && !path_join(agent_dir, "extensions", d, sizeof(d)))
        loaded += load_dir(h, d, "user", agent_dir);
    if (data_dir && data_dir[0] && (!agent_dir || strcmp(data_dir, agent_dir)) && !path_join(data_dir, "extensions", d, sizeof(d)))
        loaded += load_dir(h, d, "user", data_dir);
    return loaded;
}

int pi_lua_active(const pi_lua *h)
{
    int i, n = 0;
    for (i = 0; i < h->n; i++)
        n += h->x[i].L != 0;
    return n;
}

void pi_lua_free(pi_lua *h)
{
    int i;
    for (i = 0; i < h->n; i++) {
        if (h->x[i].L) {
            lua_close(h->x[i].L);
            h->x[i].L = 0;
        }
        free_registered(&h->x[i]);
    }
    h->n = 0;
}
