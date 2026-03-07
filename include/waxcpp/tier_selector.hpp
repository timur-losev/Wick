#pragma once

#include "waxcpp/importance_scorer.hpp"
#include "waxcpp/query_analyzer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace waxcpp {

/// Compression tier for surrogate retrieval.
enum class SurrogateTier {
  kFull,
  kGist,
  kMicro,
};

/// Age thresholds for tier selection (in days).
struct AgeThresholds {
  int recent_days = 7;   // Memories newer than this use full tier.
  int old_days = 30;     // Memories older than this use micro tier.

  std::int64_t RecentMs() const {
    return static_cast<std::int64_t>(recent_days) * 24LL * 60 * 60 * 1000;
  }
  std::int64_t OldMs() const {
    return static_cast<std::int64_t>(old_days) * 24LL * 60 * 60 * 1000;
  }
};

/// Importance score thresholds for tier selection.
struct ImportanceThresholds {
  float full_threshold = 0.6f;   // Score >= this uses full tier.
  float gist_threshold = 0.3f;   // Score >= this uses gist tier (below = micro).
};

// ---------- TierSelectionPolicy (variant-based) ----------

struct TierPolicyDisabled {};

struct TierPolicyAgeOnly {
  AgeThresholds thresholds;
};

struct TierPolicyImportance {
  ImportanceThresholds thresholds;
};

using TierSelectionPolicy =
    std::variant<TierPolicyDisabled, TierPolicyAgeOnly, TierPolicyImportance>;

/// Preset: balanced age-only (7 days recent, 30 days old).
inline TierSelectionPolicy TierPolicyAgeBalanced() {
  return TierPolicyAgeOnly{AgeThresholds{}};
}

/// Preset: balanced importance-based (0.6 full, 0.3 gist).
inline TierSelectionPolicy TierPolicyImportanceBalanced() {
  return TierPolicyImportance{ImportanceThresholds{}};
}

// ---------- TierSelectionContext ----------

struct TierSelectionContext {
  std::int64_t frame_timestamp_ms = 0;
  const FrameAccessStats* access_stats = nullptr;
  const QuerySignals* query_signals = nullptr;
  std::int64_t now_ms = 0;
};

// ---------- SurrogateTierSelector ----------

/// Selects the appropriate surrogate tier based on policy and context.
class SurrogateTierSelector {
 public:
  TierSelectionPolicy policy;
  ImportanceScorer scorer;

  /// How much query specificity boosts importance (0.0 - 1.0).
  float query_boost_factor = 0.15f;

  SurrogateTierSelector();
  explicit SurrogateTierSelector(TierSelectionPolicy policy,
                                 ImportanceScorer scorer = ImportanceScorer(),
                                 float query_boost_factor = 0.15f);

  /// Select the appropriate tier for a frame based on policy and context.
  SurrogateTier SelectTier(const TierSelectionContext& context) const;

  /// Extract the appropriate tier text from surrogate payload data.
  /// Handles both hierarchical (WAXSURR1) and legacy (plain text) formats.
  static std::optional<std::string> ExtractTier(std::string_view data,
                                                SurrogateTier tier);

 private:
  SurrogateTier SelectByAge(const TierSelectionContext& context,
                            const AgeThresholds& thresholds) const;
  SurrogateTier SelectByImportance(const TierSelectionContext& context,
                                   const ImportanceThresholds& thresholds) const;
};

}  // namespace waxcpp
