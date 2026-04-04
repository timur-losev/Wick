// server/bpl_json_compressor.hpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace waxcpp::server {

struct BplJsonCompressorConfig {
    bool strip_links = true;
    bool strip_empty_defaults = true;
    bool shorten_class_paths = true;
    bool shorten_type_objs = true;
    bool compact_output = true;
};

/// Compress a raw .bpl_json file, removing noise fields and keeping only
/// semantically relevant data for LLM enrichment.
/// Returns empty string if input is not valid JSON.
std::string CompressBplJson(std::string_view raw_json,
                            const BplJsonCompressorConfig& config = {});

/// Extract the short class name from a full UE class_path.
/// "/Script/BlueprintGraph.K2Node_CallFunction" -> "K2Node_CallFunction"
std::string ShortenClassPath(std::string_view class_path);

/// Extract the short type name from a full UE type_obj path.
/// "/Script/Engine.SkeletalMeshComponent" -> "SkeletalMeshComponent"
std::string ShortenTypeObj(std::string_view type_obj);

/// Structural chunk result: compressed JSON text + graph name for metadata.
struct BplStructuralChunk {
    std::string text;
    std::string graph_name;
    int token_estimate = 0;
};

/// Chunk compressed bpl_json by graph nodes, grouping nodes up to target_tokens.
/// Each chunk contains an ancestor breadcrumb (asset_name, asset_kind, graph).
std::vector<BplStructuralChunk> ChunkBplJsonByNodes(
    std::string_view compressed_json,
    int target_tokens);

}  // namespace waxcpp::server
