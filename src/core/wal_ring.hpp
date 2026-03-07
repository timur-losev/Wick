#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace waxcpp::core::wal {

inline constexpr std::uint64_t kRecordHeaderSize = 48;
inline constexpr std::uint32_t kFlagIsPadding = 1U << 0U;

struct WalRecordHeader {
  std::uint64_t sequence = 0;
  std::uint32_t length = 0;
  std::uint32_t flags = 0;
  std::array<std::byte, 32> checksum{};

  [[nodiscard]] bool IsSentinel() const;
  [[nodiscard]] bool IsPadding() const;
};

struct WalScanState {
  std::uint64_t last_sequence = 0;
  std::uint64_t write_pos = 0;
  std::uint64_t pending_bytes = 0;
};

enum class WalMutationKind : std::uint8_t {
  kPutFrame = 1,
  kDeleteFrame = 2,
  kSupersedeFrame = 3,
  kPutEmbedding = 4,
};

struct WalPutFrameInfo {
  std::uint64_t frame_id = 0;
  std::int64_t timestamp_ms = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_length = 0;
  std::uint8_t canonical_encoding = 0;
  std::uint64_t canonical_length = 0;
  std::array<std::byte, 32> canonical_checksum{};
  std::array<std::byte, 32> stored_checksum{};
  std::optional<std::string> kind;
  std::unordered_map<std::string, std::string> metadata;
  std::vector<std::pair<std::string, std::string>> tags;
  std::vector<std::string> labels;
};

struct WalDeleteFrameInfo {
  std::uint64_t frame_id = 0;
};

struct WalSupersedeFrameInfo {
  std::uint64_t superseded_id = 0;
  std::uint64_t superseding_id = 0;
};

struct WalPutEmbeddingInfo {
  std::uint64_t frame_id = 0;
  std::uint32_t dimension = 0;
  std::vector<float> vector{};
};

struct WalPendingMutationInfo {
  std::uint64_t sequence = 0;
  WalMutationKind kind = WalMutationKind::kDeleteFrame;
  std::optional<WalPutFrameInfo> put_frame;
  std::optional<WalDeleteFrameInfo> delete_frame;
  std::optional<WalSupersedeFrameInfo> supersede_frame;
  std::optional<WalPutEmbeddingInfo> put_embedding;
};

struct WalPendingScanResult {
  std::vector<WalPendingMutationInfo> pending_mutations;
  WalScanState state{};
};

class WalRingWriter {
 public:
  WalRingWriter(std::filesystem::path path,
                std::uint64_t wal_offset,
                std::uint64_t wal_size,
                std::uint64_t write_pos = 0,
                std::uint64_t checkpoint_pos = 0,
                std::uint64_t pending_bytes = 0,
                std::uint64_t last_sequence = 0,
                std::uint64_t wrap_count = 0,
                std::uint64_t checkpoint_count = 0,
                std::uint64_t sentinel_write_count = 0,
                std::uint64_t write_call_count = 0);

  [[nodiscard]] bool CanAppend(std::size_t payload_size) const;
  [[nodiscard]] std::uint64_t Append(std::span<const std::byte> payload, std::uint32_t flags = 0);
  [[nodiscard]] std::vector<std::uint64_t> AppendBatch(const std::vector<std::vector<std::byte>>& payloads,
                                                       std::uint32_t flags = 0);
  void RecordCheckpoint();

  [[nodiscard]] std::uint64_t write_pos() const { return write_pos_; }
  [[nodiscard]] std::uint64_t checkpoint_pos() const { return checkpoint_pos_; }
  [[nodiscard]] std::uint64_t pending_bytes() const { return pending_bytes_; }
  [[nodiscard]] std::uint64_t last_sequence() const { return last_sequence_; }
  [[nodiscard]] std::uint64_t wrap_count() const { return wrap_count_; }
  [[nodiscard]] std::uint64_t checkpoint_count() const { return checkpoint_count_; }
  [[nodiscard]] std::uint64_t sentinel_write_count() const { return sentinel_write_count_; }
  [[nodiscard]] std::uint64_t write_call_count() const { return write_call_count_; }

 private:
  void WriteSentinel();
  void WriteAll(std::span<const std::byte> data, std::uint64_t file_offset);

  std::filesystem::path path_;
  std::uint64_t wal_offset_ = 0;
  std::uint64_t wal_size_ = 0;
  std::uint64_t write_pos_ = 0;
  std::uint64_t checkpoint_pos_ = 0;
  std::uint64_t pending_bytes_ = 0;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t wrap_count_ = 0;
  std::uint64_t checkpoint_count_ = 0;
  std::uint64_t sentinel_write_count_ = 0;
  std::uint64_t write_call_count_ = 0;
};

[[nodiscard]] WalRecordHeader DecodeWalRecordHeader(std::span<const std::byte> bytes);
[[nodiscard]] bool IsTerminalMarker(const std::filesystem::path& path,
                                    std::uint64_t wal_offset,
                                    std::uint64_t wal_size,
                                    std::uint64_t cursor);
[[nodiscard]] WalScanState ScanWalState(const std::filesystem::path& path,
                                        std::uint64_t wal_offset,
                                        std::uint64_t wal_size,
                                        std::uint64_t checkpoint_pos);
[[nodiscard]] WalPendingScanResult ScanPendingMutationsWithState(const std::filesystem::path& path,
                                                                 std::uint64_t wal_offset,
                                                                 std::uint64_t wal_size,
                                                                 std::uint64_t checkpoint_pos,
                                                                 std::uint64_t committed_seq);

}  // namespace waxcpp::core::wal
