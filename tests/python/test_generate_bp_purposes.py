"""Unit tests for scripts/generate_bp_purposes.py.

Tests the pure helpers (prompt builder, sanitizer, folder hint extractor).
The actual LLM call is not exercised here — that lives in the live
integration path via run_full_reindex.ps1.
"""
from __future__ import annotations

import pytest

from generate_bp_purposes import (  # type: ignore
    _folder_hint,
    _strip_generic_opener,
    build_user_prompt,
    sanitize_purpose,
)


# ── _folder_hint ────────────────────────────────────────────────────────────

class TestFolderHint:
    def test_extracts_last_two_segments(self):
        p = "/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect"
        assert _folder_hint(p) == "Game/Respawn"

    def test_shallow_path(self):
        assert _folder_hint("/Game/B_LyraGameMode.B_LyraGameMode") == "Game"

    def test_very_deep_path(self):
        p = "/Game/Characters/Heroes/Mannequin/Animations/ABP_Foo.ABP_Foo"
        # Last 2-3 segments dropping the leaf
        hint = _folder_hint(p)
        assert "Animations" in hint
        assert "ABP_Foo" not in hint  # leaf must be stripped

    def test_empty_or_malformed(self):
        assert _folder_hint("") == ""
        assert _folder_hint("noslashes") == ""
        assert _folder_hint("/") == ""


# ── _strip_generic_opener ───────────────────────────────────────────────────

class TestStripGenericOpener:
    @pytest.mark.parametrize("raw,expected_prefix", [
        ("BP_Grenade is an actor blueprint that explodes on impact",
         "Explodes on impact"),
        ("GA_Heal is a gameplay ability that restores health",
         "Restores health"),
        ("W_HealthBar is a widget that shows the player's current health",
         "Shows the player's current health"),
        ("This blueprint manages a door that opens automatically",
         "Opens automatically"),  # nested "blueprint that" strip
        ("GE_Damage is a gameplay effect that deals periodic damage",
         "Deals periodic damage"),
        ("A blueprint for a respawn timer counting down the seconds",
         "Respawn timer counting down the seconds"),
    ])
    def test_strips_common_generic_openers(self, raw, expected_prefix):
        result = _strip_generic_opener(raw)
        assert result.startswith(expected_prefix), f"got: {result!r}"

    def test_leaves_good_outputs_untouched(self):
        s = "Applies a spawn effect and disables input for a short delay"
        assert _strip_generic_opener(s) == s

    def test_handles_verb_only_outputs(self):
        # Real good outputs should be unchanged
        for good in [
            "Spawns a tagged actor when the ability is activated",
            "Displays the current score with live updates",
            "Drives pistol animation state from the owning pawn",
            "Maps movement and jump inputs to player controls",
        ]:
            assert _strip_generic_opener(good) == good


# ── sanitize_purpose ────────────────────────────────────────────────────────

class TestSanitizePurpose:
    def test_strips_surrounding_quotes(self):
        assert sanitize_purpose('"Spawns an effect"') == "Spawns an effect"
        assert sanitize_purpose("'Handles input'") == "Handles input"

    def test_strips_markdown_fences(self):
        assert sanitize_purpose("```Displays stats```") == "Displays stats"

    def test_strips_trailing_period(self):
        assert sanitize_purpose("Opens a door.") == "Opens a door"

    def test_collapses_whitespace(self):
        assert sanitize_purpose("  Applies    damage \n over time  ") == "Applies damage over time"

    def test_strips_generic_opener_via_sanitizer(self):
        s = sanitize_purpose("BP_Door is an actor blueprint that opens and closes on overlap.")
        assert s.startswith("Opens and closes")
        assert "actor blueprint" not in s.lower()

    def test_handles_empty(self):
        assert sanitize_purpose("") == ""
        assert sanitize_purpose("   ") == ""


# ── build_user_prompt ───────────────────────────────────────────────────────

class TestBuildUserPrompt:
    @pytest.fixture
    def ga_facts(self):
        return {
            "asset_name": "GA_SpawnEffect",
            "asset_path": "/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect",
            "kind": "gameplay_ability",
            "parent_class_hint": "GameplayAbility",
            "events": ["K2_ActivateAbility", "K2_OnEndAbility"],
            "custom_events": ["EnableInputAgain"],
            "variables": [
                {"name": "SpawnMontage", "type": "object"},
                {"name": "CachedController", "type": "object"},
            ],
            "calls": ["Delay", "BP_ApplyGameplayEffectToSelf", "K2_EndAbility"],
            "casts_to": [],
            "exec_chains": {
                "K2_ActivateAbility": "Set CachedController → Delay → K2_EndAbility",
            },
        }

    def test_starts_with_name(self, ga_facts):
        prompt = build_user_prompt(ga_facts)
        assert prompt.split("\n")[0] == "Name: GA_SpawnEffect"

    def test_includes_folder_hint(self, ga_facts):
        prompt = build_user_prompt(ga_facts)
        assert "folder: Game/Respawn" in prompt

    def test_includes_parent_context(self, ga_facts):
        prompt = build_user_prompt(ga_facts)
        assert "parent_context: GameplayAbility" in prompt

    def test_includes_events(self, ga_facts):
        prompt = build_user_prompt(ga_facts)
        assert "events: K2_ActivateAbility, K2_OnEndAbility" in prompt

    def test_ends_with_verb_instruction(self, ga_facts):
        # Nudges the model to start with a verb.
        prompt = build_user_prompt(ga_facts)
        assert "starting with a verb" in prompt.lower()

    def test_caps_variables_at_eight(self):
        facts = {
            "asset_name": "BP_ManyVars",
            "asset_path": "/Game/BP_ManyVars.BP_ManyVars",
            "kind": "actor_blueprint",
            "variables": [{"name": f"v{i}", "type": "int"} for i in range(20)],
        }
        prompt = build_user_prompt(facts)
        assert "v0(int)" in prompt
        assert "v7(int)" in prompt
        assert "v8(int)" not in prompt, "variables should be capped at 8 entries"

    def test_tolerates_missing_optional_fields(self):
        facts = {
            "asset_name": "BP_Minimal",
            "kind": "blueprint",
        }
        prompt = build_user_prompt(facts)
        assert "Name: BP_Minimal" in prompt
        assert "Describe" in prompt
