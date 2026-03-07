#include "waxcpp/access_stats.hpp"

#include <algorithm>
#include <limits>

namespace waxcpp {

// ---------- FrameAccessStats ----------

FrameAccessStats::FrameAccessStats(std::uint64_t frame_id, std::int64_t now_ms)
    : frame_id(frame_id),
      access_count(1),
      last_access_ms(now_ms),
      first_access_ms(now_ms) {}

void FrameAccessStats::RecordAccess(std::int64_t now_ms) {
  // Saturating addition to prevent overflow (matches Swift behavior).
  if (access_count < std::numeric_limits<std::uint32_t>::max()) {
    ++access_count;
  }
  last_access_ms = now_ms;
}

// ---------- AccessStatsManager ----------

void AccessStatsManager::RecordAccess(std::uint64_t frame_id) {
  RecordAccess(frame_id, NowMs());
}

void AccessStatsManager::RecordAccess(std::uint64_t frame_id,
                                      std::int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = stats_.find(frame_id);
  if (it != stats_.end()) {
    it->second.RecordAccess(now_ms);
  } else {
    stats_.emplace(frame_id, FrameAccessStats(frame_id, now_ms));
  }
}

void AccessStatsManager::RecordAccesses(
    const std::vector<std::uint64_t>& frame_ids) {
  RecordAccesses(frame_ids, NowMs());
}

void AccessStatsManager::RecordAccesses(
    const std::vector<std::uint64_t>& frame_ids, std::int64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto fid : frame_ids) {
    auto it = stats_.find(fid);
    if (it != stats_.end()) {
      it->second.RecordAccess(now_ms);
    } else {
      stats_.emplace(fid, FrameAccessStats(fid, now_ms));
    }
  }
}

std::optional<FrameAccessStats> AccessStatsManager::GetStats(
    std::uint64_t frame_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = stats_.find(frame_id);
  if (it != stats_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::unordered_map<std::uint64_t, FrameAccessStats>
AccessStatsManager::GetStats(
    const std::vector<std::uint64_t>& frame_ids) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::unordered_map<std::uint64_t, FrameAccessStats> result;
  result.reserve(frame_ids.size());
  for (const auto fid : frame_ids) {
    auto it = stats_.find(fid);
    if (it != stats_.end()) {
      result.emplace(fid, it->second);
    }
  }
  return result;
}

void AccessStatsManager::PruneStats(
    const std::set<std::uint64_t>& active_frame_ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = stats_.begin(); it != stats_.end();) {
    if (active_frame_ids.count(it->first) == 0) {
      it = stats_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<FrameAccessStats> AccessStatsManager::ExportStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FrameAccessStats> result;
  result.reserve(stats_.size());
  for (const auto& [_, stats] : stats_) {
    result.push_back(stats);
  }
  return result;
}

void AccessStatsManager::ImportStats(
    const std::vector<FrameAccessStats>& imported) {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.clear();
  stats_.reserve(imported.size());
  for (const auto& s : imported) {
    stats_.emplace(s.frame_id, s);
  }
}

std::size_t AccessStatsManager::Count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_.size();
}

}  // namespace waxcpp
