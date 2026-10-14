---
date: 2026-10-14
topic: U3 part 2 -- /scoped-models, /settings (theme, show images inside it), config selector, with the features behind them
tags: [pi-1.2.0, selectors, settings, P12-B11, P12-B12, P12-B13, P12-B16, P12-B17]
status: implemented
---

# Ledger (owner 2026-10-14: build all four, each with the feature behind it, as pi 1.2.0 does)

Branch feature/u3-selectors in amiga-pi/build/wt-u3 and vtcon/build/wt-u3. Goldens from pi 1.2.0 (tools/pi_oracle/tui_gen.py, tests/pi/tui).
Gates: amiga-pi `make test` + `make amiga`; vtcon `make test ONLY=claude_tui` + `make amiga`.

Decisions
- ShowImagesSelectorComponent and ThemeSelectorComponent are exported by pi 1.2.0 (components/index.js) but nothing opens them: interactive-mode.js
  imports only SettingsSelectorComponent and ScopedModelsSelectorComponent. The live equivalents are the "Show images" and "Image width" rows of
  /settings (listed only where the terminal draws images) and the ThemeSubmenu of its "Theme" row; those are what is built and compared.
- One settings writer: authstore.h pi_store_set_path / pi_store_set_sub. One table of the rows: settings_pi.c. One scope module: scoped.c.
- Every table row has `moves` (what it changes) or `exempt` (why it cannot here): tests/test_pi_settings.c fails a row with neither.

Items
- [x] U3-1 pi_store_set_path / pi_store_set_sub (nested settings.json members, pi's SettingsManager layout)
- [x] U3-2 scoped.c + /scoped-models selector (8 goldens scoped_*), line mode numbered list
- [x] U3-3 settings_pi.c table + /settings main list (14 goldens settings_*)
- [x] U3-4 theme menus (B13), warnings, per-model thinking levels (goldens settings_theme*, settings_warnings*, settings_modelthinking*)
- [x] U3-5 images rows (B16) with PI_IMAGE_PROTOCOL (goldens settings_images*)
- [x] U3-6 behaviours: one per row of the table, each moves the output (see the table's `moves`)
- [x] U3-7 line mode /settings numbered list
- [x] U3-8 config selector (B17) + `config` command
- [x] U3-9 COMPATIBILITY rows, plan P12 ticks, CHANGELOG
- [x] U3-10 gates, commits, worktrees removed

Evidence
- vtcon 2b1944d (tui.c/tui.h: pi_edpad, pi_menu_max, pi_dbl_esc, pi_hwcursor; test_claude_tui). amiga-pi 9ff67c1 (oracle + 43 scenarios), 9c0b09a (code + tests).
- Gates (worktrees build/wt-u3, UPTERM_ROOT=/Users/spot/Code/UP-Term): amiga-pi `make test` all 17 suites + layers ok, `make amiga` ok; vtcon `make test ONLY=claude_tui` (672), `claude_repl` (1806), `layers` ok, `make amiga` ok.
- Revert proofs (mutated, run, restored): per-model thinking level, block images, double escape, autocompact footer, tree filter, scope apply, config toggle each fail their check (test_pi_tui.c 1943, 2106, 2114, 2117, 2124, 2158, 2227, 2241; test_pi_model_reach.c 718, 719, 805).
- Hide thinking is exempt because amiga-pi draws no thinking blocks (found while building the table).
- /reload did not refresh the slash menu's array after the skill rows were made again (a stale pointer); reload_resources now does.
