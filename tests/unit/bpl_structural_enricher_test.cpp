// tests/unit/bpl_structural_enricher_test.cpp
#include "../../server/bpl_structural_enricher.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void TestAssert(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Container>
bool Contains(const Container& c, const std::string& value) {
    return std::find(c.begin(), c.end(), value) != c.end();
}

// Fixtures live next to the Python tests. We look them up relative to the
// source tree so the test is runnable from any build dir.
std::filesystem::path FixturePath(const std::string& name) {
    auto p = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
             / "tests" / "python" / "fixtures" / name;
    return p;
}

std::string ReadFixture(const std::string& name) {
    const auto p = FixturePath(name);
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open fixture: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ── Test: ShortenBplPath ───────────────────────────────────────────────────

void TestShortenBplPath() {
    std::cerr << "  TestShortenBplPath..." << std::endl;
    using waxcpp::server::ShortenBplPath;

    TestAssert(ShortenBplPath("/Script/Engine.Actor") == "Actor", "dotted path");
    TestAssert(ShortenBplPath("/Script/BlueprintGraph.K2Node_CallFunction") == "K2Node_CallFunction",
               "K2Node");
    TestAssert(ShortenBplPath("/Game/Blueprints/BP_Hero") == "BP_Hero", "slash only");
    TestAssert(ShortenBplPath("Plain") == "Plain", "no separator");
    TestAssert(ShortenBplPath("") == "", "empty");
    std::cerr << "    PASS" << std::endl;
}

// ── Test: IsBplExecOutPin ──────────────────────────────────────────────────

void TestIsBplExecOutPin() {
    std::cerr << "  TestIsBplExecOutPin..." << std::endl;
    using waxcpp::server::IsBplExecOutPin;

    for (std::string pin : {"then", "True", "False", "Completed",
                             "OnFailed", "OnSuccess", "OnFinish",
                             "then_0", "then_1", "Then X"}) {
        TestAssert(IsBplExecOutPin(pin), "should accept exec pin: " + pin);
    }
    for (std::string pin : {"execute", "self", "ReturnValue", "CameraMode",
                             "Condition", "OutputDelegate", ""}) {
        TestAssert(!IsBplExecOutPin(pin), "should reject non-exec pin: " + pin);
    }
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Enrich(GA_SpawnEffect) — full fact extraction ───────────────────

void TestEnrichGaSpawnEffect() {
    std::cerr << "  TestEnrichGaSpawnEffect..." << std::endl;
    const auto raw = ReadFixture("ga_spawn_effect.bpl_json");
    const auto facts_opt = waxcpp::server::BplStructuralEnricher::Enrich(raw);
    TestAssert(facts_opt.has_value(), "Enrich returned nullopt");
    const auto& f = *facts_opt;

    TestAssert(f.entity == "bp:GA_SpawnEffect", "entity");
    TestAssert(f.asset_name == "GA_SpawnEffect", "asset_name");
    TestAssert(f.kind == "gameplay_ability", "kind");
    // Real parent class lives on the event refs (member_parent is GameplayAbility).
    TestAssert(f.parent_class_hint == "GameplayAbility", "parent_class_hint");

    TestAssert(Contains(f.events, "K2_ActivateAbility"), "events: K2_ActivateAbility");
    TestAssert(Contains(f.events, "K2_OnEndAbility"),   "events: K2_OnEndAbility");

    TestAssert(Contains(f.custom_events, "DisableInput"),     "custom: DisableInput");
    TestAssert(Contains(f.custom_events, "EnableInputAgain"), "custom: EnableInputAgain");

    // Dedup check: same function called multiple times must appear once.
    for (const auto& name : f.calls) {
        const auto count = std::count(f.calls.begin(), f.calls.end(), name);
        TestAssert(count == 1, "calls contain duplicate: " + name);
    }
    // Must be sorted.
    {
        auto sorted = f.calls;
        std::sort(sorted.begin(), sorted.end());
        TestAssert(sorted == f.calls, "calls not sorted");
    }

    TestAssert(Contains(f.calls, "Delay"),                        "calls: Delay");
    TestAssert(Contains(f.calls, "K2_EndAbility"),                "calls: K2_EndAbility");
    TestAssert(Contains(f.calls, "BP_ApplyGameplayEffectToSelf"), "calls: BP_ApplyGameplayEffectToSelf");

    // call_owners must be shortened (no slashes).
    for (const auto& owner : f.call_owners) {
        TestAssert(owner.find('/') == std::string::npos,
                   "call_owner not shortened: " + owner);
    }
    TestAssert(Contains(f.call_owners, "KismetSystemLibrary"), "call_owners: KismetSystemLibrary");

    // Variables with types.
    bool found_spawn_montage = false, found_spawn_in_ge_spec = false;
    for (const auto& v : f.variables) {
        if (v.name == "SpawnMontage" && v.type == "object")  found_spawn_montage = true;
        if (v.name == "SpawnInGE_Spec" && v.type == "struct") found_spawn_in_ge_spec = true;
    }
    TestAssert(found_spawn_montage, "variable SpawnMontage:object");
    TestAssert(found_spawn_in_ge_spec, "variable SpawnInGE_Spec:struct");

    TestAssert(Contains(f.gets_variables, "CachedController"), "gets CachedController");
    TestAssert(Contains(f.sets_variables, "CachedController"), "sets CachedController");

    TestAssert(Contains(f.macros, "StandardMacros"), "macro: StandardMacros (regression for nested-object parsing)");

    // Exec chain regression: the K2_ActivateAbility chain must not contain raw JSON.
    auto act_it = f.exec_chains.find("K2_ActivateAbility");
    TestAssert(act_it != f.exec_chains.end(), "exec_chain K2_ActivateAbility missing");
    const auto& act_chain = act_it->second;
    TestAssert(act_chain.find("member_name") == std::string::npos,
               "exec chain leaked variable_ref object");
    TestAssert(act_chain.find("member_guid") == std::string::npos,
               "exec chain leaked variable_ref object");
    TestAssert(act_chain.find("BP_ApplyGameplayEffectToSelf") != std::string::npos,
               "exec chain missing BP_ApplyGameplayEffectToSelf");
    TestAssert(act_chain.find("Delay") != std::string::npos, "exec chain missing Delay");
    TestAssert(act_chain.find("K2_EndAbility") != std::string::npos, "exec chain missing K2_EndAbility");

    // Must not have a ".None" sub-entity from uninitialised CustomEvents.
    TestAssert(f.exec_chains.find("None") == f.exec_chains.end(),
               "None subentity leaked into exec_chains");

    // Hash format: 16 lowercase hex chars.
    TestAssert(f.structural_hash.size() == 16, "hash length != 16");
    for (char c : f.structural_hash) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        TestAssert(hex, "hash is not lowercase hex");
    }

    std::cerr << "    PASS" << std::endl;
}

// ── Test: Enrich(ABP_PistolAnimLayers) — minimal anim blueprint ───────────

void TestEnrichAbpPistol() {
    std::cerr << "  TestEnrichAbpPistol..." << std::endl;
    const auto raw = ReadFixture("abp_pistol.bpl_json");
    const auto facts = waxcpp::server::BplStructuralEnricher::Enrich(raw);
    TestAssert(facts.has_value(), "Enrich returned nullopt");
    TestAssert(facts->kind == "anim_blueprint", "kind");
    TestAssert(facts->parent_class_hint == "AnimInstance", "parent_class_hint");
    TestAssert(Contains(facts->events, "BlueprintUpdateAnimation"), "event");
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Enrich(B_LyraGameMode) — actor BP classification ────────────────

void TestEnrichBLyraGameMode() {
    std::cerr << "  TestEnrichBLyraGameMode..." << std::endl;
    const auto raw = ReadFixture("b_lyra_gamemode.bpl_json");
    const auto facts = waxcpp::server::BplStructuralEnricher::Enrich(raw);
    TestAssert(facts.has_value(), "Enrich returned nullopt");
    TestAssert(facts->kind == "actor_blueprint", "kind derived from B_ prefix");
    TestAssert(facts->parent_class_hint == "Actor", "parent_class_hint from events");
    TestAssert(Contains(facts->graphs, "EventGraph"), "graphs contains EventGraph");
    TestAssert(facts->custom_events.empty(), "custom_events expected empty");
    std::cerr << "    PASS" << std::endl;
}

// ── Test: hash stability across calls on the same input ───────────────────

void TestHashStability() {
    std::cerr << "  TestHashStability..." << std::endl;
    const auto raw = ReadFixture("ga_spawn_effect.bpl_json");
    const auto a = waxcpp::server::BplStructuralEnricher::Enrich(raw);
    const auto b = waxcpp::server::BplStructuralEnricher::Enrich(raw);
    TestAssert(a.has_value() && b.has_value(), "both returned value");
    TestAssert(a->structural_hash == b->structural_hash,
               "hash must be deterministic for identical inputs");
    std::cerr << "    PASS" << std::endl;
}

// ── Test: error paths ─────────────────────────────────────────────────────

void TestParseErrors() {
    std::cerr << "  TestParseErrors..." << std::endl;
    using waxcpp::server::BplStructuralEnricher;

    TestAssert(!BplStructuralEnricher::Enrich("not json at all").has_value(),
               "invalid JSON must return nullopt");
    TestAssert(!BplStructuralEnricher::Enrich("").has_value(),
               "empty input must return nullopt");
    TestAssert(!BplStructuralEnricher::Enrich("[]").has_value(),
               "non-object root must return nullopt");
    TestAssert(!BplStructuralEnricher::Enrich(R"({"asset_name": ""})").has_value(),
               "empty asset_name must return nullopt");

    // UTF-8 BOM must be tolerated.
    const std::string with_bom =
        std::string("\xEF\xBB\xBF") + R"({"asset_name": "X", "graphs": []})";
    const auto ok = BplStructuralEnricher::Enrich(with_bom);
    TestAssert(ok.has_value(), "BOM input must parse");
    TestAssert(ok->asset_name == "X", "asset_name after BOM strip");
    std::cerr << "    PASS" << std::endl;
}

}  // namespace

int main() {
    try {
        std::cerr << "bpl_structural_enricher_test:" << std::endl;
        TestShortenBplPath();
        TestIsBplExecOutPin();
        TestEnrichGaSpawnEffect();
        TestEnrichAbpPistol();
        TestEnrichBLyraGameMode();
        TestHashStability();
        TestParseErrors();
        std::cerr << "All bpl_structural_enricher tests passed." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
