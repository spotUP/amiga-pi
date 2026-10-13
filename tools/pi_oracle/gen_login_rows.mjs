// The rows of pi 1.1.0's /login provider selector, for tests/test_pi_tui.c (the tui goldens login_providers, login_search,
// login_apikey show pi's whole list, which amiga-pi has only in part): getLoginProviderOptions of interactive-mode.ts over the
// builtin providers of pi-ai (the same list the ModelRuntime holds), sorted by name. Writes tests/pi/tui/login_rows.txt:
// one line per row: <authType> TAB <id> TAB <name> TAB <login method's name> TAB <1 when subscription>
//   [PI_NPM=<pi install>] node tools/pi_oracle/gen_login_rows.mjs
import { writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const here = dirname(fileURLToPath(import.meta.url));
const npm = process.env.PI_NPM || join(here, "../../vendor/pi-1.1.0");
const all = await import(pathToFileURL(join(npm, "node_modules/@earendil-works/pi-ai/dist/providers/all.js")).href);
const options = [];
for (const provider of all.builtinProviders()) {
  const subscription = provider.auth.oauth?.isSubscription === true;
  if (provider.auth.oauth) options.push({ id: provider.id, name: provider.name, authType: "oauth", method: provider.auth.oauth.name, subscription });
  if (provider.auth.apiKey) options.push({ id: provider.id, name: provider.name, authType: "api_key", method: provider.auth.apiKey.name, subscription });
}
// the capture's models.json provider "fake" (an API key from models.json) and llama.cpp (a provider pi adds itself; its method name is not read)
options.push({ id: "fake", name: "fake", authType: "api_key", method: "", subscription: false });
options.push({ id: "llama.cpp", name: "llama.cpp", authType: "api_key", method: "", subscription: false });
options.sort((a, b) => a.name.localeCompare(b.name));
writeFileSync(join(here, "../../tests/pi/tui/login_rows.txt"),
  options.map((o) => [o.authType, o.id, o.name, o.method ?? "", o.subscription ? "1" : "0"].join("\t")).join("\n") + "\n");
console.log("[OK] login_rows.txt " + options.length + " rows");
