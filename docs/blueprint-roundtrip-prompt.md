## Blueprint Roundtrip: Hybrid C++ + Minimal BP Wiring

You have tools to **read, modify, and import UE5 Blueprints** via the WAX system. Follow these rules strictly.

### Core principle

> **All game logic goes in C++. Blueprint is only thin wiring.**

When modifying or creating Blueprint functionality:
1. Write a C++ `UFUNCTION(BlueprintCallable)` with the logic
2. Generate a minimal `.bpi_json` intent that wires the C++ function into the Blueprint graph

### Available tools

| Tool | Purpose |
|------|---------|
| `wax_blueprint_compressed_read` | Read compressed BP (semantic data only, no noise) |
| `wax_blueprint_patch` | Apply .bpi_json intent patch to a blueprint |
| `wax_blueprint_import` | Import modified .bpl_json into UE5 + compile |
| `wax_fact_search` | Query existing facts about BP entities |
| `wax_recall` | Search indexed code for patterns |

### .bpi_json format

The intent patch describes **what to change**, not the full blueprint.

```jsonc
{
  "target": "/Game/Path/Asset.Asset",  // UE asset path
  "description": "What this patch does",

  // Optional: create new blueprint instead of patching existing
  "create": true,
  "parent_class": "/Script/Engine.Actor",

  // Optional: add/update variables
  "variables": [
    {"var_name": "Health", "var_type": {"category": "real"}, "default_value": "100.0"}
  ],

  "graphs": [{
    "name": "EventGraph",

    // Nodes to add. "ref" is a local alias used in add_links.
    "add_nodes": [
      {"ref": "my_call", "type": "CallFunction",
       "function": "MyFunction", "function_owner": "MyHelperClass"}
    ],

    // Connect pins. Use "existing:Node Title" for existing nodes.
    "add_links": [
      {"from": "existing:ReceiveTick", "from_pin": "then",
       "to": "my_call", "to_pin": "execute"}
    ],

    // Optional: remove existing links before adding new ones
    "remove_links": [
      {"from": "existing:NodeA", "from_pin": "then",
       "to": "existing:NodeB", "to_pin": "execute"}
    ],

    // Optional: remove nodes by title
    "remove_nodes": ["Old Node Title"],

    // Optional: set pin default values
    "set_defaults": [
      {"node": "my_call", "pin": "Damage", "value": "50.0"}
    ]
  }]
}
```

### Node types

| type | UE5 Node | Key fields |
|------|----------|------------|
| `CallFunction` | K2Node_CallFunction | `function`, `function_owner` |
| `Event` | K2Node_Event | `event`, `event_owner` (default: Actor) |
| `CustomEvent` | K2Node_CustomEvent | `event_name` |
| `VariableGet` | K2Node_VariableGet | `variable` |
| `VariableSet` | K2Node_VariableSet | `variable` |
| `Branch` | K2Node_IfThenElse | _(no extra fields)_ |
| `Sequence` | K2Node_ExecutionSequence | _(no extra fields)_ |
| `Cast` | K2Node_DynamicCast | `cast_target` |
| `MacroInstance` | K2Node_MacroInstance | `macro` |

### Standard pin names

| Pin | Where | Direction |
|-----|-------|-----------|
| `then` | All nodes | exec out |
| `execute` | All nodes | exec in |
| `ReturnValue` | CallFunction | data out |
| `Condition` | Branch | data in |
| `True` / `False` | Branch | exec out |
| `self` | Method calls | data in |
| _(parameter name)_ | UFUNCTION params | matches C++ parameter name |

**Key insight**: When you write `UFUNCTION(BlueprintCallable) void Foo(AActor* Target, float Damage)`, the Blueprint pins are exactly `Target` and `Damage`. You wrote the C++, so you know the pin names.

### Workflow

1. **Query context**: `wax_fact_search` + `wax_recall` to understand existing BP and C++
2. **Read BP**: `wax_blueprint_compressed_read` for current state
3. **Write C++**: Create/modify UFUNCTION in Source/
4. **Patch BP**: `wax_blueprint_patch` with minimal .bpi_json (max 7 nodes)
5. **Import**: `wax_blueprint_import` with `-Compile=1`
6. If errors → fix and retry (max 3 times)

### Examples

#### Add a function call to an existing event

```jsonc
{
  "target": "/Game/Characters/Heroes/Abilities/AN_Melee.AN_Melee",
  "graphs": [{
    "name": "Received_Notify",
    "add_nodes": [
      {"ref": "calc", "type": "CallFunction",
       "function": "GetMeleeDamageMultiplier",
       "function_owner": "MeleeDamageHelper"}
    ],
    "add_links": [
      {"from": "existing:Get Owner", "from_pin": "ReturnValue",
       "to": "calc", "to_pin": "Attacker"}
    ]
  }]
}
```

#### Insert a check (Branch) into existing exec flow

```jsonc
{
  "target": "/Game/Characters/Heroes/Abilities/AN_Melee.AN_Melee",
  "graphs": [{
    "name": "Received_Notify",
    "remove_links": [
      {"from": "existing:Received Notify", "from_pin": "then",
       "to": "existing:Get Owner", "to_pin": "execute"}
    ],
    "add_nodes": [
      {"ref": "check", "type": "CallFunction",
       "function": "ShouldApplyMelee", "function_owner": "MeleeDamageHelper"},
      {"ref": "branch", "type": "Branch"}
    ],
    "add_links": [
      {"from": "existing:Received Notify", "from_pin": "then",
       "to": "check", "to_pin": "execute"},
      {"from": "check", "from_pin": "then",
       "to": "branch", "to_pin": "execute"},
      {"from": "check", "from_pin": "ReturnValue",
       "to": "branch", "to_pin": "Condition"},
      {"from": "branch", "from_pin": "True",
       "to": "existing:Get Owner", "to_pin": "execute"}
    ]
  }]
}
```

#### Create a new Blueprint

```jsonc
{
  "target": "/Game/Abilities/GA_DoubleJump.GA_DoubleJump",
  "create": true,
  "parent_class": "/Script/GameplayAbilities.GameplayAbility",
  "graphs": [{
    "name": "EventGraph",
    "add_nodes": [
      {"ref": "activate", "type": "Event", "event": "K2_ActivateAbility"},
      {"ref": "jump", "type": "CallFunction",
       "function": "ExecuteDoubleJump", "function_owner": "DoubleJumpHelper"}
    ],
    "add_links": [
      {"from": "activate", "from_pin": "then", "to": "jump", "to_pin": "execute"}
    ]
  }]
}
```

### Rules

1. **Max 7 nodes** per graph in .bpi_json
2. **All complex logic in C++** — math, conditionals, loops, attribute access
3. Blueprint only does: Event → Call → Branch → Call
4. Pin names for your UFUNCTION params = C++ parameter names exactly
5. Use `existing:Title` to reference nodes already in the blueprint
6. Always query WAX first to understand what exists before patching
