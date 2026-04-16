"""Blueprint refresh logic: re-parse one or more .bpl_json files, re-embed,
and upsert into Elasticsearch.

Exposed as pure functions so both the FastAPI `/bp_refresh` endpoint and
unit tests can use them. The embedding model and ES client are injected
to keep the module testable.

Purpose regeneration is NOT done here — it requires the LLM (llama-server)
which competes for the same GPU as the embedding model. Callers that want
a fresh purpose should use scripts/run_full_reindex.ps1 (which orchestrates
the GPU handoff) or call scripts/generate_bp_purposes.py separately with
the embedding service stopped.
"""
from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Any, Callable, Iterable

# Make scripts/ importable so we can reuse parse_bp and doc builders.
_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPTS = _REPO_ROOT / "scripts"
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from parse_bp_facts import parse_bp  # type: ignore
from index_bp_to_es import build_es_doc, build_text_for_embedding  # type: ignore


# ── Filesystem lookup ────────────────────────────────────────────────────────

def find_bpl_file(entity: str, export_dir: Path) -> Path | None:
    """Find the .bpl_json file corresponding to a bp: entity.

    UE5 exporter filename pattern: '_Path_With_Slashes_To_Asset_AssetName.bpl_json',
    which always ends in '_AssetName_AssetName.bpl_json' because asset_path repeats
    the name at the end.
    """
    if not entity.startswith("bp:"):
        return None
    asset_name = entity[3:]
    if not asset_name:
        return None

    # Exact suffix: "_<name>_<name>.bpl_json"
    candidates = list(export_dir.glob(f"*_{asset_name}_{asset_name}.bpl_json"))
    if len(candidates) == 1:
        return candidates[0]
    if len(candidates) > 1:
        # Pick the shortest path — usually correspond to the asset, not a child.
        candidates.sort(key=lambda p: len(p.name))
        return candidates[0]

    # Fallback: suffix "<name>.bpl_json"
    candidates = list(export_dir.glob(f"*_{asset_name}.bpl_json"))
    return candidates[0] if candidates else None


# ── Single-BP refresh ────────────────────────────────────────────────────────

def refresh_single(
    entity: str,
    export_dir: Path,
    *,
    encoder: Callable[[list[str]], list[list[float]]],
    es_get: Callable[[str], dict | None],
    es_upsert: Callable[[str, dict], None],
) -> dict[str, Any]:
    """Refresh a single Blueprint.

    Returns a status dict with at minimum `{status, entity, structural_hash}`.

    Status values:
        "updated"    — structural_hash changed, ES doc reindexed
        "unchanged"  — structural_hash matches existing doc, nothing done
        "indexed"    — no existing doc, created new one
        "not_found"  — no .bpl_json file found for this entity
        "parse_failed" — JSON parse error
    """
    t0 = time.perf_counter()

    file_path = find_bpl_file(entity, export_dir)
    if file_path is None:
        return {
            "status": "not_found",
            "entity": entity,
            "message": f"no .bpl_json matching {entity!r} in {export_dir}",
        }

    raw = file_path.read_bytes()
    facts = parse_bp(raw, file_path.name)
    if facts is None:
        return {
            "status": "parse_failed",
            "entity": entity,
            "file": str(file_path),
        }

    new_hash = facts["structural_hash"]
    existing = es_get(entity)
    prev_hash = None
    prev_purpose = ""
    if existing:
        prev_hash = existing.get("structural_hash")
        prev_purpose = existing.get("purpose") or ""

    if prev_hash == new_hash:
        return {
            "status": "unchanged",
            "entity": entity,
            "structural_hash": new_hash,
            "elapsed_ms": round((time.perf_counter() - t0) * 1000, 1),
        }

    # Re-embed using the (possibly stale) previous purpose plus new structural text.
    text = build_text_for_embedding(facts, prev_purpose)
    [vec] = encoder([text])

    doc = build_es_doc(facts, prev_purpose, vec)
    es_upsert(entity, doc)

    return {
        "status": "updated" if prev_hash else "indexed",
        "entity": entity,
        "structural_hash": new_hash,
        "prev_hash": prev_hash,
        "purpose_stale": bool(prev_purpose),  # true if we kept an old purpose
        "elapsed_ms": round((time.perf_counter() - t0) * 1000, 1),
    }


# ── Bulk refresh ────────────────────────────────────────────────────────────

def refresh_many(
    entities: Iterable[str] | None,
    export_dir: Path,
    *,
    encoder: Callable[[list[str]], list[list[float]]],
    es_get: Callable[[str], dict | None],
    es_upsert: Callable[[str, dict], None],
) -> dict[str, Any]:
    """Refresh a list of entities, or all .bpl_json files if entities is None."""
    if entities is None:
        # Enumerate everything in export_dir and derive entity from asset_name.
        targets = []
        for f in sorted(export_dir.glob("*.bpl_json")):
            raw = f.read_bytes()
            facts = parse_bp(raw, f.name)
            if facts:
                targets.append(facts["entity"])
    else:
        targets = list(entities)

    t0 = time.perf_counter()
    results = {
        "total": len(targets),
        "updated": 0,
        "indexed": 0,
        "unchanged": 0,
        "not_found": 0,
        "parse_failed": 0,
        "by_entity": [],
    }

    for entity in targets:
        r = refresh_single(entity, export_dir,
                           encoder=encoder, es_get=es_get, es_upsert=es_upsert)
        status = r["status"]
        if status in results:
            results[status] += 1
        results["by_entity"].append(r)

    results["elapsed_ms"] = round((time.perf_counter() - t0) * 1000, 1)
    return results
