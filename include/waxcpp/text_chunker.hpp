#pragma once

#include <functional>
#include <string>
#include <vector>

#include "waxcpp/token_counter.hpp"
#include "waxcpp/types.hpp"

namespace waxcpp {

/// Deterministic, token-aware text chunking.
/// C++ equivalent of Swift's TextChunker, using the same encoding as FastRAG.
///
/// Supports two modes:
///   - Batch: returns all chunks at once (equivalent to Swift's `chunk()`)
///   - Streaming: yields chunks one at a time via callback (equivalent to Swift's `stream()`)
class TextChunker {
 public:
  /// Chunk text into pieces of approximately `strategy.target_tokens` tokens
  /// with `strategy.overlap_tokens` overlap between consecutive chunks.
  ///
  /// When a BPE-loaded TokenCounter is provided, uses BPE-aware splitting.
  /// Otherwise falls back to whitespace-word-based splitting.
  ///
  /// Returns `[text]` when text fits in a single chunk or on error.
  static std::vector<std::string> Chunk(const std::string& text,
                                        const ChunkingStrategy& strategy,
                                        const TokenCounter* counter = nullptr);

  /// Stream chunks one at a time via callback instead of materializing the full list.
  /// Useful for very large texts where you want to process chunks incrementally.
  ///
  /// The callback receives each non-empty chunk. Return `false` from the callback
  /// to stop streaming early.
  ///
  /// When a BPE-loaded TokenCounter is provided, uses BPE-aware splitting.
  /// Otherwise falls back to whitespace-word-based splitting.
  static void Stream(const std::string& text,
                     const ChunkingStrategy& strategy,
                     const std::function<bool(const std::string& chunk)>& on_chunk,
                     const TokenCounter* counter = nullptr);

  /// Compute chunk ranges as [start, end) token index pairs without decoding.
  /// Useful for pre-computing chunk boundaries.
  struct ChunkRange {
    int start = 0;
    int end = 0;
  };
  static std::vector<ChunkRange> ComputeRanges(int token_count,
                                                int target_tokens,
                                                int overlap_tokens);
};

}  // namespace waxcpp
