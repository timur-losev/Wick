## Using the WAX knowledge base

You have access to a local RAG server called **WAX** that contains a fully indexed codebase. Use it to look up existing patterns, classes, dependencies, and architectural decisions **before writing code**.

### What is indexed

| Domain | Volume | Description |
|--------|--------|-------------|
| **UE5 Engine Source** | ~372,000 chunks, 55,530 files | Full C++ engine source (Runtime, Editor, Plugins) with regex enrichment (inheritance, UCLASS/UPROPERTY macros) |
| **OlivaVanilla C++** | ~2,000 chunks | Project gameplay code (`.h`, `.cpp`) with LLM enrichment (semantic facts: purpose, calls, depends_on) |
| **OlivaVanilla Blueprints** | ~5,400 facts | Exported Blueprint graphs (`.bpl_json`) — structured facts only (calls, has_variable, has_event, purpose, depends_on) |

### How to query the server

WAX is a JSON-RPC server at `http://127.0.0.1:8080`. Use `curl` to make requests:

```bash
curl -s -X POST http://127.0.0.1:8080 \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"METHOD","params":{...}}'
```

### Methods for information retrieval

#### 1. `recall` — full-text code search

Returns relevant source code fragments (BM25). This is your primary tool — use it to find specific implementations, class headers, and usage patterns.

```json
{"jsonrpc":"2.0","id":1,"method":"recall","params":{
  "query":"UChaosVehicleMovementComponent WheeledVehicle pawn setup",
  "limit":10
}}
```

The response contains an `items` array, each with `text` (code fragment), `score` (relevance), and `kind` (0=snippet, 1=header/declaration). The `count` field indicates the number of returned results.

**Tips for recall queries:**
- Use specific class and function names: `"FTickPrerequisite AddPrerequisite"` is better than `"tick system dependencies"`
- Combine class names with keywords: `"ULyraHeroComponent Input SetupPlayerInputComponent"`
- To find usage patterns: `"ExperienceManagerComponent CallOrRegister_OnExperienceLoaded BeginPlay"`
- Default `limit` is 12; increase to 20–30 if you need more context

#### 2. `fact.search` — structured fact search

Returns entity-attribute-value facts extracted from code. Use it to quickly determine inheritance hierarchies, dependencies, and class purposes.

```json
{"jsonrpc":"2.0","id":1,"method":"fact.search","params":{
  "entity_prefix":"cpp:UChaosVehicle",
  "limit":20
}}
```

The response contains a `facts` array, each with `entity`, `attribute`, `value`, and `id` fields.

**Entity prefixes:**
- `cpp:ClassName` — C++ classes/functions (UE5 + OlivaVanilla)
- `bp:BlueprintName` — Blueprint entities (OlivaVanilla)

**Common fact attributes:**
- `inherits` — parent class
- `calls` — functions called
- `depends_on` — dependencies
- `purpose` — what the entity does (semantic description)
- `has_variable`, `has_event` — variables and events (Blueprints)
- `kind` — entity type (uclass, ustruct, uenum)
- `specifiers` — UCLASS specifiers

**Examples:**
```bash
# Look up inheritance hierarchy
{"jsonrpc":"2.0","id":1,"method":"fact.search","params":{"entity_prefix":"cpp:AWheeledVehiclePawn","limit":10}}

# Find all weapon-related Blueprints
{"jsonrpc":"2.0","id":2,"method":"fact.search","params":{"entity_prefix":"bp:GA_Weapon","limit":20}}

# Find the purpose of a component
{"jsonrpc":"2.0","id":3,"method":"fact.search","params":{"entity_prefix":"cpp:ULyraHeroComponent","limit":10}}
```

#### 3. `answer.generate` — RAG-augmented answer with citations

Performs a recall, assembles context, and generates a detailed answer via local LLM with citations to source files. Use for complex architectural questions.

```json
{"jsonrpc":"2.0","id":1,"method":"answer.generate","params":{
  "query":"How does Lyra handle pawn possession and input setup?",
  "max_context_items":12,
  "max_context_tokens":6000,
  "max_output_tokens":1024
}}
```

The response contains `answer` (text) and `citations` (array with `relative_path`, `line_start`, `line_end`, `symbol`).

> This method is slow (10–60 sec) — only use it when `recall` + `fact.search` are not enough.

### Usage strategy

1. **Before starting implementation** — run a series of recall queries to understand existing project patterns:
   - How similar components are structured
   - Which base classes are used
   - How Input is connected, which GameplayAbilities/Tags are used
2. **fact.search** — quickly determine inheritance hierarchies and dependencies of the classes you are interested in
3. **answer.generate** — ask an architectural question if recall did not provide enough context
4. **Iterate** — after each significant decision, verify via recall that you are not diverging from project patterns
