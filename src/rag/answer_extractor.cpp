#include "waxcpp/answer_extractor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <set>
#include <string>

namespace waxcpp {

namespace {

// ---- Pre-compiled regex patterns (matching Swift's NSRegularExpression) ----
// Note: std::regex is used with ECMAScript syntax. Patterns adapted from Swift.

const std::string kDeploymentOwnershipPattern =
    R"(\b((?:[A-Z][A-Za-z]*(?:['\-][A-Z][A-Za-z]*)?)(?:\s+(?:[A-Z][A-Za-z]*(?:['\-][A-Z][A-Za-z]*)?)){0,3})\s+owns\s+deployment\s+readiness\b)";

const std::string kGenericOwnershipPattern =
    R"(\b((?:[A-Z][A-Za-z]*(?:['\-][A-Z][A-Za-z]*)?)(?:\s+(?:[A-Z][A-Za-z]*(?:['\-][A-Z][A-Za-z]*)?)){0,3})\s+owns\s+([^.,;\n]+?)(?=\s+and\s+(?:[A-Z][A-Za-z]*(?:['\-][A-Z][A-Za-z]*)?)(?:\s+(?:[A-Z][A-Za-z]*(?:['\-][A-Z][A-Za-z]*)?)){0,3}\s+owns\b|[.,;\n]|$))";

const std::string kAppointmentDateTimePattern =
    R"(\b(?:January|February|March|April|May|June|July|August|September|October|November|December)\s+\d{1,2},\s+\d{4}\s+at\s+\d{1,2}:\d{2}\s*(?:AM|PM)\b)";

const std::string kMovedCityPattern =
    R"(\b[Mm]oved\s+to\s+([A-Z][a-z]+(?:\s+[A-Z][a-z]+)?)\b)";

const std::string kFlightDestinationPattern =
    R"(\b[Ff]light\s+to\s+([A-Z][a-z]+(?:\s+[A-Z][a-z]+)?)\b)";

const std::string kAllergyPattern =
    R"(\ballergic\s+to\s+([A-Za-z]+(?:\s+[A-Za-z]+)?)\b)";

const std::string kPreferencePattern =
    R"(\bprefers\s+([^\.]+))";

const std::string kPetNamePattern =
    R"(\bnamed\s+([A-Z][a-z]+)\b)";

const std::string kAdoptionDatePattern =
    R"(\bin\s+((?:January|February|March|April|May|June|July|August|September|October|November|December)\s+\d{4})\b)";

const std::string kLaunchClausePattern =
    R"(\bpublic\s+launch[^.\n]*)";

std::string ToLower(std::string_view sv) {
  std::string result(sv);
  for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

std::string TrimWhitespace(std::string_view sv) {
  auto start = sv.begin();
  auto end = sv.end();
  while (start != end && std::isspace(static_cast<unsigned char>(*start))) ++start;
  while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
  return std::string(start, end);
}

bool ContainsDigit(std::string_view sv) {
  for (char c : sv) {
    if (std::isdigit(static_cast<unsigned char>(c))) return true;
  }
  return false;
}

}  // namespace

// ---- Static helpers ----

std::string DeterministicAnswerExtractor::CleanText(std::string_view text) {
  // Remove highlight brackets [ and ], then collapse whitespace.
  std::string result;
  result.reserve(text.size());
  for (char c : text) {
    if (c != '[' && c != ']') result += c;
  }
  // Collapse whitespace runs into single space and trim.
  std::string collapsed;
  collapsed.reserve(result.size());
  bool prev_space = false;
  for (char c : result) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!prev_space) {
        collapsed += ' ';
        prev_space = true;
      }
    } else {
      collapsed += c;
      prev_space = false;
    }
  }
  return TrimWhitespace(collapsed);
}

std::string DeterministicAnswerExtractor::FirstRegexMatch(
    const std::string& pattern,
    std::string_view text,
    int capture_group) {
  try {
    const std::regex re(pattern, std::regex_constants::ECMAScript);
    const std::string text_str(text);
    std::smatch match;
    if (std::regex_search(text_str, match, re)) {
      if (capture_group < static_cast<int>(match.size())) {
        return TrimWhitespace(match[capture_group].str());
      }
    }
  } catch (...) {
    // Regex compilation failure — return empty.
  }
  return "";
}

std::vector<std::string> DeterministicAnswerExtractor::AllRegexMatches(
    const std::string& pattern,
    std::string_view text,
    int capture_group) {
  std::vector<std::string> results;
  try {
    const std::regex re(pattern, std::regex_constants::ECMAScript);
    const std::string text_str(text);
    auto it = std::sregex_iterator(text_str.begin(), text_str.end(), re);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
      const auto& match = *it;
      if (capture_group < static_cast<int>(match.size())) {
        auto val = TrimWhitespace(match[capture_group].str());
        if (!val.empty()) results.push_back(std::move(val));
      }
    }
  } catch (...) {}
  return results;
}

std::vector<DeterministicAnswerExtractor::RegexCaptures>
DeterministicAnswerExtractor::AllRegexCaptureMatches(
    const std::string& pattern,
    std::string_view text) {
  std::vector<RegexCaptures> results;
  try {
    const std::regex re(pattern, std::regex_constants::ECMAScript);
    const std::string text_str(text);
    auto it = std::sregex_iterator(text_str.begin(), text_str.end(), re);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
      const auto& match = *it;
      RegexCaptures caps;
      for (std::size_t i = 0; i < match.size(); ++i) {
        caps.groups.push_back(TrimWhitespace(match[i].str()));
      }
      results.push_back(std::move(caps));
    }
  } catch (...) {}
  return results;
}

std::vector<std::string> DeterministicAnswerExtractor::SplitSentences(
    std::string_view text) {
  std::vector<std::string> sentences;
  std::string current;
  for (char c : text) {
    if (c == '.' || c == '!' || c == '?' || c == '\n') {
      auto trimmed = TrimWhitespace(current);
      if (!trimmed.empty()) sentences.push_back(std::move(trimmed));
      current.clear();
    } else {
      current += c;
    }
  }
  auto trimmed = TrimWhitespace(current);
  if (!trimmed.empty()) sentences.push_back(std::move(trimmed));
  return sentences;
}

std::string DeterministicAnswerExtractor::BestCandidate(
    const std::vector<AnswerCandidate>& candidates) {
  if (candidates.empty()) return "";
  const auto* best = &candidates[0];
  for (std::size_t i = 1; i < candidates.size(); ++i) {
    if (candidates[i].score > best->score ||
        (candidates[i].score == best->score &&
         candidates[i].text.size() < best->text.size())) {
      best = &candidates[i];
    }
  }
  return best->text;
}

// ---- Instance methods ----

double DeterministicAnswerExtractor::RelevanceScore(
    const std::set<std::string>& query_terms,
    const std::set<std::string>& query_entities,
    const std::set<std::string>& query_years,
    const std::set<std::string>& query_date_keys,
    std::string_view text,
    float base_score) const {
  double score = static_cast<double>(base_score);
  if (query_terms.empty() && query_entities.empty() &&
      query_years.empty() && query_date_keys.empty()) {
    return score;
  }

  const auto terms_vec = analyzer_.NormalizedTerms(text);
  const std::set<std::string> terms(terms_vec.begin(), terms_vec.end());

  if (!query_terms.empty() && !terms.empty()) {
    int overlap = 0;
    for (const auto& qt : query_terms) {
      if (terms.count(qt)) ++overlap;
    }
    const double recall =
        static_cast<double>(overlap) / static_cast<double>(std::max<int>(1, static_cast<int>(query_terms.size())));
    const double precision =
        static_cast<double>(overlap) / static_cast<double>(std::max<int>(1, static_cast<int>(terms.size())));
    score += recall * 0.70 + precision * 0.30;
  }

  if (!query_entities.empty()) {
    const auto text_entities = analyzer_.EntityTerms(text);
    int hits = 0;
    for (const auto& qe : query_entities) {
      if (text_entities.count(qe)) ++hits;
    }
    const double coverage =
        static_cast<double>(hits) / static_cast<double>(std::max<int>(1, static_cast<int>(query_entities.size())));
    score += coverage * 0.95;
    if (hits == 0) {
      score -= 0.70;
    }
  }

  if (!query_years.empty()) {
    const auto text_years = analyzer_.YearTerms(text);
    int hits = 0;
    for (const auto& qy : query_years) {
      if (text_years.count(qy)) ++hits;
    }
    const double coverage =
        static_cast<double>(hits) / static_cast<double>(std::max<int>(1, static_cast<int>(query_years.size())));
    score += coverage * 1.45;
    if (hits == 0 && !text_years.empty()) {
      score -= 1.35;
    }
  }

  if (!query_date_keys.empty()) {
    const auto text_date_keys = analyzer_.NormalizedDateKeys(text);
    int hits = 0;
    for (const auto& qd : query_date_keys) {
      if (text_date_keys.count(qd)) ++hits;
    }
    const double coverage =
        static_cast<double>(hits) / static_cast<double>(std::max<int>(1, static_cast<int>(query_date_keys.size())));
    score += coverage * 1.25;
    if (hits == 0 && !text_date_keys.empty()) {
      score -= 1.10;
    }
  }

  return score;
}

std::vector<DeterministicAnswerExtractor::AnswerCandidate>
DeterministicAnswerExtractor::OwnershipCandidates(
    std::string_view text,
    const std::set<std::string>& query_terms,
    double base_score) const {
  std::vector<AnswerCandidate> candidates;

  // 1. Deployment ownership pattern: "X owns deployment readiness"
  auto deployment_owner = FirstRegexMatch(kDeploymentOwnershipPattern, text, 1);
  if (!deployment_owner.empty()) {
    candidates.push_back({std::move(deployment_owner), base_score + 0.60});
  }

  // 2. Generic ownership pattern: "X owns Y"
  auto matches = AllRegexCaptureMatches(kGenericOwnershipPattern, text);
  for (const auto& caps : matches) {
    if (caps.groups.size() < 3) continue;
    const auto& owner = caps.groups[1];
    const auto& topic = caps.groups[2];
    if (owner.empty() || topic.empty()) continue;

    double score = base_score + 0.40;
    const auto topic_terms_vec = analyzer_.NormalizedTerms(topic);
    const std::set<std::string> topic_terms(topic_terms_vec.begin(), topic_terms_vec.end());
    if (!query_terms.empty() && !topic_terms.empty()) {
      int overlap = 0;
      for (const auto& qt : query_terms) {
        if (topic_terms.count(qt)) ++overlap;
      }
      const double recall =
          static_cast<double>(overlap) / static_cast<double>(std::max<int>(1, static_cast<int>(query_terms.size())));
      const double precision =
          static_cast<double>(overlap) / static_cast<double>(std::max<int>(1, static_cast<int>(topic_terms.size())));
      score += recall * 0.80 + precision * 0.25;
    }

    // Deployment readiness topic bonus.
    if (ToLower(topic).find("deployment readiness") != std::string::npos) {
      score += 0.20;
    }

    candidates.push_back({owner, score});
  }

  return candidates;
}

std::string DeterministicAnswerExtractor::FirstLaunchDate(
    std::string_view text) const {
  // Find "public launch..." clauses, then extract first date literal from each.
  auto clauses = AllRegexMatches(kLaunchClausePattern, text, 0);
  for (const auto& clause : clauses) {
    auto dates = analyzer_.DateLiterals(clause);
    if (!dates.empty()) return dates[0];
  }
  return "";
}

std::string DeterministicAnswerExtractor::FirstDateLiteral(
    std::string_view text) const {
  auto dates = analyzer_.DateLiterals(text);
  return dates.empty() ? "" : dates[0];
}

std::string DeterministicAnswerExtractor::BestLexicalSentence(
    std::string_view query,
    const std::vector<std::string>& texts) const {
  const auto query_terms_vec = analyzer_.NormalizedTerms(query);
  const std::set<std::string> query_terms(query_terms_vec.begin(), query_terms_vec.end());
  if (query_terms.empty()) {
    return texts.empty() ? "" : texts[0];
  }

  std::vector<std::string> all_sentences;
  for (const auto& t : texts) {
    auto sents = SplitSentences(t);
    all_sentences.insert(all_sentences.end(),
                         std::make_move_iterator(sents.begin()),
                         std::make_move_iterator(sents.end()));
  }

  std::string best_text;
  double best_score = -1.0;

  for (const auto& sentence : all_sentences) {
    const auto norm = analyzer_.NormalizedTerms(sentence);
    if (norm.empty()) continue;
    const std::set<std::string> norm_set(norm.begin(), norm.end());
    int overlap = 0;
    for (const auto& qt : query_terms) {
      if (norm_set.count(qt)) ++overlap;
    }
    const double overlap_score =
        static_cast<double>(overlap) / static_cast<double>(std::max<int>(1, static_cast<int>(norm.size())));
    const double numeric_bonus = ContainsDigit(sentence) ? 0.15 : 0.0;
    const double score = overlap_score + numeric_bonus;

    if (score > best_score ||
        (score == best_score && sentence.size() < best_text.size())) {
      best_score = score;
      best_text = sentence;
    }
  }

  return best_text;
}

// ---- Public API ----

std::string DeterministicAnswerExtractor::ExtractAnswer(
    std::string_view query,
    const std::vector<AnswerExtractionItem>& items) const {
  // Clean and filter items.
  struct NormalizedItem {
    float score;
    std::string text;
  };
  std::vector<NormalizedItem> normalized;
  normalized.reserve(items.size());
  for (const auto& item : items) {
    auto clean = CleanText(item.text);
    if (!clean.empty()) {
      normalized.push_back({item.score, std::move(clean)});
    }
  }
  if (normalized.empty()) return "";

  // Query analysis.
  const auto lower_query = ToLower(query);
  const auto query_terms_vec = analyzer_.NormalizedTerms(query);
  const std::set<std::string> query_terms(query_terms_vec.begin(), query_terms_vec.end());
  const auto query_entities = analyzer_.EntityTerms(query);
  const auto query_years = analyzer_.YearTerms(query);
  const auto query_date_keys = analyzer_.NormalizedDateKeys(query);
  const auto intent = analyzer_.DetectIntent(query);

  const bool asks_travel =
      lower_query.find("flying") != std::string::npos ||
      lower_query.find("flight") != std::string::npos ||
      lower_query.find("travel") != std::string::npos;
  const bool asks_allergy =
      lower_query.find("allergy") != std::string::npos ||
      lower_query.find("allergic") != std::string::npos;
  const bool asks_communication_style =
      lower_query.find("status update") != std::string::npos ||
      lower_query.find("written") != std::string::npos;
  const bool asks_pet =
      lower_query.find("dog") != std::string::npos ||
      lower_query.find("pet") != std::string::npos ||
      lower_query.find("adopt") != std::string::npos;
  const bool asks_dentist =
      lower_query.find("dentist") != std::string::npos ||
      lower_query.find("appointment") != std::string::npos;

  // Candidate collectors.
  std::vector<AnswerCandidate> owner_candidates;
  std::vector<AnswerCandidate> date_candidates;
  std::vector<AnswerCandidate> launch_date_candidates;
  std::vector<AnswerCandidate> appointment_datetime_candidates;
  std::vector<AnswerCandidate> city_candidates;
  std::vector<AnswerCandidate> flight_destination_candidates;
  std::vector<AnswerCandidate> allergy_candidates;
  std::vector<AnswerCandidate> preference_candidates;
  std::vector<AnswerCandidate> pet_name_candidates;
  std::vector<AnswerCandidate> adoption_date_candidates;

  for (const auto& item : normalized) {
    const double relevance = RelevanceScore(
        query_terms, query_entities, query_years, query_date_keys,
        item.text, item.score);

    // Ownership.
    auto owners = OwnershipCandidates(item.text, query_terms, relevance);
    owner_candidates.insert(owner_candidates.end(),
                            std::make_move_iterator(owners.begin()),
                            std::make_move_iterator(owners.end()));

    // Launch date.
    auto launch = FirstLaunchDate(item.text);
    if (!launch.empty()) {
      launch_date_candidates.push_back({std::move(launch), relevance + 0.55});
    }

    // Appointment date/time.
    auto appointment = FirstRegexMatch(kAppointmentDateTimePattern, item.text, 0);
    if (!appointment.empty()) {
      appointment_datetime_candidates.push_back({std::move(appointment), relevance + 0.55});
    }

    // City (moved to).
    auto city = FirstRegexMatch(kMovedCityPattern, item.text, 1);
    if (!city.empty()) {
      city_candidates.push_back({std::move(city), relevance + 0.45});
    }

    // Flight destination.
    auto dest = FirstRegexMatch(kFlightDestinationPattern, item.text, 1);
    if (!dest.empty()) {
      flight_destination_candidates.push_back({std::move(dest), relevance + 0.45});
    }

    // Allergy.
    auto allergy = FirstRegexMatch(kAllergyPattern, item.text, 1);
    if (!allergy.empty()) {
      allergy_candidates.push_back({"allergic to " + allergy, relevance + 0.40});
    }

    // Preference.
    auto pref = FirstRegexMatch(kPreferencePattern, item.text, 1);
    if (!pref.empty()) {
      preference_candidates.push_back({std::move(pref), relevance + 0.35});
    }

    // Pet name.
    auto pet = FirstRegexMatch(kPetNamePattern, item.text, 1);
    if (!pet.empty()) {
      pet_name_candidates.push_back({std::move(pet), relevance + 0.40});
    }

    // Adoption date.
    auto adoption = FirstRegexMatch(kAdoptionDatePattern, item.text, 1);
    if (!adoption.empty()) {
      adoption_date_candidates.push_back({std::move(adoption), relevance + 0.40});
    }

    // Generic date.
    auto date = FirstDateLiteral(item.text);
    if (!date.empty()) {
      date_candidates.push_back({std::move(date), relevance + 0.20});
    }
  }

  // ---- Intent-based answer selection (matching Swift priority order) ----

  // Pet question: "pet_name in adoption_date"
  if (asks_pet) {
    auto pet = BestCandidate(pet_name_candidates);
    auto adopted = BestCandidate(adoption_date_candidates);
    if (!pet.empty() && !adopted.empty()) {
      return pet + " in " + adopted;
    }
  }

  // Combined ownership + date.
  if (HasIntent(intent, QueryIntent::kAsksOwnership) &&
      HasIntent(intent, QueryIntent::kAsksDate)) {
    auto owner = BestCandidate(owner_candidates);
    if (!owner.empty()) {
      auto date = BestCandidate(launch_date_candidates);
      if (date.empty()) date = BestCandidate(date_candidates);
      if (!date.empty()) {
        return owner + " and " + date;
      }
    }
  }

  // Communication style.
  if (asks_communication_style) {
    auto style = BestCandidate(preference_candidates);
    if (!style.empty()) return style;
  }

  // Allergy.
  if (asks_allergy) {
    auto allergy = BestCandidate(allergy_candidates);
    if (!allergy.empty()) return allergy;
  }

  // Travel.
  if (asks_travel) {
    auto dest = BestCandidate(flight_destination_candidates);
    if (!dest.empty()) return dest;
  }

  // Location intent.
  if (HasIntent(intent, QueryIntent::kAsksLocation)) {
    if (asks_travel) {
      auto dest = BestCandidate(flight_destination_candidates);
      if (!dest.empty()) return dest;
    }
    auto city = BestCandidate(city_candidates);
    if (!city.empty()) return city;
  }

  // Date intent.
  if (HasIntent(intent, QueryIntent::kAsksDate)) {
    if (asks_dentist) {
      auto appointment = BestCandidate(appointment_datetime_candidates);
      if (!appointment.empty()) return appointment;
    }
    auto launch = BestCandidate(launch_date_candidates);
    if (!launch.empty()) return launch;
    auto date = BestCandidate(date_candidates);
    if (!date.empty()) return date;
  }

  // Ownership intent.
  if (HasIntent(intent, QueryIntent::kAsksOwnership)) {
    auto owner = BestCandidate(owner_candidates);
    if (!owner.empty()) return owner;
  }

  // Fallback: best lexical sentence.
  std::vector<std::string> texts;
  texts.reserve(normalized.size());
  for (const auto& n : normalized) texts.push_back(n.text);

  auto best = BestLexicalSentence(query, texts);
  return best.empty() ? texts[0] : best;
}

std::string DeterministicAnswerExtractor::ExtractAnswer(
    std::string_view query,
    const std::vector<RAGItem>& items) const {
  std::vector<AnswerExtractionItem> extraction_items;
  extraction_items.reserve(items.size());
  for (const auto& item : items) {
    extraction_items.push_back({item.score, item.text});
  }
  return ExtractAnswer(query, extraction_items);
}

}  // namespace waxcpp
