# amiga-pi progress ledger (phase A)

Plan: ~/Code/vtcon/thoughts/shared/plans/2026-10-07-amiga-pi.md (not edited; owner syncs). Running count: phase A 6 of 6 items written, 2 manual halves open (A5 Amiga run, owner read of the documents). Whole plan: 6 of ~150 items.

## Phase A

- [x] A1 ARCHITECTURE.md, every section the plan lists, plus decision table D1-D9. Commit: see git log (`A1`).
- [x] A2 COMPATIBILITY.md, 124 rows in 14 groups, 0 cells empty (checked by the generator's assert). Rows name a phase letter where the plan has no item id yet.
- [x] A3 thoughts/shared/research/2026-10-07_pi-sources-addendum.md at pi f10993bc. Confirmed: openai-completions wire details, id normalisation, compaction/branch prompts, auth.json, theme/keybinding formats. NOT confirmable from pi source (SDK-defined, marked [unverified] in the addendum): Google REST path and key header, Bedrock ConverseStream framing/URL, Azure auth header. Theme colour-role list not fetched.
- [x] A4 tools/check_layers.py + tools/test_check_layers.py, wired into `make test` (this repo). Exit 0 clean, 1 on planted include (tested). The vtcon rule "claude/http.c has no provider name" is PENDING(B5): 5 hits today, reported but not failing without --strict.
- [~] A5 Lua probe. Done: vendored Lua 5.4.9, luaconf.h-only changes, builds clean with vbcc under -warnings-as-errors, host output recorded (tools/probe/luaprobe.expected, `make lua-probe-host` matches). NOT RUN: Amiga run (criteria 2 and 4). Decision: native modules, because criterion 1 fails (10 -dontwarn numbers needed, limit 5). Criterion 3 holds (delta 182,680 bytes).
- [x] A6 this file.

## Decisions (also in ARCHITECTURE.md)
Local repo; share vtcon core in place via VTCON_DIR; tool `bash` = vsh -c; native extension modules; llm/ in this repo (D9, needs owner call for B5).

## Gotchas
- vbcc stops after 10 errors per file under -warnings-as-errors: count warning numbers WITHOUT it first.
- vbcc warning 120 cannot be suppressed (error 160): avoid `extern` on definitions (LUA_API/LUAI_FUNC empty, LUAI_DDEC keeps extern).
- `-lmsoft`/`-lm881` leave _MathIeee*Base undefined; `-lmieee` links.
- vtcon's generated build/vbcc-aos68k.cfg has a relative NDK path; the probe script regenerates the cfg from tools/vbcc-aos68k.cfg.in with an absolute one.
- Shell is zsh: unquoted $VAR does not word-split; scripts use bash.

## Next
Rig owner: run build/amiga/luaprobe on tools/probe/luaprobe.lua (compare with luaprobe.expected, time fib+loop) via `make lua-probe`. Then phase B (needs the D9 decision).
