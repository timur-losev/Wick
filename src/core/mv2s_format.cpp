#include "mv2s_format.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <type_traits>
#include <vector>

namespace waxcpp::core::mv2s {
namespace {

std::runtime_error FormatError(const std::string& message) {
  return std::runtime_error("mv2s format error: " + message);
}

template <typename T>
T ReadLE(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_integral_v<T>, "ReadLE requires integral type");
  if (offset + sizeof(T) > bytes.size()) {
    throw FormatError("read out of range");
  }
  using UnsignedT = std::make_unsigned_t<T>;
  UnsignedT out = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out |= static_cast<UnsignedT>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
  }
  return static_cast<T>(out);
}

template <typename T>
void WriteLE(std::span<std::byte> bytes, std::size_t offset, T value) {
  static_assert(std::is_integral_v<T>, "WriteLE requires integral type");
  if (offset + sizeof(T) > bytes.size()) {
    throw FormatError("write out of range");
  }
  using UnsignedT = std::make_unsigned_t<T>;
  UnsignedT v = static_cast<UnsignedT>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    bytes[offset + i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
  }
}

bool ByteRangeEquals(std::span<const std::byte> bytes, std::size_t offset, std::span<const std::byte> expected) {
  if (offset + expected.size() > bytes.size()) {
    return false;
  }
  return std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

class BinaryCursor {
 public:
  explicit BinaryCursor(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t remaining() const { return bytes_.size() - cursor_; }

  std::uint8_t ReadU8() {
    EnsureAvailable(1, "UInt8");
    return std::to_integer<std::uint8_t>(bytes_[cursor_++]);
  }

  std::uint16_t ReadU16() {
    return ReadIntegral<std::uint16_t>("UInt16");
  }

  std::uint32_t ReadU32() {
    return ReadIntegral<std::uint32_t>("UInt32");
  }

  std::uint64_t ReadU64() {
    return ReadIntegral<std::uint64_t>("UInt64");
  }

  std::int64_t ReadI64() {
    const auto value = ReadU64();
    std::int64_t out = 0;
    static_assert(sizeof(out) == sizeof(value));
    std::memcpy(&out, &value, sizeof(out));
    return out;
  }

  std::vector<std::byte> ReadFixed(std::size_t count, const char* context) {
    EnsureAvailable(count, context);
    std::vector<std::byte> out(count);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_), count, out.begin());
    cursor_ += count;
    return out;
  }

  std::vector<std::byte> ReadBytesLen32(std::size_t max_bytes, const char* context) {
    const auto length = static_cast<std::size_t>(ReadU32());
    if (length > max_bytes) {
      throw FormatError(std::string(context) + " exceeds limit");
    }
    return ReadFixed(length, context);
  }

  std::string ReadString(const char* context) {
    const auto bytes = ReadBytesLen32(kMaxStringBytes, context);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  void Finalize() {
    if (cursor_ != bytes_.size()) {
      throw FormatError("excess bytes while decoding TOC");
    }
  }

 private:
  template <typename T>
  T ReadIntegral(const char* context) {
    static_assert(std::is_integral_v<T>, "ReadIntegral requires integral type");
    EnsureAvailable(sizeof(T), context);
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT out = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      out |= static_cast<UnsignedT>(std::to_integer<std::uint8_t>(bytes_[cursor_ + i])) << (8U * i);
    }
    cursor_ += sizeof(T);
    return static_cast<T>(out);
  }

  void EnsureAvailable(std::size_t count, const char* context) {
    if (count > bytes_.size() || cursor_ > bytes_.size() - count) {
      throw FormatError(std::string("truncated buffer while reading ") + context);
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t cursor_ = 0;
};

template <typename ReaderFn>
void ReadOptional(BinaryCursor& cursor, ReaderFn&& reader, const char* field) {
  const auto tag = cursor.ReadU8();
  switch (tag) {
    case 0:
      return;
    case 1:
      reader();
      return;
    default:
      throw FormatError(std::string("invalid optional tag for ") + field);
  }
}

class BinaryBuilder {
 public:
  void AppendU8(std::uint8_t value) {
    bytes_.push_back(static_cast<std::byte>(value));
  }

  void AppendU16(std::uint16_t value) {
    AppendIntegral(value);
  }

  void AppendU32(std::uint32_t value) {
    AppendIntegral(value);
  }

  void AppendU64(std::uint64_t value) {
    AppendIntegral(value);
  }

  void AppendFixed(std::span<const std::byte> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  void AppendString(std::string_view value) {
    AppendU32(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(),
                  reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data()) + value.size());
  }

  [[nodiscard]] std::vector<std::byte> Build() && {
    return std::move(bytes_);
  }

 private:
  template <typename T>
  void AppendIntegral(T value) {
    static_assert(std::is_integral_v<T>, "AppendIntegral requires integral type");
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT v = static_cast<UnsignedT>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
    }
  }

  std::vector<std::byte> bytes_{};
};

bool SegmentMatchesManifest(const SegmentSummary& segment,
                            std::uint8_t expected_kind,
                            const IndexManifestSummary& manifest) {
  return segment.kind == expected_kind &&
         segment.bytes_offset == manifest.bytes_offset &&
         segment.bytes_length == manifest.bytes_length &&
         std::equal(segment.checksum.begin(), segment.checksum.end(), manifest.checksum.begin());
}

void ValidateManifestSegmentLinkage(const TocSummary& summary) {
  if (summary.lex_index.has_value()) {
    const auto found = std::any_of(summary.segments.begin(), summary.segments.end(), [&](const auto& segment) {
      return SegmentMatchesManifest(segment, 0, *summary.lex_index);  // lex
    });
    if (!found) {
      throw FormatError("lex index manifest missing matching segment catalog entry");
    }
  }

  if (summary.vec_index.has_value()) {
    const auto found = std::any_of(summary.segments.begin(), summary.segments.end(), [&](const auto& segment) {
      return SegmentMatchesManifest(segment, 1, *summary.vec_index);  // vec
    });
    if (!found) {
      throw FormatError("vec index manifest missing matching segment catalog entry");
    }
  }

  if (summary.time_index.has_value()) {
    const auto found = std::any_of(summary.segments.begin(), summary.segments.end(), [&](const auto& segment) {
      return SegmentMatchesManifest(segment, 2, *summary.time_index);  // time
    });
    if (!found) {
      throw FormatError("time index manifest missing matching segment catalog entry");
    }
  }
}

}  // namespace

std::array<std::byte, 32> ComputeHeaderChecksum(std::span<const std::byte> page_bytes) {
  if (page_bytes.size() != kHeaderPageSize) {
    throw FormatError("header page must be 4096 bytes");
  }

  Sha256 hasher;
  hasher.Update(page_bytes.subspan(0, kHeaderChecksumOffset));
  std::array<std::byte, kHeaderChecksumCount> zeros{};
  hasher.Update(zeros);
  hasher.Update(page_bytes.subspan(kHeaderChecksumOffset + kHeaderChecksumCount));
  return hasher.Finalize();
}

std::array<std::byte, 32> ComputeTocChecksum(std::span<const std::byte> toc_bytes) {
  if (toc_bytes.size() < 32) {
    throw FormatError("TOC bytes must be at least 32 bytes");
  }
  const auto body = toc_bytes.subspan(0, toc_bytes.size() - 32);
  Sha256 hasher;
  hasher.Update(body);
  std::array<std::byte, 32> zeros{};
  hasher.Update(zeros);
  return hasher.Finalize();
}

bool TocHashMatches(std::span<const std::byte> toc_bytes, std::span<const std::byte> expected_hash) {
  if (expected_hash.size() != 32 || toc_bytes.size() < 32) {
    return false;
  }
  const auto computed = ComputeTocChecksum(toc_bytes);
  const auto stored = toc_bytes.subspan(toc_bytes.size() - 32, 32);
  return std::equal(computed.begin(), computed.end(), stored.begin()) &&
         std::equal(computed.begin(), computed.end(), expected_hash.begin());
}

HeaderPage DecodeHeaderPage(std::span<const std::byte> page_bytes) {
  if (page_bytes.size() != kHeaderPageSize) {
    throw FormatError("header page must be 4096 bytes");
  }
  if (!ByteRangeEquals(page_bytes, 0, kMagic)) {
    throw FormatError("header magic mismatch");
  }

  HeaderPage page;
  page.format_version = ReadLE<std::uint16_t>(page_bytes, 4);
  page.spec_major = std::to_integer<std::uint8_t>(page_bytes[6]);
  page.spec_minor = std::to_integer<std::uint8_t>(page_bytes[7]);

  if (page.format_version != kSpecVersion) {
    throw FormatError("unsupported format version");
  }
  const auto unpacked_major = static_cast<std::uint8_t>(page.format_version >> 8U);
  const auto unpacked_minor = static_cast<std::uint8_t>(page.format_version & 0xFFU);
  if (page.spec_major != unpacked_major || page.spec_minor != unpacked_minor) {
    throw FormatError("spec version mismatch");
  }

  page.header_page_generation = ReadLE<std::uint64_t>(page_bytes, 8);
  page.file_generation = ReadLE<std::uint64_t>(page_bytes, 16);
  page.footer_offset = ReadLE<std::uint64_t>(page_bytes, 24);
  page.wal_offset = ReadLE<std::uint64_t>(page_bytes, 32);
  page.wal_size = ReadLE<std::uint64_t>(page_bytes, 40);
  page.wal_write_pos = ReadLE<std::uint64_t>(page_bytes, 48);
  page.wal_checkpoint_pos = ReadLE<std::uint64_t>(page_bytes, 56);
  page.wal_committed_seq = ReadLE<std::uint64_t>(page_bytes, 64);

  std::copy_n(page_bytes.begin() + static_cast<std::ptrdiff_t>(kTocChecksumOffset), 32, page.toc_checksum.begin());
  std::copy_n(page_bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderChecksumOffset), 32, page.header_checksum.begin());

  const auto computed_checksum = ComputeHeaderChecksum(page_bytes);
  if (!std::equal(computed_checksum.begin(), computed_checksum.end(), page.header_checksum.begin())) {
    throw FormatError("header checksum mismatch");
  }

  const std::size_t kReplayMagicOffset = 136;
  const std::size_t kReplayGenerationOffset = 144;
  const std::size_t kReplayCommittedSeqOffset = 152;
  const std::size_t kReplayFooterOffsetOffset = 160;
  const std::size_t kReplayWritePosOffset = 168;
  const std::size_t kReplayCheckpointPosOffset = 176;
  const std::size_t kReplayPendingBytesOffset = 184;
  const std::size_t kReplayLastSeqOffset = 192;
  const std::size_t kReplayFlagsOffset = 200;
  const std::uint64_t kReplayValidFlag = 0x1;

  if (ByteRangeEquals(page_bytes, kReplayMagicOffset, kReplaySnapshotMagic) &&
      (ReadLE<std::uint64_t>(page_bytes, kReplayFlagsOffset) & kReplayValidFlag) != 0) {
    ReplaySnapshot snapshot;
    snapshot.file_generation = ReadLE<std::uint64_t>(page_bytes, kReplayGenerationOffset);
    snapshot.wal_committed_seq = ReadLE<std::uint64_t>(page_bytes, kReplayCommittedSeqOffset);
    snapshot.footer_offset = ReadLE<std::uint64_t>(page_bytes, kReplayFooterOffsetOffset);
    snapshot.wal_write_pos = ReadLE<std::uint64_t>(page_bytes, kReplayWritePosOffset);
    snapshot.wal_checkpoint_pos = ReadLE<std::uint64_t>(page_bytes, kReplayCheckpointPosOffset);
    snapshot.wal_pending_bytes = ReadLE<std::uint64_t>(page_bytes, kReplayPendingBytesOffset);
    snapshot.wal_last_sequence = ReadLE<std::uint64_t>(page_bytes, kReplayLastSeqOffset);
    page.replay_snapshot = snapshot;
  }

  if (page.wal_offset < kHeaderRegionSize) {
    throw FormatError("wal_offset is below header region");
  }
  if (page.wal_size < kWalRecordHeaderSize) {
    throw FormatError("wal_size too small");
  }
  if (page.wal_write_pos > page.wal_size || page.wal_checkpoint_pos > page.wal_size) {
    throw FormatError("wal cursor exceeds wal_size");
  }
  if (page.footer_offset < page.wal_offset + page.wal_size) {
    throw FormatError("footer_offset precedes data region");
  }
  if (page.replay_snapshot.has_value()) {
    const auto& snapshot = *page.replay_snapshot;
    if (snapshot.wal_write_pos > page.wal_size ||
        snapshot.wal_checkpoint_pos > page.wal_size ||
        snapshot.wal_pending_bytes > page.wal_size) {
      throw FormatError("replay snapshot WAL values exceed wal_size");
    }
    if (snapshot.footer_offset < page.wal_offset + page.wal_size) {
      throw FormatError("replay snapshot footer offset precedes data region");
    }
  }

  return page;
}

std::array<std::byte, kHeaderPageSize> EncodeHeaderPage(const HeaderPage& page) {
  std::array<std::byte, kHeaderPageSize> bytes{};
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());

  WriteLE<std::uint16_t>(bytes, 4, page.format_version);
  bytes[6] = static_cast<std::byte>(page.spec_major);
  bytes[7] = static_cast<std::byte>(page.spec_minor);
  WriteLE<std::uint64_t>(bytes, 8, page.header_page_generation);
  WriteLE<std::uint64_t>(bytes, 16, page.file_generation);
  WriteLE<std::uint64_t>(bytes, 24, page.footer_offset);
  WriteLE<std::uint64_t>(bytes, 32, page.wal_offset);
  WriteLE<std::uint64_t>(bytes, 40, page.wal_size);
  WriteLE<std::uint64_t>(bytes, 48, page.wal_write_pos);
  WriteLE<std::uint64_t>(bytes, 56, page.wal_checkpoint_pos);
  WriteLE<std::uint64_t>(bytes, 64, page.wal_committed_seq);
  std::copy(page.toc_checksum.begin(), page.toc_checksum.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(kTocChecksumOffset));

  if (page.replay_snapshot.has_value()) {
    const std::size_t kReplayMagicOffset = 136;
    const std::size_t kReplayGenerationOffset = 144;
    const std::size_t kReplayCommittedSeqOffset = 152;
    const std::size_t kReplayFooterOffsetOffset = 160;
    const std::size_t kReplayWritePosOffset = 168;
    const std::size_t kReplayCheckpointPosOffset = 176;
    const std::size_t kReplayPendingBytesOffset = 184;
    const std::size_t kReplayLastSeqOffset = 192;
    const std::size_t kReplayFlagsOffset = 200;
    const std::uint64_t kReplayValidFlag = 0x1;

    const auto& snapshot = *page.replay_snapshot;
    std::copy(kReplaySnapshotMagic.begin(), kReplaySnapshotMagic.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(kReplayMagicOffset));
    WriteLE<std::uint64_t>(bytes, kReplayGenerationOffset, snapshot.file_generation);
    WriteLE<std::uint64_t>(bytes, kReplayCommittedSeqOffset, snapshot.wal_committed_seq);
    WriteLE<std::uint64_t>(bytes, kReplayFooterOffsetOffset, snapshot.footer_offset);
    WriteLE<std::uint64_t>(bytes, kReplayWritePosOffset, snapshot.wal_write_pos);
    WriteLE<std::uint64_t>(bytes, kReplayCheckpointPosOffset, snapshot.wal_checkpoint_pos);
    WriteLE<std::uint64_t>(bytes, kReplayPendingBytesOffset, snapshot.wal_pending_bytes);
    WriteLE<std::uint64_t>(bytes, kReplayLastSeqOffset, snapshot.wal_last_sequence);
    WriteLE<std::uint64_t>(bytes, kReplayFlagsOffset, kReplayValidFlag);
  }

  const auto checksum = ComputeHeaderChecksum(bytes);
  std::copy(checksum.begin(), checksum.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderChecksumOffset));

  return bytes;
}

Footer DecodeFooter(std::span<const std::byte> footer_bytes) {
  if (footer_bytes.size() != kFooterSize) {
    throw FormatError("footer size mismatch");
  }
  if (!ByteRangeEquals(footer_bytes, 0, kFooterMagic)) {
    throw FormatError("footer magic mismatch");
  }

  Footer footer;
  footer.toc_len = ReadLE<std::uint64_t>(footer_bytes, 8);
  std::copy_n(footer_bytes.begin() + 16, 32, footer.toc_hash.begin());
  footer.generation = ReadLE<std::uint64_t>(footer_bytes, 48);
  footer.wal_committed_seq = ReadLE<std::uint64_t>(footer_bytes, 56);
  return footer;
}

std::array<std::byte, kFooterSize> EncodeFooter(const Footer& footer) {
  std::array<std::byte, kFooterSize> bytes{};
  std::copy(kFooterMagic.begin(), kFooterMagic.end(), bytes.begin());
  WriteLE<std::uint64_t>(bytes, 8, footer.toc_len);
  std::copy(footer.toc_hash.begin(), footer.toc_hash.end(), bytes.begin() + 16);
  WriteLE<std::uint64_t>(bytes, 48, footer.generation);
  WriteLE<std::uint64_t>(bytes, 56, footer.wal_committed_seq);
  return bytes;
}

std::vector<std::byte> EncodeTocV1(std::span<const FrameSummary> frames,
                                   std::span<const SegmentSummary> segments) {
  BinaryBuilder builder;
  builder.AppendU64(1);  // toc_version
  if (frames.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw FormatError("too many frames for TOC v1");
  }
  builder.AppendU32(static_cast<std::uint32_t>(frames.size()));  // frames count

  for (const auto& frame : frames) {
    builder.AppendU64(frame.id);
    builder.AppendU64(static_cast<std::uint64_t>(frame.timestamp_ms));

    builder.AppendU8(0);  // anchor_ts absent

    // kind
    if (frame.kind.has_value() && !frame.kind->empty()) {
      builder.AppendU8(1);
      builder.AppendString(*frame.kind);
    } else {
      builder.AppendU8(0);
    }

    builder.AppendU8(0);  // track absent

    builder.AppendU64(frame.payload_offset);
    builder.AppendU64(frame.payload_length);
    builder.AppendFixed(frame.payload_checksum);

    builder.AppendU8(0);  // uri absent
    builder.AppendU8(0);  // title absent

    if (frame.canonical_encoding > 3) {
      throw FormatError("invalid canonical encoding in frame summary");
    }
    builder.AppendU8(frame.canonical_encoding);

    if (frame.canonical_length.has_value()) {
      builder.AppendU8(1);
      builder.AppendU64(*frame.canonical_length);
    } else {
      if (frame.canonical_encoding != 0) {
        throw FormatError("missing canonical_length for compressed frame summary");
      }
      builder.AppendU8(0);
    }

    if (frame.stored_checksum.has_value()) {
      builder.AppendU8(1);  // stored_checksum present
      builder.AppendFixed(*frame.stored_checksum);
    } else if (frame.payload_length > 0) {
      if (frame.canonical_encoding != 0) {
        throw FormatError("missing stored_checksum for compressed frame summary");
      }
      builder.AppendU8(1);  // plain payload: canonical checksum equals stored checksum
      builder.AppendFixed(frame.payload_checksum);
    } else {
      builder.AppendU8(0);  // stored_checksum absent
    }

    // metadata
    if (!frame.metadata.empty()) {
      builder.AppendU8(1);
      if (frame.metadata.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw FormatError("too many metadata entries");
      }
      builder.AppendU32(static_cast<std::uint32_t>(frame.metadata.size()));
      std::vector<const decltype(frame.metadata)::value_type*> sorted_metadata_entries{};
      sorted_metadata_entries.reserve(frame.metadata.size());
      for (const auto& entry : frame.metadata) {
        sorted_metadata_entries.push_back(&entry);
      }
      std::sort(sorted_metadata_entries.begin(),
                sorted_metadata_entries.end(),
                [](const decltype(frame.metadata)::value_type* lhs,
                   const decltype(frame.metadata)::value_type* rhs) {
                  if (lhs->first != rhs->first) {
                    return lhs->first < rhs->first;
                  }
                  return lhs->second < rhs->second;
                });
      for (const auto* entry : sorted_metadata_entries) {
        const auto& [key, value] = *entry;
        builder.AppendString(key);
        builder.AppendString(value);
      }
    } else {
      builder.AppendU8(0);
    }

    builder.AppendU8(0);   // search_text absent

    // tags
    if (frame.tags.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw FormatError("too many tags");
    }
    builder.AppendU32(static_cast<std::uint32_t>(frame.tags.size()));
    for (const auto& [key, value] : frame.tags) {
      builder.AppendString(key);
      builder.AppendString(value);
    }

    // labels
    if (frame.labels.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw FormatError("too many labels");
    }
    builder.AppendU32(static_cast<std::uint32_t>(frame.labels.size()));
    for (const auto& label : frame.labels) {
      builder.AppendString(label);
    }

    builder.AppendU32(0);  // content_dates count

    builder.AppendU8(0);  // role
    builder.AppendU8(0);  // parent_id absent
    builder.AppendU8(0);  // chunk_index absent
    builder.AppendU8(0);  // chunk_count absent
    builder.AppendU8(0);  // chunk_manifest absent

    if (frame.status > 1) {
      throw FormatError("invalid frame status in frame summary");
    }
    builder.AppendU8(frame.status);

    if (frame.supersedes.has_value()) {
      builder.AppendU8(1);
      builder.AppendU64(*frame.supersedes);
    } else {
      builder.AppendU8(0);
    }

    if (frame.superseded_by.has_value()) {
      builder.AppendU8(1);
      builder.AppendU64(*frame.superseded_by);
    } else {
      builder.AppendU8(0);
    }
  }

  builder.AppendU8(0);  // indexes.lex optional absent
  builder.AppendU8(0);  // indexes.vec optional absent
  builder.AppendU8(0);  // clip manifest absent in v1

  builder.AppendU8(0);  // time index optional absent
  builder.AppendU8(0);  // memories_track absent
  builder.AppendU8(0);  // logic_mesh absent
  builder.AppendU8(0);  // sketch_track absent

  if (segments.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw FormatError("too many segments for TOC v1");
  }
  builder.AppendU32(static_cast<std::uint32_t>(segments.size()));
  std::optional<std::uint64_t> previous_offset;
  std::optional<std::uint64_t> previous_end;
  for (const auto& segment : segments) {
    if (segment.compression > 3 || segment.kind > 3) {
      throw FormatError("invalid segment enum value in segment summary");
    }
    if (segment.bytes_offset > std::numeric_limits<std::uint64_t>::max() - segment.bytes_length) {
      throw FormatError("segment range overflow in segment summary");
    }
    const auto end = segment.bytes_offset + segment.bytes_length;
    if (previous_offset.has_value() && previous_end.has_value()) {
      if (segment.bytes_offset <= *previous_offset) {
        throw FormatError("segment offsets are not strictly increasing in segment summary");
      }
      if (*previous_end > segment.bytes_offset) {
        throw FormatError("segment ranges overlap in segment summary");
      }
    }
    previous_offset = segment.bytes_offset;
    previous_end = end;

    builder.AppendU64(segment.id);
    builder.AppendU64(segment.bytes_offset);
    builder.AppendU64(segment.bytes_length);
    builder.AppendFixed(segment.checksum);
    builder.AppendU8(segment.compression);
    builder.AppendU8(segment.kind);
  }

  builder.AppendString("");  // ticket issuer
  builder.AppendU64(0);      // ticket seq_no
  builder.AppendU64(0);      // ticket expires_in_secs
  builder.AppendU64(0);      // ticket capacity_bytes
  builder.AppendU8(0);       // ticket verified

  builder.AppendU8(0);  // memory_binding absent
  builder.AppendU8(0);  // replay_manifest absent
  builder.AppendU8(0);  // enrichment_queue absent

  std::array<std::byte, 32> merkle_root{};
  builder.AppendFixed(merkle_root);

  std::array<std::byte, 32> toc_checksum_placeholder{};
  builder.AppendFixed(toc_checksum_placeholder);

  auto toc = std::move(builder).Build();
  const auto checksum = ComputeTocChecksum(toc);
  std::copy(checksum.begin(), checksum.end(), toc.end() - 32);
  return toc;
}

std::vector<std::byte> EncodeEmptyTocV1() {
  return EncodeTocV1({}, {});
}

std::vector<std::byte> EncodeTocV1(std::span<const FrameSummary> frames) {
  return EncodeTocV1(frames, {});
}

TocSummary DecodeToc(std::span<const std::byte> toc_bytes) {
  if (toc_bytes.size() < 32) {
    throw FormatError("TOC must be at least 32 bytes");
  }
  if (toc_bytes.size() > kMaxTocBytes) {
    throw FormatError("TOC exceeds max size");
  }

  const auto computed_checksum = ComputeTocChecksum(toc_bytes);
  const auto stored_checksum = toc_bytes.subspan(toc_bytes.size() - 32, 32);
  if (!std::equal(computed_checksum.begin(), computed_checksum.end(), stored_checksum.begin())) {
    throw FormatError("TOC checksum mismatch");
  }

  BinaryCursor cursor(toc_bytes);
  TocSummary summary;
  summary.toc_version = cursor.ReadU64();
  if (summary.toc_version != 1) {
    throw FormatError("unsupported TOC version");
  }

  const auto frame_count = static_cast<std::size_t>(cursor.ReadU32());
  if (frame_count > kMaxArrayCount) {
    throw FormatError("frame count exceeds limit");
  }
  summary.frame_count = frame_count;
  summary.frames.reserve(frame_count);

  for (std::size_t index = 0; index < frame_count; ++index) {
    FrameSummary frame;
    frame.id = cursor.ReadU64();
    frame.timestamp_ms = cursor.ReadI64();

    ReadOptional(cursor, [&]() { (void)cursor.ReadI64(); }, "anchor_ts");
    ReadOptional(cursor, [&]() { frame.kind = cursor.ReadString("kind"); }, "kind");
    ReadOptional(cursor, [&]() { (void)cursor.ReadString("track"); }, "track");

    frame.payload_offset = cursor.ReadU64();
    frame.payload_length = cursor.ReadU64();
    const auto checksum = cursor.ReadFixed(32, "checksum");
    std::copy(checksum.begin(), checksum.end(), frame.payload_checksum.begin());

    ReadOptional(cursor, [&]() { (void)cursor.ReadString("uri"); }, "uri");
    ReadOptional(cursor, [&]() { (void)cursor.ReadString("title"); }, "title");

    frame.canonical_encoding = cursor.ReadU8();
    if (frame.canonical_encoding > 3) {
      throw FormatError("invalid canonical encoding");
    }

    bool has_canonical_length = false;
    ReadOptional(cursor,
                 [&]() {
                   frame.canonical_length = cursor.ReadU64();
                   has_canonical_length = true;
                 },
                 "canonical_length");

    bool has_stored_checksum = false;
    ReadOptional(cursor,
                 [&]() {
                   std::array<std::byte, 32> stored{};
                   const auto stored_bytes = cursor.ReadFixed(32, "stored_checksum");
                   std::copy(stored_bytes.begin(), stored_bytes.end(), stored.begin());
                   frame.stored_checksum = stored;
                   has_stored_checksum = true;
                 },
                 "stored_checksum");

    ReadOptional(cursor,
                 [&]() {
                   const auto metadata_count = static_cast<std::size_t>(cursor.ReadU32());
                   if (metadata_count > kMaxArrayCount) {
                     throw FormatError("metadata count exceeds limit");
                   }
                   frame.metadata.reserve(metadata_count);
                   for (std::size_t i = 0; i < metadata_count; ++i) {
                     auto key = cursor.ReadString("metadata.key");
                     auto value = cursor.ReadString("metadata.value");
                     if (frame.metadata.find(key) != frame.metadata.end()) {
                       throw FormatError("duplicate metadata key");
                     }
                     frame.metadata.emplace(std::move(key), std::move(value));
                   }
                 },
                 "metadata");

    ReadOptional(cursor, [&]() { (void)cursor.ReadString("search_text"); }, "search_text");

    const auto tags_count = static_cast<std::size_t>(cursor.ReadU32());
    if (tags_count > kMaxArrayCount) {
      throw FormatError("tags count exceeds limit");
    }
    frame.tags.reserve(tags_count);
    for (std::size_t i = 0; i < tags_count; ++i) {
      auto key = cursor.ReadString("tag.key");
      auto value = cursor.ReadString("tag.value");
      frame.tags.emplace_back(std::move(key), std::move(value));
    }

    const auto labels_count = static_cast<std::size_t>(cursor.ReadU32());
    if (labels_count > kMaxArrayCount) {
      throw FormatError("labels count exceeds limit");
    }
    frame.labels.reserve(labels_count);
    for (std::size_t i = 0; i < labels_count; ++i) {
      frame.labels.push_back(cursor.ReadString("label"));
    }

    const auto content_dates_count = static_cast<std::size_t>(cursor.ReadU32());
    if (content_dates_count > kMaxArrayCount) {
      throw FormatError("content_dates count exceeds limit");
    }
    for (std::size_t i = 0; i < content_dates_count; ++i) {
      (void)cursor.ReadString("content_date");
    }

    const auto role = cursor.ReadU8();
    if (role > 3) {
      throw FormatError("invalid frame role");
    }

    ReadOptional(cursor, [&]() { (void)cursor.ReadU64(); }, "parent_id");
    ReadOptional(cursor, [&]() { (void)cursor.ReadU32(); }, "chunk_index");
    ReadOptional(cursor, [&]() { (void)cursor.ReadU32(); }, "chunk_count");
    ReadOptional(cursor, [&]() { (void)cursor.ReadBytesLen32(kMaxBlobBytes, "chunk_manifest"); }, "chunk_manifest");

    frame.status = cursor.ReadU8();
    if (frame.status > 1) {
      throw FormatError("invalid frame status");
    }
    ReadOptional(cursor, [&]() { frame.supersedes = cursor.ReadU64(); }, "supersedes");
    ReadOptional(cursor, [&]() { frame.superseded_by = cursor.ReadU64(); }, "superseded_by");

    if (frame.canonical_encoding != 0 && !has_canonical_length) {
      throw FormatError("missing canonical_length for compressed frame");
    }
    if (frame.payload_length > 0 && !has_stored_checksum) {
      throw FormatError("missing stored_checksum for non-empty payload");
    }
    if (frame.id != static_cast<std::uint64_t>(index)) {
      throw FormatError("frame ids are not dense");
    }
    summary.frames.push_back(frame);
  }

  // Index manifests
  ReadOptional(cursor,
               [&]() {
                 (void)cursor.ReadU64();  // docCount
                 IndexManifestSummary manifest{};
                 manifest.bytes_offset = cursor.ReadU64();
                 manifest.bytes_length = cursor.ReadU64();
                 const auto checksum = cursor.ReadFixed(32, "lex checksum");
                 std::copy(checksum.begin(), checksum.end(), manifest.checksum.begin());
                 (void)cursor.ReadU32();  // version
                 summary.lex_index = manifest;
               },
               "index.lex");

  ReadOptional(cursor,
               [&]() {
                 (void)cursor.ReadU64();  // vectorCount
                 (void)cursor.ReadU32();  // dimension
                 IndexManifestSummary manifest{};
                 manifest.bytes_offset = cursor.ReadU64();
                 manifest.bytes_length = cursor.ReadU64();
                 const auto checksum = cursor.ReadFixed(32, "vec checksum");
                 std::copy(checksum.begin(), checksum.end(), manifest.checksum.begin());
                 const auto similarity = cursor.ReadU8();
                 if (similarity > 2) {
                   throw FormatError("invalid vec similarity");
                 }
                 summary.vec_index = manifest;
               },
               "index.vec");

  if (cursor.ReadU8() != 0) {
    throw FormatError("clip manifest not supported in v1");
  }

  ReadOptional(cursor,
               [&]() {
                 IndexManifestSummary manifest{};
                 manifest.bytes_offset = cursor.ReadU64();
                 manifest.bytes_length = cursor.ReadU64();
                 (void)cursor.ReadU64();  // entryCount
                 const auto checksum = cursor.ReadFixed(32, "time index checksum");
                 std::copy(checksum.begin(), checksum.end(), manifest.checksum.begin());
                 summary.time_index = manifest;
               },
               "time_index");

  if (cursor.ReadU8() != 0 || cursor.ReadU8() != 0 || cursor.ReadU8() != 0) {
    throw FormatError("unsupported v1 extension track tag");
  }

  // Segment catalog
  const auto segment_count = static_cast<std::size_t>(cursor.ReadU32());
  if (segment_count > kMaxArrayCount) {
    throw FormatError("segment count exceeds limit");
  }
  std::optional<std::uint64_t> prev_offset;
  std::optional<std::uint64_t> prev_end;
  for (std::size_t i = 0; i < segment_count; ++i) {
    SegmentSummary segment{};
    segment.id = cursor.ReadU64();
    segment.bytes_offset = cursor.ReadU64();
    segment.bytes_length = cursor.ReadU64();
    const auto checksum = cursor.ReadFixed(32, "segment checksum");
    std::copy(checksum.begin(), checksum.end(), segment.checksum.begin());
    segment.compression = cursor.ReadU8();
    segment.kind = cursor.ReadU8();
    const auto compression = segment.compression;
    const auto kind = segment.kind;
    if (compression > 3 || kind > 3) {
      throw FormatError("invalid segment enum value");
    }
    if (segment.bytes_offset > std::numeric_limits<std::uint64_t>::max() - segment.bytes_length) {
      throw FormatError("segment range overflow");
    }
    const auto end = segment.bytes_offset + segment.bytes_length;
    if (prev_offset.has_value() && prev_end.has_value()) {
      if (segment.bytes_offset <= *prev_offset) {
        throw FormatError("segment offsets are not strictly increasing");
      }
      if (*prev_end > segment.bytes_offset) {
        throw FormatError("segment ranges overlap");
      }
    }
    prev_offset = segment.bytes_offset;
    prev_end = end;
    summary.segments.push_back(segment);
  }
  ValidateManifestSegmentLinkage(summary);

  // TicketRef
  (void)cursor.ReadString("ticket.issuer");
  (void)cursor.ReadU64();  // seqNo
  (void)cursor.ReadU64();  // expiresInSecs
  (void)cursor.ReadU64();  // capacityBytes
  if (cursor.ReadU8() > 1) {
    throw FormatError("ticket verified must be 0 or 1");
  }

  if (cursor.ReadU8() != 0 || cursor.ReadU8() != 0 || cursor.ReadU8() != 0) {
    throw FormatError("unsupported v1 optional manifest tag");
  }

  (void)cursor.ReadFixed(32, "merkle_root");
  const auto toc_checksum_bytes = cursor.ReadFixed(32, "toc_checksum");
  if (!std::equal(toc_checksum_bytes.begin(), toc_checksum_bytes.end(), stored_checksum.begin())) {
    throw FormatError("toc_checksum field mismatch");
  }
  cursor.Finalize();

  std::copy(stored_checksum.begin(), stored_checksum.end(), summary.toc_checksum.begin());
  return summary;
}

}  // namespace waxcpp::core::mv2s
