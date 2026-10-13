#!/usr/bin/env python3
"""rpc_gen -- the goldens of tests/pi/rpc (plan H): real pi 1.1.0 (tui_gen.find_pi) in `--mode json` and `--mode rpc`,
against a local fake Messages API (scripted answers, no key, no cost; the helpers of tui_gen.py),
its stdout kept as pi wrote it.

  python3 tools/pi_oracle/rpc_gen.py [--only <scenario>] [--keep]

Per scenario <name>, in tests/pi/rpc:
  <name>.out        pi's stdout, every line as written (the golden; masked only when compared)
  <name>.in         the commands sent (rpc), one JSON line each; a line `#after <event type>` holds the
                    next command back until pi's output holds that event (a command typed during a run)
  <name>.cfg.json   the scenario's environment (mode, arguments, model, settings, session file, extra files, answers)
  <name>.N.sse      the N-th answer the fake served (the host test streams the same bytes to amiga-pi)
  models.json       the agent directory's models.json (the host test writes the same one)
Scenarios run in a throw-away PI_CODING_AGENT_DIR; environment pinned: PI_OFFLINE=1,
PI_SKIP_VERSION_CHECK=1, PI_TELEMETRY=0, fixed work directory /private/tmp/pirpc.
"""
import argparse, http.server, json, os, shutil, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tui_gen as T                                    # the fake server and the SSE builders (N0)

OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'tests', 'pi', 'rpc'))
SESSIONS = os.path.normpath(os.path.join(HERE, '..', '..', 'tests', 'pi', 'sessions'))
WORK = '/private/tmp/pirpc'

MODELS = [
    {'id': 'fake-model', 'name': 'fake-model', 'reasoning': False, 'input': ['text'], 'contextWindow': 200000,
     'maxTokens': 8192, 'cost': {'input': 3, 'output': 15, 'cacheRead': 0.3, 'cacheWrite': 3.75}},
    {'id': 'fake-think', 'name': 'fake-think', 'reasoning': True, 'input': ['text', 'image'], 'contextWindow': 100000,
     'maxTokens': 4096, 'cost': {'input': 1, 'output': 5, 'cacheRead': 0.1, 'cacheWrite': 1.25}},
]


def A(text, n=20):
    return ('sse', T.text_answer('msg_a', text, n))


def TOOL(name, args, lead=None, tid='toolu_1'):
    return ('sse', T.tool_answer('msg_t', tid, name, args, lead))


def SLOW(secs, answer):
    return ('slow', secs, answer[1])


# the extension of the UI scenarios: the four methods of rpc-extension-ui.md on a tool call and a registered command
EXT_UI = r'''
export default function (pi) {
  pi.on("tool_call", async (event, ctx) => {
    const choice = await ctx.ui.select("Run it?", ["Allow", "Block"]);
    const ok = await ctx.ui.confirm("Sure?", "It runs a command.");
    const why = await ctx.ui.input("Why?", "a reason");
    ctx.ui.notify("decided " + choice + " " + ok + " " + why, "info");
    if (choice !== "Allow") return { block: true, reason: "Blocked by the user" };
  });
}
'''

# step kinds: ('cmd', obj) sends obj and waits for its response; ('prompt', text) a prompt and waits for agent_settled;
# ('gate', event) waits for that event in the output (and marks the .in); ('settle',) waits for agent_settled;
# ('send', obj) sends and waits for nothing; ('raw', text) a line that is not JSON; ('ui', value-dict) answers the
# last extension_ui_request; ('sleep', s)
def P(text, **kw):
    d = {'type': 'prompt', 'message': text}
    d.update(kw)
    return d


SC = {}


def sc(name, **kw):
    SC[name] = kw


# ---- JSON mode ----
sc('json_text', mode='json', args=['hello'], answers=[A('Hello from the fixture.')])
sc('json_tool', mode='json', args=['read the file'],
   answers=[TOOL('read', {'path': 'notes.txt'}, 'I will read it.'), A('Done reading notes.txt.')])
sc('json_error', mode='json', args=['hello'], answers=[('http', 400, T.ERR_BODY)])
sc('json_think', mode='json', args=['think about it'], model='fake-think', thinking='high',
   answers=[('sse', T.sse([
       T.msg_start('msg_th'),
       {'type': 'content_block_start', 'index': 0, 'content_block': {'type': 'thinking', 'thinking': '', 'signature': ''}},
       {'type': 'content_block_delta', 'index': 0, 'delta': {'type': 'thinking_delta', 'thinking': 'Let me think.'}},
       {'type': 'content_block_delta', 'index': 0, 'delta': {'type': 'signature_delta', 'signature': 'sig1'}},
       {'type': 'content_block_stop', 'index': 0},
       {'type': 'content_block_start', 'index': 1, 'content_block': {'type': 'text', 'text': ''}},
       {'type': 'content_block_delta', 'index': 1, 'delta': {'type': 'text_delta', 'text': 'Thought.'}},
       {'type': 'content_block_stop', 'index': 1},
       {'type': 'message_delta', 'delta': {'stop_reason': 'end_turn', 'stop_sequence': None}, 'usage': {'output_tokens': 30}},
       {'type': 'message_stop'}]))])
sc('json_retry', mode='json', args=['hello'], retry=True,
   answers=[('http', 529, json.dumps({'type': 'error', 'error': {'type': 'overloaded_error', 'message': 'Overloaded'}})),
            A('Recovered.')])

# ---- RPC: prompting ----
sc('prompt', steps=[('prompt', 'hello')], answers=[A('Hello from the fixture.')])
sc('prompt_tool', steps=[('prompt', 'read the file')],
   answers=[TOOL('read', {'path': 'notes.txt'}, 'I will read it.'), A('Done reading notes.txt.')])
# the agent's own bash call: pi's tool_execution_update records (an empty one at the start, the output after)
sc('prompt_bash', steps=[('prompt', 'run it')],
   answers=[TOOL('bash', {'command': 'echo one; echo two'}, 'I will run it.'), A('Ran it.')])
# R1: a command that writes in two pieces 0.6 s apart: an update per chunk (bash.ts handleData, throttled to 100 ms)
sc('prompt_bash_chunks', steps=[('prompt', 'run it')],
   answers=[TOOL('bash', {'command': 'echo a; sleep 0.6; echo b'}, 'I will run it.'), A('Ran it.')])
# a command typed during a run: the first turn (a read) is over, the next answer is a long time coming
R1 = lambda: TOOL('read', {'path': 'notes.txt'}, 'I will read it.')
sc('prompt_busy', steps=[('send', P('read it')), ('gate', 'turn_end'), ('cmd', P('again')),
                        ('cmd', P('queue it', streamingBehavior='followUp')), ('settle',)],
   answers=[R1(), SLOW(2, A('Read it.')), A('Queued done.')])
sc('steer', steps=[('send', P('read it')), ('gate', 'turn_end'), ('cmd', {'type': 'steer', 'message': 'go left'}), ('settle',)],
   answers=[R1(), SLOW(2, A('Read it.')), A('Left it is.')])
sc('follow_up', steps=[('send', P('read it')), ('gate', 'turn_end'),
                       ('cmd', {'type': 'follow_up', 'message': 'and after'}), ('settle',)],
   answers=[R1(), SLOW(2, A('Read it.')), A('After done.')])
sc('abort', steps=[('send', P('read it')), ('gate', 'turn_end'), ('cmd', {'type': 'abort'}), ('settle',)],
   answers=[R1(), SLOW(3, A('Never seen.'))])
sc('clear_queue', steps=[('send', P('read it')), ('gate', 'turn_end'),
                         ('cmd', {'type': 'steer', 'message': 'go left'}), ('cmd', {'type': 'follow_up', 'message': 'and after'}),
                         ('cmd', {'type': 'clear_queue'}), ('settle',)],
   answers=[R1(), SLOW(2, A('Read it.'))])
sc('queue_idle', steps=[('cmd', {'type': 'steer', 'message': 'go left'}), ('cmd', {'type': 'follow_up', 'message': 'and after'}),
                       ('cmd', {'type': 'get_state'}), ('cmd', {'type': 'clear_queue'})], answers=[])
sc('get_state', steps=[('cmd', {'type': 'get_state'}), ('prompt', 'hello'), ('cmd', {'type': 'get_state'})],
   answers=[A('Hello from the fixture.')])
sc('get_messages', steps=[('prompt', 'hello'), ('cmd', {'type': 'get_messages'})], answers=[A('Hello from the fixture.')])
# ---- model, thinking ----
sc('set_model', steps=[('cmd', {'type': 'set_model', 'provider': 'fake', 'modelId': 'fake-think'}),
                       ('cmd', {'type': 'set_model', 'provider': 'fake', 'modelId': 'nope'})], answers=[])
sc('cycle_model', steps=[('cmd', {'type': 'cycle_model'})], answers=[])
sc('get_available_models', steps=[('cmd', {'type': 'get_available_models'})], answers=[])
sc('set_thinking_level', steps=[('cmd', {'type': 'set_thinking_level', 'level': 'high'}), ('cmd', {'type': 'get_state'})],
   answers=[], model='fake-think')
sc('cycle_thinking_level', steps=[('cmd', {'type': 'cycle_thinking_level'}), ('cmd', {'type': 'cycle_thinking_level'})],
   answers=[], model='fake-think')
sc('cycle_thinking_level_off', steps=[('cmd', {'type': 'cycle_thinking_level'})], answers=[])
sc('get_available_thinking_levels', steps=[('cmd', {'type': 'get_available_thinking_levels'})], answers=[], model='fake-think')
sc('get_available_thinking_levels_off', steps=[('cmd', {'type': 'get_available_thinking_levels'})], answers=[])
# ---- queue modes, compaction, retry ----
sc('set_steering_mode', steps=[('cmd', {'type': 'set_steering_mode', 'mode': 'all'}), ('cmd', {'type': 'get_state'})], answers=[])
sc('set_follow_up_mode', steps=[('cmd', {'type': 'set_follow_up_mode', 'mode': 'all'}), ('cmd', {'type': 'get_state'})], answers=[])
sc('compact', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('cmd', {'type': 'compact'})],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), A('## Goal\nSay hello.\n\n## Progress\nDone.', 40)],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
sc('compact_empty', steps=[('cmd', {'type': 'compact'})], answers=[])
# HF6 context-overflow recovery (agent-session.ts _checkCompaction): the third answer's request outgrew the window --
# pi omits the failed answer, compacts (reason overflow) and retries once; the second overflow ends it with an error
OVERFLOW = ('http', 400, json.dumps({'type': 'error', 'error': {'type': 'invalid_request_error',
                                                                 'message': 'prompt is too long: 213462 tokens > 200000 maximum'}}))
SUMMARY = A('## Goal\nSay hello.\n\n## Progress\nDone.', 40)
sc('overflow', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('prompt', 'and a third')],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), OVERFLOW, SUMMARY, A('Recovered after compaction.', 20)],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
sc('overflow_twice', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('prompt', 'and a third')],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), OVERFLOW, SUMMARY, OVERFLOW],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
# the allowance comes back at every answer that is neither an error nor a length stop (message_end) and at every user
# message (message_start): a retried answer that calls a tool lets the next overflow of the run recover again
sc('overflow_tool', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('prompt', 'and a third')],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), OVERFLOW, SUMMARY,
            TOOL('read', {'path': 'notes.txt'}), OVERFLOW, SUMMARY, SUMMARY,
            A('Recovered after compaction.', 20)],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
# the check before a prompt (prompt(): _checkCompaction(lastAssistant, false)) runs before the user message resets the
# allowance: after the second overflow, the next prompt first tells the failure again
sc('overflow_twice_next', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('prompt', 'and a third'), ('prompt', 'a fourth')],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), OVERFLOW, SUMMARY, OVERFLOW,
            A('After the failure.', 20)],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})


def BIG(text, input_tokens, stop='end_turn', out=20):
    """an answer whose usage says the request filled the window (silent overflow) or that stopped on the output limit"""
    ev = [T.msg_start('msg_big'), {'type': 'content_block_start', 'index': 0, 'content_block': {'type': 'text', 'text': ''}},
          {'type': 'content_block_delta', 'index': 0, 'delta': {'type': 'text_delta', 'text': text}},
          {'type': 'content_block_stop', 'index': 0},
          {'type': 'message_delta', 'delta': {'stop_reason': stop, 'stop_sequence': None}, 'usage': {'output_tokens': out}},
          {'type': 'message_stop'}]
    ev[0]['message']['usage']['input_tokens'] = input_tokens
    return ('sse', T.sse(ev))


# a successful answer whose input already exceeded the window is kept and compacted, not asked again (case 2); a length stop
# below the model's output limit is omitted, compacted and asked again (recoverableLength)
sc('overflow_silent', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('prompt', 'and a third')],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), BIG('Too big but fine.', 250000), SUMMARY, SUMMARY],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
sc('overflow_length', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('prompt', 'and a third')],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), BIG('Cut short', 1200, 'max_tokens', 5), SUMMARY,
            A('Recovered after compaction.', 20)],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
# agent_settled.aborted (agent-session.ts abort(): _agentRunAbortRequested is set by abort() while a run is active and by
# nothing else): the abort command during the retry wait and during an auto-compaction says true, abort_retry during the
# wait only cuts the wait (false)
RETRY_WAIT = {'retry': {'enabled': True, 'maxRetries': 2, 'baseDelayMs': 4000}}
OVERLOADED = ('http', 529, json.dumps({'type': 'error', 'error': {'type': 'overloaded_error', 'message': 'Overloaded'}}))
sc('abort_in_retry', steps=[('send', P('hello')), ('gate', 'auto_retry_start'), ('cmd', {'type': 'abort'}), ('settle',)],
   retry=True, settings=RETRY_WAIT, answers=[OVERLOADED, A('Never seen.')])
sc('abort_retry_wait', steps=[('send', P('hello')), ('gate', 'auto_retry_start'), ('cmd', {'type': 'abort_retry'}), ('settle',)],
   retry=True, settings=RETRY_WAIT, answers=[OVERLOADED, A('Never seen.')])
sc('abort_in_compaction', steps=[('prompt', 'hello'), ('prompt', 'and again'), ('send', P('and a third')), ('gate', 'compaction_start'),
                                 ('cmd', {'type': 'abort'}), ('settle',)],
   answers=[A('Hello from the fixture.', 20), A('Hello once more.', 20), OVERFLOW, SLOW(3, SUMMARY), A('Never seen.')],
   session=True, settings={'compaction': {'keepRecentTokens': 10}})
sc('set_auto_compaction', steps=[('cmd', {'type': 'set_auto_compaction', 'enabled': False}), ('cmd', {'type': 'get_state'})], answers=[])
sc('set_auto_retry', steps=[('cmd', {'type': 'set_auto_retry', 'enabled': False})], answers=[])
sc('abort_retry', steps=[('cmd', {'type': 'abort_retry'})], answers=[])
# ---- bash, stats, export ----
sc('bash', steps=[('cmd', {'type': 'bash', 'command': 'echo hi'}), ('cmd', {'type': 'bash', 'command': 'echo quiet', 'excludeFromContext': True}),
                  ('prompt', 'hello')], answers=[A('Hello from the fixture.')], session=True)
# Q15: user bash does not depend on the active tool list (agent-session.ts executeBash: createLocalBashOperations)
sc('bash_no_tools', steps=[('cmd', {'type': 'bash', 'command': 'echo hi'}), ('cmd', {'type': 'get_state'})], answers=[], args=['--no-tools'])
sc('abort_bash', steps=[('cmd', {'type': 'abort_bash'})], answers=[])
sc('get_session_stats', steps=[('prompt', 'hello'), ('cmd', {'type': 'get_session_stats'})], answers=[A('Hello from the fixture.')], session=True)
sc('export_html', steps=[('prompt', 'hello'), ('cmd', {'type': 'export_html', 'outputPath': 'out.html'})],
   answers=[A('Hello from the fixture.')], session=True)
sc('get_last_assistant_text', steps=[('cmd', {'type': 'get_last_assistant_text'}), ('prompt', 'hello'),
                                     ('cmd', {'type': 'get_last_assistant_text'})], answers=[A('Hello from the fixture.')])
sc('get_commands', steps=[('cmd', {'type': 'get_commands'})], answers=[], args=['--no-extensions'])
# J: prompt templates (core/prompt-templates.ts): agent dir prompts/, trusted project .pi/prompts/ (the project wins a name),
# frontmatter description and argument-hint, the first line as the description, the substitution forms
TEMPLATES = {
    'agent/prompts/review.md': '---\ndescription: Review code\nargument-hint: "<path> [focus]"\n---\nReview $1 focusing on ${2:-general quality}. All: $@. Rest: ${@:2}.\n',
    'project/.pi/prompts/review.md': '---\ndescription: Project review\n---\nProject review of $ARGUMENTS\n',
    'project/.pi/prompts/fix.md': 'Fix the failing test in $1 and explain what was wrong with the code in detail so that it never returns\nsecond line\n',
    'agent/prompts/sum.md': '---\ndescription: Sum\n---\n${@:2:1}|${1:-none}|$ARGUMENTS|$9|${ARGUMENTS:-empty}|$1$2\n',
}
sc('prompt_templates', steps=[('cmd', {'type': 'get_commands'}), ('prompt', '/review src "two words"'), ('prompt', '/fix a.c'),
                              ('prompt', '/sum  x   "y z" w'), ('prompt', '/sum'), ('prompt', '/nosuch q'), ('cmd', {'type': 'get_messages'})],
   answers=[A('Reviewed.'), A('Fixed.'), A('Summed.'), A('Summed none.'), A('Unknown.')], args=['--no-extensions'],
   settings={'defaultProjectTrust': 'always'}, text_files=TEMPLATES)
sc('prompt_template_path', steps=[('cmd', {'type': 'get_commands'}), ('prompt', '/ext one "two words"')],
   answers=[A('Extra.')], args=['--no-extensions', '--no-prompt-templates', '--prompt-template', 'extra'],
   settings={'defaultProjectTrust': 'always'},
   text_files={'project/extra/ext.md': 'Extra $2 then $1\n', 'project/extra/.dot.md': 'dot\n', 'agent/prompts/skipped.md': 'skipped\n'})
# ---- sessions ----
sc('new_session', steps=[('prompt', 'hello'), ('cmd', {'type': 'new_session'}), ('cmd', {'type': 'get_state'})],
   answers=[A('Hello from the fixture.')], session=True)
sc('switch_session', steps=[('cmd', {'type': 'switch_session', 'sessionPath': 'other.jsonl'}), ('cmd', {'type': 'get_state'}),
                            ('cmd', {'type': 'switch_session', 'sessionPath': 'missing.jsonl'})],
   answers=[], session=True, files={'other.jsonl': 'doc_branch'})
sc('fork', steps=[('cmd', {'type': 'get_fork_messages'}), ('cmd', {'type': 'fork', 'entryId': 'd0000006'}), ('cmd', {'type': 'get_state'}),
                  ('cmd', {'type': 'fork', 'entryId': 'nothere'})],
   answers=[], session='doc_branch')
sc('clone', steps=[('cmd', {'type': 'clone'}), ('cmd', {'type': 'get_state'})], answers=[], session='doc_branch')
sc('get_fork_messages', steps=[('cmd', {'type': 'get_fork_messages'})], answers=[], session='doc_branch')
sc('get_entries', steps=[('cmd', {'type': 'get_entries'}), ('cmd', {'type': 'get_entries', 'since': 'd0000005'}),
                         ('cmd', {'type': 'get_entries', 'since': 'nothere'})], answers=[], session='doc_branch')
sc('get_tree', steps=[('cmd', {'type': 'get_tree'})], answers=[], session='doc_branch')
sc('set_session_name', steps=[('cmd', {'type': 'set_session_name', 'name': '  my session  '}), ('cmd', {'type': 'get_state'}),
                              ('cmd', {'type': 'set_session_name', 'name': '   '})], answers=[], session=True)
# ---- framing ----
sc('errors', steps=[('raw', '{"type":"get_state"'), ('cmd', {'type': 'no_such_command'}), ('cmd', {'type': 'get_state'})], answers=[])
# ---- extension UI ----
sc('ui_dialogs', steps=[('send', P('run it')), ('gate', 'extension_ui_request'), ('ui', {'value': 'Allow'}),
                        ('gate', 'extension_ui_request'), ('ui', {'confirmed': True}),
                        ('gate', 'extension_ui_request'), ('ui', {'value': 'because'}), ('settle',)],
   answers=[TOOL('read', {'path': 'notes.txt'}), A('Ran it.')], ext=EXT_UI)
sc('ui_cancel', steps=[('send', P('run it')), ('gate', 'extension_ui_request'), ('ui', {'cancelled': True}),
                       ('gate', 'extension_ui_request'), ('ui', {'cancelled': True}),
                       ('gate', 'extension_ui_request'), ('ui', {'cancelled': True}), ('settle',)],
   answers=[TOOL('read', {'path': 'notes.txt'}), A('It was blocked.')], ext=EXT_UI)


# ---- L: extension events and decisions (amiga-pi's hook commands are the same events: ext_hooks.c) ----
# The events amiga-pi hooks (not message_update: the cumulative partial message and `partial` of pi-ai's streaming parser;
# not turn_end: BoundaryState, entries and the context preview; COMPATIBILITY.md). The dump extension writes each event pi
# emits to dump.jsonl in the project directory, one JSON line (JSON.stringify of the event, as pi's runner hands it over);
# the hook command of the amiga-pi side appends the event's file to the same name ($CLAUDE_PROJECT_DIR is the project).
L_EVENTS = ['session_start', 'input', 'agent_start', 'turn_start', 'message_start', 'message_end', 'tool_call',
            'tool_execution_start', 'tool_execution_update', 'tool_result', 'tool_execution_end', 'agent_end', 'agent_settled']
EXT_DUMP = r'''
import { appendFileSync } from "node:fs";
export default function (pi) {
  for (const name of %s) {
    pi.on(name, async (event) => { appendFileSync("dump.jsonl", JSON.stringify(event) + "\n"); });
  }
}
''' % json.dumps(L_EVENTS)


def hook(cmd, matcher=None):
    h = {'hooks': [{'type': 'command', 'command': cmd}]}
    if matcher:
        h['matcher'] = matcher
    return [h]


L_DUMP_HOOKS = {e: hook('cat >> $CLAUDE_PROJECT_DIR/dump.jsonl') for e in L_EVENTS}
L_BASH = lambda: TOOL('bash', {'command': 'echo one; echo two'}, 'I will run it.')
sc('ext_events', mode='json', args=['run it'], answers=[L_BASH(), A('Ran it.')], ext=EXT_DUMP,
   settings={'hooks': L_DUMP_HOOKS}, collect=['dump.jsonl'])


def ext_one(event, body):
    return 'export default function (pi) { pi.on("%s", async (event) => { %s }); }\n' % (event, body)


def say(obj):
    return "printf '%s'" % json.dumps(obj, separators=(',', ':'))


# tool_call: block with a reason, block without one (pi: "Tool execution was blocked"), mutate event.input
sc('ext_block', mode='json', args=['run it'], answers=[L_BASH(), A('It was blocked.')],
   ext=ext_one('tool_call', 'return { block: true, reason: "no shell today" };'),
   settings={'hooks': {'tool_call': hook(say({'block': True, 'reason': 'no shell today'}))}})
sc('ext_block_plain', mode='json', args=['run it'], answers=[L_BASH(), A('It was blocked.')],
   ext=ext_one('tool_call', 'return { block: true };'),
   settings={'hooks': {'tool_call': hook(say({'block': True}))}})
sc('ext_mutate', mode='json', args=['run it'], answers=[L_BASH(), A('Ran it.')],
   ext=ext_one('tool_call', 'event.input.command = "echo mutated";'),
   settings={'hooks': {'tool_call': hook(say({'input': {'command': 'echo mutated'}}))}})
# a matcher: the hook names the tool, a read is not its business
sc('ext_matcher', mode='json', args=['read the file'], answers=[TOOL('read', {'path': 'notes.txt'}), A('Read it.')],
   ext=ext_one('tool_call', 'if (event.toolName === "bash") return { block: true, reason: "no shell today" };'),
   settings={'hooks': {'tool_call': hook(say({'block': True, 'reason': 'no shell today'}), 'bash')}})
# tool_result: the content and the error flag replaced
sc('ext_result', mode='json', args=['run it'], answers=[L_BASH(), A('Ran it.')],
   ext=ext_one('tool_result', 'return { content: [{ type: "text", text: "redacted" }], isError: true };'),
   settings={'hooks': {'tool_result': hook(say({'content': [{'type': 'text', 'text': 'redacted'}], 'isError': True}))}})
# input: transform (the prompt the model sees is the new text) and handled (no run at all)
sc('ext_input_transform', mode='json', args=['run it'], answers=[A('Ran it.')],
   ext=ext_one('input', 'return { action: "transform", text: "run it, please" };'),
   settings={'hooks': {'input': hook(say({'action': 'transform', 'text': 'run it, please'}))}})
sc('ext_input_handled', mode='json', args=['run it'], answers=[],
   ext=ext_one('input', 'return { action: "handled" };'),
   settings={'hooks': {'input': hook(say({'action': 'handled'}))}})
# RPC: the input event's source is "rpc", a consumed prompt answers disposition "handled", new_session is a session_start "new"
sc('ext_rpc_prompt', steps=[('prompt', 'hello')], answers=[A('Hi.')], ext=EXT_DUMP, settings={'hooks': L_DUMP_HOOKS},
   collect=['dump.jsonl'])
sc('ext_rpc_handled', steps=[('send', P('run it')), ('sleep', 1.0)], answers=[],
   ext=ext_one('input', 'return { action: "handled" };'),
   settings={'hooks': {'input': hook(say({'action': 'handled'}))}})
sc('ext_rpc_new', steps=[('cmd', {'type': 'new_session'})], answers=[], ext=EXT_DUMP, settings={'hooks': L_DUMP_HOOKS},
   collect=['dump.jsonl'], session=True)


# ---- L4: tools and commands an extension registers (pi.registerTool, pi.registerCommand) ----
# The extensions sit in <agent dir>/extensions/ (pi finds them there: sourceInfo source "auto", scope "user"), as the Lua twins of
# gen_lua_scenarios.py do. A tool's execute writes what it was handed to exec.jsonl; the requests the model got are kept
# (<name>.req.N.json: tools and messages).
def tool_def(name, snippet, schema, body):
    return '''  pi.registerTool({
    name: "%s", label: "%s", description: "%s tool.",%s
    parameters: %s,
    async execute(toolCallId, params, signal, onUpdate, ctx) {
%s
    },
  });
''' % (name, name, name, snippet, schema, body)


GREET_SCHEMA = '{ type: "object", properties: { name: { type: "string", description: "Who to greet" } }, required: ["name"] }'
EXT_TOOLS = '''import { appendFileSync } from "node:fs";
export default function (pi) {
''' + tool_def('greet', ' promptSnippet: "Greet a person by name", promptGuidelines: ["Use greet when asked to greet someone."],', GREET_SCHEMA,
'''      appendFileSync("exec.jsonl", JSON.stringify({ toolCallId, params }) + "\\n");
      onUpdate({ content: [{ type: "text", text: "greeting " + params.name }], details: { step: 1 } });
      return { content: [{ type: "text", text: "Hello, " + params.name + "!" }], details: { greeted: params.name } };''') + tool_def(
    'flaky', '', '{ type: "object", properties: {} }',
'''      return { content: [{ type: "text", text: "flaked" }], details: { code: 7 }, isError: true };''') + tool_def(
    'boom', '', '{ type: "object", properties: {} }',
'''      throw new Error("boom");''') + tool_def(
    'soft', '', '{ type: "object", properties: {} }',
'''      return { content: [{ type: "text", text: "soft failure" }], isError: true };''') + "}\n"
EXT_FILES = {'agent/extensions/t.ts': EXT_TOOLS}
sc('ext_tool', mode='json', args=['greet Ada'], answers=[TOOL('greet', {'name': 'Ada'}), A('Greeted.')], text_files=EXT_FILES,
   requests=True, session=True, collect=['exec.jsonl', 'session.jsonl'])
sc('ext_tool_results', mode='json', args=['try them'], text_files=EXT_FILES, requests=True,
   answers=[TOOL('flaky', {}, tid='toolu_1'), TOOL('boom', {}, tid='toolu_2'), TOOL('soft', {}, tid='toolu_3'), A('Tried.')])
# --tools and --no-tools on an extension's tools (sdk.ts:282-294, agent-session.ts _isAllowedTool)
for nm, args in (('allow', ['--tools', 'read']), ('named', ['--tools', 'read,greet']), ('only', ['--tools', 'greet']),
                 ('none', ['--no-tools']), ('minus', ['--tools', '-greet']), ('plus_none', ['--no-tools', '--tools', '+greet']),
                 ('pattern', ['--tools', 'read,gre*'])):
    sc('ext_tool_' + nm, mode='json', args=args + ['greet Ada'], answers=[TOOL('greet', {'name': 'Ada'}), A('Greeted.')],
       text_files=EXT_FILES, requests=True)
sc('ext_tool_default', mode='json', args=['greet Ada'], answers=[TOOL('greet', {'name': 'Ada'}), A('Greeted.')],
   text_files=EXT_FILES, requests=True, settings={'defaultTools': ['read']})

EXT_CMDS = '''import { appendFileSync } from "node:fs";
export default function (pi) {
  pi.registerCommand("hello", { description: "Say hello", async handler(args, ctx) {
    appendFileSync("cmd.jsonl", JSON.stringify({ command: "hello", args }) + "\\n");
  } });
  pi.registerCommand("quiet", { async handler(args, ctx) {
    appendFileSync("cmd.jsonl", JSON.stringify({ command: "quiet", args }) + "\\n");
  } });
}
'''
sc('ext_cmd', steps=[('cmd', {'type': 'get_commands'}), ('send', P('/hello two  words')), ('send', P('/quiet')), ('prompt', 'hi')],
   answers=[A('Hi.')], text_files={'agent/extensions/t.ts': EXT_CMDS}, collect=['cmd.jsonl'])
# two extensions with the same command name: invocation names hello:1 and hello:2 (runner.ts resolveRegisteredCommands)
EXT_DUP = '''import { appendFileSync } from "node:fs";
export default function (pi) {
  pi.registerCommand("hello", { description: "Say hello from %s", async handler(args, ctx) {
    appendFileSync("cmd.jsonl", JSON.stringify({ command: "hello", from: "%s", args }) + "\\n");
  } });
}
'''
sc('ext_cmd_dup', steps=[('cmd', {'type': 'get_commands'}), ('send', P('/hello:2 x')), ('send', P('/hello:1 y')), ('prompt', '/hello z')],
   answers=[A('Hi.')], text_files={'agent/extensions/a.ts': EXT_DUP % ('a', 'a'), 'agent/extensions/b.ts': EXT_DUP % ('b', 'b')},
   collect=['cmd.jsonl'])
# a trusted project's extension (scope "project", found first) and a directory extension with an index file
sc('ext_cmd_scope', steps=[('cmd', {'type': 'get_commands'}), ('send', P('/hello:1 x')), ('send', P('/hello:2 y')), ('sleep', 0.5)], answers=[],
   settings={'defaultProjectTrust': 'always'},
   text_files={'project/.pi/extensions/p.ts': EXT_DUP % ('project', 'project'), 'agent/extensions/dirx/index.ts': EXT_DUP % ('user', 'user')},
   collect=['cmd.jsonl'])



class Trickle(T.Fake):
    """T.Fake with the SSE events sent one at a time, a pause between them: pi puts the usage of the stream so far into
    every message_update, and in a stream that arrives in one piece it serializes them after the stream is done (the
    object is shared), so all of them would carry the final usage. A stream with pauses is what a real provider is."""

    def do_POST(self):
        n = int(self.headers.get('content-length') or 0)
        body = self.rfile.read(n)
        with T.Fake.lock:
            T.Fake.served.append(body)
            a = T.Fake.answers.pop(0) if T.Fake.answers else ('http', 500, '{"type":"error","error":{"type":"api_error","message":"fixture out of answers"}}')
        if a[0] == 'slow':
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
        for ev in a[1].split('\n\n'):
            if ev:
                self.wfile.write((ev + '\n\n').encode())
                self.wfile.flush()
                time.sleep(0.04)
        self.close_connection = True


def setup(name, s, port):
    shutil.rmtree(WORK, ignore_errors=True)
    agent = os.path.join(WORK, 'agent')
    proj = os.path.join(WORK, 'project')
    os.makedirs(os.path.join(agent, 'bin'))
    for tool in ('fd', 'rg'):
        t = os.path.join(agent, 'bin', tool)
        open(t, 'w').write('#!/bin/sh\nexit 0\n')
        os.chmod(t, 0o755)
    os.makedirs(proj)
    open(os.path.join(proj, 'notes.txt'), 'w').write('one\ntwo\nthree\n')
    models = {'providers': {'fake': {'baseUrl': 'http://127.0.0.1:%d' % port, 'api': 'anthropic-messages', 'apiKey': 'k',
                                     'models': MODELS}}}
    json.dump(models, open(os.path.join(agent, 'models.json'), 'w'), indent=1)
    json.dump({'defaultProvider': 'fake', 'defaultModel': s.get('model', 'fake-model'),
               'defaultThinkingLevel': s.get('thinking', 'off'),
               'retry': {'enabled': bool(s.get('retry')), 'maxRetries': 2, 'baseDelayMs': 100},
               'quietStartup': True, **s.get('settings', {})}, open(os.path.join(agent, 'settings.json'), 'w'))
    for rel, doc in s.get('files', {}).items():
        txt = open(os.path.join(SESSIONS, doc + '.jsonl')).read()
        open(os.path.join(proj, rel), 'w').write(txt.replace('/path/to/project', proj))
    if s.get('ext'):
        open(os.path.join(proj, 'ext.ts'), 'w').write(s['ext'])
    for rel, txt in s.get('text_files', {}).items():       # J: prompt templates; rel is under agent/ or project/
        path = os.path.join(WORK, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'w').write(txt)
    return agent, proj


class Pi:
    def __init__(self, args, env, cwd):
        self.p = subprocess.Popen([T.PI] + args, cwd=cwd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.lines = []             # raw stdout lines (text), in order
        self.err = b''
        self.lock = threading.Lock()
        threading.Thread(target=self._rd, daemon=True).start()
        threading.Thread(target=self._re, daemon=True).start()

    def _rd(self):
        for ln in self.p.stdout:
            with self.lock:
                self.lines.append(ln.decode().rstrip('\n'))

    def _re(self):
        self.err = self.p.stderr.read()

    def snapshot(self):
        with self.lock:
            return list(self.lines)

    def wait(self, pred, secs=30):
        end = time.time() + secs
        while time.time() < end:
            for ln in self.snapshot():
                try:
                    o = json.loads(ln)
                except ValueError:
                    continue
                if pred(o):
                    return o
            time.sleep(0.05)
        return None

    def send(self, text):
        self.p.stdin.write((text + '\n').encode())
        self.p.stdin.flush()


def run(name, s, keep):
    Fake = T.Fake
    Fake.answers = list(s['answers'])
    Fake.served = []
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Trickle)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    agent, proj = setup(name, s, port)
    env = {'HOME': WORK, 'PATH': os.environ['PATH'], 'LANG': 'en_US.UTF-8', 'PI_CODING_AGENT_DIR': agent,
           'PI_OFFLINE': '1', 'PI_SKIP_VERSION_CHECK': '1', 'PI_TELEMETRY': '0'}
    mode = s.get('mode', 'rpc')
    args = ['--mode', mode]
    sess = s.get('session')
    if sess:
        if sess is True:
            args += ['--session', os.path.join(proj, 'session.jsonl')]
        else:
            txt = open(os.path.join(SESSIONS, sess + '.jsonl')).read()
            open(os.path.join(proj, 'session.jsonl'), 'w').write(txt.replace('/path/to/project', proj))
            args += ['--session', 'session.jsonl']
    else:
        args += ['--no-session']
    if s.get('ext'):
        args += ['-e', 'ext.ts']
    args += s.get('args', [])
    pi = Pi(args, env, proj)
    inlines = []
    try:
        nid = 0
        settled = 0
        last_ui = None
        for st in s.get('steps', []):
            k = st[0]
            if k in ('cmd', 'prompt', 'send'):
                nid += 1
                obj = P(st[1]) if k == 'prompt' else dict(st[1])
                obj = dict([('id', str(nid))] + list(obj.items()))
                line = json.dumps(obj, separators=(',', ':'))
                inlines.append(line)
                pi.send(line)
                i = str(nid)
                if k == 'send':
                    pi.wait(lambda o: o.get('type') == 'response' and o.get('id') == i)
                elif not pi.wait(lambda o: o.get('type') == 'response' and o.get('id') == i):
                    print('[ERROR] %s: no response to %s' % (name, line), file=sys.stderr)
                if k == 'prompt':
                    settled += 1
                    n = settled
                    if not pi.wait(lambda o, n=n: sum(1 for l in pi.snapshot() if '"agent_settled"' in l) >= n and o.get('type') == 'agent_settled'):
                        print('[ERROR] %s: never settled' % name, file=sys.stderr)
                    time.sleep(0.3)
                    inlines.append('#after agent_settled')      # what follows was sent once the run was over
            elif k == 'raw':
                inlines.append(st[1])
                pi.send(st[1])
                pi.wait(lambda o: o.get('command') == 'parse')
                time.sleep(0.2)
            elif k == 'gate':
                # a command typed during a run: the event must be in the output first (one more than the gates before)
                ev = st[1]
                have = sum(1 for l in pi.snapshot() if json.loads(l).get('type') == ev) if ev else 0
                inlines.append('#after ' + ev)
                want = 1 + sum(1 for l in inlines if l == '#after ' + ev) - 1
                if not pi.wait(lambda o, ev=ev, want=want: sum(1 for l in pi.snapshot() if json.loads(l).get('type') == ev) >= want and o.get('type') == ev):
                    print('[ERROR] %s: never saw %s' % (name, ev), file=sys.stderr)
                time.sleep(0.15)
            elif k == 'ui':
                # the answer to the next dialog; the .in names the request by its place ($ui:N, the N-th extension_ui_request
                # of the output: the ids are random)
                reqs = [json.loads(l) for l in pi.snapshot() if '"extension_ui_request"' in l]
                done = len([l for l in inlines if '"extension_ui_response"' in l])
                dialogs = [(i, r) for i, r in enumerate(reqs) if r.get('method') in ('select', 'confirm', 'input', 'editor')]
                idx, req = dialogs[done]
                obj = {'type': 'extension_ui_response', 'id': req['id']}
                obj.update(st[1])
                pi.send(json.dumps(obj, separators=(',', ':')))
                obj['id'] = '$ui:%d' % (idx + 1)
                inlines.append(json.dumps(obj, separators=(',', ':')))
                time.sleep(0.3)
            elif k == 'settle':
                settled += 1
                n = settled
                if not pi.wait(lambda o, n=n: sum(1 for l in pi.snapshot() if '"agent_settled"' in l) >= n and o.get('type') == 'agent_settled'):
                    print('[ERROR] %s: never settled' % name, file=sys.stderr)
                time.sleep(0.3)
                inlines.append('#after agent_settled')
            elif k == 'sleep':
                time.sleep(st[1])
        if mode == 'rpc':
            pi.p.stdin.close()
        else:
            pi.p.stdin.close()
        try:
            pi.p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            pi.p.kill()
        time.sleep(0.2)
        for f in s.get('collect', []):                  # files the scenario's extension wrote in the project directory
            if os.path.exists(os.path.join(proj, f)):
                shutil.copy(os.path.join(proj, f), os.path.join(OUT, name + '.' + f))
        if s.get('requests'):                           # L4: what pi asked the model (its tools and messages), request by request
            for i, body in enumerate(T.Fake.served):
                b = json.loads(body)
                with open(os.path.join(OUT, '%s.req.%d.json' % (name, i + 1)), 'w') as f:
                    json.dump({'tools': b.get('tools', []), 'messages': b.get('messages', [])}, f, indent=1)
                    f.write('\n')
        out = pi.snapshot()
        with open(os.path.join(OUT, name + '.out'), 'w') as f:
            f.write(''.join(l + '\n' for l in out))
        if mode == 'rpc':
            with open(os.path.join(OUT, name + '.in'), 'w') as f:
                f.write(''.join(l + '\n' for l in inlines))
        # the scenario's environment, for the host test (tests/test_pi_modes.c reads it with the golden)
        cfg = {'mode': mode, 'args': s.get('args', []), 'model': s.get('model', 'fake-model'), 'thinking': s.get('thinking', 'off'),
               'retry': bool(s.get('retry')), 'settings': s.get('settings', {}),
               'session': ('new' if sess is True else sess) if sess else None, 'files': s.get('files', {}), 'text_files': s.get('text_files', {}),
               'answers': [a[0] for a in s['answers']], 'tools': 'read,bash,edit,write', 'collect': s.get('collect', [])}
        if s.get('requests'):
            cfg['requests'] = True
        with open(os.path.join(OUT, name + '.cfg.json'), 'w') as f:
            json.dump(cfg, f, indent=1)
            f.write('\n')
        for i, a in enumerate(s['answers']):
            if a[0] in ('sse', 'slow'):
                with open(os.path.join(OUT, '%s.%d.sse' % (name, i + 1)), 'w') as f:
                    f.write(a[-1])
            else:
                with open(os.path.join(OUT, '%s.%d.http' % (name, i + 1)), 'w') as f:
                    f.write('HTTP/1.1 %d X\r\ncontent-type: application/json\r\ncontent-length: %d\r\n\r\n%s' % (a[1], len(a[2]), a[2]))
        if pi.err.strip():
            print('[WARN] %s: stderr: %s' % (name, pi.err.decode()[:300].strip()), file=sys.stderr)
        print('[OK] %s (%d lines)' % (name, len(out)))
    finally:
        if pi.p.poll() is None:
            pi.p.kill()
        srv.shutdown()
        if not keep:
            shutil.rmtree(WORK, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    json.dump({'providers': {'fake': {'baseUrl': 'http://127.0.0.1:9', 'api': 'anthropic-messages', 'apiKey': 'k', 'models': MODELS}}},
              open(os.path.join(OUT, 'models.json'), 'w'), indent=1)
    for n, s in SC.items():
        if a.only and a.only != n:
            continue
        run(n, s, a.keep)


if __name__ == '__main__':
    main()
