# Wick

> Local C++ RAG server for indexing, enriching, and searching codebases. All models run locally via llama.cpp — no cloud required.

Wick is a derivative of [Wax](https://github.com/christopherkarani/Wax) — ported from Swift to C++20, extended with an HTTP server, LLM enrichment pipeline, structured knowledge store, and MCP bridge for IDE integration. See [NOTICE](NOTICE) for full attribution.

> **Work in progress** — APIs, storage format, and configuration may change.

## Quick start

### 1. Build

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

### 2. Start a local LLM (llama.cpp)

Wick uses llama.cpp for fact enrichment and answer generation. Start a llama-server with any GGUF model:

```bash
llama-server --model /path/to/model.gguf --host 127.0.0.1 --port 8004 --ctx-size 8192 --n-gpu-layers 99
```

### 3. Configure and run the server

```bash
export WAXCPP_LLAMA_CPP_ROOT=/path/to/llama-cpp
export WAXCPP_GENERATION_MODEL=/path/to/model.gguf
./build/bin/waxcpp_rag_server
```

The server starts on `http://127.0.0.1:8080` and communicates via JSON-RPC 2.0.

### 4. Index a codebase

```bash
curl -s -X POST http://127.0.0.1:8080/ \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"index.start","params":{
    "repo_root":"/path/to/your/project",
    "include_extensions":[".h",".cpp",".hpp"],
    "enrich_regex":true,
    "enrich_llm":true
  }}'
```

### 5. Search

```bash
# Full-text search
curl -s -X POST http://127.0.0.1:8080/ \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"recall","params":{"query":"how does hashing work","top_k":5}}'

# RAG-augmented answer
curl -s -X POST http://127.0.0.1:8080/ \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"answer.generate","params":{"query":"Explain the actor lifecycle"}}'
```

## Architecture

```
  IDE / Agent ───> MCP bridge (Node.js, stdio)
                       │
                   JSON-RPC / HTTP :8080
                       │
                 waxcpp_rag_server
                  ┌────┴────┐
             BM25/FTS5   Structured facts (EAV)
           (SQLite)    (in-memory, persisted)
                  └────┬────┘
                   .mv2s store
             (append-only binary + WAL)
```

## JSON-RPC API

| Method | Description |
|--------|-------------|
| `remember` | Store a text chunk with metadata |
| `recall` | Hybrid search (BM25 + optional vector) |
| `answer.generate` | RAG-augmented answer via local LLM |
| `index.start` | Start background indexing of a directory |
| `index.status` | Poll indexing progress (state, phase, metrics) |
| `index.stop` | Cancel a running index job |
| `fact.search` | Search structured entity-attribute-value facts |
| `flush` | Force-flush pending writes to disk |
| `blueprint.read` | Read an exported UE5 Blueprint JSON |
| `blueprint.write` | Write modified Blueprint JSON (with backup) |
| `blueprint.import` | Trigger Blueprint reimport into .uasset |

### `index.start` parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `repo_root` | string | required | Path to the directory to index |
| `resume` | bool | `false` | Skip unchanged files using checkpoint manifests |
| `include_extensions` | string[] | `[".h",".cpp",".hpp",".inl",".inc"]` | File extensions to scan |
| `exclude_dirs` | string[] | `[]` | Directory names to skip |
| `enrich_regex` | bool | `false` | Extract facts via regex (class hierarchies, macros) |
| `enrich_llm` | bool | `false` | Extract facts via local LLM |
| `target_tokens` | int | `400` | Target chunk size in tokens |
| `flush_every_chunks` | int | `128` | Commit/checkpoint cadence |
| `ingest_batch_size` | int | `1` | Chunks buffered before batch apply |
| `max_files` | int | `0` | Cap on scanned files (0 = no cap) |
| `max_chunks` | int | `0` | Cap on ingested chunks (0 = no cap) |
| `max_ram_mb` | int | `0` | Soft RSS cap in MB (0 = disabled) |

## MCP bridge (IDE integration)

The `mcp/` directory contains a Node.js MCP server that connects editors (VS Code, Claude Code) to the Wick server via stdio.

Available tools: `wax_recall`, `wax_remember`, `wax_fact_search`, `wax_blueprint_read`, `wax_blueprint_write`.

```bash
cd mcp && npm install
```

MCP config example (`mcp.json`):
```json
{
  "servers": {
    "wax": {
      "type": "stdio",
      "command": "node",
      "args": ["/path/to/wick/mcp/wax_mcp_server.js"],
      "env": { "WAX_URL": "http://127.0.0.1:8080" }
    }
  }
}
```

## UE5-specific features

Wick has built-in support for Unreal Engine 5 projects:

| Source type | Extensions | Enrichment |
|-------------|------------|------------|
| C++ source | `.h`, `.hpp`, `.cpp`, `.inl`, `.inc` | Regex (UCLASS/UPROPERTY/UFUNCTION macros, class hierarchies) + optional LLM |
| Blueprints | `.bpl_json` (exported via `BlueprintGraphExport` commandlet) | LLM (entity-attribute-value facts: calls, variables, events, inheritance) |

Example scripts for indexing UE5 projects are in `ue5_sandbox/`.

## Environment variables

### Server

| Variable | Description |
|----------|-------------|
| `WAXCPP_LLAMA_CPP_ROOT` | Path to llama.cpp binaries (required) |
| `WAXCPP_GENERATION_MODEL` | Path to generation GGUF model |
| `WAXCPP_SERVER_CONFIG` | Path to `server-runtime.json` |
| `WAXCPP_SERVER_LOG` | Enable server logging (`1`) |
| `WAXCPP_RECALL_LOG` | File path for recall request/response log |
| `WAXCPP_AUTO_FLUSH_INTERVAL_MS` | Auto-flush interval in ms (default `30000`) |

### LLM generation

| Variable | Description |
|----------|-------------|
| `WAXCPP_LLAMA_GEN_ENDPOINT` | Generation endpoint (default `http://127.0.0.1:8004/completion`) |
| `WAXCPP_LLAMA_API_KEY` | API key for llama-server |
| `WAXCPP_LLAMA_GEN_TIMEOUT_MS` | Request timeout in ms |
| `WAXCPP_LLAMA_GEN_MAX_RETRIES` | Retry count on failures |

### LLM enrichment

| Variable | Description |
|----------|-------------|
| `WAXCPP_ENRICH_LLM_LOG` | Verbose enrichment logging (`1`) |
| `WAXCPP_ENRICH_LLM_MAX_TOKENS` | Max tokens for enrichment responses (default `1024`) |

### Embeddings (optional)

| Variable | Description |
|----------|-------------|
| `WAXCPP_LLAMA_EMBED_ENDPOINT` | Embedding endpoint URL |
| `WAXCPP_LLAMA_EMBED_DIMS` | Expected embedding dimension |
| `WAXCPP_LLAMA_EMBED_TIMEOUT_MS` | Request timeout in ms |
| `WAXCPP_LLAMA_EMBED_MAX_RETRIES` | Retry count |
| `WAXCPP_LLAMA_EMBED_MAX_BATCH_CONCURRENCY` | Max parallel workers |

### RAG tuning

| Variable | Description |
|----------|-------------|
| `WAXCPP_RAG_MAX_SNIPPETS` | Max snippets in RAG context |
| `WAXCPP_RAG_SEARCH_TOP_K` | Top-K results for search |
| `WAXCPP_ORCH_INGEST_CONCURRENCY` | Parallel ingest workers |
| `WAXCPP_ORCH_INGEST_BATCH_SIZE` | Ingest batch size |

## Project structure

```
.
├── include/waxcpp/     # Public headers (core library)
├── src/                # Core library implementation
│   ├── core/           #   Storage, WAL, SHA256
│   ├── text/           #   FTS5, structured memory
│   ├── vector/         #   USearch vector engine
│   ├── rag/            #   Search, embeddings, scoring, chunking
│   └── orchestrator/   #   Memory orchestrator
├── server/             # HTTP RAG server (Poco-based)
├── mcp/                # MCP bridge for IDE integration
├── tests/              # Unit, parity, and integration tests
├── fixtures/           # Cross-language parity fixtures
├── tools/              # mv2s_analyzer, mv2s_vacuum
├── scripts/            # Submodule verification, CI helpers
├── ue5_sandbox/        # UE5-specific indexing/export scripts
├── third_party/        # Git submodules (usearch, sqlite, googletest, poco)
├── manifest/           # Artifact manifests
└── docs/               # Additional documentation
```

## Dependency policy

All third-party dependencies are managed via git submodules under `third_party/`.

See: `.gitmodules`, `submodules.lock`, `scripts/verify_submodules.py`.

```bash
# Run policy test
ctest --test-dir build --output-on-failure -R waxcpp_verify_submodules_policy_test

# Strict CI checks
python3 scripts/verify_submodules.py --require-gitlinks-present
python3 scripts/verify_submodules.py --enforce-pin-required
```

## License

Apache 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
