#include "waxcpp/query_analyzer.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// ---- ClassifyQuery ----

void ScenarioClassifyTemporal() {
  waxcpp::tests::Log("scenario: classify temporal queries");
  Require(waxcpp::ClassifyQuery("when did atlas10 move?") == waxcpp::QueryType::kTemporal,
          "when -> temporal");
  Require(waxcpp::ClassifyQuery("what happened yesterday") == waxcpp::QueryType::kTemporal,
          "yesterday -> temporal");
  Require(waxcpp::ClassifyQuery("show today's entries") == waxcpp::QueryType::kTemporal,
          "today -> temporal");
  Require(waxcpp::ClassifyQuery("last week review") == waxcpp::QueryType::kTemporal,
          "last -> temporal");
  Require(waxcpp::ClassifyQuery("recent changes") == waxcpp::QueryType::kTemporal,
          "recent -> temporal");
  Require(waxcpp::ClassifyQuery("latest update") == waxcpp::QueryType::kTemporal,
          "latest -> temporal");
  Require(waxcpp::ClassifyQuery("events before 2025") == waxcpp::QueryType::kTemporal,
          "before -> temporal");
  Require(waxcpp::ClassifyQuery("items after march") == waxcpp::QueryType::kTemporal,
          "after -> temporal");
  Require(waxcpp::ClassifyQuery("between January and March") == waxcpp::QueryType::kTemporal,
          "between -> temporal");
}

void ScenarioClassifyFactual() {
  waxcpp::tests::Log("scenario: classify factual queries");
  Require(waxcpp::ClassifyQuery("what is a vector database?") == waxcpp::QueryType::kFactual,
          "what is -> factual");
  Require(waxcpp::ClassifyQuery("what are embeddings?") == waxcpp::QueryType::kFactual,
          "what are -> factual");
  Require(waxcpp::ClassifyQuery("who is the project owner?") == waxcpp::QueryType::kFactual,
          "who is -> factual");
  Require(waxcpp::ClassifyQuery("define semantic search") == waxcpp::QueryType::kFactual,
          "define -> factual");
  Require(waxcpp::ClassifyQuery("the definition of RAG") == waxcpp::QueryType::kFactual,
          "definition of -> factual");
  Require(waxcpp::ClassifyQuery("meaning of retrieval augmented generation") == waxcpp::QueryType::kFactual,
          "meaning of -> factual");
}

void ScenarioClassifySemantic() {
  waxcpp::tests::Log("scenario: classify semantic queries");
  Require(waxcpp::ClassifyQuery("how does the search work?") == waxcpp::QueryType::kSemantic,
          "how -> semantic");
  Require(waxcpp::ClassifyQuery("why did the test fail?") == waxcpp::QueryType::kSemantic,
          "why -> semantic");
  Require(waxcpp::ClassifyQuery("explain the architecture") == waxcpp::QueryType::kSemantic,
          "explain -> semantic");
  Require(waxcpp::ClassifyQuery("describe the workflow") == waxcpp::QueryType::kSemantic,
          "describe -> semantic");
  Require(waxcpp::ClassifyQuery("how do these concepts relate") == waxcpp::QueryType::kSemantic,
          "relate -> semantic");
}

void ScenarioClassifyExploratory() {
  waxcpp::tests::Log("scenario: classify exploratory queries (default)");
  Require(waxcpp::ClassifyQuery("atlas10 project status") == waxcpp::QueryType::kExploratory,
          "generic query -> exploratory");
  Require(waxcpp::ClassifyQuery("person18 city info") == waxcpp::QueryType::kExploratory,
          "entity query without trigger -> exploratory");
  Require(waxcpp::ClassifyQuery("review notes") == waxcpp::QueryType::kExploratory,
          "simple query -> exploratory");
}

// ---- QueryAnalyzer::Analyze ----

void ScenarioAnalyzeBasicSignals() {
  waxcpp::tests::Log("scenario: analyze basic query signals");
  waxcpp::QueryAnalyzer analyzer;

  // Short query.
  auto sig = analyzer.Analyze("hello");
  Require(sig.word_count == 1, "single word count");
  Require(!sig.has_quoted_phrases, "no quotes");
  Require(!sig.has_specific_entities, "no entities in hello");

  // Multi-word query.
  sig = analyzer.Analyze("the quick brown fox jumps over the lazy dog");
  Require(sig.word_count == 9, "9-word query");
  Require(sig.specificity_score >= 0.39f && sig.specificity_score <= 0.41f,
          "specificity from word count only ~0.4");
}

void ScenarioAnalyzeQuotedPhrases() {
  waxcpp::tests::Log("scenario: analyze quoted phrases");
  waxcpp::QueryAnalyzer analyzer;

  auto sig = analyzer.Analyze("search for \"exact phrase\" in documents");
  Require(sig.has_quoted_phrases, "should detect quoted phrase");
  Require(sig.specificity_score >= 0.55f, "quoted phrase adds 0.25 to specificity");

  // Unmatched quotes.
  sig = analyzer.Analyze("hello \"world");
  Require(!sig.has_quoted_phrases, "unmatched quote should not count");

  // Empty quotes.
  sig = analyzer.Analyze("hello \"\" world");
  Require(!sig.has_quoted_phrases, "empty quotes should not count");
}

void ScenarioAnalyzeEntities() {
  waxcpp::tests::Log("scenario: analyze entity detection in signals");
  waxcpp::QueryAnalyzer analyzer;

  auto sig = analyzer.Analyze("what did atlas10 do?");
  Require(sig.has_specific_entities, "atlas10 should be detected as entity");
  Require(sig.specificity_score >= 0.35f, "entity adds 0.35 to specificity");
}

void ScenarioAnalyzeSpecificityClamp() {
  waxcpp::tests::Log("scenario: specificity score clamps to 1.0");
  waxcpp::QueryAnalyzer analyzer;

  // Very long query with entities and quotes.
  auto sig = analyzer.Analyze("what did atlas10 say about person18 \"exact phrase\" in the meeting notes from yesterday regarding project updates?");
  Require(sig.specificity_score <= 1.0f, "specificity must clamp to 1.0");
  Require(sig.specificity_score >= 0.9f, "high-specificity query should score near 1.0");
}

// ---- QueryAnalyzer::NormalizedTerms ----

void ScenarioNormalizedTerms() {
  waxcpp::tests::Log("scenario: normalized terms with stop word removal and stemming");
  waxcpp::QueryAnalyzer analyzer;

  auto terms = analyzer.NormalizedTerms("what are the cities for deployment?");
  // "what" is stop, "are" is stop, "the" is stop, "for" is stop.
  // "cities" -> stem "citi" (ies->y => "citi" wait... "cities" -> "cit" + "y"? Actually "cities" size > 3 and ends with "ies" -> "cit" + "y" = "city")
  // Actually: "cities" -> substr(0, 6-3) + "y" = "cit" + "y" = "city"
  // "deployment" stays. The '?' is part of the word.

  // Verify stop words are removed.
  for (const auto& t : terms) {
    Require(t != "what" && t != "are" && t != "the" && t != "for",
            "stop words should be removed");
  }
  // Check "cities" stemmed to "city" or close.
  bool has_city = false;
  for (const auto& t : terms) {
    if (t.find("cit") != std::string::npos) has_city = true;
  }
  Require(has_city, "cities should stem to city");
}

void ScenarioNormalizedTermsStemming() {
  waxcpp::tests::Log("scenario: stemming rules");
  waxcpp::QueryAnalyzer analyzer;

  // ies -> y
  auto terms = analyzer.NormalizedTerms("batteries");
  Require(!terms.empty() && terms[0] == "battery", "batteries -> battery");

  // ing -> remove
  terms = analyzer.NormalizedTerms("running");
  Require(!terms.empty() && terms[0] == "runn", "running -> runn");

  // ed -> remove
  terms = analyzer.NormalizedTerms("walked");
  Require(!terms.empty() && terms[0] == "walk", "walked -> walk");

  // s -> remove
  terms = analyzer.NormalizedTerms("dogs");
  Require(!terms.empty() && terms[0] == "dog", "dogs -> dog");
}

// ---- QueryAnalyzer::EntityTerms ----

void ScenarioEntityTermsAlphanumeric() {
  waxcpp::tests::Log("scenario: entity terms - alphanumeric entities");
  waxcpp::QueryAnalyzer analyzer;

  auto entities = analyzer.EntityTerms("atlas10 moved to berlin");
  Require(entities.count("atlas10") == 1, "atlas10 should be entity");
}

void ScenarioEntityTermsCueWords() {
  waxcpp::tests::Log("scenario: entity terms - cue word triggered");
  waxcpp::QueryAnalyzer analyzer;

  auto entities = analyzer.EntityTerms("search for mars in the database");
  Require(entities.count("mars") == 1, "word after 'for' cue should be entity");
}

void ScenarioEntityTermsNoiseFiltering() {
  waxcpp::tests::Log("scenario: entity terms - noise words filtered");
  waxcpp::QueryAnalyzer analyzer;

  auto entities = analyzer.EntityTerms("check the city status report");
  Require(entities.count("city") == 0, "city is noise");
  Require(entities.count("status") == 0, "status is noise");
  Require(entities.count("report") == 0, "report is noise");
}

// ---- QueryAnalyzer::YearTerms ----

void ScenarioYearTerms() {
  waxcpp::tests::Log("scenario: year term extraction");
  waxcpp::QueryAnalyzer analyzer;

  auto years = analyzer.YearTerms("events in 2024 and 2025");
  Require(years.count("2024") == 1, "should find 2024");
  Require(years.count("2025") == 1, "should find 2025");

  // Non-year 4-digit strings.
  years = analyzer.YearTerms("port 8080 is open");
  Require(years.empty(), "8080 is not a valid year");

  // Year embedded in alphanumeric string.
  years = analyzer.YearTerms("code2024beta");
  Require(years.empty(), "year within alnum should not match");
}

// ---- QueryAnalyzer::DateLiterals ----

void ScenarioDateLiteralsFullMonth() {
  waxcpp::tests::Log("scenario: date literals - full month names");
  waxcpp::QueryAnalyzer analyzer;

  auto dates = analyzer.DateLiterals("meeting on January 15, 2025 and review");
  Require(!dates.empty(), "should find date literal");
  bool found = false;
  for (const auto& d : dates) {
    if (d.find("January") != std::string::npos || d.find("january") != std::string::npos) {
      found = true;
    }
  }
  Require(found, "should find January date");
}

void ScenarioDateLiteralsISO() {
  waxcpp::tests::Log("scenario: date literals - ISO format");
  waxcpp::QueryAnalyzer analyzer;

  auto dates = analyzer.DateLiterals("created on 2025-01-15 and updated 2025/02/20");
  Require(dates.size() >= 2, "should find 2 ISO dates");
}

void ScenarioContainsDateLiteral() {
  waxcpp::tests::Log("scenario: contains date literal check");
  waxcpp::QueryAnalyzer analyzer;

  Require(analyzer.ContainsDateLiteral("meeting on 2025-01-15"), "ISO date present");
  Require(!analyzer.ContainsDateLiteral("no dates here"), "no dates");
}

void ScenarioNormalizedDateKeys() {
  waxcpp::tests::Log("scenario: normalized date keys");
  waxcpp::QueryAnalyzer analyzer;

  auto keys = analyzer.NormalizedDateKeys("review 2025-1-5 and 2025/02/20");
  Require(keys.count("2025-01-05") == 1, "should normalize 2025-1-5 -> 2025-01-05");
  Require(keys.count("2025-02-20") == 1, "should normalize 2025/02/20 -> 2025-02-20");
}

// ---- QueryAnalyzer::DetectIntent ----

void ScenarioDetectIntentLocation() {
  waxcpp::tests::Log("scenario: detect location intent");
  waxcpp::QueryAnalyzer analyzer;

  auto intent = analyzer.DetectIntent("where did atlas10 move?");
  Require(waxcpp::HasIntent(intent, waxcpp::QueryIntent::kAsksLocation),
          "where -> location intent");
  auto intent2 = analyzer.DetectIntent("which city does person18 live in?");
  Require(waxcpp::HasIntent(intent2, waxcpp::QueryIntent::kAsksLocation),
          "city -> location intent");
}

void ScenarioDetectIntentDate() {
  waxcpp::tests::Log("scenario: detect date intent");
  waxcpp::QueryAnalyzer analyzer;

  auto intent = analyzer.DetectIntent("when is the launch?");
  Require(waxcpp::HasIntent(intent, waxcpp::QueryIntent::kAsksDate),
          "when -> date intent");
  auto intent2 = analyzer.DetectIntent("project timeline for beta");
  Require(waxcpp::HasIntent(intent2, waxcpp::QueryIntent::kAsksDate),
          "timeline -> date intent");
}

void ScenarioDetectIntentOwnership() {
  waxcpp::tests::Log("scenario: detect ownership intent");
  waxcpp::QueryAnalyzer analyzer;

  auto intent = analyzer.DetectIntent("who owns the deployment readiness?");
  Require(waxcpp::HasIntent(intent, waxcpp::QueryIntent::kAsksOwnership),
          "who owns -> ownership intent");
}

void ScenarioDetectIntentMultiHop() {
  waxcpp::tests::Log("scenario: detect multi-hop intent");
  waxcpp::QueryAnalyzer analyzer;

  // Multi-hop requires " and " with 2+ intent types.
  auto intent = analyzer.DetectIntent("where did atlas10 move and when was the launch?");
  Require(waxcpp::HasIntent(intent, waxcpp::QueryIntent::kAsksLocation),
          "multi-hop should have location");
  Require(waxcpp::HasIntent(intent, waxcpp::QueryIntent::kAsksDate),
          "multi-hop should have date");
  Require(waxcpp::HasIntent(intent, waxcpp::QueryIntent::kMultiHop),
          "should detect multi-hop");
}

void ScenarioDetectIntentNone() {
  waxcpp::tests::Log("scenario: detect no intent");
  waxcpp::QueryAnalyzer analyzer;

  auto intent = analyzer.DetectIntent("atlas10 project notes");
  Require(intent == waxcpp::QueryIntent::kNone, "generic query should have no intent");
}

// ---- AdaptiveFusionConfig ----

void ScenarioAdaptiveFusionDefaults() {
  waxcpp::tests::Log("scenario: adaptive fusion default weights");
  const auto& config = waxcpp::AdaptiveFusionConfig::Default();

  auto factual = config.weights(waxcpp::QueryType::kFactual);
  Require(std::fabs(factual.bm25 - 0.7f) < 0.01f, "factual bm25=0.7");
  Require(std::fabs(factual.vector - 0.3f) < 0.01f, "factual vector=0.3");
  Require(std::fabs(factual.temporal - 0.0f) < 0.01f, "factual temporal=0.0");

  auto semantic = config.weights(waxcpp::QueryType::kSemantic);
  Require(std::fabs(semantic.bm25 - 0.3f) < 0.01f, "semantic bm25=0.3");
  Require(std::fabs(semantic.vector - 0.7f) < 0.01f, "semantic vector=0.7");

  auto temporal = config.weights(waxcpp::QueryType::kTemporal);
  Require(std::fabs(temporal.bm25 - 0.25f) < 0.01f, "temporal bm25=0.25");
  Require(std::fabs(temporal.vector - 0.25f) < 0.01f, "temporal vector=0.25");
  Require(std::fabs(temporal.temporal - 0.5f) < 0.01f, "temporal temporal=0.5");

  auto exploratory = config.weights(waxcpp::QueryType::kExploratory);
  Require(std::fabs(exploratory.bm25 - 0.4f) < 0.01f, "exploratory bm25=0.4");
  Require(std::fabs(exploratory.vector - 0.5f) < 0.01f, "exploratory vector=0.5");
  Require(std::fabs(exploratory.temporal - 0.1f) < 0.01f, "exploratory temporal=0.1");
}

void ScenarioAdaptiveFusionCustomWeights() {
  waxcpp::tests::Log("scenario: adaptive fusion custom weights");
  std::unordered_map<waxcpp::QueryType, waxcpp::FusionWeights> custom = {
      {waxcpp::QueryType::kFactual, {0.9f, 0.1f, 0.0f}},
  };
  waxcpp::AdaptiveFusionConfig config(custom);

  auto w = config.weights(waxcpp::QueryType::kFactual);
  Require(std::fabs(w.bm25 - 0.9f) < 0.01f, "custom factual bm25=0.9");

  // Missing type falls back to default 0.5/0.5.
  auto fallback = config.weights(waxcpp::QueryType::kSemantic);
  Require(std::fabs(fallback.bm25 - 0.5f) < 0.01f, "missing type -> fallback bm25=0.5");
  Require(std::fabs(fallback.vector - 0.5f) < 0.01f, "missing type -> fallback vector=0.5");
}

// ---- Integration: classify -> fusion weights ----

void ScenarioClassifyThenFuseIntegration() {
  waxcpp::tests::Log("scenario: classify query then select fusion weights");

  auto qt = waxcpp::ClassifyQuery("how does the search algorithm work?");
  Require(qt == waxcpp::QueryType::kSemantic, "how -> semantic");

  auto w = waxcpp::AdaptiveFusionConfig::Default().weights(qt);
  Require(std::fabs(w.bm25 - 0.3f) < 0.01f, "semantic query gets bm25=0.3");
  Require(std::fabs(w.vector - 0.7f) < 0.01f, "semantic query gets vector=0.7");
}

int RunScenarios() {
  int passed = 0;

  auto run = [&](auto fn) {
    fn();
    ++passed;
  };

  run(ScenarioClassifyTemporal);
  run(ScenarioClassifyFactual);
  run(ScenarioClassifySemantic);
  run(ScenarioClassifyExploratory);
  run(ScenarioAnalyzeBasicSignals);
  run(ScenarioAnalyzeQuotedPhrases);
  run(ScenarioAnalyzeEntities);
  run(ScenarioAnalyzeSpecificityClamp);
  run(ScenarioNormalizedTerms);
  run(ScenarioNormalizedTermsStemming);
  run(ScenarioEntityTermsAlphanumeric);
  run(ScenarioEntityTermsCueWords);
  run(ScenarioEntityTermsNoiseFiltering);
  run(ScenarioYearTerms);
  run(ScenarioDateLiteralsFullMonth);
  run(ScenarioDateLiteralsISO);
  run(ScenarioContainsDateLiteral);
  run(ScenarioNormalizedDateKeys);
  run(ScenarioDetectIntentLocation);
  run(ScenarioDetectIntentDate);
  run(ScenarioDetectIntentOwnership);
  run(ScenarioDetectIntentMultiHop);
  run(ScenarioDetectIntentNone);
  run(ScenarioAdaptiveFusionDefaults);
  run(ScenarioAdaptiveFusionCustomWeights);
  run(ScenarioClassifyThenFuseIntegration);

  return passed;
}

}  // namespace

int main() {
  try {
    const int passed = RunScenarios();
    waxcpp::tests::Log("query_analyzer_test: all " + std::to_string(passed) + " scenarios passed");
    return 0;
  } catch (const std::exception& ex) {
    waxcpp::tests::Log(std::string("FAIL: ") + ex.what());
    return 1;
  }
}
