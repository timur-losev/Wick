#include "../../server/llama_cpp_embedding_provider.hpp"

#include "../test_logger.hpp"

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
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

bool ApproxEqual(float lhs, float rhs, float eps = 1e-5F) {
  return std::fabs(lhs - rhs) <= eps;
}

void ScenarioParseSupportsMultipleSchemas() {
  waxcpp::tests::Log("scenario: parse supports multiple schemas");
  {
    const auto parsed = waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"embedding":[1.0,2.0,3.0]})",
        3);
    Require(parsed.size() == 3, "direct embedding schema parse size mismatch");
  }
  {
    const auto parsed = waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"embeddings":[[0.1,0.2,0.3],[9,9,9]]})",
        3);
    Require(parsed.size() == 3 && ApproxEqual(parsed[0], 0.1F), "embeddings schema parse mismatch");
  }
  {
    const auto parsed = waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"data":[{"embedding":[7.0,8.0,9.0]}]})",
        3);
    Require(parsed.size() == 3 && ApproxEqual(parsed[2], 9.0F), "openai-style data schema parse mismatch");
  }
}

void ScenarioParseRejectsMalformedOrMismatched() {
  waxcpp::tests::Log("scenario: parse rejects malformed or mismatched payloads");
  bool threw = false;
  try {
    (void)waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"embedding":[1.0,2.0]})",
        3);
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "dimension mismatch must throw");

  threw = false;
  try {
    (void)waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"foo":"bar"})",
        3);
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "missing embedding field must throw");
}

void ScenarioRequestFnEmbedAndMemoization() {
  waxcpp::tests::Log("scenario: request_fn embed and memoization");
  int request_count = 0;
  waxcpp::server::LlamaCppEmbeddingProvider provider(
      waxcpp::server::LlamaCppEmbeddingProviderConfig{
          .endpoint = "",
          .model_path = "g:/Proj/Agents1/Models/Qwen/embedding.gguf",
          .dimensions = 2,
          .normalize = true,
          .timeout_ms = 1000,
          .memoization_capacity = 4,
          .request_fn =
              [&](const std::string& body) -> std::string {
            ++request_count;
            if (body.find("\"content\":\"alpha\"") != std::string::npos) {
              return R"({"embedding":[3.0,4.0]})";
            }
            return R"({"embedding":[0.0,5.0]})";
          },
      });

  const auto first = provider.Embed("alpha");
  const auto second = provider.Embed("alpha");
  const auto third = provider.Embed("beta");
  Require(first.size() == 2, "embed dimensions mismatch");
  Require(ApproxEqual(first[0], 0.6F) && ApproxEqual(first[1], 0.8F), "normalized embedding mismatch");
  Require(first == second, "memoized embedding mismatch");
  Require(ApproxEqual(third[0], 0.0F) && ApproxEqual(third[1], 1.0F), "second normalized embedding mismatch");
  Require(request_count == 2, "request_fn should be called once per unique key");

  const auto batch = provider.EmbedBatch({"alpha", "beta", "alpha"});
  Require(batch.size() == 3, "batch size mismatch");
  Require(batch[0] == first && batch[1] == third && batch[2] == first, "batch embeddings mismatch");

  const auto identity = provider.identity();
  Require(identity.has_value(), "identity should be present");
  Require(identity->provider.has_value() && *identity->provider == "llama.cpp", "identity provider mismatch");
  Require(identity->dimensions.has_value() && *identity->dimensions == 2, "identity dimensions mismatch");
}

void ScenarioRetrySucceedsAfterTransientFailures() {
  waxcpp::tests::Log("scenario: retry succeeds after transient failures");
  int attempts = 0;
  waxcpp::server::LlamaCppEmbeddingProvider provider(
      waxcpp::server::LlamaCppEmbeddingProviderConfig{
          .endpoint = "",
          .model_path = "g:/Proj/Agents1/Models/Qwen/embedding.gguf",
          .dimensions = 3,
          .normalize = false,
          .timeout_ms = 1000,
          .max_retries = 2,
          .retry_backoff_ms = 1,
          .max_batch_concurrency = 2,
          .memoization_capacity = 0,
          .request_fn =
              [&](const std::string&) -> std::string {
            ++attempts;
            if (attempts < 3) {
              throw std::runtime_error("transient failure");
            }
            return R"({"embedding":[1.0,2.0,3.0]})";
          },
      });

  const auto embedding = provider.Embed("retry-me");
  Require(embedding.size() == 3, "retry scenario embedding size mismatch");
  Require(attempts == 3, "retry scenario should attempt exactly 3 times");
}

void ScenarioRetryExhaustionThrows() {
  waxcpp::tests::Log("scenario: retry exhaustion throws");
  int attempts = 0;
  waxcpp::server::LlamaCppEmbeddingProvider provider(
      waxcpp::server::LlamaCppEmbeddingProviderConfig{
          .endpoint = "",
          .model_path = "g:/Proj/Agents1/Models/Qwen/embedding.gguf",
          .dimensions = 2,
          .normalize = false,
          .timeout_ms = 1000,
          .max_retries = 1,
          .retry_backoff_ms = 1,
          .max_batch_concurrency = 1,
          .memoization_capacity = 0,
          .request_fn =
              [&](const std::string&) -> std::string {
            ++attempts;
            throw std::runtime_error("always failing");
          },
      });

  bool threw = false;
  try {
    (void)provider.Embed("nope");
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "retry exhaustion should throw");
  Require(attempts == 2, "retry exhaustion should perform retries + initial attempt");
}

void ScenarioBatchBoundedConcurrency() {
  waxcpp::tests::Log("scenario: batch bounded concurrency");
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  std::atomic<int> request_count{0};
  waxcpp::server::LlamaCppEmbeddingProvider provider(
      waxcpp::server::LlamaCppEmbeddingProviderConfig{
          .endpoint = "",
          .model_path = "g:/Proj/Agents1/Models/Qwen/embedding.gguf",
          .dimensions = 2,
          .normalize = false,
          .timeout_ms = 1000,
          .max_retries = 0,
          .retry_backoff_ms = 0,
          .max_batch_concurrency = 2,
          .memoization_capacity = 0,
          .request_fn =
              [&](const std::string&) -> std::string {
            const int now_active = active.fetch_add(1) + 1;
            int observed = max_active.load();
            while (now_active > observed && !max_active.compare_exchange_weak(observed, now_active)) {
            }
            ++request_count;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            active.fetch_sub(1);
            return R"({"embedding":[5.0,6.0]})";
          },
      });

  const std::vector<std::string> texts = {"a", "b", "c", "d", "a", "e", "b", "f"};
  const auto out = provider.EmbedBatch(texts);
  Require(out.size() == texts.size(), "bounded concurrency batch size mismatch");
  for (const auto& embedding : out) {
    Require(embedding.size() == 2, "bounded concurrency embedding dims mismatch");
    Require(ApproxEqual(embedding[0], 5.0F) && ApproxEqual(embedding[1], 6.0F),
            "bounded concurrency embedding value mismatch");
  }
  Require(max_active.load() <= 2, "batch concurrency must not exceed configured bound");
  Require(request_count.load() == 6, "batch must dedupe duplicate keys");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("llama_cpp_embedding_provider_test: start");
    ScenarioParseSupportsMultipleSchemas();
    ScenarioParseRejectsMalformedOrMismatched();
    ScenarioRequestFnEmbedAndMemoization();
    ScenarioRetrySucceedsAfterTransientFailures();
    ScenarioRetryExhaustionThrows();
    ScenarioBatchBoundedConcurrency();
    waxcpp::tests::Log("llama_cpp_embedding_provider_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
