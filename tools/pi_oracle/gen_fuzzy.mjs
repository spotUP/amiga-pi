// Goldens for tests/test_pi_tui.c test_slash_fuzzy: the order of the slash menu from pi 1.1.0's own CombinedAutocompleteProvider
// (pi-tui autocomplete.ts:343-372, fuzzy.ts) for every one- and two-letter query and some more (digits, tokens, skills), over a
// command list like the one of the tui goldens plus prompt templates and skills. Writes tests/pi/tui/fuzzy_cases.txt:
//   line 1: the command names, space separated, in menu order
//   then one line per query: <query><TAB><the names of the menu, space separated, in order>   (text "/" + query, cursor at its end)
//   then one line per cursor case: @<text before the cursor>\t<text after it>\t<the menu>   (\n and \t spelled as two characters;
//   the menu is empty off the editor's first line: editor.ts isSlashMenuAllowed, cursorLine === 0)
//   [PI_NPM=<pi install>] node tools/pi_oracle/gen_fuzzy.mjs
import { writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const here = dirname(fileURLToPath(import.meta.url));
const npm = process.env.PI_NPM || join(here, "../../vendor/pi-1.1.0");
const dist = join(npm, "node_modules/@earendil-works/pi-tui/dist");
const { CombinedAutocompleteProvider } = await import(pathToFileURL(join(dist, "autocomplete.js")).href);
const names = ["settings", "model", "tree", "thinking", "scoped-models", "export", "import", "share", "copy", "name", "session",
  "changelog", "hotkeys", "fork", "clone", "trust", "login", "logout", "new", "compact", "resume", "reload", "quit",
  "review", "rewrite", "refactor", "re", "skill:greet", "skill:terse", "skill:review", "skill:data-2", "skill:x1y",
  "caf\u00e9", "na\u00efve", "\u00c4rger", "\u65e5\u672c\u8a9e", "\u03c3\u03afγμα", "\u03bf\u03b4\u03c5\u03c3\u03c3\u03b5\u03c5\u03c2", "i\u0307stanbul",
  "\ud83d\ude00smile", "\ud801\udc28bc", "Stra\u00dfe", "\u01c6ungla"];
const provider = new CombinedAutocompleteProvider(names.map((n) => ({ name: n, description: "d " + n })), "/tmp", null);
const ac = new AbortController();
async function menu(q) {
  const r = await provider.getSuggestions(["/" + q], 0, q.length + 1, { signal: ac.signal });
  return r ? r.items.map((i) => i.value) : [];
}
const letters = "abcdefghijklmnopqrstuvwxyz";
const queries = [""];
for (const a of letters) queries.push(a);
for (const a of letters) for (const b of letters) queries.push(a + b);
queries.push("skill", "skill:", "skill:r", "gr", "greet", "re", "rev", "res", "mod", "model", "m/t", "x1", "1x", "y1", "data2", "2data", "data-2",
  "new", "quit", "sk", "ls", "tm", "ctl", "set", "reloa", "reload", "rv", "Re", "MODEL", "a-b", "-", "scoped", "s-m");
queries.push("\u00c9", "\u00e9", "caf", "cafe", "CAF\u00c9", "\u00ef", "na\u00ef", "\u00e4rger", "\u00c4R", "\u00e4", "\u65e5", "\u672c\u8a9e", "\u65e5\u8a9e", "\u8a9e\u672c",
  "\u03c3", "\u03a3", "\u03a3\u038aΓΜΑ", "\u03c3\u03afγμα", "\u039f\u0394\u03a5\u03a3\u03a3\u0395\u03a5\u03a3", "\u03bf\u03b4\u03c5\u03c3\u03c3\u03b5\u03c5\u03c3", "\u039f\u0394\u03a5\u03a3\u03a3\u0395\u03a5\u03c2",
  "\u0130", "\u0130s", "i\u0307s", "is", "\ud83d\ude00", "\ud83d\ude00s", "s\ud83d\ude00", "\ud801\udc00", "\ud801\udc00bc", "\u00df", "STRASSE", "stra\u00dfe", "\u01c5", "\u01c6",
  "\u212a", "\u212ae", "re\u00a0v", "\u3000re", "re\u2003", "\u00a0", "m\u3000t", "\u00c9\u00c9", "\u00e9\u0301");
const lines = [names.join(" ")];
for (const q of queries) lines.push(q + "\t" + (await menu(q)).join(" "));
// the cursor: [text before the cursor, text after it]
const cursors = [["/re", ""], ["/re", "view"], ["/", "model"], ["/mo", "del more"], ["/model ", "x"], ["/mod", "el arg"], ["  /re", ""],
  ["\t/re", ""], ["\u00a0/re", ""], ["\u3000/rev", ""], ["x/re", ""], ["/re", "\nfoo"], ["/re\nfo", "o"], ["hello\n/re", ""], ["/", "re"],
  ["/r", "e"], ["/s e", "x"], ["/re/", ""], ["/m/t", ""], ["/re\u00e9", ""], ["/caf", "\u00e9"], ["/caf\u00e9", ""], ["/\u65e5", "\u672c"],
  ["/\u03bf\u03b4\u03c5\u03c3\u03c3\u03b5\u03c5\u03a3", ""], ["/\ud83d\ude00", "s"], ["/", ""], ["", "/re"], [" ", "/re"], ["/ ", "re"],
  ["/mo", "\u00e9"], ["/sk", "ill:greet"], ["/skill:", "x"], ["/re\t", "x"], ["/re", " x"]];
for (const [before, after] of cursors) {
  const lns = (before + after).split("\n");
  const line0 = before.split("\n").length - 1, col = before.length - (before.lastIndexOf("\n") + 1);
  const slash = line0 === 0 && before.trimStart().startsWith("/");   // (else it is the file menu: not this one)
  const r = slash ? await provider.getSuggestions(lns, 0, col, { signal: ac.signal }) : null;
  const esc = (x) => x.replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/\t/g, "\\t");
  lines.push("@" + esc(before) + "\t" + esc(after) + "\t" + (r ? r.items.map((i) => i.value).join(" ") : ""));
}
writeFileSync(join(here, "../../tests/pi/tui/fuzzy_cases.txt"), lines.join("\n") + "\n");
console.log("[OK] fuzzy_cases.txt " + queries.length + " queries, " + cursors.length + " cursor cases");
