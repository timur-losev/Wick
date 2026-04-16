// server/bpl_structural_enricher.cpp
#include "bpl_structural_enricher.hpp"

#include "../src/core/sha256.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace waxcpp::server {

namespace {

// ── Small helpers ────────────────────────────────────────────────────────────

constexpr std::size_t kMaxExecChainDepth = 16;
constexpr std::size_t kStructuralHashHexChars = 16;

/// Strip UTF-8 BOM (EF BB BF) if present.
std::string_view StripBom(std::string_view s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.remove_prefix(3);
    }
    return s;
}

/// Hex-encode bytes to lowercase.
std::string ToHex(std::span<const std::byte> bytes, std::size_t limit) {
    static constexpr char kHex[] = "0123456789abcdef";
    const std::size_t n = std::min(bytes.size(), limit);
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = std::to_integer<std::uint8_t>(bytes[i]);
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

/// Get an optional string field from a JSON object.
std::string OptString(const Poco::JSON::Object::Ptr& obj, const std::string& key) {
    if (obj.isNull() || !obj->has(key)) return {};
    return obj->optValue<std::string>(key, "");
}

// ── Intermediate: per-node parsed info ──────────────────────────────────────

struct NodeInfo {
    std::string guid;
    std::string class_short;           // e.g., "K2Node_CallFunction"
    std::string title;
    std::string graph_name;

    std::optional<std::string> fn_name;          // member_name
    std::optional<std::string> fn_owner_short;   // shortened member_parent
    std::optional<std::string> custom_event;
    std::optional<std::string> variable_name;
    std::optional<std::string> cast_target_short;
    std::optional<std::string> macro_ref;

    /// Exec-flow: each element is (exec_pin_name, target_guid).
    std::vector<std::pair<std::string, std::string>> exec_out;
};

NodeInfo ParseNode(const Poco::JSON::Object::Ptr& node, const std::string& graph_name) {
    NodeInfo n;
    n.guid        = OptString(node, "node_guid");
    n.class_short = ShortenBplPath(OptString(node, "class_path"));
    n.title       = OptString(node, "title");
    n.graph_name  = graph_name;

    // Function ref (CallFunction, LatentAbilityCall, CallParentFunction).
    if (node->has("function")) {
        const auto fn = node->getObject("function");
        if (!fn.isNull()) {
            if (auto mn = OptString(fn, "member_name"); !mn.empty()) n.fn_name = std::move(mn);
            if (auto mp = OptString(fn, "member_parent"); !mp.empty())
                n.fn_owner_short = ShortenBplPath(mp);
        }
    }
    // Event ref overrides function ref (Events carry event_ref).
    if (node->has("event_ref")) {
        const auto ev = node->getObject("event_ref");
        if (!ev.isNull()) {
            if (auto mn = OptString(ev, "member_name"); !mn.empty()) n.fn_name = std::move(mn);
            if (auto mp = OptString(ev, "member_parent"); !mp.empty())
                n.fn_owner_short = ShortenBplPath(mp);
        }
    }
    // CustomEvent.
    if (auto cfn = OptString(node, "custom_function_name"); !cfn.empty() && cfn != "None") {
        n.custom_event = std::move(cfn);
    }
    // Variable reference (nested object in UE5 export).
    if (node->has("variable_ref")) {
        const auto vr = node->getObject("variable_ref");
        if (!vr.isNull()) {
            if (auto mn = OptString(vr, "member_name"); !mn.empty()) n.variable_name = std::move(mn);
        }
    }
    if (auto vn = OptString(node, "var_name"); !vn.empty()) {
        n.variable_name = std::move(vn);
    }
    // Cast target.
    if (auto ct = OptString(node, "cast_target"); !ct.empty()) {
        n.cast_target_short = ShortenBplPath(ct);
    }
    // Macro reference (nested object).
    if (node->has("macro_ref")) {
        const auto mr = node->getObject("macro_ref");
        if (!mr.isNull()) {
            if (auto mb = OptString(mr, "macro_blueprint"); !mb.empty())
                n.macro_ref = ShortenBplPath(mb);
        }
    }
    return n;
}

std::string DescribeNode(const NodeInfo& n) {
    if (n.class_short == "K2Node_CallFunction" ||
        n.class_short == "K2Node_CallParentFunction" ||
        n.class_short == "K2Node_LatentAbilityCall") {
        return n.fn_name.value_or(n.title);
    }
    if (n.class_short == "K2Node_VariableSet") {
        return "Set " + n.variable_name.value_or("?");
    }
    if (n.class_short == "K2Node_VariableGet") {
        return "Get " + n.variable_name.value_or("?");
    }
    if (n.class_short == "K2Node_DynamicCast") {
        return "Cast<" + n.cast_target_short.value_or("?") + ">";
    }
    if (n.class_short == "K2Node_IfThenElse")         return "Branch";
    if (n.class_short == "K2Node_ExecutionSequence")  return "Sequence";
    if (n.class_short == "K2Node_MacroInstance") {
        return "Macro:" + n.macro_ref.value_or("?");
    }
    return n.class_short;
}

std::string WalkExecChain(const std::unordered_map<std::string, NodeInfo>& nodes,
                          const std::string& start_guid,
                          std::size_t max_depth) {
    std::vector<std::string> path;
    std::unordered_set<std::string> visited;
    std::string cur = start_guid;
    for (std::size_t d = 0; d < max_depth; ++d) {
        if (visited.contains(cur)) break;
        visited.insert(cur);
        const auto it = nodes.find(cur);
        if (it == nodes.end()) break;
        const auto& n = it->second;
        if (n.class_short != "K2Node_Event" && n.class_short != "K2Node_CustomEvent") {
            path.push_back(DescribeNode(n));
        }
        if (n.exec_out.empty()) break;
        cur = n.exec_out.front().second;
    }
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i) out += " \xE2\x86\x92 ";  // " → " as UTF-8
        out += path[i];
    }
    return out;
}

// ── Kind classification ─────────────────────────────────────────────────────

/// Classify BP kind from event set + asset-name prefix. Same rules as Python.
std::string DeriveKind(const std::string& asset_name,
                       const std::unordered_set<std::string>& events) {
    if (events.count("K2_ActivateAbility") || events.count("K2_OnEndAbility") ||
        asset_name.starts_with("GA_")) return "gameplay_ability";
    if (asset_name.starts_with("GE_"))                                    return "gameplay_effect";
    if (asset_name.starts_with("GC_") || asset_name.starts_with("GCNL_")) return "gameplay_cue";
    if (asset_name.starts_with("ABP_") || asset_name.starts_with("ALI_")) return "anim_blueprint";
    if (asset_name.starts_with("AN_"))                                    return "anim_notify";
    if (asset_name.starts_with("W_") || asset_name.starts_with("WBP_"))   return "widget";
    if (asset_name.starts_with("BP_") || asset_name.starts_with("B_"))    return "actor_blueprint";
    return "blueprint";
}

// ── Canonical hashing ───────────────────────────────────────────────────────

/// Build a canonical, sortable text representation of the structural facts and
/// take SHA-256 of it. Returns the first `kStructuralHashHexChars` hex chars.
///
/// NOTE: this hash is NOT wire-compatible with `parse_bp_facts.py`'s hash
/// (Python uses `json.dumps(sort_keys=True)` over a dict). Both backends are
/// self-consistent independently — change detection via `wax_blueprint_refresh`
/// works because the refreshing backend (currently Python) always compares its
/// own hash with the one stored in ES.
std::string ComputeHash(const BplStructuralFacts& f) {
    std::ostringstream s;
    s << "entity=" << f.entity << '\n'
      << "asset_name=" << f.asset_name << '\n'
      << "asset_path=" << f.asset_path << '\n'
      << "kind=" << f.kind << '\n'
      << "parent_class_hint=" << f.parent_class_hint << '\n'
      << "node_count=" << f.node_count << '\n'
      << "link_count=" << f.link_count << '\n';

    auto emit_list = [&](std::string_view name, const std::vector<std::string>& xs) {
        s << name << '=';
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (i) s << ',';
            s << xs[i];
        }
        s << '\n';
    };
    emit_list("graphs",          f.graphs);
    emit_list("events",          f.events);
    emit_list("event_owners",    f.event_owners);
    emit_list("custom_events",   f.custom_events);
    emit_list("calls",           f.calls);
    emit_list("call_owners",     f.call_owners);
    emit_list("gets_variables",  f.gets_variables);
    emit_list("sets_variables",  f.sets_variables);
    emit_list("casts_to",        f.casts_to);
    emit_list("macros",          f.macros);

    s << "variables=";
    for (std::size_t i = 0; i < f.variables.size(); ++i) {
        if (i) s << ',';
        s << f.variables[i].name << ':' << f.variables[i].type;
    }
    s << '\n';

    // Exec chains: sort by event name for determinism.
    std::vector<std::pair<std::string, std::string>> ordered(
        f.exec_chains.begin(), f.exec_chains.end());
    std::sort(ordered.begin(), ordered.end());
    for (const auto& [event, chain] : ordered) {
        s << "exec." << event << '=' << chain << '\n';
    }

    const auto text = s.str();
    const std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
    const auto digest = waxcpp::core::Sha256Digest(bytes);
    return ToHex(std::span<const std::byte>(digest), kStructuralHashHexChars / 2);
}

// ── Sorted-vector conversion helper ─────────────────────────────────────────

std::vector<std::string> ToSortedVector(const std::unordered_set<std::string>& s) {
    std::vector<std::string> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

// ── Public helpers ──────────────────────────────────────────────────────────

std::string ShortenBplPath(std::string_view path) {
    if (path.empty()) return {};
    if (const auto dot = path.rfind('.');
        dot != std::string_view::npos && dot + 1 < path.size()) {
        return std::string(path.substr(dot + 1));
    }
    if (const auto slash = path.rfind('/');
        slash != std::string_view::npos && slash + 1 < path.size()) {
        return std::string(path.substr(slash + 1));
    }
    return std::string(path);
}

bool IsBplExecOutPin(std::string_view pin_name) {
    if (pin_name.empty()) return false;
    if (pin_name == "then" || pin_name == "True" || pin_name == "False" ||
        pin_name == "Completed" || pin_name == "OnFailed" ||
        pin_name == "OnSuccess" || pin_name == "OnFinish") return true;
    if (pin_name.starts_with("then_")) return true;
    if (pin_name.starts_with("Then "))  return true;
    return false;
}

// ── Main enricher ───────────────────────────────────────────────────────────

std::optional<BplStructuralFacts> BplStructuralEnricher::Enrich(std::string_view raw_json) {
    const auto stripped = StripBom(raw_json);
    if (stripped.empty()) return std::nullopt;

    Poco::JSON::Object::Ptr root;
    try {
        Poco::JSON::Parser parser;
        const auto parsed = parser.parse(std::string(stripped));
        root = parsed.extract<Poco::JSON::Object::Ptr>();
    } catch (const Poco::Exception&) {
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (root.isNull()) return std::nullopt;

    const auto asset_name = OptString(root, "asset_name");
    if (asset_name.empty()) return std::nullopt;

    BplStructuralFacts facts;
    facts.asset_name = asset_name;
    facts.entity     = "bp:" + asset_name;
    facts.asset_path = OptString(root, "asset_path");

    // Pass 1: build node map across all graphs.
    std::unordered_map<std::string, NodeInfo> nodes_by_guid;
    std::size_t total_nodes = 0;
    std::size_t total_links = 0;

    if (root->has("graphs")) {
        const auto graphs = root->getArray("graphs");
        if (!graphs.isNull()) {
            for (std::size_t gi = 0; gi < graphs->size(); ++gi) {
                const auto g = graphs->getObject(static_cast<unsigned int>(gi));
                if (g.isNull()) continue;

                const auto gname = OptString(g, "name");
                facts.graphs.push_back(gname);

                if (g->has("nodes")) {
                    const auto nodes = g->getArray("nodes");
                    if (!nodes.isNull()) {
                        total_nodes += nodes->size();
                        for (std::size_t ni = 0; ni < nodes->size(); ++ni) {
                            const auto node = nodes->getObject(static_cast<unsigned int>(ni));
                            if (node.isNull()) continue;
                            auto info = ParseNode(node, gname);
                            if (!info.guid.empty()) {
                                nodes_by_guid.emplace(info.guid, std::move(info));
                            }
                        }
                    }
                }

                if (g->has("links")) {
                    const auto links = g->getArray("links");
                    if (!links.isNull()) {
                        total_links += links->size();
                        for (std::size_t li = 0; li < links->size(); ++li) {
                            const auto link = links->getObject(static_cast<unsigned int>(li));
                            if (link.isNull()) continue;
                            const auto from_guid = OptString(link, "from_node_guid");
                            const auto to_guid   = OptString(link, "to_node_guid");
                            const auto from_pin  = OptString(link, "from_pin_name");
                            if (from_guid.empty() || to_guid.empty()) continue;
                            const auto it = nodes_by_guid.find(from_guid);
                            if (it == nodes_by_guid.end()) continue;
                            if (IsBplExecOutPin(from_pin)) {
                                it->second.exec_out.emplace_back(from_pin, to_guid);
                            }
                        }
                    }
                }
            }
        }
    }

    facts.node_count = static_cast<std::uint32_t>(total_nodes);
    facts.link_count = static_cast<std::uint32_t>(total_links);

    // Pass 2: aggregate across nodes into sorted, deduplicated fact lists.
    std::unordered_set<std::string> events, event_owners, custom_events,
        calls, call_owners, gets, sets, casts, macros;

    for (const auto& [_, n] : nodes_by_guid) {
        if (n.class_short == "K2Node_Event") {
            if (n.fn_name)        events.insert(*n.fn_name);
            if (n.fn_owner_short) event_owners.insert(*n.fn_owner_short);
        } else if (n.class_short == "K2Node_CustomEvent") {
            if (n.custom_event) custom_events.insert(*n.custom_event);
        } else if (n.class_short == "K2Node_CallFunction" ||
                   n.class_short == "K2Node_CallParentFunction" ||
                   n.class_short == "K2Node_LatentAbilityCall") {
            if (n.fn_name)        calls.insert(*n.fn_name);
            if (n.fn_owner_short) call_owners.insert(*n.fn_owner_short);
        } else if (n.class_short == "K2Node_VariableGet") {
            if (n.variable_name) gets.insert(*n.variable_name);
        } else if (n.class_short == "K2Node_VariableSet") {
            if (n.variable_name) sets.insert(*n.variable_name);
        } else if (n.class_short == "K2Node_DynamicCast") {
            if (n.cast_target_short) casts.insert(*n.cast_target_short);
        } else if (n.class_short == "K2Node_MacroInstance") {
            if (n.macro_ref) macros.insert(*n.macro_ref);
        }
    }

    facts.events         = ToSortedVector(events);
    facts.event_owners   = ToSortedVector(event_owners);
    facts.custom_events  = ToSortedVector(custom_events);
    facts.calls          = ToSortedVector(calls);
    facts.call_owners    = ToSortedVector(call_owners);
    facts.gets_variables = ToSortedVector(gets);
    facts.sets_variables = ToSortedVector(sets);
    facts.casts_to       = ToSortedVector(casts);
    facts.macros         = ToSortedVector(macros);

    // Top-level variables list.
    if (root->has("variables")) {
        const auto arr = root->getArray("variables");
        if (!arr.isNull()) {
            for (std::size_t i = 0; i < arr->size(); ++i) {
                const auto v = arr->getObject(static_cast<unsigned int>(i));
                if (v.isNull()) continue;
                const auto name = OptString(v, "var_name");
                if (name.empty()) continue;
                std::string type = "unknown";
                if (v->has("var_type")) {
                    const auto vt = v->getObject("var_type");
                    if (!vt.isNull()) type = OptString(vt, "category");
                    if (type.empty()) type = "unknown";
                }
                facts.variables.push_back({name, std::move(type)});
            }
        }
    }

    // Kind derivation.
    facts.kind = DeriveKind(asset_name, events);

    // parent_class_hint: first non-generic event owner alphabetically.
    for (const auto& owner : facts.event_owners) {
        if (!owner.empty() && owner != "Object") {
            facts.parent_class_hint = owner;
            break;
        }
    }

    // Exec chains per Event / CustomEvent.
    for (const auto& [guid, n] : nodes_by_guid) {
        if (n.class_short != "K2Node_Event" && n.class_short != "K2Node_CustomEvent") continue;
        std::string start_name;
        if (n.fn_name)           start_name = *n.fn_name;
        else if (n.custom_event) start_name = *n.custom_event;
        if (start_name.empty() || start_name == "None") continue;
        auto chain = WalkExecChain(nodes_by_guid, guid, kMaxExecChainDepth);
        if (!chain.empty()) {
            facts.exec_chains.emplace(start_name, std::move(chain));
        }
    }

    facts.structural_hash = ComputeHash(facts);
    return facts;
}

}  // namespace waxcpp::server
