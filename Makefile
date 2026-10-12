# amiga-pi. C:Claude's core and llm/ are read IN PLACE from VTCON_DIR (never copied; D2, D9).
UPTERM_ROOT ?= $(abspath ..)
VTCON_DIR ?= $(UPTERM_ROOT)/vtcon

# vtcon's source lists (sources.mk), every path below VTCON_DIR
CL_DIR := $(VTCON_DIR)/
include $(VTCON_DIR)/sources.mk
# a variable of vtcon's Makefile (vbcc's command line, HOSTCFLAGS, ...), not a copy
vtvar = $(shell $(MAKE) -s --no-print-directory -C $(VTCON_DIR) -f Makefile -f $(CURDIR)/tools/vtcon_vars.mk print-$(1))

HOSTCC ?= cc
HOSTCFLAGS = $(call vtvar,HOSTCFLAGS) -I$(VTCON_DIR) -I$(VTCON_DIR)/tests

# amiga-pi itself (plan phase C): the agent loop, its events and extensions, the
# system prompt, the command line, the prompt on C:Claude's screen code
PI_CORE := amiga-pi/events.c amiga-pi/ext.c amiga-pi/agent.c amiga-pi/prompt.c amiga-pi/args.c amiga-pi/app.c \
           amiga-pi/ui_pi.c amiga-pi/truncate.c amiga-pi/tools_pi.c amiga-pi/ext_gate.c amiga-pi/image.c amiga-pi/session3.c amiga-pi/context.c amiga-pi/store.c amiga-pi/compact.c amiga-pi/cp_prompts.c amiga-pi/cmds.c amiga-pi/models.c \
           amiga-pi/resources.c amiga-pi/sha256.c amiga-pi/mcp.c amiga-pi/mcp_http.c amiga-pi/mcp_stdio.c amiga-pi/show_pi.c amiga-pi/tree_pi.c amiga-pi/export.c amiga-pi/okcolor.c amiga-pi/mode_json.c amiga-pi/mode_rpc.c amiga-pi/piapi.c
# stb_image / stb_image_write (vendor/stb) for read's images: built on their own, warnings off
STB := amiga-pi/stb_impl.c vendor/stb/stb_image.h vendor/stb/stb_image_write.h
PI_HDR := $(wildcard amiga-pi/*.h)
# what the host binaries need of vtcon besides C:Claude's core: the view (show.c's
# Markdown), the line discipline's helpers, the clipboard text; the tests also the
# engine (C:Claude's screen tests run on it) and the test helpers
VT_HOST := $(VIEW_CORE) $(call cl_src,tty/bmsg.c handler/clipfmt.c)
VT_TEST := $(call cl_src,engine/vtengine.c tests/harness.c tests/claude_load.c tests/claude_screen.c tests/pi_stub.c)
PI_TESTS := tests/main.c tests/test_pi_agent.c tests/test_pi_tools.c tests/test_pi_session.c tests/test_pi_store.c tests/test_pi_model.c tests/test_pi_model_reach.c tests/test_pi_mcp.c tests/test_pi_resources.c tests/test_pi_tui.c tests/test_pi_modes.c tests/pi/sdk_example.c

.PHONY: test layers lua-probe lua-probe-host amiga
# ONLY=pi_agent the agent suite alone, ONLY=pi_tools the tools and the gate, ONLY=pi_session the session file, ONLY=pi_store the store through pi_app, ONLY=pi_model the registry, models.json, handoff and the model switch, ONLY=pi_mcp MCP (python3 for tools/fake_mcp.py), ONLY=pi_resources context files, SYSTEM.md, trust, ONLY=pi_tui the screen against pi's captures (PI_TUI_ONLY=<scenario>, PI_TUI_DUMP=1 shows both screens), ONLY=pi_modes the JSON and RPC modes against pi's records (tests/pi/rpc, PI_MODES_ONLY=<scenario>); ONLY=layers the layering check alone
test: build/pi_test
	@if [ "$(ONLY)" != layers ]; then cd $(VTCON_DIR) && $(CURDIR)/build/pi_test $(ONLY); fi
	@if [ -z "$(ONLY)" ] || [ "$(ONLY)" = layers ]; then $(MAKE) -s layers; fi

# the suites run in VTCON_DIR (its recordings: tests/claude, tests/pi); amiga-pi's
# own goldens through PI_TESTS
build/pi_test: $(PI_CORE) $(PI_HDR) $(PI_TESTS) $(CLAUDE_CORE) $(CLAUDE_PI) $(CLAUDE_HDR) $(VT_HOST) $(VT_TEST) tests/pi_test.h build/stb_impl.o
	@mkdir -p build
	$(HOSTCC) $(HOSTCFLAGS) -DVT_COUNT_ALLOC -DPI_TESTS='"$(CURDIR)/tests"' -o $@ $(PI_TESTS) $(PI_CORE) \
	  $(CLAUDE_CORE) $(CLAUDE_PI) $(VT_HOST) $(VT_TEST) build/stb_impl.o

build/stb_impl.o: $(STB)
	@mkdir -p build
	$(HOSTCC) -std=c99 -O1 -g -fsanitize=address,undefined -w -Ivendor/stb -Iamiga-pi -c -o $@ amiga-pi/stb_impl.c

# amiga-pi on the host (manual runs against tools/claude_fixture.py), plain http only
build/amiga-pi-host: $(PI_CORE) $(PI_HDR) amiga-pi/main_posix.c $(CLAUDE_CORE) $(CLAUDE_PI) $(CLAUDE_HDR) $(VT_HOST) build/stb_impl.o
	@mkdir -p build
	$(HOSTCC) $(HOSTCFLAGS) -o $@ amiga-pi/main_posix.c $(PI_CORE) $(CLAUDE_CORE) $(CLAUDE_PI) $(VT_HOST) build/stb_impl.o

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
build/amiga/amiga-pi: $(PI_CORE) $(PI_HDR) amiga-pi/main_amiga.c $(CLAUDE_PORTABLE) $(CLAUDE_PI) $(CLAUDE_HDR) $(LLM_CORE) $(STB)
	@mkdir -p build/amiga/obj
	$(MAKE) -s -C $(VTCON_DIR) build/amiga/obj/cpuchk-amiga-pi.o
	cd $(VTCON_DIR) && $(filter-out -warnings-as-errors -warn=-1,$(call vtvar,VC)) -dontwarn=-1 -I$(CURDIR)/vendor/stb \
	  -c -o $(CURDIR)/build/amiga/obj/stb_impl.o $(CURDIR)/amiga-pi/stb_impl.c
	cd $(VTCON_DIR) && $(call vtvar,VC) -dontwarn=153,65 -I$(call vtvar,VTCON_NETINCLUDE) \
	  -c -o $(CURDIR)/build/amiga/obj/net_amiga.o claude/net_amiga.c
	cd $(VTCON_DIR) && $(call vtvar,VC) -dontwarn=153,65 $(call vtvar,CLAUDE_TLS_INC) \
	  -c -o $(CURDIR)/build/amiga/obj/tls.o $(call vtvar,CLAUDE_TLS)
	cd $(VTCON_DIR) && $(call vtvar,VC) -Dmain=up_main -dontwarn=153,65 -I$(VTCON_DIR) -o $(CURDIR)/$@ \
	  $(PI_AMIGA_SRC) $(CURDIR)/build/amiga/obj/net_amiga.o $(CURDIR)/build/amiga/obj/tls.o $(CURDIR)/build/amiga/obj/stb_impl.o \
	  build/amiga/obj/cpuchk-amiga-pi.o -lmieee

# K5: a stdio MCP server that runs on the Amiga (rig step 10), on vtcon's json.c
build/amiga/mcp-echo: tools/probe/mcp_echo.c
	@mkdir -p build/amiga
	cd $(VTCON_DIR) && $(call vtvar,VC) -I$(VTCON_DIR) -o $(CURDIR)/$@ $(CURDIR)/tools/probe/mcp_echo.c claude/json.c claude/util.c
