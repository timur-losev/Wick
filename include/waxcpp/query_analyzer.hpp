#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace waxcpp {

/// Query intent flags (OptionSet-style bitmask).
enum class QueryIntent : std::uint32_t {
  kNone = 0,
  kAsksLocation = 1 << 0,
  kAsksDate = 1 << 1,
  kAsksOwnership = 1 << 2,
  kMultiHop = 1 << 3,
};

inline QueryIntent operator|(QueryIntent lhs, QueryIntent rhs) {
  return static_cast<QueryIntent>(static_cast<std::uint32_t>(lhs) |
                                  static_cast<std::uint32_t>(rhs));
}
inline QueryIntent operator&(QueryIntent lhs, QueryIntent rhs) {
  return static_cast<QueryIntent>(static_cast<std::uint32_t>(lhs) &
                                  static_cast<std::uint32_t>(rhs));
}
inline bool HasIntent(QueryIntent flags, QueryIntent flag) {
  return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(flag)) != 0;
}

/// Rule-based query type classification.
enum class QueryType {
  kFactual,
  kSemantic,
  kTemporal,
  kExploratory,
};

}  // namespace waxcpp

/// Hash specialization for QueryType (must be declared before unordered_map usage).
template <>
struct std::hash<waxcpp::QueryType> {
  std::size_t operator()(waxcpp::QueryType qt) const noexcept {
    return std::hash<int>{}(static_cast<int>(qt));
  }
};

namespace waxcpp {

/// Extracted query signals for downstream ranking/fusion decisions.
struct QuerySignals {
  bool has_specific_entities = false;
  int word_count = 0;
  bool has_quoted_phrases = false;
  float specificity_score = 0.0f;
};

/// Fusion weights for adaptive query-type-aware search blending.
struct FusionWeights {
  float bm25 = 0.5f;
  float vector = 0.5f;
  float temporal = 0.0f;
};

/// Adaptive fusion config: maps QueryType to FusionWeights.
class AdaptiveFusionConfig {
 public:
  AdaptiveFusionConfig();
  explicit AdaptiveFusionConfig(std::unordered_map<QueryType, FusionWeights> weights);

  FusionWeights weights(QueryType query_type) const;

  static const AdaptiveFusionConfig& Default();

 private:
  std::unordered_map<QueryType, FusionWeights> weights_;
};

/// Stateless query analyzer: extracts signals, terms, entities, dates, intents.
class QueryAnalyzer {
 public:
  QueryAnalyzer() = default;

  QuerySignals Analyze(std::string_view query) const;
  std::vector<std::string> NormalizedTerms(std::string_view query) const;
  std::set<std::string> EntityTerms(std::string_view query) const;
  std::set<std::string> YearTerms(std::string_view text) const;
  std::vector<std::string> DateLiterals(std::string_view text) const;
  std::set<std::string> NormalizedDateKeys(std::string_view text) const;
  bool ContainsDateLiteral(std::string_view text) const;
  QueryIntent DetectIntent(std::string_view query) const;
};

/// Classify a query into one of four types using rule-based keyword matching.
QueryType ClassifyQuery(std::string_view query);

}  // namespace waxcpp
