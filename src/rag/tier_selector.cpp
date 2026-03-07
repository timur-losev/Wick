#include "waxcpp/tier_selector.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace waxcpp {

namespace {

/// Parse the WAXSURR1 hierarchical format.
/// Format: "WAXSURR1\nFULL:<full>\nGIST:<gist>\nMICRO:<micro>"
struct ParsedTiers {
  std::string full;
  std::string gist;
  std::string micro;
};

std::optional<ParsedTiers> ParseWaxSurr1(std::string_view data) {
  constexpr std::string_view kHeader = "WAXSURR1\n";
  if (data.size() < kHeader.size() ||
      data.substr(0, kHeader.size()) != kHeader) {
    return std::nullopt;
  }

  ParsedTiers result;
  auto remaining = data.substr(kHeader.size());

  // Parse FULL: line
  constexpr std::string_view kFullPrefix = "FULL:";
  if (remaining.substr(0, kFullPrefix.size()) != kFullPrefix) {
    return std::nullopt;
  }
  remaining.remove_prefix(kFullPrefix.size());
  auto nl = remaining.find('\n');
  if (nl == std::string_view::npos) {
    return std::nullopt;
  }
  result.full = std::string(remaining.substr(0, nl));
  remaining.remove_prefix(nl + 1);

  // Parse GIST: line
  constexpr std::string_view kGistPrefix = "GIST:";
  if (remaining.substr(0, kGistPrefix.size()) != kGistPrefix) {
    return std::nullopt;
  }
  remaining.remove_prefix(kGistPrefix.size());
  nl = remaining.find('\n');
  if (nl == std::string_view::npos) {
    return std::nullopt;
  }
  result.gist = std::string(remaining.substr(0, nl));
  remaining.remove_prefix(nl + 1);

  // Parse MICRO: line
  constexpr std::string_view kMicroPrefix = "MICRO:";
  if (remaining.substr(0, kMicroPrefix.size()) != kMicroPrefix) {
    return std::nullopt;
  }
  remaining.remove_prefix(kMicroPrefix.size());
  result.micro = std::string(remaining);

  return result;
}

}  // namespace

// ---------- SurrogateTierSelector ----------

SurrogateTierSelector::SurrogateTierSelector()
    : policy(TierPolicyImportanceBalanced()),
      scorer(ImportanceScorer()),
      query_boost_factor(0.15f) {}

SurrogateTierSelector::SurrogateTierSelector(TierSelectionPolicy policy,
                                             ImportanceScorer scorer,
                                             float query_boost_factor)
    : policy(std::move(policy)),
      scorer(std::move(scorer)),
      query_boost_factor(query_boost_factor) {}

SurrogateTier SurrogateTierSelector::SelectTier(
    const TierSelectionContext& context) const {
  return std::visit(
      [this, &context](const auto& p) -> SurrogateTier {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, TierPolicyDisabled>) {
          return SurrogateTier::kFull;
        } else if constexpr (std::is_same_v<T, TierPolicyAgeOnly>) {
          return SelectByAge(context, p.thresholds);
        } else if constexpr (std::is_same_v<T, TierPolicyImportance>) {
          return SelectByImportance(context, p.thresholds);
        } else {
          return SurrogateTier::kFull;
        }
      },
      policy);
}

SurrogateTier SurrogateTierSelector::SelectByAge(
    const TierSelectionContext& context,
    const AgeThresholds& thresholds) const {
  const auto age_ms = context.now_ms - context.frame_timestamp_ms;
  if (age_ms < thresholds.RecentMs()) {
    return SurrogateTier::kFull;
  } else if (age_ms < thresholds.OldMs()) {
    return SurrogateTier::kGist;
  } else {
    return SurrogateTier::kMicro;
  }
}

SurrogateTier SurrogateTierSelector::SelectByImportance(
    const TierSelectionContext& context,
    const ImportanceThresholds& thresholds) const {
  // Calculate base importance.
  auto importance = scorer.Score(
      context.frame_timestamp_ms,
      context.access_stats,
      context.now_ms);

  // Apply query boost if query has high specificity.
  if (context.query_signals != nullptr) {
    importance.score += context.query_signals->specificity_score * query_boost_factor;
    importance.score = std::min(1.0f, importance.score);
  }

  // Select tier based on boosted importance.
  if (importance.score >= thresholds.full_threshold) {
    return SurrogateTier::kFull;
  } else if (importance.score >= thresholds.gist_threshold) {
    return SurrogateTier::kGist;
  } else {
    return SurrogateTier::kMicro;
  }
}

std::optional<std::string> SurrogateTierSelector::ExtractTier(
    std::string_view data, SurrogateTier tier) {
  // Try hierarchical WAXSURR1 format first.
  auto parsed = ParseWaxSurr1(data);
  if (parsed.has_value()) {
    switch (tier) {
      case SurrogateTier::kFull:
        return parsed->full;
      case SurrogateTier::kGist:
        return parsed->gist;
      case SurrogateTier::kMicro:
        return parsed->micro;
    }
  }

  // Fallback: legacy single-tier (plain text).
  return std::string(data);
}

}  // namespace waxcpp
