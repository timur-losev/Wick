#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace waxcpp {

/// BPE-based token counter compatible with cl100k_base encoding.
/// When constructed with a vocab file, performs exact BPE encoding.
/// When constructed without a vocab, uses byte-level estimation (~4 bytes/token).
class TokenCounter {
 public:
  /// Construct with byte-level estimation (no vocab file needed).
  TokenCounter();

  /// Construct with a BPE vocab file (cl100k_base.tiktoken format).
  /// Each line: base64(bytes) SPACE rank
  explicit TokenCounter(const std::string& vocab_path);

  ~TokenCounter();
  TokenCounter(TokenCounter&&) noexcept;
  TokenCounter& operator=(TokenCounter&&) noexcept;

  // Non-copyable.
  TokenCounter(const TokenCounter&) = delete;
  TokenCounter& operator=(const TokenCounter&) = delete;

  /// Count tokens in text.
  int Count(std::string_view text) const;

  /// Encode text to token IDs.
  std::vector<std::uint32_t> Encode(std::string_view text) const;

  /// Decode token IDs back to text.
  std::string Decode(const std::vector<std::uint32_t>& tokens) const;

  /// Truncate text to at most max_tokens tokens.
  std::string Truncate(std::string_view text, int max_tokens) const;

  /// Count tokens for each text in the batch.
  std::vector<int> CountBatch(const std::vector<std::string>& texts) const;

  /// Whether this counter has a loaded BPE vocab (exact mode).
  bool HasVocab() const;

  /// Number of vocab entries loaded.
  std::size_t VocabSize() const;

  /// Maximum input bytes to process (safety cap).
  static constexpr std::size_t kMaxTokenizationBytes = 8 * 1024 * 1024;  // 8 MiB

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Token-aware text chunker using a TokenCounter.
struct ChunkingConfig {
  int target_tokens = 400;
  int overlap_tokens = 40;
};

/// Split text into token-bounded chunks with optional overlap.
std::vector<std::string> ChunkText(const TokenCounter& counter,
                                   std::string_view text,
                                   const ChunkingConfig& config);

}  // namespace waxcpp
