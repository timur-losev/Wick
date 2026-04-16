// Integration test for wax_bp_semantic_search and wax_bp_facts MCP tools.
//
// Runs the real wax_mcp_server.js as a stdio subprocess and speaks MCP JSON-RPC.
// Requires the embedding service (:8088) and Elasticsearch (:9200) to be running
// with the wax_bp_v1 index populated. If either is down, the integration tests
// are skipped with a clear message.
//
// Run:
//   node --test mcp/test/bp_tools.test.js
// or (from mcp/):
//   npm test

import { test, before, after, describe } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SERVER_SCRIPT = path.join(__dirname, "..", "wax_mcp_server.js");

const EMBED_URL = process.env.WAX_EMBED_URL || "http://127.0.0.1:8088";
const ES_URL    = process.env.WAX_ES_URL    || "http://127.0.0.1:9200";
const ES_INDEX  = process.env.WAX_ES_BP_INDEX || "wax_bp_v1";

// ── Service availability (used to skip tests that need live services) ──────

async function servicesAvailable() {
  try {
    const [embed, es] = await Promise.all([
      fetch(`${EMBED_URL}/health`, { signal: AbortSignal.timeout(2000) }),
      fetch(`${ES_URL}/${ES_INDEX}/_count`, { signal: AbortSignal.timeout(2000) }),
    ]);
    if (!embed.ok || !es.ok) return false;
    const count = (await es.json()).count ?? 0;
    return count > 0;
  } catch {
    return false;
  }
}

// ── MCP stdio client ───────────────────────────────────────────────────────

class McpClient {
  constructor() {
    this.proc = spawn(process.execPath, [SERVER_SCRIPT], {
      stdio: ["pipe", "pipe", "pipe"],
    });
    this.buf = "";
    this.pending = new Map();
    this.nextId = 1;

    this.proc.stdout.on("data", (chunk) => {
      this.buf += chunk.toString("utf8");
      let idx;
      while ((idx = this.buf.indexOf("\n")) >= 0) {
        const line = this.buf.slice(0, idx).trim();
        this.buf = this.buf.slice(idx + 1);
        if (!line) continue;
        try {
          const msg = JSON.parse(line);
          if (msg.id && this.pending.has(msg.id)) {
            this.pending.get(msg.id)(msg);
            this.pending.delete(msg.id);
          }
        } catch {
          // ignore non-JSON stderr bleed
        }
      }
    });

    // Suppress stderr for cleaner test output; log only on failure.
    this.stderr = "";
    this.proc.stderr.on("data", (chunk) => { this.stderr += chunk.toString("utf8"); });
  }

  async send(method, params, timeoutMs = 30000) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, resolve);
      this.proc.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`MCP timeout for ${method} (stderr: ${this.stderr.slice(-500)})`));
        }
      }, timeoutMs);
    });
  }

  async initialize() {
    await this.send("initialize", {
      protocolVersion: "2024-11-05",
      capabilities: {},
      clientInfo: { name: "wax-bp-test", version: "0.0.1" },
    });
    await this.send("notifications/initialized", {});
  }

  async listTools() {
    const msg = await this.send("tools/list", {});
    return msg.result.tools;
  }

  async callTool(name, args) {
    const msg = await this.send("tools/call", { name, arguments: args });
    return msg.result;
  }

  close() {
    this.proc.kill();
  }
}

// ── Suite ──────────────────────────────────────────────────────────────────

describe("wax_bp_* MCP tools", async () => {
  /** @type {McpClient} */
  let client;

  before(async () => {
    client = new McpClient();
    await client.initialize();
  });

  after(() => {
    client?.close();
  });

  // ── Smoke: tool registration (no services required) ─────────────────────

  test("registers wax_bp_* and wax_blueprint_refresh in tools/list", async () => {
    const tools = await client.listTools();
    const names = tools.map((t) => t.name);
    for (const expected of ["wax_bp_semantic_search", "wax_bp_facts", "wax_blueprint_refresh"]) {
      assert.ok(names.includes(expected), `missing ${expected}; got ${names.join(", ")}`);
    }
  });

  test("wax_blueprint_refresh has required input schema", async () => {
    const tools = await client.listTools();
    const t = tools.find((x) => x.name === "wax_blueprint_refresh");
    assert.ok(t, "wax_blueprint_refresh not found");
    assert.deepEqual(t.inputSchema.required, ["export_dir"]);
    assert.ok(t.inputSchema.properties.entity);
    assert.ok(t.inputSchema.properties.export_dir);
  });

  test("wax_bp_semantic_search has required input schema", async () => {
    const tools = await client.listTools();
    const t = tools.find((x) => x.name === "wax_bp_semantic_search");
    assert.ok(t, "wax_bp_semantic_search not found");
    assert.deepEqual(t.inputSchema.required, ["query"]);
    assert.ok(t.inputSchema.properties.query);
    assert.ok(t.inputSchema.properties.mode);
    assert.ok(t.inputSchema.properties.kind_filter);
  });

  test("wax_bp_facts has required input schema", async () => {
    const tools = await client.listTools();
    const t = tools.find((x) => x.name === "wax_bp_facts");
    assert.ok(t);
    assert.deepEqual(t.inputSchema.required, ["entity"]);
  });

  // ── Error paths: validation only, no live services needed ───────────────

  test("wax_bp_semantic_search rejects empty query", async () => {
    const r = await client.callTool("wax_bp_semantic_search", { query: "" });
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /query is required/i);
  });

  test("wax_bp_facts rejects empty entity", async () => {
    const r = await client.callTool("wax_bp_facts", { entity: "" });
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /entity is required/i);
  });

  test("wax_bp_facts rejects non-'bp:' prefix", async () => {
    const r = await client.callTool("wax_bp_facts", { entity: "cpp:AActor" });
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /must start with 'bp:'/);
  });

  test("wax_blueprint_refresh rejects missing export_dir", async () => {
    const r = await client.callTool("wax_blueprint_refresh", { entity: "bp:X" });
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /export_dir is required/i);
  });

  test("wax_blueprint_refresh rejects non-'bp:' entity", async () => {
    const r = await client.callTool("wax_blueprint_refresh", {
      entity: "cpp:Foo",
      export_dir: "J:/Temp/BlueprintExports",
    });
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /must start with 'bp:'/);
  });

  // ── Live-service tests ─────────────────────────────────────────────────

  const servicesUp = await servicesAvailable();

  test("[live] wax_bp_semantic_search returns matches for spawn-effect query",
    { skip: !servicesUp && "ES or embedding service unavailable, or wax_bp_v1 empty" },
    async () => {
      const r = await client.callTool("wax_bp_semantic_search", {
        query: "disables player input for a short time after respawn",
        k: 5,
        mode: "hybrid",
      });
      assert.ok(!r.isError, `search errored: ${r.content?.[0]?.text}`);
      const text = r.content[0].text;
      assert.match(text, /Found \d+ Blueprint/);
      // GA_SpawnEffect is the canonical hit for this query.
      assert.match(text, /bp:GA_SpawnEffect/);
    });

  test("[live] wax_bp_semantic_search respects kind_filter",
    { skip: !servicesUp && "services unavailable" },
    async () => {
      const r = await client.callTool("wax_bp_semantic_search", {
        query: "weapon fire ability",
        k: 5,
        mode: "knn",
        kind_filter: "gameplay_ability",
      });
      assert.ok(!r.isError);
      const text = r.content[0].text;
      // Every hit line like "[1] score=0.832  bp:X  [gameplay_ability]"
      const kindLines = text.split("\n").filter((l) => /\[[a-z_]+\]$/.test(l.trim()));
      assert.ok(kindLines.length > 0, "no hit lines found");
      for (const line of kindLines) {
        assert.match(line, /\[gameplay_ability\]$/,
          `kind_filter violated in line: ${line}`);
      }
    });

  test("[live] wax_bp_semantic_search bm25 mode skips embedding call",
    { skip: !servicesUp && "services unavailable" },
    async () => {
      // BM25 mode must work even if we pass a query that would have a non-matching vector.
      const r = await client.callTool("wax_bp_semantic_search", {
        query: "K2_ActivateAbility",
        mode: "bm25",
        k: 3,
      });
      assert.ok(!r.isError);
      assert.match(r.content[0].text, /Found \d+ Blueprint/);
    });

  test("[live] wax_bp_facts returns structured data for GA_SpawnEffect",
    { skip: !servicesUp && "services unavailable" },
    async () => {
      const r = await client.callTool("wax_bp_facts", { entity: "bp:GA_SpawnEffect" });
      assert.ok(!r.isError, `facts errored: ${r.content?.[0]?.text}`);
      const text = r.content[0].text;
      assert.match(text, /Entity: bp:GA_SpawnEffect/);
      assert.match(text, /Kind: gameplay_ability/);
      assert.match(text, /parent_class: GameplayAbility/);
      assert.match(text, /K2_ActivateAbility/);
      assert.match(text, /Calls \(\d+\):/);
      assert.match(text, /Variables \(\d+\):/);
      assert.match(text, /BP_ApplyGameplayEffectToSelf/);
    });

  test("[live] wax_bp_facts returns clean not-found for missing entity",
    { skip: !servicesUp && "services unavailable" },
    async () => {
      const r = await client.callTool("wax_bp_facts", {
        entity: "bp:SURELY_NOT_A_REAL_BLUEPRINT_XYZ",
      });
      assert.equal(r.isError, true);
      assert.match(r.content[0].text, /not found in index/);
    });

  const EXPORT_DIR = process.env.WAX_BP_EXPORT_DIR || "J:/Temp/BlueprintExports";

  test("[live] wax_blueprint_refresh returns 'unchanged' for an unmodified BP",
    { skip: !servicesUp && "services unavailable" },
    async () => {
      const r = await client.callTool("wax_blueprint_refresh", {
        entity: "bp:GA_SpawnEffect",
        export_dir: EXPORT_DIR,
      });
      assert.ok(!r.isError, `refresh errored: ${r.content?.[0]?.text}`);
      const text = r.content[0].text;
      // After the earlier run_full_reindex, GA_SpawnEffect is already indexed;
      // a fresh refresh with no file change must be a no-op.
      assert.match(text, /Status: unchanged/);
      assert.match(text, /Entity: bp:GA_SpawnEffect/);
    });

  test("[live] wax_blueprint_refresh reports not_found for missing BP",
    { skip: !servicesUp && "services unavailable" },
    async () => {
      const r = await client.callTool("wax_blueprint_refresh", {
        entity: "bp:DEFINITELY_MISSING_ASSET_12345",
        export_dir: EXPORT_DIR,
      });
      assert.equal(r.isError, true);
      assert.match(r.content[0].text, /Not found/i);
    });
});
