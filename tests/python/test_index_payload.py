"""Tests for the ES document builder in scripts/index_bp_to_es.py.

We don't hit ES here — just verify that the doc shape matches what the index
mapping in scripts/es_setup_bp_index.py expects, and that text-for-embedding
carries the right signal.
"""
from __future__ import annotations

import json

import pytest

from parse_bp_facts import parse_bp  # type: ignore
from index_bp_to_es import build_es_doc, build_text_for_embedding  # type: ignore
from es_setup_bp_index import MAPPING  # type: ignore

from conftest import load_fixture


# Fields that our index mapping declares. build_es_doc must not add anything
# outside this set because the index is `dynamic: "strict"`.
ALLOWED_FIELDS = set(MAPPING["mappings"]["properties"].keys())


@pytest.fixture
def ga_facts():
    facts = parse_bp(load_fixture("ga_spawn_effect.bpl_json"), "ga_spawn_effect.bpl_json")
    assert facts is not None
    return facts


class TestBuildEsDoc:
    def test_doc_only_uses_mapped_fields(self, ga_facts):
        doc = build_es_doc(ga_facts, purpose="test purpose", embedding=[0.0] * 1536)
        extra = set(doc.keys()) - ALLOWED_FIELDS
        assert not extra, f"doc has unmapped fields: {extra}"

    def test_doc_has_required_identity_fields(self, ga_facts):
        doc = build_es_doc(ga_facts, "x", [0.0] * 1536)
        assert doc["entity"] == ga_facts["entity"]
        assert doc["asset_name"] == ga_facts["asset_name"]
        assert doc["kind"] == ga_facts["kind"]

    def test_embedding_dim_matches_mapping(self, ga_facts):
        expected_dim = MAPPING["mappings"]["properties"]["embedding"]["dims"]
        doc = build_es_doc(ga_facts, "x", [0.0] * expected_dim)
        assert len(doc["embedding"]) == expected_dim

    def test_variables_are_stringified_keyword_list(self, ga_facts):
        doc = build_es_doc(ga_facts, "x", [0.0] * 1536)
        # Mapping declares "variables" as keyword (filterable) — must be list of strings
        assert isinstance(doc["variables"], list)
        assert all(isinstance(v, str) for v in doc["variables"])
        assert all(":" in v for v in doc["variables"]), "variables should be 'Name:type' format"

    def test_purpose_included_when_present(self, ga_facts):
        doc = build_es_doc(ga_facts, "my purpose", [0.0] * 1536)
        assert doc["purpose"] == "my purpose"

    def test_purpose_empty_string_when_missing(self, ga_facts):
        doc = build_es_doc(ga_facts, "", [0.0] * 1536)
        assert doc["purpose"] == ""

    def test_exec_chain_flattened_with_delimiters(self, ga_facts):
        doc = build_es_doc(ga_facts, "x", [0.0] * 1536)
        # Multiple events must be joined by " | "
        assert " | " in doc["exec_chain"]
        assert "K2_ActivateAbility:" in doc["exec_chain"]
        assert "K2_OnEndAbility:" in doc["exec_chain"]

    def test_structural_hash_carried_through(self, ga_facts):
        doc = build_es_doc(ga_facts, "x", [0.0] * 1536)
        assert doc["structural_hash"] == ga_facts["structural_hash"]

    def test_indexed_at_is_iso_timestamp(self, ga_facts):
        doc = build_es_doc(ga_facts, "x", [0.0] * 1536)
        # Simple sanity: must start with 4-digit year
        assert isinstance(doc["indexed_at"], str)
        assert doc["indexed_at"][:4].isdigit()


class TestBuildTextForEmbedding:
    def test_starts_with_asset_name(self, ga_facts):
        text = build_text_for_embedding(ga_facts, "purpose here")
        assert text.startswith("GA_SpawnEffect"), \
            "asset name must be first for embedding signal weight"

    def test_includes_purpose(self, ga_facts):
        text = build_text_for_embedding(ga_facts, "my unique purpose tag 12345")
        assert "my unique purpose tag 12345" in text

    def test_skips_unknown_purpose(self, ga_facts):
        text = build_text_for_embedding(ga_facts, "unknown")
        assert "unknown" not in text.split(". ")[:2], "literal 'unknown' must be skipped"

    def test_includes_exec_chains(self, ga_facts):
        text = build_text_for_embedding(ga_facts, "")
        assert "K2_ActivateAbility" in text

    def test_includes_calls_csv(self, ga_facts):
        text = build_text_for_embedding(ga_facts, "")
        assert "calls: " in text.lower() or "Delay" in text


class TestMappingCompatibility:
    """Regression: if someone changes the ES mapping, make sure we notice."""

    def test_embedding_uses_cosine(self):
        emb = MAPPING["mappings"]["properties"]["embedding"]
        assert emb["similarity"] == "cosine"
        assert emb["index"] is True

    def test_dimension_is_1536(self):
        # Qodo-Embed-1-1.5B outputs 1536.
        assert MAPPING["mappings"]["properties"]["embedding"]["dims"] == 1536

    def test_strict_dynamic_mode(self):
        # Prevents silent schema drift — new fields must be explicitly added.
        assert MAPPING["mappings"]["dynamic"] == "strict"
