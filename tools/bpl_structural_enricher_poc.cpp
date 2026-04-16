// bpl_structural_enricher_poc — proof-of-concept for deterministic fact extraction
// from exported Blueprint JSON (.bpl_json). Parses the entire file structurally
// (without LLM) and emits (entity, attribute, value) facts suitable for WAX's EAV store.
//
// Usage:  waxcpp_bpl_structural_enricher_poc <path.bpl_json>
//
// Design:
//   * No chunking — operates on the whole JSON.
//   * Walks: asset → graphs → nodes → links.
//   * Builds node_guid → NodeInfo map, then classifies each node by class_path.
//   * For exec-chain: from Event nodes, follows outgoing links by "then" pin
//     until a sink (return / dead end). Records the sequence of called functions.
//
// This is a PoC only — final integration will be a library (bpl_structural_enricher.cpp)
// invoked by the index pipeline for every .bpl_json file.

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ── Small helpers ───────────────────────────────────────────────────────────

std::string ShortenPath(std::string_view path) {
    // "/Script/Engine.Actor" → "Actor"
    // "/Script/BlueprintGraph.K2Node_CallFunction" → "K2Node_CallFunction"
    if (const auto dot = path.rfind('.'); dot != std::string_view::npos && dot + 1 < path.size()) {
        return std::string(path.substr(dot + 1));
    }
    if (const auto slash = path.rfind('/'); slash != std::string_view::npos && slash + 1 < path.size()) {
        return std::string(path.substr(slash + 1));
    }
    return std::string(path);
}

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    auto s = ss.str();
    // Strip UTF-8 BOM if present
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

// ── Fact record ─────────────────────────────────────────────────────────────

struct Fact {
    std::string entity;
    std::string attribute;
    std::string value;
};

// ── Node info built from raw JSON ───────────────────────────────────────────

struct NodeInfo {
    std::string guid;
    std::string class_short;           // e.g., "K2Node_CallFunction"
    std::string title;
    std::string graph_name;
    // CallFunction / Event / CustomEvent
    std::optional<std::string> fn_name;          // member_name
    std::optional<std::string> fn_owner_short;   // shortened member_parent
    std::optional<std::string> custom_event;     // custom_function_name (if not None/empty)
    // VariableGet / Set
    std::optional<std::string> variable_name;
    // DynamicCast
    std::optional<std::string> cast_target_short;
    // MacroInstance
    std::optional<std::string> macro_ref;
    // Pin flow: exec-out pin → target node(s)
    std::vector<std::pair<std::string, std::string>> exec_out; // (pin_name, target_guid)
};

// ── Structural enricher ─────────────────────────────────────────────────────

class BplStructuralEnricher {
public:
    std::vector<Fact> Enrich(const std::string& raw_json) {
        Poco::JSON::Parser parser;
        const auto parsed = parser.parse(raw_json);
        const auto root = parsed.extract<Poco::JSON::Object::Ptr>();
        if (root.isNull()) throw std::runtime_error("JSON root is not an object");

        const auto asset_name = root->optValue<std::string>("asset_name", "");
        const auto asset_kind_raw = root->optValue<std::string>("asset_kind", "");
        const auto asset_class = root->optValue<std::string>("asset_class", "");
        const auto blueprint_path = root->optValue<std::string>("blueprint", "");

        if (asset_name.empty()) throw std::runtime_error("asset_name is empty");

        const std::string entity = "bp:" + asset_name;
        std::vector<Fact> out;

        // ── Asset-level facts ───────────────────────────────────────────────
        out.push_back({entity, "asset_kind_raw", asset_kind_raw});
        if (!blueprint_path.empty()) {
            out.push_back({entity, "asset_path", blueprint_path});
        }
        // Note: asset_class in UE5 exports is always /Script/Engine.Blueprint for blueprints.
        // Real parent class is derived later from Event nodes (K2_ActivateAbility
        // lives on GameplayAbility, ReceiveBeginPlay lives on Actor, etc.).

        // ── Parse all graphs → collect node maps ────────────────────────────
        std::unordered_map<std::string, NodeInfo> nodes_by_guid;
        std::vector<std::string> graph_names;
        std::size_t total_nodes = 0, total_links = 0;

        const auto graphs = root->getArray("graphs");
        if (!graphs.isNull()) {
            for (std::size_t gi = 0; gi < graphs->size(); ++gi) {
                const auto g = graphs->getObject((unsigned)gi);
                if (g.isNull()) continue;
                const auto gname = g->optValue<std::string>("name", "");
                graph_names.push_back(gname);

                const auto nodes = g->getArray("nodes");
                const auto links = g->getArray("links");

                if (!nodes.isNull()) {
                    total_nodes += nodes->size();
                    for (std::size_t ni = 0; ni < nodes->size(); ++ni) {
                        auto node = nodes->getObject((unsigned)ni);
                        if (node.isNull()) continue;
                        auto info = ParseNode(node, gname);
                        if (!info.guid.empty()) {
                            nodes_by_guid.emplace(info.guid, std::move(info));
                        }
                    }
                }

                if (!links.isNull()) {
                    total_links += links->size();
                    // Populate exec_out on source nodes where from_pin_name is an exec pin
                    for (std::size_t li = 0; li < links->size(); ++li) {
                        auto link = links->getObject((unsigned)li);
                        if (link.isNull()) continue;
                        const auto from_guid = link->optValue<std::string>("from_node_guid", "");
                        const auto from_pin = link->optValue<std::string>("from_pin_name", "");
                        const auto to_guid = link->optValue<std::string>("to_node_guid", "");
                        if (from_guid.empty() || to_guid.empty()) continue;
                        auto it = nodes_by_guid.find(from_guid);
                        if (it == nodes_by_guid.end()) continue;
                        // Record only exec pins (then/then_0/then_1/True/False/Completed/...).
                        // Heuristic: pins named "then*", "True", "False", "Completed", or matching exec outs.
                        if (IsExecOutPin(from_pin)) {
                            it->second.exec_out.emplace_back(from_pin, to_guid);
                        }
                    }
                }
            }
        }

        out.push_back({entity, "node_count", std::to_string(total_nodes)});
        out.push_back({entity, "link_count", std::to_string(total_links)});
        for (const auto& gname : graph_names) {
            out.push_back({entity, "has_graph", gname});
        }

        // ── Emit per-node facts ──────────────────────────────────────────────
        // Use sets to dedup call/variable/etc.
        std::unordered_set<std::string> seen_calls, seen_call_owners, seen_events,
            seen_custom_events, seen_vars_get, seen_vars_set, seen_casts, seen_macros,
            seen_event_owners;

        for (const auto& [guid, n] : nodes_by_guid) {
            if (n.class_short == "K2Node_Event") {
                if (n.fn_name && seen_events.insert(*n.fn_name).second) {
                    out.push_back({entity, "has_event", *n.fn_name});
                }
                // Real parent_class hint: Event nodes carry the defining class in member_parent.
                if (n.fn_owner_short && seen_event_owners.insert(*n.fn_owner_short).second) {
                    out.push_back({entity, "inherits_event_from", *n.fn_owner_short});
                }
            } else if (n.class_short == "K2Node_CustomEvent") {
                if (n.custom_event && seen_custom_events.insert(*n.custom_event).second) {
                    out.push_back({entity, "has_custom_event", *n.custom_event});
                }
            } else if (n.class_short == "K2Node_CallFunction" ||
                       n.class_short == "K2Node_CallParentFunction" ||
                       n.class_short == "K2Node_LatentAbilityCall") {
                if (n.fn_name && seen_calls.insert(*n.fn_name).second) {
                    out.push_back({entity, "calls", *n.fn_name});
                }
                if (n.fn_owner_short && seen_call_owners.insert(*n.fn_owner_short).second) {
                    out.push_back({entity, "calls_owner", *n.fn_owner_short});
                }
            } else if (n.class_short == "K2Node_VariableGet") {
                if (n.variable_name && seen_vars_get.insert(*n.variable_name).second) {
                    out.push_back({entity, "gets_variable", *n.variable_name});
                }
            } else if (n.class_short == "K2Node_VariableSet") {
                if (n.variable_name && seen_vars_set.insert(*n.variable_name).second) {
                    out.push_back({entity, "sets_variable", *n.variable_name});
                }
            } else if (n.class_short == "K2Node_DynamicCast") {
                if (n.cast_target_short && seen_casts.insert(*n.cast_target_short).second) {
                    out.push_back({entity, "casts_to", *n.cast_target_short});
                    out.push_back({entity, "depends_on", *n.cast_target_short});
                }
            } else if (n.class_short == "K2Node_MacroInstance") {
                if (n.macro_ref && seen_macros.insert(*n.macro_ref).second) {
                    out.push_back({entity, "uses_macro", *n.macro_ref});
                }
            }
        }

        // ── Variables (top-level list) ──────────────────────────────────────
        if (const auto vars = root->getArray("variables"); !vars.isNull()) {
            for (std::size_t vi = 0; vi < vars->size(); ++vi) {
                const auto v = vars->getObject((unsigned)vi);
                if (v.isNull()) continue;
                const auto vname = v->optValue<std::string>("var_name", "");
                if (vname.empty()) continue;
                std::string vtype = "unknown";
                if (const auto vt = v->getObject("var_type"); !vt.isNull()) {
                    vtype = vt->optValue<std::string>("category", "unknown");
                }
                out.push_back({entity, "has_variable", vname + ":" + vtype});
            }
        }

        // ── Derive asset_kind from parent_class ─────────────────────────────
        const auto parent_short = ShortenPath(asset_class);
        std::string derived_kind = "blueprint";
        // Parent class in .bpl_json is generic Blueprint class; the real parent needs
        // to come from asset properties or from BP asset registry. Here we use naming
        // heuristics on the *asset_name* and the Event set as a fallback.
        // This is a known limitation — proper parent resolution belongs in the import plugin.
        if (seen_events.count("K2_ActivateAbility") || seen_events.count("K2_OnEndAbility") ||
            asset_name.rfind("GA_", 0) == 0) {
            derived_kind = "gameplay_ability";
        } else if (asset_name.rfind("GE_", 0) == 0) {
            derived_kind = "gameplay_effect";
        } else if (asset_name.rfind("GC_", 0) == 0 || asset_name.rfind("GCNL_", 0) == 0) {
            derived_kind = "gameplay_cue";
        } else if (asset_name.rfind("ABP_", 0) == 0 || asset_name.rfind("ALI_", 0) == 0) {
            derived_kind = "anim_blueprint";
        } else if (asset_name.rfind("AN_", 0) == 0) {
            derived_kind = "anim_notify";
        } else if (asset_name.rfind("W_", 0) == 0 || asset_name.rfind("WBP_", 0) == 0) {
            derived_kind = "widget";
        } else if (asset_name.rfind("BP_", 0) == 0 || asset_name.rfind("B_", 0) == 0) {
            derived_kind = "actor_blueprint";
        }
        out.push_back({entity, "kind", derived_kind});

        // ── Exec-chains from each Event ─────────────────────────────────────
        // For each Event/CustomEvent, walk outgoing exec pins up to a depth limit,
        // collecting the call sequence.
        for (const auto& [guid, n] : nodes_by_guid) {
            if (n.class_short != "K2Node_Event" && n.class_short != "K2Node_CustomEvent") continue;
            std::string start_name = n.fn_name ? *n.fn_name :
                                     (n.custom_event ? *n.custom_event : std::string{});
            if (start_name.empty() || start_name == "None") continue;
            const auto chain = WalkExecChain(nodes_by_guid, guid, /*max_depth=*/16);
            if (!chain.empty()) {
                const std::string subentity = entity + "." + start_name;
                out.push_back({subentity, "exec_chain", chain});
            }
        }

        return out;
    }

private:
    static bool IsExecOutPin(std::string_view name) {
        if (name.empty()) return false;
        if (name == "then" || name == "True" || name == "False" || name == "Completed") return true;
        // Sequence nodes: "then_0", "then_1", ...
        if (name.starts_with("then_") || name.starts_with("Then ")) return true;
        // Latent nodes often emit "Completed"/"OnFailed" etc.
        if (name == "OnFailed" || name == "OnSuccess" || name == "OnFinish") return true;
        return false;
    }

    static NodeInfo ParseNode(const Poco::JSON::Object::Ptr& node, const std::string& graph_name) {
        NodeInfo n;
        n.guid = node->optValue<std::string>("node_guid", "");
        n.class_short = ShortenPath(node->optValue<std::string>("class_path", ""));
        n.title = node->optValue<std::string>("title", "");
        n.graph_name = graph_name;

        // function reference (CallFunction, Event, LatentAbilityCall)
        if (node->has("function")) {
            const auto fn = node->getObject("function");
            if (!fn.isNull()) {
                if (auto mn = fn->optValue<std::string>("member_name", ""); !mn.empty()) {
                    n.fn_name = std::move(mn);
                }
                if (auto mp = fn->optValue<std::string>("member_parent", ""); !mp.empty()) {
                    n.fn_owner_short = ShortenPath(mp);
                }
            }
        }
        if (node->has("event_ref")) {
            const auto ev = node->getObject("event_ref");
            if (!ev.isNull()) {
                if (auto mn = ev->optValue<std::string>("member_name", ""); !mn.empty()) {
                    n.fn_name = std::move(mn);
                }
                if (auto mp = ev->optValue<std::string>("member_parent", ""); !mp.empty()) {
                    n.fn_owner_short = ShortenPath(mp);
                }
            }
        }
        if (auto cfn = node->optValue<std::string>("custom_function_name", "");
            !cfn.empty() && cfn != "None") {
            n.custom_event = std::move(cfn);
        }
        // variable_ref is an object { member_name, member_parent, ... } in UE5 export
        if (node->has("variable_ref")) {
            const auto vref = node->getObject("variable_ref");
            if (!vref.isNull()) {
                if (auto mn = vref->optValue<std::string>("member_name", ""); !mn.empty()) {
                    n.variable_name = std::move(mn);
                }
            }
        }
        if (auto vn = node->optValue<std::string>("var_name", ""); !vn.empty()) {
            n.variable_name = vn;
        }
        if (auto ct = node->optValue<std::string>("cast_target", ""); !ct.empty()) {
            n.cast_target_short = ShortenPath(ct);
        }
        // macro_ref is an object { macro_blueprint, macro_graph_guid } in UE5 export
        if (node->has("macro_ref")) {
            const auto mref = node->getObject("macro_ref");
            if (!mref.isNull()) {
                if (auto mb = mref->optValue<std::string>("macro_blueprint", ""); !mb.empty()) {
                    // "/Engine/EditorBlueprintResources/StandardMacros.StandardMacros" → "StandardMacros"
                    n.macro_ref = ShortenPath(mb);
                }
            }
        }

        return n;
    }

    static std::string DescribeNode(const NodeInfo& n) {
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
        if (n.class_short == "K2Node_IfThenElse") {
            return "Branch";
        }
        if (n.class_short == "K2Node_ExecutionSequence") {
            return "Sequence";
        }
        if (n.class_short == "K2Node_MacroInstance") {
            return "Macro:" + n.macro_ref.value_or("?");
        }
        return n.class_short;  // fallback
    }

    static std::string WalkExecChain(
        const std::unordered_map<std::string, NodeInfo>& nodes,
        const std::string& start_guid,
        int max_depth) {
        std::vector<std::string> path;
        std::unordered_set<std::string> visited;
        std::string cur = start_guid;
        for (int d = 0; d < max_depth; ++d) {
            if (visited.contains(cur)) break;
            visited.insert(cur);
            const auto it = nodes.find(cur);
            if (it == nodes.end()) break;
            const auto& n = it->second;
            if (n.class_short != "K2Node_Event" && n.class_short != "K2Node_CustomEvent") {
                path.push_back(DescribeNode(n));
            }
            if (n.exec_out.empty()) break;
            // Pick the first "then"-like output (simple linear walk for PoC).
            // A full implementation would fan out on Sequence/Branch and record branches.
            const auto& next = n.exec_out.front();
            cur = next.second;
        }
        std::string joined;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i) joined += " → ";
            joined += path[i];
        }
        return joined;
    }
};

// ── Pretty print ────────────────────────────────────────────────────────────

void PrintFacts(const std::vector<Fact>& facts) {
    // Group by entity
    std::unordered_map<std::string, std::vector<const Fact*>> grouped;
    std::vector<std::string> order;
    for (const auto& f : facts) {
        auto [it, inserted] = grouped.try_emplace(f.entity);
        if (inserted) order.push_back(f.entity);
        it->second.push_back(&f);
    }

    std::cout << "\n══════════════════════════════════════════════════════════\n"
              << " Structural facts — total: " << facts.size() << "\n"
              << "══════════════════════════════════════════════════════════\n";
    for (const auto& ent : order) {
        std::cout << "\n" << ent << "\n";
        std::cout << std::string(ent.size(), '-') << "\n";
        for (const auto* f : grouped[ent]) {
            std::cout << "  " << f->attribute << " : " << f->value << "\n";
        }
    }
    std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: waxcpp_bpl_structural_enricher_poc <path.bpl_json>\n";
        return 1;
    }
    try {
        const std::filesystem::path p = argv[1];
        const auto raw = ReadFile(p);
        BplStructuralEnricher enricher;
        const auto facts = enricher.Enrich(raw);
        PrintFacts(facts);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
