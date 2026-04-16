"""Create the Elasticsearch index for WAX Blueprint semantic search.

Index name: wax_bp_v1 (version it so we can re-create cleanly)

Document shape:
    {
      "entity": "bp:GA_SpawnEffect",          # primary key
      "asset_name": "GA_SpawnEffect",
      "asset_path": "/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect",
      "kind": "gameplay_ability",
      "parent_class": "GameplayAbility",
      "graph": "EventGraph",                  # for exec_chain docs
      "doc_kind": "blueprint" | "exec_chain" | "variable" | ...
      "text": "...",                          # full searchable text (FTS)
      "purpose": "...",                       # LLM-generated, separately searchable
      "exec_chain": "...",                    # sequence as string
      "calls": ["Delay", "GetPlayLength"],    # structured list
      "events": ["K2_ActivateAbility"],
      "variables": ["SpawnMontage:object"],
      "casts_to": ["LyraCharacter"],
      "embedding": [...],                     # 1536-dim dense_vector
      "indexed_at": "2026-04-16T..."
    }

Usage:
    python scripts/es_setup_bp_index.py [--recreate]
"""
from __future__ import annotations

import argparse
import os
import sys

from elasticsearch import Elasticsearch

ES_URL = os.environ.get("WAX_ES_URL", "http://127.0.0.1:9200")
INDEX = os.environ.get("WAX_ES_BP_INDEX", "wax_bp_v1")
DIMS = int(os.environ.get("WAX_EMBED_DIM", "1536"))  # Qodo-Embed-1-1.5B = 1536


MAPPING = {
    "settings": {
        "number_of_shards": 1,
        "number_of_replicas": 0,
        "analysis": {
            "analyzer": {
                "code_text": {
                    "type": "standard",
                    "stopwords": "_none_",  # don't strip words like "if", "for" in code
                }
            }
        },
    },
    "mappings": {
        "dynamic": "strict",
        "properties": {
            # Identifiers
            "entity":       {"type": "keyword"},
            "asset_name":   {"type": "keyword"},
            "asset_path":   {"type": "keyword"},
            "kind":         {"type": "keyword"},
            "parent_class": {"type": "keyword"},
            "graph":        {"type": "keyword"},
            "doc_kind":     {"type": "keyword"},

            # Searchable text
            "text":         {"type": "text", "analyzer": "code_text"},
            "purpose":      {"type": "text", "analyzer": "standard"},
            "exec_chain":   {"type": "text", "analyzer": "code_text"},

            # Structured facet lists (discrete, filterable, aggregatable)
            "calls":        {"type": "keyword"},
            "events":       {"type": "keyword"},
            "custom_events":{"type": "keyword"},
            "variables":    {"type": "keyword"},
            "casts_to":     {"type": "keyword"},
            "macros":       {"type": "keyword"},
            "calls_owners": {"type": "keyword"},

            # Dense vector for semantic search
            "embedding": {
                "type": "dense_vector",
                "dims": DIMS,
                "index": True,
                "similarity": "cosine",
                "index_options": {"type": "hnsw", "m": 16, "ef_construction": 100},
            },

            # Metadata
            "indexed_at":   {"type": "date"},
            "structural_hash": {"type": "keyword"},  # for change detection
            "node_count":   {"type": "integer"},
            "link_count":   {"type": "integer"},
        },
    },
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--recreate", action="store_true",
                        help="delete index if exists before creating")
    args = parser.parse_args()

    es = Elasticsearch(ES_URL, request_timeout=30)
    if not es.ping():
        print(f"ERROR: Elasticsearch not reachable at {ES_URL}", file=sys.stderr)
        return 2

    exists = es.indices.exists(index=INDEX)
    if exists and args.recreate:
        print(f"Deleting existing index {INDEX!r}")
        es.indices.delete(index=INDEX)
        exists = False

    if exists:
        print(f"Index {INDEX!r} already exists. Re-run with --recreate to replace it.")
        info = es.indices.get(index=INDEX)
        props = info[INDEX]["mappings"]["properties"]
        print(f"  Current dims: {props.get('embedding', {}).get('dims', '?')}")
        return 0

    print(f"Creating index {INDEX!r} with dims={DIMS}")
    es.indices.create(index=INDEX, body=MAPPING)
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
