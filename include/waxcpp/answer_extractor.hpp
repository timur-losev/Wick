#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "waxcpp/query_analyzer.hpp"
#include "waxcpp/types.hpp"

namespace waxcpp {

/// Input item for answer extraction (mirrors RAGItem text + score).
struct AnswerExtractionItem {
  float score = 0.0f;
  std::string text;
};

/// Deterministic query-aware answer extractor over retrieved RAG items.
/// Keeps Wax fully offline while producing concise answer spans for
/// benchmarking and deterministic answer-style contexts.
///
/// This is a deterministic heuristic extractor for offline pipelines.
/// It is intentionally lightweight and predictable, but not a substitute for
/// full language-model reasoning.
class DeterministicAnswerExtractor {
 public:
  DeterministicAnswerExtractor() = default;

  /// Extract a concise answer string from query + retrieved items.
  /// Returns empty string if no items contain extractable content.
  std::string ExtractAnswer(std::string_view query,
                            const std::vector<AnswerExtractionItem>& items) const;

  /// Convenience overload that extracts from RAGContext items directly.
  std::string ExtractAnswer(std::string_view query,
                            const std::vector<RAGItem>& items) const;

  // ---- Static utilities (public for testing) ----

  /// Split text into sentences by . ! ? \n delimiters.
  static std::vector<std::string> SplitSentences(std::string_view text);

  /// Apply a regex pattern and return the first match's specified capture group.
  /// Returns empty string if no match.
  static std::string FirstRegexMatch(const std::string& pattern,
                                     std::string_view text,
                                     int capture_group = 0);

 private:
  QueryAnalyzer analyzer_;

  struct AnswerCandidate {
    std::string text;
    double score = 0.0;
  };

  /// Remove highlight brackets and collapse whitespace.
  static std::string CleanText(std::string_view text);

  /// Compute relevance score of text against query signals.
  double RelevanceScore(const std::set<std::string>& query_terms,
                        const std::set<std::string>& query_entities,
                        const std::set<std::string>& query_years,
                        const std::set<std::string>& query_date_keys,
                        std::string_view text,
                        float base_score) const;

  /// Extract ownership candidates from text.
  std::vector<AnswerCandidate> OwnershipCandidates(
      std::string_view text,
      const std::set<std::string>& query_terms,
      double base_score) const;

  /// Extract first launch date from text (looks for "public launch" clauses).
  std::string FirstLaunchDate(std::string_view text) const;

  /// Extract first date literal from text.
  std::string FirstDateLiteral(std::string_view text) const;

  /// Return best candidate by score (ties broken by shorter text).
  static std::string BestCandidate(const std::vector<AnswerCandidate>& candidates);

  /// Return best sentence from texts by lexical overlap with query.
  std::string BestLexicalSentence(std::string_view query,
                                  const std::vector<std::string>& texts) const;

  /// Apply a regex and return ALL matches (each match's specified capture group).
  static std::vector<std::string> AllRegexMatches(const std::string& pattern,
                                                  std::string_view text,
                                                  int capture_group = 0);

  /// Multi-capture regex match: returns a vector of capture groups for each match.
  struct RegexCaptures {
    std::vector<std::string> groups;  // group[0] = full match, group[1+] = captures
  };
  static std::vector<RegexCaptures> AllRegexCaptureMatches(
      const std::string& pattern,
      std::string_view text);
};

}  // namespace waxcpp
