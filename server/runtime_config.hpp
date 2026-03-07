#pragma once

#include "waxcpp/runtime_model_config.hpp"

#include <filesystem>
#include <optional>

namespace waxcpp::server {

struct ServerRuntimeConfig {
  waxcpp::RuntimeModelsConfig models{};
};

[[nodiscard]] ServerRuntimeConfig DefaultServerRuntimeConfig();
[[nodiscard]] std::optional<std::filesystem::path> ResolveServerRuntimeConfigPathFromEnv();
[[nodiscard]] ServerRuntimeConfig LoadServerRuntimeConfig(
    const std::optional<std::filesystem::path>& config_path);

}  // namespace waxcpp::server
