#include "waxcpp/text_chunker.hpp"
#include "../test_logger.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace waxcpp;
using namespace waxcpp::tests;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* label) {
  if (condition) {
    ++g_pass;
    Log(std::string("  PASS: ") + label);
  } else {
    ++g_fail;
    LogError(std::string("  FAIL: ") + label);
  }
}

// ============================================================
// 1. ComputeRanges
// ============================================================

void TestRangesEmpty() {
  Log("=== TestRangesEmpty ===");
  auto ranges = TextChunker::ComputeRanges(0, 10, 2);
  Check(ranges.empty(), "empty for 0 tokens");
}

void TestRangesSingleChunk() {
  Log("=== TestRangesSingleChunk ===");
  auto ranges = TextChunker::ComputeRanges(5, 10, 2);
  Check(ranges.size() == 1, "single range when fits");
  if (!ranges.empty()) {
    Check(ranges[0].start == 0 && ranges[0].end == 5, "range covers all");
  }
}

void TestRangesMultipleChunks() {
  Log("=== TestRangesMultipleChunks ===");
  auto ranges = TextChunker::ComputeRanges(25, 10, 3);
  Check(ranges.size() >= 3, "multiple ranges for 25 tokens / 10 target / 3 overlap");
  if (!ranges.empty()) {
    Check(ranges[0].start == 0, "first range starts at 0");
    Check(ranges[0].end == 10, "first range ends at 10");
  }
  if (ranges.size() >= 2) {
    Check(ranges[1].start == 7, "second range starts at 10-3=7 (overlap)");
  }
  // Last range should end at 25.
  Check(ranges.back().end == 25, "last range ends at token_count");
  LogKV("num_ranges", static_cast<std::uint64_t>(ranges.size()));
}

void TestRangesNoOverlap() {
  Log("=== TestRangesNoOverlap ===");
  auto ranges = TextChunker::ComputeRanges(20, 10, 0);
  Check(ranges.size() == 2, "two ranges for 20/10/0");
  if (ranges.size() == 2) {
    Check(ranges[0].start == 0 && ranges[0].end == 10, "first 0..10");
    Check(ranges[1].start == 10 && ranges[1].end == 20, "second 10..20");
  }
}

void TestRangesExactFit() {
  Log("=== TestRangesExactFit ===");
  auto ranges = TextChunker::ComputeRanges(10, 10, 2);
  Check(ranges.size() == 1, "single range when exactly fits");
  if (!ranges.empty()) {
    Check(ranges[0].start == 0 && ranges[0].end == 10, "covers all");
  }
}

// ============================================================
// 2. Chunk (whitespace fallback)
// ============================================================

void TestChunkShortText() {
  Log("=== TestChunkShortText ===");
  ChunkingStrategy strategy{10, 2};
  auto chunks = TextChunker::Chunk("hello world", strategy);
  Check(chunks.size() == 1, "short text returns single chunk");
  Check(chunks[0] == "hello world", "original text preserved");
}

void TestChunkLongText() {
  Log("=== TestChunkLongText ===");
  // Create a 25-word text.
  std::string text;
  for (int i = 0; i < 25; ++i) {
    if (i > 0) text += ' ';
    text += "word" + std::to_string(i);
  }
  ChunkingStrategy strategy{10, 3};
  auto chunks = TextChunker::Chunk(text, strategy);
  Check(chunks.size() >= 3, "long text produces multiple chunks");
  // First chunk should have 10 words.
  int first_words = 0;
  for (char c : chunks[0]) {
    if (c == ' ') ++first_words;
  }
  first_words += 1;  // spaces + 1 = words
  Check(first_words == 10, "first chunk has target word count");
  LogKV("num_chunks", static_cast<std::uint64_t>(chunks.size()));
}

void TestChunkEmptyText() {
  Log("=== TestChunkEmptyText ===");
  ChunkingStrategy strategy{10, 2};
  auto chunks = TextChunker::Chunk("", strategy);
  Check(chunks.size() == 1, "empty text returns single chunk");
  Check(chunks[0].empty(), "chunk is empty string");
}

void TestChunkZeroTarget() {
  Log("=== TestChunkZeroTarget ===");
  ChunkingStrategy strategy{0, 0};
  auto chunks = TextChunker::Chunk("a b c d e", strategy);
  // target clamped to 1 → each word is a chunk.
  Check(chunks.size() >= 5, "zero target clamps to 1");
  LogKV("num_chunks", static_cast<std::uint64_t>(chunks.size()));
}

// ============================================================
// 3. Stream
// ============================================================

void TestStreamShortText() {
  Log("=== TestStreamShortText ===");
  ChunkingStrategy strategy{10, 2};
  std::vector<std::string> received;
  TextChunker::Stream("hello world", strategy,
                      [&](const std::string& chunk) {
                        received.push_back(chunk);
                        return true;
                      });
  Check(received.size() == 1, "short text yields single chunk");
  Check(received[0] == "hello world", "original text preserved");
}

void TestStreamLongText() {
  Log("=== TestStreamLongText ===");
  std::string text;
  for (int i = 0; i < 25; ++i) {
    if (i > 0) text += ' ';
    text += "word" + std::to_string(i);
  }
  ChunkingStrategy strategy{10, 3};
  std::vector<std::string> received;
  TextChunker::Stream(text, strategy,
                      [&](const std::string& chunk) {
                        received.push_back(chunk);
                        return true;
                      });
  Check(received.size() >= 3, "long text yields multiple chunks");

  // Verify matches Chunk() output.
  auto batch = TextChunker::Chunk(text, strategy);
  Check(received.size() == batch.size(), "stream yields same count as batch");
  bool match = true;
  for (std::size_t i = 0; i < std::min(received.size(), batch.size()); ++i) {
    if (received[i] != batch[i]) match = false;
  }
  Check(match, "stream and batch produce identical chunks");
}

void TestStreamEarlyStop() {
  Log("=== TestStreamEarlyStop ===");
  std::string text;
  for (int i = 0; i < 25; ++i) {
    if (i > 0) text += ' ';
    text += "word" + std::to_string(i);
  }
  ChunkingStrategy strategy{10, 3};
  std::vector<std::string> received;
  TextChunker::Stream(text, strategy,
                      [&](const std::string& chunk) {
                        received.push_back(chunk);
                        return false;  // Stop after first.
                      });
  Check(received.size() == 1, "early stop yields one chunk");
}

// ============================================================
// 4. Stream/Chunk parity
// ============================================================

void TestChunkStreamParity() {
  Log("=== TestChunkStreamParity ===");
  std::string text;
  for (int i = 0; i < 50; ++i) {
    if (i > 0) text += ' ';
    text += "token" + std::to_string(i);
  }
  ChunkingStrategy strategy{15, 5};

  auto batch_chunks = TextChunker::Chunk(text, strategy);
  std::vector<std::string> stream_chunks;
  TextChunker::Stream(text, strategy,
                      [&](const std::string& chunk) {
                        stream_chunks.push_back(chunk);
                        return true;
                      });

  Check(batch_chunks.size() == stream_chunks.size(), "same count");
  bool identical = true;
  for (std::size_t i = 0; i < batch_chunks.size() && i < stream_chunks.size(); ++i) {
    if (batch_chunks[i] != stream_chunks[i]) identical = false;
  }
  Check(identical, "batch and stream produce identical results");
}

}  // namespace

int main() {
  Log("== TextChunker Tests ==");

  TestRangesEmpty();
  TestRangesSingleChunk();
  TestRangesMultipleChunks();
  TestRangesNoOverlap();
  TestRangesExactFit();
  TestChunkShortText();
  TestChunkLongText();
  TestChunkEmptyText();
  TestChunkZeroTarget();
  TestStreamShortText();
  TestStreamLongText();
  TestStreamEarlyStop();
  TestChunkStreamParity();

  std::cout << "\n== Results: " << g_pass << " passed, " << g_fail << " failed ==\n";
  return g_fail > 0 ? 1 : 0;
}
