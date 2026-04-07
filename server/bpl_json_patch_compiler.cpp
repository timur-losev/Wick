// server/bpl_json_patch_compiler.cpp
#include "bpl_json_patch_compiler.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/UUIDGenerator.h>

#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace waxcpp::server {

namespace {

// ── Node type → UE5 class_path ─────────────────────────────

const std::unordered_map<std::string, std::string>& NodeTypeMap() {
    static const std::unordered_map<std::string, std::string> m{
        {"Event",              "/Script/BlueprintGraph.K2Node_Event"},
        {"CustomEvent",        "/Script/BlueprintGraph.K2Node_CustomEvent"},
        {"CallFunction",       "/Script/BlueprintGraph.K2Node_CallFunction"},
        {"CallParentFunction", "/Script/BlueprintGraph.K2Node_CallParentFunction"},
        {"VariableGet",        "/Script/BlueprintGraph.K2Node_VariableGet"},
        {"VariableSet",        "/Script/BlueprintGraph.K2Node_VariableSet"},
        {"Cast",               "/Script/BlueprintGraph.K2Node_DynamicCast"},
        {"Branch",             "/Script/BlueprintGraph.K2Node_IfThenElse"},
        {"Sequence",           "/Script/BlueprintGraph.K2Node_ExecutionSequence"},
        {"MacroInstance",      "/Script/BlueprintGraph.K2Node_MacroInstance"},
        {"FunctionEntry",      "/Script/BlueprintGraph.K2Node_FunctionEntry"},
        {"FunctionResult",     "/Script/BlueprintGraph.K2Node_FunctionResult"},
        {"Select",             "/Script/BlueprintGraph.K2Node_Select"},
        {"ComponentBoundEvent","/Script/BlueprintGraph.K2Node_ComponentBoundEvent"},
    };
    return m;
}

const std::string kNullGuid = "00000000000000000000000000000000";

std::string NewGuid() {
    auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
    std::string s = uuid.toString();
    // Remove dashes and uppercase
    std::string result;
    result.reserve(32);
    for (char c : s) {
        if (c != '-') {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

std::string OptStr(const Poco::JSON::Object::Ptr& obj, const std::string& key, const std::string& def = "") {
    if (obj.isNull() || !obj->has(key)) return def;
    return obj->optValue<std::string>(key, def);
}

bool OptBool(const Poco::JSON::Object::Ptr& obj, const std::string& key, bool def = false) {
    if (obj.isNull() || !obj->has(key)) return def;
    return obj->optValue<bool>(key, def);
}

// Find node in array by title, return its object and node_guid
Poco::JSON::Object::Ptr FindNodeByTitle(const Poco::JSON::Array::Ptr& nodes, const std::string& title) {
    if (nodes.isNull()) return {};
    for (unsigned i = 0; i < nodes->size(); ++i) {
        auto node = nodes->getObject(i);
        if (!node.isNull() && node->optValue<std::string>("title", "") == title) {
            return node;
        }
    }
    return {};
}

std::string AvailableTitles(const Poco::JSON::Array::Ptr& nodes) {
    std::string result;
    if (nodes.isNull()) return result;
    for (unsigned i = 0; i < nodes->size(); ++i) {
        auto node = nodes->getObject(i);
        if (node.isNull()) continue;
        auto t = node->optValue<std::string>("title", "");
        if (t.empty()) continue;
        if (!result.empty()) result += ", ";
        result += t;
    }
    return result;
}

// Resolve "existing:Title" or ref name → node_guid
std::string ResolveRef(const std::string& ref,
                       const std::unordered_map<std::string, std::string>& refMap,
                       const Poco::JSON::Array::Ptr& nodes,
                       std::string& error) {
    if (ref.rfind("existing:", 0) == 0) {
        auto title = ref.substr(9);
        auto node = FindNodeByTitle(nodes, title);
        if (node.isNull()) {
            error = "Node '" + ref + "' not found. Available: " + AvailableTitles(nodes);
            return {};
        }
        return node->optValue<std::string>("node_guid", "");
    }
    auto it = refMap.find(ref);
    if (it == refMap.end()) {
        std::string available;
        for (auto& [k, v] : refMap) {
            if (!available.empty()) available += ", ";
            available += k;
        }
        error = "Ref '" + ref + "' not found in add_nodes. Available refs: " + available;
        return {};
    }
    return it->second;
}

// Build a full node object from intent
Poco::JSON::Object::Ptr BuildNode(const Poco::JSON::Object::Ptr& intent, int posX, int posY,
                                   std::string& outGuid, std::string& error) {
    auto type = OptStr(intent, "type");
    auto& typeMap = NodeTypeMap();
    auto it = typeMap.find(type);
    if (it == typeMap.end()) {
        error = "Unknown node type '" + type + "'. Valid: ";
        for (auto& [k, v] : typeMap) { error += k + " "; }
        return {};
    }

    outGuid = NewGuid();
    auto node = Poco::makeShared<Poco::JSON::Object>();
    node->set("node_guid", outGuid);
    node->set("class_path", it->second);
    node->set("pos_x", posX);
    node->set("pos_y", posY);
    node->set("comment", "");
    node->set("pins", Poco::makeShared<Poco::JSON::Array>());

    auto title = OptStr(intent, "title");

    // CallFunction / CallParentFunction
    if (type == "CallFunction" || type == "CallParentFunction") {
        auto func = Poco::makeShared<Poco::JSON::Object>();
        func->set("member_name", OptStr(intent, "function"));
        func->set("member_parent", OptStr(intent, "function_owner"));
        func->set("member_guid", kNullGuid);
        node->set("function", func);
        if (title.empty()) title = OptStr(intent, "function");
    }

    // Event
    if (type == "Event") {
        auto ref = Poco::makeShared<Poco::JSON::Object>();
        ref->set("member_name", OptStr(intent, "event"));
        ref->set("member_parent", OptStr(intent, "event_owner", "/Script/Engine.Actor"));
        ref->set("member_guid", kNullGuid);
        node->set("event_ref", ref);
        if (title.empty()) title = OptStr(intent, "event");
    }

    // CustomEvent
    if (type == "CustomEvent") {
        auto name = OptStr(intent, "event_name", OptStr(intent, "title"));
        node->set("custom_function_name", name);
        if (title.empty()) title = name;
    }

    // VariableGet / VariableSet
    if (type == "VariableGet" || type == "VariableSet") {
        node->set("variable_ref", OptStr(intent, "variable"));
        node->set("var_name", OptStr(intent, "variable"));
        if (title.empty()) title = OptStr(intent, "variable");
    }

    // Cast
    if (type == "Cast") {
        node->set("cast_target", OptStr(intent, "cast_target"));
        if (title.empty()) title = "Cast To " + OptStr(intent, "cast_target");
    }

    // MacroInstance
    if (type == "MacroInstance") {
        node->set("macro_ref", OptStr(intent, "macro"));
        if (title.empty()) title = OptStr(intent, "macro");
    }

    // Default titles
    if (title.empty() && type == "Branch") title = "Branch";
    if (title.empty() && type == "Sequence") title = "Sequence";

    node->set("title", title);
    return node;
}

// Extract asset_name from path: "/Game/Foo/Bar.Bar" → "Bar"
std::string AssetNameFromPath(const std::string& path) {
    auto lastDot = path.rfind('.');
    auto lastSlash = path.rfind('/');
    if (lastDot != std::string::npos && (lastSlash == std::string::npos || lastDot > lastSlash)) {
        return path.substr(lastDot + 1);
    }
    if (lastSlash != std::string::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

}  // namespace

PatchResult CompileBlueprintPatch(std::string_view existing_json,
                                   std::string_view intent_json) {
    PatchResult result;

    try {
        Poco::JSON::Parser intentParser;
        auto intentParsed = intentParser.parse(std::string(intent_json));
        auto intent = intentParsed.extract<Poco::JSON::Object::Ptr>();
        if (intent.isNull()) {
            result.error = "Failed to parse intent JSON";
            return result;
        }

        // Parse or create existing bpl
        Poco::JSON::Object::Ptr bpl;
        if (OptBool(intent, "create")) {
            auto target = OptStr(intent, "target");
            bpl = Poco::makeShared<Poco::JSON::Object>();
            bpl->set("blueprint", target);
            bpl->set("asset_path", target);
            bpl->set("asset_name", AssetNameFromPath(target));
            bpl->set("asset_class", OptStr(intent, "parent_class", "/Script/Engine.Blueprint"));
            bpl->set("asset_kind", "blueprint");
            bpl->set("graphs", Poco::makeShared<Poco::JSON::Array>());
            bpl->set("variables", Poco::makeShared<Poco::JSON::Array>());
        } else {
            if (existing_json.empty()) {
                result.error = "No existing blueprint JSON provided and create:true not set";
                return result;
            }
            // Strip BOM
            auto sv = existing_json;
            if (sv.size() >= 3 &&
                static_cast<unsigned char>(sv[0]) == 0xEF &&
                static_cast<unsigned char>(sv[1]) == 0xBB &&
                static_cast<unsigned char>(sv[2]) == 0xBF) {
                sv.remove_prefix(3);
            }
            Poco::JSON::Parser existParser;
            auto existParsed = existParser.parse(std::string(sv));
            bpl = existParsed.extract<Poco::JSON::Object::Ptr>();
            if (bpl.isNull()) {
                result.error = "Failed to parse existing blueprint JSON";
                return result;
            }
        }

        int totalNodesAdded = 0, totalLinksAdded = 0;
        int totalNodesRemoved = 0, totalLinksRemoved = 0;
        std::vector<std::string> patchedGraphs;

        auto intentGraphs = intent->getArray("graphs");
        if (intentGraphs.isNull()) intentGraphs = Poco::makeShared<Poco::JSON::Array>();

        auto bplGraphs = bpl->getArray("graphs");
        if (bplGraphs.isNull()) {
            bplGraphs = Poco::makeShared<Poco::JSON::Array>();
            bpl->set("graphs", bplGraphs);
        }

        for (unsigned gi = 0; gi < intentGraphs->size(); ++gi) {
            auto graphPatch = intentGraphs->getObject(gi);
            if (graphPatch.isNull()) continue;

            auto graphName = OptStr(graphPatch, "name");

            // Find or create graph
            Poco::JSON::Object::Ptr graph;
            for (unsigned bi = 0; bi < bplGraphs->size(); ++bi) {
                auto g = bplGraphs->getObject(bi);
                if (!g.isNull() && g->optValue<std::string>("name", "") == graphName) {
                    graph = g;
                    break;
                }
            }
            if (graph.isNull()) {
                graph = Poco::makeShared<Poco::JSON::Object>();
                graph->set("name", graphName);
                graph->set("graph_guid", NewGuid());
                graph->set("schema_class", "/Script/BlueprintGraph.EdGraphSchema_K2");
                graph->set("nodes", Poco::makeShared<Poco::JSON::Array>());
                graph->set("links", Poco::makeShared<Poco::JSON::Array>());
                bplGraphs->add(graph);
            }

            auto nodes = graph->getArray("nodes");
            if (nodes.isNull()) { nodes = Poco::makeShared<Poco::JSON::Array>(); graph->set("nodes", nodes); }
            auto links = graph->getArray("links");
            if (links.isNull()) { links = Poco::makeShared<Poco::JSON::Array>(); graph->set("links", links); }

            // ── remove_nodes ──
            auto removeNodes = graphPatch->getArray("remove_nodes");
            if (!removeNodes.isNull()) {
                for (unsigned ri = 0; ri < removeNodes->size(); ++ri) {
                    std::string title;
                    auto val = removeNodes->get(ri);
                    if (val.isString()) title = val.convert<std::string>();
                    else if (val.type() == typeid(Poco::JSON::Object::Ptr)) {
                        auto obj = val.extract<Poco::JSON::Object::Ptr>();
                        title = OptStr(obj, "title");
                    }
                    if (title.empty()) continue;

                    // Find and remove
                    for (unsigned ni = 0; ni < nodes->size(); ++ni) {
                        auto n = nodes->getObject(ni);
                        if (!n.isNull() && n->optValue<std::string>("title", "") == title) {
                            auto removedGuid = n->optValue<std::string>("node_guid", "");
                            nodes->remove(ni);
                            // Remove links referencing this node
                            auto newLinks = Poco::makeShared<Poco::JSON::Array>();
                            for (unsigned li = 0; li < links->size(); ++li) {
                                auto l = links->getObject(li);
                                if (l.isNull()) continue;
                                if (l->optValue<std::string>("from_node_guid", "") == removedGuid) continue;
                                if (l->optValue<std::string>("to_node_guid", "") == removedGuid) continue;
                                newLinks->add(l);
                            }
                            graph->set("links", newLinks);
                            links = newLinks;
                            totalNodesRemoved++;
                            break;
                        }
                    }
                }
            }

            // ── remove_links ──
            auto removeLinks = graphPatch->getArray("remove_links");
            if (!removeLinks.isNull()) {
                std::unordered_map<std::string, std::string> emptyRefMap;
                for (unsigned ri = 0; ri < removeLinks->size(); ++ri) {
                    auto rl = removeLinks->getObject(ri);
                    if (rl.isNull()) continue;

                    std::string err;
                    auto fromGuid = ResolveRef(OptStr(rl, "from"), emptyRefMap, nodes, err);
                    if (!err.empty()) { result.error = err; return result; }
                    auto toGuid = ResolveRef(OptStr(rl, "to"), emptyRefMap, nodes, err);
                    if (!err.empty()) { result.error = err; return result; }
                    auto fromPin = OptStr(rl, "from_pin");
                    auto toPin = OptStr(rl, "to_pin");

                    auto newLinks = Poco::makeShared<Poco::JSON::Array>();
                    for (unsigned li = 0; li < links->size(); ++li) {
                        auto l = links->getObject(li);
                        if (l.isNull()) { continue; }
                        if (l->optValue<std::string>("from_node_guid", "") == fromGuid &&
                            l->optValue<std::string>("from_pin_name", "") == fromPin &&
                            l->optValue<std::string>("to_node_guid", "") == toGuid &&
                            l->optValue<std::string>("to_pin_name", "") == toPin) {
                            totalLinksRemoved++;
                            continue;
                        }
                        newLinks->add(l);
                    }
                    graph->set("links", newLinks);
                    links = newLinks;
                }
            }

            // ── add_nodes ──
            std::unordered_map<std::string, std::string> refMap;

            // Validate ref uniqueness
            auto addNodes = graphPatch->getArray("add_nodes");
            if (!addNodes.isNull()) {
                std::unordered_set<std::string> seenRefs;
                for (unsigned ni = 0; ni < addNodes->size(); ++ni) {
                    auto an = addNodes->getObject(ni);
                    if (an.isNull()) continue;
                    auto ref = OptStr(an, "ref");
                    if (!ref.empty() && !seenRefs.insert(ref).second) {
                        result.error = "Duplicate ref name in add_nodes: " + ref;
                        return result;
                    }
                }
            }

            // Compute starting position
            int maxX = 0;
            for (unsigned ni = 0; ni < nodes->size(); ++ni) {
                auto n = nodes->getObject(ni);
                if (n.isNull()) continue;
                int px = n->optValue<int>("pos_x", 0);
                if (px > maxX) maxX = px;
            }
            int nextX = maxX + 300;
            int nextY = 0;

            if (!addNodes.isNull()) {
                for (unsigned ni = 0; ni < addNodes->size(); ++ni) {
                    auto intentNode = addNodes->getObject(ni);
                    if (intentNode.isNull()) continue;

                    std::string guid, err;
                    auto node = BuildNode(intentNode, nextX, nextY, guid, err);
                    if (!err.empty()) { result.error = err; return result; }

                    auto ref = OptStr(intentNode, "ref");
                    if (!ref.empty()) refMap[ref] = guid;

                    nodes->add(node);
                    nextY += 200;
                    totalNodesAdded++;
                }
            }

            // ── set_defaults ──
            auto setDefaults = graphPatch->getArray("set_defaults");
            if (!setDefaults.isNull()) {
                for (unsigned si = 0; si < setDefaults->size(); ++si) {
                    auto sd = setDefaults->getObject(si);
                    if (sd.isNull()) continue;

                    auto nodeRef = OptStr(sd, "node");
                    auto pinName = OptStr(sd, "pin");
                    auto value = OptStr(sd, "value");

                    Poco::JSON::Object::Ptr targetNode;
                    if (nodeRef.rfind("existing:", 0) == 0) {
                        targetNode = FindNodeByTitle(nodes, nodeRef.substr(9));
                    } else {
                        auto it2 = refMap.find(nodeRef);
                        if (it2 != refMap.end()) {
                            for (unsigned ni2 = 0; ni2 < nodes->size(); ++ni2) {
                                auto n = nodes->getObject(ni2);
                                if (!n.isNull() && n->optValue<std::string>("node_guid", "") == it2->second) {
                                    targetNode = n;
                                    break;
                                }
                            }
                        }
                    }

                    if (!targetNode.isNull()) {
                        auto pins = targetNode->getArray("pins");
                        if (pins.isNull()) {
                            pins = Poco::makeShared<Poco::JSON::Array>();
                            targetNode->set("pins", pins);
                        }
                        bool found = false;
                        for (unsigned pi = 0; pi < pins->size(); ++pi) {
                            auto p = pins->getObject(pi);
                            if (!p.isNull() && p->optValue<std::string>("name", "") == pinName) {
                                p->set("default_value", value);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            auto stub = Poco::makeShared<Poco::JSON::Object>();
                            stub->set("pin_id", kNullGuid);
                            stub->set("name", pinName);
                            stub->set("direction", "in");
                            stub->set("type_cat", "none");
                            stub->set("default_value", value);
                            pins->add(stub);
                        }
                    }
                }
            }

            // ── add_links ──
            auto addLinks = graphPatch->getArray("add_links");
            if (!addLinks.isNull()) {
                for (unsigned li = 0; li < addLinks->size(); ++li) {
                    auto al = addLinks->getObject(li);
                    if (al.isNull()) continue;

                    std::string err;
                    auto fromGuid = ResolveRef(OptStr(al, "from"), refMap, nodes, err);
                    if (!err.empty()) { result.error = err; return result; }
                    auto toGuid = ResolveRef(OptStr(al, "to"), refMap, nodes, err);
                    if (!err.empty()) { result.error = err; return result; }

                    auto link = Poco::makeShared<Poco::JSON::Object>();
                    link->set("from_node_guid", fromGuid);
                    link->set("from_pin_name", OptStr(al, "from_pin"));
                    link->set("from_pin_id", kNullGuid);
                    link->set("to_node_guid", toGuid);
                    link->set("to_pin_name", OptStr(al, "to_pin"));
                    link->set("to_pin_id", kNullGuid);
                    links->add(link);
                    totalLinksAdded++;
                }
            }

            patchedGraphs.push_back(graphName);
        }

        // ── Merge variables ──
        auto intentVars = intent->getArray("variables");
        if (!intentVars.isNull() && intentVars->size() > 0) {
            auto bplVars = bpl->getArray("variables");
            if (bplVars.isNull()) {
                bplVars = Poco::makeShared<Poco::JSON::Array>();
                bpl->set("variables", bplVars);
            }
            for (unsigned vi = 0; vi < intentVars->size(); ++vi) {
                auto v = intentVars->getObject(vi);
                if (v.isNull()) continue;
                auto varName = OptStr(v, "var_name");
                if (varName.empty()) continue;

                // Find existing
                Poco::JSON::Object::Ptr existing;
                for (unsigned ei = 0; ei < bplVars->size(); ++ei) {
                    auto ev = bplVars->getObject(ei);
                    if (!ev.isNull() && ev->optValue<std::string>("var_name", "") == varName) {
                        existing = ev;
                        break;
                    }
                }
                if (!existing.isNull()) {
                    if (v->has("var_type")) existing->set("var_type", v->get("var_type"));
                    if (v->has("default_value")) existing->set("default_value", v->getValue<std::string>("default_value"));
                } else {
                    auto newVar = Poco::makeShared<Poco::JSON::Object>();
                    newVar->set("var_name", varName);
                    newVar->set("var_guid", NewGuid());
                    newVar->set("var_type", v->has("var_type") ? v->get("var_type") : Poco::Dynamic::Var(Poco::makeShared<Poco::JSON::Object>()));
                    newVar->set("default_value", OptStr(v, "default_value"));
                    newVar->set("property_flags", v->optValue<int>("property_flags", 0));
                    bplVars->add(newVar);
                }
            }
        }

        // Serialize
        std::ostringstream oss;
        bpl->stringify(oss, 1);
        result.json = oss.str();

        // Build summary
        std::string parts;
        if (totalNodesAdded > 0) { if (!parts.empty()) parts += ", "; parts += std::to_string(totalNodesAdded) + " nodes added"; }
        if (totalLinksAdded > 0) { if (!parts.empty()) parts += ", "; parts += std::to_string(totalLinksAdded) + " links added"; }
        if (totalNodesRemoved > 0) { if (!parts.empty()) parts += ", "; parts += std::to_string(totalNodesRemoved) + " nodes removed"; }
        if (totalLinksRemoved > 0) { if (!parts.empty()) parts += ", "; parts += std::to_string(totalLinksRemoved) + " links removed"; }

        std::string graphList;
        for (auto& g : patchedGraphs) { if (!graphList.empty()) graphList += ", "; graphList += g; }
        result.summary = "Patched " + graphList + ": " + (parts.empty() ? "no changes" : parts);

    } catch (const std::exception& e) {
        result.error = std::string("Patch compiler error: ") + e.what();
    }

    return result;
}

}  // namespace waxcpp::server
