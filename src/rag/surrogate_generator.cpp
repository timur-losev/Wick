#include "waxcpp/surrogate_generator.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace waxcpp {
namespace {

// ---- ASCII helpers ----

bool IsAsciiAlpha(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool IsAsciiDigit(char ch) { return ch >= '0' && ch <= '9'; }

bool IsAsciiAlnum(char ch) { return IsAsciiAlpha(ch) || IsAsciiDigit(ch); }

char AsciiLower(char ch) {
  if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch + ('a' - 'A'));
  return ch;
}

bool IsWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

// ---- Whitespace normalization ----

std::string NormalizeWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_ws = false;
  for (const char ch : text) {
    if (IsWhitespace(ch)) {
      if (!in_ws && !out.empty()) {
        out.push_back(' ');
      }
      in_ws = true;
    } else {
      out.push_back(ch);
      in_ws = false;
    }
  }
  // Trim trailing space.
  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

// ---- Sentence segmentation ----

bool IsSentenceBoundary(char ch) {
  return ch == '.' || ch == '!' || ch == '?' || ch == '\n' || ch == ';';
}

std::vector<std::string> SegmentSentences(std::string_view text) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (IsSentenceBoundary(text[i])) {
      // Include the boundary character.
      auto seg = std::string(text.substr(start, i - start + 1));
      // Trim whitespace.
      std::size_t begin = 0;
      while (begin < seg.size() && IsWhitespace(seg[begin])) ++begin;
      std::size_t end = seg.size();
      while (end > begin && IsWhitespace(seg[end - 1])) --end;
      if (end > begin) {
        segments.push_back(seg.substr(begin, end - begin));
      }
      start = i + 1;
    }
  }
  // Trailing segment without boundary.
  if (start < text.size()) {
    auto seg = std::string(text.substr(start));
    std::size_t begin = 0;
    while (begin < seg.size() && IsWhitespace(seg[begin])) ++begin;
    std::size_t end = seg.size();
    while (end > begin && IsWhitespace(seg[end - 1])) --end;
    if (end > begin) {
      segments.push_back(seg.substr(begin, end - begin));
    }
  }
  return segments;
}

// ---- Tokenization (for scoring and similarity) ----

// Split on non-alnum, lowercase, keep tokens with length > 2.
std::vector<std::string> TokenizeForScoring(std::string_view text) {
  std::vector<std::string> tokens;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !IsAsciiAlnum(text[i])) ++i;
    if (i >= text.size()) break;
    std::size_t start = i;
    while (i < text.size() && IsAsciiAlnum(text[i])) ++i;
    if (i - start > 2) {
      std::string token;
      token.reserve(i - start);
      for (std::size_t j = start; j < i; ++j) {
        token.push_back(AsciiLower(text[j]));
      }
      tokens.push_back(std::move(token));
    }
  }
  return tokens;
}

std::set<std::string> TokenSetForScoring(std::string_view text) {
  auto tokens = TokenizeForScoring(text);
  return {tokens.begin(), tokens.end()};
}

// ---- Scoring ----

struct Candidate {
  std::string text;
  std::set<std::string> token_set;
  float score = 0.0f;
  std::size_t original_index = 0;
};

float ScoreSegment(std::string_view text, const std::vector<std::string>& tokens,
                   const std::set<std::string>& unique_tokens) {
  const auto word_count = static_cast<int>(tokens.size());
  float score = static_cast<float>(std::min(word_count, 40));

  // Short sentence penalty.
  if (word_count < 4) score *= 0.25f;
  // Long sentence penalty.
  if (word_count > 80) score *= 0.7f;

  // Content bonuses.
  bool has_digit = false;
  bool has_colon = false;
  bool has_backtick = false;
  for (const char ch : text) {
    if (IsAsciiDigit(ch)) has_digit = true;
    if (ch == ':') has_colon = true;
    if (ch == '`') has_backtick = true;
  }
  if (has_digit) score += 6.0f;
  if (has_colon) score += 4.0f;
  if (has_backtick) score += 2.0f;

  // Bullet/dash bonus.
  if (!text.empty() && (text[0] == '-' || text[0] == '*')) {
    score += 2.0f;
  }

  // Uniqueness bonus.
  if (word_count > 0) {
    const float uniqueness = static_cast<float>(unique_tokens.size()) /
                             static_cast<float>(word_count);
    score += uniqueness * 3.0f;
  }

  return score;
}

// ---- Jaccard similarity ----

float JaccardSimilarity(const std::set<std::string>& a, const std::set<std::string>& b) {
  if (a.empty() || b.empty()) return 0.0f;
  int intersection = 0;
  for (const auto& token : a) {
    if (b.count(token)) ++intersection;
  }
  if (intersection == 0) return 0.0f;
  const int union_size = static_cast<int>(a.size()) + static_cast<int>(b.size()) - intersection;
  if (union_size == 0) return 0.0f;
  return static_cast<float>(intersection) / static_cast<float>(union_size);
}

// ---- MMR Selection ----

std::vector<Candidate> SelectMMR(std::vector<Candidate> candidates, int max_items) {
  if (max_items <= 0 || candidates.empty()) return {};

  // Sort by score descending.
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

  std::vector<Candidate> selected;
  selected.reserve(static_cast<std::size_t>(max_items));

  while (static_cast<int>(selected.size()) < max_items && !candidates.empty()) {
    if (selected.empty()) {
      selected.push_back(std::move(candidates.front()));
      candidates.erase(candidates.begin());
      continue;
    }

    // Find candidate with best MMR value = score * (1 - max_redundancy).
    float best_value = -std::numeric_limits<float>::infinity();
    std::size_t best_idx = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      float max_redundancy = 0.0f;
      for (const auto& s : selected) {
        float sim = JaccardSimilarity(candidates[i].token_set, s.token_set);
        max_redundancy = std::max(max_redundancy, sim);
      }
      float value = candidates[i].score * (1.0f - max_redundancy);
      if (value > best_value) {
        best_value = value;
        best_idx = i;
      }
    }
    selected.push_back(std::move(candidates[best_idx]));
    candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(best_idx));
  }

  return selected;
}

// ---- Reorder by original position ----

std::vector<std::string> ReorderByPosition(std::vector<Candidate>& selected) {
  std::sort(selected.begin(), selected.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.original_index < b.original_index;
            });
  std::vector<std::string> texts;
  texts.reserve(selected.size());
  for (auto& c : selected) {
    texts.push_back(std::move(c.text));
  }
  return texts;
}

// ---- Truncation helper ----

std::string TruncateToTokens(std::string_view text, int max_tokens, const TokenCounter* counter) {
  if (max_tokens <= 0) return {};
  if (counter) {
    return counter->Truncate(text, max_tokens);
  }
  // Fallback: byte-level estimation (~4 bytes/token).
  auto max_bytes = static_cast<std::size_t>(max_tokens) * 4;
  if (text.size() <= max_bytes) return std::string(text);
  return std::string(text.substr(0, max_bytes));
}

// ---- Build candidates from segments ----

std::vector<Candidate> BuildCandidates(const std::vector<std::string>& segments) {
  std::vector<Candidate> candidates;
  candidates.reserve(segments.size());
  for (std::size_t i = 0; i < segments.size(); ++i) {
    Candidate c;
    c.text = segments[i];
    auto tokens = TokenizeForScoring(c.text);
    c.token_set = {tokens.begin(), tokens.end()};
    c.score = ScoreSegment(c.text, tokens, c.token_set);
    c.original_index = i;
    candidates.push_back(std::move(c));
  }
  return candidates;
}

std::string JoinWithSeparator(const std::vector<std::string>& texts, std::string_view sep) {
  if (texts.empty()) return {};
  std::string out = texts[0];
  for (std::size_t i = 1; i < texts.size(); ++i) {
    out.append(sep);
    out.append(texts[i]);
  }
  return out;
}

}  // namespace

// ---- ExtractiveSurrogateGenerator ----

ExtractiveSurrogateGenerator::ExtractiveSurrogateGenerator(const TokenCounter* counter)
    : counter_(counter) {}

std::string ExtractiveSurrogateGenerator::Generate(std::string_view source_text, int max_tokens) const {
  if (max_tokens <= 0) return {};

  // Trim.
  std::size_t begin = 0;
  while (begin < source_text.size() && IsWhitespace(source_text[begin])) ++begin;
  std::size_t end = source_text.size();
  while (end > begin && IsWhitespace(source_text[end - 1])) --end;
  if (begin >= end) return {};

  auto trimmed = source_text.substr(begin, end - begin);
  auto normalized = NormalizeWhitespace(trimmed);
  auto segments = SegmentSentences(normalized);

  if (segments.empty()) {
    return TruncateToTokens(normalized, max_tokens, counter_);
  }

  auto candidates = BuildCandidates(segments);
  auto selected = SelectMMR(std::move(candidates), 8);
  auto ordered = ReorderByPosition(selected);
  auto joined = JoinWithSeparator(ordered, "\n");
  return TruncateToTokens(joined, max_tokens, counter_);
}

SurrogateTiers ExtractiveSurrogateGenerator::GenerateTiers(
    std::string_view source_text, const SurrogateTierConfig& config) const {
  SurrogateTiers tiers;
  tiers.version = 1;

  // Trim.
  std::size_t begin = 0;
  while (begin < source_text.size() && IsWhitespace(source_text[begin])) ++begin;
  std::size_t end = source_text.size();
  while (end > begin && IsWhitespace(source_text[end - 1])) --end;
  if (begin >= end) return tiers;

  auto trimmed = source_text.substr(begin, end - begin);
  auto normalized = NormalizeWhitespace(trimmed);
  auto segments = SegmentSentences(normalized);

  if (segments.empty()) {
    tiers.full = TruncateToTokens(normalized, config.full_max_tokens, counter_);
    tiers.gist = TruncateToTokens(normalized, config.gist_max_tokens, counter_);
    tiers.micro = TruncateToTokens(normalized, config.micro_max_tokens, counter_);
    return tiers;
  }

  // Score all segments once.
  auto all_candidates = BuildCandidates(segments);

  // Full tier: 8 items, joined with newline.
  {
    auto full_selected = SelectMMR(all_candidates, 8);
    auto full_texts = ReorderByPosition(full_selected);
    auto full_joined = JoinWithSeparator(full_texts, "\n");
    tiers.full = TruncateToTokens(full_joined, config.full_max_tokens, counter_);
  }

  // Gist tier: 3 items, joined with space.
  {
    auto gist_selected = SelectMMR(all_candidates, 3);
    auto gist_texts = ReorderByPosition(gist_selected);
    auto gist_joined = JoinWithSeparator(gist_texts, " ");
    tiers.gist = TruncateToTokens(gist_joined, config.gist_max_tokens, counter_);
  }

  // Micro tier: 1 item, joined with space.
  {
    auto micro_selected = SelectMMR(all_candidates, 1);
    auto micro_texts = ReorderByPosition(micro_selected);
    auto micro_joined = JoinWithSeparator(micro_texts, " ");
    tiers.micro = TruncateToTokens(micro_joined, config.micro_max_tokens, counter_);
  }

  return tiers;
}

}  // namespace waxcpp
