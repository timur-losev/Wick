"""Unit tests for scripts/parse_bp_facts.py.

These tests use real .bpl_json fixtures copied from the OlivaVanilla export directory,
but the expectations are hardcoded so the tests are self-contained and deterministic.
"""
from __future__ import annotations

import json
import re

import pytest

from parse_bp_facts import parse_bp, shorten_path, is_exec_out, describe_node  # type: ignore

from conftest import load_fixture


# ═══ Helper builders ═══════════════════════════════════════════════════════

def _parse(name: str) -> dict:
    facts = parse_bp(load_fixture(name), name)
    assert facts is not None, f"parse_bp returned None for {name}"
    return facts


# ═══ shorten_path ══════════════════════════════════════════════════════════

class TestShortenPath:
    def test_dotted_path(self):
        assert shorten_path("/Script/Engine.Actor") == "Actor"

    def test_slash_only(self):
        assert shorten_path("/Game/Blueprints/BP_Hero") == "BP_Hero"

    def test_nested_k2node(self):
        assert shorten_path("/Script/BlueprintGraph.K2Node_CallFunction") == "K2Node_CallFunction"

    def test_empty(self):
        assert shorten_path("") == ""

    def test_no_separator(self):
        assert shorten_path("Plain") == "Plain"


# ═══ is_exec_out ═══════════════════════════════════════════════════════════

class TestIsExecOut:
    @pytest.mark.parametrize("pin", ["then", "True", "False", "Completed",
                                     "OnFailed", "OnSuccess", "OnFinish",
                                     "then_0", "then_1", "Then X"])
    def test_accepts_exec_pins(self, pin):
        assert is_exec_out(pin) is True

    @pytest.mark.parametrize("pin", ["execute", "self", "ReturnValue",
                                     "CameraMode", "Condition", "",
                                     "OutputDelegate"])
    def test_rejects_non_exec(self, pin):
        assert is_exec_out(pin) is False


# ═══ parse_bp: GA_SpawnEffect (gameplay_ability with exec chains) ═════════

class TestParseGaSpawnEffect:
    @pytest.fixture(scope="class")
    def facts(self):
        return _parse("ga_spawn_effect.bpl_json")

    def test_entity_has_bp_prefix(self, facts):
        assert facts["entity"] == "bp:GA_SpawnEffect"

    def test_asset_name(self, facts):
        assert facts["asset_name"] == "GA_SpawnEffect"

    def test_kind_is_gameplay_ability(self, facts):
        assert facts["kind"] == "gameplay_ability"

    def test_parent_class_hint_derived_from_events(self, facts):
        # GA_SpawnEffect overrides K2_ActivateAbility whose member_parent is GameplayAbility
        assert facts["parent_class_hint"] == "GameplayAbility"

    def test_has_expected_events(self, facts):
        assert set(facts["events"]) == {"K2_ActivateAbility", "K2_OnEndAbility"}

    def test_has_expected_custom_events(self, facts):
        assert set(facts["custom_events"]) == {"DisableInput", "EnableInputAgain"}

    def test_calls_deduped_and_sorted(self, facts):
        calls = facts["calls"]
        # Dedup: same function called multiple times must appear once
        assert len(calls) == len(set(calls)), "calls contain duplicates"
        # Must be sorted
        assert calls == sorted(calls), "calls not sorted"
        # Sanity: at least these known calls from GA_SpawnEffect
        expected_subset = {"Delay", "K2_EndAbility", "DisableInput",
                           "BP_ApplyGameplayEffectToSelf", "GetPlayLength"}
        assert expected_subset.issubset(set(calls)), f"missing: {expected_subset - set(calls)}"

    def test_call_owners_shortened(self, facts):
        owners = facts["call_owners"]
        # "/Script/KismetSystemLibrary..." → "KismetSystemLibrary"
        assert "KismetSystemLibrary" in owners
        assert not any("/" in o for o in owners), "owners must be shortened (no /)"

    def test_variables_with_types(self, facts):
        names_types = {(v["name"], v["type"]) for v in facts["variables"]}
        expected = {
            ("SpawnMontage", "object"),
            ("SpawnInGE_Spec", "struct"),
            ("EnableInputAfterTimeFraction", "real"),
            ("CachedController", "object"),
        }
        assert names_types == expected

    def test_gets_and_sets_variables(self, facts):
        assert "CachedController" in facts["gets_variables"]
        assert "CachedController" in facts["sets_variables"]
        assert "SpawnInGE_Spec" in facts["sets_variables"]
        assert "SpawnMontage" in facts["gets_variables"]

    def test_uses_macro(self, facts):
        # macro_ref is an object {macro_blueprint, macro_graph_guid}, we extract the shortened path
        assert "StandardMacros" in facts["macros"]

    def test_exec_chain_for_activate_ability(self, facts):
        chain = facts["exec_chains"]["K2_ActivateAbility"]
        # It must not contain raw JSON (regression for variable_ref bug)
        assert "member_name" not in chain, "exec chain leaked variable_ref object"
        assert "member_guid" not in chain, "exec chain leaked variable_ref object"
        # Must contain recognizable node descriptions joined by arrows
        assert " → " in chain
        # Specific path we know from hand inspection
        assert "BP_ApplyGameplayEffectToSelf" in chain
        assert "Delay" in chain
        assert "K2_EndAbility" in chain

    def test_exec_chain_for_end_ability(self, facts):
        chain = facts["exec_chains"]["K2_OnEndAbility"]
        assert "BP_RemoveGameplayEffectFromOwnerWithHandle" in chain
        assert "EnableInputAgain" in chain

    def test_no_none_subentity_in_chains(self, facts):
        # Ensure we don't accidentally emit bp:GA_SpawnEffect.None
        assert "None" not in facts["exec_chains"]

    def test_structural_hash_present_and_stable(self, facts):
        h = facts["structural_hash"]
        assert isinstance(h, str)
        assert len(h) == 16
        assert re.fullmatch(r"[0-9a-f]{16}", h), f"hash not hex: {h}"

    def test_hash_excludes_source_file(self, facts):
        # Re-parse with a different source_file name; hash must match
        again = parse_bp(load_fixture("ga_spawn_effect.bpl_json"), "different_name.bpl_json")
        assert again["structural_hash"] == facts["structural_hash"]


# ═══ parse_bp: ABP_PistolAnimLayers (anim_blueprint) ══════════════════════

class TestParseAbpPistol:
    @pytest.fixture(scope="class")
    def facts(self):
        return _parse("abp_pistol.bpl_json")

    def test_kind_is_anim_blueprint(self, facts):
        assert facts["kind"] == "anim_blueprint"

    def test_parent_class_hint_is_anim_instance(self, facts):
        assert facts["parent_class_hint"] == "AnimInstance"

    def test_has_blueprint_update_animation_event(self, facts):
        assert "BlueprintUpdateAnimation" in facts["events"]


# ═══ parse_bp: B_LyraGameMode (actor_blueprint, minimal) ══════════════════

class TestParseBLyraGameMode:
    @pytest.fixture(scope="class")
    def facts(self):
        return _parse("b_lyra_gamemode.bpl_json")

    def test_kind_is_actor_blueprint(self, facts):
        # Derived from the B_ prefix
        assert facts["kind"] == "actor_blueprint"

    def test_parent_class_hint_is_actor(self, facts):
        # Events ReceiveBeginPlay / ReceiveTick live on Actor
        assert facts["parent_class_hint"] == "Actor"

    def test_has_both_graph_and_construction_script(self, facts):
        assert "EventGraph" in facts["graphs"]

    def test_no_custom_events(self, facts):
        assert facts["custom_events"] == []


# ═══ parse_bp: W_RespawnTimer (widget, rich BP) ═══════════════════════════

class TestParseWRespawnTimer:
    @pytest.fixture(scope="class")
    def facts(self):
        return _parse("w_respawn_timer.bpl_json")

    def test_kind_is_widget(self, facts):
        assert facts["kind"] == "widget"

    def test_has_calls(self, facts):
        # Must collect multiple deduped calls
        assert len(facts["calls"]) > 0

    def test_structural_hash_present(self, facts):
        assert re.fullmatch(r"[0-9a-f]{16}", facts["structural_hash"])


# ═══ parse_bp: error handling ═════════════════════════════════════════════

class TestParseBpErrors:
    def test_utf8_bom_is_stripped(self):
        body = b'{"asset_name": "X", "graphs": []}'
        with_bom = b"\xef\xbb\xbf" + body
        facts = parse_bp(with_bom, "bom.bpl_json")
        assert facts is not None
        assert facts["asset_name"] == "X"

    def test_invalid_json_returns_none(self):
        assert parse_bp(b"not json at all", "bad.bpl_json") is None

    def test_empty_asset_name_returns_none(self):
        assert parse_bp(b'{"asset_name": ""}', "empty.bpl_json") is None

    def test_non_object_root_returns_none(self):
        assert parse_bp(b"[]", "array.bpl_json") is None

    def test_missing_graphs_array_is_tolerated(self):
        facts = parse_bp(b'{"asset_name": "X"}', "noarr.bpl_json")
        assert facts is not None
        assert facts["graphs"] == []
        assert facts["events"] == []

    def test_kind_defaults_to_blueprint(self):
        # No name prefix, no gameplay-ability events
        body = json.dumps({
            "asset_name": "MyCustomAsset",
            "graphs": [{"name": "EventGraph", "nodes": [], "links": []}],
        }).encode()
        facts = parse_bp(body, "custom.bpl_json")
        assert facts is not None
        assert facts["kind"] == "blueprint"


# ═══ parse_bp: kind classification heuristics ═════════════════════════════

@pytest.mark.parametrize("asset_name,expected_kind", [
    ("GA_Heal",                 "gameplay_ability"),
    ("GE_StunBuff",             "gameplay_effect"),
    ("GC_Explosion",            "gameplay_cue"),
    ("GCNL_Sparks",             "gameplay_cue"),
    ("ABP_Character",           "anim_blueprint"),
    ("ALI_WeaponLayer",         "anim_blueprint"),
    ("AN_FootstepSound",        "anim_notify"),
    ("W_HealthBar",             "widget"),
    ("WBP_Inventory",           "widget"),
    ("BP_Door",                 "actor_blueprint"),
    ("B_Something",             "actor_blueprint"),
    ("UnclassifiedName",        "blueprint"),
])
def test_kind_classification_by_name_prefix(asset_name, expected_kind):
    body = json.dumps({"asset_name": asset_name, "graphs": []}).encode()
    facts = parse_bp(body, f"{asset_name}.bpl_json")
    assert facts is not None
    assert facts["kind"] == expected_kind, f"{asset_name}: expected {expected_kind}, got {facts['kind']}"
