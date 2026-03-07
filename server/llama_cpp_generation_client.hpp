#pragma once

#include <functional>
#include <string>

namespace waxcpp::server {

struct LlamaCppGenerationConfig {
  std::string endpoint{};
  std::string api_key{};
  std::string model_path{};
  int timeout_ms = 60000;
  int max_retries = 2;
  int retry_backoff_ms = 100;
  std::function<std::string(const std::string& body)> request_fn{};
};

struct LlamaCppGenerationRequest {
  std::string prompt{};
  std::string system_prompt{};  // Optional; when set, uses chat completions format.
  int max_tokens = 512;
  float temperature = 0.1f;
  float top_p = 0.95f;
};

class LlamaCppGenerationClient {
 public:
  explicit LlamaCppGenerationClient(LlamaCppGenerationConfig config);

  [[nodiscard]] std::string Generate(const LlamaCppGenerationRequest& request) const;

  [[nodiscard]] static std::string ParseGenerationResponse(const std::string& payload);

 private:
  [[nodiscard]] static std::string BuildRequestBody(const LlamaCppGenerationRequest& request);
  [[nodiscard]] std::string PerformRequest(const std::string& body) const;
  [[nodiscard]] std::string PerformRequestWithRetry(const std::string& body) const;

  LlamaCppGenerationConfig config_{};
};

}  // namespace waxcpp::server
