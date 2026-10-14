#!/usr/bin/env python3
"""tui_gen -- the goldens of tests/pi/tui (plan N0): real pi 1.2.0 in tmux at the Amiga window's
size, against a local fake Messages API (scripted answers, no key, no cost), each screen captured
with `tmux capture-pane -e -p` (text and SGR colours).

  python3 tools/pi_oracle/tui_gen.py [--only <scenario>] [--stale] [--cols 80] [--rows 25] [--keep]

The mode and the theme of every scenario are written into its settings.json (TM1, TM6): `tui` is pi's tuiMode
("fullscreen", pi 1.0.4's default and the mode of every golden captured before TM1, or "regular"), `theme` the theme
name ("system", the default, or "dark" / "light": pi's own, with element backgrounds), `settings` more settings.json
keys (fullscreenExitOutput, ...). <name>.mode records what tmux saw (alternate screen on or off, the pane dead or not).
`raw` keeps pi's whole output stream (PI_TUI_WRITE_LOG) as <name>.raw; `exit` ends pi with Ctrl+D and captures the
main screen it leaves behind.

Writes, per scenario, tests/pi/tui/<name>.golden (the pane with SGR), <name>.txt (text only) and
the SSE answers it served (<name>.N.sse: the host screen tests play the same bytes to amiga-pi).
Scenarios run in a throw-away PI_CODING_AGENT_DIR with a models.json whose provider "fake" points
at the fixture. Environment pinned: TERM=xterm-256color, COLORTERM unset (256 colours: what the
Amiga side is mapped from), PI_OFFLINE=1, PI_SKIP_VERSION_CHECK=1, PI_TELEMETRY=0, fixed cwd.
"""
import argparse, http.server, json, os, shutil, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'tests', 'pi', 'tui'))
MODEL = 'fake-model'


def find_pi():
    """The pi the goldens come from: $PI_BIN, else the pinned 1.2.0 install (vendor/pi-1.2.0, npm install --prefix
    vendor/pi-1.2.0 @earendil-works/pi-coding-agent@1.2.0) in this checkout or the main one. Never `pi` on the PATH:
    the owner's `pi update` moves it (it moved to 1.1.0 mid-phase once)."""
    if os.environ.get('PI_BIN'):
        return os.environ['PI_BIN']
    # the one place that knows the pin: tools/pi_oracle/pin.mjs (this checkout's vendor/, else the main one; exit 2 if not 1.2.0)
    r = subprocess.run(['node', os.path.join(HERE, 'pin.mjs'), 'npm'], capture_output=True, text=True)
    if r.returncode:
        sys.exit('tui_gen: ' + r.stderr.strip())
    return os.path.join(r.stdout.strip(), '.bin', 'pi')


TAG = os.environ.get('PITUI_TAG', 'pitui')
PI = find_pi()
PI_VERSION = subprocess.run([PI, '--version'], capture_output=True, text=True).stdout.strip()


def sse(events):
    return ''.join('event: %s\ndata: %s\n\n' % (e['type'], json.dumps(e, separators=(',', ':'))) for e in events)


def msg_start(mid):
    return {'type': 'message_start', 'message': {'id': mid, 'type': 'message', 'role': 'assistant', 'model': MODEL,
            'content': [], 'stop_reason': None, 'stop_sequence': None,
            'usage': {'input_tokens': 1200, 'output_tokens': 1, 'cache_read_input_tokens': 0, 'cache_creation_input_tokens': 0}}}


def text_answer(mid, text, out_tokens=60, chunk=24):
    ev = [msg_start(mid), {'type': 'content_block_start', 'index': 0, 'content_block': {'type': 'text', 'text': ''}}]
    for i in range(0, len(text), chunk):
        ev.append({'type': 'content_block_delta', 'index': 0, 'delta': {'type': 'text_delta', 'text': text[i:i + chunk]}})
    ev += [{'type': 'content_block_stop', 'index': 0},
           {'type': 'message_delta', 'delta': {'stop_reason': 'end_turn', 'stop_sequence': None},
            'usage': {'output_tokens': out_tokens}},
           {'type': 'message_stop'}]
    return sse(ev)


def tool_answer(mid, tid, name, args, lead=None):
    ev = [msg_start(mid)]
    i = 0
    if lead:
        ev += [{'type': 'content_block_start', 'index': 0, 'content_block': {'type': 'text', 'text': ''}},
               {'type': 'content_block_delta', 'index': 0, 'delta': {'type': 'text_delta', 'text': lead}},
               {'type': 'content_block_stop', 'index': 0}]
        i = 1
    ev += [{'type': 'content_block_start', 'index': i, 'content_block': {'type': 'tool_use', 'id': tid, 'name': name, 'input': {}}},
           {'type': 'content_block_delta', 'index': i, 'delta': {'type': 'input_json_delta', 'partial_json': json.dumps(args)}},
           {'type': 'content_block_stop', 'index': i},
           {'type': 'message_delta', 'delta': {'stop_reason': 'tool_use', 'stop_sequence': None}, 'usage': {'output_tokens': 40}},
           {'type': 'message_stop'}]
    return sse(ev)


def tool_answer2(mid, calls):
    """one assistant message with several tool calls (calls: (id, name, args)): pi runs them in parallel"""
    ev = [msg_start(mid)]
    for i, (tid, name, args) in enumerate(calls):
        ev += [{'type': 'content_block_start', 'index': i, 'content_block': {'type': 'tool_use', 'id': tid, 'name': name, 'input': {}}},
               {'type': 'content_block_delta', 'index': i, 'delta': {'type': 'input_json_delta', 'partial_json': json.dumps(args)}},
               {'type': 'content_block_stop', 'index': i}]
    ev += [{'type': 'message_delta', 'delta': {'stop_reason': 'tool_use', 'stop_sequence': None}, 'usage': {'output_tokens': 40}},
           {'type': 'message_stop'}]
    return sse(ev)


MARKDOWN = (
    "# Plan\n\nHere is **bold**, *italic* and `inline code`.\n\n"
    "- first item\n- second item with a [link](https://example.org)\n\n"
    "> a quoted line\n\n"
    "```c\nint main(void)\n{\n    return 0;\n}\n```\n")

MARKDOWN2 = (
    "## Second\n\nA paragraph straight above a list:\n- alpha\n- beta\n    - nested one\n    - nested two\n- gamma\n\n"
    "### Third level\nText right under a heading with ~~struck~~ words and https://example.org/x bare.\n\n"
    "1. first\n2. second\n3. third\n\n---\n\nClosing line.\n")

TABLE = "1. one\n2. two\n\n| name | size |\n|------|------|\n| a | 1 |\n| b | 22 |\n"

ERR_BODY = json.dumps({'type': 'error', 'error': {'type': 'invalid_request_error', 'message': 'model: fake-model is not available'}})

# scenario: name, keys sent after the start (list of (text, enter?, wait)), answers served in order
# an answer is ('sse', text) or ('http', status, body)

# P12-B2/B3: the CHANGELOG.md pi reads (PI_PACKAGE_DIR holds symlinks to the install and this file); amiga-pi's tests feed the same file
CHANGELOG_FIXTURE = """# Changelog

## [1.2.0] - 2026-10-14

### Added

- Added `/changelog` and the **What's New** notice after an update.
- Added [the manual](https://example.org/manual) for the Amiga port.

### Fixed

- Fixed a very long line in the changelog so that the Markdown renderer has to wrap it at the width of the window.

## [1.1.0] - 2026-10-07

### Added

- Added the first interactive screen.

## [1.0.0] - 2026-10-01

- First release.
"""

SCENARIOS = {
    # the start screen alone
    'start': dict(steps=[], answers=[]),
    # the editor with typed text, the autocomplete menu for a slash
    'editor': dict(steps=[('type', 'hello there')], answers=[]),
    'slash': dict(steps=[('type', '/')], answers=[]),
    # J6: the menu filter is pi-tui's fuzzy match (autocomplete.ts:343-372, fuzzy.ts), in order of score: '/re' lists commands that only
    # contain r then e ('tree'), not just those that start with it
    'slash_re': dict(steps=[('type', '/re')], answers=[]),
    'slash_ct': dict(steps=[('type', '/ct')], answers=[]),
    # J: prompt templates in the menu (the agent directory's prompts/: description with its [u] tag, argument hint first)
    'slash_prompts': dict(steps=[('type', '/rev')], answers=[], agent_tree='agent_dir'),
    # U1: the menu follows the text before the cursor ('/res' with the cursor before the x), and matches non-ASCII names lowercased
    # (agent_dir_u/prompts: café, ärger, 日本語): '/É' lists café
    'slash_mid': dict(steps=[('type', '/tre'), ('key', 'Left')], answers=[]),
    # ... but typing that left no match closed the menu, and moving the cursor does not open it again (editor.ts updateAutocomplete
    # runs only while the menu is open)
    'slash_closed': dict(steps=[('type', '/resx'), ('key', 'Left')], answers=[]),
    'slash_utf': dict(steps=[('type', '/\u00c9')], answers=[], agent_tree='agent_dir_u'),
    # U1: /login (interactive-mode.ts handleLoginCommand, showLoginAuthTypeSelector, showLoginProviderSelector, showLoginDialog)
    'login_method': dict(steps=[('type', '/login'), ('enter',), ('sleep', 0.5)], answers=[]),
    'login_providers': dict(steps=[('type', '/login'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 0.5)], answers=[]),
    'login_search': dict(steps=[('type', '/login'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 0.5), ('type', 'cop')], answers=[]),
    # Q3-18: /login with text that names no provider opens the provider selector with that text in its search box (setValue: the cursor
    # at the start); Right, Right, a typed character, End and Left then move and insert at the cursor (input.ts:171-203)
    'login_arg_search': dict(steps=[('type', '/login cop'), ('key', 'Escape'), ('enter',), ('sleep', 0.5)], answers=[]),
    'login_arg_keys': dict(steps=[('type', '/login cop'), ('key', 'Escape'), ('enter',), ('sleep', 0.5), ('key', 'Right'), ('key', 'Right'), ('type', 'x'),
                                  ('key', 'End'), ('key', 'Left'), ('key', 'Left'), ('sleep', 0.5)], answers=[]),
    'login_apikey': dict(steps=[('type', '/login'), ('enter',), ('sleep', 0.5), ('key', 'Down'), ('enter',), ('sleep', 0.5)], answers=[]),
    'login_copilot': dict(steps=[('type', '/login github-copilot'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 1.0)], answers=[]),
    'login_openai': dict(steps=[('type', '/login openai'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 1.0), ('enter',), ('sleep', 1.5)], answers=[]),
    # the method of one provider, and Esc in its dialog back to it (showLoginDialog onBack)
    'login_openai_method': dict(steps=[('type', '/login openai'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 1.0)], answers=[]),
    # Esc in the Copilot dialog (its domain prompt): back to the method selector it came from. The ChatGPT dialog is different in pi:
    # its Esc ends in "Failed to login to OpenAI: This operation was aborted" (login_openai_cancel), no way back
    'login_copilot_back': dict(steps=[('type', '/login github-copilot'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 0.5), ('enter',),
                                      ('sleep', 1.0), ('key', 'Escape'), ('sleep', 1.0)], answers=[]),
    'login_openai_cancel': dict(steps=[('type', '/login openai'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 1.0),
                                       ('key', 'Escape'), ('sleep', 1.0)], answers=[]),
    # /logout with two stored credentials (a subscription and an API key: the rows say which)
    'login_logout': dict(steps=[('type', '/logout'), ('enter',), ('sleep', 0.5)], answers=[],
                         agent_files={'auth.json': json.dumps({'openai': {'type': 'api_key', 'key': 'sk-test'},
                                                              'github-copilot': {'type': 'oauth', 'access': 'a', 'refresh': 'r', 'expires': 99999999999999}})}),
    # the device code screen (showDeviceCode + showWaiting): github-copilot against the fixture's OAuth server (pending forever)
    'login_device': dict(steps=[('type', '/login github-copilot'), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 0.5), ('enter',), ('sleep', 1.0),
                                ('enter',), ('sleep', 3.0)], answers=[],
                         oauth=[{'url': 'https://github.com/login/device/code', 'status': 200,
                                 'json': {'device_code': 'dc-1', 'user_code': 'WXYZ-9876', 'verification_uri': 'https://github.com/login/device',
                                          'interval': 1, 'expires_in': 900}},
                                {'url': 'https://github.com/login/oauth/access_token', 'status': 200, 'json': {'error': 'authorization_pending'}}]),
    # a markdown answer
    'markdown': dict(steps=[('type', 'show markdown'), ('enter',), ('wait', 'return 0')],
                     answers=[('sse', text_answer('msg_md', MARKDOWN, 180))]),
    'table': dict(steps=[('type', 'show a table'), ('enter',), ('wait', '22')],
                  answers=[('sse', text_answer('msg_tb', TABLE, 60))]),
    # the project trust question: the one approval prompt pi has at start (a tool call is never asked)
    'approval': dict(steps=[('wait', 'rust')], answers=[], files={'.pi/SYSTEM.md': 'Be brief.\n'}),
    'markdown2': dict(steps=[('type', 'more markdown'), ('enter',), ('wait', 'Closing line')],
                      answers=[('sse', text_answer('msg_md2', MARKDOWN2, 150))], rows=50),
    # the spinner row while the answer is awaited (the captured frame is a moment of it)
    'working': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'Working'), ('capture_now',)],
                    answers=[('slow', 6, text_answer('msg_w', 'late answer', 20))]),
    # a message typed while the agent works (Enter steers) and one queued for after (Alt+Enter follows up)
    'queued': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'Working'), ('type', 'steer me'), ('enter',),
                          ('type', 'and then this'), ('key', 'M-Enter'), ('sleep', 0.5), ('capture_now',)],
                   answers=[('slow', 6, text_answer('msg_q', 'late answer', 20))]),
    # a second prompt after an answer: the blank lines between messages
    'twice': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('type', 'second'), ('enter',), ('wait', 'two')],
                  answers=[('sse', text_answer('msg_a', 'answer one', 20)), ('sse', text_answer('msg_b', 'answer two', 20))]),
    # /tree over a session with a branch (tests/pi/sessions/doc_branch.jsonl): the selector and its keys
    'tree': dict(steps=[('type', '/tree'), ('sleep', 0.8), ('enter',), ('sleep', 1.5)], answers=[], session='doc_branch.jsonl'),
    # ! runs a command and puts its output into the context, !! keeps it out
    'bang': dict(steps=[('type', '!echo hi'), ('enter',), ('sleep', 1.5), ('type', '!!echo secret'), ('enter',), ('sleep', 1.5)], answers=[]),
    # a tool call block (read of a file in the cwd) and the answer after it
    'tool': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading')],
                 answers=[('sse', tool_answer('msg_t1', 'toolu_1', 'read', {'path': 'notes.txt'}, 'I will read it.')),
                          ('sse', text_answer('msg_t2', 'Done reading notes.txt: three lines.', 20))]),
    # a bash tool block that fails
    'toolerr': dict(steps=[('type', 'run it'), ('enter',), ('wait', 'It failed')],
                    answers=[('sse', tool_answer('msg_e1', 'toolu_2', 'bash', {'command': 'cat nothere.txt'})),
                             ('sse', text_answer('msg_e2', 'It failed: no such file.', 20))]),
    # the request itself fails
    'error': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'not available')],
                  answers=[('http', 400, ERR_BODY)]),
    # TM1/TM3: regular mode (the main screen, no clearing, the editor follows the content) against fullscreen
    # (the alternate screen, the editor pinned to the bottom: the `twice` golden above)
    'reg_start': dict(steps=[], answers=[], tui='regular'),
    # P12-B2/B3: What's New on the first start after an update (lastChangelogVersion older than 1.2.0), collapsed, on a fresh install,
    # and /changelog; the settings.json pi leaves is kept as <name>.settings
    # P12-B9: /model, pi's model-selector.ts, over a fake provider with three models (the first is the session's)
    'model_selector': dict(steps=[('type', '/model'), ('enter',), ('wait', 'Model Name')], answers=[],
                           models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama=True),
    'model_search': dict(steps=[('type', '/model'), ('enter',), ('wait', 'Model Name'), ('type', 'large'), ('sleep', 0.5)], answers=[],
                         models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama=True),
    'model_down': dict(steps=[('type', '/model'), ('enter',), ('wait', 'Model Name'), ('key', 'Down'), ('sleep', 0.5)], answers=[],
                       models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama=True),
    'model_scoped': dict(steps=[('type', '/model'), ('enter',), ('wait', 'Scope:')], answers=[],
                         models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama=True,
                         settings={'enabledModels': ['fake/fake-model', 'fake/fake-large']}),
    'model_select': dict(steps=[('type', '/model'), ('enter',), ('wait', 'Model Name'), ('key', 'Down'), ('enter',), ('wait', 'Model: ')], answers=[],
                         models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama=True),
    'model_arg': dict(steps=[('type', '/model fake-small'), ('key', 'Escape'), ('enter',), ('wait', 'Model: ')], answers=[],
                      models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')]),
    'model_arg_search': dict(steps=[('type', '/model fak'), ('key', 'Escape'), ('enter',), ('wait', 'Model Name'), ('sleep', 0.5)], answers=[],
                             models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')]),
    'model_arg_keys': dict(steps=[('type', '/model fak'), ('key', 'Escape'), ('enter',), ('wait', 'Model Name'), ('key', 'Right'), ('key', 'Right'),
                                  ('type', 'x'), ('key', 'Home'), ('key', 'Right'), ('sleep', 0.5)], answers=[],
                           models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')]),
    # P12-B11: /scoped-models, scoped-models-selector.ts (all enabled, a scope, a toggle, a save to settings.json, a search, a reorder, "clear all")
    'scoped_selector': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0)], answers=[],
                            models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'scoped_listed': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0)], answers=[],
                          models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True,
                          settings={'enabledModels': ['fake/fake-large', 'fake/fake-model']}),
    'scoped_toggle': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0), ('key', 'Down'), ('enter',), ('sleep', 0.5)],
                          answers=[], models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'scoped_save': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0), ('key', 'Down'), ('enter',), ('sleep', 0.5),
                               ('key', 'C-s'), ('sleep', 0.5), ('key', 'Escape'), ('wait', 'Model selection saved')],
                        answers=[], models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'scoped_search': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0), ('type', 'large'), ('sleep', 0.5)],
                          answers=[], models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'scoped_reorder': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0), ('key', 'M-Down'), ('sleep', 0.5)],
                           answers=[], models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True,
                           settings={'enabledModels': ['fake/fake-model', 'fake/fake-large']}),
    'scoped_clear': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0), ('key', 'C-x'), ('sleep', 0.5)],
                         answers=[], models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'scoped_provider': dict(steps=[('type', '/scoped-models'), ('enter',), ('wait', 'Model Configuration'), ('sleep', 1.0), ('key', 'C-x'), ('sleep', 0.3), ('key', 'C-p'), ('sleep', 0.5)],
                            answers=[], models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    # P12-B12/B13/B16: /settings, settings-selector.ts (the list, scrolled to its end, a search, a boolean and an enum toggled, the theme menus, the
    # warnings menu, the per-model thinking menu, and with a terminal that draws images the Show images and Image width rows)
    'settings_list': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_end': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('key', 'Up'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_down': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('key', 'Down'),
                                 ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('sleep', 0.5)],
                          answers=[], settings_out=True),
    'settings_search': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'steer'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_nomatch': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'zzqq'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_toggle': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('enter',), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_enum': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'steering'), ('enter',), ('sleep', 0.5)],
                          answers=[], settings_out=True),
    'settings_theme': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'theme'), ('enter',), ('wait', 'Select a theme'), ('sleep', 0.5)],
                           answers=[], settings_out=True),
    'settings_theme_pick': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'theme'), ('enter',), ('wait', 'Select a theme'),
                                       ('key', 'Down'), ('key', 'Down'), ('key', 'Down'), ('enter',), ('sleep', 1.0)], answers=[], settings_out=True),
    'settings_theme_auto': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'theme'), ('enter',), ('wait', 'Select a theme'),
                                       ('key', 'Down'), ('enter',), ('wait', 'Automatic Theme'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_theme_apply': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'theme'), ('enter',), ('wait', 'Select a theme'),
                                        ('key', 'Down'), ('enter',), ('wait', 'Automatic Theme'), ('key', 'Down'), ('key', 'Down'), ('enter',), ('sleep', 1.0)],
                                 answers=[], settings_out=True),
    'settings_warnings': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'warnings'), ('enter',), ('wait', 'Anthropic extra usage'),
                                     ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_warnings_off': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'warnings'), ('enter',), ('wait', 'Anthropic extra usage'),
                                         ('enter',), ('sleep', 0.5), ('key', 'Escape'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_modelthinking': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'per model'), ('enter',), ('wait', 'Per-Model'),
                                          ('sleep', 0.5)], answers=[], reasoning=True, models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'settings_modelthinking_level': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'per model'), ('enter',), ('wait', 'Per-Model'),
                                                ('enter',), ('wait', 'Thinking Level for'), ('sleep', 0.5)], answers=[], reasoning=True,
                                         models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'settings_modelthinking_set': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'per model'), ('enter',), ('wait', 'Per-Model'),
                                              ('enter',), ('wait', 'Thinking Level for'), ('key', 'Down'), ('key', 'Down'), ('enter',), ('sleep', 0.8)],
                                       answers=[], reasoning=True, models=[('fake-small', 'Fake Small'), ('fake-large', 'Fake Large')], ollama='first', settings_out=True),
    'settings_followup': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('type', 'follow'), ('sleep', 0.5)], answers=[], settings_out=True),
    'settings_images': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('sleep', 0.5)], answers=[], settings_out=True,
                            env='PI_IMAGE_PROTOCOL=kitty'),
    'settings_images_off': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('key', 'Down'), ('enter',), ('sleep', 0.5)], answers=[],
                                settings_out=True, env='PI_IMAGE_PROTOCOL=kitty'),
    'settings_set': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('sleep', 0.5)], answers=[], settings_out=True, rows=45,
                         settings={'compaction': {'enabled': False, 'reserveTokens': 1000}, 'steeringMode': 'all', 'followUpMode': 'all', 'quietStartup': True,
                                   'hideThinkingBlock': True, 'terminal': {'showImages': False, 'clearOnShrink': True}, 'images': {'blockImages': True},
                                   'outputPad': 0, 'editorPaddingX': 2, 'autocompleteMaxVisible': 10, 'defaultProjectTrust': 'always',
                                   'doubleEscapeAction': 'fork', 'treeFilterMode': 'user-only', 'fullscreenWheelScrollLines': 7, 'httpIdleTimeoutMs': 0,
                                   'transport': 'sse', 'cacheWarming': 'idle', 'markdown': {'mermaid': 'off'}, 'enableInstallTelemetry': False,
                                   'collapseChangelog': True, 'showCacheMissNotices': True, 'enableSkillCommands': False, 'showHardwareCursor': True,
                                   'tuiMode': 'fullscreen', 'fullscreenExitOutput': 'resume-hint', 'fullscreenScrollbar': 'hidden',
                                   'fullscreenCopyOnSelect': False, 'warnings': {'anthropicExtraUsage': False}}),
    'settings_set_b': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact')] + [('key', 'Down')] * 14 + [('sleep', 0.5)], answers=[],
                           settings_out=True, rows=45, settings={'compaction': {'enabled': False, 'reserveTokens': 1000}, 'steeringMode': 'all', 'followUpMode': 'all', 'quietStartup': True,
                                   'hideThinkingBlock': True, 'terminal': {'showImages': False, 'clearOnShrink': True}, 'images': {'blockImages': True},
                                   'outputPad': 0, 'editorPaddingX': 2, 'autocompleteMaxVisible': 10, 'defaultProjectTrust': 'always',
                                   'doubleEscapeAction': 'fork', 'treeFilterMode': 'user-only', 'fullscreenWheelScrollLines': 7, 'httpIdleTimeoutMs': 0,
                                   'transport': 'sse', 'cacheWarming': 'idle', 'markdown': {'mermaid': 'off'}, 'enableInstallTelemetry': False,
                                   'collapseChangelog': True, 'showCacheMissNotices': True, 'enableSkillCommands': False, 'showHardwareCursor': True,
                                   'tuiMode': 'fullscreen', 'fullscreenExitOutput': 'resume-hint', 'fullscreenScrollbar': 'hidden',
                                   'fullscreenCopyOnSelect': False, 'warnings': {'anthropicExtraUsage': False}}),
    'settings_set_c': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact')] + [('key', 'Down')] * 24 + [('sleep', 0.5)], answers=[],
                           settings_out=True, rows=45, settings={'compaction': {'enabled': False, 'reserveTokens': 1000}, 'steeringMode': 'all', 'followUpMode': 'all', 'quietStartup': True,
                                   'hideThinkingBlock': True, 'terminal': {'showImages': False, 'clearOnShrink': True}, 'images': {'blockImages': True},
                                   'outputPad': 0, 'editorPaddingX': 2, 'autocompleteMaxVisible': 10, 'defaultProjectTrust': 'always',
                                   'doubleEscapeAction': 'fork', 'treeFilterMode': 'user-only', 'fullscreenWheelScrollLines': 7, 'httpIdleTimeoutMs': 0,
                                   'transport': 'sse', 'cacheWarming': 'idle', 'markdown': {'mermaid': 'off'}, 'enableInstallTelemetry': False,
                                   'collapseChangelog': True, 'showCacheMissNotices': True, 'enableSkillCommands': False, 'showHardwareCursor': True,
                                   'tuiMode': 'fullscreen', 'fullscreenExitOutput': 'resume-hint', 'fullscreenScrollbar': 'hidden',
                                   'fullscreenCopyOnSelect': False, 'warnings': {'anthropicExtraUsage': False}}),
    'settings_set_d': dict(steps=[('type', '/settings'), ('enter',), ('wait', 'Auto-compact'), ('key', 'Up'), ('sleep', 0.5)], answers=[],
                           settings_out=True, rows=45, settings={'compaction': {'enabled': False, 'reserveTokens': 1000}, 'steeringMode': 'all', 'followUpMode': 'all', 'quietStartup': True,
                                   'hideThinkingBlock': True, 'terminal': {'showImages': False, 'clearOnShrink': True}, 'images': {'blockImages': True},
                                   'outputPad': 0, 'editorPaddingX': 2, 'autocompleteMaxVisible': 10, 'defaultProjectTrust': 'always',
                                   'doubleEscapeAction': 'fork', 'treeFilterMode': 'user-only', 'fullscreenWheelScrollLines': 7, 'httpIdleTimeoutMs': 0,
                                   'transport': 'sse', 'cacheWarming': 'idle', 'markdown': {'mermaid': 'off'}, 'enableInstallTelemetry': False,
                                   'collapseChangelog': True, 'showCacheMissNotices': True, 'enableSkillCommands': False, 'showHardwareCursor': True,
                                   'tuiMode': 'fullscreen', 'fullscreenExitOutput': 'resume-hint', 'fullscreenScrollbar': 'hidden',
                                   'fullscreenCopyOnSelect': False, 'warnings': {'anthropicExtraUsage': False}}),
    # P12-B12 behaviours: editorPaddingX and autocompleteMaxVisible (the editor and the slash menu), showHardwareCursor
    'pad_editor': dict(steps=[('type', 'hello padded world')], answers=[], settings={'editorPaddingX': 2}),
    'pad_menu': dict(steps=[('type', '/')], answers=[], settings={'editorPaddingX': 3, 'autocompleteMaxVisible': 10}),
    'menu_max3': dict(steps=[('type', '/')], answers=[], settings={'autocompleteMaxVisible': 3}),
    # P12-B10: /thinking, thinking-selector.ts (a reasoning model); P12-B15: /fork, user-message-selector.ts
    'thinking_selector': dict(steps=[('type', '/thinking'), ('enter',), ('wait', 'cycles thinking')], answers=[], reasoning=True),
    'thinking_search': dict(steps=[('type', '/thinking'), ('enter',), ('wait', 'cycles thinking'), ('type', 'low'), ('sleep', 0.5)], answers=[],
                            reasoning=True),
    'thinking_select': dict(steps=[('type', '/thinking'), ('enter',), ('wait', 'cycles thinking'), ('key', 'Down'), ('enter',), ('wait', 'Thinking level: ')],
                            answers=[], reasoning=True),
    'fork_selector': dict(steps=[('type', '/fork'), ('enter',), ('wait', 'Fork from')], answers=[], session='../sessions/doc_branch.jsonl'),
    'fork_up': dict(steps=[('type', '/fork'), ('enter',), ('wait', 'Fork from'), ('key', 'Up'), ('sleep', 0.5)], answers=[],
                    session='../sessions/doc_branch.jsonl'),
    'whats_new': dict(steps=[], answers=[], changelog=True, settings={'lastChangelogVersion': '1.1.0'}),
    'whats_new_collapsed': dict(steps=[], answers=[], changelog=True, settings={'lastChangelogVersion': '1.1.0', 'collapseChangelog': True}),
    'whats_new_fresh': dict(steps=[], answers=[], changelog=True),
    'whats_new_current': dict(steps=[], answers=[], changelog=True, settings={'lastChangelogVersion': '1.2.0'}),
    'changelog_cmd': dict(steps=[('type', '/changelog'), ('enter',)], answers=[], changelog=True, settings={'lastChangelogVersion': '1.2.0'}),
    'reg_empty': dict(steps=[], answers=[], tui='regular'),
    'reg_twice': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('type', 'second'), ('enter',), ('wait', 'two')],
                      answers=[('sse', text_answer('msg_a', 'answer one', 20)), ('sse', text_answer('msg_b', 'answer two', 20))],
                      tui='regular'),
    'reg_markdown': dict(steps=[('type', 'show markdown'), ('enter',), ('wait', 'return 0')],
                         answers=[('sse', text_answer('msg_md', MARKDOWN, 180))], tui='regular'),
    # a transcript longer than the window in regular mode: the editor at the bottom, the older rows in scrollback
    'reg_long': dict(steps=[('type', 'more markdown'), ('enter',), ('wait', 'Closing line')],
                     answers=[('sse', text_answer('msg_md2', MARKDOWN2, 150))], tui='regular', rows=14),
    # a transcript longer than the window in fullscreen mode: the viewport follows the end; the scrollbar's cells
    'fs_long': dict(steps=[('type', 'more markdown'), ('enter',), ('wait', 'Closing line')],
                    answers=[('sse', text_answer('msg_md2', MARKDOWN2, 150))], rows=14),
    # the same after PageUp (the transcript scrolled back one page)
    'fs_long_up': dict(steps=[('type', 'more markdown'), ('enter',), ('wait', 'Closing line'), ('key', 'PageUp'), ('sleep', 0.8)],
                       answers=[('sse', text_answer('msg_md2', MARKDOWN2, 150))], rows=14),
    # fullscreenScrollbar "always": the track and the thumb at the right edge of the transcript
    'fs_long_bar': dict(steps=[('type', 'more markdown'), ('enter',), ('wait', 'Closing line'), ('key', 'PageUp'), ('sleep', 0.8)],
                        answers=[('sse', text_answer('msg_md2', MARKDOWN2, 150))], rows=14,
                        settings={'fullscreenScrollbar': 'always'}),
    # leaving fullscreen: "transcript" prints the conversation on the main screen, "resume-hint" only the hint
    'exit_transcript': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('sleep', 1.5), ('exit',)],
                            answers=[('sse', text_answer('msg_a', 'answer one', 20))], persist=True, raw=True),
    'exit_hint': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('sleep', 1.5), ('exit',)],
                      answers=[('sse', text_answer('msg_a', 'answer one', 20))], persist=True, raw=True,
                      settings={'fullscreenExitOutput': 'resume-hint'}),
    # Q11: --session-dir with a space in the path: the resume line carries it, quoted (formatResumeCommand, quoteIfNeeded)
    'exit_sdir': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('sleep', 1.5), ('exit',)],
                      answers=[('sse', text_answer('msg_a', 'answer one', 20))], persist=True, raw=True,
                      sdir='my sessions'),
    'exit_regular': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('sleep', 1.5), ('exit',)],
                         answers=[('sse', text_answer('msg_a', 'answer one', 20))], persist=True, raw=True, tui='regular'),
    # TM4: what pi's output stream holds when a turn finishes (no bell, no notification)
    'turn_raw': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'late answer'), ('sleep', 1.0)],
                     answers=[('sse', text_answer('msg_n', 'late answer', 20))], raw=True),
    # TM6: pi's dark and light themes draw backgrounds behind the user message, the tool boxes and the bash output
    'dark_start': dict(steps=[], answers=[], theme='dark'),
    'dark_twice': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('type', 'second'), ('enter',), ('wait', 'two')],
                       answers=[('sse', text_answer('msg_a', 'answer one', 20)), ('sse', text_answer('msg_b', 'answer two', 20))],
                       theme='dark'),
    'dark_markdown': dict(steps=[('type', 'show markdown'), ('enter',), ('wait', 'return 0')],
                          answers=[('sse', text_answer('msg_md', MARKDOWN, 180))], theme='dark'),
    'dark_tool': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading')],
                      answers=[('sse', tool_answer('msg_t1', 'toolu_1', 'read', {'path': 'notes.txt'}, 'I will read it.')),
                               ('sse', text_answer('msg_t2', 'Done reading notes.txt: three lines.', 20))], theme='dark'),
    'dark_toolerr': dict(steps=[('type', 'run it'), ('enter',), ('wait', 'It failed')],
                         answers=[('sse', tool_answer('msg_e1', 'toolu_2', 'bash', {'command': 'cat nothere.txt'})),
                                  ('sse', text_answer('msg_e2', 'It failed: no such file.', 20))], theme='dark'),
    # a tool box while the tool runs (toolPendingBg)
    'dark_toolrun': dict(steps=[('type', 'run it'), ('enter',), ('wait', 'sleep 4'), ('sleep', 1.0), ('capture_now',)],
                         answers=[('sse', tool_answer('msg_r1', 'toolu_3', 'bash', {'command': 'sleep 4'})),
                                  ('sse', text_answer('msg_r2', 'Slept.', 20))], theme='dark'),
    # Q9: the same with output so far: bash's update carries "hi", the box shows it above the Elapsed row (renderers/bash.ts:61-111)
    'dark_toolrun_out': dict(steps=[('type', 'run it'), ('enter',), ('wait', 'sleep 4'), ('sleep', 1.0), ('capture_now',)],
                             answers=[('sse', tool_answer('msg_r1', 'toolu_3', 'bash', {'command': 'echo hi; sleep 4'})),
                                      ('sse', text_answer('msg_r2', 'Slept.', 20))], theme='dark'),
    # Q10: two tool calls of one message run in parallel; the second (echo hi) ends before the first (cat of a missing file,
    # which fails): each box takes its own result's colour, whatever the order they end in
    'dark_tools2': dict(steps=[('type', 'run both'), ('enter',), ('wait', 'It failed')],
                        answers=[('sse', tool_answer2('msg_p1', [('toolu_p1', 'bash', {'command': 'sleep 1; cat nothere.txt'}),
                                                                  ('toolu_p2', 'bash', {'command': 'echo hi'})])),
                                 ('sse', text_answer('msg_p2', 'It failed: no such file.', 20))], theme='dark', rows=32,
                        # amiga-pi runs its tools one after the other: the host test plays the first call alone and makes
                        # the second one's events itself (tests/test_pi_tui.c), so the screens can be compared
                        host=[tool_answer('msg_p1', 'toolu_p1', 'bash', {'command': 'sleep 1; cat nothere.txt'}),
                              text_answer('msg_p2', 'It failed: no such file.', 20)]),
    'dark_bang': dict(steps=[('type', '!echo hi'), ('enter',), ('sleep', 1.5)],
                      answers=[], theme='dark'),
    # P11 (pi 1.1.0): `!!` keeps its dim header after the output arrives (bash-execution.ts colorKey)
    'bang_excluded': dict(steps=[('type', '!!echo hi'), ('enter',), ('sleep', 1.5)], answers=[]),
    # P11: outputPad 0 (settings.json) removes the horizontal padding of messages, tool boxes and `!` output
    'pad0_markdown': dict(steps=[('type', 'show markdown'), ('enter',), ('wait', 'return 0')],
                          answers=[('sse', text_answer('msg_md', MARKDOWN, 180))], settings={'outputPad': 0}),
    'pad0_tool': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading')],
                      answers=[('sse', tool_answer('msg_t1', 'toolu_1', 'read', {'path': 'notes.txt'}, 'I will read it.')),
                               ('sse', text_answer('msg_t2', 'Done reading notes.txt: three lines.', 20))],
                      settings={'outputPad': 0}),
    'pad0_toolerr': dict(steps=[('type', 'run it'), ('enter',), ('wait', 'It failed')],
                         answers=[('sse', tool_answer('msg_e1', 'toolu_2', 'bash', {'command': 'cat nothere.txt'})),
                                  ('sse', text_answer('msg_e2', 'It failed: no such file.', 20))],
                         settings={'outputPad': 0}),
    'pad0_bang': dict(steps=[('type', '!echo hi'), ('enter',), ('sleep', 1.5), ('type', '!!echo secret'), ('enter',), ('sleep', 1.5)],
                      answers=[], settings={'outputPad': 0}),
    'dark_pad0_tool': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading')],
                           answers=[('sse', tool_answer('msg_t1', 'toolu_1', 'read', {'path': 'notes.txt'}, 'I will read it.')),
                                    ('sse', text_answer('msg_t2', 'Done reading notes.txt: three lines.', 20))],
                           theme='dark', settings={'outputPad': 0}),
    # P11 (pi 1.1.0 terminal.ts:296-298): the bytes pi writes to its terminal at start, taken from the pty with tmux pipe-pane
    # (PI_TUI_WRITE_LOG sees only the renderer's writes, not the startup queries): kitty flags + query, the OSC 7501 program
    # status query, then device attributes. tmux answers none of OSC 7501, so no status is ever reported.
    'start_bytes': dict(steps=[('sleep', 1.0)], answers=[], pipe=True),
    # PI_PROGRAM_STATUS=1 reports without asking: `working` while the turn runs, `done` after (program-status-reporter.ts)
    'status_bytes': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'late answer'), ('sleep', 1.5)],
                         answers=[('sse', text_answer('msg_n', 'late answer', 20))], pipe=True, env='PI_PROGRAM_STATUS=1'),
    # P11: a resumed session shows `Took` from the recorded durationMs of the toolResult (2300 ms: "Took 2.3s"); the
    # second call has none recorded and shows no Took (tool-execution.ts:137, renderers/bash.ts:104)
    # Q2: a bash tool result with escape sequences in a resumed session: pi's getTextOutput (render-utils.ts) strips them
    'resume_ansi': dict(steps=[('sleep', 1.0)], answers=[], session='ansi_session.jsonl'),
    'resume_took': dict(steps=[('sleep', 1.0)], answers=[], session='took_session.jsonl'),
    # Q8: a resumed session with a compaction summary, a custom message (display true) and a branch summary: pi 1.1.0's
    # CompactionSummaryMessageComponent, CustomMessageComponent, BranchSummaryMessageComponent (customMessageBg, outputPad)
    'resume_summaries': dict(steps=[('sleep', 1.0)], answers=[], session='summaries_session.jsonl', rows=50),
    # R4: ctrl+o (app.tools.expand) over the same session: toolOutputExpanded on, the summaries show their Markdown
    # (compaction-summary-message.ts:49-54, branch-summary-message.ts:43-48) and "Tool output: expanded" is shown
    'resume_summaries_expanded': dict(steps=[('sleep', 1.0), ('key', 'C-o'), ('sleep', 1.0)], answers=[],
                                      session='summaries_session.jsonl', rows=64),
    'dark_resume_summaries_expanded': dict(steps=[('sleep', 1.0), ('key', 'C-o'), ('sleep', 1.0)], answers=[],
                                           session='summaries_session.jsonl', rows=64, theme='dark'),
    'dark_pad0_resume_summaries_expanded': dict(steps=[('sleep', 1.0), ('key', 'C-o'), ('sleep', 1.0)], answers=[],
                                                session='summaries_session.jsonl', rows=64, theme='dark',
                                                settings={'outputPad': 0}),
    # ctrl+o twice: collapsed again, "Tool output: collapsed"
    'resume_summaries_toggled': dict(steps=[('sleep', 1.0), ('key', 'C-o'), ('sleep', 0.5), ('key', 'C-o'), ('sleep', 1.0)],
                                     answers=[], session='summaries_session.jsonl', rows=50),
    # Q17: live /compact on a resumed session: pi clears the chat, redraws it and appends the summary box
    # (interactive-mode.ts compaction_end); the second answer is the summarization request
    'compact': dict(steps=[('sleep', 0.5), ('type', '/compact'), ('enter',), ('wait', 'Compacted from'), ('sleep', 1.0)],
                    answers=[('sse', text_answer('msg_sum', '## Goal\nRun two commands.\n\n## Progress\nBoth ran.', 120))],
                    session='took_session.jsonl', rows=50, settings={'compaction': {'keepRecentTokens': 10}}),
    # Q17: automatic compaction after a turn (usage 1260 tokens against a window of 200000 less reserveTokens 199000)
    'compact_auto': dict(steps=[('sleep', 0.5), ('type', 'and then?'), ('enter',), ('wait', 'Compacted from'), ('sleep', 1.0)],
                         answers=[('sse', text_answer('msg_next', 'Nothing more.', 20)),
                                  ('sse', text_answer('msg_sum2', '## Goal\nRun two commands.\n\n## Progress\nBoth ran.', 120))],
                         session='took_session.jsonl', rows=50,
                         settings={'compaction': {'keepRecentTokens': 10, 'reserveTokens': 199000}}),
    'dark_resume_summaries': dict(steps=[('sleep', 1.0)], answers=[], session='summaries_session.jsonl', rows=50, theme='dark'),
    'light_resume_summaries': dict(steps=[('sleep', 1.0)], answers=[], session='summaries_session.jsonl', rows=50, theme='light'),
    'dark_pad0_resume_summaries': dict(steps=[('sleep', 1.0)], answers=[], session='summaries_session.jsonl', rows=50, theme='dark',
                                       settings={'outputPad': 0}),
    'light_twice': dict(steps=[('type', 'first'), ('enter',), ('wait', 'one'), ('type', 'second'), ('enter',), ('wait', 'two')],
                        answers=[('sse', text_answer('msg_a', 'answer one', 20)), ('sse', text_answer('msg_b', 'answer two', 20))],
                        theme='light'),
    'light_tool': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading')],
                       answers=[('sse', tool_answer('msg_t1', 'toolu_1', 'read', {'path': 'notes.txt'}, 'I will read it.')),
                                ('sse', text_answer('msg_t2', 'Done reading notes.txt: three lines.', 20))], theme='light'),
}


# I1/I3: skills in the agent directory (pi's ~/.pi/agent/skills): a valid one, a name pi warns about (it still loads), a skill without
# a description (skipped, warned), and two with one name (the second is a collision)
# U2: `pi -r`, pi 1.1.0's SessionSelectorComponent over seeded sessions. seed: (cwd name or '' for the project, file stem,
# first prompt, ISO timestamp, session name or None); ages are in years so the goldens hold while the clock moves
RESUME_SEED = [
    ('', '20200301T100000_aaaaaaaa', 'fix the build on the old branch', '2020-03-01T10:00:00.000Z', None),
    ('', '20220815T093000_bbbbbbbb', 'add a --verbose flag to the tool', '2022-08-15T09:30:00.000Z', 'verbose flag work'),
    ('', '20240615T180000_cccccccc', 'why does the second test hang', '2024-06-15T18:00:00.000Z', None),
]
RESUME_OTHER = [('other', '20230101T120000_dddddddd', 'a session of another folder', '2023-01-01T12:00:00.000Z', None)]
SCENARIOS.update({
    'resume_list': dict(steps=[], answers=[], resume=True, seed=RESUME_SEED),
    'resume_search': dict(steps=[('type', 'second'), ('sleep', 0.5)], answers=[], resume=True, seed=RESUME_SEED),
    'resume_down': dict(steps=[('key', 'Down'), ('key', 'Down')], answers=[], resume=True, seed=RESUME_SEED),
    'resume_none': dict(steps=[], answers=[], resume=True, seed=[]),
    'resume_none_esc': dict(steps=[('key', 'Escape'), ('ended',)], answers=[], resume=True, seed=[]),
    'resume_all': dict(steps=[('key', 'Tab'), ('sleep', 1.0)], answers=[], resume=True, seed=RESUME_SEED + RESUME_OTHER),
    'resume_confirm': dict(steps=[('key', 'C-d')], answers=[], resume=True, seed=RESUME_SEED),
    'resume_named': dict(steps=[('key', 'C-n')], answers=[], resume=True, seed=RESUME_SEED),
    'resume_sort': dict(steps=[('key', 'C-s')], answers=[], resume=True, seed=RESUME_SEED),
    'resume_path': dict(steps=[('key', 'C-p')], answers=[], resume=True, seed=RESUME_SEED),
    'resume_fork': dict(steps=[], answers=[], resume=True, seed=RESUME_SEED + [
        ('', '20240616T090000_eeeeeeee', 'a fork of the hang', '2024-06-16T09:00:00.000Z', None, '20240615T180000_cccccccc'),
        ('', '20240617T090000_ffffffff', 'a second fork', '2024-06-17T09:00:00.000Z', None, '20240615T180000_cccccccc')]),
    'resume_many': dict(steps=[('key', 'Down')] * 11, answers=[], resume=True, seed=[
        ('', '2020%02d01T100000_%08x' % (m, m), 'session number %d' % m, '2020-%02d-01T10:00:00.000Z' % m, None) for m in range(1, 13)]),
    'resume_enter': dict(steps=[('key', 'Down'), ('key', 'Enter'), ('wait', 'add a --verbose flag'), ('sleep', 1.5)],
                         answers=[], resume=True, seed=RESUME_SEED, persist=True),
})


def _sk(name, desc, extra=''):
    return '---\n' + ('name: %s\n' % name if name else '') + ('description: %s\n' % desc if desc else '') + extra + '---\nSay hello warmly.\n'


SKILL_FILES = {
    'skills/greet/SKILL.md': _sk('greet', 'Greets the user by name'),
    'skills/terse/SKILL.md': _sk('terse', 'Answers in five words'),
    'skills/Bad_Name/SKILL.md': _sk(None, 'Has a name pi warns about'),
    'skills/nodesc/SKILL.md': _sk('nodesc', None),
    'skills/zdup/SKILL.md': _sk('greet', 'A second skill called greet'),
}

SCENARIOS.update({
    # the start screen with skills: the [Skills] list (not quiet) and the [Skill conflicts] diagnostics (interactive-mode.ts:1772-1930)
    'skills_start': dict(steps=[], answers=[], agent_files=SKILL_FILES, rows=40),
    # quietStartup hides the header and the list, not the diagnostics (showDiagnosticsWhenQuiet, interactive-mode.ts:2044)
    'skills_quiet': dict(steps=[], answers=[], agent_files=SKILL_FILES, rows=30),
    # the slash menu offers /skill:name with "[u] description"
    'skills_menu': dict(steps=[('type', '/skill:'), ('sleep', 0.8)], answers=[], agent_files=SKILL_FILES, rows=30),
    # a session whose user messages are skill blocks: pi draws SkillInvocationMessageComponent, collapsed and (ctrl+o) expanded
    'resume_skill': dict(steps=[('sleep', 1.0)], answers=[], session='skill_session.jsonl', rows=30),
    'resume_skill_expanded': dict(steps=[('sleep', 1.0), ('key', 'C-o'), ('sleep', 1.0)], answers=[], session='skill_session.jsonl', rows=40),
    # /skill:name args typed live: the block and the arguments are one message; pi shows the block, then the arguments as the user's
    'skill_cmd': dict(steps=[('type', '/skill:greet please greet Bob'), ('enter',), ('wait', 'nice to meet you'), ('sleep', 1.0)],
                      answers=[('sse', text_answer('msg_sk', 'Hello Bob, nice to meet you!', 20))], agent_files=SKILL_FILES, rows=30),
})


# J5/I8: the start with prompt templates: the [Prompts] list, its Ctrl+O form, the [Prompt conflicts] block (a name two files have, a
# file whose YAML pi refuses, a path that is not there), and the skills list in its Ctrl+O form
CONFIG_FILES = {
    'skills/greet/SKILL.md': _sk('greet', 'Greets the user by name'),
    'skills/terse/SKILL.md': _sk('terse', 'Answers in five words'),
    'prompts/fix.md': '---\ndescription: Fix the failing test\n---\nFix the failing test: $ARGUMENTS\n',
    'prompts/review.md': '---\ndescription: Review the diff\n---\nReview the diff: $ARGUMENTS\n',
}

SCENARIOS.update({
    # P12-B17: `pi config -na`, config-selector.ts: the user's skills and prompt templates (and what settings.json switches off)
    'config_list': dict(steps=[('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources', agent_files=CONFIG_FILES, settings_out=True),
    'config_down': dict(steps=[('key', 'Down'), ('key', 'Down'), ('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources',
                        agent_files=CONFIG_FILES, settings_out=True),
    'config_toggle': dict(steps=[('type', ' '), ('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources', agent_files=CONFIG_FILES,
                          settings_out=True),
    'config_toggle_back': dict(steps=[('key', 'Down'), ('key', 'Down'), ('type', ' '), ('key', 'Up'), ('type', ' '), ('sleep', 0.5)], answers=[],
                               argv='config -na', ready='Global Resources', agent_files=CONFIG_FILES, settings_out=True),
    'config_search': dict(steps=[('type', 'fix'), ('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources', agent_files=CONFIG_FILES,
                          settings_out=True),
    'config_nomatch': dict(steps=[('type', 'zzqq'), ('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources', agent_files=CONFIG_FILES,
                           settings_out=True),
    'config_off': dict(steps=[('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources', agent_files=CONFIG_FILES, settings_out=True,
                       settings={'skills': ['-skills/terse/SKILL.md'], 'prompts': ['-prompts/fix.md']}),
    'config_empty': dict(steps=[('sleep', 0.5)], answers=[], argv='config -na', ready='Global Resources', settings_out=True)
})

PROMPT_FILES = {
    'prompts/review.md': '---\ndescription: Review a file\nargument-hint: "<path>"\n---\nReview $1 carefully.\n',
    'prompts/summarize.md': 'Summarize $ARGUMENTS in three lines.\n',
    'prompts/explain.md': '---\ndescription: Explain a piece of code\n---\nExplain $@.\n',
}
CONFLICT_FILES = dict(PROMPT_FILES)
CONFLICT_FILES.update({
    'extra/review.md': 'Another review, from a settings.json entry: $1\n',
    'prompts/broken.md': '---\ndescription: Use when: this\n---\nA template with refused YAML.\n',
})

SCENARIOS.update({
    'prompts_start': dict(steps=[], answers=[], agent_files=PROMPT_FILES, rows=40, settings={'quietStartup': False}),
    'prompts_start_expanded': dict(steps=[('key', 'C-o'), ('sleep', 1.0)], answers=[], agent_files=PROMPT_FILES, rows=40,
                                   settings={'quietStartup': False}),
    'skills_start_expanded': dict(steps=[('key', 'C-o'), ('sleep', 1.0)], answers=[], agent_files=SKILL_FILES, rows=50,
                                  settings={'quietStartup': False}),
    # a message after the lists: the blank rows between the block and the user's message
    'prompts_start_msg': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'Hello there')],
                              answers=[('sse', text_answer('msg_ps', 'Hello there, friend!', 20))], agent_files=PROMPT_FILES, rows=40,
                              settings={'quietStartup': False}),
    # quietStartup hides the lists, not the conflicts
    'prompts_conflicts': dict(steps=[], answers=[], agent_files=CONFLICT_FILES, rows=30, settings={'prompts': ['extra']}),
})


# I8/J5: /reload reads the skills and prompt templates (and the context files) again: files made while pi runs show in the lists
RELOAD_FILES = {
    'skills/greet/SKILL.md': _sk('greet', 'Greets the user by name'),
    'prompts/review.md': '---\ndescription: Review a file\n---\nReview $1.\n',
}
RELOAD_WRITES = {
    'skills/fresh/SKILL.md': _sk('fresh', 'Made while pi runs'),
    'prompts/summarize.md': 'Summarize $ARGUMENTS.\n',
    'prompts/review.md': '---\ndescription: Review changed\n---\nReview $1 again.\n',
}
SCENARIOS.update({
    'reload_resources': dict(steps=[('sleep', 0.5)] + [('write', k, v) for k, v in RELOAD_WRITES.items()] +
                             [('type', '/reload'), ('enter',), ('sleep', 2.0)],
                             answers=[], agent_files=RELOAD_FILES, rows=40, settings={'quietStartup': False}),
})


# R5: Ctrl+O (app.tools.expand) over tool boxes: the read box shows its file when expanded (renderers/read.ts:73-90), bash all of
# its output (renderers/bash.ts:60-77), ls all its rows (renderers/ls.ts:35-39); a line that is not in the session store (the
# request's error) stays; a running bash call stays live (tool-execution.ts setExpanded, interactive-mode.ts:4557-4574)
LS_FILES = {'files/f%02d.txt' % i: 'x\n' for i in range(1, 26)}
READ_ANS = [('sse', tool_answer('msg_t1', 'toolu_1', 'read', {'path': 'notes.txt'}, 'I will read it.')),
            ('sse', text_answer('msg_t2', 'Done reading notes.txt: three lines.', 20))]
SEQ_ANS = [('sse', tool_answer('msg_s1', 'toolu_5', 'bash', {'command': 'seq 1 12'})),
           ('sse', text_answer('msg_s2', 'Counted.', 20))]
LS_ANS = [('sse', tool_answer('msg_l1', 'toolu_6', 'ls', {'path': 'files'})),
          ('sse', text_answer('msg_l2', 'Listed.', 20))]
SCENARIOS.update({
    'tool_expanded': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading'), ('key', 'C-o'), ('sleep', 1.0)],
                          answers=READ_ANS),
    'dark_tool_expanded': dict(steps=[('type', 'read the file'), ('enter',), ('wait', 'Done reading'), ('key', 'C-o'), ('sleep', 1.0)],
                               answers=READ_ANS, theme='dark'),
    'dark_bash_long': dict(steps=[('type', 'count'), ('enter',), ('wait', 'Counted')], answers=SEQ_ANS, theme='dark', rows=30),
    'dark_bash_long_expanded': dict(steps=[('type', 'count'), ('enter',), ('wait', 'Counted'), ('key', 'C-o'), ('sleep', 1.0)],
                                    answers=SEQ_ANS, theme='dark', rows=30),
    'dark_ls_long': dict(steps=[('type', 'list'), ('enter',), ('wait', 'Listed')], answers=LS_ANS, theme='dark', rows=40,
                         files=LS_FILES, settings={'defaultTools': ['+ls']}),
    'dark_ls_long_expanded': dict(steps=[('type', 'list'), ('enter',), ('wait', 'Listed'), ('key', 'C-o'), ('sleep', 1.0)],
                                  answers=LS_ANS, theme='dark', rows=40, files=LS_FILES,
                                  settings={'defaultTools': ['+ls']}),
    # the request's error is a line of the chat only; Ctrl+O keeps it
    'error_expanded': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'not available'), ('key', 'C-o'), ('sleep', 1.0)],
                           answers=[('http', 400, ERR_BODY)]),
    # a `!` command's output is a line of the chat that the conversation drawn again from the store does not hold (pi keeps the component)
    'bang_expanded': dict(steps=[('type', '!echo hi'), ('enter',), ('sleep', 1.5), ('key', 'C-o'), ('sleep', 1.0)], answers=[]),
    # Ctrl+O while bash runs: the box stays live (expanded shows the output so far above the Elapsed row)
    'dark_toolrun_out_expanded': dict(steps=[('type', 'run it'), ('enter',), ('wait', 'sleep 4'), ('sleep', 1.0), ('key', 'C-o'),
                                             ('sleep', 0.1), ('capture_now',)],
                                      answers=[('sse', tool_answer('msg_r1', 'toolu_3', 'bash', {'command': 'seq 1 12; sleep 4'})),
                                               ('sse', text_answer('msg_r2', 'Slept.', 20))], theme='dark', rows=32),
})


# L5: ctx.ui.select / confirm / input / notify from an extension's command (pi's showExtensionSelector / Confirm / Input / Notify,
# interactive-mode.ts:2690-2970). The extension sits in the agent directory; each command asks one question and tells the answer
# with notify (info, warning or error by what came back), so the screen after the dialog shows pi's status, warning and error rows.
EXT_UI = r'''export default function (pi) {
  pi.registerCommand("pick", { handler: async (args, ctx) => {
    const v = await ctx.ui.select("Pick one", ["Alpha", "Beta", "Gamma"], args ? { timeout: 60000 } : undefined);
    if (v === undefined) ctx.ui.notify("nothing picked", "warning"); else ctx.ui.notify("picked " + v);
  } });
  pi.registerCommand("sure", { handler: async (args, ctx) => {
    const v = await ctx.ui.confirm("Delete it?", "This cannot be undone.");
    if (v) ctx.ui.notify("confirmed", "info"); else ctx.ui.notify("declined", "warning");
  } });
  pi.registerCommand("ask", { handler: async (args, ctx) => {
    const v = await ctx.ui.input("Your name", "a name");
    if (v === undefined) ctx.ui.notify("no name", "error"); else ctx.ui.notify("hello " + v, "info");
  } });
  pi.registerCommand("tell", { handler: async (args, ctx) => {
    ctx.ui.notify("plain note");
    ctx.ui.notify("a warning", "warning");
    ctx.ui.notify("an error", "error");
  } });
}
'''
UI_FILES = {'extensions/t.ts': EXT_UI}
SCENARIOS.update({
    'ext_ui_select': dict(steps=[('type', '/pick'), ('enter',), ('wait', 'Alpha')], answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_select_down': dict(steps=[('type', '/pick'), ('enter',), ('wait', 'Alpha'), ('key', 'Down'), ('key', 'Down')],
                               answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_select_done': dict(steps=[('type', '/pick'), ('enter',), ('wait', 'Alpha'), ('key', 'Down'), ('enter',), ('wait', 'picked')],
                               answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_select_cancel': dict(steps=[('type', '/pick'), ('enter',), ('wait', 'Alpha'), ('key', 'Escape'), ('wait', 'nothing picked')],
                                 answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_select_timeout': dict(steps=[('type', '/pick x'), ('enter',), ('wait', 'Pick one (60s)'), ('capture_now',)],
                                  answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_confirm': dict(steps=[('type', '/sure'), ('enter',), ('wait', 'Delete it')], answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_confirm_yes': dict(steps=[('type', '/sure'), ('enter',), ('wait', 'Delete it'), ('enter',), ('wait', 'confirmed')],
                               answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_confirm_no': dict(steps=[('type', '/sure'), ('enter',), ('wait', 'Delete it'), ('key', 'Down'), ('enter',), ('wait', 'declined')],
                              answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_input': dict(steps=[('type', '/ask'), ('enter',), ('wait', 'Your name'), ('type', 'Ada')],
                         answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_input_done': dict(steps=[('type', '/ask'), ('enter',), ('wait', 'Your name'), ('type', 'Ada'), ('enter',), ('wait', 'hello Ada')],
                              answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_input_cancel': dict(steps=[('type', '/ask'), ('enter',), ('wait', 'Your name'), ('type', 'Ada'), ('key', 'Escape'), ('wait', 'no name')],
                                answers=[], agent_files=UI_FILES, rows=30),
    'ext_ui_notify': dict(steps=[('type', '/tell'), ('enter',), ('wait', 'an error')], answers=[], agent_files=UI_FILES, rows=30),
})

# L11: pi.sendMessage with display true, drawn by an extension's registerMessageRenderer or by the default box (custom-message.ts); a
# message with display false is in the session only. Ctrl+O sets expanded for the renderer.
EXT_MSG = r'''import { Text } from "@earendil-works/pi-tui";
export default function (pi) {
  pi.registerMessageRenderer("note", (message, options, theme) => {
    const body = typeof message.content === "string" ? message.content : "";
    return new Text(theme.fg("accent", "Note: " + body) + (options.expanded ? " (expanded)" : ""), 1, 0);
  });
  pi.registerCommand("note", { handler: async () => { pi.sendMessage({ customType: "note", content: "hello there", display: true }); } });
  pi.registerCommand("plain", { handler: async () => { pi.sendMessage({ customType: "plain", content: "some **bold** text", display: true }); } });
  pi.registerCommand("quiet", { handler: async () => { pi.sendMessage({ customType: "quiet", content: "hidden", display: false }); } });
  pi.registerCommand("multi", { handler: async () => { pi.sendMessage({ customType: "multi", content: "one", display: true }); pi.sendMessage({ customType: "note", content: "two", display: true }); } });
}
'''
MSG_FILES = {'extensions/t.ts': EXT_MSG}
SCENARIOS.update({
    'ext_msg_render': dict(steps=[('type', '/note'), ('enter',), ('wait', 'Note:')], answers=[], agent_files=MSG_FILES, rows=30),
    'ext_msg_plain': dict(steps=[('type', '/plain'), ('enter',), ('wait', 'bold')], answers=[], agent_files=MSG_FILES, rows=30),
    'ext_msg_hidden': dict(steps=[('type', '/quiet'), ('enter',), ('sleep', 1.0), ('type', '/note'), ('enter',), ('wait', 'Note:')],
                           answers=[], agent_files=MSG_FILES, rows=30),
    'ext_msg_expanded': dict(steps=[('type', '/note'), ('enter',), ('wait', 'Note:'), ('key', 'C-o'), ('sleep', 1.0)],
                             answers=[], agent_files=MSG_FILES, rows=30),
    'ext_msg_multi': dict(steps=[('type', '/multi'), ('enter',), ('wait', 'two')], answers=[], agent_files=MSG_FILES, rows=30),
})

# L16: ctx.ui.setStatus (the footer's extra row), setWidget (lines above or below the editor) and setWorkingMessage from an extension's command
EXT_UIX = r'''export default function (pi) {
  pi.registerCommand("stat", { handler: async (args, ctx) => { ctx.ui.setStatus("b", "second"); ctx.ui.setStatus("a", "first"); } });
  pi.registerCommand("above", { handler: async (args, ctx) => { ctx.ui.setWidget("w", ["widget line one", "widget line two"]); } });
  pi.registerCommand("below", { handler: async (args, ctx) => { ctx.ui.setWidget("wb", ["under the editor"], { placement: "belowEditor" }); } });
  pi.registerCommand("many", { handler: async (args, ctx) => { ctx.ui.setWidget("m", Array.from({ length: 12 }, (_, i) => "row " + (i + 1))); } });
  pi.registerCommand("clear", { handler: async (args, ctx) => { ctx.ui.setStatus("a", undefined); ctx.ui.setStatus("b", undefined); ctx.ui.setWidget("w", undefined); ctx.ui.setWidget("wb", undefined); } });
}
'''
UIX_FILES = {'extensions/t.ts': EXT_UIX}
SCENARIOS.update({
    'ext_ui_status': dict(steps=[('type', '/stat'), ('enter',), ('wait', 'first')], answers=[], agent_files=UIX_FILES, rows=30),
    'ext_ui_widget_above': dict(steps=[('type', '/above'), ('enter',), ('wait', 'widget line')], answers=[], agent_files=UIX_FILES, rows=30),
    'ext_ui_widget_below': dict(steps=[('type', '/below'), ('enter',), ('wait', 'under the')], answers=[], agent_files=UIX_FILES, rows=30),
    'ext_ui_widget_many': dict(steps=[('type', '/many'), ('enter',), ('wait', 'truncated')], answers=[], agent_files=UIX_FILES, rows=30),
    'ext_ui_widget_clear': dict(steps=[('type', '/above'), ('enter',), ('wait', 'widget line'), ('type', '/stat'), ('enter',), ('wait', 'first'),
                                       ('type', '/clear'), ('enter',), ('sleep', 1.0)], answers=[], agent_files=UIX_FILES, rows=30),
})




# G2 (U1b): pi's footer at the widths where its rules differ (footer.ts:177-249): the usage "left" is 33 columns after one answer,
# the model "fake-model" 10. 45 columns: both fit with the two spaces of padding (flush to the last column); 41: the model is cut to
# the room left ("fake-m"), never the usage; 30: the usage alone is wider than the window and is cut with "..."
FOOT_ANS = [('sse', text_answer('msg_a', 'answer one', 20))]
FOOT_STEPS = [('type', 'first'), ('enter',), ('wait', 'one')]
SCENARIOS.update({
    'foot_exact': dict(steps=FOOT_STEPS, answers=FOOT_ANS, cols=45),
    'foot_cut': dict(steps=FOOT_STEPS, answers=FOOT_ANS, cols=41),
    'foot_left': dict(steps=FOOT_STEPS, answers=FOOT_ANS, cols=30),
})


# U1b: showStatus (interactive-mode.ts:3824-3844) puts a new dim status into the last one's place when it is the chat's last child:
# Alt+Up twice with nothing queued ("No queued messages to restore") is one row, not two
SCENARIOS.update({
    'status_twice': dict(steps=[('key', 'M-Up'), ('sleep', 0.5), ('key', 'M-Up'), ('sleep', 0.8)], answers=[]),
})


# G3 (U1b): the start header at 64 columns, where the hint line wraps, with outputPad 1 and 0. The header is a BuiltInHeader(.., 1, 0)
# (interactive-mode.ts:1055-1075): its own padding of one column, whatever outputPad says (settings.md:100); the wrapped part of the
# hint starts under the logo, in that padding column, not in the text column
SCENARIOS.update({
    'start_w64': dict(steps=[], answers=[], cols=64),
    'pad0_start_w64': dict(steps=[], answers=[], cols=64, settings={'outputPad': 0}),
})


# U3 (U1b): an invalid settings.json is reported, not ignored: main.ts:670 collects settingsManager.drainErrors() into startup
# diagnostics, interactive mode shows each as a warning (interactive-mode.ts:1197-1204) -- "Invalid settings file <path>: <JSON.parse
# message>" (core/settings-diagnostics.ts); the settings it could not read count as empty. quietStartup stays off (the file is the
# one that is broken), so the header is up.
SCENARIOS.update({
    'settings_bad': dict(steps=[], answers=[], rows=30, settings_raw='"outputPad": 0\n'),
})


# U1b: Ctrl+O at the start shows the header's long form (expandedInstructions, interactive-mode.ts:1036-1057): one key hint a row, and the
# hint line of the collapsed form ("Press ctrl+o ...") gives way to it; quietStartup "header" keeps the header but not the loaded resources
SCENARIOS.update({
    'start_expanded': dict(steps=[('key', 'C-o'), ('sleep', 1.0)], answers=[], rows=40),
    'start_expanded_w64': dict(steps=[('key', 'C-o'), ('sleep', 1.0)], answers=[], rows=40, cols=64),
    'start_header_only': dict(steps=[], answers=[], settings={'quietStartup': 'header'}),
})


# U1b (tool boxes): read highlights a file of a known language when expanded (renderers/read.ts:121-136), shows a skill, a context file
# or a pi docs file in its short form while collapsed (read.ts:22-111, 155-158); write shows its content under the call, ten lines
# collapsed (renderers/write.ts:96-127); edit shows the diff under the call, in the box coloured by the preview (renderers/edit.ts);
# the result's details warn of truncation (read.ts:138-147, ls.ts:48-55)
CODE_C = ('/* a greeting */\nstatic const char *name = "world";\nunsigned long n = 42;\nif (n > 1) {\n\tname = "again";\n} else {\n\tn = 0x1f; // hex\n}\n'
          'while (n) {\n\tn--;\n}\nreturn 0;\n')
CODE_PY = '\n'.join(['import os', 'import sys', '', '# count the lines', 'total = 0', 'for line in sys.stdin:', '    if line:', '        total += 1',
                    '    else:', '        total -= 1', 'while total > 3:', '    total = total - 1', 'name = "box"', 'size = 3.5',
                    'flag = total and not size', 'other = size or 2', 'result = total if flag else 0']) + '\n'
SKILL_MD = '---\nname: greet\ndescription: Greets people.\n---\n\n# Greet\n\nSay hello.\n'
R_ANS = lambda name, path, extra=None: [('sse', tool_answer('msg_h1', 'toolu_h', name, dict({'path': path}, **(extra or {})), 'Looking.')),
                                         ('sse', text_answer('msg_h2', 'Finished.', 20))]
SCENARIOS.update({
    'dark_read_hl': dict(steps=[('type', 'show main'), ('enter',), ('wait', 'Finished'), ('key', 'C-o'), ('sleep', 1.0)],
                         answers=R_ANS('read', 'main.c'), theme='dark', files={'main.c': CODE_C}, rows=30),
    'dark_read_hl_py': dict(steps=[('type', 'show it'), ('enter',), ('wait', 'Finished'), ('key', 'C-o'), ('sleep', 1.0)],
                            answers=R_ANS('read', 'count.py'), theme='dark', files={'count.py': CODE_PY}, rows=30),
    'dark_read_skill': dict(steps=[('type', 'read'), ('enter',), ('wait', 'Done compact')], theme='dark', rows=30,
                            files={'skills/greet/SKILL.md': SKILL_MD, 'docs/AGENTS.md': 'Be brief.\n'},
                            answers=[('sse', tool_answer2('msg_k1', [('toolu_k1', 'read', {'path': 'skills/greet/SKILL.md'}),
                                                                     ('toolu_k2', 'read', {'path': 'docs/AGENTS.md', 'offset': 1, 'limit': 5}),
                                                                     ('toolu_k3', 'read', {'path': 'notes.txt'})])),
                                     ('sse', text_answer('msg_k2', 'Done compact.', 20))]),
    'dark_read_skill_expanded': dict(steps=[('type', 'read'), ('enter',), ('wait', 'Done compact'), ('key', 'C-o'), ('sleep', 1.0)],
                                     theme='dark', rows=40, files={'skills/greet/SKILL.md': SKILL_MD, 'docs/AGENTS.md': 'Be brief.\n'},
                                     answers=[('sse', tool_answer2('msg_k1', [('toolu_k1', 'read', {'path': 'skills/greet/SKILL.md'}),
                                                                              ('toolu_k2', 'read', {'path': 'docs/AGENTS.md', 'offset': 1, 'limit': 5})])),
                                              ('sse', text_answer('msg_k2', 'Done compact.', 20))]),
    'dark_write': dict(steps=[('type', 'write it'), ('enter',), ('wait', 'Finished')], theme='dark', rows=40,
                       answers=[('sse', tool_answer('msg_w1', 'toolu_w', 'write', {'path': 'count.py', 'content': CODE_PY}, 'Writing.')),
                                ('sse', text_answer('msg_w2', 'Finished.', 20))]),
    'dark_write_expanded': dict(steps=[('type', 'write it'), ('enter',), ('wait', 'Finished'), ('key', 'C-o'), ('sleep', 1.0)], theme='dark',
                                rows=40,
                                answers=[('sse', tool_answer('msg_w1', 'toolu_w', 'write', {'path': 'count.py', 'content': CODE_PY}, 'Writing.')),
                                         ('sse', text_answer('msg_w2', 'Finished.', 20))]),
    'dark_write_txt': dict(steps=[('type', 'write it'), ('enter',), ('wait', 'Finished')], theme='dark', rows=30,
                           answers=[('sse', tool_answer('msg_w1', 'toolu_w', 'write', {'path': 'out.txt', 'content': 'one\ntwo\n'}, 'Writing.')),
                                    ('sse', text_answer('msg_w2', 'Finished.', 20))]),
    'dark_edit': dict(steps=[('type', 'edit it'), ('enter',), ('wait', 'Finished')], theme='dark', rows=30,
                      files={'count.py': CODE_PY},
                      answers=[('sse', tool_answer('msg_e1', 'toolu_e', 'edit',
                                                   {'path': 'count.py', 'edits': [{'oldText': 'total = 0\n', 'newText': 'total = 1\n'},
                                                                                  {'oldText': 'name = "box"', 'newText': 'name = "crate"\n# done'}]},
                                                   'Editing.')),
                               ('sse', text_answer('msg_e2', 'Finished.', 20))]),
    'dark_edit_err': dict(steps=[('type', 'edit it'), ('enter',), ('wait', 'Finished')], theme='dark', rows=30,
                          files={'count.py': CODE_PY},
                          answers=[('sse', tool_answer('msg_e1', 'toolu_e', 'edit',
                                                       {'path': 'count.py', 'edits': [{'oldText': 'no such text', 'newText': 'x'}]}, 'Editing.')),
                                   ('sse', text_answer('msg_e2', 'Finished.', 20))]),
    'dark_read_trunc_expanded': dict(steps=[('type', 'read big'), ('enter',), ('wait', 'Finished'), ('key', 'C-o'), ('sleep', 1.5)], theme='dark',
                                     rows=30, files={'big.txt': ''.join('line %d\n' % i for i in range(1, 2501))},
                                     answers=R_ANS('read', 'big.txt')),
    'dark_ls_limit': dict(steps=[('type', 'list'), ('enter',), ('wait', 'Finished')], theme='dark', rows=30, files=LS_FILES,
                          settings={'defaultTools': ['+ls']}, answers=R_ANS('ls', 'files', {'limit': 3})),
})

class Fake(http.server.BaseHTTPRequestHandler):
    answers = []
    served = []
    lock = threading.Lock()

    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get('content-length') or 0)
        body = self.rfile.read(n)
        with Fake.lock:
            Fake.served.append(body)
            a = Fake.answers.pop(0) if Fake.answers else ('http', 500, '{"type":"error","error":{"type":"api_error","message":"fixture out of answers"}}')
        if a[0] == 'slow':          # the answer after a pause (the spinner shows meanwhile)
            time.sleep(a[1])
            a = ('sse', a[2])
        if a[0] == 'http':
            self.send_response(a[1])
            self.send_header('content-type', 'application/json')
            self.send_header('content-length', str(len(a[2])))
            self.end_headers()
            self.wfile.write(a[2].encode())
            return
        self.send_response(200)
        self.send_header('content-type', 'text/event-stream')
        self.end_headers()
        self.wfile.write(a[1].encode())
        self.wfile.flush()
        self.close_connection = True


def free_port():
    import socket
    sk = socket.socket()
    sk.bind(('127.0.0.1', 0))
    port = sk.getsockname()[1]
    sk.close()
    return port


def tmux(*a):
    return subprocess.run(['tmux'] + list(a), capture_output=True, text=True).stdout


PAD = False         # -N: trailing blanks kept (tmux trims them, backgrounds with them: a themed golden needs the cells)


HIST = False        # -S -: the history too (a pane that kept its last output scrolls by the "Pane is dead" line)


def capture(sess, esc):
    args = ['capture-pane', '-t', sess, '-p']
    if HIST:
        args += ['-S', '-']
    if esc:
        args.insert(1, '-e')
        if PAD:
            args.insert(1, '-N')
    return tmux(*args)


def wait_text(sess, text, secs=20):
    end = time.time() + secs
    while time.time() < end:
        if text in capture(sess, False):
            return True
        time.sleep(0.2)
    return False


def settle(sess, secs=1.2):
    last = None
    same = 0
    end = time.time() + 15
    while time.time() < end:
        cur = capture(sess, True)
        same = same + 1 if cur == last else 0
        if same >= int(secs / 0.3):
            return cur
        last = cur
        time.sleep(0.3)
    return last


def run(name, sc, cols, rows, keep):
    global PAD
    PAD = sc.get('theme', 'system') != 'system'
    global HIST
    HIST = False
    rows = sc.get('rows', rows)
    cols = sc.get('cols', cols)         # a scenario at its own width (U1b: the footer and header at 64 columns)
    work = '/private/tmp/' + TAG      # the trust prompt and the footer show the path; PITUI_TAG gives a run its own directory and tmux sessions
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    agent = os.path.join(work, 'agent')
    proj = os.path.join(work, 'project')
    os.makedirs(os.path.join(agent, 'bin'))
    for tool in ('fd', 'rg', 'open', 'xdg-open'):      # pi looks for fd and rg in its own bin first; stubs keep it from warning, and a login's
        #                                          sign-in URL from opening a browser (pi's openBrowser runs `open`)
        t = os.path.join(agent, 'bin', tool)
        open(t, 'w').write('#!/bin/sh\nexit 0\n')
        os.chmod(t, 0o755)
    os.makedirs(proj)
    with open(os.path.join(proj, 'notes.txt'), 'w') as f:
        f.write('one\ntwo\nthree\n')
    for seed in sc.get('seed', []):      # U2: sessions pi lists for -r
        cwdname, stem, first, ts, sname = seed[:5]
        parent_stem = seed[5] if len(seed) > 5 else None
        scwd = proj if not cwdname else os.path.join(work, cwdname)
        os.makedirs(scwd, exist_ok=True)
        sdir_ = os.path.join(agent, 'sessions', '--' + scwd.lstrip('/').replace('/', '-') + '--')
        os.makedirs(sdir_, exist_ok=True)
        t0 = int(time.mktime(time.strptime(ts[:19], '%Y-%m-%dT%H:%M:%S'))) * 1000
        sid = '0199aa00-0000-7000-8000-' + stem[-8:] * 1 + '0000'
        lines = [{'type': 'session', 'version': 3, 'id': sid, 'timestamp': ts, 'cwd': scwd},
                 {'type': 'message', 'id': 'u0000001', 'parentId': None, 'timestamp': ts,
                  'message': {'role': 'user', 'content': first, 'timestamp': t0}},
                 {'type': 'message', 'id': 'a0000001', 'parentId': 'u0000001', 'timestamp': ts,
                  'message': {'role': 'assistant', 'content': [{'type': 'text', 'text': 'ok'}], 'api': 'anthropic-messages',
                              'provider': 'fake', 'model': MODEL, 'usage': {'input': 1, 'output': 1, 'cacheRead': 0, 'cacheWrite': 0,
                              'totalTokens': 2, 'cost': {'input': 0, 'output': 0, 'cacheRead': 0, 'cacheWrite': 0, 'total': 0}},
                              'stopReason': 'stop', 'timestamp': t0 + 1000}}]
        if parent_stem:
            lines[0]['parentSession'] = os.path.join(sdir_, parent_stem + '.jsonl')
        if sname:
            lines.append({'type': 'session_info', 'id': 'n0000001', 'parentId': 'a0000001', 'timestamp': ts, 'name': sname})
        with open(os.path.join(sdir_, stem + '.jsonl'), 'w') as f:
            f.write(''.join(json.dumps(l, separators=(',', ':')) + '\n' for l in lines))
    if sc.get('session'):       # a session file to open (the tree view needs a conversation with branches)
        own = os.path.join(OUT, sc['session'])       # a fixture of this directory first (took_session.jsonl), else tests/pi/sessions
        txt = open(own if os.path.exists(own) else os.path.join(HERE, '..', '..', 'tests', 'pi', 'sessions', sc['session'])).read()
        open(os.path.join(proj, 'session.jsonl'), 'w').write(txt.replace('/path/to/project', proj))   # else pi asks about the folder
    if sc.get('agent_tree'):    # J: a tree copied into the agent directory (prompts/*.md of the templates scenarios)
        shutil.copytree(os.path.join(OUT, sc['agent_tree']), agent, dirs_exist_ok=True)
    for rel, text in sc.get('files', {}).items():
        os.makedirs(os.path.dirname(os.path.join(proj, rel)), exist_ok=True)
        open(os.path.join(proj, rel), 'w').write(text)
    for rel, text in sc.get('agent_files', {}).items():      # the agent directory (skills/, ...)
        os.makedirs(os.path.dirname(os.path.join(agent, rel)), exist_ok=True)
        open(os.path.join(agent, rel), 'w').write(text)
    Fake.answers = [a for a in sc['answers']]
    Fake.served = []
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fake)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    json.dump({'providers': {'fake': {'baseUrl': 'http://127.0.0.1:%d' % port, 'api': 'anthropic-messages', 'apiKey': 'k',
              'models': [{'id': MODEL, 'name': MODEL, 'reasoning': sc.get('reasoning', False), 'input': ['text'], 'contextWindow': 200000,
                          'maxTokens': 8192, 'cost': {'input': 3, 'output': 15, 'cacheRead': 0.3, 'cacheWrite': 3.75}}] +
                         [{'id': mid, 'name': mname, 'reasoning': False, 'input': ['text'], 'contextWindow': 100000,
                           'maxTokens': 4096, 'cost': {'input': 1, 'output': 2, 'cacheRead': 0, 'cacheWrite': 0}}
                          for mid, mname in sc.get('models', [])]}}},
              open(os.path.join(agent, 'models.json'), 'w'))
    if sc.get('ollama'):      # amiga-pi lists its built-in local ollama model with no key; pi gets the same model from models.json
        mj = json.load(open(os.path.join(agent, 'models.json')))
        oll = {'baseUrl': 'http://127.0.0.1:11434/v1', 'api': 'openai-completions', 'apiKey': 'ollama',
               'models': [{'id': 'qwen2.5-coder:7b', 'name': 'qwen2.5-coder:7b', 'reasoning': False, 'input': ['text'],
                           'contextWindow': 32768, 'maxTokens': 4096, 'cost': {'input': 0, 'output': 0, 'cacheRead': 0, 'cacheWrite': 0}}]}
        if sc['ollama'] == 'first':       # pi lists models.json's providers in file order; amiga-pi's built-in ollama is the first provider of its registry
            mj['providers'] = dict([('ollama', oll)] + list(mj['providers'].items()))
        else:
            mj['providers']['ollama'] = oll
        json.dump(mj, open(os.path.join(agent, 'models.json'), 'w'))
    # quietStartup hides the header: only the scenarios about the header show it (the rest would scroll by its height)
    settings = {'defaultProvider': 'fake', 'defaultModel': MODEL, 'defaultThinkingLevel': 'off',
                'retry': {'enabled': False}, 'quietStartup': name not in ('start', 'reg_start', 'dark_start', 'skills_start', 'start_w64', 'pad0_start_w64', 'settings_bad', 'start_expanded', 'start_header_only', 'start_expanded_w64'),
                'tuiMode': sc.get('tui', 'fullscreen'), 'theme': sc.get('theme', 'system')}
    settings.update(sc.get('settings', {}))
    json.dump(settings, open(os.path.join(agent, 'settings.json'), 'w'))
    if 'settings_raw' in sc:        # a settings.json that is no JSON (U3): its text as it is; the project's too ('project_settings_raw')
        open(os.path.join(agent, 'settings.json'), 'w').write(sc['settings_raw'])
    if 'project_settings_raw' in sc:
        os.makedirs(os.path.join(proj, '.pi'), exist_ok=True)
        open(os.path.join(proj, '.pi', 'settings.json'), 'w').write(sc['project_settings_raw'])
    fixture = None
    if sc.get('oauth'):         # an OAuth server for the login scenarios: claude_fixture.py --oauth, pi's fetch sent there (fetch_shim.mjs)
        odir = os.path.join(work, 'oauth')
        os.makedirs(odir)
        json.dump({'responses': sc['oauth']}, open(os.path.join(odir, 'script.json'), 'w'))
        fport = free_port()
        vt = os.environ.get('VTCON_DIR') or next(p for p in (os.path.join(HERE, r, '..', 'vtcon') for r in ('../..', '../../../..'))
                                                  if os.path.exists(os.path.join(p, 'tools', 'claude_fixture.py')))
        fixture = subprocess.Popen([sys.executable, '-I', os.path.join(vt, 'tools', 'claude_fixture.py'), '--port', str(fport), '--oauth', odir],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
    sess = TAG + '-' + name
    tmux('kill-session', '-t', sess)
    env = ('env -i HOME=%s PATH=%s TERM=xterm-256color LANG=en_US.UTF-8 PI_CODING_AGENT_DIR=%s PI_OFFLINE=1 '
           'PI_SKIP_VERSION_CHECK=1 PI_TELEMETRY=0 ' % (work, os.path.join(agent, 'bin') + ':' + os.environ['PATH'], agent))
    rawlog = os.path.join(work, 'raw.log')
    if sc.get('raw'):
        env = env.replace('env -i ', 'env -i PI_TUI_WRITE_LOG=%s ' % rawlog)
    if sc.get('env'):
        env = env.replace('env -i ', 'env -i %s ' % sc['env'])
    if sc.get('changelog'):     # pi's package directory with a CHANGELOG.md of ours: symlinks to the install, the file written
        real = os.path.dirname(os.path.dirname(os.path.realpath(PI)))
        while real != '/' and not os.path.exists(os.path.join(real, 'CHANGELOG.md')):
            real = os.path.dirname(real)
        pkgd = os.path.join(work, 'pkg')
        os.makedirs(pkgd)
        for ent in os.listdir(real):
            if ent != 'CHANGELOG.md':
                os.symlink(os.path.join(real, ent), os.path.join(pkgd, ent))
        open(os.path.join(pkgd, 'CHANGELOG.md'), 'w').write(CHANGELOG_FIXTURE)
        env = env.replace('env -i ', 'env -i PI_PACKAGE_DIR=%s ' % pkgd)
    if fixture:
        # (HTTPS_PROXY to a closed port: a request the shim misses fails here instead of reaching the real server)
        env = env.replace('env -i ', "env -i PI_FIXTURE=http://127.0.0.1:%d NODE_OPTIONS='--import %s' HTTPS_PROXY=http://127.0.0.1:1 NO_PROXY=127.0.0.1 "
                          % (fport, os.path.join(HERE, 'fetch_shim.mjs')))
    pipelog = os.path.join(work, 'pty.bytes')
    tmux('new-session', '-d', '-s', sess, '-x', str(cols), '-y', str(rows), '-c', proj,
         env + (PI + ' ' + sc['argv'] if 'argv' in sc else PI + (" --session-dir '%s'" % os.path.join(work, sc['sdir']) if sc.get('sdir') else '') + (' --session session.jsonl' if sc.get('session') else ' -r' if sc.get('resume') else '' if sc.get('persist') else ' --no-session')))
    tmux('set-option', '-t', sess, 'remain-on-exit', 'on')
    if sc.get('pipe'):
        tmux('pipe-pane', '-t', sess, 'cat > %s' % pipelog)
    try:
        wait_text(sess, sc['ready'] if 'ready' in sc else 'Resume Session' if sc.get('resume') else MODEL, 30)       # the footer names the model once pi is up
        time.sleep(4.0 if sc.get('session') else 1.0)     # a session is loaded after that
        shots = {}
        for st in sc['steps']:
            if st[0] == 'type':
                tmux('send-keys', '-t', sess, '-l', st[1])
            elif st[0] == 'enter':
                tmux('send-keys', '-t', sess, 'Enter')
            elif st[0] == 'key':
                tmux('send-keys', '-t', sess, st[1])
            elif st[0] == 'write':      # a file of the agent directory made or changed while pi runs
                os.makedirs(os.path.dirname(os.path.join(agent, st[1])), exist_ok=True)
                open(os.path.join(agent, st[1]), 'w').write(st[2])
            elif st[0] == 'sleep':
                time.sleep(st[1])
            elif st[0] == 'exit':       # Ctrl+D on the empty editor ends pi; its last output stays in the pane
                alt = tmux('display-message', '-t', sess, '-p', '#{alternate_on}').strip()
                tmux('send-keys', '-t', sess, 'C-d')
                end = time.time() + 15
                while time.time() < end and tmux('display-message', '-t', sess, '-p', '#{pane_dead}').strip() != '1':
                    time.sleep(0.2)
                time.sleep(0.5)
                shots['alt'] = alt
            elif st[0] == 'ended':      # pi left by itself (an Escape in -r's selector)
                end = time.time() + 15
                while time.time() < end and tmux('display-message', '-t', sess, '-p', '#{pane_dead}').strip() != '1':
                    time.sleep(0.2)
                time.sleep(0.5)
            elif st[0] == 'capture_now':
                pane = capture(sess, True)
                shots['now'] = pane
            elif st[0] == 'wait':
                if not wait_text(sess, st[1]):
                    print('[ERROR] %s: never saw %r' % (name, st[1]), file=sys.stderr)
            time.sleep(0.3)
        if 'alt' not in shots:
            shots['alt'] = tmux('display-message', '-t', sess, '-p', '#{alternate_on}').strip()
        if any(st[0] in ('exit', 'ended') for st in sc['steps']):
            HIST = True       # what pi left on the main screen: its rows without the dead pane's notice and the blank rest
        pane = shots.get('now') or settle(sess, 1.5)
        if HIST:
            keep = lambda t: '\n'.join(l for l in t.split('\n') if not l.startswith('Pane is dead')).rstrip('\n ') + '\n'
            pane = keep(pane)
        with open(os.path.join(OUT, name + '.golden'), 'w') as f:
            f.write(pane)
        with open(os.path.join(OUT, name + '.txt'), 'w') as f:
            f.write(keep(capture(sess, False)) if HIST else capture(sess, False))
        with open(os.path.join(OUT, name + '.mode'), 'w') as f:      # what tmux saw before pi left
            f.write('pi=%s tuiMode=%s theme=%s alternate_screen=%s\n' % (PI_VERSION, sc.get('tui', 'fullscreen'), sc.get('theme', 'system'), shots['alt']))
        if sc.get('changelog') or sc.get('settings_out'):      # what pi wrote to settings.json (lastChangelogVersion, the selectors' changes), sorted
            with open(os.path.join(OUT, name + '.settings'), 'w') as f:
                f.write(json.dumps(json.load(open(os.path.join(agent, 'settings.json'))), sort_keys=True, indent=1) + '\n')
        if sc.get('pipe'):
            shutil.copyfile(pipelog, os.path.join(OUT, name + '.bytes'))
        if sc.get('raw'):
            shutil.copyfile(rawlog, os.path.join(OUT, name + '.raw'))
        for i, a in enumerate(sc.get('host', [])):
            with open(os.path.join(OUT, '%s.host.%d.sse' % (name, i + 1)), 'w') as f:
                f.write(a)
        for i, a in enumerate(sc['answers']):
            if a[0] in ('sse', 'slow'):
                with open(os.path.join(OUT, '%s.%d.sse' % (name, i + 1)), 'w') as f:
                    f.write(a[-1])
            else:       # a whole HTTP answer, as the stub plays it
                with open(os.path.join(OUT, '%s.%d.http' % (name, i + 1)), 'w') as f:
                    f.write('HTTP/1.1 %d X\r\ncontent-type: application/json\r\ncontent-length: %d\r\n\r\n%s' % (a[1], len(a[2]), a[2]))
        print('[OK] %s' % name)
    finally:
        if not keep:
            tmux('kill-session', '-t', sess)
            shutil.rmtree(work, ignore_errors=True)
        if fixture:
            fixture.terminate()
        srv.shutdown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only')
    ap.add_argument('--cols', type=int, default=80)
    ap.add_argument('--rows', type=int, default=25)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--stale', action='store_true', help='only scenarios whose <name>.mode does not name the pinned pi (resumable)')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    # the skills of the skills_* scenarios, for tests/test_pi_tui.c to put in its own agent directory
    json.dump(SKILL_FILES, open(os.path.join(OUT, 'skills_files.json'), 'w'), indent=1)
    json.dump(PROMPT_FILES, open(os.path.join(OUT, 'prompts_files.json'), 'w'), indent=1)
    json.dump(CONFIG_FILES, open(os.path.join(OUT, 'config_files.json'), 'w'), indent=1)
    json.dump(CONFLICT_FILES, open(os.path.join(OUT, 'conflict_files.json'), 'w'), indent=1)
    open(os.path.join(OUT, 'changelog_fixture.md'), 'w').write(CHANGELOG_FIXTURE)
    json.dump(RELOAD_FILES, open(os.path.join(OUT, 'reload_files.json'), 'w'), indent=1)
    json.dump(RELOAD_WRITES, open(os.path.join(OUT, 'reload_writes.json'), 'w'), indent=1)
    # the project files of the scenarios that have some (the tool scenarios of tests/test_pi_tui.c run amiga-pi's own tools on them)
    json.dump({n: sc['files'] for n, sc in SCENARIOS.items() if 'files' in sc}, open(os.path.join(OUT, 'scenario_files.json'), 'w'), indent=1)
    for n, sc in SCENARIOS.items():
        if a.only and a.only != n:
            continue
        if a.stale:
            try:
                if open(os.path.join(OUT, n + '.mode')).readline().split()[0] == 'pi=' + PI_VERSION:
                    continue
            except OSError:
                pass
        run(n, sc, a.cols, a.rows, a.keep)


if __name__ == '__main__':
    main()
