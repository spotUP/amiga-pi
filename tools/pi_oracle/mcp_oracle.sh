#!/bin/sh
# mcp_oracle.sh -- the phase K oracle: pi's own MCP code (pi f10993bc) run by node,
# writing the expected outputs of tests/test_pi_mcp.c into tests/pi/mcp/*.expected.
#
#   tools/pi_oracle/mcp_oracle.sh            (PI_ORACLE: the oracle tree, default
#                                             ~/Code/amiga-pi/build/pi-oracle/oracle)
#
# It fetches, once, into $PI_ORACLE/mcp/: extensions/mcp/tools.ts,
# extensions/tool-search/tool.ts, core/mcp-servers.ts, core/tools/truncate.ts,
# pi-mcp's protocol/content.ts, and the part of extensions/mcp/index.ts that renders
# the mcp_servers section; the imports that only draw the TUI (pi-tui, render-utils,
# keybinding-hints, visual-truncate, output-files, typebox) point at small stubs.
# Then tools/pi_oracle/mcp_gen.ts runs over tests/pi/mcp/.
set -e
cd "$(dirname "$0")/../.."
PI=f10993bc7f28145df1375f3ff39c7f5c4cfc05f0
O=${PI_ORACLE:-$HOME/Code/amiga-pi/build/pi-oracle/oracle}/mcp
RAW=https://raw.githubusercontent.com/earendil-works/pi/$PI/packages
mkdir -p "$O"
get() { [ -f "$O/$2" ] || curl -sf "$RAW/$1" -o "$O/$2"; }
get coding-agent/src/extensions/mcp/tools.ts tools.orig.ts
get coding-agent/src/extensions/tool-search/tool.ts tool-search.orig.ts
get coding-agent/src/core/mcp-servers.ts mcp-servers.ts
get coding-agent/src/core/tools/truncate.ts truncate.ts
get mcp/src/protocol/content.ts content.ts
get coding-agent/src/extensions/mcp/index.ts index.orig.ts
sed -e 's#"@earendil-works/pi-mcp"#"./content.ts"#' \
    -e 's#"@earendil-works/pi-tui"#"./stubs.ts"#' \
    -e 's#"../../core/mcp-servers.ts"#"./mcp-servers.ts"#' \
    -e 's#"../../core/tools/render-utils.ts"#"./stubs.ts"#' \
    -e 's#"../../core/tools/truncate.ts"#"./truncate.ts"#' \
    -e 's#"../../modes/interactive/components/keybinding-hints.ts"#"./stubs.ts"#' \
    -e 's#"../../modes/interactive/components/visual-truncate.ts"#"./stubs.ts"#' \
    -e 's#"../../utils/output-files.ts"#"./stubs.ts"#' \
    "$O/tools.orig.ts" > "$O/tools.ts"
sed -e 's#import { type Static, Type } from "typebox";#import { Type } from "./stubs.ts"; type Static<T> = any;#' \
    "$O/tool-search.orig.ts" > "$O/tool-search.ts"
# the mcp_servers section: renderServersSection and the helpers above it (index.ts)
{
    echo 'import { mcpNamespace } from "./mcp-servers.ts";'
    echo 'type McpExposure = string; type McpServerEntry = any; type McpServer = any;'
    awk '/^function isEnabled/,/^}/' "$O/index.orig.ts"
    awk '/^function exposureOf/,/^}/' "$O/index.orig.ts"
    awk '/^function configuredExposures/,/^}/' "$O/index.orig.ts"
    awk '/^function hasIndirectTools/,/^}/' "$O/index.orig.ts"
    awk '/^export const MCP_SERVERS_SECTION/,/^export function renderServersSection/' "$O/index.orig.ts" | sed '$d'
    awk '/^export function renderServersSection/,/^}/' "$O/index.orig.ts"
} > "$O/section.ts"
cat > "$O/stubs.ts" <<'EOF'
export class Container { addChild() {} clear() {} }
export class Spacer { constructor(..._a: unknown[]) {} }
export class Text { constructor(..._a: unknown[]) {} setText() {} }
export class VisualLinePreview { constructor(..._a: unknown[]) {} }
export const formatToolCallWithArgs = () => "";
export const getTextOutput = () => "";
export const replaceTabs = (s: string) => s;
export const keyHint = () => "";
export async function writeOutputFile(): Promise<string> { throw new Error("not in the oracle"); }
export const Type = {
	Object: (properties: Record<string, unknown>) => ({ type: "object", properties }),
	String: (o: object = {}) => ({ type: "string", ...o }),
	Number: (o: object = {}) => ({ type: "number", ...o }),
	Optional: (s: object) => s,
};
EOF
PI_ORACLE_MCP="$O" node tools/pi_oracle/mcp_gen.ts
