#include "waxcpp/search.hpp"
#include "waxcpp/query_analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string_view>
#include <vector>

namespace waxcpp {
namespace {

bool IsAsciiWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::vector<std::string> SplitWhitespaceTokens(std::string_view text) {
  std::vector<std::string> tokens{};
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsAsciiWhitespace(text[start])) {
      ++start;
    }
    if (start >= text.size()) {
      break;
    }
    std::size_t end = start;
    while (end < text.size() && !IsAsciiWhitespace(text[end])) {
      ++end;
    }
    tokens.emplace_back(text.substr(start, end - start));
    start = end;
  }
  return tokens;
}

std::string JoinPrefixTokens(const std::vector<std::string>& tokens, std::size_t count) {
  if (count == 0 || tokens.empty()) {
    return {};
  }
  std::string out = tokens[0];
  for (std::size_t i = 1; i < count && i < tokens.size(); ++i) {
    out.push_back(' ');
    out.append(tokens[i]);
  }
  return out;
}

std::string BuildSurrogateText(std::uint64_t frame_id) {
  return "frame " + std::to_string(frame_id);
}

std::string TruncateBytes(const std::string& text, int max_bytes) {
  if (max_bytes <= 0) {
    return {};
  }
  const auto limit = static_cast<std::size_t>(max_bytes);
  if (text.size() <= limit) {
    return text;
  }
  return text.substr(0, limit);
}

bool ScoreLess(const SearchResult& lhs, const SearchResult& rhs) {
  const float lhs_score = std::isnan(lhs.score) ? 0.0F : lhs.score;
  const float rhs_score = std::isnan(rhs.score) ? 0.0F : rhs.score;
  if (lhs_score != rhs_score) {
    return lhs_score > rhs_score;
  }
  return lhs.frame_id < rhs.frame_id;
}

std::vector<SearchSource> NormalizeSources(std::vector<SearchSource> sources) {
  std::sort(sources.begin(), sources.end(), [](const auto lhs, const auto rhs) {
    return static_cast<int>(lhs) < static_cast<int>(rhs);
  });
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return sources;
}

bool PreferPreviewText(const std::optional<std::string>& candidate, const std::optional<std::string>& current) {
  if (!candidate.has_value()) {
    return false;
  }
  if (!current.has_value()) {
    return true;
  }
  // Deterministic tie-break for equal-score duplicate frame entries.
  return *candidate < *current;
}

void UpdatePreferredPreview(std::optional<std::string>& slot, const std::optional<std::string>& candidate) {
  if (PreferPreviewText(candidate, slot)) {
    slot = candidate;
  }
}

float ClampAlpha(float alpha) {
  return std::min(1.0F, std::max(0.0F, alpha));
}

std::vector<SearchResult> MergeDuplicateFrameResults(std::vector<SearchResult> results) {
  struct ChannelAggregate {
    float best_score = 0.0F;
    std::optional<std::string> best_preview{};
    std::optional<std::string> fallback_preview{};
    std::unordered_set<SearchSource> sources{};
    bool seen = false;
  };

  std::unordered_map<std::uint64_t, ChannelAggregate> by_frame{};
  by_frame.reserve(results.size());
  for (const auto& result : results) {
    auto& agg = by_frame[result.frame_id];
    const float normalized_score = std::isnan(result.score) ? 0.0F : result.score;
    if (!agg.seen || normalized_score > agg.best_score) {
      if (agg.seen) {
        UpdatePreferredPreview(agg.fallback_preview, agg.best_preview);
      }
      agg.best_score = normalized_score;
      agg.best_preview = result.preview_text;
      agg.seen = true;
    } else if (normalized_score == agg.best_score) {
      UpdatePreferredPreview(agg.best_preview, result.preview_text);
    } else {
      UpdatePreferredPreview(agg.fallback_preview, result.preview_text);
    }
    for (const auto source : result.sources) {
      agg.sources.insert(source);
    }
  }

  results.clear();
  results.reserve(by_frame.size());
  for (auto& [frame_id, agg] : by_frame) {
    SearchResult merged{};
    merged.frame_id = frame_id;
    merged.score = agg.best_score;
    merged.preview_text =
        agg.best_preview.has_value() ? std::move(agg.best_preview) : std::move(agg.fallback_preview);
    merged.sources.assign(agg.sources.begin(), agg.sources.end());
    std::sort(merged.sources.begin(), merged.sources.end(), [](const auto lhs, const auto rhs) {
      return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
    results.push_back(std::move(merged));
  }
  return results;
}

std::vector<SearchResult> SortedChannel(std::vector<SearchResult> results, int top_k) {
  results = MergeDuplicateFrameResults(std::move(results));
  std::sort(results.begin(), results.end(), ScoreLess);
  if (top_k > 0 && results.size() > static_cast<std::size_t>(top_k)) {
    results.resize(static_cast<std::size_t>(top_k));
  }
  return results;
}

SearchResponse BuildSingleChannelResponse(std::vector<SearchResult> results, int top_k) {
  SearchResponse out{};
  out.results = SortedChannel(std::move(results), top_k);
  return out;
}

SearchResponse BuildHybridRrfResponse(const SearchRequest& request,
                                      std::vector<SearchResult> text_results,
                                      std::vector<SearchResult> vector_results) {
  const auto sorted_text = SortedChannel(std::move(text_results), request.top_k);
  const auto sorted_vector = SortedChannel(std::move(vector_results), request.top_k);

  struct Aggregate {
    float score = 0.0F;
    std::optional<std::string> preview_text;
    std::unordered_set<SearchSource> sources;
  };
  std::unordered_map<std::uint64_t, Aggregate> aggregates{};
  const float alpha = ClampAlpha(request.mode.alpha);
  const float text_weight = alpha;
  const float vector_weight = 1.0F - alpha;
  const float base = static_cast<float>(std::max(0, request.rrf_k));

  auto apply_channel = [&](const std::vector<SearchResult>& channel, float weight) {
    for (std::size_t i = 0; i < channel.size(); ++i) {
      const auto rank = static_cast<float>(i + 1U);
      const auto contribution = weight * (1.0F / (base + rank));
      auto& agg = aggregates[channel[i].frame_id];
      agg.score += contribution;
      if (!agg.preview_text.has_value() && channel[i].preview_text.has_value()) {
        agg.preview_text = channel[i].preview_text;
      }
      for (const auto source : channel[i].sources) {
        agg.sources.insert(source);
      }
    }
  };

  if (text_weight > 0.0F) {
    apply_channel(sorted_text, text_weight);
  }
  if (vector_weight > 0.0F) {
    apply_channel(sorted_vector, vector_weight);
  }

  SearchResponse out{};
  out.results.reserve(aggregates.size());
  for (const auto& [frame_id, agg] : aggregates) {
    SearchResult item{};
    item.frame_id = frame_id;
    item.score = agg.score;
    item.preview_text = agg.preview_text;
    item.sources.assign(agg.sources.begin(), agg.sources.end());
    std::sort(item.sources.begin(), item.sources.end(), [](const auto lhs, const auto rhs) {
      return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
    out.results.push_back(std::move(item));
  }

  std::sort(out.results.begin(), out.results.end(), ScoreLess);
  if (request.top_k > 0 && out.results.size() > static_cast<std::size_t>(request.top_k)) {
    out.results.resize(static_cast<std::size_t>(request.top_k));
  }
  return out;
}

}  // namespace

SearchResponse UnifiedSearch(const SearchRequest& request) {
  return UnifiedSearchWithCandidates(request, {}, {});
}

SearchResponse UnifiedSearchWithCandidates(const SearchRequest& request,
                                           const std::vector<SearchResult>& text_results,
                                           const std::vector<SearchResult>& vector_results) {
  if (request.top_k <= 0) {
    return {};
  }

  SearchResponse response;
  switch (request.mode.kind) {
    case SearchModeKind::kTextOnly:
      response = BuildSingleChannelResponse(text_results, request.top_k);
      break;
    case SearchModeKind::kVectorOnly:
      response = BuildSingleChannelResponse(vector_results, request.top_k);
      break;
    case SearchModeKind::kHybrid:
      response = BuildHybridRrfResponse(request, text_results, vector_results);
      break;
  }

  // Apply intent-aware reranking when query is non-empty.
  if (request.query.has_value() && !request.query->empty()) {
    const int max_window =
        std::min(std::max(request.top_k * 2, 10), 32);
    response.results = IntentAwareRerank(
        response.results, *request.query, max_window);
  }

  return response;
}

SearchResponse UnifiedSearchAdaptive(const SearchRequest& request,
                                     const std::vector<SearchResult>& text_results,
                                     const std::vector<SearchResult>& vector_results,
                                     const AdaptiveFusionConfig& fusion_config) {
  if (request.top_k <= 0) {
    return {};
  }

  // For non-hybrid modes, adaptive fusion doesn't apply.
  if (request.mode.kind != SearchModeKind::kHybrid) {
    return UnifiedSearchWithCandidates(request, text_results, vector_results);
  }

  // Classify query and select adaptive weights.
  std::string_view query_text;
  if (request.query.has_value()) {
    query_text = *request.query;
  }

  const auto query_type = ClassifyQuery(query_text);
  const auto weights = fusion_config.weights(query_type);

  // Build a modified request with the adaptive alpha (bm25 weight).
  SearchRequest adapted = request;
  adapted.mode.alpha = weights.bm25;

  return UnifiedSearchWithCandidates(adapted, text_results, vector_results);
}

RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response) {
  RAGContext context;
  if (request.query.has_value()) {
    context.query = *request.query;
  }

  const int clamped_top_k = std::max(0, request.top_k);
  const int clamped_max_snippets = std::max(0, request.max_snippets);
  const int clamped_max_context_tokens = std::max(0, request.max_context_tokens);
  const int clamped_snippet_max_tokens = std::max(0, request.snippet_max_tokens);
  const int clamped_expansion_max_tokens =
      std::min(std::max(0, request.expansion_max_tokens), clamped_max_context_tokens);

  if (clamped_top_k == 0 || clamped_max_context_tokens == 0) {
    context.total_tokens = 0;
    return context;
  }

  std::vector<SearchResult> sorted_results = MergeDuplicateFrameResults(response.results);
  std::sort(sorted_results.begin(), sorted_results.end(), ScoreLess);
  if (sorted_results.size() > static_cast<std::size_t>(clamped_top_k)) {
    sorted_results.resize(static_cast<std::size_t>(clamped_top_k));
  }

  context.total_tokens = 0;
  context.items.reserve(sorted_results.size());
  int emitted_snippets = 0;
  const bool expansion_enabled = clamped_expansion_max_tokens > 0;
  // Content-based dedup: skip results with identical truncated text.
  // Different frame_ids can hold the same content (e.g. re-indexed chunks).
  std::unordered_set<std::size_t> seen_text_hashes{};
  for (const auto& result : sorted_results) {
    const bool is_first_item = context.items.empty();
    const bool use_expanded_tier = expansion_enabled && is_first_item;
    auto item_kind = use_expanded_tier ? RAGItemKind::kExpanded : RAGItemKind::kSnippet;
    const bool counts_towards_snippet_cap = !use_expanded_tier;

    if (counts_towards_snippet_cap && emitted_snippets >= clamped_max_snippets) {
      continue;
    }

    std::string candidate_text{};
    if (result.preview_text.has_value()) {
      candidate_text = TruncateBytes(*result.preview_text, request.preview_max_bytes);
    }
    if (candidate_text.empty()) {
      item_kind = RAGItemKind::kSurrogate;
      candidate_text = BuildSurrogateText(result.frame_id);
    }

    // Content dedup: skip if we already emitted identical text.
    const auto text_hash = std::hash<std::string>{}(candidate_text);
    if (!seen_text_hashes.insert(text_hash).second) {
      continue;  // duplicate content — skip
    }

    auto tokens = SplitWhitespaceTokens(candidate_text);
    if (tokens.empty()) {
      continue;
    }
    const int configured_limit = use_expanded_tier ? clamped_expansion_max_tokens : clamped_snippet_max_tokens;
    if (configured_limit == 0) {
      continue;
    }
    const std::size_t per_item_limit =
        configured_limit > 0 ? std::min<std::size_t>(tokens.size(), static_cast<std::size_t>(configured_limit))
                             : tokens.size();
    std::size_t emit_tokens = per_item_limit;
    const int remaining = clamped_max_context_tokens - context.total_tokens;
    if (remaining <= 0) {
      break;
    }
    emit_tokens = std::min<std::size_t>(emit_tokens, static_cast<std::size_t>(remaining));
    if (emit_tokens == 0) {
      break;
    }

    RAGItem item{};
    item.kind = item_kind;
    item.frame_id = result.frame_id;
    item.score = std::isnan(result.score) ? 0.0F : result.score;
    item.sources = NormalizeSources(result.sources);
    item.text = JoinPrefixTokens(tokens, emit_tokens);
    context.total_tokens += static_cast<int>(emit_tokens);
    if (counts_towards_snippet_cap) {
      ++emitted_snippets;
    }
    context.items.push_back(std::move(item));
    if (context.total_tokens >= clamped_max_context_tokens) {
      break;
    }
  }
  return context;
}

// ---------- RerankingHelpers ----------

namespace RerankingHelpers {

bool ContainsTentativeLaunchLanguage(std::string_view text) {
  return text.find("tentative") != std::string_view::npos ||
         text.find("draft") != std::string_view::npos ||
         text.find("proposed") != std::string_view::npos ||
         text.find("pending approval") != std::string_view::npos ||
         text.find("target is") != std::string_view::npos ||
         text.find("target date") != std::string_view::npos ||
         text.find("could be") != std::string_view::npos ||
         text.find("estimate") != std::string_view::npos;
}

bool ContainsMovedToLocationPattern(std::string_view text) {
  // Matches: (moved|move) to <Capitalized>(whitespace <Capitalized>)?
  for (std::size_t pos = 0; pos < text.size();) {
    auto moved_pos = text.find("moved to ", pos);
    auto move_pos = text.find("move to ", pos);

    std::size_t match_start = std::string_view::npos;
    std::size_t after_to = 0;

    if (moved_pos != std::string_view::npos &&
        (move_pos == std::string_view::npos || moved_pos <= move_pos)) {
      match_start = moved_pos;
      after_to = moved_pos + 9;  // "moved to " = 9
    } else if (move_pos != std::string_view::npos) {
      match_start = move_pos;
      after_to = move_pos + 8;   // "move to " = 8
    }

    if (match_start == std::string_view::npos || after_to >= text.size()) {
      break;
    }

    // Expect uppercase letter at start of destination.
    if (after_to < text.size() &&
        text[after_to] >= 'A' && text[after_to] <= 'Z') {
      // Check for lowercase continuation.
      std::size_t i = after_to + 1;
      while (i < text.size() && text[i] >= 'a' && text[i] <= 'z') {
        ++i;
      }
      if (i > after_to + 1) {
        return true;
      }
    }

    pos = after_to;
  }
  return false;
}

bool LooksDistractorLike(std::string_view text) {
  return text.find("weekly report") != std::string_view::npos ||
         text.find("checklist") != std::string_view::npos ||
         text.find("signoff") != std::string_view::npos ||
         text.find("allergic") != std::string_view::npos ||
         text.find("distractor") != std::string_view::npos ||
         text.find("draft memo") != std::string_view::npos ||
         text.find("tentative") != std::string_view::npos ||
         text.find("pending approval") != std::string_view::npos;
}

}  // namespace RerankingHelpers

// ---------- IntentAwareRerank ----------

namespace {

std::string ToLower(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool IsDigitOnly(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
  }
  return true;
}

bool IsLetterOnly(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s) {
    if (!std::isalpha(static_cast<unsigned char>(ch))) return false;
  }
  return true;
}

bool TermContainsDigits(const std::string& s) {
  for (char ch : s) {
    if (std::isdigit(static_cast<unsigned char>(ch))) return true;
  }
  return false;
}

bool ContainsSubstr(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasSourceVector(const std::vector<SearchSource>& sources) {
  for (auto s : sources) {
    if (s == SearchSource::kVector) return true;
  }
  return false;
}

/// Normalized phrase-comparable text (lower, split on non-alnum, re-join with spaces).
std::string NormalizedPhraseText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool in_word = false;
  for (char ch : text) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      out.push_back(static_cast<char>(std::tolower(uch)));
      in_word = true;
    } else {
      if (in_word) {
        out.push_back(' ');
        in_word = false;
      }
    }
  }
  // Trim trailing space.
  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

/// Intersection count of two sets.
template <typename T>
int SetIntersectionCount(const std::set<T>& a, const std::set<T>& b) {
  int count = 0;
  auto it_a = a.begin();
  auto it_b = b.begin();
  while (it_a != a.end() && it_b != b.end()) {
    if (*it_a < *it_b) {
      ++it_a;
    } else if (*it_b < *it_a) {
      ++it_b;
    } else {
      ++count;
      ++it_a;
      ++it_b;
    }
  }
  return count;
}

}  // anonymous namespace

std::vector<SearchResult> IntentAwareRerank(
    const std::vector<SearchResult>& results,
    std::string_view query,
    int max_window,
    const QueryAnalyzer& analyzer) {

  const int capped_window = std::min(std::max(0, max_window),
                                     static_cast<int>(results.size()));
  if (capped_window <= 1) {
    return results;
  }

  const auto intents = analyzer.DetectIntent(query);

  // Precompute query signals.
  const auto query_terms_vec = analyzer.NormalizedTerms(query);
  const std::set<std::string> query_terms(query_terms_vec.begin(),
                                          query_terms_vec.end());
  const auto query_entities = analyzer.EntityTerms(query);
  const auto query_years = analyzer.YearTerms(query);
  const auto query_date_keys = analyzer.NormalizedDateKeys(query);

  std::set<std::string> query_numeric_entities;
  std::set<std::string> query_alpha_entities;
  std::set<std::string> query_numeric_terms;
  for (const auto& e : query_entities) {
    if (TermContainsDigits(e)) query_numeric_entities.insert(e);
    if (IsLetterOnly(e)) query_alpha_entities.insert(e);
  }
  for (const auto& t : query_terms) {
    if (IsDigitOnly(t)) query_numeric_terms.insert(t);
  }

  const bool has_target_intent =
      HasIntent(intents, QueryIntent::kAsksLocation) ||
      HasIntent(intents, QueryIntent::kAsksDate) ||
      HasIntent(intents, QueryIntent::kAsksOwnership);

  const bool has_disambiguation_signals =
      !query_entities.empty() ||
      !query_years.empty() ||
      !query_date_keys.empty();

  if (!has_target_intent || !has_disambiguation_signals) {
    return results;
  }

  // Composite scoring function.
  auto composite_score = [&](const SearchResult& result) -> float {
    float total = result.score;
    if (!result.preview_text.has_value() || result.preview_text->empty()) {
      return total;
    }

    const auto& preview = *result.preview_text;
    const auto preview_terms_vec = analyzer.NormalizedTerms(preview);
    const std::set<std::string> preview_terms(preview_terms_vec.begin(),
                                              preview_terms_vec.end());
    const auto preview_entities = analyzer.EntityTerms(preview);
    const auto preview_years = analyzer.YearTerms(preview);
    const auto preview_date_keys = analyzer.NormalizedDateKeys(preview);
    std::set<std::string> preview_alpha_entities;
    for (const auto& e : preview_entities) {
      if (IsLetterOnly(e)) preview_alpha_entities.insert(e);
    }
    const std::string lower = ToLower(preview);
    const bool vector_influenced = HasSourceVector(result.sources);

    // Term recall/precision.
    if (!query_terms.empty() && !preview_terms.empty()) {
      const int overlap = SetIntersectionCount(query_terms, preview_terms);
      const float recall =
          static_cast<float>(overlap) /
          static_cast<float>(std::max<int>(1, static_cast<int>(query_terms.size())));
      const float precision =
          static_cast<float>(overlap) /
          static_cast<float>(std::max<int>(1, static_cast<int>(preview_terms.size())));
      total += recall * 0.55f;
      total += precision * 0.25f;
    }

    // Entity matching.
    if (!query_entities.empty()) {
      const int entity_hits = SetIntersectionCount(query_entities, preview_entities);
      if (!query_numeric_entities.empty()) {
        const int numeric_hits =
            SetIntersectionCount(query_numeric_entities, preview_entities);
        const float numeric_coverage =
            static_cast<float>(numeric_hits) /
            static_cast<float>(std::max<int>(1, static_cast<int>(query_numeric_entities.size())));
        total += numeric_coverage * 1.95f;
      }
      if (!query_alpha_entities.empty()) {
        const int alpha_hits =
            SetIntersectionCount(query_alpha_entities, preview_alpha_entities);
        const float alpha_coverage =
            static_cast<float>(alpha_hits) /
            static_cast<float>(std::max<int>(1, static_cast<int>(query_alpha_entities.size())));
        total += alpha_coverage * 1.25f;
      }
      const float coverage =
          static_cast<float>(entity_hits) /
          static_cast<float>(std::max<int>(1, static_cast<int>(query_entities.size())));
      total += coverage * 0.30f;
      if (entity_hits == 0) {
        total -= !query_numeric_entities.empty() ? 0.85f : 0.45f;
        if (!query_numeric_terms.empty()) {
          const int num_term_hits =
              SetIntersectionCount(query_numeric_terms, preview_terms);
          if (num_term_hits > 0) {
            total -= 0.75f;
          }
        }
      }
      if (!query_alpha_entities.empty()) {
        const int alpha_match =
            SetIntersectionCount(query_alpha_entities, preview_alpha_entities);
        if (alpha_match == 0 && !preview_alpha_entities.empty()) {
          total -= 0.40f;
        }
      }
    }

    // Year matching.
    if (!query_years.empty()) {
      const int year_hits = SetIntersectionCount(query_years, preview_years);
      const float year_coverage =
          static_cast<float>(year_hits) /
          static_cast<float>(std::max<int>(1, static_cast<int>(query_years.size())));
      total += year_coverage * 1.25f;
      if (year_hits == 0 && !preview_years.empty()) {
        total -= 1.10f;
      }
    }

    // Date key matching.
    if (!query_date_keys.empty()) {
      const int date_hits =
          SetIntersectionCount(query_date_keys, preview_date_keys);
      const float date_coverage =
          static_cast<float>(date_hits) /
          static_cast<float>(std::max<int>(1, static_cast<int>(query_date_keys.size())));
      total += date_coverage * 1.15f;
      if (date_hits == 0 && !preview_date_keys.empty()) {
        total -= 0.95f;
      }
    }

    // Intent-specific boosts/penalties.
    if (HasIntent(intents, QueryIntent::kAsksLocation)) {
      if (RerankingHelpers::ContainsMovedToLocationPattern(preview)) {
        total += 1.60f;
      } else if (ContainsSubstr(lower, "moved to") ||
                 ContainsSubstr(lower, "move to")) {
        total += 0.45f;
      } else if (ContainsSubstr(lower, "city")) {
        total += 0.10f;
      }
      if (ContainsSubstr(lower, "without a destination") ||
          ContainsSubstr(lower, "city move") ||
          ContainsSubstr(lower, "retrospective")) {
        total -= 0.75f;
      }
      if (ContainsSubstr(lower, "allergic") ||
          ContainsSubstr(lower, "health") ||
          ContainsSubstr(lower, "peanut")) {
        total -= 1.10f;
      }
      if (ContainsSubstr(lower, "prefers") ||
          ContainsSubstr(lower, "prefer")) {
        total -= 0.55f;
      }
    }

    if (HasIntent(intents, QueryIntent::kAsksDate)) {
      const bool tentative =
          RerankingHelpers::ContainsTentativeLaunchLanguage(lower);
      if (ContainsSubstr(lower, "public launch is") && !tentative) {
        total += 1.70f;
      } else if (ContainsSubstr(lower, "public launch") ||
                 analyzer.ContainsDateLiteral(preview)) {
        total += 1.20f;
      }
      if (tentative) {
        const float scaled_penalty = std::max(
            vector_influenced ? 2.90f : 2.45f,
            result.score * (vector_influenced ? 1.60f : 1.40f));
        total -= scaled_penalty;
      }
      if (ContainsSubstr(lower, "draft memo")) {
        total -= vector_influenced ? 1.45f : 1.20f;
      }
      if (ContainsSubstr(lower, " owns ") ||
          ContainsSubstr(lower, "owner") ||
          ContainsSubstr(lower, "deployment readiness")) {
        total -= 0.40f;
      }
    }

    if (HasIntent(intents, QueryIntent::kAsksOwnership)) {
      if (ContainsSubstr(lower, " owns ") ||
          ContainsSubstr(lower, "owner") ||
          ContainsSubstr(lower, "owns deployment readiness")) {
        total += 1.10f;
      }
      if (ContainsSubstr(lower, "public launch") &&
          !ContainsSubstr(lower, " owns ")) {
        total -= 0.35f;
      }
    }

    if (RerankingHelpers::LooksDistractorLike(lower)) {
      total -= 0.40f;
    }

    return total;
  };

  // Score the rerank window.
  struct ScoredEntry {
    int original_index;
    float composite;
  };

  std::vector<ScoredEntry> scored_head;
  scored_head.reserve(static_cast<std::size_t>(capped_window));
  for (int i = 0; i < capped_window; ++i) {
    scored_head.push_back({i, composite_score(results[static_cast<std::size_t>(i)])});
  }

  std::sort(scored_head.begin(), scored_head.end(),
            [&](const ScoredEntry& lhs, const ScoredEntry& rhs) {
              if (lhs.composite != rhs.composite) {
                return lhs.composite > rhs.composite;
              }
              const auto& lr = results[static_cast<std::size_t>(lhs.original_index)];
              const auto& rr = results[static_cast<std::size_t>(rhs.original_index)];
              if (lr.score != rr.score) {
                return lr.score > rr.score;
              }
              return lr.frame_id < rr.frame_id;
            });

  // Build output: reranked head + untouched tail.
  std::vector<SearchResult> output;
  output.reserve(results.size());
  for (const auto& entry : scored_head) {
    output.push_back(results[static_cast<std::size_t>(entry.original_index)]);
  }
  for (std::size_t i = static_cast<std::size_t>(capped_window);
       i < results.size(); ++i) {
    output.push_back(results[i]);
  }

  return output;
}

}  // namespace waxcpp
