#include "waxcpp/token_counter.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// Path to bundled cl100k_base vocab, resolved relative to project root.
std::string FindVocabPath() {
  // Try common relative locations from the build directory.
  const char* env = std::getenv("WAXCPP_VOCAB_PATH");
  if (env && env[0]) return env;

  // Common paths relative to project root.
  std::vector<std::string> candidates = {
      "Sources/Wax/RAG/Resources/cl100k_base.tiktoken",
      "../Sources/Wax/RAG/Resources/cl100k_base.tiktoken",
      "../../Sources/Wax/RAG/Resources/cl100k_base.tiktoken",
      "../../../Sources/Wax/RAG/Resources/cl100k_base.tiktoken",
  };
  for (const auto& path : candidates) {
    std::ifstream f(path);
    if (f.is_open()) return path;
  }
  return {};
}

// ---- Estimation mode (no vocab) ----

void ScenarioEstimationModeEmpty() {
  waxcpp::tests::Log("scenario: estimation mode - empty text");
  waxcpp::TokenCounter counter;
  Require(!counter.HasVocab(), "should not have vocab");
  Require(counter.Count("") == 0, "empty text => 0 tokens");
}

void ScenarioEstimationModeShort() {
  waxcpp::tests::Log("scenario: estimation mode - short text");
  waxcpp::TokenCounter counter;
  Require(counter.Count("hi") >= 1, "short text >= 1 token");
  Require(counter.Count("hello world") >= 1, "hello world >= 1 token");
}

void ScenarioEstimationModeLonger() {
  waxcpp::tests::Log("scenario: estimation mode - longer text");
  waxcpp::TokenCounter counter;
  // ~4 bytes per token for English text.
  std::string text(400, 'a');  // 400 bytes => ~100 tokens
  int count = counter.Count(text);
  Require(count >= 90 && count <= 110, "400 bytes should be ~100 tokens (got " + std::to_string(count) + ")");
}

void ScenarioEstimationModeTruncate() {
  waxcpp::tests::Log("scenario: estimation mode - truncate");
  waxcpp::TokenCounter counter;
  std::string text(400, 'x');
  auto truncated = counter.Truncate(text, 10);
  // 10 tokens * 4 bytes = 40 bytes max.
  Require(truncated.size() <= 40, "truncated should be <= 40 bytes");
  Require(!truncated.empty(), "truncated should not be empty");
}

void ScenarioEstimationModeTruncateNoOp() {
  waxcpp::tests::Log("scenario: estimation mode - truncate no-op for short text");
  waxcpp::TokenCounter counter;
  auto result = counter.Truncate("hello", 1000);
  Require(result == "hello", "short text should not be truncated");
}

void ScenarioEstimationModeCountBatch() {
  waxcpp::tests::Log("scenario: estimation mode - count batch");
  waxcpp::TokenCounter counter;
  auto counts = counter.CountBatch({"hello", "world", ""});
  Require(counts.size() == 3, "batch should return 3 counts");
  Require(counts[0] >= 1, "hello >= 1");
  Require(counts[1] >= 1, "world >= 1");
  Require(counts[2] == 0, "empty => 0");
}

void ScenarioEstimationModeZeroMaxTokens() {
  waxcpp::tests::Log("scenario: estimation mode - truncate zero max tokens");
  waxcpp::TokenCounter counter;
  auto result = counter.Truncate("hello world", 0);
  Require(result.empty(), "zero max_tokens should return empty");
}

void ScenarioEstimationModeNegativeMaxTokens() {
  waxcpp::tests::Log("scenario: estimation mode - truncate negative max tokens");
  waxcpp::TokenCounter counter;
  auto result = counter.Truncate("hello world", -5);
  Require(result.empty(), "negative max_tokens should return empty");
}

// ---- BPE mode (with vocab) ----

void ScenarioBpeLoadVocab() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE load vocab - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE load vocab from " + path);
  waxcpp::TokenCounter counter(path);
  Require(counter.HasVocab(), "should have vocab");
  Require(counter.VocabSize() > 50000, "cl100k_base should have >50k entries, got " +
                                            std::to_string(counter.VocabSize()));
}

void ScenarioBpeEncodeSimple() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE encode simple - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE encode simple");
  waxcpp::TokenCounter counter(path);

  auto tokens = counter.Encode("hello");
  Require(!tokens.empty(), "hello should encode to >0 tokens");
  auto decoded = counter.Decode(tokens);
  Require(decoded == "hello", "decode(encode(hello)) should round-trip, got: " + decoded);
}

void ScenarioBpeRoundTrip() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE round-trip - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE round-trip");
  waxcpp::TokenCounter counter(path);

  std::vector<std::string> test_texts = {
      "Hello, world!",
      "The quick brown fox jumps over the lazy dog.",
      "BPE tokenization is deterministic.",
      "Numbers like 12345 and symbols @#$ work too.",
      "",
      "a",
  };

  for (const auto& text : test_texts) {
    auto tokens = counter.Encode(text);
    auto decoded = counter.Decode(tokens);
    Require(decoded == text, "round-trip failed for: '" + text + "', got: '" + decoded + "'");
  }
}

void ScenarioBpeDeterminism() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE determinism - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE determinism");
  waxcpp::TokenCounter counter(path);

  std::string text = "The memory orchestrator manages recall and remember operations.";
  auto tokens1 = counter.Encode(text);
  auto tokens2 = counter.Encode(text);
  Require(tokens1 == tokens2, "BPE encoding must be deterministic");
}

void ScenarioBpeCount() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE count - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE count");
  waxcpp::TokenCounter counter(path);

  // "hello world" is typically 2 tokens in cl100k_base.
  int count = counter.Count("hello world");
  Require(count >= 2 && count <= 4, "hello world should be 2-4 tokens, got " + std::to_string(count));

  Require(counter.Count("") == 0, "empty should be 0 tokens");
}

void ScenarioBpeTruncate() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE truncate - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE truncate");
  waxcpp::TokenCounter counter(path);

  std::string text = "The quick brown fox jumps over the lazy dog and runs through the forest.";
  auto truncated = counter.Truncate(text, 5);
  int truncated_count = counter.Count(truncated);
  Require(truncated_count <= 5, "truncated should be <= 5 tokens, got " + std::to_string(truncated_count));
  Require(!truncated.empty(), "truncated should not be empty");

  // Full text should be unchanged if max_tokens is large enough.
  auto full = counter.Truncate(text, 10000);
  Require(full == text, "large max_tokens should not truncate");
}

void ScenarioBpeCountBatch() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: BPE count batch - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: BPE count batch");
  waxcpp::TokenCounter counter(path);

  auto counts = counter.CountBatch({"hello", "world", "", "token counting"});
  Require(counts.size() == 4, "batch should return 4 counts");
  Require(counts[0] >= 1, "hello >= 1 token");
  Require(counts[1] >= 1, "world >= 1 token");
  Require(counts[2] == 0, "empty => 0");
  Require(counts[3] >= 1, "token counting >= 1 token");
}

void ScenarioBpeInvalidVocabPath() {
  waxcpp::tests::Log("scenario: BPE invalid vocab path throws");
  bool caught = false;
  try {
    waxcpp::TokenCounter counter("/nonexistent/path/vocab.tiktoken");
  } catch (const std::runtime_error&) {
    caught = true;
  }
  Require(caught, "should throw on invalid vocab path");
}

// ---- TextChunker ----

void ScenarioChunkShortText() {
  waxcpp::tests::Log("scenario: chunk short text - single chunk");
  waxcpp::TokenCounter counter;
  waxcpp::ChunkingConfig config{400, 40};
  auto chunks = waxcpp::ChunkText(counter, "hello world", config);
  Require(chunks.size() == 1, "short text should be a single chunk");
  Require(chunks[0] == "hello world", "single chunk should be entire text");
}

void ScenarioChunkLongText() {
  waxcpp::tests::Log("scenario: chunk long text - multiple chunks");
  waxcpp::TokenCounter counter;
  waxcpp::ChunkingConfig config{10, 2};  // 10 tokens target, 2 overlap

  // Create text that's ~100 tokens (estimation: 400 bytes / 4 = 100 tokens).
  std::string text;
  for (int i = 0; i < 50; ++i) {
    text += "word" + std::to_string(i) + " ";
  }

  auto chunks = waxcpp::ChunkText(counter, text, config);
  Require(chunks.size() > 1, "long text should produce multiple chunks, got " +
                                  std::to_string(chunks.size()));

  // Each chunk should not exceed target_tokens * 4 bytes.
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    Require(!chunks[i].empty(), "chunk " + std::to_string(i) + " should not be empty");
  }
}

void ScenarioChunkEmpty() {
  waxcpp::tests::Log("scenario: chunk empty text");
  waxcpp::TokenCounter counter;
  waxcpp::ChunkingConfig config{400, 40};
  auto chunks = waxcpp::ChunkText(counter, "", config);
  Require(chunks.empty(), "empty text should produce no chunks");
}

void ScenarioChunkZeroTarget() {
  waxcpp::tests::Log("scenario: chunk zero target tokens");
  waxcpp::TokenCounter counter;
  waxcpp::ChunkingConfig config{0, 0};
  auto chunks = waxcpp::ChunkText(counter, "hello world", config);
  Require(chunks.empty(), "zero target should produce no chunks");
}

void ScenarioChunkWithBpeVocab() {
  auto path = FindVocabPath();
  if (path.empty()) {
    waxcpp::tests::Log("scenario: chunk with BPE vocab - SKIPPED (vocab not found)");
    return;
  }
  waxcpp::tests::Log("scenario: chunk with BPE vocab");
  waxcpp::TokenCounter counter(path);
  waxcpp::ChunkingConfig config{10, 2};

  std::string text = "The quick brown fox jumps over the lazy dog. "
                     "Pack my box with five dozen liquor jugs. "
                     "How vexingly quick daft zebras jump.";

  auto chunks = waxcpp::ChunkText(counter, text, config);
  Require(chunks.size() > 1, "should produce multiple BPE-based chunks");

  // Verify each chunk respects token budget.
  for (const auto& chunk : chunks) {
    int count = counter.Count(chunk);
    Require(count <= config.target_tokens,
            "chunk should be <= target_tokens, got " + std::to_string(count));
  }
}

void ScenarioChunkNoOverlap() {
  waxcpp::tests::Log("scenario: chunk with zero overlap");
  waxcpp::TokenCounter counter;
  waxcpp::ChunkingConfig config{5, 0};

  std::string text(200, 'a');  // ~50 tokens
  auto chunks = waxcpp::ChunkText(counter, text, config);
  Require(chunks.size() >= 8, "should produce ~10 chunks without overlap");
}

// ---- Move semantics ----

void ScenarioMoveSemantics() {
  waxcpp::tests::Log("scenario: move semantics");
  waxcpp::TokenCounter counter1;
  Require(!counter1.HasVocab(), "counter1 should not have vocab");

  waxcpp::TokenCounter counter2 = std::move(counter1);
  Require(!counter2.HasVocab(), "counter2 should not have vocab after move");

  int count = counter2.Count("hello");
  Require(count >= 1, "moved counter should still work");
}

int RunScenarios() {
  int passed = 0;
  auto run = [&](auto fn) {
    fn();
    ++passed;
  };

  // Estimation mode.
  run(ScenarioEstimationModeEmpty);
  run(ScenarioEstimationModeShort);
  run(ScenarioEstimationModeLonger);
  run(ScenarioEstimationModeTruncate);
  run(ScenarioEstimationModeTruncateNoOp);
  run(ScenarioEstimationModeCountBatch);
  run(ScenarioEstimationModeZeroMaxTokens);
  run(ScenarioEstimationModeNegativeMaxTokens);

  // BPE mode.
  run(ScenarioBpeLoadVocab);
  run(ScenarioBpeEncodeSimple);
  run(ScenarioBpeRoundTrip);
  run(ScenarioBpeDeterminism);
  run(ScenarioBpeCount);
  run(ScenarioBpeTruncate);
  run(ScenarioBpeCountBatch);
  run(ScenarioBpeInvalidVocabPath);

  // TextChunker.
  run(ScenarioChunkShortText);
  run(ScenarioChunkLongText);
  run(ScenarioChunkEmpty);
  run(ScenarioChunkZeroTarget);
  run(ScenarioChunkWithBpeVocab);
  run(ScenarioChunkNoOverlap);

  // Move semantics.
  run(ScenarioMoveSemantics);

  return passed;
}

}  // namespace

int main() {
  try {
    const int passed = RunScenarios();
    waxcpp::tests::Log("token_counter_test: all " + std::to_string(passed) + " scenarios passed");
    return 0;
  } catch (const std::exception& ex) {
    waxcpp::tests::Log(std::string("FAIL: ") + ex.what());
    return 1;
  }
}
