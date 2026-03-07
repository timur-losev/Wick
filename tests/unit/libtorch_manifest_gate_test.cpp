#include "waxcpp/embeddings.hpp"

#include "../test_logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

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

bool EnvIsTruthy(const char* name) {
  const auto raw = GetEnvValue(name);
  if (!raw.has_value()) {
    return false;
  }
  std::string normalized = *raw;
  for (auto& ch : normalized) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string ToAsciiLower(std::string value) {
  for (auto& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool PathElementEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
#ifdef _WIN32
  return ToAsciiLower(lhs.generic_string()) == ToAsciiLower(rhs.generic_string());
#else
  return lhs == rhs;
#endif
}

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path) {
  std::error_code ec{};
  auto normalized = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    normalized = std::filesystem::absolute(path, ec);
    if (ec) {
      return path.lexically_normal();
    }
  }
  return normalized.lexically_normal();
}

bool IsPathWithinRoot(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  const auto normalized_candidate = NormalizeAbsolutePath(candidate);
  const auto normalized_root = NormalizeAbsolutePath(root);

  if (normalized_root.empty()) {
    return false;
  }

  auto candidate_it = normalized_candidate.begin();
  auto root_it = normalized_root.begin();
  for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == normalized_candidate.end()) {
      return false;
    }
    if (!PathElementEqual(*candidate_it, *root_it)) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("libtorch_manifest_gate_test: start");
    if (!EnvIsTruthy("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256")) {
      waxcpp::tests::Log("libtorch_manifest_gate_test: skipped (WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256 is not enabled)");
      return EXIT_SUCCESS;
    }

    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();

    Require(info.libtorch_manifest_detected, "expected detected libtorch manifest");
    Require(info.libtorch_manifest_valid, "expected valid libtorch manifest");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "expected selected artifact path from manifest");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "expected selected artifact sha256 from manifest");
    Require(info.libtorch_selected_artifact_class.has_value(),
            "expected selected artifact class from manifest");
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "expected resolved selected artifact file path");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "expected selected artifact sha256 verification to be true");

    const std::filesystem::path resolved(*info.libtorch_selected_artifact_resolved_path);
    Require(std::filesystem::exists(resolved), "resolved selected artifact file does not exist");
    Require(std::filesystem::is_regular_file(resolved), "resolved selected artifact path is not a regular file");
    if (const auto dist_root = GetEnvValue("WAXCPP_LIBTORCH_DIST_ROOT"); dist_root.has_value()) {
      Require(IsPathWithinRoot(resolved, std::filesystem::path(*dist_root)),
              "resolved selected artifact path escapes configured dist root");
    }

    const auto runtime_policy = GetEnvValue("WAXCPP_TORCH_RUNTIME").value_or("cpu_only");
    if (runtime_policy == "cpu_only") {
      Require(info.selected_backend == "fallback_cpu",
              "cpu_only policy must resolve to fallback_cpu backend");
    } else if (runtime_policy == "cuda_preferred") {
      const bool should_use_cuda = info.cuda_runtime_available && info.libtorch_manifest_cuda_artifact_count > 0;
      const std::string expected_backend = should_use_cuda ? "fallback_cuda" : "fallback_cpu";
      Require(info.selected_backend == expected_backend,
              "cuda_preferred policy backend mismatch with runtime/manifest availability");
    }

    waxcpp::tests::LogKV("selected_backend", info.selected_backend);
    waxcpp::tests::LogKV("selected_artifact_path", *info.libtorch_selected_artifact_path);
    waxcpp::tests::LogKV("selected_artifact_resolved_path", *info.libtorch_selected_artifact_resolved_path);
    waxcpp::tests::LogKV("selected_artifact_class", *info.libtorch_selected_artifact_class);
    waxcpp::tests::Log("libtorch_manifest_gate_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
