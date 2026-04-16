"""Phase 1: Structural BP enricher (native Python port of bpl_structural_enricher_poc.cpp).

Parses every .bpl_json in the export directory and emits a single bp_facts.json
with per-entity structural facts. No GPU, no LLM, ~30 seconds for 657 BPs.

Output shape (list of dicts, one per BP):
    {
      "entity": "bp:GA_SpawnEffect",
      "asset_name": "GA_SpawnEffect",
      "asset_path": "/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect",
      "kind": "gameplay_ability",          # derived
      "parent_class_hint": "GameplayAbility",  # from event refs
      "node_count": 33,
      "link_count": 32,
      "graphs": ["EventGraph"],
      "events": ["K2_ActivateAbility", "K2_OnEndAbility"],
      "custom_events": ["EnableInputAgain"],
      "calls": ["Delay", "GetPlayLength", ...],          # deduped
      "call_owners": ["KismetSystemLibrary", ...],
      "gets_variables": [...], "sets_variables": [...],
      "variables": [{"name": "SpawnMontage", "type": "object"}, ...],
      "casts_to": ["LyraCharacter"],
      "macros": ["StandardMacros"],
      "exec_chains": {
          "K2_ActivateAbility": "Set CachedController → BP_ApplyGameplayEffectToSelf → ...",
      },
      "structural_hash": "<sha256 of the above for change detection>",
    }

Usage:
    python scripts/parse_bp_facts.py [--export-dir DIR] [--output FILE]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

# ── Helpers ─────────────────────────────────────────────────────────────────

EXEC_OUT_PINS = {"then", "True", "False", "Completed", "OnFailed", "OnSuccess", "OnFinish"}


def shorten_path(p: str) -> str:
    """/Script/Engine.Actor → Actor; /Script/BlueprintGraph.K2Node_Event → K2Node_Event"""
    if not p:
        return p
    if "." in p:
        return p.rsplit(".", 1)[-1]
    if "/" in p:
        return p.rsplit("/", 1)[-1]
    return p


def is_exec_out(pin: str) -> bool:
    if not pin:
        return False
    return (pin in EXEC_OUT_PINS) or pin.startswith("then_") or pin.startswith("Then ")


def describe_node(n: dict) -> str:
    """Human-readable one-word(ish) description for exec-chain."""
    cls = n["class_short"]
    if cls in ("K2Node_CallFunction", "K2Node_CallParentFunction", "K2Node_LatentAbilityCall"):
        return n.get("fn_name") or n.get("title") or cls
    if cls == "K2Node_VariableSet":
        return f"Set {n.get('variable_name') or '?'}"
    if cls == "K2Node_VariableGet":
        return f"Get {n.get('variable_name') or '?'}"
    if cls == "K2Node_DynamicCast":
        return f"Cast<{n.get('cast_target_short') or '?'}>"
    if cls == "K2Node_IfThenElse":
        return "Branch"
    if cls == "K2Node_ExecutionSequence":
        return "Sequence"
    if cls == "K2Node_MacroInstance":
        return f"Macro:{n.get('macro_ref') or '?'}"
    return cls


# ── Core: parse a single BP into a facts dict ───────────────────────────────

def parse_bp(raw: bytes, source_filename: str) -> dict[str, Any] | None:
    # Strip UTF-8 BOM if present
    if raw[:3] == b"\xef\xbb\xbf":
        raw = raw[3:]

    try:
        root = json.loads(raw)
    except json.JSONDecodeError as e:
        print(f"  ! JSON parse failed for {source_filename}: {e}", file=sys.stderr)
        return None

    if not isinstance(root, dict):
        return None

    asset_name = root.get("asset_name", "")
    if not asset_name:
        return None

    # Pass 1: build node map
    nodes_by_guid: dict[str, dict] = {}
    graphs_list: list[str] = []
    total_nodes, total_links = 0, 0

    for g in root.get("graphs", []) or []:
        if not isinstance(g, dict):
            continue
        gname = g.get("name", "")
        graphs_list.append(gname)

        for node in g.get("nodes", []) or []:
            if not isinstance(node, dict):
                continue
            guid = node.get("node_guid")
            if not guid:
                continue

            class_short = shorten_path(node.get("class_path", ""))
            info: dict[str, Any] = {
                "guid": guid,
                "class_short": class_short,
                "title": node.get("title", ""),
                "graph_name": gname,
                "exec_out": [],
            }

            # function reference (CallFunction, Event, LatentAbilityCall)
            if isinstance(node.get("function"), dict):
                fn = node["function"]
                if fn.get("member_name"):
                    info["fn_name"] = fn["member_name"]
                if fn.get("member_parent"):
                    info["fn_owner_short"] = shorten_path(fn["member_parent"])
            # Event ref overrides fn fields — K2Node_Event uses event_ref
            if isinstance(node.get("event_ref"), dict):
                ev = node["event_ref"]
                if ev.get("member_name"):
                    info["fn_name"] = ev["member_name"]
                if ev.get("member_parent"):
                    info["fn_owner_short"] = shorten_path(ev["member_parent"])
            # CustomEvent
            cfn = node.get("custom_function_name", "")
            if cfn and cfn != "None":
                info["custom_event"] = cfn
            # Variable reference (object form or legacy string)
            if isinstance(node.get("variable_ref"), dict):
                mn = node["variable_ref"].get("member_name")
                if mn:
                    info["variable_name"] = mn
            if node.get("var_name"):
                info["variable_name"] = node["var_name"]
            # Cast target
            if node.get("cast_target"):
                info["cast_target_short"] = shorten_path(node["cast_target"])
            # Macro reference (object form)
            if isinstance(node.get("macro_ref"), dict):
                mb = node["macro_ref"].get("macro_blueprint")
                if mb:
                    info["macro_ref"] = shorten_path(mb)

            nodes_by_guid[guid] = info

        # Pass 2: populate exec_out from links
        for link in g.get("links", []) or []:
            if not isinstance(link, dict):
                continue
            total_links += 1
            from_guid = link.get("from_node_guid")
            to_guid = link.get("to_node_guid")
            from_pin = link.get("from_pin_name", "")
            if not (from_guid and to_guid):
                continue
            n = nodes_by_guid.get(from_guid)
            if n is None:
                continue
            if is_exec_out(from_pin):
                n["exec_out"].append((from_pin, to_guid))

        total_nodes += len(g.get("nodes", []) or [])

    # ── Collect unique facts across all graphs ─────────────────────────────
    events: set[str] = set()
    event_owners: set[str] = set()
    custom_events: set[str] = set()
    calls: set[str] = set()
    call_owners: set[str] = set()
    gets: set[str] = set()
    sets: set[str] = set()
    casts: set[str] = set()
    macros: set[str] = set()

    for n in nodes_by_guid.values():
        cls = n["class_short"]
        if cls == "K2Node_Event":
            if "fn_name" in n:
                events.add(n["fn_name"])
            if "fn_owner_short" in n:
                event_owners.add(n["fn_owner_short"])
        elif cls == "K2Node_CustomEvent":
            if "custom_event" in n:
                custom_events.add(n["custom_event"])
        elif cls in ("K2Node_CallFunction", "K2Node_CallParentFunction", "K2Node_LatentAbilityCall"):
            if "fn_name" in n:
                calls.add(n["fn_name"])
            if "fn_owner_short" in n:
                call_owners.add(n["fn_owner_short"])
        elif cls == "K2Node_VariableGet":
            if "variable_name" in n:
                gets.add(n["variable_name"])
        elif cls == "K2Node_VariableSet":
            if "variable_name" in n:
                sets.add(n["variable_name"])
        elif cls == "K2Node_DynamicCast":
            if "cast_target_short" in n:
                casts.add(n["cast_target_short"])
        elif cls == "K2Node_MacroInstance":
            if "macro_ref" in n:
                macros.add(n["macro_ref"])

    # Variables from top-level `variables` list
    variables: list[dict] = []
    for v in root.get("variables", []) or []:
        if not isinstance(v, dict):
            continue
        vname = v.get("var_name")
        if not vname:
            continue
        vtype = "unknown"
        if isinstance(v.get("var_type"), dict):
            vtype = v["var_type"].get("category", "unknown")
        variables.append({"name": vname, "type": vtype})

    # ── Derive kind ─────────────────────────────────────────────────────────
    kind = "blueprint"
    if "K2_ActivateAbility" in events or "K2_OnEndAbility" in events or asset_name.startswith("GA_"):
        kind = "gameplay_ability"
    elif asset_name.startswith("GE_"):
        kind = "gameplay_effect"
    elif asset_name.startswith(("GC_", "GCNL_")):
        kind = "gameplay_cue"
    elif asset_name.startswith(("ABP_", "ALI_")):
        kind = "anim_blueprint"
    elif asset_name.startswith("AN_"):
        kind = "anim_notify"
    elif asset_name.startswith(("W_", "WBP_")):
        kind = "widget"
    elif asset_name.startswith(("BP_", "B_")):
        kind = "actor_blueprint"

    # ── Exec chains ─────────────────────────────────────────────────────────
    def walk_chain(start_guid: str, max_depth: int = 16) -> str:
        path: list[str] = []
        visited: set[str] = set()
        cur = start_guid
        for _ in range(max_depth):
            if cur in visited:
                break
            visited.add(cur)
            n = nodes_by_guid.get(cur)
            if n is None:
                break
            if n["class_short"] not in ("K2Node_Event", "K2Node_CustomEvent"):
                path.append(describe_node(n))
            if not n["exec_out"]:
                break
            cur = n["exec_out"][0][1]
        return " → ".join(path)

    exec_chains: dict[str, str] = {}
    for guid, n in nodes_by_guid.items():
        if n["class_short"] not in ("K2Node_Event", "K2Node_CustomEvent"):
            continue
        start_name = n.get("fn_name") or n.get("custom_event") or ""
        if not start_name or start_name == "None":
            continue
        chain = walk_chain(guid)
        if chain:
            exec_chains[start_name] = chain

    # ── Parent class hint: pick first event owner that's not generic ────────
    parent_class_hint = ""
    for owner in sorted(event_owners):
        if owner and owner != "Object":
            parent_class_hint = owner
            break

    facts = {
        "entity": "bp:" + asset_name,
        "asset_name": asset_name,
        "asset_path": root.get("asset_path", ""),
        "kind": kind,
        "parent_class_hint": parent_class_hint,
        "node_count": total_nodes,
        "link_count": total_links,
        "graphs": graphs_list,
        "events": sorted(events),
        "event_owners": sorted(event_owners),
        "custom_events": sorted(custom_events),
        "calls": sorted(calls),
        "call_owners": sorted(call_owners),
        "gets_variables": sorted(gets),
        "sets_variables": sorted(sets),
        "variables": variables,
        "casts_to": sorted(casts),
        "macros": sorted(macros),
        "exec_chains": exec_chains,
        "source_file": source_filename,
    }

    # Hash for change detection (stable: dump with sort_keys)
    stable = json.dumps({k: v for k, v in facts.items() if k != "source_file"},
                        sort_keys=True, ensure_ascii=False)
    facts["structural_hash"] = hashlib.sha256(stable.encode("utf-8")).hexdigest()[:16]

    return facts


# ── Main ────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--export-dir", default=r"J:\Temp\BlueprintExports",
                        help="Directory with .bpl_json files")
    parser.add_argument("--output", default=r"G:\Proj\Wick\data\bp_facts.json",
                        help="Path to write aggregated facts JSON")
    args = parser.parse_args()

    export_dir = Path(args.export_dir)
    if not export_dir.is_dir():
        print(f"ERROR: export dir not found: {export_dir}", file=sys.stderr)
        return 1

    files = sorted(export_dir.glob("*.bpl_json"))
    print(f"Found {len(files)} .bpl_json files in {export_dir}")

    results: list[dict] = []
    errors: list[tuple[str, str]] = []
    t0 = time.perf_counter()

    for i, f in enumerate(files, 1):
        if i % 50 == 0 or i == 1 or i == len(files):
            elapsed = time.perf_counter() - t0
            print(f"  [{i}/{len(files)}] {f.name[:60]}  ({elapsed:.1f}s)")
        try:
            raw = f.read_bytes()
            facts = parse_bp(raw, f.name)
            if facts:
                results.append(facts)
        except Exception as e:
            errors.append((f.name, str(e)))

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fp:
        json.dump(results, fp, ensure_ascii=False, indent=2)

    elapsed = time.perf_counter() - t0
    print(f"\n✓ Wrote {len(results)} BP facts to {out_path}")
    print(f"  Total time: {elapsed:.1f}s")
    if errors:
        print(f"  Errors: {len(errors)}")
        for name, msg in errors[:10]:
            print(f"    {name}: {msg}")

    # Summary stats
    from collections import Counter
    kinds = Counter(r["kind"] for r in results)
    print(f"\nBP kinds distribution:")
    for k, n in sorted(kinds.items(), key=lambda x: -x[1]):
        print(f"  {k:<22} {n}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
