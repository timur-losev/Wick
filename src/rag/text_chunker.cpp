#include "waxcpp/text_chunker.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace waxcpp {

namespace {

/// Split content by whitespace into word tokens (matching ChunkContentWhitespace logic).
std::vector<std::string> TokenizeWhitespace(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        tokens.push_back(std::move(current));
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

/// Rejoin a range of whitespace tokens with single spaces.
std::string JoinTokens(const std::vector<std::string>& tokens,
                       std::size_t start, std::size_t end) {
  std::string result;
  for (std::size_t i = start; i < end && i < tokens.size(); ++i) {
    if (!result.empty()) result += ' ';
    result += tokens[i];
  }
  return result;
}

/// Trim whitespace from both ends.
std::string TrimWS(const std::string& s) {
  auto start = s.begin();
  auto end = s.end();
  while (start != end && std::isspace(static_cast<unsigned char>(*start))) ++start;
  while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
  return std::string(start, end);
}

}  // namespace

std::vector<TextChunker::ChunkRange> TextChunker::ComputeRanges(
    int token_count, int target_tokens, int overlap_tokens) {
  if (token_count <= 0) return {};
  const int capped_target = std::max(1, target_tokens);
  const int capped_overlap = std::max(0, overlap_tokens);
  if (token_count <= capped_target) {
    return {{0, token_count}};
  }

  std::vector<ChunkRange> ranges;
  int start = 0;
  while (start < token_count) {
    const int end = std::min(start + capped_target, token_count);
    ranges.push_back({start, end});
    if (end == token_count) break;
    const int proposed = end - capped_overlap;
    const int next_start = proposed > start ? proposed : end;
    if (next_start <= start) break;  // safety
    start = next_start;
  }
  return ranges;
}

std::vector<std::string> TextChunker::Chunk(
    const std::string& text,
    const ChunkingStrategy& strategy,
    const TokenCounter* counter) {
  const int target = std::max(1, strategy.target_tokens);
  const int overlap = std::max(0, strategy.overlap_tokens);

  // BPE-aware path: encode → compute ranges → decode per range.
  if (counter != nullptr && counter->HasVocab()) {
    auto tokens = counter->Encode(text);
    if (static_cast<int>(tokens.size()) <= target) {
      return {text};
    }
    auto ranges = ComputeRanges(static_cast<int>(tokens.size()), target, overlap);
    std::vector<std::string> chunks;
    chunks.reserve(ranges.size());
    for (const auto& r : ranges) {
      std::vector<std::uint32_t> slice(tokens.begin() + r.start, tokens.begin() + r.end);
      auto chunk = counter->Decode(slice);
      auto trimmed = TrimWS(chunk);
      if (!trimmed.empty()) {
        chunks.push_back(std::move(trimmed));
      }
    }
    return chunks.empty() ? std::vector<std::string>{text} : chunks;
  }

  // Whitespace fallback path.
  auto words = TokenizeWhitespace(text);
  if (words.empty()) return {text};
  if (static_cast<int>(words.size()) <= target) return {text};

  auto ranges = ComputeRanges(static_cast<int>(words.size()), target, overlap);
  std::vector<std::string> chunks;
  chunks.reserve(ranges.size());
  for (const auto& r : ranges) {
    auto chunk = JoinTokens(words, static_cast<std::size_t>(r.start),
                            static_cast<std::size_t>(r.end));
    if (!chunk.empty()) {
      chunks.push_back(std::move(chunk));
    }
  }
  return chunks.empty() ? std::vector<std::string>{text} : chunks;
}

void TextChunker::Stream(
    const std::string& text,
    const ChunkingStrategy& strategy,
    const std::function<bool(const std::string& chunk)>& on_chunk,
    const TokenCounter* counter) {
  if (!on_chunk) return;

  const int target = std::max(1, strategy.target_tokens);
  const int overlap = std::max(0, strategy.overlap_tokens);

  // BPE-aware path.
  if (counter != nullptr && counter->HasVocab()) {
    auto tokens = counter->Encode(text);
    if (static_cast<int>(tokens.size()) <= target) {
      on_chunk(text);
      return;
    }
    auto ranges = ComputeRanges(static_cast<int>(tokens.size()), target, overlap);
    for (const auto& r : ranges) {
      std::vector<std::uint32_t> slice(tokens.begin() + r.start, tokens.begin() + r.end);
      auto chunk = counter->Decode(slice);
      auto trimmed = TrimWS(chunk);
      if (!trimmed.empty()) {
        if (!on_chunk(trimmed)) return;  // early stop
      }
    }
    return;
  }

  // Whitespace fallback path.
  auto words = TokenizeWhitespace(text);
  if (words.empty()) {
    on_chunk(text);
    return;
  }
  if (static_cast<int>(words.size()) <= target) {
    on_chunk(text);
    return;
  }

  auto ranges = ComputeRanges(static_cast<int>(words.size()), target, overlap);
  for (const auto& r : ranges) {
    auto chunk = JoinTokens(words, static_cast<std::size_t>(r.start),
                            static_cast<std::size_t>(r.end));
    if (!chunk.empty()) {
      if (!on_chunk(chunk)) return;  // early stop
    }
  }
}

}  // namespace waxcpp
