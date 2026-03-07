#pragma once

#include <string>
#include <string_view>

namespace waxcpp {

enum class ModelRuntimeKind {
  kUnknown = 0,
  kDisabled,
  kLlamaCpp,
  kLibTorch,
};

struct ModelRuntimeSpec {
  std::string runtime{};
  std::string model_path{};
};

struct RuntimeModelsConfig {
  ModelRuntimeSpec generation_model{};
  ModelRuntimeSpec embedding_model{};
  std::string llama_cpp_root{};
  bool enable_vector_search = false;
  bool require_distinct_models = true;
};

[[nodiscard]] ModelRuntimeKind ParseModelRuntimeKind(std::string_view runtime);
[[nodiscard]] bool IsGgufPath(std::string_view path);
void ValidateRuntimeModelsConfig(const RuntimeModelsConfig& config);

}  // namespace waxcpp
