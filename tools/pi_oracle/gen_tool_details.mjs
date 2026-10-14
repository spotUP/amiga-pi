// gen_tool_details -- the oracle of the `details` amiga-pi's tools return (U1b): pi 1.1.0's own read, edit, ls, find, grep and bash tools run on
// a fixed tree; prints one line "<name>\t<details JSON>" per case (the text of details.truncation is cut to its first 40 characters: the test
// compares that member's start). tests/test_pi_tools.c test_details holds the same cases.
//   node tools/pi_oracle/gen_tool_details.mjs
import { npmRoot } from "./pin.mjs";
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
const root = npmRoot + '/';
const T = await import(root + '@earendil-works/pi-coding-agent/dist/core/tools/index.js');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pidet-'));
const w = (n, s) => { fs.mkdirSync(path.dirname(path.join(dir, n)), { recursive: true }); fs.writeFileSync(path.join(dir, n), s); };
w('big.txt', Array.from({ length: 2500 }, (_, i) => 'Line ' + (i + 1)).join('\n'));
w('wide.txt', Array.from({ length: 500 }, (_, i) => 'Line ' + (i + 1) + ': ' + 'x'.repeat(200)).join('\n'));
w('small.txt', 'one\ntwo\nthree\n');
w('edit.txt', 'a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nl\n');
for (let i = 1; i <= 8; i++) w('files/f' + i + '.txt', 'x\n');
w('grep/a.txt', Array.from({ length: 10 }, (_, i) => 'match ' + i).join('\n') + '\n');
w('grep/b.txt', 'match ' + 'y'.repeat(600) + '\n');
const out = [];
const run = async (name, tool, params) => {
  const r = await tool.execute('call_1', params);
  let d = r.details === undefined ? 'undefined' : JSON.stringify(r.details, (k, v) => (k === 'content' && typeof v === 'string' ? v.slice(0, 40) : k === 'fullOutputPath' ? '<path>' : v));
  out.push(name + '\t' + d);
};
await run('read big', T.createReadTool(dir), { path: 'big.txt' });
await run('read wide', T.createReadTool(dir), { path: 'wide.txt' });
await run('read small', T.createReadTool(dir), { path: 'small.txt' });
await run('read limit', T.createReadTool(dir), { path: 'big.txt', offset: 10, limit: 5 });
await run('edit', T.createEditTool(dir), { path: 'edit.txt', edits: [{ oldText: 'c\n', newText: 'C\nC2\n' }, { oldText: 'k\n', newText: 'K\n' }] });
await run('ls limit', T.createLsTool(dir), { path: 'files', limit: 3 });
await run('ls all', T.createLsTool(dir), { path: 'files' });
await run('find limit', T.createFindTool(dir), { pattern: '*.txt', path: 'files', limit: 2 });
await run('grep limit', T.createGrepTool(dir), { pattern: 'match', path: 'grep/a.txt', limit: 4 });
await run('grep long', T.createGrepTool(dir), { pattern: 'match', path: 'grep/b.txt' });
await run('bash big', T.createBashTool(dir), { command: 'seq 1 3000' });
await run('bash small', T.createBashTool(dir), { command: 'echo hi' });
console.log(out.join('\n'));
fs.rmSync(dir, { recursive: true, force: true });
