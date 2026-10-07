# amiga-pi. C:Claude's core is read IN PLACE from VTCON_DIR (never copied).
VTCON_DIR ?= $(HOME)/Code/vtcon

.PHONY: test layers lua-probe lua-probe-host
test: layers

layers:
	python3 tools/test_check_layers.py
	python3 tools/check_layers.py --vtcon $(VTCON_DIR)

# A5: Lua 5.4 under vbcc (uses VTCON_DIR's vbcc cfg) and the host half.
lua-probe:
	tools/probe/build_lua_probe.sh

lua-probe-host:
	@mkdir -p build
	cc -std=c99 -O2 -Ivendor/lua-5.4 -o build/luaprobe_host tools/probe/luaprobe.c \
	  $(filter-out vendor/lua-5.4/lua.c vendor/lua-5.4/luac.c,$(wildcard vendor/lua-5.4/*.c)) -lm
	build/luaprobe_host tools/probe/luaprobe.lua | diff - tools/probe/luaprobe.expected && echo "luaprobe host: matches expected"
