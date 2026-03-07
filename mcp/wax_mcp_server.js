#!/usr/bin/env node
// ============================================================
//  WAX MCP Bridge — connects VS Code agents to WAX C++ RAG
//  server via JSON-RPC over HTTP.
//
//  Tools exposed:
//    wax_recall          — BM25 search over indexed code + enriched facts
//    wax_remember        — store knowledge for future recall
//    wax_fact_search     — search structured facts by entity prefix
//    wax_blueprint_read  — read exported Blueprint JSON
//    wax_blueprint_write — write modified Blueprint JSON
//
//  Env:
//    WAX_URL  — WAX C++ server URL (default http://127.0.0.1:8080)
// ============================================================

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

const WAX_URL = process.env.WAX_URL || "http://127.0.0.1:8080";

// ── JSON-RPC helper ──────────────────────────────────────────

let rpcId = 0;

async function callWax(method, params = {}) {
  const body = JSON.stringify({
    jsonrpc: "2.0",
    id: ++rpcId,
    method,
    params,
  });

  const res = await fetch(WAX_URL, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body,
  });

  if (!res.ok) {
    throw new Error(`WAX HTTP ${res.status}: ${await res.text()}`);
  }

  const json = await res.json();

  // Support both response styles:
  // 1) JSON-RPC envelope: { jsonrpc, id, result|error }
  // 2) Direct JSON payload: { count, items, ... } or plain "OK"
  if (
    json &&
    typeof json === "object" &&
    (Object.prototype.hasOwnProperty.call(json, "jsonrpc") ||
      Object.prototype.hasOwnProperty.call(json, "result") ||
      Object.prototype.hasOwnProperty.call(json, "error"))
  ) {
    if (json.error) {
      const msg = json.error.message || JSON.stringify(json.error);
      throw new Error(`WAX RPC error: ${msg}`);
    }

    const raw = json.result;
    if (typeof raw === "string") {
      try {
        return JSON.parse(raw);
      } catch {
        return raw; // plain string like "OK"
      }
    }
    return raw;
  }

  const raw = json;
  if (typeof raw === "string") {
    try {
      return JSON.parse(raw);
    } catch {
      return raw; // plain string like "OK"
    }
  }
  return raw;
}

// ── MCP Server ───────────────────────────────────────────────

const server = new Server(
  { name: "wax", version: "0.1.0" },
  { capabilities: { tools: {} } }
);

// ── List Tools ───────────────────────────────────────────────

server.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: "wax_recall",
      description:
        "Search the WAX code index (725K+ UE5/Oliva code chunks + 649K enriched facts). " +
        "Returns relevant code chunks and LLM-extracted facts ranked by BM25+RRF. " +
        "Use for finding implementations, APIs, classes, patterns. " +
        "For targeted entity queries, use wax_fact_search instead.",
      inputSchema: {
        type: "object",
        properties: {
          query: {
            type: "string",
            description:
              "Search query — natural language or C++ symbol names " +
              '(e.g. "FTickFunction prerequisites", "AActor::BeginPlay")',
          },
        },
        required: ["query"],
      },
    },
    {
      name: "wax_remember",
      description:
        "Store a piece of knowledge in WAX long-term memory. " +
        "Use to save notes, conclusions, summaries, or structured " +
        "facts for future recall.",
      inputSchema: {
        type: "object",
        properties: {
          content: {
            type: "string",
            description: "Text content to remember",
          },
          metadata: {
            type: "object",
            description:
              "Optional key-value metadata. Useful keys: " +
              "symbol, source_kind, relative_path, language",
            additionalProperties: { type: "string" },
          },
        },
        required: ["content"],
      },
    },
    {
      name: "wax_fact_search",
      description:
        "Search structured facts extracted by LLM enrichment during indexing. " +
        "Facts are (entity, attribute, value) triples like: " +
        '(bp:BP_ThirdPersonCharacter, calls, K2_SetActorLocation). ' +
        'Use entity_prefix "bp:" for Blueprint facts, "cpp:" for C++ facts. ' +
        "Useful for answering: what does X call? what events does Y handle? " +
        "what variables does Z use?",
      inputSchema: {
        type: "object",
        properties: {
          entity_prefix: {
            type: "string",
            description:
              'Entity prefix to filter by (e.g. "bp:", "cpp:", ' +
              '"bp:BP_ThirdPersonCharacter")',
          },
          limit: {
            type: "number",
            description: "Max facts to return (default 100)",
          },
        },
        required: ["entity_prefix"],
      },
    },
    {
      name: "wax_blueprint_read",
      description:
        "Read a UE Blueprint's exported JSON by its asset path. " +
        "Returns the full Blueprint graph JSON (nodes, pins, links, variables). " +
        'Example blueprint_path: "/Game/Blueprints/BP_MyActor.BP_MyActor"',
      inputSchema: {
        type: "object",
        properties: {
          blueprint_path: {
            type: "string",
            description: "UE asset path (e.g. /Game/Blueprints/BP_MyActor.BP_MyActor)",
          },
          export_dir: {
            type: "string",
            description: "Path to BlueprintExports directory on disk",
          },
        },
        required: ["blueprint_path", "export_dir"],
      },
    },
    {
      name: "wax_blueprint_write",
      description:
        "Write modified Blueprint JSON back to the export directory. " +
        "Creates a .backup.json of the original before overwriting. " +
        "After writing, use blueprint.import to recompile into .uasset.",
      inputSchema: {
        type: "object",
        properties: {
          blueprint_path: {
            type: "string",
            description: "UE asset path (e.g. /Game/Blueprints/BP_MyActor.BP_MyActor)",
          },
          export_dir: {
            type: "string",
            description: "Path to BlueprintExports directory on disk",
          },
          json: {
            type: "string",
            description: "Full modified Blueprint JSON string",
          },
        },
        required: ["blueprint_path", "export_dir", "json"],
      },
    },
  ],
}));

// ── Call Tool ─────────────────────────────────────────────────

server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {
      case "wax_recall": {
        const result = await callWax("recall", { query: args.query });

        // Server may return error string instead of { items, count, total_tokens }
        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Recall response: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        // Format items for the agent: include text + score
        const items = result.items || [];
        const lines = items.map(
          (item, i) =>
            `--- [${i + 1}] score=${item.score?.toFixed(4) ?? "?"} ---\n${item.text}`
        );

        const summary = `Found ${result.count ?? items.length} items (${result.total_tokens ?? "?"} tokens)\n\n${lines.join("\n\n")}`;

        return {
          content: [{ type: "text", text: summary }],
        };
      }

      case "wax_remember": {
        const result = await callWax("remember", {
          content: args.content,
          metadata: args.metadata || {},
        });

        return {
          content: [{ type: "text", text: String(result) }],
        };
      }

      case "wax_fact_search": {
        const result = await callWax("fact.search", {
          entity_prefix: args.entity_prefix,
          limit: args.limit || 100,
        });

        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Fact search response: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        const facts = result.facts || [];
        const lines = facts.map(
          (f, i) => `[${i + 1}] ${f.entity} | ${f.attribute} | ${f.value}`
        );

        const summary = `Found ${result.count ?? facts.length} facts for prefix "${result.entity_prefix}"\n\n${lines.join("\n")}`;

        return {
          content: [{ type: "text", text: summary }],
        };
      }

      case "wax_blueprint_read": {
        const result = await callWax("blueprint.read", {
          blueprint_path: args.blueprint_path,
          export_dir: args.export_dir,
        });

        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Blueprint read error: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        if (result.error) {
          return {
            content: [{ type: "text", text: `Error: ${result.error}` }],
            isError: true,
          };
        }

        return {
          content: [{ type: "text", text: result.json || JSON.stringify(result) }],
        };
      }

      case "wax_blueprint_write": {
        const result = await callWax("blueprint.write", {
          blueprint_path: args.blueprint_path,
          export_dir: args.export_dir,
          json: args.json,
        });

        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Blueprint write response: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        if (result.error) {
          return {
            content: [{ type: "text", text: `Error: ${result.error}` }],
            isError: true,
          };
        }

        return {
          content: [{ type: "text", text: `Written ${result.bytes_written} bytes to ${result.file_path}` }],
        };
      }

      default:
        return {
          content: [{ type: "text", text: `Unknown tool: ${name}` }],
          isError: true,
        };
    }
  } catch (err) {
    return {
      content: [{ type: "text", text: `Error: ${err.message}` }],
      isError: true,
    };
  }
});

// ── Start ────────────────────────────────────────────────────

const transport = new StdioServerTransport();
await server.connect(transport);
