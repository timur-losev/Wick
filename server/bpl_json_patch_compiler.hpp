// server/bpl_json_patch_compiler.hpp
#pragma once

#include <string>
#include <string_view>

namespace waxcpp::server {

struct PatchResult {
    std::string json;     // Merged bpl_json
    std::string summary;  // Human-readable summary
    std::string error;    // Empty on success
};

/// Apply a Blueprint Intent patch (.bpi_json) to an existing .bpl_json.
/// If intent has "create":true, existing_json may be empty.
/// Returns merged JSON + summary, or error message.
PatchResult CompileBlueprintPatch(std::string_view existing_json,
                                  std::string_view intent_json);

}  // namespace waxcpp::server
