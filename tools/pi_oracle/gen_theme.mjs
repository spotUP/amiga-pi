// gen_theme.mjs -- tests/pi/tui/system_theme.txt: pi 1.0.4's own `system` theme as it draws on a terminal that
// reports no colours (generateSystemThemeColors with nothing), one line per role: name, ANSI slot or "-", dim flag.
//   node tools/pi_oracle/gen_theme.mjs
import { writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { execSync } from "node:child_process";

const root = join(execSync("npm root -g").toString().trim(), "@earendil-works/pi-coding-agent/dist/modes/interactive/theme");
const { generateSystemThemeColors } = await import(pathToFileURL(join(root, "system-theme.js")).href);
const t = generateSystemThemeColors({});
const out = [];
for (const [k, v] of Object.entries(t.colors)) out.push(`${k} ${v === "" ? "-" : v} ${t.dim.includes(k) ? "dim" : "-"}`);
writeFileSync(join(dirname(fileURLToPath(import.meta.url)), "..", "..", "tests", "pi", "tui", "system_theme.txt"), out.join("\n") + "\n");
console.log(`[OK] ${out.length} roles`);
