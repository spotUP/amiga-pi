// gen_settings_errors -- the oracle of amiga-pi/settings_diag.c (U3): what pi 1.1.0 makes of a settings.json text.
// core/settings-manager.ts loadFromStorage: JSON.parse(stripBom(content)), then migrateSettings(settings), which asks
// `"queueMode" in settings` first (a TypeError for a primitive); an empty file is {}. The message is the Error's, from this
// machine's V8 (Node 24: positions with "(line L column C)").
//   node tools/pi_oracle/gen_settings_errors.mjs > tests/pi/settings/cases.json
const doc = '{"defaultProvider":"x","theme":"dark","retry":{"enabled":false,"maxRetries":3},"list":[1,2.5e3,-4,true,null,"a\\u00e9b"],"quietStartup":true}';
const cases = [];
function add(text) { cases.push(text); }
for (const t of ['"outputPad": 0', '{', '{"a":}', '{"a":1,}', '[1,]', "{'a':1}", '{"a":1', 'nonsense', '{"a": tru}', '{"a":"x', '{"a":"\\q"}',
  '{"a":"x\ny"}', '{"a":1}}', '{"a":1}\n{', '{"a" 1}', '{"a":01}', '{"a":-}', '{"a":1e}', '[', ']', '{,}', 'tru', 'nul', '{"a":"\\u12"}',
  '{\n  "a": 1,\n  "b": \n}', '{"a":1 "b":2}', '  ', '{} x', '"abc', '[1 2]', '{"a":NaN}', '{"a":.5}', '{"a":+1}', '1.', '5', 'true', 'null',
  '"abc"', '[]', '[1,2]', '{}', ' {} ', 'undefined', 'NaN', 'Infinity', '[object Object]', '﻿{"a":1', '﻿{}', '{"é":1 x}',
  '{"a":"é"}x', '{"a":1,\r\n  "b": }', '{"a":1,\r  "b": }', 'truex', 'nulx', '{"a":tru', '{"a":1}\u0000', '{"a":"😀"} x', '{"😀":1 x}',
  '{"a":"\t"}', '[1,2,3', '[1,', '{"a":', '{"a"', '{"a":1,', '-', '- 1', '1E+', '0.5.5', '00', '-01', '"\\u00zz"'])
  add(t);
const marks = [',', '}', '{', ']', '[', ':', '"', 'x', '0', '-', '\\', '\n', ' '];
for (let i = 0; i <= doc.length; i++) {
  add(doc.slice(0, i));                       // cut short
  if (i < doc.length) add(doc.slice(0, i) + doc.slice(i + 1));      // a character gone
  if (i < doc.length) for (const m of marks.slice(i % 3, i % 3 + 4)) add(doc.slice(0, i) + m + doc.slice(i + 1));
}
const long = '{"aaaaaaaaaaaaaaaaaaaa":1,"bbbbbbbbbbbbbbbbbbbbbbbb":2,"cccccccccccccccc":3,"d":4,"eeeeeeeeeeeeeeeeeeee":5}';
for (const i of [0, 3, 9, 10, 11, 30, 50, 80, long.length - 11, long.length - 10, long.length - 9, long.length - 1])
  add(long.slice(0, i) + ']' + long.slice(i + 1));
const out = [];
for (const text of new Set(cases)) {
  let message = null;
  if (!text) { out.push([text, null]); continue; }       // `if (!content) return {}`
  try {
    const s = JSON.parse(text.charCodeAt(0) === 0xfeff ? text.slice(1) : text);
    if (typeof s !== 'object' || s === null) "queueMode" in s;
  } catch (e) {
    message = e.message;
  }
  out.push([text, message]);
}
console.log(JSON.stringify(out));
