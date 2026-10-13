// Goldens for tests/test_pi_tui.c test_slash_fuzzy: the order of the slash menu from pi 1.1.0's own CombinedAutocompleteProvider
// (pi-tui autocomplete.ts:343-372, fuzzy.ts) for every one- and two-letter query and some more (digits, tokens, skills), over a
// command list like the one of the tui goldens plus prompt templates and skills. Writes tests/pi/tui/fuzzy_cases.txt:
//   line 1: the command names, space separated, in menu order
//   then one line per query: <query><TAB><the names of the menu, space separated, in order>
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
  "review", "rewrite", "refactor", "re", "skill:greet", "skill:terse", "skill:review", "skill:data-2", "skill:x1y"];
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
const lines = [names.join(" ")];
for (const q of queries) lines.push(q + "\t" + (await menu(q)).join(" "));
writeFileSync(join(here, "../../tests/pi/tui/fuzzy_cases.txt"), lines.join("\n") + "\n");
console.log("[OK] fuzzy_cases.txt " + queries.length + " queries");
