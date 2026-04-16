## WAX Agent Guide: C++ + Blueprint Round-Trip

You are a coding agent with access to **WAX** — a local RAG server with MCP tools for searching, modifying, and importing UE5 code and Blueprints.

> For full API schemas and advanced usage, read `docs/wax-api-reference.md`.

### Core Principle

> **All game logic goes in C++. Blueprint is only thin wiring.**

When implementing or modifying gameplay functionality:
1. Write C++ with `UFUNCTION(BlueprintCallable)` containing the logic
2. Generate a minimal `.bpi_json` intent to wire the C++ function into the Blueprint graph
3. Import back into UE5

### Available Tools

**For C++ code:**

| Tool | Purpose |
|------|---------|
| `wax_recall` | Full-text BM25 search over indexed C++ |
| `wax_fact_search` | Search structured (entity, attribute, value) facts by prefix (mostly `cpp:`) |
| `wax_remember` | Store knowledge for future recall |

**For Blueprints** — use the semantic stack (backed by Elasticsearch + vector embeddings, much higher quality than EAV facts):

| Tool | Purpose |
|------|---------|
| `wax_bp_semantic_search` | **PRIMARY for BP discovery** — find BP by what it DOES, hybrid kNN + BM25 |
| `wax_bp_facts` | **PRIMARY for BP details** — exact entity lookup, returns kind/parent/exec_chains/calls/events |
| `wax_blueprint_compressed_read` | Read compressed BP JSON (when you need node-level detail) |
| `wax_blueprint_read` | Full BP JSON — **WARNING: often exceeds context limit, use compressed_read instead** |
| `wax_blueprint_patch` | Apply `.bpi_json` intent patch to a Blueprint |
| `wax_blueprint_write` | Write full Blueprint JSON to disk (with backup) |
| `wax_blueprint_import` | Import `.bpl_json` into UE5 via commandlet + compile |
| `wax_blueprint_refresh` | Re-parse + re-embed + ES upsert after a patch (call after patch+import so subsequent searches see the change) |

**Note**: `wax_fact_search` with `entity_prefix="bp:"` is legacy — it returns a few sparse LLM-extracted facts. Always prefer `wax_bp_facts` or `wax_bp_semantic_search` for Blueprint work.

### Round-Trip Architecture

The system has three layers connected by `.bpl_json` files on disk:

```
┌─────────────┐     commandlet      ┌──────────────────┐     MCP tools      ┌───────────┐
│  UE5 Editor │ ←──────────────────→ │ .bpl_json on disk │ ←────────────────→ │ WAX + Agent│
│  (truth)    │  export ↓  ↑ import │  (interchange)    │  read/patch/write │ (AI side)  │
└─────────────┘                     └──────────────────┘                    └───────────┘
```

**Export** (UE5 → disk): `BlueprintGraphExport` commandlet scans the project, serializes each Blueprint/DataAsset to `.bpl_json`.

**Agent reads/modifies** (disk ↔ WAX): Agent uses `wax_blueprint_compressed_read` to understand current state, then `wax_blueprint_patch` to apply changes. Files on disk are the interchange format.

**Import** (disk → UE5): `wax_blueprint_import` invokes `BlueprintGraphImport` commandlet which loads `.bpl_json`, reconstructs nodes/pins/links in the editor graph, compiles, and saves.

**C++ changes** happen outside this loop — the agent edits `.h`/`.cpp` files directly with standard tools (Read/Edit/Write), then the project must be rebuilt before Blueprint import can reference the new functions.

### Workflow

#### Full round-trip (C++ + Blueprint)

```
1. Search      wax_recall + wax_fact_search     → find C++ references
               wax_bp_semantic_search            → find related Blueprints
2. Read BP     wax_bp_facts (then wax_blueprint_compressed_read if needed)
3. Write C++   Edit/Write .h/.cpp files → add UFUNCTION(BlueprintCallable)
4. Build       Rebuild project (agent prompts user, or runs build command)
5. Patch BP    wax_blueprint_patch with .bpi_json (max 7 nodes)
6. Import      wax_blueprint_import with compile=true, save=true
7. Refresh     wax_blueprint_refresh({entity, export_dir}) → ES sees new facts
               Register new C++ code in WAX index (see "Keeping WAX in Sync")
8. Verify      Check import stdout/stderr → if errors, fix and retry (max 3)
```

#### Blueprint-only modification

```
1. Find        wax_bp_semantic_search → locate the BP by description
2. Inspect     wax_bp_facts (structural) + wax_blueprint_compressed_read (graph)
3. Patch BP    wax_blueprint_patch
4. Import      wax_blueprint_import
5. Refresh     wax_blueprint_refresh({entity, export_dir})
```

### Keeping WAX in Sync

WAX index is **not updated automatically** when you edit files. After making changes, you must explicitly update the knowledge base so subsequent searches find your new code.

#### After writing C++ code

Use `wax_remember` to index the new code immediately:

```
wax_remember({
  content: "<full text of new/modified .h or .cpp file>",
  metadata: {
    symbol: "UMyClass::MyFunction",
    relative_path: "Source/MyGame/MyClass.cpp",
    language: "cpp",
    source_kind: "cpp"
  }
})
```

Add structured facts about new classes/functions with `fact.add`:

```json
{"method":"fact.add","params":{
  "entity": "cpp:UMyClass",
  "attribute": "kind",
  "value": "uclass",
  "metadata": {"source": "agent"}
}}

{"method":"fact.add","params":{
  "entity": "cpp:UMyClass",
  "attribute": "inherits",
  "value": "AActor",
  "metadata": {"source": "agent"}
}}

{"method":"fact.add","params":{
  "entity": "cpp:UMyClass::MyFunction",
  "attribute": "kind",
  "value": "ufunction",
  "metadata": {"source": "agent"}
}}
```

#### After patching a Blueprint

Add facts about the new Blueprint wiring:

```json
{"method":"fact.add","params":{
  "entity": "bp:BP_Hero",
  "attribute": "calls",
  "value": "UMyClass::MyFunction",
  "metadata": {"source": "agent"}
}}
```

#### For bulk re-indexing

If many files changed, use `index.start` with `resume=true` to re-index only changed files:

```json
{"method":"index.start","params":{
  "repo_root": "<path/to/project>",
  "resume": true,
  "checkpoint_namespace": "project_code",
  "include_extensions": [".h", ".cpp", ".hpp"],
  "enrich_regex": true
}}
```

Monitor with `index.status`, wait for `state: "idle"` before continuing.

#### Priority

| Situation | Method | Why |
|-----------|--------|-----|
| Added 1-2 files | `wax_remember` + `fact.add` | Fast, immediate, no downtime |
| Modified existing indexed file | `wax_remember` (new version) | Old chunks remain but new content is searchable |
| Large refactor (10+ files) | `index.start` with `resume=true` | Proper incremental re-index |
| Correcting wrong facts | `fact.update` or `fact.delete` | Fix specific EAV triples |

#### Prerequisites

Before the agent can work with Blueprints, they must be **exported** from UE5:

```bash
UnrealEditor-Cmd.exe <Project>.uproject \
  -run=BlueprintGraphExport \
  -Root=/Game \
  -ExportDir=<Saved/BlueprintExports> \
  -Prefix=BP_ \
  -unattended -nop4
```

Export arguments:
- `-Root=/Game` — asset path root to scan
- `-ExportDir=<path>` — where to write `.bpl_json` files
- `-Prefix=BP_,B_` — comma-separated name prefixes to filter (optional)
- `-PathContains=Blueprints` — comma-separated path segments to match (optional)

File naming: `/Game/Blueprints/BP_Hero.BP_Hero` → `_Game_Blueprints_BP_Hero_BP_Hero.bpl_json`

The agent should **not** run the export itself — ask the user to export Blueprints before starting round-trip work. The export only needs to happen once per session (or after manual BP edits in the editor).

### Searching

#### For C++: `wax_recall` and `wax_fact_search`

**`wax_recall`** — full-text BM25 search over indexed C++ code.

Use specific class and function names for best results:
- Good: `"UCharacterMovementComponent SetMovementMode"`
- Bad: `"character movement system"`

Combine class + keyword for usage patterns:
- `"ULyraHeroComponent SetupPlayerInputComponent BeginPlay"`

Returns `items[]` with `text` (code fragment), `score`, `kind` (0=snippet, 1=declaration).

**`wax_fact_search`** — structured (entity, attribute, value) triples by prefix:
- `entity_prefix: "cpp:AMyActor"` → class facts (inherits, kind, specifiers)
- `entity_prefix: "cpp:UCharacterMovement"` → partial prefix match

Common attributes for `cpp:` entities: `inherits`, `kind`, `calls`, `depends_on`, `purpose`, `specifiers`, `api_macro`, `type`, `returns`.

#### For Blueprints: `wax_bp_semantic_search` and `wax_bp_facts`

**`wax_bp_semantic_search`** — semantic + keyword fusion over ~640 indexed Blueprints. Finds BPs by **what they do**, not by exact name.

```
wax_bp_semantic_search({
  query: "disables player input for a short time after respawn",
  k: 5,               // top-K, default 5, max 20
  mode: "hybrid",     // "knn" | "bm25" | "hybrid" (default)
  kind_filter: ""     // optional: gameplay_ability, gameplay_effect,
                      //   gameplay_cue, anim_blueprint, anim_notify,
                      //   widget, actor_blueprint, blueprint
})
```

Returns list of hits with `entity`, `kind`, `parent_class`, `purpose`, `exec_chain`.

When to use each mode:
- **hybrid** (default) — best for natural language like "ability that heals on activation"
- **knn** — pure semantic when you want close meaning regardless of wording
- **bm25** — keyword-based, use for exact function/class names like "K2_ActivateAbility"

**`wax_bp_facts`** — exact entity lookup. Returns all structural facts for one BP.

```
wax_bp_facts({ entity: "bp:GA_SpawnEffect" })
```

Returns:
- `kind`, `parent_class`, `asset_path`
- `node_count`, `link_count`
- `purpose` (LLM-generated one-sentence description)
- `exec_chain` (sequence of calls from each event)
- `events`, `custom_events`, `calls`, `call_owners`, `variables`, `casts_to`, `macros`

**Typical BP workflow**:
1. Don't know which BP handles something? → `wax_bp_semantic_search` with natural language
2. Found it, need details? → `wax_bp_facts` with the entity id
3. Need node-level graph? → `wax_blueprint_compressed_read` with the asset_path
4. About to patch? → generate `.bpi_json` and call `wax_blueprint_patch`

### Blueprint Intent Format (.bpi_json)

The intent describes **what to change**, not the full Blueprint.

```jsonc
{
  "target": "/Game/Path/Asset.Asset",
  "description": "What this patch does",

  // Optional: create new blueprint
  "create": true,
  "parent_class": "/Script/Engine.Actor",

  // Optional: add/update variables
  "variables": [
    {"var_name": "Health", "var_type": {"category": "real"}, "default_value": "100.0"}
  ],

  "graphs": [{
    "name": "EventGraph",

    "add_nodes": [
      {"ref": "my_call", "type": "CallFunction",
       "function": "MyFunction", "function_owner": "MyClass"}
    ],

    "add_links": [
      {"from": "existing:ReceiveTick", "from_pin": "then",
       "to": "my_call", "to_pin": "execute"}
    ],

    "remove_links": [
      {"from": "existing:NodeA", "from_pin": "then",
       "to": "existing:NodeB", "to_pin": "execute"}
    ],

    "remove_nodes": ["Old Node Title"],

    "set_defaults": [
      {"node": "my_call", "pin": "Damage", "value": "50.0"}
    ]
  }]
}
```

### Node Types

| type | UE5 Node | Key fields |
|------|----------|------------|
| `CallFunction` | K2Node_CallFunction | `function`, `function_owner` |
| `CallParentFunction` | K2Node_CallParentFunction | `function`, `function_owner` |
| `Event` | K2Node_Event | `event`, `event_owner` (default: Actor) |
| `CustomEvent` | K2Node_CustomEvent | `event_name` |
| `VariableGet` | K2Node_VariableGet | `variable` |
| `VariableSet` | K2Node_VariableSet | `variable` |
| `Branch` | K2Node_IfThenElse | _(none)_ |
| `Sequence` | K2Node_ExecutionSequence | _(none)_ |
| `Cast` | K2Node_DynamicCast | `cast_target` |
| `MacroInstance` | K2Node_MacroInstance | `macro` |
| `FunctionEntry` | K2Node_FunctionEntry | _(auto)_ |
| `FunctionResult` | K2Node_FunctionResult | _(auto)_ |
| `Select` | K2Node_Select | _(none)_ |
| `ComponentBoundEvent` | K2Node_ComponentBoundEvent | _(none)_ |

### Standard Pin Names

| Pin | Where | Direction |
|-----|-------|-----------|
| `then` | All nodes | exec out |
| `execute` | All nodes | exec in |
| `ReturnValue` | CallFunction | data out |
| `Condition` | Branch | data in (bool) |
| `True` / `False` | Branch | exec out |
| `self` | Method calls | data in |
| _(parameter name)_ | UFUNCTION params | matches C++ param name exactly |

### C++ Conventions for Blueprint-Callable Code

```cpp
// Expose a function to Blueprint
UFUNCTION(BlueprintCallable, Category = "Combat")
static float CalculateDamage(AActor* Target, float BaseDamage);

// Expose a pure function (no exec pins)
UFUNCTION(BlueprintPure, Category = "Combat")
static bool IsTargetAlive(AActor* Target);

// Expose a property to Blueprint
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
float MaxHealth = 100.0f;
```

Pin names in Blueprint match C++ parameter names exactly. If you write `CalculateDamage(AActor* Target, float BaseDamage)`, the Blueprint pins are `Target` and `BaseDamage`.

### Examples

#### Add a function call to an existing event

```json
{
  "target": "/Game/Characters/BP_Hero.BP_Hero",
  "graphs": [{
    "name": "EventGraph",
    "add_nodes": [
      {"ref": "calc", "type": "CallFunction",
       "function": "CalculateDamage", "function_owner": "CombatHelper"}
    ],
    "add_links": [
      {"from": "existing:ReceiveBeginPlay", "from_pin": "then",
       "to": "calc", "to_pin": "execute"}
    ]
  }]
}
```

#### Insert a Branch into existing exec flow

```json
{
  "target": "/Game/Characters/BP_Hero.BP_Hero",
  "graphs": [{
    "name": "EventGraph",
    "remove_links": [
      {"from": "existing:ReceiveTick", "from_pin": "then",
       "to": "existing:UpdateMovement", "to_pin": "execute"}
    ],
    "add_nodes": [
      {"ref": "check", "type": "CallFunction",
       "function": "ShouldUpdate", "function_owner": "MovementHelper"},
      {"ref": "branch", "type": "Branch"}
    ],
    "add_links": [
      {"from": "existing:ReceiveTick", "from_pin": "then",
       "to": "check", "to_pin": "execute"},
      {"from": "check", "from_pin": "then",
       "to": "branch", "to_pin": "execute"},
      {"from": "check", "from_pin": "ReturnValue",
       "to": "branch", "to_pin": "Condition"},
      {"from": "branch", "from_pin": "True",
       "to": "existing:UpdateMovement", "to_pin": "execute"}
    ]
  }]
}
```

#### Create a new Blueprint

```json
{
  "target": "/Game/Abilities/GA_DoubleJump.GA_DoubleJump",
  "create": true,
  "parent_class": "/Script/GameplayAbilities.GameplayAbility",
  "graphs": [{
    "name": "EventGraph",
    "add_nodes": [
      {"ref": "activate", "type": "Event", "event": "K2_ActivateAbility"},
      {"ref": "jump", "type": "CallFunction",
       "function": "ExecuteDoubleJump", "function_owner": "JumpHelper"}
    ],
    "add_links": [
      {"from": "activate", "from_pin": "then",
       "to": "jump", "to_pin": "execute"}
    ]
  }]
}
```

### Error Handling

| Error | Cause | Fix |
|-------|-------|-----|
| `"File not found"` | Blueprint not exported | Export via UE5 or check `export_dir` path |
| `"Ref 'existing:X' not found"` | Node title mismatch | Use `wax_blueprint_compressed_read` to find exact titles |
| `"Import failed (exit 1)"` | Compile error | Check stderr for pin/type mismatches, fix C++ or .bpi_json |
| `"Failed to parse intent JSON"` | Invalid .bpi_json | Validate JSON syntax, check required fields |

### Rules

1. **Max 7 nodes** per graph in .bpi_json — complex logic belongs in C++
2. Blueprint only does: Event → Call → Branch → Call
3. Always `wax_recall` + `wax_fact_search` before modifying anything
4. Use `existing:Title` to reference nodes already in the Blueprint
5. Use `wax_blueprint_compressed_read` to verify node titles before patching
6. After C++ changes, rebuild before `wax_blueprint_import`
