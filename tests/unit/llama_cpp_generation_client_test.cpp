#include "../../server/llama_cpp_generation_client.hpp"

#include "../test_logger.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ScenarioParseSupportedSchemas() {
  waxcpp::tests::Log("scenario: parse supported generation schemas");
  {
    const auto text = waxcpp::server::LlamaCppGenerationClient::ParseGenerationResponse(
        R"({"content":"hello"})");
    Require(text == "hello", "content schema parse mismatch");
  }
  {
    const auto text = waxcpp::server::LlamaCppGenerationClient::ParseGenerationResponse(
        R"({"response":"world"})");
    Require(text == "world", "response schema parse mismatch");
  }
  {
    const auto text = waxcpp::server::LlamaCppGenerationClient::ParseGenerationResponse(
        R"({"choices":[{"text":"alpha"}]})");
    Require(text == "alpha", "choices text schema parse mismatch");
  }
  {
    const auto text = waxcpp::server::LlamaCppGenerationClient::ParseGenerationResponse(
        R"({"choices":[{"message":{"content":"beta"}}]})");
    Require(text == "beta", "choices message schema parse mismatch");
  }
}

void ScenarioParseRejectsMalformed() {
  waxcpp::tests::Log("scenario: parse rejects malformed payload");
  bool threw = false;
  try {
    (void)waxcpp::server::LlamaCppGenerationClient::ParseGenerationResponse(R"({"foo":"bar"})");
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "missing generation text must throw");
}

void ScenarioGenerateUsesRequestFnAndRetry() {
  waxcpp::tests::Log("scenario: generate uses request_fn and retry");
  int attempts = 0;
  std::string last_request_body{};
  waxcpp::server::LlamaCppGenerationClient client(
      waxcpp::server::LlamaCppGenerationConfig{
          .endpoint = "",
          .model_path = "g:/Proj/Agents1/Models/Qwen/Qwen3-Coder-Next-Q4_K_M.gguf",
          .timeout_ms = 1000,
          .max_retries = 2,
          .retry_backoff_ms = 1,
          .request_fn =
              [&](const std::string& body) -> std::string {
            last_request_body = body;
            ++attempts;
            if (attempts < 2) {
              throw std::runtime_error("transient");
            }
            return R"({"content":"ok"})";
          },
      });

  const auto text = client.Generate(
      waxcpp::server::LlamaCppGenerationRequest{
          .prompt = "test prompt",
          .max_tokens = 222,
          .temperature = 0.2f,
          .top_p = 0.9f,
      });
  Require(text == "ok", "generate text mismatch");
  Require(attempts == 2, "generate retry attempts mismatch");
  Require(last_request_body.find("\"prompt\":\"test prompt\"") != std::string::npos,
          "request body must contain prompt");
  Require(last_request_body.find("\"n_predict\":222") != std::string::npos,
          "request body must contain max token value");
}

void ScenarioStripThinkingBlocks() {
  waxcpp::tests::Log("scenario: strip <think> blocks from model output");
  // Simulate Qwen3 chat completions response with <think>...</think> block.
  std::string captured_body{};
  waxcpp::server::LlamaCppGenerationClient client(
      waxcpp::server::LlamaCppGenerationConfig{
          .endpoint = "",
          .model_path = "model.gguf",
          .timeout_ms = 1000,
          .max_retries = 0,
          .retry_backoff_ms = 0,
          .request_fn =
              [&](const std::string& body) -> std::string {
            captured_body = body;
            return R"({"choices":[{"message":{"content":"<think>\nLet me reason about this.\n</think>\nThe answer is 42."}}]})";
          },
      });

  const auto result = client.Generate(
      waxcpp::server::LlamaCppGenerationRequest{
          .prompt = "What is the meaning?",
          .system_prompt = "You are helpful.",
          .max_tokens = 256,
          .temperature = 0.1f,
          .top_p = 0.95f,
      });
  Require(result == "The answer is 42.", "thinking block must be stripped, got: " + result);
  // Verify chat format was used (system_prompt is non-empty).
  Require(captured_body.find("\"messages\"") != std::string::npos,
          "chat format must use messages array");
  Require(captured_body.find("\"max_tokens\"") != std::string::npos,
          "chat format must use max_tokens (not n_predict)");
}

void ScenarioLegacyCompletionFormat() {
  waxcpp::tests::Log("scenario: legacy completion format when system_prompt is empty");
  std::string captured_body{};
  waxcpp::server::LlamaCppGenerationClient client(
      waxcpp::server::LlamaCppGenerationConfig{
          .endpoint = "",
          .model_path = "model.gguf",
          .timeout_ms = 1000,
          .max_retries = 0,
          .retry_backoff_ms = 0,
          .request_fn =
              [&](const std::string& body) -> std::string {
            captured_body = body;
            return R"({"content":"legacy result"})";
          },
      });

  const auto result = client.Generate(
      waxcpp::server::LlamaCppGenerationRequest{
          .prompt = "test prompt",
          .max_tokens = 100,
          .temperature = 0.2f,
          .top_p = 0.9f,
      });
  Require(result == "legacy result", "legacy format result mismatch");
  Require(captured_body.find("\"prompt\"") != std::string::npos,
          "legacy format must use prompt field");
  Require(captured_body.find("\"n_predict\"") != std::string::npos,
          "legacy format must use n_predict");
  Require(captured_body.find("\"messages\"") == std::string::npos,
          "legacy format must not contain messages");
}

void ScenarioGenerateValidation() {
  waxcpp::tests::Log("scenario: generate request validation");
  waxcpp::server::LlamaCppGenerationClient client(
      waxcpp::server::LlamaCppGenerationConfig{
          .endpoint = "",
          .model_path = "model.gguf",
          .timeout_ms = 1000,
          .max_retries = 0,
          .retry_backoff_ms = 0,
          .request_fn = [](const std::string&) { return std::string(R"({"content":"ok"})"); },
      });

  bool threw = false;
  try {
    (void)client.Generate(waxcpp::server::LlamaCppGenerationRequest{
        .prompt = "",
        .max_tokens = 10,
        .temperature = 0.0f,
        .top_p = 1.0f,
    });
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "empty prompt must throw");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("llama_cpp_generation_client_test: start");
    ScenarioParseSupportedSchemas();
    ScenarioParseRejectsMalformed();
    ScenarioGenerateUsesRequestFnAndRetry();
    ScenarioStripThinkingBlocks();
    ScenarioLegacyCompletionFormat();
    ScenarioGenerateValidation();
    waxcpp::tests::Log("llama_cpp_generation_client_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
