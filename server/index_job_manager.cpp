#include "index_job_manager.hpp"
#include "server_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace waxcpp::server {

namespace {

bool ParseBool(std::string_view text, bool fallback) {
  const auto normalized = ToAsciiLower(text);
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  return fallback;
}

std::optional<std::uint64_t> ParseU64(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(std::string(text), &consumed, 10);
    if (consumed != text.size()) {
      return std::nullopt;
    }
    return parsed;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

std::string ToString(IndexJobState state) {
  switch (state) {
    case IndexJobState::kIdle:
      return "idle";
    case IndexJobState::kRunning:
      return "running";
    case IndexJobState::kStopped:
      return "stopped";
    case IndexJobState::kFailed:
      return "failed";
  }
  return "idle";
}

std::optional<IndexJobState> ParseIndexJobState(std::string_view text) {
  const auto normalized = ToAsciiLower(text);
  if (normalized == "idle") {
    return IndexJobState::kIdle;
  }
  if (normalized == "running") {
    return IndexJobState::kRunning;
  }
  if (normalized == "stopped") {
    return IndexJobState::kStopped;
  }
  if (normalized == "failed") {
    return IndexJobState::kFailed;
  }
  return std::nullopt;
}

IndexJobManager::IndexJobManager(std::filesystem::path checkpoint_path)
    : checkpoint_path_(std::move(checkpoint_path)) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.checkpoint_path = checkpoint_path_;
  LoadLocked();
}

IndexJobStatus IndexJobManager::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

bool IndexJobManager::Start(const std::filesystem::path& repo_root, bool resume_requested) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state == IndexJobState::kRunning) {
    return false;
  }

  const auto now_ms = NowMs();
  const auto previous_repo_root = status_.repo_root;
  const auto previous_scanned = status_.scanned_files;
  const auto previous_indexed = status_.indexed_chunks;
  const auto previous_committed = status_.committed_chunks;

  status_.generation += 1;
  status_.state = IndexJobState::kRunning;
  status_.phase = "starting";
  status_.resume_requested = resume_requested;
  status_.repo_root = repo_root.string();
  status_.last_error.reset();
  status_.started_at_ms = now_ms;
  status_.updated_at_ms = now_ms;
  status_.job_id = "index-" + std::to_string(status_.generation) + "-" + std::to_string(now_ms);

  const bool can_resume_counters =
      resume_requested && previous_repo_root.has_value() && *previous_repo_root == status_.repo_root;
  if (can_resume_counters) {
    status_.scanned_files = previous_scanned;
    status_.indexed_chunks = previous_indexed;
    status_.committed_chunks = previous_committed;
  } else {
    status_.scanned_files = 0;
    status_.total_chunks = 0;
    status_.indexed_chunks = 0;
    status_.committed_chunks = 0;
  }

  PersistLocked();
  return true;
}

bool IndexJobManager::Complete(std::uint64_t scanned_files,
                               std::uint64_t indexed_chunks,
                               std::uint64_t committed_chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != IndexJobState::kRunning) {
    return false;
  }
  status_.scanned_files = scanned_files;
  // On resume, never decrease counters — a no-op resume must not wipe previous progress.
  if (status_.resume_requested) {
    status_.indexed_chunks = std::max(status_.indexed_chunks, indexed_chunks);
    status_.committed_chunks = std::max(status_.committed_chunks, committed_chunks);
  } else {
    status_.indexed_chunks = indexed_chunks;
    status_.committed_chunks = committed_chunks;
  }
  status_.state = IndexJobState::kStopped;
  status_.phase = "completed";
  status_.updated_at_ms = NowMs();
  PersistLocked();
  return true;
}

bool IndexJobManager::UpdateProgress(std::uint64_t scanned_files,
                                     std::uint64_t indexed_chunks,
                                     std::uint64_t committed_chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != IndexJobState::kRunning) {
    return false;
  }
  status_.scanned_files = scanned_files;
  status_.indexed_chunks = indexed_chunks;
  status_.committed_chunks = committed_chunks;
  status_.updated_at_ms = NowMs();
  PersistLocked();
  return true;
}

bool IndexJobManager::SetPhase(std::string phase) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != IndexJobState::kRunning) {
    return false;
  }
  if (phase.empty()) {
    phase = "running";
  }
  status_.phase = std::move(phase);
  status_.updated_at_ms = NowMs();
  PersistLocked();
  return true;
}

void IndexJobManager::SetTotalChunks(std::uint64_t total) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.total_chunks = total;
}

bool IndexJobManager::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != IndexJobState::kRunning) {
    return false;
  }
  status_.state = IndexJobState::kStopped;
  status_.phase = "stopped";
  status_.updated_at_ms = NowMs();
  PersistLocked();
  return true;
}

bool IndexJobManager::Fail(std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != IndexJobState::kRunning) {
    return false;
  }
  status_.state = IndexJobState::kFailed;
  status_.phase = "failed";
  status_.last_error = std::move(error);
  status_.updated_at_ms = NowMs();
  PersistLocked();
  return true;
}

std::uint64_t IndexJobManager::NowMs() {
  const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now());
  return static_cast<std::uint64_t>(now.time_since_epoch().count());
}

void IndexJobManager::PersistLocked() const {
  std::error_code ec;
  const auto parent = checkpoint_path_.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error("failed to create index checkpoint directory: " + parent.string());
    }
  }

  std::ofstream out(checkpoint_path_, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open index checkpoint file for write: " + checkpoint_path_.string());
  }

  out << "state=" << ToString(status_.state) << "\n";
  out << "phase=" << status_.phase << "\n";
  out << "generation=" << status_.generation << "\n";
  out << "job_id=" << status_.job_id.value_or("") << "\n";
  out << "repo_root=" << status_.repo_root.value_or("") << "\n";
  out << "checkpoint_path=" << checkpoint_path_.string() << "\n";
  out << "started_at_ms=" << status_.started_at_ms << "\n";
  out << "updated_at_ms=" << status_.updated_at_ms << "\n";
  out << "scanned_files=" << status_.scanned_files << "\n";
  out << "total_chunks=" << status_.total_chunks << "\n";
  out << "indexed_chunks=" << status_.indexed_chunks << "\n";
  out << "committed_chunks=" << status_.committed_chunks << "\n";
  out << "resume_requested=" << (status_.resume_requested ? "1" : "0") << "\n";
  out << "last_error=" << status_.last_error.value_or("") << "\n";
  if (!out) {
    throw std::runtime_error("failed to write index checkpoint file: " + checkpoint_path_.string());
  }
}

void IndexJobManager::LoadLocked() {
  std::ifstream in(checkpoint_path_, std::ios::binary);
  if (!in) {
    status_.checkpoint_path = checkpoint_path_;
    return;
  }

  std::unordered_map<std::string, std::string> entries{};
  std::string line{};
  while (std::getline(in, line)) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    entries[line.substr(0, pos)] = line.substr(pos + 1);
  }

  if (const auto it = entries.find("state"); it != entries.end()) {
    if (const auto parsed = ParseIndexJobState(it->second); parsed.has_value()) {
      status_.state = *parsed;
    }
  }
  bool has_phase = false;
  if (const auto it = entries.find("phase"); it != entries.end() && !it->second.empty()) {
    has_phase = true;
    status_.phase = it->second;
  }
  if (const auto it = entries.find("generation"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.generation = *parsed;
    }
  }
  if (const auto it = entries.find("job_id"); it != entries.end() && !it->second.empty()) {
    status_.job_id = it->second;
  }
  if (const auto it = entries.find("repo_root"); it != entries.end() && !it->second.empty()) {
    status_.repo_root = it->second;
  }
  if (const auto it = entries.find("started_at_ms"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.started_at_ms = *parsed;
    }
  }
  if (const auto it = entries.find("updated_at_ms"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.updated_at_ms = *parsed;
    }
  }
  if (const auto it = entries.find("scanned_files"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.scanned_files = *parsed;
    }
  }
  if (const auto it = entries.find("total_chunks"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.total_chunks = *parsed;
    }
  }
  if (const auto it = entries.find("indexed_chunks"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.indexed_chunks = *parsed;
    }
  }
  if (const auto it = entries.find("committed_chunks"); it != entries.end()) {
    if (const auto parsed = ParseU64(it->second); parsed.has_value()) {
      status_.committed_chunks = *parsed;
    }
  }
  if (const auto it = entries.find("resume_requested"); it != entries.end()) {
    status_.resume_requested = ParseBool(it->second, false);
  }
  if (const auto it = entries.find("last_error"); it != entries.end() && !it->second.empty()) {
    status_.last_error = it->second;
  }

  // A previously running process is treated as stopped when this manager starts.
  if (status_.state == IndexJobState::kRunning) {
    status_.state = IndexJobState::kStopped;
    status_.phase = "stopped";
  } else if (!has_phase) {
    status_.phase = ToString(status_.state);
  }
  status_.checkpoint_path = checkpoint_path_;
}

}  // namespace waxcpp::server
