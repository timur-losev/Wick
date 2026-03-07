#pragma once

#include "waxcpp/access_stats.hpp"

#include <cstdint>

namespace waxcpp {

/// Importance score for a frame, with component breakdown.
struct ImportanceScore {
  float score = 0.0f;
  float age_component = 0.0f;
  float frequency_component = 0.0f;
  float recency_component = 0.0f;
};

/// Configuration for importance scoring weights and decay rates.
struct ImportanceScoringConfig {
  /// Weight for memory age component (0.0 - 1.0).
  float age_weight = 0.3f;

  /// Weight for access frequency component (0.0 - 1.0).
  float frequency_weight = 0.4f;

  /// Weight for recency of last access component (0.0 - 1.0).
  float recency_weight = 0.3f;

  /// Half-life for age decay in hours (age at which importance drops to ~37%).
  float age_half_life_hours = 168.0f;  // 1 week

  /// Half-life for recency decay in hours.
  float recency_half_life_hours = 24.0f;  // 1 day

  static ImportanceScoringConfig Default() { return {}; }
};

/// Calculates importance scores for frames based on age and access patterns.
///
/// Scoring formula:
///   age_component     = exp(-age_hours / age_half_life_hours)
///   frequency_component = min(1.0, log(access_count + 1) / 5.0)
///   recency_component = exp(-hours_since_access / recency_half_life_hours)
///   score = weighted_average(age, frequency, recency)
class ImportanceScorer {
 public:
  ImportanceScoringConfig config;

  ImportanceScorer();
  explicit ImportanceScorer(ImportanceScoringConfig config);

  /// Calculate importance score for a frame.
  ImportanceScore Score(std::int64_t frame_timestamp_ms,
                        const FrameAccessStats* access_stats,
                        std::int64_t now_ms) const;
};

}  // namespace waxcpp
