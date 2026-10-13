// Preloaded into pi (NODE_OPTIONS=--import <this file>) by tools/pi_oracle/tui_gen.py for the login scenarios that need an OAuth
// server: every https://host/path fetch goes to $PI_FIXTURE/host/path, where tools/claude_fixture.py --oauth answers it. pi
// installs its own global fetch (core/http-dispatcher.js) after this file ran, so the global is a property that wraps whatever
// is assigned to it.
const base = process.env.PI_FIXTURE;
if (base) {
  let inner = globalThis.fetch;
  const wrapped = (input, init) => {
    const url = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
    if (!url.startsWith("https://")) return inner(input, init);
    const mapped = base + "/" + url.slice("https://".length);
    return inner(typeof input === "string" || input instanceof URL ? mapped : new Request(mapped, input), init);
  };
  Object.defineProperty(globalThis, "fetch", { configurable: true, enumerable: true, get: () => wrapped, set: (f) => { inner = f; } });
}
