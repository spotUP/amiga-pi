# Changelog

amiga-pi follows pi's release numbers: a section here says what amiga-pi has of that pi release. What pi's own changelog lists for a
release but amiga-pi does not have (codemode, OpenRouter per-key lists unless a line below says otherwise) is not listed.
COMPATIBILITY.md has the full table.

## [1.2.0] - 2026-10-14

### Added

- Added the **What's New** notice after an update (the first interactive start with a newer version shows the new entries of this file once; `collapseChangelog` shows one line instead) and `/changelog`
- Added `lastChangelogVersion` and `collapseChangelog` to `settings.json`
- Added `/scoped-models`: pi's selector for the models Ctrl+P and Shift+Ctrl+P cycle (Enter toggles, Ctrl+A all, Ctrl+X clear, Ctrl+P a provider, Option+Up and Option+Down reorder, Ctrl+S saves `enabledModels`); a default model chosen with Ctrl+S in `/model` joins a non-empty scope
- Added `/settings`: pi's settings selector with its theme menus (including the automatic light and dark pair), the warnings menu and the default thinking level per model; Show images and Image width are listed only where `PI_IMAGE_PROTOCOL` says the terminal draws images
- Added `amiga-pi config`: pi's resource configuration screen for the user's skills and prompt templates (`-l` and the project mode are not built)
- Added the settings `images.autoResize`, `images.blockImages`, `httpIdleTimeoutMs`, `editorPaddingX`, `autocompleteMaxVisible`, `doubleEscapeAction`, `treeFilterMode`, `showHardwareCursor`, `modelThinkingLevels` and `terminal.*` to the ones amiga-pi reads and writes; the ones amiga-pi cannot act on (`transport`, `cacheWarming`, `markdown.mermaid`, `hideThinkingBlock`, ...) are kept as they are
- Added the cut-off warning of print mode: `-p` on an answer that stopped at the output token limit prints the text, then `Response was truncated: the model reached its output token limit.` on stderr and exits 1

### Changed

- Changed `keybindings.json` to skip `$schema` without a warning and to drop a value that is not a key id (for example `ctrl+ctrl+x`), as pi 1.2.0
- Changed theme files to be checked as pi 1.2.0 checks them: keys the schema does not name in the root, `colors` and `export` objects, and an `appearance` other than `dark` or `light`, are refused
- Changed `~` to be expanded in the `!pattern`, `+path` and `-path` entries of the `extensions`, `skills`, `prompts` and `themes` settings
- Changed `settings.json` and `models.json` to accept `$schema`

## [1.1.0] - 2026-10-13

### Added

- Added the JSON and RPC modes with pi's records, the C SDK header and extension events for hook commands and Lua extensions
- Added MCP servers (stdio and HTTP), skills and prompt templates, packages in the `extensions`, `skills`, `prompts` and `themes` settings
- Added `/login` and `/logout` for the subscription accounts and API keys, and the Radius models through the pi-messages API
- Added the session tree, `/fork`, `/clone`, `-r` and compaction in pi's look
- Added pi's themes and `keybindings.json`

## [1.0.4] - 2026-10-11

### Added

- Added amiga-pi: the pi coding agent on the Amiga, in C, with the tools `read`, `bash`, `edit`, `write`, `grep`, `find` and `ls`
- Added the Anthropic, OpenAI, Google, Azure and Bedrock providers, `models.json` and sessions as pi writes them
- Added pi's interactive screen in UP-Term and on a plain console, and print mode (`-p`)
