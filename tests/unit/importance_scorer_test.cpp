#include "waxcpp/importance_scorer.hpp"
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

bool ApproxEq(float a, float b, float eps = 0.01f) {
  return std::fabs(a - b) < eps;
}

constexpr std::int64_t kHourMs = 60LL * 60 * 1000;
constexpr std::int64_t kDayMs = 24LL * kHourMs;

// ---------- Tests ----------

void TestDefaultConfigValues() {
  Log("=== TestDefaultConfigValues ===");
  auto cfg = ImportanceScoringConfig::Default();
  Check(ApproxEq(cfg.age_weight, 0.3f), "age_weight");
  Check(ApproxEq(cfg.frequency_weight, 0.4f), "frequency_weight");
  Check(ApproxEq(cfg.recency_weight, 0.3f), "recency_weight");
  Check(ApproxEq(cfg.age_half_life_hours, 168.0f), "age_half_life_hours");
  Check(ApproxEq(cfg.recency_half_life_hours, 24.0f), "recency_half_life_hours");
}

void TestBrandNewFrame() {
  Log("=== TestBrandNewFrame ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;
  auto result = scorer.Score(now, nullptr, now);
  // Brand new frame (age=0): age_component=1.0, freq=0, recency=0.
  Check(ApproxEq(result.age_component, 1.0f), "age_component = 1.0");
  Check(ApproxEq(result.frequency_component, 0.0f), "frequency_component = 0.0");
  Check(ApproxEq(result.recency_component, 0.0f), "recency_component = 0.0");
  // score = (0.3 * 1.0 + 0.4 * 0.0 + 0.3 * 0.0) / 1.0 = 0.3
  Check(ApproxEq(result.score, 0.3f), "score = 0.3 (age-only contribution)");
}

void TestOldFrameNoAccess() {
  Log("=== TestOldFrameNoAccess ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;
  const std::int64_t frame_time = now - 7 * kDayMs;  // 1 week old
  auto result = scorer.Score(frame_time, nullptr, now);
  // age=168 hours, half_life=168 hours → exp(-1) ≈ 0.368
  Check(ApproxEq(result.age_component, std::exp(-1.0f), 0.01f), "age_component ~ exp(-1)");
  Check(ApproxEq(result.frequency_component, 0.0f), "frequency_component = 0.0");
  // score = (0.3 * 0.368) / 1.0 = ~0.11
  Check(result.score < 0.15f, "score low for old unaccessed frame");
}

void TestRecentAccessBoost() {
  Log("=== TestRecentAccessBoost ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;
  const std::int64_t frame_time = now - 7 * kDayMs;  // 1 week old

  // Frame accessed 10 times, most recently just now.
  FrameAccessStats stats(1, frame_time);
  stats.access_count = 10;
  stats.last_access_ms = now;

  auto result = scorer.Score(frame_time, &stats, now);

  // Frequency: min(1.0, log(11)/5.0) ≈ min(1.0, 0.479) = 0.479
  float expected_freq = std::min(1.0f, std::log(11.0f) / 5.0f);
  Check(ApproxEq(result.frequency_component, expected_freq, 0.02f),
        "frequency_component ~ log(11)/5");

  // Recency: accessed just now → exp(0) = 1.0
  Check(ApproxEq(result.recency_component, 1.0f), "recency_component = 1.0");

  // Combined score should be higher than without access.
  auto no_access = scorer.Score(frame_time, nullptr, now);
  Check(result.score > no_access.score, "score boosted by access");
}

void TestHighFrequencyAccess() {
  Log("=== TestHighFrequencyAccess ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;

  FrameAccessStats stats(1, now);
  stats.access_count = 150;
  stats.last_access_ms = now;

  auto result = scorer.Score(now, &stats, now);
  // log(151)/5.0 ≈ 1.0 → capped at 1.0
  Check(result.frequency_component >= 0.95f, "frequency_component near 1.0 at 150 accesses");
}

void TestStaleAccessPenalty() {
  Log("=== TestStaleAccessPenalty ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;
  const std::int64_t frame_time = now;

  // Frame accessed 5 times, but last access was 48 hours ago.
  FrameAccessStats stats(1, frame_time);
  stats.access_count = 5;
  stats.last_access_ms = now - 2 * kDayMs;

  auto result = scorer.Score(frame_time, &stats, now);
  // Recency: exp(-48/24) = exp(-2) ≈ 0.135
  float expected_recency = std::exp(-2.0f);
  Check(ApproxEq(result.recency_component, expected_recency, 0.02f),
        "recency decayed for stale access");
}

void TestCustomWeights() {
  Log("=== TestCustomWeights ===");
  ImportanceScoringConfig cfg;
  cfg.age_weight = 1.0f;
  cfg.frequency_weight = 0.0f;
  cfg.recency_weight = 0.0f;
  ImportanceScorer scorer(cfg);

  const std::int64_t now = 1000000;
  FrameAccessStats stats(1, now);
  stats.access_count = 100;
  stats.last_access_ms = now;

  auto result = scorer.Score(now, &stats, now);
  // With age_weight=1.0 and others=0, score should equal age_component.
  Check(ApproxEq(result.score, result.age_component), "age-only weighting");
}

void TestZeroWeightsFallback() {
  Log("=== TestZeroWeightsFallback ===");
  ImportanceScoringConfig cfg;
  cfg.age_weight = 0.0f;
  cfg.frequency_weight = 0.0f;
  cfg.recency_weight = 0.0f;
  ImportanceScorer scorer(cfg);

  const std::int64_t now = 1000000;
  auto result = scorer.Score(now, nullptr, now);
  // Fallback to age-only: brand new → 1.0
  Check(ApproxEq(result.score, 1.0f), "zero weights fall back to age_component");
}

void TestVeryOldFrameApproachesZero() {
  Log("=== TestVeryOldFrameApproachesZero ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;
  const std::int64_t frame_time = now - 365LL * kDayMs;  // 1 year old

  auto result = scorer.Score(frame_time, nullptr, now);
  Check(result.age_component < 0.001f, "age_component near zero for 1-year-old frame");
  Check(result.score < 0.001f, "score near zero");
}

void TestScoreInRange() {
  Log("=== TestScoreInRange ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;

  // Test various scenarios - score should always be in [0, 1].
  for (int age_days = 0; age_days <= 365; age_days += 30) {
    for (int count : {0, 1, 10, 100}) {
      FrameAccessStats stats(1, now - age_days * kDayMs);
      stats.access_count = static_cast<std::uint32_t>(count);
      stats.last_access_ms = now;

      auto result = scorer.Score(now - age_days * kDayMs,
                                 count > 0 ? &stats : nullptr, now);
      if (result.score < -0.001f || result.score > 1.001f) {
        Check(false, "score out of range");
        return;
      }
    }
  }
  Check(true, "all scores in [0, 1]");
}

void TestFutureTimestamp() {
  Log("=== TestFutureTimestamp ===");
  ImportanceScorer scorer;
  const std::int64_t now = 1000000;
  // Frame created in the future (clock skew).
  const std::int64_t frame_time = now + kDayMs;
  auto result = scorer.Score(frame_time, nullptr, now);
  // age = max(0, -1 day) = 0 → age_component = 1.0
  Check(ApproxEq(result.age_component, 1.0f), "future timestamp clamped to zero age");
}

}  // namespace

int main() {
  TestDefaultConfigValues();
  TestBrandNewFrame();
  TestOldFrameNoAccess();
  TestRecentAccessBoost();
  TestHighFrequencyAccess();
  TestStaleAccessPenalty();
  TestCustomWeights();
  TestZeroWeightsFallback();
  TestVeryOldFrameApproachesZero();
  TestScoreInRange();
  TestFutureTimestamp();

  std::cout << "\nimportance_scorer_test: " << g_pass << " passed, " << g_fail
            << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
