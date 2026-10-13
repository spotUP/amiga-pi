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
    if not m or m.group(1).startswith('ext_lua') or m.group(1).startswith(('ext_tool', 'ext_cmd', 'ext_entry')):
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


# ---- L4: the scenarios of rpc_gen.py that register tools and commands (ext_tool*, ext_cmd*): the Lua
# extension that does the same, written by hand next to the TypeScript one there. The record of what execute / a handler was
# handed goes through print (the notice sink, "lua_dump") instead of appendFileSync; the object members of a JSON text have no
# order in Lua (json.encode sorts them), so the scenarios compare with sorted keys ("canonical"). ----
GREET = '''pi.registerTool({
  name = "greet", label = "greet", description = "greet tool.",
  promptSnippet = "Greet a person by name", promptGuidelines = { "Use greet when asked to greet someone." },
  parameters = { type = "object", properties = { name = { type = "string", description = "Who to greet" } }, required = { "name" } },
  execute = function(toolCallId, params, signal, onUpdate, ctx)
    print(json.encode({ toolCallId = toolCallId, params = params }))
    onUpdate({ content = { { type = "text", text = "greeting " .. params.name } }, details = { step = 1 } })
    return { content = { { type = "text", text = "Hello, " .. params.name .. "!" } }, details = { greeted = params.name } }
  end,
})
'''
FLAKY = '''pi.registerTool({
  name = "flaky", label = "flaky", description = "flaky tool.",
  parameters = { type = "object", properties = {} },
  execute = function(toolCallId, params, signal, onUpdate, ctx)
    return { content = { { type = "text", text = "flaked" } }, details = { code = 7 }, isError = true }
  end,
})
'''
BOOM = '''pi.registerTool({
  name = "boom", label = "boom", description = "boom tool.",
  parameters = { type = "object", properties = {} },
  execute = function(toolCallId, params, signal, onUpdate, ctx)
    error("boom", 0)
  end,
})
'''
SOFT = '''pi.registerTool({
  name = "soft", label = "soft", description = "soft tool.",
  parameters = { type = "object", properties = {} },
  execute = function(toolCallId, params, signal, onUpdate, ctx)
    return { content = { { type = "text", text = "soft failure" } }, isError = true }
  end,
})
'''
LUA_TOOLS = GREET + FLAKY + BOOM + SOFT
LUA_CMDS = '''pi.registerCommand("hello", { description = "Say hello", handler = function(args, ctx)
  print(json.encode({ command = "hello", args = args }))
end })
pi.registerCommand("quiet", { handler = function(args, ctx)
  print(json.encode({ command = "quiet", args = args }))
end })
'''
LUA_DUP = '''pi.registerCommand("hello", { description = "Say hello from %s", handler = function(args, ctx)
  print(json.encode({ command = "hello", from = "%s", args = args }))
end })
'''
L4 = {
    'ext_tool': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_results': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_allow': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_named': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_only': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_none': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_minus': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_plus_none': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_pattern': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_tool_default': {'agent/extensions/t.lua': LUA_TOOLS},
    'ext_cmd': {'agent/extensions/t.lua': LUA_CMDS},
    'ext_cmd_dup': {'agent/extensions/a.lua': LUA_DUP % ('a', 'a'), 'agent/extensions/b.lua': LUA_DUP % ('b', 'b')},
    'ext_cmd_scope': {'project/.pi/extensions/p.lua': LUA_DUP % ('project', 'project'),
                      'agent/extensions/dirx/index.lua': LUA_DUP % ('user', 'user')},
}
for name, files in L4.items():
    p = os.path.join(D, name + '.cfg.json')
    if not os.path.exists(p):
        continue
    cfg = json.load(open(p))
    cfg['golden'] = name
    cfg['text_files'] = files
    if cfg.get('collect'):
        cfg['lua_dump'] = cfg['collect'][0]
        cfg['canonical'] = True
    open(os.path.join(D, 'ext_lua_' + name[4:] + '.cfg.json'), 'w').write(json.dumps(cfg, indent=1) + '\n')
    print('wrote ext_lua_' + name[4:])
