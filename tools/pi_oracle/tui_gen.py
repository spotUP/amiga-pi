#!/usr/bin/env python3
"""tui_gen -- the goldens of tests/pi/tui (plan N0): real pi 1.0.4 in tmux at the Amiga window's
size, against a local fake Messages API (scripted answers, no key, no cost), each screen captured
with `tmux capture-pane -e -p` (text and SGR colours).

  python3 tools/pi_oracle/tui_gen.py [--only <scenario>] [--cols 80] [--rows 25] [--keep]

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


MARKDOWN = (
    "# Plan\n\nHere is **bold**, *italic* and `inline code`.\n\n"
    "- first item\n- second item with a [link](https://example.org)\n\n"
    "> a quoted line\n\n"
    "```c\nint main(void)\n{\n    return 0;\n}\n```\n")

TABLE = "1. one\n2. two\n\n| name | size |\n|------|------|\n| a | 1 |\n| b | 22 |\n"

ERR_BODY = json.dumps({'type': 'error', 'error': {'type': 'invalid_request_error', 'message': 'model: fake-model is not available'}})

# scenario: name, keys sent after the start (list of (text, enter?, wait)), answers served in order
# an answer is ('sse', text) or ('http', status, body)
SCENARIOS = {
    # the start screen alone
    'start': dict(steps=[], answers=[]),
    # the editor with typed text, the autocomplete menu for a slash
    'editor': dict(steps=[('type', 'hello there')], answers=[]),
    'slash': dict(steps=[('type', '/')], answers=[]),
    # a markdown answer
    'markdown': dict(steps=[('type', 'show markdown'), ('enter',), ('wait', 'return 0')],
                     answers=[('sse', text_answer('msg_md', MARKDOWN, 180))]),
    'table': dict(steps=[('type', 'show a table'), ('enter',), ('wait', '22')],
                  answers=[('sse', text_answer('msg_tb', TABLE, 60))]),
    # the project trust question: the one approval prompt pi has at start (a tool call is never asked)
    'approval': dict(steps=[('wait', 'rust')], answers=[], files={'.pi/SYSTEM.md': 'Be brief.\n'}),
    # the spinner row while the answer is awaited (the captured frame is a moment of it)
    'working': dict(steps=[('type', 'hello'), ('enter',), ('wait', 'Working'), ('capture_now',)],
                    answers=[('slow', 6, text_answer('msg_w', 'late answer', 20))]),
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
}


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


def tmux(*a):
    return subprocess.run(['tmux'] + list(a), capture_output=True, text=True).stdout


def capture(sess, esc):
    args = ['capture-pane', '-t', sess, '-p']
    if esc:
        args.insert(1, '-e')
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
    work = '/private/tmp/pitui'      # fixed: the trust prompt and the footer show the path
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    agent = os.path.join(work, 'agent')
    proj = os.path.join(work, 'project')
    os.makedirs(os.path.join(agent, 'bin'))
    for tool in ('fd', 'rg'):      # pi looks for them in its own bin first; stubs keep it from warning
        t = os.path.join(agent, 'bin', tool)
        open(t, 'w').write('#!/bin/sh\nexit 0\n')
        os.chmod(t, 0o755)
    os.makedirs(proj)
    with open(os.path.join(proj, 'notes.txt'), 'w') as f:
        f.write('one\ntwo\nthree\n')
    for rel, text in sc.get('files', {}).items():
        os.makedirs(os.path.dirname(os.path.join(proj, rel)), exist_ok=True)
        open(os.path.join(proj, rel), 'w').write(text)
    Fake.answers = [a for a in sc['answers']]
    Fake.served = []
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fake)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    json.dump({'providers': {'fake': {'baseUrl': 'http://127.0.0.1:%d' % port, 'api': 'anthropic-messages', 'apiKey': 'k',
              'models': [{'id': MODEL, 'name': MODEL, 'reasoning': False, 'input': ['text'], 'contextWindow': 200000,
                          'maxTokens': 8192, 'cost': {'input': 3, 'output': 15, 'cacheRead': 0.3, 'cacheWrite': 3.75}}]}}},
              open(os.path.join(agent, 'models.json'), 'w'))
    json.dump({'defaultProvider': 'fake', 'defaultModel': MODEL, 'defaultThinkingLevel': 'off',
               'retry': {'enabled': False}}, open(os.path.join(agent, 'settings.json'), 'w'))
    sess = 'pitui-' + name
    tmux('kill-session', '-t', sess)
    env = ('env -i HOME=%s PATH=%s TERM=xterm-256color LANG=en_US.UTF-8 PI_CODING_AGENT_DIR=%s PI_OFFLINE=1 '
           'PI_SKIP_VERSION_CHECK=1 PI_TELEMETRY=0 ' % (work, os.environ['PATH'], agent))
    tmux('new-session', '-d', '-s', sess, '-x', str(cols), '-y', str(rows), '-c', proj,
         env + 'pi --no-session')
    try:
        time.sleep(2.0)
        shots = {}
        for st in sc['steps']:
            if st[0] == 'type':
                tmux('send-keys', '-t', sess, '-l', st[1])
            elif st[0] == 'enter':
                tmux('send-keys', '-t', sess, 'Enter')
            elif st[0] == 'key':
                tmux('send-keys', '-t', sess, st[1])
            elif st[0] == 'capture_now':
                pane = capture(sess, True)
                shots['now'] = pane
            elif st[0] == 'wait':
                if not wait_text(sess, st[1]):
                    print('[ERROR] %s: never saw %r' % (name, st[1]), file=sys.stderr)
            time.sleep(0.3)
        pane = shots.get('now') or settle(sess, 1.5)
        with open(os.path.join(OUT, name + '.golden'), 'w') as f:
            f.write(pane)
        with open(os.path.join(OUT, name + '.txt'), 'w') as f:
            f.write(capture(sess, False))
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
        srv.shutdown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only')
    ap.add_argument('--cols', type=int, default=80)
    ap.add_argument('--rows', type=int, default=25)
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    for n, sc in SCENARIOS.items():
        if a.only and a.only != n:
            continue
        run(n, sc, a.cols, a.rows, a.keep)


if __name__ == '__main__':
    main()
