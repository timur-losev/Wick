#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace waxcpp::server {

enum class IndexJobState : std::uint8_t {
  kIdle = 0,
  kRunning,
  kStopped,
  kFailed,
};

struct IndexJobStatus {
  IndexJobState state = IndexJobState::kIdle;
  std::string phase = "idle";
  std::uint64_t generation = 0;
  std::optional<std::string> job_id;
  std::optional<std::string> repo_root;
  std::filesystem::path checkpoint_path{};
  std::uint64_t started_at_ms = 0;
  std::uint64_t updated_at_ms = 0;
  std::uint64_t scanned_files = 0;
  std::uint64_t total_chunks = 0;
  std::uint64_t indexed_chunks = 0;
  std::uint64_t committed_chunks = 0;
  bool resume_requested = false;
  std::optional<std::string> last_error;
};

[[nodiscard]] std::string ToString(IndexJobState state);
[[nodiscard]] std::optional<IndexJobState> ParseIndexJobState(std::string_view text);

class IndexJobManager {
 public:
  explicit IndexJobManager(std::filesystem::path checkpoint_path);

  [[nodiscard]] IndexJobStatus status() const;
  [[nodiscard]] bool Start(const std::filesystem::path& repo_root, bool resume_requested);
  [[nodiscard]] bool Complete(std::uint64_t scanned_files,
                              std::uint64_t indexed_chunks,
                              std::uint64_t committed_chunks);
  [[nodiscard]] bool UpdateProgress(std::uint64_t scanned_files,
                                    std::uint64_t indexed_chunks,
                                    std::uint64_t committed_chunks);
  [[nodiscard]] bool SetPhase(std::string phase);
  void SetTotalChunks(std::uint64_t total);
  [[nodiscard]] bool Stop();
  [[nodiscard]] bool Fail(std::string error);

 private:
  static std::uint64_t NowMs();
  void PersistLocked() const;
  void LoadLocked();

  mutable std::mutex mutex_{};
  IndexJobStatus status_{};
  std::filesystem::path checkpoint_path_{};
};

}  // namespace waxcpp::server
