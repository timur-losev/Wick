#include "waxcpp/token_counter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace waxcpp {
namespace {

// ---- Base64 decode ----

static constexpr int kB64Table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

std::string Base64Decode(std::string_view encoded) {
  std::string out;
  out.reserve(encoded.size() * 3 / 4);
  int val = 0;
  int bits = -8;
  for (const char ch : encoded) {
    if (ch == '=' || ch == '\n' || ch == '\r') continue;
    const int d = kB64Table[static_cast<unsigned char>(ch)];
    if (d < 0) continue;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// ---- ASCII pre-tokenization (cl100k_base-compatible, ASCII-safe) ----
// Splits text into chunks for BPE processing, matching cl100k_base patterns:
// - Contractions: 's, 't, 're, 've, 'm, 'll, 'd
// - Letter sequences (with optional leading non-letter-non-digit)
// - Number sequences (1-3 digits)
// - Punctuation + optional trailing newlines
// - Whitespace (newlines, or spaces not before non-space)

bool IsAsciiLetter(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool IsAsciiDigit(char ch) { return ch >= '0' && ch <= '9'; }

bool IsWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

bool IsNewline(char ch) { return ch == '\n' || ch == '\r'; }

// Check for contraction at position (case-insensitive).
// Returns length of contraction or 0.
int MatchContraction(std::string_view text, std::size_t pos) {
  if (pos >= text.size() || (text[pos] != '\'' && text[pos] != '\xE2')) return 0;
  // ASCII apostrophe.
  if (text[pos] == '\'') {
    if (pos + 1 >= text.size()) return 0;
    char next = text[pos + 1] | 0x20;  // lowercase
    if (next == 's' || next == 't' || next == 'm' || next == 'd') return 2;
    if (pos + 2 < text.size()) {
      char next2 = text[pos + 2] | 0x20;
      if ((next == 'r' && next2 == 'e') || (next == 'v' && next2 == 'e') ||
          (next == 'l' && next2 == 'l')) {
        return 3;
      }
    }
  }
  return 0;
}

std::vector<std::string> PreTokenize(std::string_view text) {
  std::vector<std::string> chunks;
  std::size_t i = 0;
  const std::size_t len = text.size();

  while (i < len) {
    // Try contraction.
    int clen = MatchContraction(text, i);
    if (clen > 0) {
      chunks.emplace_back(text.substr(i, static_cast<std::size_t>(clen)));
      i += static_cast<std::size_t>(clen);
      continue;
    }

    // Letter sequence (optional leading non-letter-non-digit).
    if (IsAsciiLetter(text[i]) ||
        (!IsAsciiLetter(text[i]) && !IsAsciiDigit(text[i]) && !IsWhitespace(text[i]) &&
         i + 1 < len && IsAsciiLetter(text[i + 1]))) {
      std::size_t start = i;
      if (!IsAsciiLetter(text[i])) ++i;  // consume leading non-letter-non-digit
      while (i < len && IsAsciiLetter(text[i])) ++i;
      // High bytes (>=0x80) can be part of UTF-8 sequences that are letter-like.
      // For ASCII-safe pre-tokenization, treat runs of high bytes as part of the chunk.
      while (i < len && (static_cast<unsigned char>(text[i]) >= 0x80)) ++i;
      if (i > start) {
        chunks.emplace_back(text.substr(start, i - start));
      }
      continue;
    }

    // Number sequence (1-3 digits).
    if (IsAsciiDigit(text[i])) {
      std::size_t start = i;
      int count = 0;
      while (i < len && IsAsciiDigit(text[i]) && count < 3) {
        ++i;
        ++count;
      }
      chunks.emplace_back(text.substr(start, i - start));
      continue;
    }

    // Newline sequences.
    if (IsNewline(text[i])) {
      std::size_t start = i;
      while (i < len && IsNewline(text[i])) ++i;
      chunks.emplace_back(text.substr(start, i - start));
      continue;
    }

    // Whitespace (space/tab).
    if (IsWhitespace(text[i])) {
      std::size_t start = i;
      while (i < len && IsWhitespace(text[i]) && !IsNewline(text[i])) ++i;
      // If followed by non-space, keep space as separate chunk.
      chunks.emplace_back(text.substr(start, i - start));
      continue;
    }

    // Punctuation/special chars.
    if (!IsAsciiLetter(text[i]) && !IsAsciiDigit(text[i]) && !IsWhitespace(text[i])) {
      std::size_t start = i;
      while (i < len && !IsAsciiLetter(text[i]) && !IsAsciiDigit(text[i]) &&
             !IsWhitespace(text[i])) {
        ++i;
      }
      // Consume trailing newlines.
      while (i < len && IsNewline(text[i])) ++i;
      chunks.emplace_back(text.substr(start, i - start));
      continue;
    }

    // Fallback: single byte.
    chunks.emplace_back(1, text[i]);
    ++i;
  }

  return chunks;
}

}  // namespace

// ---- BPE Implementation ----

struct TokenCounter::Impl {
  // Encoder: byte sequence -> rank (token ID).
  std::unordered_map<std::string, std::uint32_t> encoder;
  // Decoder: rank -> byte sequence.
  std::unordered_map<std::uint32_t, std::string> decoder;
  bool has_vocab = false;

  Impl() = default;

  void LoadVocab(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      throw std::runtime_error("TokenCounter: cannot open vocab file: " + path);
    }

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty()) continue;
      // Format: base64_bytes SPACE rank
      auto space_pos = line.find(' ');
      if (space_pos == std::string::npos) continue;
      std::string b64 = line.substr(0, space_pos);
      std::string rank_str = line.substr(space_pos + 1);
      std::string bytes = Base64Decode(b64);
      auto rank = static_cast<std::uint32_t>(std::stoul(rank_str));
      encoder[bytes] = rank;
      decoder[rank] = bytes;
    }
    has_vocab = !encoder.empty();
  }

  // BPE encode a single pre-tokenized chunk.
  std::vector<std::uint32_t> BpeEncode(std::string_view chunk) const {
    if (chunk.empty()) return {};

    // Single byte optimization.
    if (chunk.size() == 1) {
      std::string key(1, chunk[0]);
      auto it = encoder.find(key);
      if (it != encoder.end()) {
        return {it->second};
      }
      return {};
    }

    // Initialize parts: each byte is a separate part.
    struct Part {
      std::size_t start;
      std::size_t len;
      bool removed = false;
    };
    std::vector<Part> parts;
    parts.reserve(chunk.size());
    for (std::size_t i = 0; i < chunk.size(); ++i) {
      parts.push_back({i, 1, false});
    }

    // Greedy BPE merge using priority queue.
    struct MergeCandidate {
      std::uint32_t rank;
      std::size_t left_idx;
      std::size_t gen;  // generation counter to invalidate stale entries
    };
    auto cmp = [](const MergeCandidate& a, const MergeCandidate& b) {
      return a.rank > b.rank;  // min-heap
    };
    std::priority_queue<MergeCandidate, std::vector<MergeCandidate>, decltype(cmp)> heap(cmp);

    // Generation counter per part (incremented on merge to invalidate old entries).
    std::vector<std::size_t> gen(parts.size(), 0);

    auto get_merged_key = [&](std::size_t left, std::size_t right) -> std::string {
      return std::string(chunk.substr(parts[left].start, parts[left].len)) +
             std::string(chunk.substr(parts[right].start, parts[right].len));
    };

    auto find_next_active = [&](std::size_t idx) -> std::size_t {
      std::size_t j = idx + 1;
      while (j < parts.size() && parts[j].removed) ++j;
      return j;
    };

    auto find_prev_active = [&](std::size_t idx) -> std::size_t {
      if (idx == 0) return parts.size();  // sentinel
      std::size_t j = idx - 1;
      while (j > 0 && parts[j].removed) --j;
      if (parts[j].removed) return parts.size();  // none found
      return j;
    };

    // Seed heap with all adjacent pairs.
    {
      std::size_t prev = 0;
      for (std::size_t cur = find_next_active(0); cur < parts.size();
           prev = cur, cur = find_next_active(cur)) {
        auto key = get_merged_key(prev, cur);
        auto it = encoder.find(key);
        if (it != encoder.end()) {
          heap.push({it->second, prev, gen[prev]});
        }
      }
    }

    while (!heap.empty()) {
      auto best = heap.top();
      heap.pop();

      // Skip if stale (part was already merged).
      if (parts[best.left_idx].removed || best.gen != gen[best.left_idx]) continue;

      std::size_t right = find_next_active(best.left_idx);
      if (right >= parts.size() || parts[right].removed) continue;

      // Merge: extend left to include right.
      parts[best.left_idx].len =
          (parts[right].start + parts[right].len) - parts[best.left_idx].start;
      parts[right].removed = true;
      gen[best.left_idx]++;

      // Re-evaluate pair with previous active part.
      std::size_t prev = find_prev_active(best.left_idx);
      if (prev < parts.size()) {
        auto key = get_merged_key(prev, best.left_idx);
        auto it = encoder.find(key);
        if (it != encoder.end()) {
          heap.push({it->second, prev, gen[prev]});
        }
      }

      // Re-evaluate pair with next active part.
      std::size_t next = find_next_active(best.left_idx);
      if (next < parts.size()) {
        auto key = get_merged_key(best.left_idx, next);
        auto it = encoder.find(key);
        if (it != encoder.end()) {
          heap.push({it->second, best.left_idx, gen[best.left_idx]});
        }
      }
    }

    // Collect results.
    std::vector<std::uint32_t> tokens;
    for (const auto& part : parts) {
      if (part.removed) continue;
      auto key = std::string(chunk.substr(part.start, part.len));
      auto it = encoder.find(key);
      if (it != encoder.end()) {
        tokens.push_back(it->second);
      } else {
        // Fallback: encode individual bytes.
        for (std::size_t b = part.start; b < part.start + part.len; ++b) {
          std::string byte_key(1, chunk[b]);
          auto byte_it = encoder.find(byte_key);
          if (byte_it != encoder.end()) {
            tokens.push_back(byte_it->second);
          }
        }
      }
    }
    return tokens;
  }

  std::vector<std::uint32_t> Encode(std::string_view text) const {
    if (!has_vocab) return {};
    // Cap input.
    if (text.size() > TokenCounter::kMaxTokenizationBytes) {
      text = text.substr(0, TokenCounter::kMaxTokenizationBytes);
    }
    auto chunks = PreTokenize(text);
    std::vector<std::uint32_t> tokens;
    tokens.reserve(text.size() / 3);  // rough estimate
    for (const auto& chunk : chunks) {
      auto chunk_tokens = BpeEncode(chunk);
      tokens.insert(tokens.end(), chunk_tokens.begin(), chunk_tokens.end());
    }
    return tokens;
  }

  int Count(std::string_view text) const {
    if (has_vocab) {
      return static_cast<int>(Encode(text).size());
    }
    // Byte-level estimation: ~4 bytes per token for English (cl100k_base average).
    if (text.empty()) return 0;
    auto capped = std::min(text.size(), TokenCounter::kMaxTokenizationBytes);
    return std::max(1, static_cast<int>(std::ceil(static_cast<double>(capped) / 4.0)));
  }

  std::string Decode(const std::vector<std::uint32_t>& tokens) const {
    std::string out;
    for (const auto token : tokens) {
      auto it = decoder.find(token);
      if (it != decoder.end()) {
        out.append(it->second);
      }
    }
    return out;
  }

  std::string Truncate(std::string_view text, int max_tokens) const {
    if (max_tokens <= 0) return {};
    if (!has_vocab) {
      // Byte-level estimation truncation.
      auto max_bytes = static_cast<std::size_t>(max_tokens) * 4;
      if (text.size() <= max_bytes) return std::string(text);
      return std::string(text.substr(0, max_bytes));
    }
    auto tokens = Encode(text);
    if (static_cast<int>(tokens.size()) <= max_tokens) {
      return std::string(text);
    }
    tokens.resize(static_cast<std::size_t>(max_tokens));
    return Decode(tokens);
  }
};

// ---- TokenCounter public API ----

TokenCounter::TokenCounter() : impl_(std::make_unique<Impl>()) {}

TokenCounter::TokenCounter(const std::string& vocab_path)
    : impl_(std::make_unique<Impl>()) {
  impl_->LoadVocab(vocab_path);
}

TokenCounter::~TokenCounter() = default;
TokenCounter::TokenCounter(TokenCounter&&) noexcept = default;
TokenCounter& TokenCounter::operator=(TokenCounter&&) noexcept = default;

int TokenCounter::Count(std::string_view text) const {
  return impl_->Count(text);
}

std::vector<std::uint32_t> TokenCounter::Encode(std::string_view text) const {
  return impl_->Encode(text);
}

std::string TokenCounter::Decode(const std::vector<std::uint32_t>& tokens) const {
  return impl_->Decode(tokens);
}

std::string TokenCounter::Truncate(std::string_view text, int max_tokens) const {
  return impl_->Truncate(text, max_tokens);
}

std::vector<int> TokenCounter::CountBatch(const std::vector<std::string>& texts) const {
  std::vector<int> counts;
  counts.reserve(texts.size());
  for (const auto& text : texts) {
    counts.push_back(Count(text));
  }
  return counts;
}

bool TokenCounter::HasVocab() const { return impl_->has_vocab; }
std::size_t TokenCounter::VocabSize() const { return impl_->encoder.size(); }

// ---- TextChunker ----

std::vector<std::string> ChunkText(const TokenCounter& counter,
                                   std::string_view text,
                                   const ChunkingConfig& config) {
  if (text.empty() || config.target_tokens <= 0) return {};

  const int total_tokens = counter.Count(text);
  if (total_tokens <= config.target_tokens) {
    return {std::string(text)};
  }

  // Token-based chunking with overlap.
  if (counter.HasVocab()) {
    auto all_tokens = counter.Encode(text);
    const int token_count = static_cast<int>(all_tokens.size());
    if (token_count <= config.target_tokens) {
      return {std::string(text)};
    }

    std::vector<std::string> chunks;
    int start = 0;
    while (start < token_count) {
      int end = std::min(start + config.target_tokens, token_count);
      std::vector<std::uint32_t> slice(all_tokens.begin() + start,
                                       all_tokens.begin() + end);
      auto chunk = counter.Decode(slice);
      // Filter whitespace-only chunks.
      bool all_ws = true;
      for (char ch : chunk) {
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
          all_ws = false;
          break;
        }
      }
      if (!all_ws && !chunk.empty()) {
        chunks.push_back(std::move(chunk));
      }

      // Advance with overlap.
      int proposed = end - config.overlap_tokens;
      // Prevent stalling: if overlap would not advance, skip overlap.
      start = (proposed > start) ? proposed : end;
    }
    return chunks;
  }

  // Byte-level estimation fallback.
  const std::size_t bytes_per_token = 4;
  const std::size_t target_bytes = static_cast<std::size_t>(config.target_tokens) * bytes_per_token;
  const std::size_t overlap_bytes = static_cast<std::size_t>(config.overlap_tokens) * bytes_per_token;

  std::vector<std::string> chunks;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = std::min(start + target_bytes, text.size());
    // Try to break at word boundary.
    if (end < text.size()) {
      std::size_t back = end;
      while (back > start && text[back] != ' ' && text[back] != '\n') --back;
      if (back > start) end = back;
    }
    auto chunk = std::string(text.substr(start, end - start));
    bool all_ws = true;
    for (char ch : chunk) {
      if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
        all_ws = false;
        break;
      }
    }
    if (!all_ws && !chunk.empty()) {
      chunks.push_back(std::move(chunk));
    }
    std::size_t proposed = end > overlap_bytes ? end - overlap_bytes : end;
    start = (proposed > start) ? proposed : end;
  }
  return chunks;
}

}  // namespace waxcpp
