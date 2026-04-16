// server/bpl_structural_enricher.hpp
//
// Deterministic extraction of structural facts from exported Blueprint JSON
// (.bpl_json). This is a C++ peer of scripts/parse_bp_facts.py — same output
// contract, so that both paths produce identical `structural_hash` values and
// the WAX C++ server and the Python indexing pipeline stay in sync.
//
// Usage:
//     const auto facts = BplStructuralEnricher::Enrich(raw_json);
//     if (facts.has_value()) {
//         // facts->entity, facts->calls, facts->exec_chains, etc.
//     }
//
// The enricher is stateless — `Enrich` is a pure function. Thread-safe.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace waxcpp::server {

// ── Output records ──────────────────────────────────────────────────────────

/// A single (Name:Type) variable declared by the Blueprint.
struct BplVariable {
    std::string name;
    std::string type;
};

/// Output of the structural enricher — a flat, JSON-serialisable record.
struct BplStructuralFacts {
    std::string entity;             // "bp:AssetName"
    std::string asset_name;
    std::string asset_path;
    std::string kind;               // "gameplay_ability", "widget", "actor_blueprint", ...
    std::string parent_class_hint;  // derived from event refs

    std::uint32_t node_count = 0;
    std::uint32_t link_count = 0;

    std::vector<std::string> graphs;
    std::vector<std::string> events;
    std::vector<std::string> event_owners;
    std::vector<std::string> custom_events;
    std::vector<std::string> calls;
    std::vector<std::string> call_owners;
    std::vector<std::string> gets_variables;
    std::vector<std::string> sets_variables;
    std::vector<BplVariable> variables;
    std::vector<std::string> casts_to;
    std::vector<std::string> macros;

    /// Event name → human-readable exec sequence (e.g.
    /// "K2_ActivateAbility": "Set A → Delay → K2_EndAbility").
    std::unordered_map<std::string, std::string> exec_chains;

    /// Stable 16-hex-char hash of the structural content (excludes source file
    /// name). Used for change detection in `wax_blueprint_refresh`.
    std::string structural_hash;
};

// ── Public entry points ─────────────────────────────────────────────────────

class BplStructuralEnricher {
public:
    /// Parse a .bpl_json payload (UTF-8, optional BOM) into structural facts.
    /// Returns nullopt on parse errors, empty asset_name, or non-object root.
    ///
    /// Thread-safe. Does not retain any pointers to `raw_json`.
    [[nodiscard]] static std::optional<BplStructuralFacts> Enrich(std::string_view raw_json);
};

// ── Helpers (exposed for testing) ───────────────────────────────────────────

/// "/Script/Engine.Actor"                      → "Actor"
/// "/Script/BlueprintGraph.K2Node_CallFunction" → "K2Node_CallFunction"
[[nodiscard]] std::string ShortenBplPath(std::string_view path);

/// True for exec-out pin names on K2 nodes: "then", "True", "False",
/// "Completed", "OnFailed", "OnSuccess", "OnFinish", "then_0", "then_1", …
[[nodiscard]] bool IsBplExecOutPin(std::string_view pin_name);

}  // namespace waxcpp::server
