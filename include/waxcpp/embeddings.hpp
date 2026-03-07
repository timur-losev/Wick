#pragma once

#include "waxcpp/types.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

struct EmbeddingIdentity {
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<int> dimensions;
  std::optional<bool> normalized;
};

struct MiniLMRuntimeInfo {
  bool fallback_active = true;
  bool cuda_preferred_requested = false;
  bool cuda_runtime_available = false;
  bool libtorch_runtime_compiled = false;
  bool libtorch_runtime_enabled = false;
  bool libtorch_runtime_strict = false;
  std::string runtime_policy = "cpu_only";
  std::string selected_backend = "fallback_cpu";
  bool libtorch_manifest_detected = false;
  bool libtorch_manifest_valid = false;
  std::size_t libtorch_manifest_artifact_count = 0;
  std::size_t libtorch_manifest_cpu_artifact_count = 0;
  std::size_t libtorch_manifest_cuda_artifact_count = 0;
  std::optional<std::string> libtorch_manifest_path;
  std::optional<std::string> libtorch_selected_artifact_path;
  std::optional<std::string> libtorch_selected_artifact_resolved_path;
  std::optional<std::string> libtorch_selected_artifact_sha256;
  std::optional<std::string> libtorch_selected_artifact_class;
  std::optional<std::string> libtorch_script_module_path;
  bool libtorch_script_module_loaded = false;
  bool libtorch_selected_artifact_sha256_verified = false;
  std::optional<std::string> libtorch_runtime_error;
};

class EmbeddingProvider {
 public:
  virtual ~EmbeddingProvider() = default;

  virtual int dimensions() const = 0;
  virtual bool normalize() const = 0;
  virtual std::optional<EmbeddingIdentity> identity() const = 0;
  virtual std::vector<float> Embed(const std::string& text) = 0;
};

class BatchEmbeddingProvider : public EmbeddingProvider {
 public:
  ~BatchEmbeddingProvider() override = default;
  virtual std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) = 0;
};

class MiniLMEmbedderTorch final : public BatchEmbeddingProvider {
 public:
  explicit MiniLMEmbedderTorch(std::size_t memoization_capacity = 4096);

  int dimensions() const override;
  bool normalize() const override;
  std::optional<EmbeddingIdentity> identity() const override;
  std::vector<float> Embed(const std::string& text) override;
  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override;
  [[nodiscard]] std::size_t cache_size() const;
  [[nodiscard]] MiniLMRuntimeInfo runtime_info() const;

 private:
  std::size_t memoization_capacity_ = 0;
  MiniLMRuntimeInfo runtime_info_{};
  std::unordered_map<std::string, std::vector<float>> memoized_embeddings_{};
  std::deque<std::string> memoization_order_{};
  mutable std::mutex memoization_mutex_{};
};

}  // namespace waxcpp
