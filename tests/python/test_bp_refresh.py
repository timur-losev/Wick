"""Unit tests for services/embedding/bp_refresh.py.

The refresh logic depends on three things: the embedding encoder, an ES
get function, and an ES upsert function. All three are injected, so these
tests don't hit the real embedding model or Elasticsearch — we pass fake
callables.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make services/embedding/ importable
REPO_ROOT = Path(__file__).resolve().parents[2]
SERVICES = REPO_ROOT / "services" / "embedding"
if str(SERVICES) not in sys.path:
    sys.path.insert(0, str(SERVICES))

from bp_refresh import find_bpl_file, refresh_single, refresh_many  # type: ignore

FIXTURES = Path(__file__).resolve().parent / "fixtures"


# ── Fake dependencies ───────────────────────────────────────────────────────

class FakeEs:
    """In-memory replacement for ES used by refresh_* functions."""

    def __init__(self, initial: dict[str, dict] | None = None):
        self.docs: dict[str, dict] = dict(initial or {})
        self.upserts: list[tuple[str, dict]] = []

    def get(self, entity: str) -> dict | None:
        return self.docs.get(entity)

    def upsert(self, entity: str, doc: dict) -> None:
        self.upserts.append((entity, doc))
        self.docs[entity] = doc


def fake_encoder(texts: list[str]) -> list[list[float]]:
    """Return deterministic fake embeddings — length equals real dim (1536)."""
    # Not fp16 in tests; that's fine, only dim matters.
    return [[float(len(t) % 7) / 7.0] * 1536 for t in texts]


# ── find_bpl_file ───────────────────────────────────────────────────────────

class TestFindBplFile:
    def test_matches_double_name_suffix(self, tmp_path):
        (tmp_path / "_ShooterCore_Foo_GA_X_GA_X.bpl_json").write_bytes(b"{}")
        assert find_bpl_file("bp:GA_X", tmp_path).name == "_ShooterCore_Foo_GA_X_GA_X.bpl_json"

    def test_returns_none_for_missing(self, tmp_path):
        assert find_bpl_file("bp:NOPE", tmp_path) is None

    def test_requires_bp_prefix(self, tmp_path):
        (tmp_path / "_Foo_X_X.bpl_json").write_bytes(b"{}")
        assert find_bpl_file("cpp:X", tmp_path) is None

    def test_empty_after_prefix_is_none(self, tmp_path):
        assert find_bpl_file("bp:", tmp_path) is None

    def test_single_name_suffix_fallback(self, tmp_path):
        # UE5 usually writes "_Name_Name.bpl_json" but be tolerant.
        (tmp_path / "_Some_Path_Weird.bpl_json").write_bytes(b"{}")
        found = find_bpl_file("bp:Weird", tmp_path)
        assert found is not None
        assert found.name.endswith("_Weird.bpl_json")

    def test_multiple_matches_picks_shortest(self, tmp_path):
        (tmp_path / "_A_GA_X_GA_X.bpl_json").write_bytes(b"{}")
        (tmp_path / "_A_B_C_GA_X_GA_X.bpl_json").write_bytes(b"{}")
        # Shortest name wins — less nested path
        assert find_bpl_file("bp:GA_X", tmp_path).name == "_A_GA_X_GA_X.bpl_json"


# ── refresh_single ──────────────────────────────────────────────────────────

class TestRefreshSingle:
    def _copy_fixture(self, tmp_path: Path, src_name: str, dst_name: str) -> None:
        (tmp_path / dst_name).write_bytes((FIXTURES / src_name).read_bytes())

    def test_indexes_new_doc_when_no_existing(self, tmp_path):
        self._copy_fixture(tmp_path, "ga_spawn_effect.bpl_json",
                           "_ShooterCore_GA_SpawnEffect_GA_SpawnEffect.bpl_json")
        es = FakeEs()
        r = refresh_single("bp:GA_SpawnEffect", tmp_path,
                           encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r["status"] == "indexed"
        assert r["entity"] == "bp:GA_SpawnEffect"
        assert r["prev_hash"] is None
        assert r["structural_hash"]
        assert len(es.upserts) == 1

    def test_returns_unchanged_when_hash_matches(self, tmp_path):
        self._copy_fixture(tmp_path, "ga_spawn_effect.bpl_json",
                           "_ShooterCore_GA_SpawnEffect_GA_SpawnEffect.bpl_json")
        es = FakeEs()
        first = refresh_single("bp:GA_SpawnEffect", tmp_path,
                               encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        # Second refresh with no file change should be a no-op
        second = refresh_single("bp:GA_SpawnEffect", tmp_path,
                                encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert second["status"] == "unchanged"
        assert second["structural_hash"] == first["structural_hash"]
        assert len(es.upserts) == 1, "unchanged refresh must not re-upsert"

    def test_updates_when_hash_differs(self, tmp_path):
        """Simulate a patched BP: same entity, different content."""
        self._copy_fixture(tmp_path, "ga_spawn_effect.bpl_json",
                           "_ShooterCore_GA_SpawnEffect_GA_SpawnEffect.bpl_json")

        # Pre-seed ES with a stale hash + purpose
        es = FakeEs({
            "bp:GA_SpawnEffect": {
                "structural_hash": "0000stalehash000",
                "purpose": "old purpose that we want to retain",
            }
        })

        r = refresh_single("bp:GA_SpawnEffect", tmp_path,
                           encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r["status"] == "updated"
        assert r["prev_hash"] == "0000stalehash000"
        assert r["purpose_stale"] is True
        # Purpose must have been retained in the upserted doc
        ent, doc = es.upserts[0]
        assert ent == "bp:GA_SpawnEffect"
        assert doc["purpose"] == "old purpose that we want to retain"
        # Embedding dim must be 1536 (mapping contract)
        assert len(doc["embedding"]) == 1536

    def test_returns_not_found_for_missing_file(self, tmp_path):
        es = FakeEs()
        r = refresh_single("bp:NONEXISTENT", tmp_path,
                           encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r["status"] == "not_found"
        assert "bp:NONEXISTENT" in r["message"]
        assert es.upserts == []

    def test_returns_parse_failed_for_bad_json(self, tmp_path):
        (tmp_path / "_X_Junk_Junk.bpl_json").write_bytes(b"this is not JSON")
        es = FakeEs()
        r = refresh_single("bp:Junk", tmp_path,
                           encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r["status"] == "parse_failed"
        assert es.upserts == []

    def test_result_includes_timing(self, tmp_path):
        self._copy_fixture(tmp_path, "abp_pistol.bpl_json",
                           "_Foo_ABP_PistolAnimLayers_ABP_PistolAnimLayers.bpl_json")
        es = FakeEs()
        r = refresh_single("bp:ABP_PistolAnimLayers", tmp_path,
                           encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert "elapsed_ms" in r
        assert isinstance(r["elapsed_ms"], (int, float))


# ── refresh_many ────────────────────────────────────────────────────────────

class TestRefreshMany:
    def test_scans_directory_when_entities_is_none(self, tmp_path):
        # Copy 2 fixtures
        (tmp_path / "_X_GA_SpawnEffect_GA_SpawnEffect.bpl_json").write_bytes(
            (FIXTURES / "ga_spawn_effect.bpl_json").read_bytes())
        (tmp_path / "_Y_ABP_PistolAnimLayers_ABP_PistolAnimLayers.bpl_json").write_bytes(
            (FIXTURES / "abp_pistol.bpl_json").read_bytes())

        es = FakeEs()
        r = refresh_many(None, tmp_path,
                         encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r["total"] == 2
        assert r["indexed"] == 2
        assert r["unchanged"] == 0
        assert r["not_found"] == 0
        entities = sorted(x["entity"] for x in r["by_entity"])
        assert entities == ["bp:ABP_PistolAnimLayers", "bp:GA_SpawnEffect"]

    def test_explicit_entity_list(self, tmp_path):
        (tmp_path / "_X_GA_SpawnEffect_GA_SpawnEffect.bpl_json").write_bytes(
            (FIXTURES / "ga_spawn_effect.bpl_json").read_bytes())

        es = FakeEs()
        r = refresh_many(["bp:GA_SpawnEffect", "bp:Missing"], tmp_path,
                         encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r["total"] == 2
        assert r["indexed"] == 1
        assert r["not_found"] == 1

    def test_mixed_unchanged_and_updated(self, tmp_path):
        (tmp_path / "_A_GA_SpawnEffect_GA_SpawnEffect.bpl_json").write_bytes(
            (FIXTURES / "ga_spawn_effect.bpl_json").read_bytes())

        es = FakeEs()
        # First pass: index
        r1 = refresh_many(["bp:GA_SpawnEffect"], tmp_path,
                          encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        # Second pass: unchanged
        r2 = refresh_many(["bp:GA_SpawnEffect"], tmp_path,
                          encoder=fake_encoder, es_get=es.get, es_upsert=es.upsert)
        assert r1["indexed"] == 1
        assert r2["unchanged"] == 1
        assert r2["indexed"] == 0
