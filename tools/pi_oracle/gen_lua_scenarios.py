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
    if not m or m.group(1).startswith('ext_lua') or m.group(1).startswith(('ext_tool', 'ext_cmd', 'ext_entry', 'ext_ui')):
        continue
    name = m.group(1)
    cfg = json.load(open(os.path.join(D, f)))
    if 'hooks' not in cfg.get('settings', {}):
        continue                     # no hook-command twin: the L7 section below writes its Lua by hand
    lua = lua_of(cfg)
    del cfg['settings']['hooks']
    cfg['golden'] = name
    cfg['text_files'] = {'agent/extensions/t.lua': lua}
    if cfg.get('collect') and cfg['collect'][0] == 'dump.jsonl':     # the event recorders; session.jsonl is the session file
        cfg['lua_dump'] = cfg['collect'][0]
        cfg['canonical'] = True
    open(os.path.join(D, 'ext_lua_' + name[4:] + '.cfg.json'), 'w').write(json.dumps(cfg, indent=1) + '\n')
    print('wrote ext_lua_' + name[4:])


# ---- L4, L6: the scenarios of rpc_gen.py that register tools, commands and entries (ext_tool*, ext_cmd*, ext_entry): the Lua
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
LUA_ENTRY = '''pi.on("session_start", function() pi.appendEntry("boot", { n = 1 }) end)
pi.registerTool({
  name = "note", label = "note", description = "note tool.",
  parameters = { type = "object", properties = { text = { type = "string" } }, required = { "text" } },
  execute = function(toolCallId, params, signal, onUpdate, ctx)
    pi.appendEntry("note", { text = params.text, tags = { "a", "b" } })
    pi.appendEntry("bare")
    return { content = { { type = "text", text = "noted" } }, details = {} }
  end,
})
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
    'ext_entry': {'agent/extensions/t.lua': LUA_ENTRY},
}
for name, files in L4.items():
    p = os.path.join(D, name + '.cfg.json')
    if not os.path.exists(p):
        continue
    cfg = json.load(open(p))
    cfg['golden'] = name
    cfg['text_files'] = files
    if cfg.get('collect'):
        cfg['lua_dump'] = cfg['collect'][0] if name != 'ext_entry' else None
        cfg['canonical'] = True
        if cfg['lua_dump'] is None:
            del cfg['lua_dump']
    open(os.path.join(D, 'ext_lua_' + name[4:] + '.cfg.json'), 'w').write(json.dumps(cfg, indent=1) + '\n')
    print('wrote ext_lua_' + name[4:])


# ---- L5: ctx.ui from a Lua extension. The goldens are pi 1.1.0's with a TypeScript extension that does the same (rpc_gen.py:
# ui_dialogs, ui_cancel, ext_ui_*); the reason of the block carries what the dialogs gave back, written as JS writes it
# (undefined for nothing), so the twin has to give nil where pi gives undefined and false where it gives false. ----
LUA_SHOW = '''local function show(v)
  if v == nil then return "undefined" end
  return tostring(v)
end
'''
LUA_UI = LUA_SHOW + '''pi.on("tool_call", function(event, ctx)
  local choice = ctx.ui.select("Run it?", { "Allow", "Block" })
  local ok = ctx.ui.confirm("Sure?", "It runs a command.")
  local why = ctx.ui.input("Why?", "a reason")
  ctx.ui.notify("decided " .. show(choice) .. " " .. show(ok) .. " " .. show(why), "info")
  if choice ~= "Allow" then return { block = true, reason = "Blocked by the user" } end
end)
'''
LUA_UI_TIMEOUT = LUA_SHOW + '''pi.on("tool_call", function(event, ctx)
  local a = ctx.ui.select("Pick", { "One", "Two" }, { timeout = 5000 })
  local b = ctx.ui.confirm("Sure?", "It runs a command.", { timeout = 250 })
  local c = ctx.ui.input("Why?", "a reason", { timeout = 250 })
  return { block = true, reason = "select=" .. show(a) .. " confirm=" .. show(b) .. " input=" .. show(c) .. " hasUI=" .. show(ctx.hasUI) }
end)
'''
LUA_UI_NOTIFY = '''pi.on("tool_call", function(event, ctx)
  ctx.ui.notify("plain")
  ctx.ui.notify("one", "info")
  ctx.ui.notify("two", "warning")
  ctx.ui.notify("three", "error")
  return { block = true, reason = "notified" }
end)
'''
LUA_UI_PRINT = LUA_SHOW + '''pi.on("tool_call", function(event, ctx)
  local a = ctx.ui.select("Pick", { "One", "Two" })
  local b = ctx.ui.confirm("Sure?", "It runs a command.")
  local c = ctx.ui.input("Why?", "a reason")
  ctx.ui.notify("lost", "error")
  return { block = true, reason = "hasUI=" .. show(ctx.hasUI) .. " select=" .. show(a) .. " confirm=" .. show(b) .. " input=" .. show(c) }
end)
'''
L5 = {'ui_dialogs': LUA_UI, 'ui_cancel': LUA_UI, 'ext_ui_timeout': LUA_UI_TIMEOUT, 'ext_ui_notify': LUA_UI_NOTIFY,
      'ext_ui_print': LUA_UI_PRINT}
for name, lua in L5.items():
    cfg = json.load(open(os.path.join(D, name + '.cfg.json')))
    cfg['golden'] = name
    cfg['text_files'] = {'agent/extensions/t.lua': lua}
    twin = 'ext_lua_' + (name if name.startswith('ui_') else name[4:])
    open(os.path.join(D, twin + '.cfg.json'), 'w').write(json.dumps(cfg, indent=1) + '\n')
    print('wrote ' + twin)


# ---- L7: the handlers that edit in place or keep state (no hook-command twin); the golden is pi's run of the TypeScript one ----
L7 = {
    'ext_ctx_inplace': '''pi.on("context_with_system", function(event, ctx)
  for _, m in ipairs(event.messages) do
    if m.role == "user" then m.content[1].text = m.content[1].text .. " [seen]" end
  end
end)
''',
    'ext_compact_own': '''pi.on("session_before_compact", function(event, ctx)
  return { compaction = { summary = "by the extension", firstKeptEntryId = event.preparation.firstKeptEntryId, tokensBefore = 777 } }
end)
''',
    'ext_settle_continue': '''local n = 0
pi.on("agent_before_settle", function(event, ctx)
  n = n + 1
  if n == 1 then
    return { entries = { { type = "custom_message", customType = "more", content = "keep going", display = true } }, continue = true }
  end
end)
''',
    'ext_resources': '''pi.on("resources_discover", function(event, ctx)
  return { promptPaths = { event.cwd .. "/extra" } }
end)
''',
}
for name, lua in L7.items():
    cfg = json.load(open(os.path.join(D, name + '.cfg.json')))
    cfg['golden'] = name
    cfg['text_files'] = dict(cfg.get('text_files', {}), **{'agent/extensions/t.lua': lua})
    open(os.path.join(D, 'ext_lua_' + name[4:] + '.cfg.json'), 'w').write(json.dumps(cfg, indent=1) + '\n')
    print('wrote ext_lua_' + name[4:])
