#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace waxcpp::core::mv2s {

// "MV2S" in ASCII
inline constexpr std::array<std::byte, 4> kMagic = {
  std::byte{0x4D},  // 'M'
  std::byte{0x56},  // 'V'
  std::byte{0x32},  // '2'
  std::byte{0x53},  // 'S'
};

inline constexpr std::array<std::byte, 8> kFooterMagic = {
  std::byte{0x4D},  // 'M'
  std::byte{0x56},  // 'V'
  std::byte{0x32},  // '2'
  std::byte{0x53},  // 'S'
  std::byte{0x46},  // 'F'
  std::byte{0x4F},  // 'O'
  std::byte{0x4F},  // 'O'
  std::byte{0x54},  // 'T'
};

inline constexpr std::array<std::byte, 8> kReplaySnapshotMagic = {
  std::byte{0x57},  // 'W'
  std::byte{0x41},  // 'A'
  std::byte{0x4C},  // 'L'
  std::byte{0x53},  // 'S'
  std::byte{0x4E},  // 'N'
  std::byte{0x41},  // 'A'
  std::byte{0x50},  // 'P'
  std::byte{0x31},  // '1'
};

inline constexpr std::uint8_t kSpecMajor = 1;
inline constexpr std::uint8_t kSpecMinor = 0;
inline constexpr std::uint16_t kSpecVersion = (static_cast<std::uint16_t>(kSpecMajor) << 8U) | kSpecMinor;

inline constexpr std::uint64_t kHeaderPageSize = 4096;
inline constexpr std::uint64_t kHeaderRegionSize = 8192;
inline constexpr std::uint64_t kFooterSize = 64;
inline constexpr std::uint64_t kWalRecordHeaderSize = 48;
inline constexpr std::uint64_t kWalOffset = kHeaderRegionSize; // Header region is reserved for two header pages, so WAL starts after that.
inline constexpr std::uint64_t kDefaultWalSize = 256ULL * 1024ULL * 1024ULL; // 256 MiB for WAL by default, which should be enough for most workloads to avoid forcing frequent checkpoints.
inline constexpr std::uint64_t kMaxTocBytes = 512ULL * 1024ULL * 1024ULL; // 512 MiB max TOC size — supports ~1.4M frames with UE5-sized metadata (~370 B/frame).  Previous 64 MiB limit was hit at ~172K frames during large codebase indexing.
inline constexpr std::uint64_t kMaxFooterScanBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxStringBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxBlobBytes = 256U * 1024U * 1024U;
inline constexpr std::size_t kMaxArrayCount = 10'000'000U;

inline constexpr std::size_t kHeaderChecksumOffset = 104;
inline constexpr std::size_t kHeaderChecksumCount = 32;
inline constexpr std::size_t kTocChecksumOffset = 72;

struct ReplaySnapshot {
  std::uint64_t file_generation = 0;
  std::uint64_t wal_committed_seq = 0;
  std::uint64_t footer_offset = 0;
  std::uint64_t wal_write_pos = 0;
  std::uint64_t wal_checkpoint_pos = 0;
  std::uint64_t wal_pending_bytes = 0;
  std::uint64_t wal_last_sequence = 0;
};

struct HeaderPage {
  std::uint16_t format_version = kSpecVersion;
  std::uint8_t spec_major = kSpecMajor;
  std::uint8_t spec_minor = kSpecMinor;
  std::uint64_t header_page_generation = 0;
  std::uint64_t file_generation = 0;
  std::uint64_t footer_offset = 0;
  std::uint64_t wal_offset = kWalOffset;
  std::uint64_t wal_size = kDefaultWalSize;
  std::uint64_t wal_write_pos = 0;
  std::uint64_t wal_checkpoint_pos = 0;
  std::uint64_t wal_committed_seq = 0;
  std::array<std::byte, 32> toc_checksum{};
  std::array<std::byte, 32> header_checksum{};
  std::optional<ReplaySnapshot> replay_snapshot;
};

struct Footer {
  std::uint64_t toc_len = 0;
  std::array<std::byte, 32> toc_hash{};
  std::uint64_t generation = 0;
  std::uint64_t wal_committed_seq = 0;
};

struct FooterSlice {
  std::uint64_t footer_offset = 0;
  std::uint64_t toc_offset = 0;
  Footer footer{};
  std::vector<std::byte> toc_bytes;
};

struct FrameSummary {
  std::uint64_t id = 0;
  std::int64_t timestamp_ms = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_length = 0;
  std::array<std::byte, 32> payload_checksum{};
  std::uint8_t canonical_encoding = 0;
  std::optional<std::uint64_t> canonical_length;
  std::optional<std::array<std::byte, 32>> stored_checksum;
  std::optional<std::string> kind;
  std::unordered_map<std::string, std::string> metadata;
  std::vector<std::pair<std::string, std::string>> tags;
  std::vector<std::string> labels;
  std::uint8_t status = 0;
  std::optional<std::uint64_t> supersedes;
  std::optional<std::uint64_t> superseded_by;
};

struct SegmentSummary {
  std::uint64_t id = 0;
  std::uint64_t bytes_offset = 0;
  std::uint64_t bytes_length = 0;
  std::array<std::byte, 32> checksum{};
  std::uint8_t compression = 0;
  std::uint8_t kind = 0;
};

struct IndexManifestSummary {
  std::uint64_t bytes_offset = 0;
  std::uint64_t bytes_length = 0;
  std::array<std::byte, 32> checksum{};
};

struct TocSummary {
  std::uint64_t toc_version = 0;
  std::uint64_t frame_count = 0;
  std::array<std::byte, 32> toc_checksum{};
  std::vector<FrameSummary> frames;
  std::vector<SegmentSummary> segments;
  std::optional<IndexManifestSummary> lex_index;
  std::optional<IndexManifestSummary> vec_index;
  std::optional<IndexManifestSummary> time_index;
};

[[nodiscard]] std::array<std::byte, 32> ComputeHeaderChecksum(std::span<const std::byte> page_bytes);
[[nodiscard]] std::array<std::byte, 32> ComputeTocChecksum(std::span<const std::byte> toc_bytes);
[[nodiscard]] bool TocHashMatches(std::span<const std::byte> toc_bytes, std::span<const std::byte> expected_hash);

[[nodiscard]] HeaderPage DecodeHeaderPage(std::span<const std::byte> page_bytes);
[[nodiscard]] std::array<std::byte, kHeaderPageSize> EncodeHeaderPage(const HeaderPage& page);

[[nodiscard]] Footer DecodeFooter(std::span<const std::byte> footer_bytes);
[[nodiscard]] std::array<std::byte, kFooterSize> EncodeFooter(const Footer& footer);
[[nodiscard]] std::vector<std::byte> EncodeTocV1(std::span<const FrameSummary> frames);
[[nodiscard]] std::vector<std::byte> EncodeTocV1(std::span<const FrameSummary> frames,
                                                 std::span<const SegmentSummary> segments);
[[nodiscard]] std::vector<std::byte> EncodeEmptyTocV1();
[[nodiscard]] TocSummary DecodeToc(std::span<const std::byte> toc_bytes);

}  // namespace waxcpp::core::mv2s
