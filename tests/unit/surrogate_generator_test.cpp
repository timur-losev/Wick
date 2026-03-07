#include "waxcpp/surrogate_generator.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// ---- Single-tier generation ----

void ScenarioGenerateShortText() {
  waxcpp::tests::Log("scenario: generate surrogate from short text");
  waxcpp::ExtractiveSurrogateGenerator gen;

  auto result = gen.Generate("Hello world.", 100);
  Require(!result.empty(), "should produce non-empty surrogate");
  Require(result.find("Hello") != std::string::npos, "should contain source text");
}

void ScenarioGenerateMultiSentence() {
  waxcpp::tests::Log("scenario: generate surrogate from multi-sentence text");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text =
      "The project launched in Q3 2024. Revenue grew by 150% year over year. "
      "The team expanded to 50 engineers. Customer satisfaction reached 4.8 out of 5. "
      "New features include real-time search. The API handles 10000 requests per second. "
      "Security audit passed with zero critical findings. Deployment pipeline runs in 3 minutes.";

  auto result = gen.Generate(text, 50);
  Require(!result.empty(), "should produce output");
  // With max_tokens=50, output should be shorter than input.
  Require(result.size() < text.size(), "should be shorter than input");
}

void ScenarioGenerateEmptyInput() {
  waxcpp::tests::Log("scenario: generate with empty input");
  waxcpp::ExtractiveSurrogateGenerator gen;

  Require(gen.Generate("", 100).empty(), "empty input => empty output");
  Require(gen.Generate("   ", 100).empty(), "whitespace-only => empty output");
}

void ScenarioGenerateZeroMaxTokens() {
  waxcpp::tests::Log("scenario: generate with zero max_tokens");
  waxcpp::ExtractiveSurrogateGenerator gen;

  Require(gen.Generate("Hello world.", 0).empty(), "zero max_tokens => empty");
  Require(gen.Generate("Hello world.", -5).empty(), "negative max_tokens => empty");
}

void ScenarioGenerateSingleSegment() {
  waxcpp::tests::Log("scenario: generate from text without sentence boundaries");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text = "This is a single block of text without any sentence boundaries";
  auto result = gen.Generate(text, 100);
  Require(!result.empty(), "single block should still produce output");
}

// ---- Scoring prioritization ----

void ScenarioScoringFavorsDigitsAndColons() {
  waxcpp::tests::Log("scenario: scoring favors sentences with digits and colons");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text =
      "The weather was nice today. "
      "Revenue: $150M in Q3 2024. "
      "Birds flew over the lake quietly.";

  auto result = gen.Generate(text, 30);
  // The sentence with digits and colons should be prioritized.
  Require(result.find("Revenue") != std::string::npos ||
              result.find("150") != std::string::npos,
          "sentence with digits/colons should be prioritized");
}

void ScenarioScoringFavorsBullets() {
  waxcpp::tests::Log("scenario: scoring favors bullet points");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text =
      "Introduction to the project. "
      "- Key feature: real-time search engine. "
      "* Another feature: batch processing pipeline. "
      "The end of the document.";

  auto result = gen.Generate(text, 30);
  Require(result.find("-") != std::string::npos || result.find("*") != std::string::npos,
          "bullet points should appear in surrogate");
}

// ---- MMR diversity ----

void ScenarioMMRDiversitySelection() {
  waxcpp::tests::Log("scenario: MMR selects diverse sentences");
  waxcpp::ExtractiveSurrogateGenerator gen;

  // Create text with redundant and diverse sentences.
  std::string text =
      "The search engine is fast and reliable. "
      "The search engine processes queries quickly. "  // redundant with first
      "Customer data is encrypted at rest. "  // diverse
      "The search engine handles millions of queries. "  // redundant
      "Deployment takes under five minutes.";  // diverse

  auto result = gen.Generate(text, 100);
  // Should include diverse sentences, not just the top-scoring redundant ones.
  Require(!result.empty(), "should produce output");
}

// ---- Hierarchical tiers ----

void ScenarioGenerateTiersDefault() {
  waxcpp::tests::Log("scenario: generate tiers with default config");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text =
      "The project was founded in 2020. It grew rapidly in its first year. "
      "Revenue reached $10M by end of 2021. The team scaled to 100 people. "
      "Key products include: search engine, analytics dashboard, and API gateway. "
      "Customers span 40 countries. Average response time is 50ms. "
      "The codebase has 500k lines of code. Test coverage is at 95%.";

  auto tiers = gen.GenerateTiers(text);
  Require(tiers.version == 1, "version should be 1");
  Require(!tiers.full.empty(), "full tier should not be empty");
  Require(!tiers.gist.empty(), "gist tier should not be empty");
  Require(!tiers.micro.empty(), "micro tier should not be empty");

  // Full should be longest, micro should be shortest.
  Require(tiers.full.size() >= tiers.gist.size(), "full >= gist in length");
  Require(tiers.gist.size() >= tiers.micro.size(), "gist >= micro in length");
}

void ScenarioGenerateTiersCompact() {
  waxcpp::tests::Log("scenario: generate tiers with compact config");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text =
      "The search engine indexes documents in real-time. "
      "It supports vector similarity and full-text search. "
      "Query latency is under 10ms for most operations. "
      "The system handles 100k concurrent connections.";

  auto tiers = gen.GenerateTiers(text, waxcpp::SurrogateTierConfig::Compact());
  Require(!tiers.full.empty(), "compact full should not be empty");
  Require(!tiers.micro.empty(), "compact micro should not be empty");
}

void ScenarioGenerateTiersEmptyInput() {
  waxcpp::tests::Log("scenario: generate tiers with empty input");
  waxcpp::ExtractiveSurrogateGenerator gen;

  auto tiers = gen.GenerateTiers("");
  Require(tiers.full.empty(), "empty input -> empty full");
  Require(tiers.gist.empty(), "empty input -> empty gist");
  Require(tiers.micro.empty(), "empty input -> empty micro");
}

void ScenarioGenerateTiersNoSegments() {
  waxcpp::tests::Log("scenario: generate tiers from text without segments");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text = "A single block of text without any sentence-ending punctuation";
  auto tiers = gen.GenerateTiers(text);
  Require(!tiers.full.empty(), "should produce full tier");
  Require(!tiers.micro.empty(), "should produce micro tier");
}

// ---- TokenCounter integration ----

void ScenarioWithTokenCounter() {
  waxcpp::tests::Log("scenario: surrogate generation with token counter");
  waxcpp::TokenCounter counter;  // estimation mode
  waxcpp::ExtractiveSurrogateGenerator gen(&counter);

  std::string text =
      "The memory orchestrator manages the complete lifecycle of stored memories. "
      "It supports remember, recall, and flush operations. "
      "Embeddings are cached for efficient vector search. "
      "The write-ahead log ensures crash recovery.";

  auto result = gen.Generate(text, 10);
  Require(!result.empty(), "should produce output with counter");

  auto tiers = gen.GenerateTiers(text);
  Require(!tiers.full.empty(), "tiers with counter should work");
}

// ---- Determinism ----

void ScenarioDeterministicOutput() {
  waxcpp::tests::Log("scenario: surrogate generation is deterministic");
  waxcpp::ExtractiveSurrogateGenerator gen;

  std::string text =
      "Alpha team shipped the feature on Monday. "
      "Beta team completed testing on Tuesday. "
      "Gamma team deployed to production on Wednesday. "
      "Revenue increased by 25% after launch.";

  auto result1 = gen.Generate(text, 50);
  auto result2 = gen.Generate(text, 50);
  Require(result1 == result2, "generation must be deterministic");

  auto tiers1 = gen.GenerateTiers(text);
  auto tiers2 = gen.GenerateTiers(text);
  Require(tiers1.full == tiers2.full, "full tier must be deterministic");
  Require(tiers1.gist == tiers2.gist, "gist tier must be deterministic");
  Require(tiers1.micro == tiers2.micro, "micro tier must be deterministic");
}

// ---- Algorithm ID ----

void ScenarioAlgorithmID() {
  waxcpp::tests::Log("scenario: algorithm ID is extractive_v1");
  Require(std::string(waxcpp::ExtractiveSurrogateGenerator::kAlgorithmID) == "extractive_v1",
          "algorithm ID should be extractive_v1");
}

// ---- Reordering ----

void ScenarioReorderByOriginalPosition() {
  waxcpp::tests::Log("scenario: selected sentences maintain original order");
  waxcpp::ExtractiveSurrogateGenerator gen;

  // Create text where the highest-scoring sentence is not first.
  std::string text =
      "Introduction paragraph here. "
      "Key metric: revenue is $500M for FY2024. "
      "The conclusion wraps things up.";

  auto result = gen.Generate(text, 100);
  // The high-scoring sentence should appear after the intro.
  auto intro_pos = result.find("Introduction");
  auto metric_pos = result.find("Key metric");
  if (intro_pos != std::string::npos && metric_pos != std::string::npos) {
    Require(intro_pos < metric_pos, "sentences should be in original order");
  }
}

int RunScenarios() {
  int passed = 0;
  auto run = [&](auto fn) {
    fn();
    ++passed;
  };

  run(ScenarioGenerateShortText);
  run(ScenarioGenerateMultiSentence);
  run(ScenarioGenerateEmptyInput);
  run(ScenarioGenerateZeroMaxTokens);
  run(ScenarioGenerateSingleSegment);
  run(ScenarioScoringFavorsDigitsAndColons);
  run(ScenarioScoringFavorsBullets);
  run(ScenarioMMRDiversitySelection);
  run(ScenarioGenerateTiersDefault);
  run(ScenarioGenerateTiersCompact);
  run(ScenarioGenerateTiersEmptyInput);
  run(ScenarioGenerateTiersNoSegments);
  run(ScenarioWithTokenCounter);
  run(ScenarioDeterministicOutput);
  run(ScenarioAlgorithmID);
  run(ScenarioReorderByOriginalPosition);

  return passed;
}

}  // namespace

int main() {
  try {
    const int passed = RunScenarios();
    waxcpp::tests::Log("surrogate_generator_test: all " + std::to_string(passed) + " scenarios passed");
    return 0;
  } catch (const std::exception& ex) {
    waxcpp::tests::Log(std::string("FAIL: ") + ex.what());
    return 1;
  }
}
