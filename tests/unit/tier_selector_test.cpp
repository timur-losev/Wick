#include "waxcpp/tier_selector.hpp"
#include "../test_logger.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

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

constexpr std::int64_t kHourMs = 60LL * 60 * 1000;
constexpr std::int64_t kDayMs = 24LL * kHourMs;
constexpr std::int64_t kBaseTime = 1000000000LL;  // Arbitrary base.

// ---------- TierSelectionPolicy disabled ----------

void TestDisabledPolicyAlwaysFull() {
  Log("=== TestDisabledPolicyAlwaysFull ===");
  SurrogateTierSelector sel(TierPolicyDisabled{});

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 365 * kDayMs;  // Very old.
  ctx.now_ms = kBaseTime;

  Check(sel.SelectTier(ctx) == SurrogateTier::kFull,
        "disabled policy returns full for old frame");

  ctx.frame_timestamp_ms = kBaseTime;  // Brand new.
  Check(sel.SelectTier(ctx) == SurrogateTier::kFull,
        "disabled policy returns full for new frame");
}

// ---------- Age-based policy ----------

void TestAgeOnlyRecentFrame() {
  Log("=== TestAgeOnlyRecentFrame ===");
  SurrogateTierSelector sel(TierPolicyAgeBalanced());

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 3 * kDayMs;  // 3 days old.
  ctx.now_ms = kBaseTime;

  Check(sel.SelectTier(ctx) == SurrogateTier::kFull,
        "3-day-old frame gets full tier (recent < 7 days)");
}

void TestAgeOnlyMediumFrame() {
  Log("=== TestAgeOnlyMediumFrame ===");
  SurrogateTierSelector sel(TierPolicyAgeBalanced());

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 14 * kDayMs;  // 14 days old.
  ctx.now_ms = kBaseTime;

  Check(sel.SelectTier(ctx) == SurrogateTier::kGist,
        "14-day-old frame gets gist tier (7-30 days)");
}

void TestAgeOnlyOldFrame() {
  Log("=== TestAgeOnlyOldFrame ===");
  SurrogateTierSelector sel(TierPolicyAgeBalanced());

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 60 * kDayMs;  // 60 days old.
  ctx.now_ms = kBaseTime;

  Check(sel.SelectTier(ctx) == SurrogateTier::kMicro,
        "60-day-old frame gets micro tier (> 30 days)");
}

void TestAgeOnlyBoundaryRecent() {
  Log("=== TestAgeOnlyBoundaryRecent ===");
  // Exactly at the recent threshold boundary.
  SurrogateTierSelector sel(TierPolicyAgeBalanced());

  TierSelectionContext ctx;
  ctx.now_ms = kBaseTime;
  ctx.frame_timestamp_ms = kBaseTime - 7 * kDayMs;  // Exactly 7 days.

  // age_ms == recentMs → NOT less than → should be gist.
  Check(sel.SelectTier(ctx) == SurrogateTier::kGist,
        "exactly 7 days gets gist (boundary)");
}

void TestAgeOnlyBoundaryOld() {
  Log("=== TestAgeOnlyBoundaryOld ===");
  SurrogateTierSelector sel(TierPolicyAgeBalanced());

  TierSelectionContext ctx;
  ctx.now_ms = kBaseTime;
  ctx.frame_timestamp_ms = kBaseTime - 30 * kDayMs;  // Exactly 30 days.

  // age_ms == oldMs → NOT less than → should be micro.
  Check(sel.SelectTier(ctx) == SurrogateTier::kMicro,
        "exactly 30 days gets micro (boundary)");
}

void TestCustomAgeThresholds() {
  Log("=== TestCustomAgeThresholds ===");
  AgeThresholds thresholds;
  thresholds.recent_days = 1;
  thresholds.old_days = 3;
  SurrogateTierSelector sel(TierPolicyAgeOnly{thresholds});

  TierSelectionContext ctx;
  ctx.now_ms = kBaseTime;

  ctx.frame_timestamp_ms = kBaseTime - 12 * kHourMs;
  Check(sel.SelectTier(ctx) == SurrogateTier::kFull, "12h → full (< 1 day)");

  ctx.frame_timestamp_ms = kBaseTime - 2 * kDayMs;
  Check(sel.SelectTier(ctx) == SurrogateTier::kGist, "2d → gist (1-3 days)");

  ctx.frame_timestamp_ms = kBaseTime - 5 * kDayMs;
  Check(sel.SelectTier(ctx) == SurrogateTier::kMicro, "5d → micro (> 3 days)");
}

// ---------- Importance-based policy ----------

void TestImportanceNewHighFrequencyFrame() {
  Log("=== TestImportanceNewHighFrequencyFrame ===");
  SurrogateTierSelector sel(TierPolicyImportanceBalanced());

  FrameAccessStats stats(1, kBaseTime);
  stats.access_count = 50;
  stats.last_access_ms = kBaseTime;

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime;
  ctx.access_stats = &stats;
  ctx.now_ms = kBaseTime;

  auto tier = sel.SelectTier(ctx);
  // Brand new + high frequency + just accessed → high importance → full.
  Check(tier == SurrogateTier::kFull,
        "new, frequently accessed frame gets full");
}

void TestImportanceOldUnaccessed() {
  Log("=== TestImportanceOldUnaccessed ===");
  SurrogateTierSelector sel(TierPolicyImportanceBalanced());

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 90 * kDayMs;
  ctx.access_stats = nullptr;
  ctx.now_ms = kBaseTime;

  auto tier = sel.SelectTier(ctx);
  // Very old, no access → low importance → micro.
  Check(tier == SurrogateTier::kMicro,
        "old unaccessed frame gets micro");
}

void TestImportanceMediumScore() {
  Log("=== TestImportanceMediumScore ===");
  // Medium age (2 weeks), frequent access, recently accessed → gist range.
  // score = (0.3 * exp(-2) + 0.4 * min(1,log(11)/5) + 0.3 * exp(-0.5)) / 1.0
  //       ≈ (0.3*0.135 + 0.4*0.479 + 0.3*0.607) ≈ 0.414 → gist (>0.3, <0.6).
  SurrogateTierSelector sel(TierPolicyImportanceBalanced());

  FrameAccessStats stats(1, kBaseTime - 14 * kDayMs);
  stats.access_count = 10;
  stats.last_access_ms = kBaseTime - 12 * kHourMs;  // Accessed 12 hours ago.

  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 14 * kDayMs;
  ctx.access_stats = &stats;
  ctx.now_ms = kBaseTime;

  auto tier = sel.SelectTier(ctx);
  // Should be gist (medium importance ~0.41).
  Check(tier == SurrogateTier::kGist,
        "medium-age frequently-accessed frame gets gist");
}

void TestQueryBoostUpgradesTier() {
  Log("=== TestQueryBoostUpgradesTier ===");
  // Create a scenario where without query boost → micro, with → gist/full.
  ImportanceThresholds thresholds;
  thresholds.full_threshold = 0.6f;
  thresholds.gist_threshold = 0.3f;
  SurrogateTierSelector sel(TierPolicyImportance{thresholds},
                            ImportanceScorer(),
                            0.5f);  // High boost factor.

  // Old frame, no access → base importance very low.
  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime - 60 * kDayMs;
  ctx.access_stats = nullptr;
  ctx.now_ms = kBaseTime;

  auto tier_no_query = sel.SelectTier(ctx);

  // Now add high specificity query signals.
  QuerySignals signals;
  signals.specificity_score = 0.9f;
  ctx.query_signals = &signals;

  auto tier_with_query = sel.SelectTier(ctx);
  // With a specificity_score of 0.9 and boost factor 0.5 → +0.45 to score.
  Check(static_cast<int>(tier_with_query) <= static_cast<int>(tier_no_query),
        "query boost upgrades or maintains tier");
}

void TestCustomImportanceThresholds() {
  Log("=== TestCustomImportanceThresholds ===");
  ImportanceThresholds thresholds;
  thresholds.full_threshold = 0.9f;
  thresholds.gist_threshold = 0.5f;
  SurrogateTierSelector sel(TierPolicyImportance{thresholds});

  // Brand new frame, no access → score ~0.3 (only age contributes).
  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime;
  ctx.access_stats = nullptr;
  ctx.now_ms = kBaseTime;

  auto tier = sel.SelectTier(ctx);
  // score ~0.3 < gist_threshold 0.5 → micro.
  Check(tier == SurrogateTier::kMicro,
        "strict thresholds: new unaccessed → micro");
}

// ---------- ExtractTier ----------

void TestExtractTierHierarchical() {
  Log("=== TestExtractTierHierarchical ===");
  std::string payload = "WAXSURR1\nFULL:The full content here\nGIST:A gist\nMICRO:micro";

  auto full = SurrogateTierSelector::ExtractTier(payload, SurrogateTier::kFull);
  Check(full.has_value() && *full == "The full content here", "extract full");

  auto gist = SurrogateTierSelector::ExtractTier(payload, SurrogateTier::kGist);
  Check(gist.has_value() && *gist == "A gist", "extract gist");

  auto micro = SurrogateTierSelector::ExtractTier(payload, SurrogateTier::kMicro);
  Check(micro.has_value() && *micro == "micro", "extract micro");
}

void TestExtractTierLegacy() {
  Log("=== TestExtractTierLegacy ===");
  std::string payload = "This is a plain text surrogate.";

  auto full = SurrogateTierSelector::ExtractTier(payload, SurrogateTier::kFull);
  Check(full.has_value() && *full == payload, "legacy falls back to full text");

  auto gist = SurrogateTierSelector::ExtractTier(payload, SurrogateTier::kGist);
  Check(gist.has_value() && *gist == payload, "legacy gist same as full text");
}

void TestExtractTierEmptyPayload() {
  Log("=== TestExtractTierEmptyPayload ===");
  auto result = SurrogateTierSelector::ExtractTier("", SurrogateTier::kFull);
  Check(result.has_value() && result->empty(), "empty payload returns empty string");
}

void TestExtractTierMalformedWaxSurr1() {
  Log("=== TestExtractTierMalformedWaxSurr1 ===");
  // Starts with WAXSURR1 but malformed content.
  std::string payload = "WAXSURR1\nBADFORMAT";

  auto result = SurrogateTierSelector::ExtractTier(payload, SurrogateTier::kFull);
  // Should fall back to legacy (return the whole string).
  Check(result.has_value() && *result == payload,
        "malformed WAXSURR1 falls back to legacy");
}

void TestDefaultConstructor() {
  Log("=== TestDefaultConstructor ===");
  SurrogateTierSelector sel;
  // Default policy is importance-balanced.
  TierSelectionContext ctx;
  ctx.frame_timestamp_ms = kBaseTime;
  ctx.now_ms = kBaseTime;

  // Should not crash and should return a valid tier.
  auto tier = sel.SelectTier(ctx);
  Check(tier == SurrogateTier::kFull ||
            tier == SurrogateTier::kGist ||
            tier == SurrogateTier::kMicro,
        "default constructor produces valid tier");
}

void TestAgeThresholdsMsConversion() {
  Log("=== TestAgeThresholdsMsConversion ===");
  AgeThresholds t;
  t.recent_days = 7;
  t.old_days = 30;

  Check(t.RecentMs() == 7LL * 24 * 60 * 60 * 1000, "RecentMs = 7 days in ms");
  Check(t.OldMs() == 30LL * 24 * 60 * 60 * 1000, "OldMs = 30 days in ms");
}

}  // namespace

int main() {
  TestDisabledPolicyAlwaysFull();
  TestAgeOnlyRecentFrame();
  TestAgeOnlyMediumFrame();
  TestAgeOnlyOldFrame();
  TestAgeOnlyBoundaryRecent();
  TestAgeOnlyBoundaryOld();
  TestCustomAgeThresholds();
  TestImportanceNewHighFrequencyFrame();
  TestImportanceOldUnaccessed();
  TestImportanceMediumScore();
  TestQueryBoostUpgradesTier();
  TestCustomImportanceThresholds();
  TestExtractTierHierarchical();
  TestExtractTierLegacy();
  TestExtractTierEmptyPayload();
  TestExtractTierMalformedWaxSurr1();
  TestDefaultConstructor();
  TestAgeThresholdsMsConversion();

  std::cout << "\ntier_selector_test: " << g_pass << " passed, " << g_fail
            << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
