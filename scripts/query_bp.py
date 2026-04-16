"""Interactive semantic BP search.

Usage:
    python scripts/query_bp.py "player takes damage and respawns"
    python scripts/query_bp.py "spawn effect timer"  --k 10
    python scripts/query_bp.py "jump ability"       --hybrid   (kNN + BM25 via RRF)
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from urllib.request import Request, urlopen

from elasticsearch import Elasticsearch

EMBED_URL = os.environ.get("WAX_EMBED_URL", "http://127.0.0.1:8088")
ES_URL    = os.environ.get("WAX_ES_URL",    "http://127.0.0.1:9200")
INDEX     = os.environ.get("WAX_ES_BP_INDEX", "wax_bp_v1")


def embed(text: str) -> list[float]:
    body = json.dumps({"text": text, "is_query": True}).encode("utf-8")
    req = Request(f"{EMBED_URL}/embed",
                  data=body,
                  headers={"Content-Type": "application/json"},
                  method="POST")
    with urlopen(req, timeout=30) as r:
        return json.loads(r.read())["vector"]


def knn_search(es: Elasticsearch, vec: list[float], k: int) -> list[dict]:
    res = es.search(
        index=INDEX,
        knn={"field": "embedding", "query_vector": vec, "k": k, "num_candidates": max(100, k * 4)},
        source=["entity", "asset_name", "kind", "purpose", "exec_chain"],
        size=k,
    )
    return res["hits"]["hits"]


def bm25_search(es: Elasticsearch, q: str, k: int) -> list[dict]:
    res = es.search(
        index=INDEX,
        query={"multi_match": {
            "query": q,
            "fields": ["purpose^2", "asset_name^2", "text", "exec_chain", "calls"],
        }},
        source=["entity", "asset_name", "kind", "purpose"],
        size=k,
    )
    return res["hits"]["hits"]


def hybrid_search(es: Elasticsearch, q: str, vec: list[float], k: int) -> list[dict]:
    """RRF-style fusion — let ES do it via rank aggregator."""
    res = es.search(
        index=INDEX,
        size=k,
        knn={"field": "embedding", "query_vector": vec, "k": k * 2, "num_candidates": 200, "boost": 0.7},
        query={"multi_match": {
            "query": q,
            "fields": ["purpose^2", "asset_name^2", "text", "exec_chain", "calls"],
            "boost": 0.3,
        }},
        source=["entity", "asset_name", "kind", "purpose"],
    )
    return res["hits"]["hits"]


def print_hits(hits: list[dict], label: str) -> None:
    print(f"\n── {label} ── ({len(hits)} hits)")
    for i, h in enumerate(hits, 1):
        src = h["_source"]
        score = h["_score"]
        print(f"  [{i}]  {score:6.3f}  {src['entity']:<40}  [{src['kind']}]")
        if "purpose" in src and src["purpose"]:
            print(f"         {src['purpose'][:120]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("query", nargs="+", help="search query")
    parser.add_argument("--k", type=int, default=5)
    parser.add_argument("--mode", choices=["knn", "bm25", "hybrid", "all"], default="all")
    args = parser.parse_args()
    query = " ".join(args.query)

    es = Elasticsearch(ES_URL, request_timeout=30)
    vec = embed(query)
    print(f"Query: {query!r}")
    print(f"Embedding dim: {len(vec)}  (first 5: {[round(v, 4) for v in vec[:5]]})")

    if args.mode in ("knn", "all"):
        hits = knn_search(es, vec, args.k)
        print_hits(hits, "kNN (vector cosine)")

    if args.mode in ("bm25", "all"):
        hits = bm25_search(es, query, args.k)
        print_hits(hits, "BM25 (keyword)")

    if args.mode in ("hybrid", "all"):
        hits = hybrid_search(es, query, vec, args.k)
        print_hits(hits, "Hybrid (kNN + BM25 weighted)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
