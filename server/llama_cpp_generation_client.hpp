#pragma once

#include <functional>
#include <string>

namespace waxcpp::server {

enum class GenerationApiKind {
  kLlamaCpp = 0,
  kOpenAIResponses,
  kOpenAICompatibleChatCompletions,
};

struct LlamaCppGenerationConfig {
  GenerationApiKind api = GenerationApiKind::kLlamaCpp;
  std::string endpoint{};
  std::string api_key{};
  std::string model_path{};
  std::string reasoning_effort = "low";
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
  [[nodiscard]] std::string BuildRequestBody(const LlamaCppGenerationRequest& request) const;
  [[nodiscard]] std::string PerformRequest(const std::string& body) const;
  [[nodiscard]] std::string PerformRequestWithRetry(const std::string& body) const;

  LlamaCppGenerationConfig config_{};
};

}  // namespace waxcpp::server
