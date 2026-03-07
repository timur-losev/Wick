#include "waxcpp/query_analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace waxcpp {
namespace {

// --- ASCII helpers (locale-independent) ---

char AsciiLower(char ch) {
  if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch + ('a' - 'A'));
  return ch;
}

std::string ToAsciiLower(std::string_view sv) {
  std::string out;
  out.reserve(sv.size());
  for (const char ch : sv) {
    out.push_back(AsciiLower(ch));
  }
  return out;
}

bool IsAsciiAlnum(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9');
}

bool IsAsciiDigit(char ch) { return ch >= '0' && ch <= '9'; }
bool IsAsciiAlpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

bool IsAsciiWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

// Split on whitespace, return lowercased tokens.
std::vector<std::string> SplitWords(std::string_view text) {
  std::vector<std::string> words;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && IsAsciiWhitespace(text[i])) ++i;
    if (i >= text.size()) break;
    std::size_t start = i;
    while (i < text.size() && !IsAsciiWhitespace(text[i])) ++i;
    words.push_back(ToAsciiLower(text.substr(start, i - start)));
  }
  return words;
}

// Check if a string starts with a prefix (case-insensitive).
bool StartsWithCI(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (AsciiLower(text[i]) != AsciiLower(prefix[i])) return false;
  }
  return true;
}

// Check if lowered text contains a substring.
bool ContainsCI(std::string_view text, std::string_view needle) {
  const auto lowered = ToAsciiLower(text);
  const auto lowered_needle = ToAsciiLower(needle);
  return lowered.find(lowered_needle) != std::string::npos;
}

// Stop words for normalization (Swift parity).
const std::unordered_set<std::string>& StopWords() {
  static const std::unordered_set<std::string> words = {
      "a",     "an",    "and",   "are",  "at",    "did",   "do",
      "for",   "from",  "in",    "is",   "of",    "on",    "or",
      "the",   "to",    "what",  "when", "where", "which", "who",
      "with",
  };
  return words;
}

// Entity cue words.
const std::unordered_set<std::string>& EntityCueWords() {
  static const std::unordered_set<std::string> words = {
      "for", "about", "did", "does", "with", "from",
  };
  return words;
}

// Name follower cue words.
const std::unordered_set<std::string>& NameFollowerCues() {
  static const std::unordered_set<std::string> words = {
      "moved", "move", "owns", "owned", "launch", "launched",
  };
  return words;
}

// Entity noise terms.
const std::unordered_set<std::string>& EntityNoiseTerms() {
  static const std::unordered_set<std::string> words = {
      "city",       "date",     "owner",     "owns",       "launch",
      "public",     "project",  "beta",      "deployment", "readiness",
      "timeline",   "status",   "updates",   "update",     "report",
      "checklist",  "signoff",  "team",      "health",     "allergic",
  };
  return words;
}

// Check if a word looks like an entity (contains both alpha and digit).
bool LooksLikeEntity(std::string_view word) {
  bool has_alpha = false;
  bool has_digit = false;
  for (char ch : word) {
    if (IsAsciiAlpha(ch)) has_alpha = true;
    if (IsAsciiDigit(ch)) has_digit = true;
    if (has_alpha && has_digit) return true;
  }
  return false;
}

// Simple stemming rules (Swift parity: ies->y, ing->, ed->, s->).
std::string SimpleStem(const std::string& word) {
  if (word.size() > 3 && word.substr(word.size() - 3) == "ies") {
    return word.substr(0, word.size() - 3) + "y";
  }
  if (word.size() > 3 && word.substr(word.size() - 3) == "ing") {
    return word.substr(0, word.size() - 3);
  }
  if (word.size() > 2 && word.substr(word.size() - 2) == "ed") {
    return word.substr(0, word.size() - 2);
  }
  if (word.size() > 2 && word.back() == 's') {
    return word.substr(0, word.size() - 1);
  }
  return word;
}

// Check if text has quoted phrases (double quotes with content).
bool HasQuotedPhrases(std::string_view text) {
  auto pos = text.find('"');
  while (pos != std::string_view::npos) {
    auto close = text.find('"', pos + 1);
    if (close != std::string_view::npos && close > pos + 1) {
      return true;
    }
    if (close == std::string_view::npos) break;
    pos = text.find('"', close + 1);
  }
  return false;
}

// --- Month names for date parsing ---
struct MonthEntry {
  const char* full;
  const char* abbrev;
};

const MonthEntry kMonths[] = {
    {"january", "jan"},   {"february", "feb"},  {"march", "mar"},
    {"april", "apr"},     {"may", "may"},        {"june", "jun"},
    {"july", "jul"},      {"august", "aug"},     {"september", "sep"},
    {"october", "oct"},   {"november", "nov"},   {"december", "dec"},
};

}  // namespace

// ---- QueryAnalyzer ----

QuerySignals QueryAnalyzer::Analyze(std::string_view query) const {
  QuerySignals signals{};
  const auto words = SplitWords(query);
  signals.word_count = static_cast<int>(words.size());
  signals.has_quoted_phrases = HasQuotedPhrases(query);

  // Check for specific entities.
  const auto entities = EntityTerms(query);
  signals.has_specific_entities = !entities.empty();

  // Specificity score: min(wordCount/8, 0.4) + entities*0.35 + quotes*0.25
  float score = std::min(static_cast<float>(signals.word_count) / 8.0f, 0.4f);
  if (signals.has_specific_entities) score += 0.35f;
  if (signals.has_quoted_phrases) score += 0.25f;
  signals.specificity_score = std::min(score, 1.0f);

  return signals;
}

std::vector<std::string> QueryAnalyzer::NormalizedTerms(std::string_view query) const {
  const auto words = SplitWords(query);
  const auto& stops = StopWords();
  std::vector<std::string> terms;
  terms.reserve(words.size());
  for (const auto& w : words) {
    if (stops.count(w)) continue;
    terms.push_back(SimpleStem(w));
  }
  return terms;
}

std::set<std::string> QueryAnalyzer::EntityTerms(std::string_view query) const {
  const auto words = SplitWords(query);
  const auto& stops = StopWords();
  const auto& cues = EntityCueWords();
  const auto& followers = NameFollowerCues();
  const auto& noise = EntityNoiseTerms();

  std::set<std::string> entities;
  bool after_cue = false;

  for (std::size_t i = 0; i < words.size(); ++i) {
    const auto& w = words[i];

    // Entity-like: alphanumeric mix (e.g., "atlas10", "person18").
    if (LooksLikeEntity(w) && !noise.count(w)) {
      entities.insert(w);
      after_cue = false;
      continue;
    }

    // After an entity cue word, the next non-stop word is an entity candidate.
    if (cues.count(w)) {
      after_cue = true;
      continue;
    }

    // After a name follower cue, pick up entity-like next word.
    if (i > 0 && followers.count(words[i - 1]) && !stops.count(w) && !noise.count(w)) {
      entities.insert(w);
      after_cue = false;
      continue;
    }

    if (after_cue && !stops.count(w) && !noise.count(w)) {
      entities.insert(w);
      after_cue = false;
      continue;
    }

    after_cue = false;
  }
  return entities;
}

std::set<std::string> QueryAnalyzer::YearTerms(std::string_view text) const {
  std::set<std::string> years;
  // Find standalone 4-digit years (1900-2099).
  std::size_t i = 0;
  while (i < text.size()) {
    // Skip to digit.
    while (i < text.size() && !IsAsciiDigit(text[i])) ++i;
    if (i >= text.size()) break;
    std::size_t start = i;
    while (i < text.size() && IsAsciiDigit(text[i])) ++i;
    const std::size_t len = i - start;
    if (len == 4) {
      // Check not adjacent to more digits.
      bool isolated = true;
      if (start > 0 && IsAsciiAlnum(text[start - 1])) isolated = false;
      if (i < text.size() && IsAsciiAlnum(text[i])) isolated = false;
      if (isolated) {
        auto year_str = std::string(text.substr(start, 4));
        int year_val = std::stoi(year_str);
        if (year_val >= 1900 && year_val <= 2099) {
          years.insert(year_str);
        }
      }
    }
  }
  return years;
}

std::vector<std::string> QueryAnalyzer::DateLiterals(std::string_view text) const {
  std::vector<std::string> dates;
  const std::string lowered = ToAsciiLower(text);

  // Pattern 1: Full month name "January 15, 2025" or "January 15 2025"
  // Pattern 2: Abbreviated "Jan 15, 2025" or "Jan 15 2025"
  // Pattern 3: Day-first "15 January 2025" or "15 Jan 2025"
  for (const auto& month : kMonths) {
    const std::string full_name = month.full;
    const std::string abbrev = month.abbrev;

    // Search for month name occurrences.
    for (const auto* name : {&full_name, &abbrev}) {
      std::size_t pos = 0;
      while ((pos = lowered.find(*name, pos)) != std::string::npos) {
        // Check it's a word boundary before.
        if (pos > 0 && IsAsciiAlpha(lowered[pos - 1])) {
          // Not if just an abbreviation within a word, unless exact abbreviation match.
          if (name->size() < full_name.size()) {
            // Abbreviated: must be word boundary.
            pos += name->size();
            continue;
          }
          pos += name->size();
          continue;
        }
        // Check word boundary after.
        std::size_t after_name = pos + name->size();
        if (after_name < lowered.size() && IsAsciiAlpha(lowered[after_name])) {
          // Part of a longer word - skip for abbreviations.
          if (name->size() == abbrev.size() && name->size() < full_name.size()) {
            pos += name->size();
            continue;
          }
        }

        // Try "Month D[D][,] YYYY" pattern.
        std::size_t j = after_name;
        while (j < lowered.size() && IsAsciiWhitespace(lowered[j])) ++j;
        if (j < lowered.size() && IsAsciiDigit(lowered[j])) {
          std::size_t day_start = j;
          while (j < lowered.size() && IsAsciiDigit(lowered[j])) ++j;
          if (j - day_start <= 2) {
            // Optional comma.
            std::size_t k = j;
            if (k < lowered.size() && lowered[k] == ',') ++k;
            while (k < lowered.size() && IsAsciiWhitespace(lowered[k])) ++k;
            // Year.
            if (k < lowered.size() && IsAsciiDigit(lowered[k])) {
              std::size_t year_start = k;
              while (k < lowered.size() && IsAsciiDigit(lowered[k])) ++k;
              if (k - year_start == 4) {
                dates.emplace_back(text.substr(pos, k - pos));
              }
            }
          }
        }

        // Try "D[D] Month YYYY" (day-first) - look backwards.
        if (pos > 0) {
          std::size_t back = pos;
          while (back > 0 && IsAsciiWhitespace(lowered[back - 1])) --back;
          if (back > 0 && IsAsciiDigit(lowered[back - 1])) {
            std::size_t day_end = back;
            while (back > 0 && IsAsciiDigit(lowered[back - 1])) --back;
            if (day_end - back <= 2) {
              // Now look for year after month name.
              std::size_t fwd = after_name;
              while (fwd < lowered.size() && IsAsciiWhitespace(lowered[fwd])) ++fwd;
              if (fwd < lowered.size() && IsAsciiDigit(lowered[fwd])) {
                std::size_t year_start = fwd;
                while (fwd < lowered.size() && IsAsciiDigit(lowered[fwd])) ++fwd;
                if (fwd - year_start == 4) {
                  dates.emplace_back(text.substr(back, fwd - back));
                }
              }
            }
          }
        }

        pos += name->size();
      }
    }
  }

  // Pattern 4: ISO-like YYYY[-/.]MM[-/.]DD
  {
    const std::regex iso_re(R"(\b(\d{4})[-/.](\d{1,2})[-/.](\d{1,2})\b)");
    std::string text_str(text);
    std::sregex_iterator it(text_str.begin(), text_str.end(), iso_re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
      dates.push_back((*it)[0].str());
    }
  }

  return dates;
}

std::set<std::string> QueryAnalyzer::NormalizedDateKeys(std::string_view text) const {
  std::set<std::string> keys;
  // Extract ISO dates from the text.
  const std::string text_str(text);
  const std::regex iso_re(R"(\b(\d{4})[-/.](\d{1,2})[-/.](\d{1,2})\b)");
  std::sregex_iterator it(text_str.begin(), text_str.end(), iso_re);
  std::sregex_iterator end;
  for (; it != end; ++it) {
    int year = std::stoi((*it)[1].str());
    int month = std::stoi((*it)[2].str());
    int day = std::stoi((*it)[3].str());
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
      keys.insert(buf);
    }
  }
  return keys;
}

bool QueryAnalyzer::ContainsDateLiteral(std::string_view text) const {
  return !DateLiterals(text).empty();
}

QueryIntent QueryAnalyzer::DetectIntent(std::string_view query) const {
  const auto lowered = ToAsciiLower(query);
  QueryIntent intent = QueryIntent::kNone;
  int intent_count = 0;

  // Location intent.
  if (ContainsCI(query, "city") || ContainsCI(query, "where") ||
      ContainsCI(query, "move") || ContainsCI(query, "mov") ||
      ContainsCI(query, "moved") || ContainsCI(query, "moving")) {
    intent = intent | QueryIntent::kAsksLocation;
    ++intent_count;
  }

  // Date intent.
  if (ContainsCI(query, "date") || ContainsCI(query, "when") ||
      ContainsCI(query, "launch") || ContainsCI(query, "timeline")) {
    intent = intent | QueryIntent::kAsksDate;
    ++intent_count;
  }

  // Ownership intent.
  if (ContainsCI(query, "who") || ContainsCI(query, "owner") ||
      ContainsCI(query, "owns") || ContainsCI(query, "deployment readiness")) {
    intent = intent | QueryIntent::kAsksOwnership;
    ++intent_count;
  }

  // Multi-hop: " and " appears with multiple intents.
  if (ContainsCI(query, " and ") && intent_count >= 2) {
    intent = intent | QueryIntent::kMultiHop;
  }

  return intent;
}

// ---- ClassifyQuery ----

QueryType ClassifyQuery(std::string_view query) {
  const auto lowered = ToAsciiLower(query);

  // Temporal triggers.
  if (lowered.find("when") != std::string::npos ||
      lowered.find("yesterday") != std::string::npos ||
      lowered.find("today") != std::string::npos ||
      lowered.find("last ") != std::string::npos ||
      lowered.find("recent") != std::string::npos ||
      lowered.find("latest") != std::string::npos ||
      lowered.find("before ") != std::string::npos ||
      lowered.find("after ") != std::string::npos ||
      lowered.find("between ") != std::string::npos) {
    return QueryType::kTemporal;
  }

  // Factual triggers.
  if (StartsWithCI(lowered, "what is") || StartsWithCI(lowered, "what are") ||
      StartsWithCI(lowered, "who is") || StartsWithCI(lowered, "who are") ||
      lowered.find("define ") != std::string::npos ||
      lowered.find("definition of") != std::string::npos ||
      lowered.find("meaning of") != std::string::npos) {
    return QueryType::kFactual;
  }

  // Semantic triggers.
  if (lowered.find("how ") != std::string::npos ||
      lowered.find("why ") != std::string::npos ||
      lowered.find("explain") != std::string::npos ||
      lowered.find("describe") != std::string::npos ||
      lowered.find("relate") != std::string::npos) {
    return QueryType::kSemantic;
  }

  return QueryType::kExploratory;
}

// ---- AdaptiveFusionConfig ----

AdaptiveFusionConfig::AdaptiveFusionConfig() {
  weights_[QueryType::kFactual] = FusionWeights{0.7f, 0.3f, 0.0f};
  weights_[QueryType::kSemantic] = FusionWeights{0.3f, 0.7f, 0.0f};
  weights_[QueryType::kTemporal] = FusionWeights{0.25f, 0.25f, 0.5f};
  weights_[QueryType::kExploratory] = FusionWeights{0.4f, 0.5f, 0.1f};
}

AdaptiveFusionConfig::AdaptiveFusionConfig(
    std::unordered_map<QueryType, FusionWeights> weights)
    : weights_(std::move(weights)) {}

FusionWeights AdaptiveFusionConfig::weights(QueryType query_type) const {
  auto it = weights_.find(query_type);
  if (it != weights_.end()) {
    return it->second;
  }
  // Fallback.
  return FusionWeights{0.5f, 0.5f, 0.0f};
}

const AdaptiveFusionConfig& AdaptiveFusionConfig::Default() {
  static const AdaptiveFusionConfig instance;
  return instance;
}

}  // namespace waxcpp
