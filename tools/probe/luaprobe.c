/* A5 probe: run a Lua script with the libraries that need no OS beyond ISO C
   (base, coroutine, table, string, math, utf8). Usage: luaprobe script.lua */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void openlib(lua_State *L, const char *name, lua_CFunction f)
{
    luaL_requiref(L, name, f, 1);
    lua_pop(L, 1);
}

int main(int argc, char **argv)
{
    lua_State *L;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: luaprobe script.lua\n");
        return 20;
    }
    L = luaL_newstate();
    if (L == NULL) {
        fprintf(stderr, "luaprobe: out of memory\n");
        return 20;
    }
    openlib(L, "_G", luaopen_base);
    openlib(L, LUA_COLIBNAME, luaopen_coroutine);
    openlib(L, LUA_TABLIBNAME, luaopen_table);
    openlib(L, LUA_STRLIBNAME, luaopen_string);
    openlib(L, LUA_MATHLIBNAME, luaopen_math);
    openlib(L, LUA_UTF8LIBNAME, luaopen_utf8);
    rc = luaL_dofile(L, argv[1]);
    if (rc != 0) {
        fprintf(stderr, "luaprobe: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 10;
    }
    lua_close(L);
    return 0;
}
