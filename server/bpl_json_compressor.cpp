// server/bpl_json_compressor.cpp
#include "bpl_json_compressor.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace waxcpp::server {

namespace {

/// Approximate token count by counting word boundaries (whitespace + punctuation).
int ApproxTokens(const std::string& text) {
    if (text.empty()) return 0;
    int count = 1;
    bool in_word = false;
    for (char c : text) {
        const bool is_sep = std::isspace(static_cast<unsigned char>(c)) ||
                            c == '{' || c == '}' || c == '[' || c == ']' ||
                            c == ',' || c == ':' || c == '"';
        if (is_sep && in_word) {
            ++count;
            in_word = false;
        } else if (!is_sep) {
            in_word = true;
        }
    }
    return std::max(1, count);
}

bool IsEmptyOrTrivialDefault(const std::string& val) {
    if (val.empty()) return true;
    if (val == "false" || val == "False" || val == "FALSE") return true;
    if (val == "None" || val == "none" || val == "NONE") return true;
    if (val == "0" || val == "0.0" || val == "0.000000") return true;
    return false;
}

const std::unordered_set<std::string>& NoisePinNames() {
    static const std::unordered_set<std::string> names{
        "then", "execute", "self", "ReturnValue",
        "OutputDelegate",
    };
    return names;
}

/// Process a single node object, returning a compressed version or null if empty.
Poco::JSON::Object::Ptr CompressNode(
    const Poco::JSON::Object::Ptr& node,
    const BplJsonCompressorConfig& config) {

    if (node.isNull()) return {};

    auto out = Poco::makeShared<Poco::JSON::Object>();

    // title
    if (node->has("title")) {
        out->set("title", node->getValue<std::string>("title"));
    }

    // class_path -> class (shortened)
    if (node->has("class_path")) {
        const auto class_path = node->getValue<std::string>("class_path");
        if (config.shorten_class_paths) {
            out->set("class", ShortenClassPath(class_path));
        } else {
            out->set("class", class_path);
        }
    }

    // function reference (K2Node_CallFunction, K2Node_CallParentFunction)
    if (node->has("function")) {
        const auto func = node->getObject("function");
        if (!func.isNull()) {
            if (func->has("member_name")) {
                out->set("calls", func->getValue<std::string>("member_name"));
            }
            if (func->has("member_parent")) {
                auto parent = func->getValue<std::string>("member_parent");
                if (config.shorten_type_objs) {
                    parent = ShortenTypeObj(parent);
                }
                out->set("from", parent);
            }
        }
    }

    // event_ref (K2Node_Event)
    if (node->has("event_ref")) {
        const auto event_ref = node->getObject("event_ref");
        if (!event_ref.isNull() && event_ref->has("member_name")) {
            out->set("event", event_ref->getValue<std::string>("member_name"));
        }
    }

    // custom_function_name
    if (node->has("custom_function_name")) {
        const auto cfn = node->getValue<std::string>("custom_function_name");
        if (cfn != "None" && !cfn.empty()) {
            out->set("custom_event", cfn);
        }
    }

    // variable_ref (K2Node_VariableGet/Set)
    if (node->has("variable_ref")) {
        out->set("variable_ref", node->getValue<std::string>("variable_ref"));
    }
    if (node->has("var_name")) {
        out->set("var_name", node->getValue<std::string>("var_name"));
    }

    // cast_target (K2Node_DynamicCast)
    if (node->has("cast_target")) {
        auto ct = node->getValue<std::string>("cast_target");
        if (config.shorten_type_objs) {
            ct = ShortenTypeObj(ct);
        }
        out->set("cast_target", ct);
    }

    // macro_ref (K2Node_MacroInstance)
    if (node->has("macro_ref")) {
        out->set("macro_ref", node->getValue<std::string>("macro_ref"));
    }

    // properties (DataAssets)
    if (node->has("properties")) {
        out->set("properties", node->get("properties"));
    }

    // Pins: extract only semantically useful ones
    if (node->has("pins")) {
        const auto pins = node->getArray("pins");
        if (!pins.isNull() && pins->size() > 0) {
            auto typed_pins = Poco::makeShared<Poco::JSON::Array>();
            auto params = Poco::makeShared<Poco::JSON::Object>();

            for (std::size_t i = 0; i < pins->size(); ++i) {
                const auto pin = pins->getObject(static_cast<unsigned int>(i));
                if (pin.isNull()) continue;

                const auto pin_name = pin->optValue<std::string>("name", "");
                if (NoisePinNames().contains(pin_name)) continue;

                const auto type_obj = pin->optValue<std::string>("type_obj", "");
                const auto default_value = pin->optValue<std::string>("default_value", "");
                const auto direction = pin->optValue<std::string>("direction", "");

                // Keep pins with non-empty default_value (meaningful params)
                if (!default_value.empty() && !IsEmptyOrTrivialDefault(default_value) &&
                    direction == "in") {
                    params->set(pin_name, default_value);
                    continue;
                }

                // Keep pins with non-empty type_obj (typed outputs/inputs)
                if (!type_obj.empty() && direction == "out") {
                    auto pin_obj = Poco::makeShared<Poco::JSON::Object>();
                    pin_obj->set("name", pin_name);
                    if (config.shorten_type_objs) {
                        pin_obj->set("type", ShortenTypeObj(type_obj));
                    } else {
                        pin_obj->set("type", type_obj);
                    }
                    typed_pins->add(pin_obj);
                }
            }

            if (typed_pins->size() > 0) {
                out->set("typed_pins", typed_pins);
            }
            if (params->size() > 0) {
                out->set("params", params);
            }
        }
    }

    // Skip nodes that only have a title and class with no semantic content
    if (out->size() <= 2 && out->has("title") && out->has("class")) {
        const auto cls = out->optValue<std::string>("class", "");
        if (cls == "K2Node_FunctionResult" || cls == "K2Node_Knot") {
            return {};
        }
    }

    return out->size() > 0 ? out : Poco::JSON::Object::Ptr{};
}

}  // namespace

std::string ShortenClassPath(std::string_view class_path) {
    // "/Script/BlueprintGraph.K2Node_CallFunction" -> "K2Node_CallFunction"
    const auto dot_pos = class_path.rfind('.');
    if (dot_pos != std::string_view::npos && dot_pos + 1 < class_path.size()) {
        return std::string(class_path.substr(dot_pos + 1));
    }
    const auto slash_pos = class_path.rfind('/');
    if (slash_pos != std::string_view::npos && slash_pos + 1 < class_path.size()) {
        return std::string(class_path.substr(slash_pos + 1));
    }
    return std::string(class_path);
}

std::string ShortenTypeObj(std::string_view type_obj) {
    // "/Script/Engine.SkeletalMeshComponent" -> "SkeletalMeshComponent"
    // "/Script/GameplayAbilities.AbilitySystemBlueprintLibrary" -> "AbilitySystemBlueprintLibrary"
    const auto dot_pos = type_obj.rfind('.');
    if (dot_pos != std::string_view::npos && dot_pos + 1 < type_obj.size()) {
        return std::string(type_obj.substr(dot_pos + 1));
    }
    const auto slash_pos = type_obj.rfind('/');
    if (slash_pos != std::string_view::npos && slash_pos + 1 < type_obj.size()) {
        return std::string(type_obj.substr(slash_pos + 1));
    }
    return std::string(type_obj);
}

std::string CompressBplJson(std::string_view raw_json,
                            const BplJsonCompressorConfig& config) {
    if (raw_json.empty()) return {};

    // Strip UTF-8 BOM (EF BB BF) if present — UE5 exports with BOM.
    if (raw_json.size() >= 3 &&
        static_cast<unsigned char>(raw_json[0]) == 0xEF &&
        static_cast<unsigned char>(raw_json[1]) == 0xBB &&
        static_cast<unsigned char>(raw_json[2]) == 0xBF) {
        raw_json.remove_prefix(3);
    }

    try {
        Poco::JSON::Parser parser;
        const auto parsed = parser.parse(std::string(raw_json));
        const auto root = parsed.extract<Poco::JSON::Object::Ptr>();
        if (root.isNull()) return {};

        auto out = Poco::makeShared<Poco::JSON::Object>();

        // Top-level metadata
        if (root->has("asset_name")) {
            out->set("asset_name", root->getValue<std::string>("asset_name"));
        }
        if (root->has("asset_kind")) {
            out->set("asset_kind", root->getValue<std::string>("asset_kind"));
        }
        if (root->has("asset_class")) {
            auto ac = root->getValue<std::string>("asset_class");
            if (config.shorten_class_paths) {
                ac = ShortenClassPath(ac);
            }
            out->set("asset_class", ac);
        }

        // Process graphs
        if (root->has("graphs")) {
            const auto graphs = root->getArray("graphs");
            if (!graphs.isNull()) {
                auto out_graphs = Poco::makeShared<Poco::JSON::Array>();

                for (std::size_t gi = 0; gi < graphs->size(); ++gi) {
                    const auto graph = graphs->getObject(static_cast<unsigned int>(gi));
                    if (graph.isNull()) continue;

                    auto out_graph = Poco::makeShared<Poco::JSON::Object>();
                    if (graph->has("name")) {
                        out_graph->set("name", graph->getValue<std::string>("name"));
                    }

                    // Process nodes
                    if (graph->has("nodes")) {
                        const auto nodes = graph->getArray("nodes");
                        if (!nodes.isNull()) {
                            auto out_nodes = Poco::makeShared<Poco::JSON::Array>();
                            for (std::size_t ni = 0; ni < nodes->size(); ++ni) {
                                const auto node = nodes->getObject(static_cast<unsigned int>(ni));
                                auto compressed = CompressNode(node, config);
                                if (!compressed.isNull()) {
                                    out_nodes->add(compressed);
                                }
                            }
                            if (out_nodes->size() > 0) {
                                out_graph->set("nodes", out_nodes);
                            }
                        }
                    }

                    // Skip links entirely when strip_links is true
                    if (!config.strip_links && graph->has("links")) {
                        out_graph->set("links", graph->get("links"));
                    }

                    // Only add graph if it has nodes
                    if (out_graph->has("nodes")) {
                        out_graphs->add(out_graph);
                    }
                }

                if (out_graphs->size() > 0) {
                    out->set("graphs", out_graphs);
                }
            }
        }

        // Process variables at top level
        if (root->has("variables")) {
            const auto vars = root->getArray("variables");
            if (!vars.isNull() && vars->size() > 0) {
                out->set("variables", vars);
            }
        }

        // Process properties at top level (DataAssets)
        if (root->has("properties")) {
            out->set("properties", root->get("properties"));
        }

        std::ostringstream oss;
        if (config.compact_output) {
            out->stringify(oss);
        } else {
            out->stringify(oss, 2);
        }
        return oss.str();

    } catch (const std::exception& e) {
        std::cerr << "[BPL-COMPRESS] JSON parse error: " << e.what() << "\n";
        return {};
    }
}

std::vector<BplStructuralChunk> ChunkBplJsonByNodes(
    std::string_view compressed_json,
    int target_tokens) {

    std::vector<BplStructuralChunk> chunks;
    if (compressed_json.empty()) return chunks;
    if (target_tokens <= 0) target_tokens = 3000;

    try {
        Poco::JSON::Parser parser;
        const auto parsed = parser.parse(std::string(compressed_json));
        const auto root = parsed.extract<Poco::JSON::Object::Ptr>();
        if (root.isNull()) return chunks;

        const auto asset_name = root->optValue<std::string>("asset_name", "");
        const auto asset_kind = root->optValue<std::string>("asset_kind", "");
        const auto asset_class = root->optValue<std::string>("asset_class", "");

        // If entire compressed JSON fits in target, return as single chunk
        const int total_tokens = ApproxTokens(std::string(compressed_json));
        if (total_tokens <= target_tokens) {
            chunks.push_back({std::string(compressed_json), "", total_tokens});
            return chunks;
        }

        // Top-level properties (DataAsset) — emit as a separate chunk if present
        if (root->has("properties")) {
            auto prop_chunk = Poco::makeShared<Poco::JSON::Object>();
            prop_chunk->set("asset_name", asset_name);
            prop_chunk->set("asset_kind", asset_kind);
            if (!asset_class.empty()) prop_chunk->set("asset_class", asset_class);
            prop_chunk->set("properties", root->get("properties"));
            std::ostringstream oss;
            prop_chunk->stringify(oss);
            auto text = oss.str();
            const int prop_tokens = ApproxTokens(text);
            chunks.push_back({std::move(text), "_properties", prop_tokens});
        }

        const auto graphs = root->getArray("graphs");
        if (graphs.isNull()) return chunks;

        for (std::size_t gi = 0; gi < graphs->size(); ++gi) {
            const auto graph = graphs->getObject(static_cast<unsigned int>(gi));
            if (graph.isNull()) continue;

            const auto graph_name = graph->optValue<std::string>("name", "");
            const auto nodes = graph->getArray("nodes");
            if (nodes.isNull() || nodes->size() == 0) continue;

            // Build ancestor breadcrumb overhead
            auto make_chunk_obj = [&]() {
                auto obj = Poco::makeShared<Poco::JSON::Object>();
                obj->set("asset_name", asset_name);
                if (!asset_kind.empty()) obj->set("asset_kind", asset_kind);
                obj->set("graph", graph_name);
                obj->set("nodes", Poco::makeShared<Poco::JSON::Array>());
                return obj;
            };

            // Serialize breadcrumb to estimate overhead
            auto breadcrumb = make_chunk_obj();
            std::ostringstream bss;
            breadcrumb->stringify(bss);
            const int breadcrumb_tokens = ApproxTokens(bss.str());

            auto current = make_chunk_obj();
            int current_tokens = breadcrumb_tokens;

            for (std::size_t ni = 0; ni < nodes->size(); ++ni) {
                const auto node = nodes->getObject(static_cast<unsigned int>(ni));
                if (node.isNull()) continue;

                std::ostringstream nss;
                node->stringify(nss);
                const int node_tokens = ApproxTokens(nss.str());

                // If a single node exceeds target, emit it alone
                if (node_tokens + breadcrumb_tokens > target_tokens) {
                    // Flush current chunk first if it has nodes
                    auto cur_nodes = current->getArray("nodes");
                    if (!cur_nodes.isNull() && cur_nodes->size() > 0) {
                        std::ostringstream css;
                        current->stringify(css);
                        auto text = css.str();
                        chunks.push_back({std::move(text), graph_name, current_tokens});
                        current = make_chunk_obj();
                        current_tokens = breadcrumb_tokens;
                    }
                    // Emit oversized node as its own chunk
                    auto solo = make_chunk_obj();
                    solo->getArray("nodes")->add(node);
                    std::ostringstream sss;
                    solo->stringify(sss);
                    auto text = sss.str();
                    chunks.push_back({std::move(text), graph_name, node_tokens + breadcrumb_tokens});
                    continue;
                }

                // Would adding this node exceed target?
                if (current_tokens + node_tokens > target_tokens) {
                    // Flush current chunk
                    auto cur_nodes = current->getArray("nodes");
                    if (!cur_nodes.isNull() && cur_nodes->size() > 0) {
                        std::ostringstream css;
                        current->stringify(css);
                        auto text = css.str();
                        chunks.push_back({std::move(text), graph_name, current_tokens});
                    }
                    current = make_chunk_obj();
                    current_tokens = breadcrumb_tokens;
                }

                current->getArray("nodes")->add(node);
                current_tokens += node_tokens;
            }

            // Flush remaining nodes
            auto cur_nodes = current->getArray("nodes");
            if (!cur_nodes.isNull() && cur_nodes->size() > 0) {
                std::ostringstream css;
                current->stringify(css);
                auto text = css.str();
                chunks.push_back({std::move(text), graph_name, current_tokens});
            }
        }

    } catch (const std::exception&) {
        // Fallback: return entire compressed JSON as single chunk
        if (!compressed_json.empty()) {
            chunks.push_back({std::string(compressed_json), "", ApproxTokens(std::string(compressed_json))});
        }
    }

    return chunks;
}

}  // namespace waxcpp::server
