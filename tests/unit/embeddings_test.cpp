#include "waxcpp/embeddings.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double L2Norm(const std::vector<float>& values) {
  double sum = 0.0;
  for (const auto value : values) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return std::sqrt(sum);
}

bool ApproxEqual(double lhs, double rhs, double eps) {
  return std::fabs(lhs - rhs) <= eps;
}

std::optional<std::string> GetEnvValue(const char* name) {
#ifdef _WIN32
  char* raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
    return std::nullopt;
  }
  std::string value(raw);
  std::free(raw);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
#else
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  return std::string(raw);
#endif
}

void SetEnvVar(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
  if (!value.has_value()) {
    (void)_putenv_s(name, "");
    return;
  }
  (void)_putenv_s(name, value->c_str());
#else
  if (!value.has_value()) {
    (void)unsetenv(name);
    return;
  }
  (void)setenv(name, value->c_str(), 1);
#endif
}

class ScopedEnvVar final {
 public:
  ScopedEnvVar(const char* name, std::optional<std::string> value) : name_(name) {
    original_ = GetEnvValue(name_);
    SetEnvVar(name_, value);
  }

  ~ScopedEnvVar() {
    SetEnvVar(name_, original_);
  }

 private:
  const char* name_;
  std::optional<std::string> original_{};
};

void ScenarioIdentityAndShape() {
  waxcpp::tests::Log("scenario: identity and shape");
  waxcpp::MiniLMEmbedderTorch embedder;
  Require(embedder.dimensions() == 384, "unexpected embedding dimension");
  Require(embedder.normalize(), "embedder should normalize vectors");
  const auto identity = embedder.identity();
  Require(identity.has_value(), "identity should be present");
  Require(identity->model.has_value() && *identity->model == "MiniLM-Torch", "identity model mismatch");
}

void ScenarioDeterministicEmbedding() {
  waxcpp::tests::Log("scenario: deterministic embedding");
  waxcpp::MiniLMEmbedderTorch embedder;
  const auto first = embedder.Embed("hello deterministic world");
  const auto second = embedder.Embed("hello deterministic world");
  const auto third = embedder.Embed("different content");

  Require(first.size() == static_cast<std::size_t>(embedder.dimensions()), "embedding size mismatch");
  Require(first == second, "same text should produce identical embedding");
  Require(first != third, "different text should produce different embedding");
}

void ScenarioNormalizationAndEmptyInput() {
  waxcpp::tests::Log("scenario: normalization and empty input");
  waxcpp::MiniLMEmbedderTorch embedder;
  const auto non_empty = embedder.Embed("alpha beta gamma");
  const auto norm = L2Norm(non_empty);
  Require(ApproxEqual(norm, 1.0, 1e-5), "non-empty embedding must be L2 normalized");

  const auto empty = embedder.Embed("");
  const auto empty_norm = L2Norm(empty);
  Require(ApproxEqual(empty_norm, 0.0, 1e-6), "empty embedding should stay zero vector");
}

void ScenarioBatchParity() {
  waxcpp::tests::Log("scenario: batch parity");
  waxcpp::MiniLMEmbedderTorch embedder;
  const std::vector<std::string> texts = {"first item", "second item", "third item"};
  const auto batch = embedder.EmbedBatch(texts);

  Require(batch.size() == texts.size(), "batch result size mismatch");
  for (std::size_t i = 0; i < texts.size(); ++i) {
    Require(batch[i] == embedder.Embed(texts[i]), "batch item must match single Embed output");
  }
}

void ScenarioMemoizationCapacity() {
  waxcpp::tests::Log("scenario: memoization capacity");
  waxcpp::MiniLMEmbedderTorch cached_embedder(2);
  const auto first_a = cached_embedder.Embed("alpha");
  const auto second_a = cached_embedder.Embed("alpha");
  Require(first_a == second_a, "memoized embedding must remain deterministic");
  Require(cached_embedder.cache_size() == 1, "repeated key should not increase cache size");

  (void)cached_embedder.Embed("beta");
  Require(cached_embedder.cache_size() == 2, "second unique key should fill cache");
  (void)cached_embedder.Embed("gamma");
  Require(cached_embedder.cache_size() == 2, "cache should enforce capacity bound");
  const auto third_a = cached_embedder.Embed("alpha");
  Require(third_a == first_a, "evicted key recomputation should remain deterministic");

  waxcpp::MiniLMEmbedderTorch uncached_embedder(0);
  (void)uncached_embedder.Embed("alpha");
  Require(uncached_embedder.cache_size() == 0, "zero-capacity embedder should not memoize");
}

void ScenarioAsciiTokenizationDeterminism() {
  waxcpp::tests::Log("scenario: ascii tokenization determinism");
  waxcpp::MiniLMEmbedderTorch embedder;
  const auto a = embedder.Embed("Alpha-42");
  const auto b = embedder.Embed("alpha 42");
  Require(a == b, "ASCII case-fold + delimiter tokenization should be deterministic");

  const auto c = embedder.Embed("alpha \xC3\xA9 42");
  const auto d = embedder.Embed("alpha 42");
  Require(c == d, "non-ASCII bytes should not perturb ASCII tokenization path");
}

void ScenarioRuntimeInfoAndManifestPolicy() {
  waxcpp::tests::Log("scenario: runtime info and manifest policy");
  const ScopedEnvVar clear_override("WAXCPP_LIBTORCH_MANIFEST", std::nullopt);
  const ScopedEnvVar clear_require("WAXCPP_REQUIRE_LIBTORCH_MANIFEST", std::nullopt);
  const ScopedEnvVar clear_runtime("WAXCPP_TORCH_RUNTIME", std::nullopt);
  const ScopedEnvVar clear_cuda_runtime("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::nullopt);

  {
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(!info.libtorch_runtime_compiled, "default test build should not have libtorch runtime compiled");
    Require(!info.libtorch_runtime_enabled, "default test build should not enable real libtorch runtime");
    Require(info.fallback_active, "fallback backend should remain active in current build");
    Require(info.runtime_policy == "cpu_only", "default torch runtime policy should be cpu_only");
    Require(!info.cuda_preferred_requested, "default runtime should not request cuda");
    Require(!info.cuda_runtime_available, "cuda runtime should be unavailable in fallback build");
    Require(info.selected_backend == "fallback_cpu", "fallback backend should report fallback_cpu");
    if (info.libtorch_manifest_detected) {
      Require(info.libtorch_manifest_path.has_value(), "manifest path should be present when detected");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "selected artifact path should be present when manifest is detected");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "selected artifact sha256 should be present when manifest is detected");
      Require(info.libtorch_selected_artifact_class.has_value(),
              "selected artifact class should be present when manifest is detected");
      Require(!info.libtorch_selected_artifact_sha256_verified,
              "artifact sha256 should not be verified unless explicit checksum gate is enabled");
    } else {
      Require(!info.libtorch_selected_artifact_path.has_value(),
              "selected artifact path should be empty without detected manifest");
      Require(!info.libtorch_selected_artifact_sha256.has_value(),
              "selected artifact sha256 should be empty without detected manifest");
      Require(!info.libtorch_selected_artifact_class.has_value(),
              "selected artifact class should be empty without detected manifest");
      Require(!info.libtorch_selected_artifact_resolved_path.has_value(),
              "resolved artifact path should be empty without detected manifest");
      Require(!info.libtorch_selected_artifact_sha256_verified,
              "artifact sha256 verification must remain false without manifest");
    }
    Require(!info.libtorch_script_module_path.has_value(),
            "script module path should be empty when WAXCPP_TORCH_SCRIPT_MODULE is unset");
    Require(!info.libtorch_script_module_loaded,
            "script module loaded flag should be false at construction");
  }

  {
    const ScopedEnvVar require_real("WAXCPP_REQUIRE_REAL_TORCH_RUNTIME", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "strict real-runtime policy should fail when libtorch runtime is not compiled");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.runtime_policy == "cuda_preferred", "runtime policy should reflect cuda_preferred override");
    Require(info.cuda_preferred_requested, "cuda_preferred override should set request flag");
    Require(info.selected_backend == "fallback_cpu", "fallback build should keep fallback_cpu backend");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.cuda_runtime_available, "assumed CUDA runtime should be reflected in runtime info");
    const bool manifest_blocks_cuda =
        info.libtorch_manifest_detected && info.libtorch_manifest_cuda_artifact_count == 0;
    Require(info.selected_backend == (manifest_blocks_cuda ? "fallback_cpu" : "fallback_cuda"),
            "cuda_preferred backend must respect detected manifest CUDA artifact availability");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("CPU_ONLY"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.runtime_policy == "cpu_only", "runtime policy parsing should be case-insensitive");
    Require(!info.cuda_preferred_requested, "cpu_only should clear cuda request flag");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("gpu_auto"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "invalid torch runtime policy should be rejected");
  }

  const auto temp_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_runtime_info.json";
  const auto empty_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_empty.json";
  const auto malformed_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_malformed.json";
  const auto bad_sha_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_bad_sha.json";
  const auto split_fields_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_split_fields.json";
  const auto nested_fields_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_nested_fields.json";
  const auto nested_plus_top_level_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_nested_plus_top_level.json";
  const auto cpu_cuda_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_cpu_cuda.json";
  const auto alias_fields_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_alias_fields.json";
  const auto cu_tag_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_cu_tag.json";
  const auto multi_cuda_manifest_a =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_multi_cuda_a.json";
  const auto multi_cuda_manifest_b =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_multi_cuda_b.json";
  const auto multi_cpu_manifest_a =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_multi_cpu_a.json";
  const auto multi_cpu_manifest_b =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_multi_cpu_b.json";
  const auto generic_manifest_a =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_generic_a.json";
  const auto generic_manifest_b =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_generic_b.json";
  const auto duplicate_path_manifest_a =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_duplicate_path_a.json";
  const auto duplicate_path_manifest_b =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_duplicate_path_b.json";
  const auto root_array_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_root_array.json";
  const auto artifact_root =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_dist_root";
  const auto artifact_file = artifact_root / "cpu" / "libtorch-cpu.zip";
  const auto artifact_manifest_dir = artifact_root / "manifest";
  const auto artifact_escape_file = artifact_root.parent_path() / "waxcpp_test_libtorch_escape.bin";
  const auto artifact_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_artifact.json";
  const auto artifact_escaped_slash_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_escaped_slash_artifact.json";
  const auto artifact_unicode_slash_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_unicode_slash_artifact.json";
  const auto artifact_bad_unicode_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_bad_unicode_artifact.json";
  const auto artifact_control_path_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_control_path_artifact.json";
  const auto artifact_bad_sha_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_artifact_bad_sha.json";
  const auto artifact_escape_manifest = artifact_manifest_dir / "manifest_escape.json";
  const auto artifact_absolute_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_absolute_artifact.json";
  const auto artifact_absolute_in_root_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_absolute_in_root_artifact.json";
  const auto manifest_local_artifact =
      std::filesystem::temp_directory_path() / "waxcpp_test_manifest_local_only.bin";
  const auto artifact_dist_root_strict_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_dist_root_strict.json";
  const auto artifact_empty_file = artifact_root / "empty" / "libtorch-empty.zip";
  const auto artifact_empty_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_with_empty_artifact.json";
  {
    std::ofstream out(temp_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create temp manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream out(empty_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create empty manifest file");
    }
  }
  {
    std::ofstream out(malformed_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create malformed manifest file");
    }
    out << "not-a-json-manifest";
  }
  {
    std::ofstream out(bad_sha_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create bad-sha manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"1234"}]})";
  }
  {
    std::ofstream out(split_fields_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create split-fields manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip"},{"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream out(nested_fields_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create nested-fields manifest file");
    }
    out << R"({"artifacts":[{"meta":{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}}]})";
  }
  {
    std::ofstream out(nested_plus_top_level_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create nested-plus-top-level manifest file");
    }
    out << R"({"artifacts":[{"meta":{"path":"ignored-nested.zip","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream out(cpu_cuda_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create cpu-cuda manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"},{"path":"libtorch-cuda121.zip","sha256":"1111111111111111111111111111111111111111111111111111111111111111"}]})";
  }
  {
    std::ofstream out(alias_fields_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create alias-fields manifest file");
    }
    out << R"({"files":[{"file":"libtorch-cuda124.zip","sha256sum":"2222222222222222222222222222222222222222222222222222222222222222"}]})";
  }
  {
    std::ofstream out(cu_tag_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create cu-tag manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cu124.zip","sha256":"4444444444444444444444444444444444444444444444444444444444444444"}]})";
  }
  {
    std::ofstream out(multi_cuda_manifest_a, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create multi-cuda manifest A file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cuda121.zip","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{"path":"libtorch-cu118.zip","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}]})";
  }
  {
    std::ofstream out(multi_cuda_manifest_b, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create multi-cuda manifest B file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cu118.zip","sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},{"path":"libtorch-cuda121.zip","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]})";
  }
  {
    std::ofstream out(multi_cpu_manifest_a, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create multi-cpu manifest A file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"},{"path":"libtorch-cpu-avx2.zip","sha256":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"}]})";
  }
  {
    std::ofstream out(multi_cpu_manifest_b, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create multi-cpu manifest B file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu-avx2.zip","sha256":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},{"path":"libtorch-cpu.zip","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}]})";
  }
  {
    std::ofstream out(generic_manifest_a, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create generic manifest A file");
    }
    out << R"({"artifacts":[{"path":"libtorch-z.tar","sha256":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"},{"path":"libtorch-a.tar","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}]})";
  }
  {
    std::ofstream out(generic_manifest_b, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create generic manifest B file");
    }
    out << R"({"artifacts":[{"path":"libtorch-a.tar","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},{"path":"libtorch-z.tar","sha256":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}]})";
  }
  {
    std::ofstream out(duplicate_path_manifest_a, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create duplicate-path manifest A file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream out(duplicate_path_manifest_b, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create duplicate-path manifest B file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"},{"path":"libtorch-cpu.zip","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"}]})";
  }
  {
    std::ofstream out(root_array_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create root-array manifest file");
    }
    out << R"([{"path":"libtorch-cpu.zip","sha256":"3333333333333333333333333333333333333333333333333333333333333333"}])";
  }
  {
    std::error_code mkdir_ec;
    std::filesystem::create_directories(artifact_file.parent_path(), mkdir_ec);
    if (mkdir_ec) {
      throw std::runtime_error("failed to create test artifact directory");
    }
    std::filesystem::create_directories(artifact_manifest_dir, mkdir_ec);
    if (mkdir_ec) {
      throw std::runtime_error("failed to create test artifact manifest directory");
    }
    std::ofstream artifact_out(artifact_file, std::ios::binary | std::ios::trunc);
    if (!artifact_out.is_open()) {
      throw std::runtime_error("failed to create test artifact file");
    }
    artifact_out << "abc";
    std::filesystem::create_directories(artifact_empty_file.parent_path(), mkdir_ec);
    if (mkdir_ec) {
      throw std::runtime_error("failed to create test empty artifact directory");
    }
    std::ofstream empty_out(artifact_empty_file, std::ios::binary | std::ios::trunc);
    if (!empty_out.is_open()) {
      throw std::runtime_error("failed to create empty artifact file");
    }
  }
  {
    std::ofstream out(artifact_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"cpu/libtorch-cpu.zip","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_escaped_slash_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create escaped-slash artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"cpu\/libtorch-cpu.zip","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_unicode_slash_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create unicode-slash artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"cpu\u002flibtorch-cpu.zip","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_bad_unicode_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create bad-unicode artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"cpu\u00ZZlibtorch-cpu.zip","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_control_path_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create control-path artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"cpu\nlibtorch-cpu.zip","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_bad_sha_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create bad artifact sha manifest file");
    }
    out << R"({"artifacts":[{"path":"cpu/libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream outside_out(artifact_escape_file, std::ios::binary | std::ios::trunc);
    if (!outside_out.is_open()) {
      throw std::runtime_error("failed to create escape artifact file");
    }
    outside_out << "abc";
  }
  {
    std::ofstream out(artifact_escape_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create escape artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"../waxcpp_test_libtorch_escape.bin","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_absolute_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create absolute artifact manifest file");
    }
    out << "{\"artifacts\":[{\"path\":\""
        << artifact_escape_file.generic_string()
        << "\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}]}";
  }
  {
    std::ofstream out(artifact_absolute_in_root_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create absolute in-root artifact manifest file");
    }
    out << "{\"artifacts\":[{\"path\":\""
        << artifact_file.generic_string()
        << "\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}]}";
  }
  {
    std::ofstream out(manifest_local_artifact, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create manifest-local artifact file");
    }
    out << "abc";
  }
  {
    std::ofstream out(artifact_dist_root_strict_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create dist-root strict manifest file");
    }
    out << R"({"artifacts":[{"path":"waxcpp_test_manifest_local_only.bin","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
  }
  {
    std::ofstream out(artifact_empty_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create empty artifact manifest file");
    }
    out << R"({"artifacts":[{"path":"empty/libtorch-empty.zip","sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}]})";
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", temp_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_detected, "manifest override should be detected");
    Require(info.libtorch_manifest_valid, "valid manifest override should pass validation");
    Require(info.libtorch_manifest_artifact_count > 0, "valid manifest should report artifact count");
    Require(info.libtorch_manifest_cpu_artifact_count == 1, "single cpu manifest should report one cpu artifact");
    Require(info.libtorch_manifest_cuda_artifact_count == 0, "single cpu manifest should report zero cuda artifacts");
    Require(info.libtorch_manifest_path.has_value(), "manifest override path should be preserved");
    Require(*info.libtorch_manifest_path == std::filesystem::absolute(temp_manifest).string(),
            "manifest override absolute path mismatch");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "single cpu manifest should select cpu artifact path");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cpu.zip",
            "single cpu manifest selected artifact mismatch");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "single cpu manifest should select cpu artifact sha256");
    Require(*info.libtorch_selected_artifact_sha256 ==
                "0000000000000000000000000000000000000000000000000000000000000000",
            "single cpu manifest selected artifact sha256 mismatch");
    Require(info.libtorch_selected_artifact_class.has_value() &&
                *info.libtorch_selected_artifact_class == "cpu",
            "single cpu manifest selected artifact class mismatch");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", cpu_cuda_manifest.string());
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "cpu-cuda manifest should pass validation");
    Require(info.libtorch_manifest_artifact_count == 2, "cpu-cuda manifest should report two valid artifacts");
    Require(info.libtorch_manifest_cpu_artifact_count == 1, "cpu-cuda manifest should report one cpu artifact");
    Require(info.libtorch_manifest_cuda_artifact_count == 1, "cpu-cuda manifest should report one cuda artifact");
    Require(info.cuda_preferred_requested, "cuda-preferred runtime should remain requested with mixed manifest");
    Require(info.selected_backend == "fallback_cuda",
            "cuda-preferred runtime with CUDA artifacts and runtime availability should choose fallback_cuda");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "cpu-cuda manifest should select cuda artifact in fallback_cuda mode");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cuda121.zip",
            "cpu-cuda manifest selected cuda artifact mismatch");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "cpu-cuda manifest should select cuda artifact sha256");
    Require(*info.libtorch_selected_artifact_sha256 ==
                "1111111111111111111111111111111111111111111111111111111111111111",
            "cpu-cuda manifest selected cuda artifact sha256 mismatch");
    Require(info.libtorch_selected_artifact_class.has_value() &&
                *info.libtorch_selected_artifact_class == "cuda",
            "cpu-cuda manifest selected artifact class mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", cpu_cuda_manifest.string());
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));

    std::vector<float> cpu_embedding{};
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cpu_only"));
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cpu",
              "cpu_only policy should route to fallback_cpu backend");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "cpu_only policy should select cpu artifact path");
      Require(*info.libtorch_selected_artifact_path == "libtorch-cpu.zip",
              "cpu_only policy should select cpu artifact");
      cpu_embedding = embedder.Embed("policy parity text");
    }

    std::vector<float> cuda_embedding{};
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cuda",
              "cuda_preferred policy with available CUDA should route to fallback_cuda backend");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "cuda_preferred policy should select cuda artifact path");
      Require(*info.libtorch_selected_artifact_path == "libtorch-cuda121.zip",
              "cuda_preferred policy should select cuda artifact");
      cuda_embedding = embedder.Embed("policy parity text");
    }

    Require(cpu_embedding == cuda_embedding,
            "fallback embeddings should remain deterministic across cpu_only and cuda_preferred policies");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cpu_only"));
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", cpu_cuda_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.cuda_runtime_available, "assumed CUDA runtime should be reflected in runtime info");
    Require(info.libtorch_manifest_cuda_artifact_count == 1, "cpu-cuda manifest should still report cuda artifacts");
    Require(info.selected_backend == "fallback_cpu",
            "cpu_only runtime policy must keep fallback_cpu even when CUDA is available");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "cpu_only runtime should still select cpu artifact from mixed manifest");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cpu.zip",
            "cpu_only runtime selected artifact mismatch");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "cpu_only runtime should select cpu artifact sha256");
    Require(*info.libtorch_selected_artifact_sha256 ==
                "0000000000000000000000000000000000000000000000000000000000000000",
            "cpu_only runtime selected artifact sha256 mismatch");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", cpu_cuda_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "cpu-cuda manifest should pass validation");
    Require(!info.cuda_runtime_available, "default runtime should not assume CUDA availability");
    Require(info.selected_backend == "fallback_cpu",
            "cuda-preferred runtime without available CUDA must keep fallback_cpu backend");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "cuda-preferred without runtime availability should select cpu artifact");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cpu.zip",
            "fallback_cpu runtime selected artifact mismatch");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST",
                                    temp_manifest.string() + ".definitely-missing-runtime-manifest");
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(!info.libtorch_manifest_detected, "missing override path should not mark manifest as detected");
    Require(info.selected_backend == "fallback_cuda",
            "cuda-preferred runtime with CUDA availability and no detected manifest should use fallback_cuda");
    Require(!info.libtorch_selected_artifact_path.has_value(),
            "selected artifact path should be empty when manifest override is missing");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", temp_manifest.string());
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "cpu-only manifest should pass validation");
    Require(info.libtorch_manifest_cuda_artifact_count == 0, "cpu-only manifest should report zero cuda artifacts");
    Require(info.selected_backend == "fallback_cpu",
            "cuda-preferred runtime should fall back to CPU when manifest lacks CUDA artifacts");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "cpu-only manifest should select cpu artifact path");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cpu.zip",
            "cpu-only manifest selected artifact mismatch");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", alias_fields_manifest.string());
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "alias-fields manifest should pass validation");
    Require(info.libtorch_manifest_artifact_count == 1, "alias-fields manifest should report one artifact");
    Require(info.libtorch_manifest_cpu_artifact_count == 0, "alias-fields manifest should report zero cpu artifacts");
    Require(info.libtorch_manifest_cuda_artifact_count == 1, "alias-fields manifest should report one cuda artifact");
    Require(info.selected_backend == "fallback_cuda",
            "cuda-preferred runtime should accept file/sha256sum alias fields for cuda routing");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "alias-fields manifest should select cuda artifact path");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cuda124.zip",
            "alias-fields selected artifact mismatch");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "alias-fields manifest should select cuda artifact sha256");
    Require(*info.libtorch_selected_artifact_sha256 ==
                "2222222222222222222222222222222222222222222222222222222222222222",
            "alias-fields selected artifact sha256 mismatch");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", cu_tag_manifest.string());
    const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "cu-tag manifest should pass validation");
    Require(info.libtorch_manifest_artifact_count == 1, "cu-tag manifest should report one artifact");
    Require(info.libtorch_manifest_cpu_artifact_count == 0, "cu-tag manifest should report zero cpu artifacts");
    Require(info.libtorch_manifest_cuda_artifact_count == 1, "cu-tag manifest should report one cuda artifact");
    Require(info.selected_backend == "fallback_cuda",
            "cu-tag manifest should route cuda_preferred runtime to fallback_cuda backend");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "cu-tag manifest should select cuda artifact path");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cu124.zip",
            "cu-tag manifest selected artifact mismatch");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "cu-tag manifest should select cuda artifact sha256");
    Require(*info.libtorch_selected_artifact_sha256 ==
                "4444444444444444444444444444444444444444444444444444444444444444",
            "cu-tag manifest selected artifact sha256 mismatch");
  }

  {
    std::optional<std::string> selected_a{};
    std::optional<std::string> selected_b{};
    std::optional<std::string> selected_sha_a{};
    std::optional<std::string> selected_sha_b{};
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", multi_cuda_manifest_a.string());
      const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cuda",
              "multi-cuda manifest A should route to fallback_cuda");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "multi-cuda manifest A should select cuda artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "multi-cuda manifest A should select cuda artifact sha256");
      selected_a = info.libtorch_selected_artifact_path;
      selected_sha_a = info.libtorch_selected_artifact_sha256;
    }
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", multi_cuda_manifest_b.string());
      const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cuda",
              "multi-cuda manifest B should route to fallback_cuda");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "multi-cuda manifest B should select cuda artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "multi-cuda manifest B should select cuda artifact sha256");
      selected_b = info.libtorch_selected_artifact_path;
      selected_sha_b = info.libtorch_selected_artifact_sha256;
    }
    Require(selected_a.has_value() && selected_b.has_value() &&
                selected_sha_a.has_value() && selected_sha_b.has_value(),
            "multi-cuda manifests should produce selected artifact path");
    Require(*selected_a == *selected_b,
            "selected cuda artifact path should be deterministic regardless of manifest entry order");
    Require(*selected_sha_a == *selected_sha_b,
            "selected cuda artifact sha256 should be deterministic regardless of manifest entry order");
    Require(*selected_a == "libtorch-cu118.zip",
            "multi-cuda deterministic selection should pick lexicographically smallest path");
    Require(*selected_sha_a == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "multi-cuda deterministic selection should pick matching sha256");
  }

  {
    std::optional<std::string> selected_a{};
    std::optional<std::string> selected_b{};
    std::optional<std::string> selected_sha_a{};
    std::optional<std::string> selected_sha_b{};
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cpu_only"));
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", multi_cpu_manifest_a.string());
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cpu",
              "multi-cpu manifest A should route to fallback_cpu");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "multi-cpu manifest A should select cpu artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "multi-cpu manifest A should select cpu artifact sha256");
      selected_a = info.libtorch_selected_artifact_path;
      selected_sha_a = info.libtorch_selected_artifact_sha256;
    }
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cpu_only"));
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", multi_cpu_manifest_b.string());
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cpu",
              "multi-cpu manifest B should route to fallback_cpu");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "multi-cpu manifest B should select cpu artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "multi-cpu manifest B should select cpu artifact sha256");
      selected_b = info.libtorch_selected_artifact_path;
      selected_sha_b = info.libtorch_selected_artifact_sha256;
    }
    Require(selected_a.has_value() && selected_b.has_value() &&
                selected_sha_a.has_value() && selected_sha_b.has_value(),
            "multi-cpu manifests should produce selected artifact path");
    Require(*selected_a == *selected_b,
            "selected cpu artifact path should be deterministic regardless of manifest entry order");
    Require(*selected_sha_a == *selected_sha_b,
            "selected cpu artifact sha256 should be deterministic regardless of manifest entry order");
    Require(*selected_a == "libtorch-cpu-avx2.zip",
            "multi-cpu deterministic selection should pick lexicographically smallest path");
    Require(*selected_sha_a == "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            "multi-cpu deterministic selection should pick matching sha256");
  }

  {
    std::optional<std::string> selected_a{};
    std::optional<std::string> selected_b{};
    std::optional<std::string> selected_sha_a{};
    std::optional<std::string> selected_sha_b{};
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", generic_manifest_a.string());
      const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cpu",
              "generic manifest without cuda tags should keep fallback_cpu backend");
      Require(info.libtorch_manifest_cpu_artifact_count == 0 && info.libtorch_manifest_cuda_artifact_count == 0,
              "generic manifest should report zero cpu/cuda classified artifacts");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "generic manifest A should select fallback any-artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "generic manifest A should select fallback any-artifact sha256");
      Require(info.libtorch_selected_artifact_class.has_value() &&
                  *info.libtorch_selected_artifact_class == "any",
              "generic manifest A selected artifact class mismatch");
      selected_a = info.libtorch_selected_artifact_path;
      selected_sha_a = info.libtorch_selected_artifact_sha256;
    }
    {
      const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", generic_manifest_b.string());
      const ScopedEnvVar assume_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.selected_backend == "fallback_cpu",
              "generic manifest without cuda tags should keep fallback_cpu backend");
      Require(info.libtorch_manifest_cpu_artifact_count == 0 && info.libtorch_manifest_cuda_artifact_count == 0,
              "generic manifest should report zero cpu/cuda classified artifacts");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "generic manifest B should select fallback any-artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "generic manifest B should select fallback any-artifact sha256");
      Require(info.libtorch_selected_artifact_class.has_value() &&
                  *info.libtorch_selected_artifact_class == "any",
              "generic manifest B selected artifact class mismatch");
      selected_b = info.libtorch_selected_artifact_path;
      selected_sha_b = info.libtorch_selected_artifact_sha256;
    }
    Require(selected_a.has_value() && selected_b.has_value() &&
                selected_sha_a.has_value() && selected_sha_b.has_value(),
            "generic manifests should produce selected artifact path");
    Require(*selected_a == *selected_b,
            "generic any-artifact selection should be deterministic regardless of manifest entry order");
    Require(*selected_sha_a == *selected_sha_b,
            "generic any-artifact sha256 should be deterministic regardless of manifest entry order");
    Require(*selected_a == "libtorch-a.tar",
            "generic any-artifact deterministic selection should pick lexicographically smallest path");
    Require(*selected_sha_a == "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            "generic any-artifact deterministic selection should pick matching sha256");
  }

  {
    std::optional<std::string> selected_a{};
    std::optional<std::string> selected_b{};
    std::optional<std::string> selected_sha_a{};
    std::optional<std::string> selected_sha_b{};
    {
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", duplicate_path_manifest_a.string());
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.libtorch_manifest_valid, "duplicate-path manifest A should pass validation");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "duplicate-path manifest A should select artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "duplicate-path manifest A should select artifact sha256");
      selected_a = info.libtorch_selected_artifact_path;
      selected_sha_a = info.libtorch_selected_artifact_sha256;
    }
    {
      const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", duplicate_path_manifest_b.string());
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.libtorch_manifest_valid, "duplicate-path manifest B should pass validation");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "duplicate-path manifest B should select artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "duplicate-path manifest B should select artifact sha256");
      selected_b = info.libtorch_selected_artifact_path;
      selected_sha_b = info.libtorch_selected_artifact_sha256;
    }
    Require(selected_a.has_value() && selected_b.has_value() &&
                selected_sha_a.has_value() && selected_sha_b.has_value(),
            "duplicate-path manifests should produce selected artifact path+sha");
    Require(*selected_a == *selected_b,
            "duplicate-path selection should be deterministic across entry order");
    Require(*selected_sha_a == *selected_sha_b,
            "duplicate-path sha selection should be deterministic across entry order");
    Require(*selected_a == "libtorch-cpu.zip",
            "duplicate-path manifest should keep expected artifact path");
    Require(*selected_sha_a == "0000000000000000000000000000000000000000000000000000000000000000",
            "duplicate-path manifest should prefer lexicographically minimal sha for equal path");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", root_array_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "root-array manifest should pass validation");
    Require(info.libtorch_manifest_artifact_count == 1, "root-array manifest should report one artifact");
    Require(info.libtorch_manifest_cpu_artifact_count == 1, "root-array manifest should report one cpu artifact");
    Require(info.libtorch_manifest_cuda_artifact_count == 0, "root-array manifest should report zero cuda artifacts");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "root-array manifest should select artifact path");
    Require(*info.libtorch_selected_artifact_path == "libtorch-cpu.zip",
            "root-array selected artifact mismatch");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "root-array manifest should select artifact sha256");
    Require(*info.libtorch_selected_artifact_sha256 ==
                "3333333333333333333333333333333333333333333333333333333333333333",
            "root-array selected artifact sha256 mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", empty_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "empty manifest should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", malformed_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "malformed manifest should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", bad_sha_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest with invalid sha256 should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", split_fields_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest must reject artifacts where path and sha are split across different objects");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", nested_fields_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest must reject artifacts with path/sha only in nested fields");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", nested_plus_top_level_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "manifest should accept top-level path/sha even with nested metadata");
    Require(info.libtorch_manifest_artifact_count == 1, "expected one valid artifact object in nested-plus-top-level manifest");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", temp_manifest.string() + ".missing");
    const ScopedEnvVar require_manifest("WAXCPP_REQUIRE_LIBTORCH_MANIFEST", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "required manifest policy should throw when override path is missing");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "artifact manifest should pass validation");
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "artifact manifest should resolve selected artifact path");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "artifact sha256 must be verified when checksum gate is enabled");
    Require(*info.libtorch_selected_artifact_resolved_path == std::filesystem::absolute(artifact_file).string(),
            "resolved artifact path mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_escaped_slash_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "escaped-slash artifact manifest should pass validation");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "escaped-slash artifact manifest should select artifact path");
    Require(*info.libtorch_selected_artifact_path == "cpu/libtorch-cpu.zip",
            "escaped-slash artifact manifest selected path mismatch");
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "escaped-slash artifact manifest should resolve selected artifact path");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "escaped-slash artifact manifest should pass checksum gate");
    Require(*info.libtorch_selected_artifact_resolved_path == std::filesystem::absolute(artifact_file).string(),
            "escaped-slash artifact manifest resolved path mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_unicode_slash_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "unicode-slash artifact manifest should pass validation");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "unicode-slash artifact manifest should select artifact path");
    Require(*info.libtorch_selected_artifact_path == "cpu/libtorch-cpu.zip",
            "unicode-slash artifact manifest selected path mismatch");
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "unicode-slash artifact manifest should resolve selected artifact path");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "unicode-slash artifact manifest should pass checksum gate");
    Require(*info.libtorch_selected_artifact_resolved_path == std::filesystem::absolute(artifact_file).string(),
            "unicode-slash artifact manifest resolved path mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_bad_unicode_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest with malformed unicode escape in artifact path should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_control_path_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest with decoded control characters in artifact path should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_manifest.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "checksum gate should throw when selected artifact file cannot be resolved");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_bad_sha_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "checksum gate should throw on selected artifact sha256 mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_escape_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "checksum gate should reject selected artifact path traversal outside dist root");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_absolute_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "checksum gate should reject absolute artifact paths outside dist root");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_absolute_in_root_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "absolute in-root artifact should resolve when dist root is set");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "absolute in-root artifact should pass checksum verification when dist root is set");
    Require(*info.libtorch_selected_artifact_resolved_path == std::filesystem::absolute(artifact_file).string(),
            "absolute in-root artifact resolved path mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_absolute_manifest.string());
    const ScopedEnvVar clear_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", std::nullopt);
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "absolute artifact manifest should resolve selected artifact path when dist root is unset");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "absolute artifact manifest should pass checksum verification when dist root is unset");
    Require(*info.libtorch_selected_artifact_resolved_path == std::filesystem::absolute(artifact_escape_file).string(),
            "absolute artifact manifest resolved path mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_dist_root_strict_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw,
            "when dist root is set, manifest-local artifact fallback must be disabled");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_empty_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "checksum gate should reject empty selected artifact file");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", artifact_manifest.string());
    const ScopedEnvVar set_dist_root("WAXCPP_LIBTORCH_DIST_ROOT", artifact_root.string());
    const ScopedEnvVar require_artifact_sha("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256", std::nullopt);
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "resolved artifact path should still be populated without checksum gate");
    Require(!info.libtorch_selected_artifact_sha256_verified,
            "artifact sha256 should stay unverified when checksum gate is disabled");
  }

  std::error_code ec;
  std::filesystem::remove(temp_manifest, ec);
  std::filesystem::remove(empty_manifest, ec);
  std::filesystem::remove(malformed_manifest, ec);
  std::filesystem::remove(bad_sha_manifest, ec);
  std::filesystem::remove(split_fields_manifest, ec);
  std::filesystem::remove(nested_fields_manifest, ec);
  std::filesystem::remove(nested_plus_top_level_manifest, ec);
  std::filesystem::remove(cpu_cuda_manifest, ec);
  std::filesystem::remove(alias_fields_manifest, ec);
  std::filesystem::remove(cu_tag_manifest, ec);
  std::filesystem::remove(multi_cuda_manifest_a, ec);
  std::filesystem::remove(multi_cuda_manifest_b, ec);
  std::filesystem::remove(multi_cpu_manifest_a, ec);
  std::filesystem::remove(multi_cpu_manifest_b, ec);
  std::filesystem::remove(generic_manifest_a, ec);
  std::filesystem::remove(generic_manifest_b, ec);
  std::filesystem::remove(duplicate_path_manifest_a, ec);
  std::filesystem::remove(duplicate_path_manifest_b, ec);
  std::filesystem::remove(root_array_manifest, ec);
  std::filesystem::remove(artifact_manifest, ec);
  std::filesystem::remove(artifact_escaped_slash_manifest, ec);
  std::filesystem::remove(artifact_unicode_slash_manifest, ec);
  std::filesystem::remove(artifact_bad_unicode_manifest, ec);
  std::filesystem::remove(artifact_control_path_manifest, ec);
  std::filesystem::remove(artifact_bad_sha_manifest, ec);
  std::filesystem::remove(artifact_escape_manifest, ec);
  std::filesystem::remove(artifact_absolute_manifest, ec);
  std::filesystem::remove(artifact_absolute_in_root_manifest, ec);
  std::filesystem::remove(artifact_dist_root_strict_manifest, ec);
  std::filesystem::remove(artifact_empty_manifest, ec);
  std::filesystem::remove(manifest_local_artifact, ec);
  std::filesystem::remove(artifact_escape_file, ec);
  std::filesystem::remove(artifact_empty_file, ec);
  std::filesystem::remove(artifact_empty_file.parent_path(), ec);
  std::filesystem::remove(artifact_file, ec);
  std::filesystem::remove(artifact_file.parent_path(), ec);
  std::filesystem::remove(artifact_manifest_dir, ec);
  std::filesystem::remove(artifact_root, ec);
}

void ScenarioConcurrentEmbedThreadSafety() {
  waxcpp::tests::Log("scenario: concurrent embed thread safety");
  waxcpp::MiniLMEmbedderTorch embedder(32);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> workers{};
  workers.reserve(kThreads);
  for (int thread_id = 0; thread_id < kThreads; ++thread_id) {
    workers.emplace_back([&embedder, thread_id]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string text = (i % 3 == 0)
                                     ? "shared-key"
                                     : ("t" + std::to_string(thread_id) + "-i" + std::to_string(i % 20));
        const auto vec = embedder.Embed(text);
        if (vec.size() != static_cast<std::size_t>(embedder.dimensions())) {
          throw std::runtime_error("concurrent embed produced invalid shape");
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto a = embedder.Embed("shared-key");
  const auto b = embedder.Embed("shared-key");
  Require(a == b, "concurrent memoization must remain deterministic for same key");
  Require(embedder.cache_size() <= 32, "concurrent memoization must respect capacity bound");
}

void ScenarioRuntimeInfoSnapshotStability() {
  waxcpp::tests::Log("scenario: runtime info snapshot stability");
  const ScopedEnvVar clear_override("WAXCPP_LIBTORCH_MANIFEST", std::nullopt);
  const ScopedEnvVar clear_require("WAXCPP_REQUIRE_LIBTORCH_MANIFEST", std::nullopt);
  const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
  const ScopedEnvVar set_cuda("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE", std::string("1"));

  waxcpp::MiniLMEmbedderTorch embedder;
  const auto before = embedder.runtime_info();
  (void)embedder.Embed("runtime info stability text");
  const auto after_single = embedder.runtime_info();
  (void)embedder.EmbedBatch({"runtime", "info", "stability"});
  const auto after_batch = embedder.runtime_info();

  Require(before.runtime_policy == after_single.runtime_policy &&
              after_single.runtime_policy == after_batch.runtime_policy,
          "runtime policy should remain stable after embedding calls");
  Require(before.selected_backend == after_single.selected_backend &&
              after_single.selected_backend == after_batch.selected_backend,
          "selected backend should remain stable after embedding calls");
  Require(before.libtorch_manifest_detected == after_single.libtorch_manifest_detected &&
              after_single.libtorch_manifest_detected == after_batch.libtorch_manifest_detected,
          "manifest detection flag should remain stable after embedding calls");
  Require(before.libtorch_manifest_artifact_count == after_single.libtorch_manifest_artifact_count &&
              after_single.libtorch_manifest_artifact_count == after_batch.libtorch_manifest_artifact_count,
          "manifest artifact count should remain stable after embedding calls");
  Require(before.libtorch_selected_artifact_path == after_single.libtorch_selected_artifact_path &&
              after_single.libtorch_selected_artifact_path == after_batch.libtorch_selected_artifact_path,
          "selected artifact path should remain stable after embedding calls");
  Require(before.libtorch_selected_artifact_sha256 == after_single.libtorch_selected_artifact_sha256 &&
              after_single.libtorch_selected_artifact_sha256 == after_batch.libtorch_selected_artifact_sha256,
          "selected artifact sha256 should remain stable after embedding calls");
  Require(before.libtorch_selected_artifact_class == after_single.libtorch_selected_artifact_class &&
              after_single.libtorch_selected_artifact_class == after_batch.libtorch_selected_artifact_class,
          "selected artifact class should remain stable after embedding calls");
  Require(before.libtorch_selected_artifact_resolved_path == after_single.libtorch_selected_artifact_resolved_path &&
              after_single.libtorch_selected_artifact_resolved_path == after_batch.libtorch_selected_artifact_resolved_path,
          "selected artifact resolved path should remain stable after embedding calls");
  Require(before.libtorch_selected_artifact_sha256_verified == after_single.libtorch_selected_artifact_sha256_verified &&
              after_single.libtorch_selected_artifact_sha256_verified == after_batch.libtorch_selected_artifact_sha256_verified,
          "selected artifact checksum verification flag should remain stable after embedding calls");
}

void ScenarioManifestParserFuzzDeterminism() {
  waxcpp::tests::Log("scenario: manifest parser fuzz determinism");
  const auto fuzz_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_fuzz.json";
  const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", fuzz_manifest.string());
  const ScopedEnvVar clear_require("WAXCPP_REQUIRE_LIBTORCH_MANIFEST", std::nullopt);

  constexpr std::size_t kIterations = 256;
  std::mt19937 rng(0x57A5BEEF);
  std::uniform_int_distribution<int> len_dist(0, 512);
  std::uniform_int_distribution<int> ascii_dist(32, 126);

  const std::string valid_seed =
      R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";

  for (std::size_t i = 0; i < kIterations; ++i) {
    std::string payload{};
    if ((i % 4U) == 0U) {
      payload = valid_seed;
      if (!payload.empty()) {
        const std::size_t flips = 1U + (i % 7U);
        for (std::size_t flip = 0; flip < flips; ++flip) {
          const std::size_t idx = static_cast<std::size_t>(rng() % payload.size());
          payload[idx] = static_cast<char>(ascii_dist(rng));
        }
      }
    } else {
      payload.resize(static_cast<std::size_t>(len_dist(rng)));
      for (auto& ch : payload) {
        ch = static_cast<char>(ascii_dist(rng));
      }
    }

    std::ofstream out(fuzz_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create fuzz manifest file");
    }
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();

    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      const auto info = embedder.runtime_info();
      Require(info.libtorch_manifest_detected, "fuzz manifest should be detected when override file exists");
      Require(info.libtorch_manifest_valid, "successful constructor must report valid manifest");
      Require(info.libtorch_manifest_artifact_count > 0, "valid manifest must expose at least one artifact");
      Require(info.libtorch_selected_artifact_path.has_value(),
              "valid fuzz manifest must resolve selected artifact path");
      Require(info.libtorch_selected_artifact_sha256.has_value(),
              "valid fuzz manifest must resolve selected artifact sha256");
    } catch (const std::exception&) {
      // Expected for malformed fuzz payloads. We only require deterministic no-crash behavior.
    }
  }

  std::error_code ec;
  std::filesystem::remove(fuzz_manifest, ec);
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("embeddings_test: start");
    ScenarioIdentityAndShape();
    ScenarioDeterministicEmbedding();
    ScenarioNormalizationAndEmptyInput();
    ScenarioBatchParity();
    ScenarioMemoizationCapacity();
    ScenarioAsciiTokenizationDeterminism();
    ScenarioRuntimeInfoAndManifestPolicy();
    ScenarioConcurrentEmbedThreadSafety();
    ScenarioRuntimeInfoSnapshotStability();
    ScenarioManifestParserFuzzDeterminism();
    waxcpp::tests::Log("embeddings_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
