#include "waxcpp/live_set_rewrite.hpp"
#include "waxcpp/wax_store.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <stdexcept>

namespace waxcpp {

// ── EvaluateMaintenanceGate ─────────────────────────────────

bool EvaluateMaintenanceGate(const MaintenanceGateInput& input,
                             ScheduledLiveSetMaintenanceReport& report) {
  report = {};
  report.flush_count = input.flush_count;
  report.dead_payload_bytes = input.dead_payload_bytes;
  report.total_payload_bytes = input.total_payload_bytes;
  report.dead_payload_fraction = input.total_payload_bytes > 0
      ? static_cast<double>(input.dead_payload_bytes) /
            static_cast<double>(input.total_payload_bytes)
      : 0.0;

  if (input.schedule == nullptr || !input.schedule->enabled) {
    report.outcome = MaintenanceOutcome::kDisabled;
    report.notes = "live-set rewrite schedule is disabled";
    if (input.force) {
      return false;
    }
    return false;
  }

  const auto& sched = *input.schedule;

  // Cadence gate: only check every N flushes (unless forced).
  if (!input.force) {
    const auto cadence = static_cast<std::uint64_t>(std::max(1, sched.check_every_flushes));
    if (input.flush_count % cadence != 0) {
      report.outcome = MaintenanceOutcome::kCadenceSkipped;
      report.notes = "cadence gate: flush " + std::to_string(input.flush_count) +
                     "; every " + std::to_string(cadence) + " flushes";
      return false;
    }
  }

  // Cooldown gate: minimum interval between runs.
  if (!input.force && sched.min_interval_ms > 0 && input.last_completed_ms > 0) {
    const auto next_allowed_ms =
        input.last_completed_ms + static_cast<std::int64_t>(std::max(0, sched.min_interval_ms));
    if (input.now_ms < next_allowed_ms) {
      report.outcome = MaintenanceOutcome::kCooldownSkipped;
      report.notes = "cooldown gate: waiting for minimum interval";
      return false;
    }
  }

  // Idle gate: recent write activity.
  if (!input.force && sched.minimum_idle_ms > 0 && input.last_write_activity_ms > 0) {
    const auto idle_eligible_ms =
        input.last_write_activity_ms + static_cast<std::int64_t>(std::max(0, sched.minimum_idle_ms));
    if (input.now_ms < idle_eligible_ms) {
      report.outcome = MaintenanceOutcome::kIdleSkipped;
      report.notes = "idle gate: recent writes detected";
      return false;
    }
  }

  // Threshold gate: check dead payload thresholds.
  const double clamped_fraction_threshold =
      std::min(1.0, std::max(0.0, sched.min_dead_payload_fraction));
  const bool meets_bytes = input.dead_payload_bytes >= sched.min_dead_payload_bytes;
  const bool meets_fraction = report.dead_payload_fraction >= clamped_fraction_threshold;

  if (!meets_bytes && !meets_fraction) {
    report.outcome = MaintenanceOutcome::kBelowThreshold;
    report.notes = "below thresholds: bytes=" +
                   std::to_string(input.dead_payload_bytes) + "/" +
                   std::to_string(sched.min_dead_payload_bytes) +
                   " fraction=" + std::to_string(report.dead_payload_fraction) +
                   "/" + std::to_string(clamped_fraction_threshold);
    return false;
  }

  // All gates passed — eligible for rewrite.
  return true;
}

// ── RewriteLiveSet ──────────────────────────────────────────

namespace {

std::uint64_t FileLogicalSize(const std::filesystem::path& p) {
  std::error_code ec;
  const auto sz = std::filesystem::file_size(p, ec);
  return ec ? 0 : sz;
}

/// Generate a random hex suffix for candidate filenames.
std::string RandomHexSuffix(int chars = 16) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned> dist(0, 15);
  const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(static_cast<std::size_t>(chars));
  for (int i = 0; i < chars; ++i) {
    out.push_back(hex[dist(rng)]);
  }
  return out;
}

/// Prune old rewrite candidate files in a directory, keeping the latest N.
void PruneScheduledRewriteCandidates(
    const std::filesystem::path& directory,
    const std::string& base_name,
    int keep_latest) {
  const int keep_count = std::max(0, keep_latest);
  const auto prefix = base_name + "-liveset-";

  std::vector<std::filesystem::path> candidates;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto name = entry.path().filename().string();
    if (name.size() > prefix.size() &&
        name.compare(0, prefix.size(), prefix) == 0 &&
        name.size() > 5 &&
        name.compare(name.size() - 5, 5, ".mv2s") == 0) {
      candidates.push_back(entry.path());
    }
  }

  if (static_cast<int>(candidates.size()) <= keep_count) return;

  // Sort by last-write-time descending (newest first).
  std::sort(candidates.begin(), candidates.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              std::error_code e1, e2;
              return std::filesystem::last_write_time(a, e1) >
                     std::filesystem::last_write_time(b, e2);
            });

  for (std::size_t i = static_cast<std::size_t>(keep_count); i < candidates.size(); ++i) {
    std::error_code rm_ec;
    std::filesystem::remove(candidates[i], rm_ec);
  }
}

}  // namespace

LiveSetRewriteReport RewriteLiveSet(WaxStore& source,
                                     const std::filesystem::path& destination,
                                     const LiveSetRewriteOptions& options) {
  const auto started = std::chrono::steady_clock::now();

  const auto& source_path = source.Path();
  const auto source_metas = source.FrameMetas();

  if (source_path == destination) {
    throw std::runtime_error(
        "RewriteLiveSet: destination must differ from source");
  }

  if (std::filesystem::exists(destination)) {
    if (!options.overwrite_destination) {
      throw std::runtime_error("RewriteLiveSet: destination already exists: " +
                               destination.string());
    }
    std::filesystem::remove(destination);
  }

  const std::uint64_t allocated_before = FileLogicalSize(source_path);
  std::uint64_t source_logical_bytes = 0;
  for (const auto& m : source_metas) {
    source_logical_bytes += m.payload_length;
  }

  auto dest = WaxStore::Create(destination);
  int dropped_payload_frames = 0;
  int deleted_frame_count = 0;
  int superseded_frame_count = 0;
  int active_frame_count = 0;

  try {
    for (const auto& frame : source_metas) {
      const bool is_active = frame.status == 0;  // 0 = active in mv2s
      const bool is_live = is_active && !frame.superseded_by.has_value();

      if (is_live) ++active_frame_count;
      if (frame.status != 0) ++deleted_frame_count;
      if (frame.superseded_by.has_value()) ++superseded_frame_count;

      std::vector<std::byte> content;
      if (options.drop_non_live_payloads && !is_live) {
        ++dropped_payload_frames;
      } else {
        content = source.FrameContent(frame.id);
      }

      const auto rewritten_id = dest.Put(content, {});
      if (rewritten_id != frame.id) {
        throw std::runtime_error(
            "RewriteLiveSet: frame id mismatch: expected " +
            std::to_string(frame.id) + ", got " +
            std::to_string(rewritten_id));
      }
    }

    dest.Commit();

    if (options.verify_deep) {
      dest.Verify(true);
    }

    dest.Close();
  } catch (...) {
    try { dest.Close(); } catch (...) {}
    std::error_code ec;
    std::filesystem::remove(destination, ec);
    throw;
  }

  const auto ended = std::chrono::steady_clock::now();
  const double duration_ms =
      std::chrono::duration<double, std::milli>(ended - started).count();

  const std::uint64_t allocated_after = FileLogicalSize(destination);

  // Compute destination logical payload bytes.
  std::uint64_t dest_logical_bytes = 0;
  {
    auto reopened = WaxStore::Open(destination);
    for (const auto& m : reopened.FrameMetas()) {
      dest_logical_bytes += m.payload_length;
    }
    reopened.Close();
  }

  LiveSetRewriteReport report;
  report.source_path = source_path.string();
  report.destination_path = destination.string();
  report.frame_count = static_cast<int>(source_metas.size());
  report.active_frame_count = active_frame_count;
  report.dropped_payload_frames = dropped_payload_frames;
  report.deleted_frame_count = deleted_frame_count;
  report.superseded_frame_count = superseded_frame_count;
  report.copied_lex_index = false;   // C++ rebuilds indexes on open
  report.copied_vec_index = false;   // C++ rebuilds indexes on open
  report.logical_bytes_before = source_logical_bytes;
  report.logical_bytes_after = dest_logical_bytes;
  report.allocated_bytes_before = allocated_before;
  report.allocated_bytes_after = allocated_after;
  report.duration_ms = duration_ms;

  return report;
}

// ── RunScheduledLiveSetMaintenance ──────────────────────────

ScheduledLiveSetMaintenanceReport RunScheduledLiveSetMaintenance(
    WaxStore& source,
    const LiveSetRewriteSchedule& schedule,
    std::uint64_t flush_count,
    bool force,
    std::int64_t now_ms,
    std::int64_t last_completed_ms,
    std::int64_t last_write_activity_ms) {
  // Step 1: Compute dead-payload statistics from the store.
  const auto metas = source.FrameMetas();
  std::uint64_t total_payload_bytes = 0;
  std::uint64_t dead_payload_bytes = 0;
  for (const auto& frame : metas) {
    if (frame.payload_length == 0) continue;
    total_payload_bytes += frame.payload_length;
    const bool is_live = (frame.status == 0) && !frame.superseded_by.has_value();
    if (!is_live) {
      dead_payload_bytes += frame.payload_length;
    }
  }

  // Step 2: Evaluate the gate.
  MaintenanceGateInput gate_input;
  gate_input.schedule = &schedule;
  gate_input.flush_count = flush_count;
  gate_input.force = force;
  gate_input.now_ms = now_ms;
  gate_input.last_completed_ms = last_completed_ms;
  gate_input.last_write_activity_ms = last_write_activity_ms;
  gate_input.dead_payload_bytes = dead_payload_bytes;
  gate_input.total_payload_bytes = total_payload_bytes;

  ScheduledLiveSetMaintenanceReport report;
  const bool eligible = EvaluateMaintenanceGate(gate_input, report);
  report.triggered_by_flush = !force;

  if (!eligible) {
    return report;
  }

  // Step 3: Determine candidate path.
  const auto& source_path = source.Path();
  std::filesystem::path dest_dir;
  if (!schedule.destination_directory.empty()) {
    dest_dir = schedule.destination_directory;
  } else {
    dest_dir = source_path.parent_path();
  }
  std::filesystem::create_directories(dest_dir);

  const auto base_name = source_path.stem().string();
  const auto candidate_name = base_name + "-liveset-" + RandomHexSuffix() + ".mv2s";
  const auto candidate_path = dest_dir / candidate_name;
  report.candidate_path = candidate_path.string();

  // Step 4: Perform the rewrite.
  LiveSetRewriteReport rewrite_report;
  try {
    rewrite_report = RewriteLiveSet(source, candidate_path, {
        /*overwrite_destination=*/true,
        /*drop_non_live_payloads=*/true,
        /*verify_deep=*/schedule.verify_deep,
    });
  } catch (const std::exception& ex) {
    std::error_code ec;
    std::filesystem::remove(candidate_path, ec);
    report.outcome = MaintenanceOutcome::kRewriteFailed;
    report.rollback_performed = true;
    report.notes = std::string("rewrite failed: ") + ex.what();
    return report;
  } catch (...) {
    std::error_code ec;
    std::filesystem::remove(candidate_path, ec);
    report.outcome = MaintenanceOutcome::kRewriteFailed;
    report.rollback_performed = true;
    report.notes = "rewrite failed: unknown exception";
    return report;
  }

  report.rewrite_report = rewrite_report;

  // Step 5: Validate the candidate.
  bool validation_failed = false;
  std::string validation_notes;

  const std::uint64_t compaction_gain =
      (rewrite_report.logical_bytes_before > rewrite_report.logical_bytes_after)
          ? rewrite_report.logical_bytes_before - rewrite_report.logical_bytes_after
          : 0;

  if (compaction_gain < schedule.minimum_compaction_gain_bytes) {
    validation_failed = true;
    validation_notes += "compaction gain below threshold: gained " +
                        std::to_string(compaction_gain) +
                        ", required " +
                        std::to_string(schedule.minimum_compaction_gain_bytes) + "; ";
  }

  // Verify frame count by reopening.
  try {
    auto rewritten = WaxStore::Open(candidate_path);
    const auto rewritten_stats = rewritten.Stats();
    if (rewritten_stats.frame_count != static_cast<std::uint64_t>(rewrite_report.frame_count)) {
      validation_failed = true;
      validation_notes += "frame count mismatch: expected " +
                          std::to_string(rewrite_report.frame_count) +
                          ", got " +
                          std::to_string(rewritten_stats.frame_count) + "; ";
    }
    if (schedule.verify_deep) {
      rewritten.Verify(true);
    }
    rewritten.Close();
  } catch (const std::exception& ex) {
    validation_failed = true;
    validation_notes += std::string("verification failed: ") + ex.what() + "; ";
  }

  if (validation_failed) {
    std::error_code ec;
    std::filesystem::remove(candidate_path, ec);
    report.outcome = MaintenanceOutcome::kValidationFailedRolledBack;
    report.rollback_performed = true;
    report.notes = validation_notes;
    return report;
  }

  // Step 6: Prune old candidates.
  PruneScheduledRewriteCandidates(dest_dir, base_name, schedule.keep_latest_candidates);

  report.outcome = MaintenanceOutcome::kRewriteSucceeded;
  report.notes = "rewrite candidate validated; compaction gain bytes: " +
                 std::to_string(compaction_gain);
  return report;
}

}  // namespace waxcpp
