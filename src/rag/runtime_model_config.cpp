#include "waxcpp/runtime_model_config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace waxcpp {

namespace {

std::string ToAsciiLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

}  // namespace

ModelRuntimeKind ParseModelRuntimeKind(std::string_view runtime) {
  const std::string normalized = ToAsciiLower(runtime);
  if (normalized.empty() || normalized == "disabled" || normalized == "none") {
    return ModelRuntimeKind::kDisabled;
  }
  if (normalized == "llama_cpp" || normalized == "llama.cpp" || normalized == "llamacpp") {
    return ModelRuntimeKind::kLlamaCpp;
  }
  if (normalized == "libtorch" || normalized == "torch") {
    return ModelRuntimeKind::kLibTorch;
  }
  return ModelRuntimeKind::kUnknown;
}

bool IsGgufPath(std::string_view path) {
  if (path.empty()) {
    return false;
  }
  const std::string normalized = ToAsciiLower(path);
  return normalized.size() >= 5 && normalized.ends_with(".gguf");
}

void ValidateRuntimeModelsConfig(const RuntimeModelsConfig& config) {
  const auto generation_runtime = ParseModelRuntimeKind(config.generation_model.runtime);
  if (generation_runtime != ModelRuntimeKind::kLlamaCpp) {
    throw std::runtime_error("generation_model.runtime must be llama_cpp");
  }
  if (!IsGgufPath(config.generation_model.model_path)) {
    throw std::runtime_error("generation_model.model_path must point to a .gguf model");
  }

  const auto embedding_runtime = ParseModelRuntimeKind(config.embedding_model.runtime);
  if (embedding_runtime == ModelRuntimeKind::kLibTorch) {
    throw std::runtime_error("embedding_model.runtime must not use torch/libtorch");
  }
  if (embedding_runtime == ModelRuntimeKind::kUnknown) {
    throw std::runtime_error("embedding_model.runtime is unknown");
  }

  if (config.enable_vector_search) {
    if (embedding_runtime != ModelRuntimeKind::kLlamaCpp) {
      throw std::runtime_error("enable_vector_search requires embedding_model.runtime=llama_cpp");
    }
    if (!IsGgufPath(config.embedding_model.model_path)) {
      throw std::runtime_error("embedding_model.model_path must point to a .gguf model when vector search is enabled");
    }
  } else if (!config.embedding_model.model_path.empty()) {
    if (embedding_runtime != ModelRuntimeKind::kLlamaCpp) {
      throw std::runtime_error("embedding_model.model_path requires embedding_model.runtime=llama_cpp");
    }
    if (!IsGgufPath(config.embedding_model.model_path)) {
      throw std::runtime_error("embedding_model.model_path must point to a .gguf model");
    }
  }

  if (config.require_distinct_models &&
      !config.generation_model.model_path.empty() &&
      !config.embedding_model.model_path.empty()) {
    const std::string generation_path = ToAsciiLower(config.generation_model.model_path);
    const std::string embedding_path = ToAsciiLower(config.embedding_model.model_path);
    if (generation_path == embedding_path) {
      throw std::runtime_error("generation_model and embedding_model must use distinct model paths");
    }
  }

  if (generation_runtime == ModelRuntimeKind::kLlamaCpp ||
      embedding_runtime == ModelRuntimeKind::kLlamaCpp) {
    if (config.llama_cpp_root.empty()) {
      throw std::runtime_error("llama_cpp_root must be set when llama_cpp runtime is used");
    }
    std::error_code ec;
    const std::filesystem::path root(config.llama_cpp_root);
    if (!std::filesystem::exists(root, ec) || ec) {
      throw std::runtime_error("llama_cpp_root does not exist: " + config.llama_cpp_root);
    }
    if (!std::filesystem::is_directory(root, ec) || ec) {
      throw std::runtime_error("llama_cpp_root must be a directory: " + config.llama_cpp_root);
    }
  }
}

}  // namespace waxcpp
