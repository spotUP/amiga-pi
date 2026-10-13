#!/usr/bin/env python3 -I
"""L3: for each hook-command scenario of L1/L2 (tests/pi/rpc/ext_*.cfg.json) write ext_lua_<rest>.cfg.json, the same
behaviour as a Lua extension in <agent dir>/extensions/t.lua. The new scenario names the old one as its "golden": pi 1.1.0's
records (.out, .N.sse, .in, dump.jsonl) are shared, so the Lua extension is compared with pi itself. Events that only
record go through print (the notice sink of the test, "lua_dump") and json.encode, whose objects have sorted keys, so
those scenarios are "canonical" (both sides are compared with sorted keys). Usage: python3 -I tools/pi_oracle/gen_lua_scenarios.py"""
import json, os, re

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tests', 'pi', 'rpc')
SKIP = {'ext_lua'}

def lua_of(cfg):
    hooks = cfg['settings']['hooks']
    out = []
    for ev, groups in hooks.items():
        for g in groups:
            for h in g['hooks']:
                cmd = h['command']
                cond = ''
                if g.get('matcher'):
                    cond = 'event.toolName == %s' % json.dumps(g['matcher'])
                if cmd.startswith('cat >>'):
                    body = 'print(json.encode(event))'
                else:
                    m = re.match(r"printf '(.*)'$", cmd)
                    dec = m.group(1)
                    if ev == 'tool_call' and '"input"' in dec:
                        # pi mutates event.input in place; a table of the same members does it here
                        inp = json.loads(dec)['input']
                        body = ''.join('event.input[%s] = %s\n  ' % (json.dumps(k), json.dumps(v)) for k, v in inp.items())
                    else:
                        body = 'return json.decode(%s)' % json.dumps(dec)
                if cond:
                    body = 'if %s then\n    %s\n  end' % (cond, body)
                out.append('pi.on(%s, function(event, ctx)\n  %s\nend)\n' % (json.dumps(ev), body))
    return ''.join(out)

for f in sorted(os.listdir(D)):
    m = re.match(r'(ext_.*)\.cfg\.json$', f)
    if not m or m.group(1).startswith('ext_lua'):
        continue
    name = m.group(1)
    cfg = json.load(open(os.path.join(D, f)))
    lua = lua_of(cfg)
    del cfg['settings']['hooks']
    cfg['golden'] = name
    cfg['text_files'] = {'agent/extensions/t.lua': lua}
    if cfg.get('collect'):
        cfg['lua_dump'] = cfg['collect'][0]
        cfg['canonical'] = True
    open(os.path.join(D, 'ext_lua_' + name[4:] + '.cfg.json'), 'w').write(json.dumps(cfg, indent=1) + '\n')
    print('wrote ext_lua_' + name[4:])
