#include "waxcpp/search.hpp"
#include "waxcpp/query_analyzer.hpp"
#include "../test_logger.hpp"

#include <cmath>
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

SearchResult MakeResult(std::uint64_t frame_id, float score,
                        const std::string& preview,
                        std::vector<SearchSource> sources = {SearchSource::kText}) {
  SearchResult r;
  r.frame_id = frame_id;
  r.score = score;
  r.preview_text = preview;
  r.sources = std::move(sources);
  return r;
}

// ============================================================
// 1. No intent → passthrough (no reranking)
// ============================================================

void TestNoIntentPassthrough() {
  Log("=== TestNoIntentPassthrough ===");
  // Generic query with no location/date/ownership intent should pass through.
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "alpha beta gamma"),
      MakeResult(2, 1.0f, "delta epsilon zeta"),
  };
  auto reranked = IntentAwareRerank(results, "tell me about the project", 10);
  Check(reranked.size() == 2, "same count after passthrough");
  Check(reranked[0].frame_id == 1, "order preserved - first");
  Check(reranked[1].frame_id == 2, "order preserved - second");
}

// ============================================================
// 2. Location intent: "Where did X move to?"
// ============================================================

void TestLocationIntentBoostsMovedTo() {
  Log("=== TestLocationIntentBoostsMovedTo ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 1.5f, "She is allergic to peanuts and avoids them."),
      MakeResult(2, 1.4f, "She moved to Portland last year for the new role."),
      MakeResult(3, 1.3f, "She prefers written summaries over calls."),
  };
  auto reranked = IntentAwareRerank(results, "Where did she move to?", 10);
  Check(!reranked.empty(), "non-empty results");
  Check(reranked[0].frame_id == 2, "location result should be promoted to top");
}

void TestLocationIntentPenalizesAllergic() {
  Log("=== TestLocationIntentPenalizesAllergic ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "She is allergic to peanuts and should avoid them."),
      MakeResult(2, 1.8f, "She moved to Seattle in 2024 for the new job."),
  };
  auto reranked = IntentAwareRerank(results, "Where did she move to?", 10);
  Check(reranked[0].frame_id == 2, "allergy result should be demoted for location query");
}

// ============================================================
// 3. Date intent: "When is the public launch?"
// ============================================================

void TestDateIntentBoostsConfirmedLaunch() {
  Log("=== TestDateIntentBoostsConfirmedLaunch ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 1.5f, "Alice owns deployment readiness for the project."),
      MakeResult(2, 1.4f, "The public launch is scheduled for June 1, 2025."),
      MakeResult(3, 1.3f, "Weekly report on progress items."),
  };
  auto reranked = IntentAwareRerank(results, "When is the public launch date?", 10);
  Check(reranked[0].frame_id == 2, "confirmed launch date should be promoted to top");
}

void TestDateIntentPenalizesTentative() {
  Log("=== TestDateIntentPenalizesTentative ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "The tentative launch target is March 2025, pending approval."),
      MakeResult(2, 1.5f, "The public launch is confirmed for June 1, 2025."),
  };
  auto reranked = IntentAwareRerank(results, "When is the public launch date?", 10);
  Check(reranked[0].frame_id == 2, "tentative date should be demoted below confirmed date");
}

void TestDateIntentPenalizesDraftMemo() {
  Log("=== TestDateIntentPenalizesDraftMemo ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "The draft memo suggests a launch window of Q2 2025."),
      MakeResult(2, 1.5f, "The public launch is set for June 1, 2025."),
  };
  auto reranked = IntentAwareRerank(results, "When is the public launch date?", 10);
  Check(reranked[0].frame_id == 2, "draft memo should be demoted for date query");
}

// ============================================================
// 4. Ownership intent: "Who owns deployment readiness?"
// ============================================================

void TestOwnershipIntentBoostsOwnsKeyword() {
  Log("=== TestOwnershipIntentBoostsOwnsKeyword ===");
  // Query must include entities/years for disambiguation signals to activate reranking.
  std::vector<SearchResult> results = {
      MakeResult(1, 1.5f, "The public launch is scheduled for June 1, 2025."),
      MakeResult(2, 1.4f, "Alice Chen owns deployment readiness for the Q2 rollout."),
      MakeResult(3, 1.3f, "Weekly checklist items for the team signoff."),
  };
  auto reranked = IntentAwareRerank(results, "Who owns Alice Chen's deployment readiness in 2025?", 10);
  Check(reranked[0].frame_id == 2, "ownership result should be promoted to top");
}

void TestOwnershipIntentPenalizesLaunchOnly() {
  Log("=== TestOwnershipIntentPenalizesLaunchOnly ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "The public launch date is confirmed."),
      MakeResult(2, 1.5f, "Bob Smith owns backend infrastructure."),
  };
  auto reranked = IntentAwareRerank(results, "Who owns backend infrastructure?", 10);
  Check(reranked[0].frame_id == 2, "launch-only result should be demoted for ownership query");
}

// ============================================================
// 5. Entity coverage disambiguation
// ============================================================

void TestEntityCoverageDisambiguates() {
  Log("=== TestEntityCoverageDisambiguates ===");
  // Two results mentioning different entities — the one matching query entities should win.
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "Carol Davis moved to Denver in 2023."),
      MakeResult(2, 1.8f, "Alice Chen moved to Portland in 2024."),
  };
  auto reranked = IntentAwareRerank(results, "Where did Alice Chen move to?", 10);
  Check(reranked[0].frame_id == 2, "result matching query entity should be promoted");
}

// ============================================================
// 6. Year coverage disambiguation
// ============================================================

void TestYearCoverageDisambiguates() {
  Log("=== TestYearCoverageDisambiguates ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "The public launch is set for June 2024."),
      MakeResult(2, 1.8f, "The public launch is set for June 2025."),
  };
  auto reranked = IntentAwareRerank(results, "When is the 2025 public launch date?", 10);
  Check(reranked[0].frame_id == 2, "result with matching year should be promoted");
}

// ============================================================
// 7. Distractor penalty
// ============================================================

void TestDistractorPenalty() {
  Log("=== TestDistractorPenalty ===");
  // Query needs entities for disambiguation signals.
  // Score gap (0.1) must be smaller than distractor penalty (-0.40).
  std::vector<SearchResult> results = {
      MakeResult(1, 1.6f, "Alice owns deployment readiness per the weekly report checklist."),
      MakeResult(2, 1.5f, "Alice owns deployment readiness for Q2."),
  };
  auto reranked = IntentAwareRerank(results, "Who owns Alice's deployment readiness in 2025?", 10);
  Check(reranked[0].frame_id == 2, "distractor-like content should be penalized");
}

// ============================================================
// 8. Window capping
// ============================================================

void TestWindowCapping() {
  Log("=== TestWindowCapping ===");
  std::vector<SearchResult> results;
  for (int i = 0; i < 20; ++i) {
    results.push_back(MakeResult(
        static_cast<std::uint64_t>(i + 1),
        20.0f - static_cast<float>(i),
        "She moved to City" + std::to_string(i + 1) + " for work."));
  }
  // Rerank with window=5 — only the first 5 should be reranked.
  auto reranked = IntentAwareRerank(results, "Where did she move?", 5);
  Check(reranked.size() == 20, "total count unchanged after window-capped rerank");
  // The tail (beyond window) should be in original order.
  for (int i = 5; i < 20; ++i) {
    Check(reranked[static_cast<std::size_t>(i)].frame_id == results[static_cast<std::size_t>(i)].frame_id,
          "tail beyond rerank window should be untouched");
  }
}

// ============================================================
// 9. Empty input / single item
// ============================================================

void TestEmptyInput() {
  Log("=== TestEmptyInput ===");
  auto reranked = IntentAwareRerank({}, "Where did she move?", 10);
  Check(reranked.empty(), "empty input returns empty output");
}

void TestSingleItem() {
  Log("=== TestSingleItem ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 1.0f, "She moved to Portland."),
  };
  auto reranked = IntentAwareRerank(results, "Where did she move?", 10);
  Check(reranked.size() == 1, "single item passes through");
  Check(reranked[0].frame_id == 1, "single item frame preserved");
}

// ============================================================
// 10. Vector source influence on tentative penalty
// ============================================================

void TestVectorSourceInfluenceOnTentativePenalty() {
  Log("=== TestVectorSourceInfluenceOnTentativePenalty ===");
  // Vector-sourced tentative result should get a stronger penalty.
  std::vector<SearchResult> results_text = {
      MakeResult(1, 3.0f, "The tentative launch target is March 2025."),
      MakeResult(2, 1.0f, "The public launch is confirmed for June 2025."),
  };
  results_text[0].sources = {SearchSource::kText};

  std::vector<SearchResult> results_vector = {
      MakeResult(1, 3.0f, "The tentative launch target is March 2025."),
      MakeResult(2, 1.0f, "The public launch is confirmed for June 2025."),
  };
  results_vector[0].sources = {SearchSource::kVector};

  auto reranked_text = IntentAwareRerank(results_text, "When is the 2025 public launch date?", 10);
  auto reranked_vector = IntentAwareRerank(results_vector, "When is the 2025 public launch date?", 10);

  // Both should demote the tentative result.
  Check(reranked_text[0].frame_id == 2, "text-sourced tentative should be demoted");
  Check(reranked_vector[0].frame_id == 2, "vector-sourced tentative should be demoted");
}

// ============================================================
// 11. No preview text → no boost/penalty
// ============================================================

void TestNoPreviewTextNeutral() {
  Log("=== TestNoPreviewTextNeutral ===");
  std::vector<SearchResult> results = {
      MakeResult(1, 2.0f, "She moved to Portland."),
  };
  // Add a result with no preview.
  SearchResult no_preview;
  no_preview.frame_id = 2;
  no_preview.score = 1.5f;
  no_preview.preview_text = std::nullopt;
  no_preview.sources = {SearchSource::kText};
  results.push_back(no_preview);

  auto reranked = IntentAwareRerank(results, "Where did she move?", 10);
  Check(reranked.size() == 2, "both results returned");
  // The result with preview and location match should still rank first.
  Check(reranked[0].frame_id == 1, "result with location preview should rank above no-preview");
}

// ============================================================
// 12. Combined date + ownership intent in single query
// ============================================================

void TestCombinedDateOwnershipIntent() {
  Log("=== TestCombinedDateOwnershipIntent ===");
  // Query needs entities/years for disambiguation signals.
  std::vector<SearchResult> results = {
      MakeResult(1, 1.5f, "The weekly report on deployment status."),
      MakeResult(2, 1.4f, "Alice owns deployment readiness. The public launch is set for March 15, 2025."),
      MakeResult(3, 1.3f, "Draft memo from January about tentative dates."),
  };
  auto reranked = IntentAwareRerank(
      results, "Who owns Alice's deployment readiness and when is the 2025 launch?", 10);
  Check(reranked[0].frame_id == 2,
        "result with both ownership and date should rank first for combined query");
}

// ============================================================
// 13. RerankingHelpers unit tests
// ============================================================

void TestContainsTentativeLaunchLanguage() {
  Log("=== TestContainsTentativeLaunchLanguage ===");
  Check(RerankingHelpers::ContainsTentativeLaunchLanguage("tentative launch date"),
        "detects 'tentative'");
  Check(RerankingHelpers::ContainsTentativeLaunchLanguage("the draft plan is ready"),
        "detects 'draft'");
  Check(RerankingHelpers::ContainsTentativeLaunchLanguage("pending approval from the board"),
        "detects 'pending approval'");
  Check(RerankingHelpers::ContainsTentativeLaunchLanguage("the target is Q2"),
        "detects 'target is'");
  Check(RerankingHelpers::ContainsTentativeLaunchLanguage("the estimate is March"),
        "detects 'estimate'");
  Check(!RerankingHelpers::ContainsTentativeLaunchLanguage("confirmed for June 1"),
        "no false positive on confirmed");
}

void TestContainsMovedToLocationPattern() {
  Log("=== TestContainsMovedToLocationPattern ===");
  Check(RerankingHelpers::ContainsMovedToLocationPattern("She moved to Portland last year."),
        "matches 'moved to Portland'");
  Check(RerankingHelpers::ContainsMovedToLocationPattern("Plans to move to Seattle next month."),
        "matches 'move to Seattle'");
  Check(!RerankingHelpers::ContainsMovedToLocationPattern("She moved to the store."),
        "rejects 'moved to the' (lowercase destination)");
  Check(!RerankingHelpers::ContainsMovedToLocationPattern("No relocation mentioned."),
        "no false positive on unrelated text");
}

void TestLooksDistractorLike() {
  Log("=== TestLooksDistractorLike ===");
  Check(RerankingHelpers::LooksDistractorLike("the weekly report summary"),
        "detects 'weekly report'");
  Check(RerankingHelpers::LooksDistractorLike("team signoff checklist"),
        "detects 'checklist' and 'signoff'");
  Check(RerankingHelpers::LooksDistractorLike("draft memo about budgets"),
        "detects 'draft memo'");
  Check(!RerankingHelpers::LooksDistractorLike("Alice owns deployment readiness."),
        "no false positive on ownership text");
}

}  // namespace

int main() {
  Log("== IntentAwareRerank Tests ==");

  TestNoIntentPassthrough();
  TestLocationIntentBoostsMovedTo();
  TestLocationIntentPenalizesAllergic();
  TestDateIntentBoostsConfirmedLaunch();
  TestDateIntentPenalizesTentative();
  TestDateIntentPenalizesDraftMemo();
  TestOwnershipIntentBoostsOwnsKeyword();
  TestOwnershipIntentPenalizesLaunchOnly();
  TestEntityCoverageDisambiguates();
  TestYearCoverageDisambiguates();
  TestDistractorPenalty();
  TestWindowCapping();
  TestEmptyInput();
  TestSingleItem();
  TestVectorSourceInfluenceOnTentativePenalty();
  TestNoPreviewTextNeutral();
  TestCombinedDateOwnershipIntent();
  TestContainsTentativeLaunchLanguage();
  TestContainsMovedToLocationPattern();
  TestLooksDistractorLike();

  std::cout << "\n== Results: " << g_pass << " passed, " << g_fail << " failed ==\n";
  return g_fail > 0 ? 1 : 0;
}
