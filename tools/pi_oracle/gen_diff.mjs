// gen_diff -- the oracle of amiga-pi/jsdiff.c (U1b): what pi 1.1.0's own code makes of two texts.
//   lines: [0, old, new, diff, firstChangedLine, patch]   generateDiffString + generateUnifiedPatch (core/tools/edit-diff.ts)
//   words: [1, old, new, [[flag, value], ...]]            Diff.diffWords (components/diff.ts renderIntraLineDiff); flag 0 same, 1 added, 2 removed
//   node tools/pi_oracle/gen_diff.mjs > tests/pi/diff/cases.json
import { createRequire } from 'node:module';
const root = new URL('../../vendor/pi-1.1.0/node_modules/', import.meta.url).pathname;
const { generateDiffString, generateUnifiedPatch } = await import(root + '@earendil-works/pi-coding-agent/dist/core/tools/edit-diff.js');
const Diff = await import(root + 'diff/libesm/index.js');
let seed = 12345;
const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return (seed >> 8) % n; };
const pick = (a) => a[rnd(a.length)];
const cases = [];
const VOCAB = ['int main(void)', '{', '}', 'return 0;', '', '    x = 1;', '    y = 2;', '\tif (a) {', '\t}', '// note', 'foo bar baz', 'end', 'a', 'b', 'c',
  'printf("hi %d\\n", n);', '  ', 'été', 'x = 1  # comment'];
function lines(n) { const o = []; for (let i = 0; i < n; i++) o.push(rnd(4) ? pick(VOCAB) : 'line ' + i); return o; }
function mutate(ls) {
  const o = ls.slice();
  const k = 1 + rnd(3);
  for (let i = 0; i < k; i++) {
    const at = rnd(o.length + 1), m = rnd(4);
    if (m === 0) o.splice(at, 1 + rnd(3));
    else if (m === 1) o.splice(at, 0, ...lines(1 + rnd(3)));
    else if (m === 2) o.splice(at, 1, ...lines(1 + rnd(2)));
    else if (o[at] !== undefined) o[at] = o[at] + ' ' + pick(['x', 'tail', '']);
  }
  return o;
}
function join(ls) { return ls.join('\n') + (rnd(5) ? '\n' : ''); }
for (let i = 0; i < 700; i++) {
  const n = rnd(8) === 0 ? 40 + rnd(40) : 1 + rnd(14);
  const a = lines(n), b = mutate(a);
  const old = join(a), nw = rnd(9) ? join(b) : join(b).replace(/\n$/, '');
  const d = generateDiffString(old, nw);
  cases.push([0, old, nw, d.diff, d.firstChangedLine ?? 0, generateUnifiedPatch('f.txt', old, nw)]);
}
for (const [a, b] of [['', ''], ['', 'a\n'], ['a\n', ''], ['a', 'a\n'], ['a\n', 'a'], ['a\nb', 'a\nb\n'], ['x\ny\nz\n', 'x\ny\nz\n']]) {
  const d = generateDiffString(a, b);
  cases.push([0, a, b, d.diff, d.firstChangedLine ?? 0, generateUnifiedPatch('f.txt', a, b)]);
}
const WORDS = ['foo', 'bar', 'baz', 'x', 'y1', 'total', 'count', '(', ')', '=', '+', '"s"', ',', '->', ';', '{', '}', 'é', 'naïve', '_a', '0x1F', '42'];
const WS = [' ', ' ', ' ', '  ', '\t', '', '   '];
function sentence(n) { let s = pick(['', '    ', '  ', '\t']); for (let i = 0; i < n; i++) s += pick(WORDS) + pick(WS); return s.replace(/ +$/, rnd(3) ? '' : ' '); }
for (let i = 0; i < 1500; i++) {
  const n = 1 + rnd(9);
  const a = sentence(n);
  let t = a.split(/(\s+)/);
  const k = 1 + rnd(3);
  for (let j = 0; j < k; j++) {
    const at = rnd(t.length);
    const m = rnd(4);
    if (m === 0) t.splice(at, 1);
    else if (m === 1) t.splice(at, 0, pick(WORDS), pick(WS));
    else if (m === 2) t[at] = pick(WORDS);
    else t[at] = pick(WS) || ' ';
  }
  const b = t.join('');
  const parts = Diff.diffWords(a, b).map((p) => [p.added ? 1 : p.removed ? 2 : 0, p.value]);
  cases.push([1, a, b, parts]);
}
for (const [a, b] of [['', ''], ['a', ''], ['', 'a'], [' ', '  '], ['foo\tbar', 'foo bar'], ['a b', 'a\nb'], ['  x', 'x'], ['x  ', 'x'], [' a', 'a'],
  ['foo bar baz', 'foo baz'], ['foo bar baz', 'foo qux baz'], ['foo\nbar baz', 'foo baz'], ['foo baz', 'foo\nbar baz'], ['foo   bar baz', 'foo  baz'],
  [' a b', ' a c']]) {
  cases.push([1, a, b, Diff.diffWords(a, b).map((p) => [p.added ? 1 : p.removed ? 2 : 0, p.value])]);
}
process.stdout.write(JSON.stringify(cases) + '\n');
