"""End-to-end sanity check for the WAX semantic BP stack.

Runs through:
    1. Embedding service is up and returns expected dim
    2. Elasticsearch is reachable and wax_bp_v1 index exists
    3. Index three fake BP docs
    4. Query by semantic meaning and by keyword → print top hits
    5. Delete fake docs

Usage:
    python scripts/test_embed_roundtrip.py
"""
from __future__ import annotations

import json
import os
import sys
import time
from datetime import datetime, timezone
from urllib.request import Request, urlopen
from urllib.error import URLError

from elasticsearch import Elasticsearch

EMBED_URL = os.environ.get("WAX_EMBED_URL", "http://127.0.0.1:8088")
ES_URL    = os.environ.get("WAX_ES_URL",    "http://127.0.0.1:9200")
INDEX     = os.environ.get("WAX_ES_BP_INDEX", "wax_bp_v1")


def http_post(url: str, body: dict, timeout: float = 60.0) -> dict:
    data = json.dumps(body).encode("utf-8")
    req = Request(url, data=data, headers={"Content-Type": "application/json"}, method="POST")
    with urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def step(msg: str) -> None:
    print(f"\n==== {msg} ====")


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


# ── 1. Embedding service health ─────────────────────────────────────────────
step("1. Checking embedding service /health")
try:
    with urlopen(f"{EMBED_URL}/health", timeout=10) as r:
        health = json.loads(r.read())
except URLError as e:
    fail(f"embedding service unreachable at {EMBED_URL}: {e}")

print(json.dumps(health, indent=2))
if health.get("status") != "ok":
    fail(f"service status is {health.get('status')!r}, expected 'ok'")
dim = health["dim"]
print(f"✓ service ready, dim={dim}")


# ── 2. Elasticsearch ─────────────────────────────────────────────────────────
step("2. Checking Elasticsearch")
es = Elasticsearch(ES_URL, request_timeout=30)
if not es.ping():
    fail(f"ES unreachable at {ES_URL}")
if not es.indices.exists(index=INDEX):
    fail(f"index {INDEX!r} missing — run es_setup_bp_index.py first")
print(f"✓ ES ready, index={INDEX}")


# ── 3. Index three fake BP docs ─────────────────────────────────────────────
step("3. Indexing three sample BP docs")

samples = [
    {
        "entity": "bp:TEST_SpawnEffect",
        "asset_name": "TEST_SpawnEffect",
        "kind": "gameplay_ability",
        "parent_class": "GameplayAbility",
        "purpose": "Applies a spawn-in effect and disables player input for a short delay, then restores input and removes the effect when the ability ends.",
        "events": ["K2_ActivateAbility", "K2_OnEndAbility"],
        "calls": ["BP_ApplyGameplayEffectToSelf", "Delay", "DisableInput", "EnableInput"],
        "doc_kind": "blueprint",
    },
    {
        "entity": "bp:TEST_Jump",
        "asset_name": "TEST_Jump",
        "kind": "gameplay_ability",
        "parent_class": "GameplayAbility",
        "purpose": "Makes the hero character jump when the ability is activated.",
        "events": ["K2_ActivateAbility"],
        "calls": ["Jump", "K2_EndAbility"],
        "doc_kind": "blueprint",
    },
    {
        "entity": "bp:TEST_AnimLayer",
        "asset_name": "TEST_AnimLayer",
        "kind": "anim_blueprint",
        "parent_class": "AnimInstance",
        "purpose": "Animation layer that updates weapon-specific animations each frame based on the owning pawn's state.",
        "events": ["BlueprintUpdateAnimation"],
        "calls": ["TryGetPawnOwner"],
        "doc_kind": "blueprint",
    },
]

for doc in samples:
    embed_resp = http_post(f"{EMBED_URL}/embed", {"text": doc["purpose"]})
    vec = embed_resp["vector"]
    if len(vec) != dim:
        fail(f"dim mismatch: service returned {len(vec)}, health said {dim}")

    body = {
        **doc,
        "embedding": vec,
        "indexed_at": datetime.now(timezone.utc).isoformat(),
    }
    es.index(index=INDEX, id=doc["entity"], document=body, refresh="wait_for")
    print(f"  indexed {doc['entity']}")
print("✓ indexed 3 sample docs")


# ── 4. Query 1: semantic (kNN vector search) ─────────────────────────────────
step("4. Semantic query: 'temporarily freezes player controls after respawn'")
query_text = "temporarily freezes player controls after respawn"
q_resp = http_post(f"{EMBED_URL}/embed", {"text": query_text, "is_query": True})
q_vec = q_resp["vector"]

knn_result = es.search(
    index=INDEX,
    knn={"field": "embedding", "query_vector": q_vec, "k": 5, "num_candidates": 50},
    source=["entity", "purpose"],
)
print("Top hits (by semantic similarity):")
for hit in knn_result["hits"]["hits"]:
    print(f"  [{hit['_score']:.3f}] {hit['_source']['entity']}")
    print(f"         {hit['_source']['purpose'][:100]}")


# ── 5. Query 2: keyword (BM25) ───────────────────────────────────────────────
step("5. Keyword query: 'jump'")
bm25_result = es.search(
    index=INDEX,
    query={"match": {"purpose": "jump"}},
    source=["entity", "purpose"],
)
print("Top hits (by BM25):")
for hit in bm25_result["hits"]["hits"]:
    print(f"  [{hit['_score']:.3f}] {hit['_source']['entity']}")
    print(f"         {hit['_source']['purpose'][:100]}")


# ── 6. Clean up test docs ────────────────────────────────────────────────────
step("6. Cleaning up test docs")
for doc in samples:
    es.delete(index=INDEX, id=doc["entity"], refresh="wait_for")
print("✓ test docs removed")

print("\n✓✓✓ roundtrip OK")
