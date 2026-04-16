## WAX API Reference

Full API reference for the WAX RAG server. The compact guide is in `docs/wax-agent-guide.md`.

---

### System topology

WAX has **two parallel retrieval stacks**:

```
┌─ C++ stack (for code) ────────────────────────────────────┐
│  WAX C++ server     :8080  JSON-RPC  (recall/fact.search) │
│  ├─ FTS5 (SQLite)            BM25 text index              │
│  ├─ StructuredMemoryStore    EAV fact store               │
│  └─ .mv2s                    append-only binary store     │
└───────────────────────────────────────────────────────────┘

┌─ Python + ES stack (for Blueprints) ──────────────────────┐
│  embedding service  :8088  FastAPI  (Qodo-Embed-1-1.5B)   │
│  Elasticsearch       :9200  wax_bp_v1 index               │
│     ├─ dense_vector(1536)   cosine kNN                    │
│     └─ text fields           BM25                          │
│  Kibana             :5601  (dev/ops UI)                   │
└───────────────────────────────────────────────────────────┘
```

Both stacks are exposed through the same MCP bridge (`mcp/wax_mcp_server.js`). The agent calls
one set of `wax_*` tools — the bridge routes to the right backend:

| MCP tool | Backend |
|----------|---------|
| `wax_recall`, `wax_remember`, `wax_fact_search`, `wax_blueprint_*` | WAX C++ server (:8080) |
| `wax_bp_semantic_search`, `wax_bp_facts` | ES + embedding service |

See `docs/wax-agent-guide.md` for when to use which.

---

### Blueprint Export Commandlet

Before the agent can read or patch Blueprints, they must be exported from UE5 to `.bpl_json` files on disk.

**Commandlet:** `BlueprintGraphExport` (provided by OlivaBlueprintRAG plugin)

```bash
UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintGraphExport \
  -Root=/Game \
  -ExportDir=<Saved/BlueprintExports> \
  [-Prefix=BP_,B_] \
  [-PathContains=Blueprints] \
  [-NoRecursive] \
  -unattended -nop4
```

| Argument | Default | Description |
|----------|---------|-------------|
| `-Root=<path>` | `/Game` | Asset registry root path to scan |
| `-ExportDir=<path>` | `Saved/BlueprintExports` | Output directory for .bpl_json files |
| `-Prefix=<list>` | _(all)_ | Comma-separated asset name prefixes to filter |
| `-PathContains=<list>` | _(all)_ | Comma-separated path segments to match |
| `-NoRecursive` | _(recursive)_ | Disable recursive path traversal |

**File naming:** Asset path with slashes/dots → underscores + `.bpl_json`
- `/Game/Blueprints/BP_Hero.BP_Hero` → `_Game_Blueprints_BP_Hero_BP_Hero.bpl_json`

**Exports:** UBlueprint and UDataAsset instances. Each file contains full graph data: nodes, pins, links, variables, and node-type-specific properties (function refs, event refs, variable refs, cast targets, macro refs, component events).

### Blueprint Import Commandlet

**Commandlet:** `BlueprintGraphImport` (provided by OlivaBlueprintRAG plugin)

```bash
UnrealEditor-Cmd.exe <Project>.uproject -run=BlueprintGraphImport \
  -ImportDir=<Saved/BlueprintExports> \
  -ClearGraph=1 \
  -Compile=1 \
  -Save=1 \
  [-CreateMissingGraphs=1] \
  [-CreateMissingBlueprints=0] \
  -unattended -nop4
```

| Argument | Default | Description |
|----------|---------|-------------|
| `-ImportDir=<path>` | `Saved/BlueprintExports` | Directory with .bpl_json files |
| `-Compile=<0\|1>` | `1` | Compile blueprints after import |
| `-Save=<0\|1>` | `1` | Save packages after import |
| `-ClearGraph=<0\|1>` | `0` | Clear existing graph before import (vs merge) |
| `-CreateMissingGraphs=<0\|1>` | `1` | Create graph if not found |
| `-CreateMissingBlueprints=<0\|1>` | `0` | Create blueprint if doesn't exist |

The `wax_blueprint_import` MCP tool wraps this commandlet.

---

### MCP Tool Schemas

#### wax_recall

Full-text BM25 search over indexed code chunks and enriched facts.

```
Input:
  query: string (required) — natural language or C++ symbol names
  limit: number (optional, default 12) — max results

Output:
  items: [{text: string, score: float, kind: int}]  // kind: 0=snippet, 1=declaration/fact
  count: number
  total_tokens: number
```

Query tips:
- Use class + method names: `"UCharacterMovementComponent SetMovementMode"`
- Combine with keywords: `"FTickFunction AddPrerequisite"`
- Results are ranked by BM25 relevance, capped by token budget

#### wax_fact_search

Search structured (entity, attribute, value) facts by entity prefix.

```
Input:
  entity_prefix: string (required) — e.g., "cpp:AMyActor", "bp:BP_Hero"
  limit: number (optional, default 100) — max facts returned

Output:
  facts: [{
    id: uint64,
    entity: string,
    attribute: string,
    value: string,
    version: uint64,
    pinned: bool,
    deleted: bool,
    supersedes: uint64 | null,
    timestamp_ms: uint64,
    metadata: {string: string}
  }]
  count: number
  entity_prefix: string
```

#### wax_bp_semantic_search

Semantic + keyword search over indexed Blueprints. Backed by Elasticsearch (`wax_bp_v1`) with a dense 1536-dim vector field (Qodo-Embed-1-1.5B) plus standard BM25 fields.

```
Input:
  query: string (required) — natural language describing the Blueprint's behavior
  k: number (optional, default 5, max 20) — number of hits to return
  mode: "knn" | "bm25" | "hybrid" (optional, default "hybrid")
    knn     — pure vector cosine similarity (needs embed service)
    bm25    — keyword match on purpose, asset_name, text, exec_chain, calls
    hybrid  — kNN boost 0.7 + BM25 boost 0.3, fused by ES _score
  kind_filter: string (optional) — restrict to one of:
    gameplay_ability | gameplay_effect | gameplay_cue |
    anim_blueprint   | anim_notify     |
    widget           | actor_blueprint | blueprint

Output:
  hits: [{
    entity: "bp:AssetName",
    kind: string,
    parent_class: string,
    purpose: string,
    exec_chain: string,
    score: number
  }]
```

**Query tips**:
- Describe behavior: `"ability that temporarily freezes player input"` (not: `"GA_SpawnEffect"`)
- For known names use `bm25` mode: `"K2_ActivateAbility SpawnActor"`
- Use `kind_filter` when you know the category: `{query: "weapon pickup", kind_filter: "gameplay_ability"}`

**Examples**:

```json
// Find a BP by description
{"query": "disables player input for a short time after respawn", "k": 5}

// Restrict to one BP kind
{"query": "pickup that gives ammo", "kind_filter": "gameplay_ability", "mode": "knn"}

// Keyword-only (no embedding call)
{"query": "BP_ApplyGameplayEffectToSelf", "mode": "bm25"}
```

#### wax_bp_facts

Exact Blueprint lookup by entity id. Returns all structural facts for one BP.

```
Input:
  entity: string (required) — must start with "bp:", e.g. "bp:GA_SpawnEffect"

Output:
  {
    entity: "bp:GA_SpawnEffect",
    kind: "gameplay_ability",
    parent_class: "GameplayAbility",
    asset_path: "/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect",
    node_count: 33,
    link_count: 32,
    purpose: "...",                       # LLM-generated one-sentence description
    exec_chain: "event1: a → b → c | event2: x → y",
    events: ["K2_ActivateAbility", ...],
    custom_events: [...],
    calls: [...],                         # deduped function names
    call_owners: [...],                   # owning classes of called functions
    variables: ["Name:type", ...],
    casts_to: [...],                      # DynamicCast targets
    macros: [...]
  }
```

**Errors**:
- Entity not starting with `bp:` → 400
- Entity not in index → 404 (`"Blueprint bp:X not found in index"`)

#### wax_blueprint_read

Read full Blueprint JSON from exported `.bpl_json` file.

```
Input:
  blueprint_path: string (required) — UE asset path, e.g., "/Game/Blueprints/BP_Hero.BP_Hero"
  export_dir: string (required) — path to BlueprintExports directory on disk

Output:
  json: string — full Blueprint JSON (nodes, pins, links, variables)
  file_path: string
  size_bytes: number
```

Blueprint JSON top-level structure:
```json
{
  "blueprint": "/Game/Path/Asset",
  "asset_path": "/Game/Path/Asset.Asset",
  "asset_name": "BP_Hero",
  "asset_class": "/Script/Engine.Actor",
  "asset_kind": "blueprint",
  "graphs": [{
    "name": "EventGraph",
    "graph_guid": "GUID",
    "nodes": [...],
    "links": [...]
  }],
  "variables": [...]
}
```

#### wax_blueprint_compressed_read

Read semantically-compressed Blueprint — strips pins, GUIDs, positions, links. Returns only data relevant for understanding the Blueprint.

```
Input:
  blueprint_path: string (required)
  export_dir: string (required)

Output:
  json: string — compressed Blueprint JSON
  file_path: string
  original_size: number
  compressed_size: number
```

Compressed format keeps per node:
- `title` — node display title
- `class` — shortened node class (e.g., "K2Node_CallFunction")
- `calls` — function name (for CallFunction nodes)
- `from` — function owner class (shortened)
- `event` — event name (for Event nodes)
- `custom_event` — custom event name
- `variable_ref`, `var_name` — variable references
- `cast_target` — cast target class (shortened)
- `typed_pins` — output pins with type info: `[{name, type}]`
- `params` — input pins with non-trivial defaults: `{pin_name: value}`

Nodes with only title + class (e.g., K2Node_Knot, K2Node_FunctionResult) are omitted.

#### wax_blueprint_patch

Apply Blueprint Intent JSON (.bpi_json) to an existing or new Blueprint.

```
Input:
  blueprint_path: string (required)
  export_dir: string (required)
  intent_json: string (required) — .bpi_json content

Output:
  summary: string — human-readable summary, e.g., "Patched EventGraph: 3 nodes added, 2 links added"
  file_path: string
  bytes_written: number
```

Creates `.backup.bpl_json` before writing. See the guide for .bpi_json format.

#### wax_blueprint_write

Write full Blueprint JSON directly to disk. Use when you need to make changes not expressible as .bpi_json.

```
Input:
  blueprint_path: string (required)
  export_dir: string (required)
  json: string (required) — full Blueprint JSON (must contain "blueprint" key)

Output:
  file_path: string
  bytes_written: number
```

Creates `.backup.bpl_json` before overwriting.

#### wax_blueprint_import

Import modified `.bpl_json` files into UE5 via the BlueprintGraphImport commandlet.

```
Input:
  ue_editor: string (required) — path to UnrealEditor-Cmd.exe
  uproject: string (required) — path to .uproject file
  import_dir: string (required) — directory containing .bpl_json files
  compile: bool (optional, default true) — compile after import
  save: bool (optional, default true) — save packages after import

Output:
  exit_code: number
  stdout: string
  stderr: string
  elapsed_ms: number
```

Commandlet flags: `-ClearGraph=1 -Compile={0|1} -Save={0|1} -unattended -nop4`

#### wax_remember

Store knowledge in WAX memory for future recall. Use this to register new or modified code in the WAX index immediately after editing files.

```
Input:
  content: string (required) — text to remember (full file content or relevant excerpt)
  metadata: object (optional):
    symbol: string — primary symbol name, e.g., "UMyClass::MyFunction"
    source_kind: string — "cpp", "header", "bpl_json"
    relative_path: string — file path relative to project root
    language: string — "cpp", "bpl_json"

Output:
  string — confirmation message
```

The content is chunked, embedded (if vector search is enabled), and stored in the FTS5 index. It becomes immediately searchable via `wax_recall`.

**Important**: `wax_remember` adds new chunks but does not remove old ones. If you modify an existing file, the old version's chunks remain in the index alongside the new ones. For clean re-indexing of many files, use `index.start` with `resume=true` instead.

---

### Keeping the Index in Sync

The WAX index is a **snapshot** — it is not updated automatically when files change. After the agent modifies C++ or Blueprint files, it must explicitly update the knowledge base.

#### Lightweight update (1-3 files)

1. **`wax_remember`** — store the new file content so `wax_recall` finds it
2. **`fact.add`** — register structured facts (entity, attribute, value) for new classes/functions

#### Bulk re-index (many files)

Use `index.start` with `resume=true` — this compares file content hashes against the checkpoint and only re-indexes changed files:

```json
{"method":"index.start","params":{
  "repo_root": "/path/to/project",
  "resume": true,
  "checkpoint_namespace": "project_code",
  "include_extensions": [".h", ".cpp", ".hpp"],
  "enrich_regex": true
}}
```

Monitor with `index.status` until `state` returns to `"idle"`.

#### Correcting facts

- `fact.update` — create a new revision that supersedes the old value
- `fact.delete` — remove a specific fact revision
- `fact.pin` — protect a corrected fact from future overwrites

---

### Fact System

Facts are (entity, attribute, value) triples with versioning. Each mutation creates a new revision.

#### Entity prefixes

| Prefix | Source | Example |
|--------|--------|---------|
| `cpp:` | C++ code (regex enrichment) | `cpp:ACharacter`, `cpp:UActorComponent.MaxHealth` |
| `bp:` | Blueprint JSON (LLM enrichment) | `bp:BP_Hero`, `bp:ABP_RifleAnimLayers` |

#### Common attributes

| Attribute | Meaning | Example value |
|-----------|---------|---------------|
| `inherits` | Parent class | `AActor` |
| `kind` | Entity type | `uclass`, `ustruct`, `uenum`, `uproperty`, `ufunction` |
| `specifiers` | UCLASS/UPROPERTY specifiers | `BlueprintType,Blueprintable` |
| `type` | Property type | `float`, `TArray<uint8>` |
| `returns` | Function return type | `void`, `bool` |
| `category` | UPROPERTY category | `Combat`, `Movement` |
| `api_macro` | API export macro | `ENGINE_API` |
| `calls` | Functions called | `TryGetPawnOwner` |
| `depends_on` | Dependencies | `ABP_RifleAnimLayers_C` |
| `has_event` | Blueprint events | `BlueprintUpdateAnimation` |
| `has_variable` | Blueprint variables | `Health` |
| `purpose` | Semantic description | `handles animation updates for rifle weapon` |

#### Fact lifecycle methods

These are available via `wax_fact_search` for reading, and via JSON-RPC for mutations:

| Method | Purpose | Key params |
|--------|---------|------------|
| `fact.add` | Create new fact | `entity`, `attribute`, `value`, `metadata` |
| `fact.get` | Load one fact by ID | `id` |
| `fact.update` | Create new revision (supersedes old) | `id`, `value`, `metadata` |
| `fact.delete` | Delete a fact revision | `id` |
| `fact.pin` | Pin fact (protect from updates) | `id`, `pinned` |
| `fact.history` | Get revision chain | `id` |
| `fact.search` | Search by entity prefix | `entity_prefix`, `limit` |

Important: `fact.update` and `fact.pin` create new revisions, they do not modify in place. Pinned facts cannot be updated or deleted.

---

### BPI Node Types (Full Reference)

| type | UE5 Node Class | Required fields | Optional fields |
|------|---------------|-----------------|-----------------|
| `CallFunction` | K2Node_CallFunction | `function`, `function_owner` | `title` |
| `CallParentFunction` | K2Node_CallParentFunction | `function`, `function_owner` | `title` |
| `Event` | K2Node_Event | `event` | `event_owner` (default: `/Script/Engine.Actor`) |
| `CustomEvent` | K2Node_CustomEvent | `event_name` or `title` | |
| `VariableGet` | K2Node_VariableGet | `variable` | |
| `VariableSet` | K2Node_VariableSet | `variable` | |
| `Branch` | K2Node_IfThenElse | _(none)_ | |
| `Sequence` | K2Node_ExecutionSequence | _(none)_ | |
| `Cast` | K2Node_DynamicCast | `cast_target` | |
| `MacroInstance` | K2Node_MacroInstance | `macro` | |
| `FunctionEntry` | K2Node_FunctionEntry | _(none)_ | |
| `FunctionResult` | K2Node_FunctionResult | _(none)_ | |
| `Select` | K2Node_Select | _(none)_ | |
| `ComponentBoundEvent` | K2Node_ComponentBoundEvent | _(none)_ | |

All add_nodes entries require `ref` (local alias for linking) and `type`.

#### Node resolution

- `"ref": "my_node"` — reference a node added in the same `add_nodes` array
- `"existing:Node Title"` — reference an existing node in the graph by its display title
- GUIDs are assigned automatically (32-char uppercase hex)
- Positions are assigned automatically (X: maxX + 300, Y: nextY + 200 per node)

#### Pin names

Pins for `CallFunction` nodes match C++ parameter names exactly:
```cpp
UFUNCTION(BlueprintCallable)
void ApplyDamage(AActor* Target, float Amount, EDamageType Type);
// Blueprint pins: "Target", "Amount", "Type", "execute" (in), "then" (out)
```

Standard exec pins: `execute` (in), `then` (out).
Branch pins: `Condition` (bool in), `True` (exec out), `False` (exec out).
Return value: `ReturnValue`.

---

### Index System

Start indexing via JSON-RPC (not exposed as MCP tool):

```json
{"jsonrpc":"2.0","id":1,"method":"index.start","params":{
  "repo_root": "/path/to/ue5/project",
  "resume": true,
  "checkpoint_namespace": "my_project",
  "include_extensions": [".h", ".cpp", ".hpp", ".bpl_json"],
  "exclude_dirs": ["Build", "Intermediate", "Binaries"],
  "enrich_regex": true,
  "enrich_llm": false,
  "target_tokens": 400,
  "flush_every_chunks": 4096
}}
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `repo_root` | string | required | Directory to scan |
| `resume` | bool | false | Skip unchanged files via checkpoint |
| `checkpoint_namespace` | string | "" | Isolate checkpoints per project |
| `include_extensions` | string[] | `[".h",".cpp",".hpp",".inl",".inc"]` | File patterns |
| `exclude_dirs` | string[] | `[]` | Directories to skip |
| `enrich_regex` | bool | false | Extract UE5 macros, inheritance, properties |
| `enrich_llm` | bool | false | LLM-based semantic fact extraction |
| `target_tokens` | int | 400 | Chunk size target |
| `overlap_tokens` | int | 40 | Chunk overlap |
| `flush_every_chunks` | int | 4096 | Commit batch size |
| `ingest_batch_size` | int | 1 | Parallel ingest |
| `max_files` | int | 0 | File cap (0=unlimited) |
| `max_chunks` | int | 0 | Chunk cap (0=unlimited) |
| `max_ram_mb` | int | 0 | Soft RSS cap (0=unlimited) |

Monitor progress: `{"method":"index.status"}` returns state, phase, scanned_files, indexed_chunks, committed_chunks, elapsed_ms.

Cancel: `{"method":"index.stop"}`.

---

### answer.generate — RAG-augmented answers

Performs recall, builds context, generates answer via local LLM with source citations. **Slow** (10-60 seconds) — use only for complex architectural questions.

```json
{"jsonrpc":"2.0","id":1,"method":"answer.generate","params":{
  "query": "How does the pawn possession and input setup work?",
  "max_context_items": 12,
  "max_context_tokens": 6000,
  "max_output_tokens": 1024
}}
```

Response:
```json
{
  "answer": "text with citations [1][2]...",
  "citations": [
    {"relative_path": "Source/MyGame/MyComponent.cpp", "line_start": 42, "line_end": 68, "symbol": "UMyComponent::SetupInput"}
  ],
  "context_items_used": 8,
  "total_context_tokens": 4200
}
```

---

### JSON-RPC Direct Access

All methods are accessible via HTTP POST to the WAX server (default `http://127.0.0.1:8080`):

```bash
curl -s -X POST http://127.0.0.1:8080 \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"METHOD","params":{...}}'
```

#### All methods

| Method | Purpose |
|--------|---------|
| `recall` | Full-text code search |
| `remember` | Store knowledge |
| `answer.generate` | RAG answer with citations |
| `flush` | Force commit to disk |
| `index.start` | Start background indexing |
| `index.status` | Poll indexing progress |
| `index.stop` | Cancel indexing |
| `blueprint.read` | Read full Blueprint JSON |
| `blueprint.compressed_read` | Read compressed Blueprint |
| `blueprint.patch` | Apply .bpi_json intent |
| `blueprint.write` | Write Blueprint JSON |
| `blueprint.import` | Import into UE5 via commandlet |
| `fact.add` | Add new fact |
| `fact.get` | Get fact by ID |
| `fact.update` | Create new fact revision |
| `fact.delete` | Delete fact revision |
| `fact.pin` | Pin/unpin fact |
| `fact.history` | Get fact revision chain |
| `fact.search` | Search facts by entity prefix |

#### Quick examples

```bash
# Search code
curl -s -X POST http://127.0.0.1:8080 \
  -d '{"jsonrpc":"2.0","id":1,"method":"recall","params":{"query":"AActor BeginPlay","limit":10}}'

# Search facts
curl -s -X POST http://127.0.0.1:8080 \
  -d '{"jsonrpc":"2.0","id":1,"method":"fact.search","params":{"entity_prefix":"cpp:ACharacter","limit":20}}'

# Read compressed Blueprint
curl -s -X POST http://127.0.0.1:8080 \
  -d '{"jsonrpc":"2.0","id":1,"method":"blueprint.compressed_read","params":{"blueprint_path":"/Game/BP/BP_Hero.BP_Hero","export_dir":"/path/to/exports"}}'

# Apply intent patch
curl -s -X POST http://127.0.0.1:8080 \
  -d '{"jsonrpc":"2.0","id":1,"method":"blueprint.patch","params":{"blueprint_path":"/Game/BP/BP_Hero.BP_Hero","export_dir":"/path/to/exports","intent_json":"{...}"}}'

# Check index status
curl -s -X POST http://127.0.0.1:8080 \
  -d '{"jsonrpc":"2.0","id":1,"method":"index.status"}'
```
