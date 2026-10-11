// mcp_gen.ts -- the expected outputs of tests/test_pi_mcp.c from pi's own MCP code
// (pi f10993bc): createMcpToolName, limitMcpContent, convertMcpResult,
// createMcpToolDefinition, the tool_search tool (BM25) and renderServersSection.
// Run by tools/pi_oracle/mcp_oracle.sh (PI_ORACLE_MCP: the prepared files).
// Writes tests/pi/mcp/{names,limit,convert,search,section}.expected.
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const dir = resolve(here, "..", "..", "tests", "pi", "mcp");
const O = process.env.PI_ORACLE_MCP!;
const T = await import(pathToFileURL(join(O, "tools.ts")).href);
const S = await import(pathToFileURL(join(O, "tool-search.ts")).href);
const R = await import(pathToFileURL(join(O, "section.ts")).href);

// ---- the shared text generator (mirrored in tests/test_pi_mcp.c: long_text) ----
export function longText(n: number): string {
	let s = "";
	for (let i = 0; Buffer.byteLength(s) < n; i++)
		s += `line ${String(i).padStart(4, "0")} of the long answer, with ü and € in it\n`;
	const b = Buffer.from(s).subarray(0, n);
	// drop a character cut in two at the end
	let e = b.length;
	let k = e - 1;
	while (k >= 0 && (b[k] & 0xc0) === 0x80) k--;
	if (k >= 0) {
		const lead = b[k];
		const want = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
		if (e - k < want) e = k;
	}
	return b.subarray(0, e).toString("utf8");
}

// the model content as lines: "text: <text, \n as \\n>" or "image: <mime> <data length>"
function blocks(content: { type: string; text?: string; data?: string; mimeType?: string }[]): string {
	return content
		.map((b) => (b.type === "text" ? `text: ${b.text!.replace(/\\/g, "\\\\").replace(/\n/g, "\\n")}` : `image: ${b.mimeType} ${b.data!.length}`))
		.join("\n");
}

// ---- names ----
{
	const cases: [string, string, boolean][] = [
		["fake", "echo", false],
		["my-server", "get.file/contents", false],
		["s", "x".repeat(60), false],
		["github", "a_tool_with_a_name_long_enough_to_pass_the_limit_of_64", false],
		["fake", "echo", true],
		["dev-radius", "do-it", false],
		["s", "üml", false],
	];
	const out = cases.map(([s, t, taken]) => `${s}|${t}|${taken ? 1 : 0} -> ${T.createMcpToolName(s, t, () => taken)}`);
	writeFileSync(join(dir, "names.expected"), out.join("\n") + "\n");
}

// ---- limitMcpContent ----
{
	const save = async () => "PATH";
	const out: string[] = [];
	const img = { type: "image", data: "AAAA", mimeType: "image/png" };
	const cases: [string, unknown[]][] = [
		["exact 20480", [{ type: "text", text: longText(20480) }]],
		["over 20481", [{ type: "text", text: longText(20481) }]],
		["long 30000", [{ type: "text", text: longText(30000) }]],
		["two blocks and an image", [{ type: "text", text: longText(12000) }, img, { type: "text", text: longText(9000) }]],
		["short", [{ type: "text", text: "short" }, img]],
	];
	for (const [name, content] of cases) {
		const r = await T.limitMcpContent(content, save);
		out.push(`== ${name} (cut ${r.fullOutputPath ? 1 : 0})`, blocks(r.content));
	}
	writeFileSync(join(dir, "limit.expected"), out.join("\n") + "\n");
}

// ---- convertMcpResult ----
{
	const results = JSON.parse(readFileSync(join(dir, "results.json"), "utf8"));
	const save = async (_d: unknown, ext: string) => `SAVED${ext}`;
	const out: string[] = [];
	for (const [name, res] of Object.entries(results) as [string, any][]) {
		const r = { ...res, content: res.content ?? [] };
		const c = await T.convertMcpResult("fake", name, r, { saveOutput: save });
		out.push(`== ${name} (isError ${c.isError ? 1 : 0})`, blocks(c.content));
	}
	writeFileSync(join(dir, "convert.expected"), out.join("\n") + "\n");
}

// ---- tool definitions and tool_search over the github toolset ----
{
	const set = JSON.parse(readFileSync(join(dir, "github.json"), "utf8"));
	const description = "GitHub: issues, pull requests and code";
	const namespace = { name: "mcp__gh", description, instructions: set.instructions };
	const defs = set.tools.map((tool: any) =>
		T.createMcpToolDefinition({
			server: "gh",
			tool,
			name: T.createMcpToolName("gh", tool.name),
			exposure: "deferred",
			namespace,
			timeoutMs: 1000,
			getClient: async () => {
				throw new Error("not called");
			},
		}),
	);
	const out: string[] = [];
	for (const d of defs) out.push(`tool ${d.name}`, `description ${d.description}`, `parameters ${JSON.stringify(d.parameters)}`);
	let active: string[] = [];
	const api = {
		getAllTools: () => defs,
		getActiveTools: () => active,
		setActiveTools: (names: string[]) => {
			active = names;
		},
	};
	const tool = S.createToolSearchToolDefinition({ tools: api });
	out.push(`search description ${JSON.stringify(tool.description)}`);
	const queries: [string, number | undefined][] = [
		["issue", undefined],
		["create pull request", 1],
		["file contents", undefined],
		["HTTP server", undefined],
		["comment on an issue", undefined],
		["the a of", undefined],
		["zzz", undefined],
		["issues", undefined],
		["pull request draft", 2],
		["repository branches", undefined],
	];
	for (const [q, limit] of queries) {
		const r = await tool.execute("id", limit === undefined ? { query: q } : { query: q, limit });
		out.push(`== ${q} (limit ${limit ?? "-"})`, r.content[0].text, `loaded ${r.details.loaded.join(",")}`);
	}
	writeFileSync(join(dir, "search.expected"), out.join("\n") + "\n");
}

// ---- the mcp_servers section ----
{
	const srv = (name: string, config: Record<string, unknown>, instructions?: string) => ({
		entry: { name, config },
		...(instructions ? { connection: { instructions } } : {}),
	});
	const long = "A server whose description goes on and on. ".repeat(10);
	const sets: [string, unknown[]][] = [
		["none", [srv("a", { command: "x", exposure: "direct" })]],
		[
			"three",
			[
				srv("zeta", { url: "http://h/mcp", exposure: "deferred", description: "  Last one.\nSecond line  " }),
				srv("alpha-one", { command: "x", exposure: "direct", toolExposure: { "get_*": "deferred" } }, "From the instructions.\nMore."),
				srv("off", { command: "x", exposure: "deferred", enabled: false }),
				srv("mid", { command: "x", exposure: "deferred" }),
				srv("long", { command: "x", exposure: "deferred", description: long }),
			],
		],
		[
			"many",
			Array.from({ length: 90 }, (_, i) =>
				srv(`server-with-a-rather-long-name-${String(i).padStart(3, "0")}`, { command: "x", exposure: "deferred", description: `Server number ${i}` }),
			),
		],
	];
	const out: string[] = [];
	for (const [name, servers] of sets) out.push(`== ${name}`, R.renderServersSection(servers) ?? "(none)");
	writeFileSync(join(dir, "section.expected"), out.join("\n") + "\n");
}
console.log("mcp oracle: names, limit, convert, search, section written to " + dir);
