#pragma once

#include "waxcpp/surrogate_generator.hpp"
#include "waxcpp/token_counter.hpp"
#include "waxcpp/wax_store.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace waxcpp {

/// Configuration for a maintenance run.
struct MaintenanceOptions {
  std::optional<int> max_frames;         // Limit scan to N frames.
  std::optional<int> max_wall_time_ms;   // Time deadline (milliseconds).
  int surrogate_max_tokens = 60;         // Default tokens per surrogate.
  bool overwrite_existing = false;       // Force regenerate existing surrogates.
  bool enable_hierarchical = true;       // Enable 3-tier compression.
  SurrogateTierConfig tier_config = SurrogateTierConfig::Default();
};

/// Report from a maintenance run.
struct MaintenanceReport {
  int scanned_frames = 0;               // Total frames examined.
  int eligible_frames = 0;              // Frames meeting criteria for generation.
  int generated_surrogates = 0;         // New surrogates created.
  int superseded_surrogates = 0;        // Existing surrogates replaced.
  int skipped_up_to_date = 0;           // Skipped (already up-to-date).
  bool did_timeout = false;             // Operation exceeded time limit.
};

/// Run surrogate optimization on a WaxStore using the given generator.
/// Scans committed frames, identifies eligible active frames without surrogates,
/// generates surrogates, and persists them as store frames with metadata.
///
/// The store must be open for writing. Commits in batches of 64 frames.
MaintenanceReport OptimizeSurrogates(
    WaxStore& store,
    const ExtractiveSurrogateGenerator& generator,
    const MaintenanceOptions& options = {});

}  // namespace waxcpp
