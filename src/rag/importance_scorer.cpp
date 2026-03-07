#include "waxcpp/importance_scorer.hpp"

#include <algorithm>
#include <cmath>

namespace waxcpp {

ImportanceScorer::ImportanceScorer() : config(ImportanceScoringConfig::Default()) {}

ImportanceScorer::ImportanceScorer(ImportanceScoringConfig config)
    : config(std::move(config)) {}

ImportanceScore ImportanceScorer::Score(std::int64_t frame_timestamp_ms,
                                       const FrameAccessStats* access_stats,
                                       std::int64_t now_ms) const {
  // Age component: newer = higher importance (exponential decay).
  const float age_ms = static_cast<float>(std::max<std::int64_t>(0, now_ms - frame_timestamp_ms));
  const float age_hours = age_ms / (1000.0f * 60.0f * 60.0f);
  const float age_component = std::exp(-age_hours / config.age_half_life_hours);

  // Frequency component: more accesses = higher importance (log scale, capped).
  // log(1 + count) / 5.0 normalizes to ~1.0 at ~148 accesses.
  float frequency_component = 0.0f;
  if (access_stats != nullptr) {
    frequency_component = std::min(
        1.0f,
        std::log(static_cast<float>(access_stats->access_count) + 1.0f) / 5.0f);
  }

  // Recency component: recently accessed = higher importance.
  float recency_component = 0.0f;
  if (access_stats != nullptr) {
    const float hours_since_access =
        static_cast<float>(std::max<std::int64_t>(0, now_ms - access_stats->last_access_ms)) /
        (1000.0f * 60.0f * 60.0f);
    recency_component = std::exp(-hours_since_access / config.recency_half_life_hours);
  }

  // Weighted sum, normalized to 0-1.
  const float total_weight =
      config.age_weight + config.frequency_weight + config.recency_weight;
  float raw_score;
  if (total_weight > 0.0f) {
    raw_score = (config.age_weight * age_component +
                 config.frequency_weight * frequency_component +
                 config.recency_weight * recency_component) /
                total_weight;
  } else {
    raw_score = age_component;  // Fallback to age-only if weights are zero.
  }

  return ImportanceScore{
      .score = raw_score,
      .age_component = age_component,
      .frequency_component = frequency_component,
      .recency_component = recency_component,
  };
}

}  // namespace waxcpp
