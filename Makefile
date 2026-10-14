# amiga-pi. C:Claude's core and llm/ are read IN PLACE from VTCON_DIR (never copied; D2, D9).
UPTERM_ROOT ?= $(abspath $(CURDIR)/..)
VTCON_DIR ?= $(UPTERM_ROOT)/vtcon

# vtcon's source lists (sources.mk), every path below VTCON_DIR
CL_DIR := $(VTCON_DIR)/
include $(VTCON_DIR)/sources.mk
# a variable of vtcon's Makefile (vbcc's command line, HOSTCFLAGS, ...), not a copy
vtvar = $(shell $(MAKE) -s --no-print-directory -C $(VTCON_DIR) -f Makefile -f $(CURDIR)/tools/vtcon_vars.mk print-$(1))

HOSTCC ?= cc
HOSTCFLAGS = $(call vtvar,HOSTCFLAGS) -I$(VTCON_DIR) -I$(VTCON_DIR)/tests -I$(CURDIR)/vendor/lua-5.4

# Lua 5.4 (L3, decision D4): what an extension gets (ext_lua.h) -- the core and the base, coroutine, table, string, math
# and utf8 libraries; not io, os, package, debug (linit.c, the standalone lua.c and luac.c are left out too). Built on
# its own with the warnings off, as stb; luaconf.h is the one changed file (A5).
LUA_DIR := vendor/lua-5.4
LUA_SRC := $(filter-out $(addprefix $(LUA_DIR)/,lua.c luac.c liolib.c loslib.c loadlib.c ldblib.c linit.c),$(wildcard $(LUA_DIR)/*.c))
LUA_HOST_OBJ := $(patsubst $(LUA_DIR)/%.c,build/lua/%.o,$(LUA_SRC))
LUA_AMIGA_OBJ := $(patsubst $(LUA_DIR)/%.c,build/amiga/obj/lua_%.o,$(LUA_SRC))

# amiga-pi itself (plan phase C): the agent loop, its events and extensions, the
# system prompt, the command line, the prompt on C:Claude's screen code
PI_CORE := amiga-pi/events.c amiga-pi/ext.c amiga-pi/agent.c amiga-pi/prompt.c amiga-pi/args.c amiga-pi/app.c \
           amiga-pi/ui_pi.c amiga-pi/truncate.c amiga-pi/tools_pi.c amiga-pi/ext_gate.c amiga-pi/ext_hooks.c amiga-pi/ext_payload.c amiga-pi/ext_events.c amiga-pi/ext_lua.c amiga-pi/image.c amiga-pi/session3.c amiga-pi/context.c amiga-pi/store.c amiga-pi/compact.c amiga-pi/cp_prompts.c amiga-pi/cmds.c amiga-pi/models.c \
           amiga-pi/resources.c amiga-pi/skills.c amiga-pi/yaml.c amiga-pi/yaml_lex.c amiga-pi/yaml_cst.c amiga-pi/frontmatter.c amiga-pi/pkgres.c amiga-pi/resreport.c amiga-pi/mcp.c amiga-pi/mcp_http.c amiga-pi/mcp_stdio.c amiga-pi/show_pi.c amiga-pi/tree_pi.c amiga-pi/resume_pi.c amiga-pi/export.c amiga-pi/okcolor.c amiga-pi/mode_json.c amiga-pi/mode_rpc.c amiga-pi/piapi.c amiga-pi/ansi.c amiga-pi/fuzzy.c amiga-pi/pstatus.c amiga-pi/templates.c amiga-pi/oauth.c amiga-pi/login.c amiga-pi/radius.c amiga-pi/authstore.c amiga-pi/settings_diag.c amiga-pi/jsdiff.c
PI_CORE += amiga-pi/changelog.c amiga-pi/scoped.c amiga-pi/settings_pi.c amiga-pi/config_pi.c
# stb_image / stb_image_write (vendor/stb) for read's images: built on their own, warnings off
STB := amiga-pi/stb_impl.c vendor/stb/stb_image.h vendor/stb/stb_image_write.h
PI_HDR := $(wildcard amiga-pi/*.h)
# what the host binaries need of vtcon besides C:Claude's core: the view (show.c's
# Markdown), the line discipline's helpers, the clipboard text; the tests also the
# engine (C:Claude's screen tests run on it) and the test helpers
VT_HOST := $(VIEW_CORE) $(call cl_src,tty/bmsg.c handler/clipfmt.c)
VT_TEST := $(call cl_src,engine/vtengine.c tests/harness.c tests/claude_load.c tests/claude_screen.c tests/pi_stub.c)
PI_TESTS := tests/main.c tests/test_pi_agent.c tests/test_pi_tools.c tests/test_pi_session.c tests/test_pi_store.c tests/test_pi_model.c tests/test_pi_model_reach.c tests/test_pi_mcp.c tests/test_pi_resources.c tests/test_pi_tui.c tests/test_pi_modes.c tests/test_pi_templates.c tests/test_pi_hooks.c tests/test_pi_lua.c tests/test_pi_yaml.c tests/test_pi_diff.c tests/test_pi_login.c tests/test_pi_radius.c tests/test_pi_settings.c tests/pi/sdk_example.c

# a recipe that fails must not leave its target behind (M2; vtcon tests/test_make_delete_on_error.py proves the make behaviour)
.DELETE_ON_ERROR:

.PHONY: test layers lua-probe lua-probe-host amiga oracle-session
# ONLY=pi_agent the agent suite alone, ONLY=pi_tools the tools and the gate, ONLY=pi_session the session file, ONLY=pi_store the store through pi_app, ONLY=pi_model the registry, models.json, handoff and the model switch, ONLY=pi_mcp MCP (python3 for tools/fake_mcp.py), ONLY=pi_resources context files, SYSTEM.md, trust, ONLY=pi_tui the screen against pi's captures (PI_TUI_ONLY=<scenario>, PI_TUI_DUMP=1 shows both screens), ONLY=pi_templates prompt templates (substitution table from pi 1.1.0, discovery, /name through pi_app), ONLY=pi_login the subscription logins (the flows against tools/claude_fixture.py --oauth and pi's own run of them, tests/pi/oauth; /login and the credential of a request through pi_app; PI_LOGIN_ONLY=<scenario> runs one), ONLY=pi_hooks hook commands on pi's extension events (decisions and payloads against pi's goldens, tests/pi/rpc/ext_*), ONLY=pi_radius pi-messages and the Radius catalog (pi's own run of the API, tests/pi/radius; the gateway's answers on the stub transport), ONLY=pi_modes the JSON and RPC modes against pi's records (tests/pi/rpc, PI_MODES_ONLY=<scenario>); ONLY=layers the layering check alone
test: build/pi_test
	@if [ "$(ONLY)" != layers ]; then cd $(VTCON_DIR) && $(CURDIR)/build/pi_test $(ONLY); fi
	@if [ -z "$(ONLY)" ] || [ "$(ONLY)" = layers ]; then $(MAKE) -s layers; fi
	@if [ -z "$(ONLY)" ]; then grep -q '^\.DELETE_ON_ERROR:' Makefile || { echo "[ERROR] Makefile lacks .DELETE_ON_ERROR: (M2)"; exit 1; }; fi

# the suites run in VTCON_DIR (its recordings: tests/claude, tests/pi); amiga-pi's
# own goldens through PI_TESTS
build/pi_test: $(PI_CORE) $(PI_HDR) amiga-pi/host_posix.c amiga-pi/host_posix.h $(PI_TESTS) $(CLAUDE_CORE) $(CLAUDE_PI) $(CLAUDE_HDR) $(VT_HOST) $(VT_TEST) tests/pi_test.h build/stb_impl.o $(LUA_HOST_OBJ)
	@mkdir -p build
	$(HOSTCC) $(HOSTCFLAGS) -DVT_COUNT_ALLOC -DPI_TESTS='"$(CURDIR)/tests"' -o $@ $(PI_TESTS) amiga-pi/host_posix.c $(PI_CORE) \
	  $(CLAUDE_CORE) $(CLAUDE_PI) $(VT_HOST) $(VT_TEST) build/stb_impl.o $(LUA_HOST_OBJ) -lm

build/lua/%.o: $(LUA_DIR)/%.c $(wildcard $(LUA_DIR)/*.h)
	@mkdir -p build/lua
	$(HOSTCC) -std=c99 -O1 -g -fsanitize=address -w -I$(LUA_DIR) -c -o $@ $<

build/stb_impl.o: $(STB)
	@mkdir -p build
	$(HOSTCC) -std=c99 -O1 -g -fsanitize=address,undefined -w -Ivendor/stb -Iamiga-pi -c -o $@ amiga-pi/stb_impl.c

# amiga-pi on the host (manual runs against tools/claude_fixture.py), plain http only
build/amiga-pi-host: $(PI_CORE) $(PI_HDR) amiga-pi/main_posix.c amiga-pi/host_posix.c $(CLAUDE_CORE) $(CLAUDE_PI) $(CLAUDE_HDR) $(VT_HOST) build/stb_impl.o $(LUA_HOST_OBJ)
	@mkdir -p build
	$(HOSTCC) $(HOSTCFLAGS) -o $@ amiga-pi/main_posix.c amiga-pi/host_posix.c $(PI_CORE) $(CLAUDE_CORE) $(CLAUDE_PI) $(VT_HOST) build/stb_impl.o $(LUA_HOST_OBJ) -lm

# the tests/pi/sessions expected files, from the pinned pi 1.2.0 install (vendor/pi-1.2.0); gen.ts refuses any other version
oracle-session:
	node tools/pi_oracle/gen.ts

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

# amiga-pi for AmigaOS (vbcc, 68020), C:Claude's Amiga build with amiga-pi's
# main: vbcc's command line, the netinclude and the TLS layer (AmiSSL when its
# SDK is in vtcon's vendor/, else none: https refused) are vtcon's
# (tools/vtcon_vars.mk); compiled in VTCON_DIR, where vbcc's config is.
PI_AMIGA_SRC := $(abspath $(PI_CORE) amiga-pi/main_amiga.c) $(filter-out $(CL_DIR)claude/main_amiga.c,$(CLAUDE_PORTABLE)) \
                $(filter-out $(LLM_CLAUDE),$(LLM_CORE)) $(CLAUDE_PI)
amiga: build/amiga/amiga-pi
# Lua's own files, one object each, with the warning numbers A5 measured (ARCHITECTURE.md, Extensions: 65, 79, 153, 163,
# 166, 167, 170, 172, 306, 307; vtcon's VC already carries 163, 166, 167, 170, 306, 307), still -warnings-as-errors
build/amiga/obj/lua_%.o: $(LUA_DIR)/%.c $(wildcard $(LUA_DIR)/*.h)
	@mkdir -p build/amiga/obj
	cd $(VTCON_DIR) && $(call vtvar,VC) -dontwarn=65,79,153,172 -I$(CURDIR)/$(LUA_DIR) -c -o $(CURDIR)/$@ $(CURDIR)/$<

# net_amiga.c and the TLS layer are compiled in the recipe: listed here, an edit of either relinks
build/amiga/amiga-pi: $(PI_CORE) $(PI_HDR) amiga-pi/main_amiga.c $(CLAUDE_PORTABLE) $(CLAUDE_PI) $(CLAUDE_HDR) $(LLM_CORE) $(STB) $(LUA_AMIGA_OBJ) \
  $(VTCON_DIR)/claude/net_amiga.c $(wildcard $(VTCON_DIR)/claude/tls_*.c)
	@mkdir -p build/amiga/obj
	$(MAKE) -s -C $(VTCON_DIR) build/amiga/obj/cpuchk-amiga-pi.o
	cd $(VTCON_DIR) && $(filter-out -warnings-as-errors -warn=-1,$(call vtvar,VC)) -dontwarn=-1 -I$(CURDIR)/vendor/stb \
	  -c -o $(CURDIR)/build/amiga/obj/stb_impl.o $(CURDIR)/amiga-pi/stb_impl.c
	cd $(VTCON_DIR) && $(call vtvar,VC) -dontwarn=153,65 -I$(call vtvar,VTCON_NETINCLUDE) \
	  -c -o $(CURDIR)/build/amiga/obj/net_amiga.o claude/net_amiga.c
	cd $(VTCON_DIR) && $(call vtvar,VC) -dontwarn=153,65 $(call vtvar,CLAUDE_TLS_INC) \
	  -c -o $(CURDIR)/build/amiga/obj/tls.o $(call vtvar,CLAUDE_TLS)
	cd $(VTCON_DIR) && $(call vtvar,VC) -Dmain=up_main -dontwarn=153,65 -I$(VTCON_DIR) -I$(CURDIR)/$(LUA_DIR) -o $(CURDIR)/$@ \
	  $(PI_AMIGA_SRC) $(CURDIR)/build/amiga/obj/net_amiga.o $(CURDIR)/build/amiga/obj/tls.o $(CURDIR)/build/amiga/obj/stb_impl.o \
	  $(addprefix $(CURDIR)/,$(LUA_AMIGA_OBJ)) build/amiga/obj/cpuchk-amiga-pi.o -lmieee

# K5: a stdio MCP server that runs on the Amiga (rig step 10), on vtcon's json.c
build/amiga/mcp-echo: tools/probe/mcp_echo.c
	@mkdir -p build/amiga
	cd $(VTCON_DIR) && $(call vtvar,VC) -I$(VTCON_DIR) -o $(CURDIR)/$@ $(CURDIR)/tools/probe/mcp_echo.c claude/json.c claude/util.c
