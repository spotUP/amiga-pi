# amiga-pi. C:Claude's core and llm/ are read IN PLACE from VTCON_DIR (never copied; D2, D9).
UPTERM_ROOT ?= $(abspath ..)
VTCON_DIR ?= $(UPTERM_ROOT)/vtcon

.PHONY: test layers lua-probe lua-probe-host
test: layers

# the layering rule lives in vtcon (llm/ is there, D9) and checks this tree too
layers:
	python3 -I $(VTCON_DIR)/tools/check_layers.py --vtcon $(VTCON_DIR) --pi $(CURDIR)

# A5: Lua 5.4 under vbcc (uses VTCON_DIR's vbcc cfg) and the host half.
lua-probe:
	tools/probe/build_lua_probe.sh

lua-probe-host:
	@mkdir -p build
	cc -std=c99 -O2 -Ivendor/lua-5.4 -o build/luaprobe_host tools/probe/luaprobe.c \
	  $(filter-out vendor/lua-5.4/lua.c vendor/lua-5.4/luac.c,$(wildcard vendor/lua-5.4/*.c)) -lm
	build/luaprobe_host tools/probe/luaprobe.lua | diff - tools/probe/luaprobe.expected && echo "luaprobe host: matches expected"
