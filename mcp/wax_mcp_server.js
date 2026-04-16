#!/usr/bin/env node
// ============================================================
//  WAX MCP Bridge — connects VS Code agents to WAX C++ RAG
//  server via JSON-RPC over HTTP, and to the Blueprint semantic
//  search stack (Elasticsearch + local embedding service).
//
//  Tools exposed:
//    wax_recall               — BM25 search over indexed code + facts (C++ path)
//    wax_remember             — store knowledge for future recall
//    wax_fact_search          — search structured facts by entity prefix (EAV)
//    wax_blueprint_read       — read exported Blueprint JSON
//    wax_blueprint_*          — compressed_read / patch / write / import
//    wax_bp_semantic_search   — semantic (kNN / BM25 / hybrid) over indexed BPs
//    wax_bp_facts             — exact BP lookup by entity (from ES)
//
//  Env:
//    WAX_URL              — WAX C++ server URL (default http://127.0.0.1:8080)
//    WAX_EMBED_URL        — embedding service URL (default http://127.0.0.1:8088)
//    WAX_ES_URL           — Elasticsearch URL (default http://127.0.0.1:9200)
//    WAX_ES_BP_INDEX      — BP index name (default wax_bp_v1)
// ============================================================

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
// Patch compiler is now in C++ (blueprint.patch endpoint)

const WAX_URL       = process.env.WAX_URL       || "http://127.0.0.1:8080";
const EMBED_URL     = process.env.WAX_EMBED_URL || "http://127.0.0.1:8088";
const ES_URL        = process.env.WAX_ES_URL    || "http://127.0.0.1:9200";
const ES_BP_INDEX   = process.env.WAX_ES_BP_INDEX || "wax_bp_v1";

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

  // Server may return plain text (e.g., "OK") or JSON.
  // Parse defensively to handle both.
  const text = await res.text();
  let json;
  try {
    json = JSON.parse(text);
  } catch {
    // Plain text response (e.g., "OK", "Error: ...") — wrap it.
    return text;
  }

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

// ── Embedding service + Elasticsearch helpers ────────────────

async function embedQuery(text) {
  const res = await fetch(`${EMBED_URL}/embed`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text, is_query: true }),
  });
  if (!res.ok) {
    throw new Error(`Embed HTTP ${res.status}: ${await res.text()}`);
  }
  const j = await res.json();
  return j.vector;
}

async function esSearch(body) {
  const res = await fetch(`${ES_URL}/${ES_BP_INDEX}/_search`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    throw new Error(`ES HTTP ${res.status}: ${await res.text()}`);
  }
  return await res.json();
}

async function esGetDoc(id) {
  const url = `${ES_URL}/${ES_BP_INDEX}/_doc/${encodeURIComponent(id)}?_source_excludes=embedding`;
  const res = await fetch(url);
  if (res.status === 404) return null;
  if (!res.ok) {
    throw new Error(`ES HTTP ${res.status}: ${await res.text()}`);
  }
  return await res.json();
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
        "Read a UE Blueprint's FULL exported JSON (nodes, pins, links, variables). " +
        "WARNING: Full JSON is often 100K+ chars and may exceed context limits. " +
        "PREFER wax_blueprint_compressed_read for most use cases — it returns only " +
        "semantic data (node titles, function calls, events) at 10-15x smaller size. " +
        "Use this tool only when you need raw pin IDs or link GUIDs.",
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
    {
      name: "wax_blueprint_patch",
      description:
        "Apply a Blueprint Intent patch (.bpi_json) to an existing blueprint. " +
        "Reads current blueprint, merges intent (add/remove nodes and links), writes result. " +
        "Intent format uses ref names instead of GUIDs, existing:Title for existing nodes. " +
        "For creating new blueprints, set create:true in the intent.",
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
          intent_json: {
            type: "string",
            description: "Blueprint Intent JSON (.bpi_json) string with add_nodes, add_links, etc.",
          },
        },
        required: ["blueprint_path", "export_dir", "intent_json"],
      },
    },
    {
      name: "wax_blueprint_import",
      description:
        "Import modified .bpl_json files into UE5 via the BlueprintGraphImport commandlet. " +
        "Compiles blueprints and saves packages. Returns stdout/stderr for error handling.",
      inputSchema: {
        type: "object",
        properties: {
          ue_editor: {
            type: "string",
            description: "Path to UnrealEditor-Cmd.exe",
          },
          uproject: {
            type: "string",
            description: "Path to .uproject file",
          },
          import_dir: {
            type: "string",
            description: "Path to directory with .bpl_json files",
          },
          compile: {
            type: "boolean",
            description: "Compile after import (default true)",
          },
          save: {
            type: "boolean",
            description: "Save packages after import (default true)",
          },
        },
        required: ["ue_editor", "uproject", "import_dir"],
      },
    },
    {
      name: "wax_blueprint_compressed_read",
      description:
        "Read a compressed view of a Blueprint — strips pins, GUIDs, links, positions. " +
        "Returns only semantic data (node titles, function calls, events, variables). " +
        "Use this for LLM context instead of full blueprint_read.",
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
      name: "wax_bp_semantic_search",
      description:
        "Semantic search over indexed Blueprints (hybrid vector + BM25). " +
        "Use when you need to FIND a Blueprint by what it DOES, not by exact name. " +
        "Returns top-K hits with kind, parent_class, purpose, and exec_chains. " +
        "Good queries: 'disables player input temporarily', 'handles weapon pickup with ammo', " +
        "'ability that spawns a tagged actor'. " +
        "Prefer this over wax_recall when searching for Blueprints.",
      inputSchema: {
        type: "object",
        properties: {
          query: {
            type: "string",
            description: "Natural language description of what the Blueprint does",
          },
          k: {
            type: "number",
            description: "Number of hits to return (default 5, max 20)",
          },
          mode: {
            type: "string",
            enum: ["knn", "bm25", "hybrid"],
            description:
              "knn = pure vector similarity, bm25 = keyword search, " +
              "hybrid = weighted combination of both (default, recommended)",
          },
          kind_filter: {
            type: "string",
            description:
              "Optional filter by kind: gameplay_ability, gameplay_effect, gameplay_cue, " +
              "anim_blueprint, anim_notify, widget, actor_blueprint, blueprint",
          },
        },
        required: ["query"],
      },
    },
    {
      name: "wax_bp_facts",
      description:
        "Exact Blueprint lookup by entity id. Returns all structural facts for a " +
        "single BP: kind, parent_class, events, calls, variables, exec_chains, purpose. " +
        "Use this AFTER wax_bp_semantic_search to get full details on a specific BP, " +
        "or when you already know the BP name.",
      inputSchema: {
        type: "object",
        properties: {
          entity: {
            type: "string",
            description: "Full entity id, e.g. 'bp:GA_SpawnEffect'. Must start with 'bp:'.",
          },
        },
        required: ["entity"],
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

        const isError = result && typeof result === "object" && result.error;
        const text = isError
          ? `Error: ${result.error}`
          : (result && typeof result === "object" && result.status)
            ? `Remembered (${result.status})`
            : `Remembered: ${String(result)}`;

        return {
          content: [{ type: "text", text }],
          isError: !!isError,
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

        const fullJson = result.json || JSON.stringify(result);

        // Auto-fallback to compressed_read if full JSON is too large for context.
        // 40K chars ≈ 10K tokens — safe limit to avoid MCP truncation.
        if (fullJson.length > 40000) {
          const compressed = await callWax("blueprint.compressed_read", {
            blueprint_path: args.blueprint_path,
            export_dir: args.export_dir,
          });

          if (compressed && typeof compressed === "object" && !compressed.error) {
            const ratio = compressed.original_size > 0
              ? (compressed.original_size / compressed.compressed_size).toFixed(1)
              : "?";
            return {
              content: [{ type: "text", text:
                `⚠ Full blueprint JSON too large (${fullJson.length} chars). ` +
                `Auto-switched to compressed_read (${ratio}x smaller).\n` +
                `Use wax_blueprint_compressed_read directly to avoid this fallback.\n\n` +
                compressed.json }],
            };
          }
        }

        return {
          content: [{ type: "text", text: fullJson }],
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

      case "wax_blueprint_patch": {
        const result = await callWax("blueprint.patch", {
          blueprint_path: args.blueprint_path,
          export_dir: args.export_dir,
          intent_json: args.intent_json,
        });

        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Patch response: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        if (result.error) {
          return {
            content: [{ type: "text", text: `Patch error: ${result.error}` }],
            isError: true,
          };
        }

        return {
          content: [{ type: "text", text: `${result.summary}\nWritten ${result.bytes_written} bytes to ${result.file_path}` }],
        };
      }

      case "wax_blueprint_import": {
        const result = await callWax("blueprint.import", {
          ue_editor: args.ue_editor,
          uproject: args.uproject,
          import_dir: args.import_dir,
          compile: args.compile !== false,
          save: args.save !== false,
        });

        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Import response: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        const success = !result.error && (result.exit_code === 0 || result.exit_code === undefined);
        const text = success
          ? `Import successful.\n${result.stdout || ""}`
          : `Import failed (exit ${result.exit_code}).\nstdout: ${result.stdout || ""}\nstderr: ${result.stderr || ""}\nerror: ${result.error || ""}`;

        return {
          content: [{ type: "text", text }],
          isError: !success,
        };
      }

      case "wax_blueprint_compressed_read": {
        const result = await callWax("blueprint.compressed_read", {
          blueprint_path: args.blueprint_path,
          export_dir: args.export_dir,
        });

        if (result == null || typeof result !== "object") {
          return {
            content: [{ type: "text", text: `Compressed read error: ${String(result ?? "empty")}` }],
            isError: true,
          };
        }

        if (result.error) {
          return {
            content: [{ type: "text", text: `Error: ${result.error}` }],
            isError: true,
          };
        }

        const ratio = result.original_size > 0
          ? (result.original_size / result.compressed_size).toFixed(1)
          : "?";

        return {
          content: [{ type: "text", text: `Compressed ${ratio}x (${result.original_size} → ${result.compressed_size} bytes)\n\n${result.json}` }],
        };
      }

      case "wax_bp_semantic_search": {
        const query = (args.query || "").toString();
        if (!query.trim()) {
          return {
            content: [{ type: "text", text: "Error: query is required" }],
            isError: true,
          };
        }
        const k = Math.min(20, Math.max(1, Math.round(args.k ?? 5)));
        const mode = ["knn", "bm25", "hybrid"].includes(args.mode) ? args.mode : "hybrid";
        const kindFilter = (args.kind_filter || "").toString().trim();

        // Build ES request body based on mode.
        let body;
        if (mode === "bm25") {
          // Pure keyword — no embedding call needed.
          body = {
            size: k,
            _source: { excludes: ["embedding"] },
            query: {
              multi_match: {
                query,
                fields: ["purpose^2", "asset_name^2", "text", "exec_chain", "calls"],
              },
            },
          };
        } else {
          // knn or hybrid — embedding required.
          const vec = await embedQuery(query);
          if (mode === "knn") {
            body = {
              size: k,
              _source: { excludes: ["embedding"] },
              knn: {
                field: "embedding",
                query_vector: vec,
                k,
                num_candidates: Math.max(100, k * 20),
              },
            };
          } else {
            // hybrid: kNN (weight 0.7) + BM25 (weight 0.3), fused by ES _score.
            body = {
              size: k,
              _source: { excludes: ["embedding"] },
              knn: {
                field: "embedding",
                query_vector: vec,
                k: k * 2,
                num_candidates: Math.max(200, k * 20),
                boost: 0.7,
              },
              query: {
                multi_match: {
                  query,
                  fields: ["purpose^2", "asset_name^2", "text", "exec_chain", "calls"],
                  boost: 0.3,
                },
              },
            };
          }
        }

        if (kindFilter) {
          const filter = { term: { kind: kindFilter } };
          if (body.knn) body.knn.filter = filter;
          if (body.query) {
            body.query = { bool: { must: [body.query], filter: [filter] } };
          } else {
            body.query = { bool: { filter: [filter] } };
          }
        }

        const resp = await esSearch(body);
        const hits = resp.hits?.hits ?? [];
        if (hits.length === 0) {
          return {
            content: [{ type: "text", text: `No Blueprints matched "${query}" (mode=${mode})` }],
          };
        }

        const lines = hits.map((h, i) => {
          const s = h._source || {};
          const parts = [
            `[${i + 1}] score=${h._score?.toFixed(3) ?? "?"}  ${s.entity}  [${s.kind}]`,
          ];
          if (s.purpose) parts.push(`    purpose: ${s.purpose}`);
          if (s.parent_class) parts.push(`    parent:  ${s.parent_class}`);
          if (s.exec_chain) {
            const chain = s.exec_chain.length > 200 ? s.exec_chain.slice(0, 200) + "…" : s.exec_chain;
            parts.push(`    exec:    ${chain}`);
          }
          return parts.join("\n");
        });
        const header = `Found ${hits.length} Blueprint(s) for "${query}" (mode=${mode}${kindFilter ? `, kind=${kindFilter}` : ""}):`;
        return {
          content: [{ type: "text", text: `${header}\n\n${lines.join("\n\n")}` }],
        };
      }

      case "wax_bp_facts": {
        const entity = (args.entity || "").toString().trim();
        if (!entity) {
          return {
            content: [{ type: "text", text: "Error: entity is required" }],
            isError: true,
          };
        }
        if (!entity.startsWith("bp:")) {
          return {
            content: [{
              type: "text",
              text: `Error: entity must start with 'bp:' (got ${JSON.stringify(entity)})`,
            }],
            isError: true,
          };
        }

        const doc = await esGetDoc(entity);
        if (!doc || !doc.found) {
          return {
            content: [{ type: "text", text: `Blueprint ${entity} not found in index` }],
            isError: true,
          };
        }
        const s = doc._source;

        // Format as structured readable text.
        const sections = [];
        sections.push(`Entity: ${s.entity}`);
        sections.push(`Kind: ${s.kind}   parent_class: ${s.parent_class || "?"}`);
        if (s.asset_path) sections.push(`Asset path: ${s.asset_path}`);
        sections.push(`Nodes: ${s.node_count}   Links: ${s.link_count}`);
        if (s.purpose) {
          sections.push("");
          sections.push(`Purpose:\n  ${s.purpose}`);
        }
        if (s.exec_chain) {
          sections.push("");
          sections.push(`Exec chains:\n  ${s.exec_chain}`);
        }

        const listIfAny = (label, arr) => {
          if (!arr || !arr.length) return null;
          return `${label} (${arr.length}):\n  ${arr.join(", ")}`;
        };
        for (const line of [
          listIfAny("Events", s.events),
          listIfAny("Custom events", s.custom_events),
          listIfAny("Calls", s.calls),
          listIfAny("Call owners", s.calls_owners),
          listIfAny("Variables", s.variables),
          listIfAny("Casts to", s.casts_to),
          listIfAny("Macros", s.macros),
        ]) {
          if (line) { sections.push(""); sections.push(line); }
        }

        return {
          content: [{ type: "text", text: sections.join("\n") }],
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
