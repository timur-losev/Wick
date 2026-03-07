#include "waxcpp/answer_extractor.hpp"
#include "../test_logger.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace waxcpp;
using namespace waxcpp::tests;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* label) {
  if (condition) {
    ++g_pass;
    Log(std::string("  PASS: ") + label);
  } else {
    ++g_fail;
    LogError(std::string("  FAIL: ") + label);
  }
}

AnswerExtractionItem MakeItem(float score, const std::string& text) {
  return {score, text};
}

// ============================================================
// 1. Empty / trivial input
// ============================================================

void TestEmptyItems() {
  Log("=== TestEmptyItems ===");
  DeterministicAnswerExtractor extractor;
  auto result = extractor.ExtractAnswer("Who owns X?", std::vector<AnswerExtractionItem>{});
  Check(result.empty(), "empty items returns empty string");
}

void TestAllWhitespaceItems() {
  Log("=== TestAllWhitespaceItems ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {MakeItem(1.0f, "   "), MakeItem(0.5f, "\n\t ")};
  auto result = extractor.ExtractAnswer("Who owns X?", items);
  Check(result.empty(), "all whitespace items returns empty string");
}

// ============================================================
// 2. CleanText: removes brackets and collapses whitespace
// ============================================================

void TestCleanTextRemovesBrackets() {
  Log("=== TestCleanTextRemovesBrackets ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(1.0f, "[Alice] owns [deployment readiness]"),
  };
  auto result = extractor.ExtractAnswer("Who owns deployment readiness?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result.find("Alice") != std::string::npos, "contains Alice after bracket removal");
}

// ============================================================
// 3. Ownership extraction
// ============================================================

void TestDeploymentOwnership() {
  Log("=== TestDeploymentOwnership ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "Alice Chen owns deployment readiness for the project."),
  };
  auto result = extractor.ExtractAnswer("Who owns deployment readiness?", items);
  Check(result == "Alice Chen", "deployment ownership extracted");
  LogKV("result", result);
}

void TestGenericOwnership() {
  Log("=== TestGenericOwnership ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.8f, "Bob Smith owns backend infrastructure, and Carol Davis owns frontend platform."),
  };
  auto result = extractor.ExtractAnswer("Who owns backend infrastructure?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result == "Bob Smith", "generic ownership extracted");
  LogKV("result", result);
}

void TestOwnershipWithDate() {
  Log("=== TestOwnershipWithDate ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "Alice owns deployment readiness. The public launch is set for March 15, 2025."),
  };
  auto result = extractor.ExtractAnswer("Who owns deployment readiness and when is the launch?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result.find("Alice") != std::string::npos, "contains owner");
  Check(result.find("March 15, 2025") != std::string::npos, "contains date");
  LogKV("result", result);
}

// ============================================================
// 4. Date extraction
// ============================================================

void TestLaunchDate() {
  Log("=== TestLaunchDate ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "The public launch is scheduled for June 1, 2025."),
  };
  auto result = extractor.ExtractAnswer("When is the public launch date?", items);
  Check(result == "June 1, 2025", "launch date extracted");
  LogKV("result", result);
}

void TestGenericDateFallback() {
  Log("=== TestGenericDateFallback ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.8f, "The meeting was held on January 10, 2025 to discuss progress."),
  };
  auto result = extractor.ExtractAnswer("When was the meeting?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result.find("January 10, 2025") != std::string::npos, "contains date");
  LogKV("result", result);
}

void TestAppointmentDateTime() {
  Log("=== TestAppointmentDateTime ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "Next dentist appointment is February 14, 2025 at 2:30 PM."),
  };
  auto result = extractor.ExtractAnswer("When is the dentist appointment?", items);
  Check(result == "February 14, 2025 at 2:30 PM", "appointment date+time extracted");
  LogKV("result", result);
}

// ============================================================
// 5. Location extraction
// ============================================================

void TestMovedToCity() {
  Log("=== TestMovedToCity ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "She moved to Portland last year for a new job."),
  };
  auto result = extractor.ExtractAnswer("Where did she move to?", items);
  Check(result == "Portland", "city extracted from moved-to pattern");
  LogKV("result", result);
}

void TestFlightDestination() {
  Log("=== TestFlightDestination ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.8f, "Booked a flight to Tokyo for the conference."),
  };
  auto result = extractor.ExtractAnswer("Where is the flight to?", items);
  Check(result == "Tokyo", "flight destination extracted");
  LogKV("result", result);
}

// ============================================================
// 6. Allergy extraction
// ============================================================

void TestAllergyExtraction() {
  Log("=== TestAllergyExtraction ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "She is allergic to peanuts and should avoid them."),
  };
  auto result = extractor.ExtractAnswer("What is her allergy?", items);
  Check(result.find("allergic to peanuts") != std::string::npos, "allergy extracted");
  LogKV("result", result);
}

// ============================================================
// 7. Preference extraction
// ============================================================

void TestPreferenceExtraction() {
  Log("=== TestPreferenceExtraction ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.8f, "For status updates, she prefers written summaries over calls."),
  };
  auto result = extractor.ExtractAnswer("How does she prefer status updates to be written?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result.find("written summaries") != std::string::npos, "preference extracted");
  LogKV("result", result);
}

// ============================================================
// 8. Pet name + adoption date
// ============================================================

void TestPetNameWithAdoptionDate() {
  Log("=== TestPetNameWithAdoptionDate ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "They adopted a golden retriever named Buddy in March 2024."),
  };
  auto result = extractor.ExtractAnswer("When did they adopt their dog?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result.find("Buddy") != std::string::npos, "pet name extracted");
  Check(result.find("March 2024") != std::string::npos, "adoption date extracted");
  LogKV("result", result);
}

// ============================================================
// 9. Lexical sentence fallback
// ============================================================

void TestFallbackToLexicalSentence() {
  Log("=== TestFallbackToLexicalSentence ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.7f, "The project uses React for the frontend. The backend runs on Node.js with Express."),
  };
  auto result = extractor.ExtractAnswer("What technology does the backend use?", items);
  Check(!result.empty(), "result is non-empty");
  Check(result.find("backend") != std::string::npos, "selects backend sentence");
  LogKV("result", result);
}

void TestFallbackToFirstText() {
  Log("=== TestFallbackToFirstText ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.5f, "This is completely unrelated content about gardening."),
  };
  auto result = extractor.ExtractAnswer("xyzzy foobar?", items);
  Check(!result.empty(), "fallback returns non-empty");
  LogKV("result", result);
}

// ============================================================
// 10. Multiple items — best relevance wins
// ============================================================

void TestMultipleItemsBestRelevanceWins() {
  Log("=== TestMultipleItemsBestRelevanceWins ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.3f, "Old data: moved to Chicago in 2019."),
      MakeItem(0.9f, "Latest update: moved to Seattle in 2024 for the new role."),
  };
  auto result = extractor.ExtractAnswer("Where did they move to?", items);
  Check(result == "Seattle", "higher-scored item wins");
  LogKV("result", result);
}

// ============================================================
// 11. RAGItem overload
// ============================================================

void TestRAGItemOverload() {
  Log("=== TestRAGItemOverload ===");
  DeterministicAnswerExtractor extractor;
  std::vector<RAGItem> items;
  RAGItem item;
  item.score = 0.9f;
  item.text = "Alice owns deployment readiness for Q2 rollout.";
  items.push_back(item);
  auto result = extractor.ExtractAnswer("Who owns deployment readiness?", items);
  Check(result == "Alice", "RAGItem overload works");
  LogKV("result", result);
}

// ============================================================
// 12. Sentence splitting
// ============================================================

void TestSentenceSplitting() {
  Log("=== TestSentenceSplitting ===");
  auto sentences = DeterministicAnswerExtractor::SplitSentences(
      "Hello world. How are you? Fine! Bye\nDone");
  Check(sentences.size() == 5, "five sentences");
  if (sentences.size() >= 5) {
    Check(sentences[0] == "Hello world", "sentence 0");
    Check(sentences[1] == "How are you", "sentence 1");
    Check(sentences[2] == "Fine", "sentence 2");
    Check(sentences[3] == "Bye", "sentence 3");
    Check(sentences[4] == "Done", "sentence 4");
  }
}

// ============================================================
// 13. Regex helpers
// ============================================================

void TestFirstRegexMatch() {
  Log("=== TestFirstRegexMatch ===");
  auto result = DeterministicAnswerExtractor::FirstRegexMatch(
      R"(\bnamed\s+([A-Z][a-z]+)\b)",
      "The dog named Rover is friendly.",
      1);
  Check(result == "Rover", "capture group 1 extracted");
  LogKV("result", result);
}

void TestFirstRegexMatchNoMatch() {
  Log("=== TestFirstRegexMatchNoMatch ===");
  auto result = DeterministicAnswerExtractor::FirstRegexMatch(
      R"(\bnamed\s+([A-Z][a-z]+)\b)",
      "no match here",
      1);
  Check(result.empty(), "no match returns empty");
}

// ============================================================
// 14. Travel + location interaction
// ============================================================

void TestTravelPriorityOverLocation() {
  Log("=== TestTravelPriorityOverLocation ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.8f, "She moved to Portland. She has a flight to Denver next week."),
  };
  auto result = extractor.ExtractAnswer("Where is her flight to?", items);
  Check(result == "Denver", "flight destination takes priority");
  LogKV("result", result);
}

// ============================================================
// 15. Ownership alone
// ============================================================

void TestOwnershipAloneReturnsOwner() {
  Log("=== TestOwnershipAloneReturnsOwner ===");
  DeterministicAnswerExtractor extractor;
  std::vector<AnswerExtractionItem> items = {
      MakeItem(0.9f, "Marcus Lee owns deployment readiness. Launch date is April 1, 2025."),
  };
  auto result = extractor.ExtractAnswer("Who owns deployment readiness?", items);
  Check(result == "Marcus Lee", "ownership-only returns owner");
  LogKV("result", result);
}

}  // namespace

int main() {
  Log("== DeterministicAnswerExtractor Tests ==");

  TestEmptyItems();
  TestAllWhitespaceItems();
  TestCleanTextRemovesBrackets();
  TestDeploymentOwnership();
  TestGenericOwnership();
  TestOwnershipWithDate();
  TestLaunchDate();
  TestGenericDateFallback();
  TestAppointmentDateTime();
  TestMovedToCity();
  TestFlightDestination();
  TestAllergyExtraction();
  TestPreferenceExtraction();
  TestPetNameWithAdoptionDate();
  TestFallbackToLexicalSentence();
  TestFallbackToFirstText();
  TestMultipleItemsBestRelevanceWins();
  TestRAGItemOverload();
  TestSentenceSplitting();
  TestFirstRegexMatch();
  TestFirstRegexMatchNoMatch();
  TestTravelPriorityOverLocation();
  TestOwnershipAloneReturnsOwner();

  std::cout << "\n== Results: " << g_pass << " passed, " << g_fail << " failed ==\n";
  return g_fail > 0 ? 1 : 0;
}
