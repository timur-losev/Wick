#pragma once

#include "waxcpp/types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace waxcpp {

struct WaxStats {
  std::uint64_t frame_count = 0;
  std::uint64_t pending_frames = 0;
  std::uint64_t generation = 0;
};

struct WaxWALStats {
  std::uint64_t wal_size = 0;
  std::uint64_t write_pos = 0;
  std::uint64_t checkpoint_pos = 0;
  std::uint64_t pending_bytes = 0;
  std::uint64_t committed_seq = 0;
  std::uint64_t last_seq = 0;
  std::uint64_t wrap_count = 0;
  std::uint64_t checkpoint_count = 0;
  std::uint64_t sentinel_write_count = 0;
  std::uint64_t write_call_count = 0;
  std::uint64_t auto_commit_count = 0;
  std::uint64_t pending_embedding_mutations = 0;
  std::uint64_t pending_delete_mutations = 0;
  std::uint64_t pending_supersede_mutations = 0;
  std::uint64_t replay_snapshot_hit_count = 0;
};

struct WaxFrameMeta {
  std::uint64_t id = 0;
  std::int64_t timestamp_ms = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_length = 0;
  std::uint8_t canonical_encoding = 0;
  std::uint8_t status = 0;
  std::optional<std::string> kind;
  std::unordered_map<std::string, std::string> metadata;
  std::vector<std::pair<std::string, std::string>> tags;
  std::vector<std::string> labels;
  std::optional<std::uint64_t> supersedes;
  std::optional<std::uint64_t> superseded_by;
};

struct WaxPendingEmbedding {
  std::uint64_t frame_id = 0;
  std::uint32_t dimension = 0;
  std::vector<float> vector{};
};

struct WaxPendingEmbeddingSnapshot {
  std::vector<WaxPendingEmbedding> embeddings{};
  std::optional<std::uint64_t> latest_sequence{};
};

class WaxStore {
 public:
  WaxStore(const WaxStore&) = delete;
  WaxStore& operator=(const WaxStore&) = delete;
  WaxStore(WaxStore&&) noexcept = default;
  WaxStore& operator=(WaxStore&&) noexcept = default;
  ~WaxStore() = default;

  static WaxStore Create(const std::filesystem::path& path);
  static WaxStore Open(const std::filesystem::path& path, bool repair);
  static WaxStore Open(const std::filesystem::path& path);

  void Verify(bool deep);

  std::uint64_t Put(const std::vector<std::byte>& content, const Metadata& metadata = {});
  std::vector<std::uint64_t> PutBatch(const std::vector<std::vector<std::byte>>& contents,
                                      const std::vector<Metadata>& metadatas);
  void PutEmbedding(std::uint64_t frame_id, const std::vector<float>& vector);
  void PutEmbeddingBatch(const std::vector<std::uint64_t>& frame_ids,
                         const std::vector<std::vector<float>>& vectors);
  void Delete(std::uint64_t frame_id);
  void Supersede(std::uint64_t superseded_id, std::uint64_t superseding_id);

  void Commit();
  void Close();
  bool TryRefreshIfPublishedCommitVisible();

 [[nodiscard]] WaxStats Stats() const;
 [[nodiscard]] WaxWALStats WalStats() const;
 [[nodiscard]] std::optional<WaxFrameMeta> FrameMeta(std::uint64_t frame_id,
                                                     bool include_pending = false) const;
 [[nodiscard]] std::vector<WaxFrameMeta> FrameMetas(bool include_pending = false) const;
 /// Returns a const reference to the committed frame metadata — zero-copy.
 /// Callers must NOT hold this reference across mutations (Put/Delete/Commit).
 [[nodiscard]] const std::vector<WaxFrameMeta>& CommittedFrameMetasRef() const;
 [[nodiscard]] std::vector<std::byte> FrameContent(std::uint64_t frame_id,
                                                   bool include_pending = false) const;
 [[nodiscard]] std::unordered_map<std::uint64_t, std::vector<std::byte>> FrameContents(const std::vector<std::uint64_t>& frame_ids) const;
 [[nodiscard]] WaxPendingEmbeddingSnapshot PendingEmbeddingMutations(std::optional<std::uint64_t> since_sequence = std::nullopt) const;
 [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  void LoadState(bool deep_verify, bool repair_trailing_bytes);
  explicit WaxStore(std::filesystem::path path);

  std::filesystem::path path_;
  std::uint64_t file_generation_ = 0;
  std::uint64_t header_page_generation_ = 0;
  std::uint64_t wal_offset_ = 0;
  std::uint64_t wal_size_ = 0;
  std::uint64_t wal_committed_seq_ = 0;
  std::uint64_t wal_write_pos_ = 0;
  std::uint64_t wal_checkpoint_pos_ = 0;
  std::uint64_t wal_pending_bytes_ = 0;
  std::uint64_t wal_last_sequence_ = 0;
  std::uint64_t wal_wrap_count_ = 0;
  std::uint64_t wal_checkpoint_count_ = 0;
  std::uint64_t wal_sentinel_write_count_ = 0;
  std::uint64_t wal_write_call_count_ = 0;
  std::uint64_t wal_auto_commit_count_ = 0;
  std::uint64_t wal_pending_embedding_mutations_ = 0;
  std::uint64_t wal_pending_delete_mutations_ = 0;
  std::uint64_t wal_pending_supersede_mutations_ = 0;
  std::uint64_t wal_replay_snapshot_hit_count_ = 0;
  std::uint64_t footer_offset_ = 0;
  std::uint64_t next_frame_id_ = 0;

  enum class PendingMutationKind : std::uint8_t {
    kPutFrame = 1,
    kDeleteFrame = 2,
    kSupersedeFrame = 3,
    kPutEmbedding = 4,
  };

  struct PendingPutFrameCache {
    std::uint64_t frame_id = 0;
    std::int64_t timestamp_ms = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_length = 0;
    std::uint8_t canonical_encoding = 0;
    std::uint64_t canonical_length = 0;
    std::array<std::byte, 32> canonical_checksum{};
    std::array<std::byte, 32> stored_checksum{};
    std::optional<std::string> kind;
    Metadata metadata{};
    std::vector<std::pair<std::string, std::string>> tags{};
    std::vector<std::string> labels{};
  };

  struct PendingDeleteFrameCache {
    std::uint64_t frame_id = 0;
  };

  struct PendingSupersedeFrameCache {
    std::uint64_t superseded_id = 0;
    std::uint64_t superseding_id = 0;
  };

  std::vector<std::pair<std::uint64_t, PendingMutationKind>> pending_mutation_order_cache_{};
  std::unordered_map<std::uint64_t, PendingPutFrameCache> pending_put_frame_cache_{};
  std::unordered_map<std::uint64_t, PendingDeleteFrameCache> pending_delete_frame_cache_{};
  std::unordered_map<std::uint64_t, PendingSupersedeFrameCache> pending_supersede_frame_cache_{};
  std::unordered_map<std::uint64_t, WaxPendingEmbedding> pending_embedding_cache_{};

  bool dirty_ = false;
  bool has_local_mutations_ = false;
  bool is_open_ = false;
  std::vector<WaxFrameMeta> committed_frame_metas_{};
  std::shared_ptr<void> writer_lease_{};
  WaxStats stats_{};
};

}  // namespace waxcpp
