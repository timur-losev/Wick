#pragma once

#include "waxcpp/query_analyzer.hpp"
#include "waxcpp/types.hpp"

namespace waxcpp {

SearchResponse UnifiedSearch(const SearchRequest& request);
SearchResponse UnifiedSearchWithCandidates(const SearchRequest& request,
                                           const std::vector<SearchResult>& text_results,
                                           const std::vector<SearchResult>& vector_results);

/// Adaptive search: classifies query, selects fusion weights, then runs search.
/// Uses AdaptiveFusionConfig to determine alpha based on query type.
SearchResponse UnifiedSearchAdaptive(const SearchRequest& request,
                                     const std::vector<SearchResult>& text_results,
                                     const std::vector<SearchResult>& vector_results,
                                     const AdaptiveFusionConfig& fusion_config = AdaptiveFusionConfig::Default());

RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response);

/// Shared text-matching helpers for reranking (used by both UnifiedSearch
/// and FastRAGContextBuilder rerankers). Stateless predicates only.
namespace RerankingHelpers {

/// True when text contains language indicating a tentative / unconfirmed date.
bool ContainsTentativeLaunchLanguage(std::string_view text);

/// True when text matches "moved/move to <Capitalized>" pattern.
bool ContainsMovedToLocationPattern(std::string_view text);

/// True when text looks like a distractor result.
bool LooksDistractorLike(std::string_view text);

}  // namespace RerankingHelpers

/// Intent-aware reranking: boosts/penalizes search results based on
/// query intents (location, date, ownership) and entity/date/phrase overlap.
/// Only activates when target intents AND disambiguation signals are present.
std::vector<SearchResult> IntentAwareRerank(
    const std::vector<SearchResult>& results,
    std::string_view query,
    int max_window,
    const QueryAnalyzer& analyzer = QueryAnalyzer());

}  // namespace waxcpp
