#include "waxcpp/runtime_model_config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path UniqueTempDir() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_runtime_model_config_test_" + std::to_string(static_cast<long long>(now)));
}

waxcpp::RuntimeModelsConfig MakeBaseConfig(const std::filesystem::path& llama_root) {
  waxcpp::RuntimeModelsConfig config{};
  config.generation_model.runtime = "llama_cpp";
  config.generation_model.model_path = "g:/Proj/Agents1/Models/Qwen/Qwen3-Coder-Next-Q4_K_M.gguf";
  config.embedding_model.runtime = "disabled";
  config.embedding_model.model_path = "";
  config.llama_cpp_root = llama_root.string();
  config.enable_vector_search = false;
  config.require_distinct_models = true;
  return config;
}

void ExpectThrows(const std::string& scenario, const std::function<void()>& fn) {
  bool threw = false;
  try {
    fn();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "expected throw: " + scenario);
}

}  // namespace

int main() {
  const auto temp_dir = UniqueTempDir();
  std::error_code ec;
  std::filesystem::create_directories(temp_dir, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp dir for runtime_model_config_test");
  }

  try {
    {
      auto config = MakeBaseConfig(temp_dir);
      waxcpp::ValidateRuntimeModelsConfig(config);
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.generation_model.runtime = "libtorch";
      ExpectThrows("generation runtime must be llama_cpp", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.generation_model.model_path = "qwen-model.bin";
      ExpectThrows("generation model path must be gguf", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.embedding_model.runtime = "libtorch";
      config.embedding_model.model_path = "embedder.pt";
      ExpectThrows("embedding runtime must not be libtorch", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.enable_vector_search = true;
      config.embedding_model.runtime = "disabled";
      ExpectThrows("vector search requires llama_cpp embedding runtime", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.enable_vector_search = true;
      config.embedding_model.runtime = "llama_cpp";
      config.embedding_model.model_path = "embedder-model.bin";
      ExpectThrows("vector search embedding path must be gguf", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.enable_vector_search = true;
      config.embedding_model.runtime = "llama_cpp";
      config.embedding_model.model_path = "g:/Proj/Agents1/Models/Qwen/Qwen3-Embedding-0.6B-Q4_K_M.gguf";
      waxcpp::ValidateRuntimeModelsConfig(config);
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.embedding_model.runtime = "llama_cpp";
      config.embedding_model.model_path = config.generation_model.model_path;
      ExpectThrows("generation and embedding paths must differ", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir);
      config.llama_cpp_root.clear();
      ExpectThrows("llama_cpp root must be required", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    {
      auto config = MakeBaseConfig(temp_dir / "missing-dir");
      ExpectThrows("llama_cpp root must exist", [&]() {
        waxcpp::ValidateRuntimeModelsConfig(config);
      });
    }

    Require(waxcpp::ParseModelRuntimeKind("llama_cpp") == waxcpp::ModelRuntimeKind::kLlamaCpp,
            "runtime parse failed for llama_cpp");
    Require(waxcpp::ParseModelRuntimeKind("llama.cpp") == waxcpp::ModelRuntimeKind::kLlamaCpp,
            "runtime parse failed for llama.cpp");
    Require(waxcpp::ParseModelRuntimeKind("torch") == waxcpp::ModelRuntimeKind::kLibTorch,
            "runtime parse failed for torch");
    Require(waxcpp::ParseModelRuntimeKind("disabled") == waxcpp::ModelRuntimeKind::kDisabled,
            "runtime parse failed for disabled");
    Require(waxcpp::IsGgufPath("qwen.gguf"), "gguf detection failed for lowercase extension");
    Require(waxcpp::IsGgufPath("QWEN.GGUF"), "gguf detection failed for uppercase extension");
    Require(!waxcpp::IsGgufPath("qwen.bin"), "gguf detection should reject non-gguf extension");

    std::filesystem::remove_all(temp_dir, ec);
    return EXIT_SUCCESS;
  } catch (...) {
    std::filesystem::remove_all(temp_dir, ec);
    throw;
  }
}
