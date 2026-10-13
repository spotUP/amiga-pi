/* see ext_lua.h */
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "ext_lua.h"
#include "ext_payload.h"
#include "claude/util.h"
#include "claude/path.h"

long pi_lua_calls;

#define K_HANDLERS "pi.handlers"        /* registry: event name -> array of handler functions */
#define K_CTX "pi.ctx"                  /* registry: the ctx table every handler gets */
#define K_ARRAY "pi.array"              /* registry: the metatable that marks a table as a JSON array */

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
    /* Lua's integers are 32 bits: what fits is an integer, the rest a float (24 bits of it) */
    if (is_int && v.n < 64 && (digits < 10 || (digits == 10 && strcmp(t + neg, "2147483647") <= 0))) {
        long n = 0;
        for (i = neg; i < k; i++)
            n = n * 10 + (t[i] - '0');
        lua_pushinteger(L, (lua_Integer)(neg ? -n : n));
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
        if (lua_isinteger(L, idx))
            jw_long(w, (long)lua_tointeger(L, idx));
        else {
            lua_Number d = lua_tonumber(L, idx);
            if (d != d || d > 1e30 || d < -1e30)
                jw_rawz(w, "null");             /* JSON.stringify: NaN and Infinity are null */
            else if (d == (lua_Number)(long)d && d > -2147483647.0 && d < 2147483647.0)
                jw_long(w, (long)d);
            else {
                /* Lua's own text of a float ("%.7g", an exponent form for the big ones: valid JSON numbers) */
                size_t n;
                const char *t;
                lua_pushvalue(L, idx);
                t = lua_tolstring(L, -1, &n);
                jw_raw(w, t, (long)n);
                lua_pop(L, 1);
            }
        }
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
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "hasUI");
    lua_pushcfunction(L, ctx_trusted);
    lua_setfield(L, -2, "isProjectTrusted");
    lua_setfield(L, LUA_REGISTRYINDEX, K_CTX);
    /* pi */
    lua_newtable(L);
    lua_pushcfunction(L, pi_on);
    lua_setfield(L, -2, "on");
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

/* pi's text for an extension that failed: interactive-mode.ts showExtensionError, and the extension stops */
static void ext_error(pi_lua_ext *x, const char *msg)
{
    char m[900];
    cl_copy(m, "Extension \"", sizeof(m));
    cl_cat(m, x->path, sizeof(m));
    cl_cat(m, "\" error: ", sizeof(m));
    cl_cat(m, msg, sizeof(m) - 40);
    cl_cat(m, " (the extension is disabled)", sizeof(m));
    x->disabled = 1;
    tell(x->host, m);
}

/* after a dispatch, back on the stack's base: a disabled extension's state is closed */
static void settle(pi_lua_ext *x, int top)
{
    if (x->L && !x->disabled) {
        lua_settop(x->L, top);
        return;
    }
    if (x->L) {
        lua_close(x->L);
        x->L = 0;
        x->mem = 0;
    }
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
 * the extension is disabled (the error reported; err holds its text) */
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
    if (!n)
        return;
    lidx = lua_gettop(L);
    jw_init(&b);
    if (pi_ext_payload_event(x->host->s, e, &b) && push_event(x, &b) == 0) {
        eidx = lua_gettop(L);
        for (i = 1; i <= (long)lua_rawlen(L, lidx) && !x->disabled; i++) {
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
 * error's message (agent-session.ts:679 -> agent-loop.ts) and the extension is disabled. */
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
            for (i = 1; i <= (long)lua_rawlen(L, lidx) && !x->disabled && !blocked; i++) {
                jv o;
                int rc;
                if (call_one(x, lidx, i, eidx, 1, err, sizeof(err)) != 0) {
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
                } else if (rc > 0 && pi_ext_decide_call(o, args, reason, cap))
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
    if (!n)
        return;
    lidx = lua_gettop(L);
    jw_init(&b);
    jw_init(&r);
    for (i = 1; i <= (long)lua_rawlen(L, lidx) && !x->disabled; i++) {
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
        if (!x->disabled)
            lua_pop(L, 1);                      /* the event */
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
    if (!n)
        return 0;
    lidx = lua_gettop(L);
    jw_init(&b);
    jw_init(&r);
    for (i = 1; i <= (long)lua_rawlen(L, lidx) && !x->disabled && !handled; i++) {
        jv o;
        jw_reset(&b);
        pi_ext_payload_input(&b, text, source, streaming);
        if (push_event(x, &b))
            break;
        if (call_one(x, lidx, i, lua_gettop(L), 1, err, sizeof(err)) == 0) {
            if (result_json(x, &r, &o) > 0 && pi_ext_decide_input(o, text))
                handled = 1;
            lua_pop(L, 1);
        }
        if (!x->disabled)
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
    if (!n)
        return;
    lidx = lua_gettop(L);
    jw_init(&b);
    pi_ext_payload_start(&b, reason, prev);
    if (push_event(x, &b) == 0) {
        int eidx = lua_gettop(L);
        for (i = 1; i <= (long)lua_rawlen(L, lidx) && !x->disabled; i++)
            if (call_one(x, lidx, i, eidx, 1, err, sizeof(err)) == 0)
                lua_pop(L, 1);
    }
    jw_free(&b);
    settle(x, top);
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

int pi_lua_load(pi_lua *h, const char *path)
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
    x->ext.u = x;
    x->ext.event = lx_event;
    x->ext.tool_call = lx_tool_call;
    x->ext.tool_result = lx_tool_result;
    x->ext.input = lx_input;
    x->ext.session_start = lx_session_start;
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
            rc = lua_pcall(L, 1, 0, 0);
        }
    }
    CL_FREE(src);
    if (rc != LUA_OK) {
        cl_copy(err, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "the error value is not a string", sizeof(err));
        load_failed(h, path, err);
        lua_close(L);
        x->L = 0;
        return -1;
    }
    lua_settop(L, 0);
    if (pi_ext_add(&h->s->ext, &x->ext)) {
        load_failed(h, path, "the extension list is full");
        lua_close(L);
        x->L = 0;
        return -1;
    }
    h->n++;
    return 0;
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
static int load_dir(pi_lua *h, const char *dir)
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
        if (pi_lua_load(h, p) == 0)
            loaded++;
    }
    free(ns.n);
    return loaded;
}

int pi_lua_discover(pi_lua *h, const char *agent_dir, const char *data_dir)
{
    char d[400];
    int loaded = 0;
    if (h->trusted && h->cwd && h->cwd[0] && !path_join(h->cwd, ".pi/extensions", d, sizeof(d)))
        loaded += load_dir(h, d);
    if (agent_dir && agent_dir[0] && !path_join(agent_dir, "extensions", d, sizeof(d)))
        loaded += load_dir(h, d);
    if (data_dir && data_dir[0] && (!agent_dir || strcmp(data_dir, agent_dir)) && !path_join(data_dir, "extensions", d, sizeof(d)))
        loaded += load_dir(h, d);
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
    for (i = 0; i < h->n; i++)
        if (h->x[i].L) {
            lua_close(h->x[i].L);
            h->x[i].L = 0;
        }
    h->n = 0;
}
