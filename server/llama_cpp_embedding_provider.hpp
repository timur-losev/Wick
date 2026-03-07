#pragma once

#include "waxcpp/embeddings.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp::server {

struct LlamaCppEmbeddingProviderConfig {
  std::string endpoint{};
  std::string api_key{};
  std::string model_path{};
  int dimensions = 1024;
  bool normalize = true;
  int timeout_ms = 30000;
  int max_retries = 2;
  int retry_backoff_ms = 100;
  int max_batch_concurrency = 4;
  std::size_t memoization_capacity = 4096;
  std::function<std::string(const std::string& body)> request_fn{};
};

class LlamaCppEmbeddingProvider final : public waxcpp::BatchEmbeddingProvider {
 public:
  explicit LlamaCppEmbeddingProvider(LlamaCppEmbeddingProviderConfig config);

  int dimensions() const override;
  bool normalize() const override;
  std::optional<waxcpp::EmbeddingIdentity> identity() const override;
  std::vector<float> Embed(const std::string& text) override;
  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override;

  [[nodiscard]] static std::vector<float> ParseEmbeddingResponse(
      const std::string& payload,
      int expected_dimensions);

 private:
  std::string RequestEmbeddingPayload(const std::string& text) const;
  std::vector<float> FetchEmbedding(const std::string& text) const;
  std::vector<float> FetchEmbeddingWithRetry(const std::string& text) const;
  std::vector<float> GetCachedEmbedding(const std::string& key) const;
  void MemoizeLocked(const std::string& key, const std::vector<float>& embedding);

  LlamaCppEmbeddingProviderConfig config_{};
  mutable std::mutex memoization_mutex_{};
  mutable std::unordered_map<std::string, std::vector<float>> memoized_embeddings_{};
  mutable std::list<std::string> memoization_order_{};
  mutable std::unordered_map<std::string, std::list<std::string>::iterator> memoization_iterators_{};
};

}  // namespace waxcpp::server
