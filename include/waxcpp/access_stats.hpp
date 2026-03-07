#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace waxcpp {

/// Access statistics for a single frame.
struct FrameAccessStats {
  std::uint64_t frame_id = 0;
  std::uint32_t access_count = 0;
  std::int64_t last_access_ms = 0;
  std::int64_t first_access_ms = 0;

  FrameAccessStats() = default;
  explicit FrameAccessStats(std::uint64_t frame_id, std::int64_t now_ms);

  /// Record a new access, incrementing count and updating last_access_ms.
  void RecordAccess(std::int64_t now_ms);
};

/// Current epoch time in milliseconds.
inline std::int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

/// Thread-safe manager for frame access statistics.
/// C++ equivalent of the Swift `AccessStatsManager` actor.
class AccessStatsManager {
 public:
  AccessStatsManager() = default;

  /// Record a single frame access (uses wall-clock time).
  void RecordAccess(std::uint64_t frame_id);

  /// Record a single frame access at a specific timestamp.
  void RecordAccess(std::uint64_t frame_id, std::int64_t now_ms);

  /// Record accesses for multiple frames at once.
  void RecordAccesses(const std::vector<std::uint64_t>& frame_ids);

  /// Record accesses for multiple frames at a specific timestamp.
  void RecordAccesses(const std::vector<std::uint64_t>& frame_ids,
                      std::int64_t now_ms);

  /// Get stats for a single frame.
  std::optional<FrameAccessStats> GetStats(std::uint64_t frame_id) const;

  /// Get stats for multiple frames.
  std::unordered_map<std::uint64_t, FrameAccessStats> GetStats(
      const std::vector<std::uint64_t>& frame_ids) const;

  /// Remove stats for frames not in the active set.
  void PruneStats(const std::set<std::uint64_t>& active_frame_ids);

  /// Export all stats for persistence.
  std::vector<FrameAccessStats> ExportStats() const;

  /// Import stats from persistence (replaces all current stats).
  void ImportStats(const std::vector<FrameAccessStats>& imported);

  /// Total number of tracked frames.
  std::size_t Count() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, FrameAccessStats> stats_;
};

}  // namespace waxcpp
