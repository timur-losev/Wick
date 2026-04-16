"""Phase 3: Embed and bulk-index BP documents into Elasticsearch.

Reads:
    - bp_facts.json      (structural facts from Phase 1)
    - bp_purposes.json   (LLM purposes from Phase 2)

For each BP:
    1. Merge facts + purpose into an ES document
    2. Build a text-for-embedding combining purpose, exec_chains, calls
    3. POST /embed_batch to the embedding service
    4. Bulk index into Elasticsearch

Output: populated wax_bp_v1 index.

Usage:
    python scripts/index_bp_to_es.py [--recreate] [--batch N] [--limit N]
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator
from urllib.request import Request, urlopen
from urllib.error import URLError

from elasticsearch import Elasticsearch, helpers

EMBED_URL = os.environ.get("WAX_EMBED_URL", "http://127.0.0.1:8088")
ES_URL    = os.environ.get("WAX_ES_URL",    "http://127.0.0.1:9200")
INDEX     = os.environ.get("WAX_ES_BP_INDEX", "wax_bp_v1")


def build_text_for_embedding(facts: dict, purpose: str) -> str:
    """Combine facts + purpose into a single string to embed.

    Order matters for cross-encoder-ish models; we put the name and purpose first
    because those carry the most semantic signal.
    """
    parts: list[str] = []
    parts.append(facts["asset_name"])
    parts.append(facts.get("kind", ""))
    if purpose and purpose != "unknown":
        parts.append(purpose)

    chains = facts.get("exec_chains") or {}
    for evt, chain in list(chains.items())[:3]:
        parts.append(f"on {evt}: {chain}")

    calls = facts.get("calls") or []
    if calls:
        parts.append("calls: " + ", ".join(calls[:15]))

    events = facts.get("events") or []
    if events:
        parts.append("events: " + ", ".join(events))

    casts = facts.get("casts_to") or []
    if casts:
        parts.append("casts to: " + ", ".join(casts))

    return ". ".join(p for p in parts if p)


def build_es_doc(facts: dict, purpose: str, embedding: list[float]) -> dict[str, Any]:
    chains = facts.get("exec_chains") or {}
    exec_chain_text = " | ".join(f"{evt}: {chain}" for evt, chain in chains.items())

    variables_flat = [
        f"{v['name']}:{v['type']}" for v in (facts.get("variables") or [])
    ]

    return {
        "entity":           facts["entity"],
        "asset_name":       facts["asset_name"],
        "asset_path":       facts.get("asset_path", ""),
        "kind":             facts.get("kind", "blueprint"),
        "parent_class":     facts.get("parent_class_hint", ""),
        "graph":            facts.get("graphs", [""])[0] if facts.get("graphs") else "",
        "doc_kind":         "blueprint",
        "text":             build_text_for_embedding(facts, purpose),
        "purpose":          purpose or "",
        "exec_chain":       exec_chain_text,
        "calls":            facts.get("calls") or [],
        "events":           facts.get("events") or [],
        "custom_events":    facts.get("custom_events") or [],
        "variables":        variables_flat,
        "casts_to":         facts.get("casts_to") or [],
        "macros":           facts.get("macros") or [],
        "calls_owners":     facts.get("call_owners") or [],
        "embedding":        embedding,
        "indexed_at":       datetime.now(timezone.utc).isoformat(),
        "structural_hash":  facts.get("structural_hash", ""),
        "node_count":       facts.get("node_count", 0),
        "link_count":       facts.get("link_count", 0),
    }


def embed_batch(texts: list[str]) -> list[list[float]]:
    body = json.dumps({"texts": texts}).encode("utf-8")
    req = Request(f"{EMBED_URL}/embed_batch",
                  data=body,
                  headers={"Content-Type": "application/json"},
                  method="POST")
    with urlopen(req, timeout=180) as r:
        resp = json.loads(r.read())
    return resp["vectors"]


def chunked(seq: list, size: int) -> Iterator[list]:
    for i in range(0, len(seq), size):
        yield seq[i:i + size]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--facts", default=r"G:\Proj\Wick\data\bp_facts.json")
    parser.add_argument("--purposes", default=r"G:\Proj\Wick\data\bp_purposes.json")
    parser.add_argument("--recreate", action="store_true",
                        help="drop the index before indexing")
    parser.add_argument("--batch", type=int, default=32,
                        help="embed + bulk batch size")
    parser.add_argument("--limit", type=int, default=0,
                        help="0 = all BPs")
    args = parser.parse_args()

    # Check services
    print(f"Checking embedding service at {EMBED_URL}...")
    try:
        with urlopen(f"{EMBED_URL}/health", timeout=5) as r:
            health = json.loads(r.read())
        print(f"  ✓ ready, model={health.get('model')}, dim={health.get('dim')}, vram={health.get('vram_mb')} MB")
    except Exception as e:
        print(f"ERROR: embedding service not reachable: {e}", file=sys.stderr)
        return 2

    print(f"Checking Elasticsearch at {ES_URL}...")
    es = Elasticsearch(ES_URL, request_timeout=60)
    if not es.ping():
        print("ERROR: ES not reachable", file=sys.stderr)
        return 2
    print("  ✓ ES reachable")

    # Load data
    facts_path = Path(args.facts)
    purposes_path = Path(args.purposes)
    if not facts_path.is_file():
        print(f"ERROR: facts file missing: {facts_path}", file=sys.stderr)
        return 1

    with open(facts_path, encoding="utf-8") as fp:
        all_facts = json.load(fp)
    purposes_map: dict[str, str] = {}
    if purposes_path.is_file():
        with open(purposes_path, encoding="utf-8") as fp:
            for item in json.load(fp):
                purposes_map[item["entity"]] = item.get("purpose", "")
    else:
        print(f"  (no purposes file, continuing without LLM purpose)")

    print(f"Loaded {len(all_facts)} BPs, {len(purposes_map)} purposes")

    if args.limit:
        all_facts = all_facts[: args.limit]
        print(f"  --limit={args.limit}")

    # Drop index if asked
    if args.recreate and es.indices.exists(index=INDEX):
        print(f"Deleting index {INDEX!r}")
        es.indices.delete(index=INDEX)
        # Recreate with schema via scripts/es_setup_bp_index.py logic inline
        from es_setup_bp_index import MAPPING  # type: ignore
        es.indices.create(index=INDEX, body=MAPPING)
        print(f"Recreated index {INDEX!r}")

    # Process in batches: embed then bulk
    total = len(all_facts)
    t_all = time.perf_counter()
    indexed = 0
    errors = 0

    for batch in chunked(all_facts, args.batch):
        texts = []
        payloads = []
        for facts in batch:
            purpose = purposes_map.get(facts["entity"], "")
            text = build_text_for_embedding(facts, purpose)
            texts.append(text)
            payloads.append((facts, purpose))

        # Embed
        t0 = time.perf_counter()
        try:
            vectors = embed_batch(texts)
        except Exception as e:
            print(f"  ! embedding batch failed: {e}", file=sys.stderr)
            errors += len(batch)
            continue
        embed_ms = (time.perf_counter() - t0) * 1000

        # Build actions for bulk
        actions = []
        for (facts, purpose), vec in zip(payloads, vectors):
            doc = build_es_doc(facts, purpose, vec)
            actions.append({
                "_op_type": "index",
                "_index": INDEX,
                "_id": facts["entity"],
                "_source": doc,
            })

        # Bulk
        t0 = time.perf_counter()
        try:
            success, failures = helpers.bulk(es, actions, stats_only=False, raise_on_error=False)
            bulk_ms = (time.perf_counter() - t0) * 1000
            indexed += success
            if failures:
                errors += len(failures)
                for f in failures[:3]:
                    print(f"  ! bulk failure: {f}", file=sys.stderr)
        except Exception as e:
            print(f"  ! bulk failed: {e}", file=sys.stderr)
            errors += len(batch)
            continue

        elapsed = time.perf_counter() - t_all
        rate = indexed / elapsed if elapsed > 0 else 0
        print(f"  [{indexed:>3}/{total}]  embed={embed_ms:>5.0f}ms  bulk={bulk_ms:>4.0f}ms  "
              f"rate={rate:.1f}/s  elapsed={elapsed:.1f}s")

    # Refresh so count is immediate
    es.indices.refresh(index=INDEX)
    final_count = es.count(index=INDEX)["count"]
    elapsed = time.perf_counter() - t_all
    print(f"\n✓ Indexed {indexed} BPs in {elapsed:.1f}s ({errors} errors)")
    print(f"  {INDEX!r} now has {final_count} documents")

    return 0 if errors == 0 else 3


if __name__ == "__main__":
    # Allow importing es_setup_bp_index from the same dir
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
