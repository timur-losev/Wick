#include "wal_ring.hpp"

#include "mv2s_format.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace waxcpp::core::wal {
namespace {

std::runtime_error WalError(const std::string& message) {
  return std::runtime_error("wal ring error: " + message);
}

template <typename T>
T ReadLE(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_integral_v<T>, "ReadLE requires integral type");
  if (offset + sizeof(T) > bytes.size()) {
    throw WalError("read out of range");
  }
  using UnsignedT = std::make_unsigned_t<T>;
  UnsignedT out = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out |= static_cast<UnsignedT>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
  }
  return static_cast<T>(out);
}

std::vector<std::byte> ReadExactly(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  if (length == 0) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw WalError("failed to open file for read: " + path.string());
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw WalError("failed to seek for read");
  }
  std::vector<std::byte> out(length);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    throw WalError("short read");
  }
  return out;
}

std::array<std::byte, 32> EmptyPayloadChecksum() {
  static const std::array<std::byte, 32> checksum = [] {
    const std::vector<std::byte> empty{};
    return Sha256Digest(empty);
  }();
  return checksum;
}

void WriteLE64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) {
  if (offset + sizeof(std::uint64_t) > out.size()) {
    throw WalError("WriteLE64 out of range");
  }
  for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
    out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
  }
}

void WriteLE32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) {
  if (offset + sizeof(std::uint32_t) > out.size()) {
    throw WalError("WriteLE32 out of range");
  }
  for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
    out[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
  }
}

std::array<std::byte, kRecordHeaderSize> BuildWalRecordHeader(std::uint64_t sequence,
                                                              std::uint32_t length,
                                                              std::uint32_t flags,
                                                              const std::array<std::byte, 32>& checksum) {
  std::array<std::byte, kRecordHeaderSize> header{};
  WriteLE64(header, 0, sequence);
  WriteLE32(header, 8, length);
  WriteLE32(header, 12, flags);
  std::copy(checksum.begin(), checksum.end(), header.begin() + 16);
  return header;
}

std::vector<std::byte> BuildWalDataRecord(std::uint64_t sequence,
                                          std::uint32_t flags,
                                          std::span<const std::byte> payload) {
  if (payload.empty()) {
    throw WalError("wal payload must be non-empty");
  }
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw WalError("wal payload exceeds UInt32.max");
  }
  const auto checksum = Sha256Digest(payload);
  const auto header = BuildWalRecordHeader(sequence,
                                           static_cast<std::uint32_t>(payload.size()),
                                           flags,
                                           checksum);
  std::vector<std::byte> out{};
  out.reserve(static_cast<std::size_t>(kRecordHeaderSize) + payload.size());
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::array<std::byte, kRecordHeaderSize> BuildWalPaddingRecord(std::uint64_t sequence,
                                                               std::uint32_t skip_bytes) {
  return BuildWalRecordHeader(sequence, skip_bytes, kFlagIsPadding, EmptyPayloadChecksum());
}

std::uint64_t NextSequenceOrThrow(std::uint64_t current) {
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    throw WalError("sequence overflow");
  }
  return current + 1;
}

class PayloadCursor {
 public:
  explicit PayloadCursor(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t remaining() const {
    return bytes_.size() - cursor_;
  }

  [[nodiscard]] std::size_t position() const {
    return cursor_;
  }

  std::uint8_t ReadU8() {
    EnsureAvailable(1, "UInt8");
    return std::to_integer<std::uint8_t>(bytes_[cursor_++]);
  }

  std::uint32_t ReadU32() {
    return ReadIntegral<std::uint32_t>("UInt32");
  }

  std::uint64_t ReadU64() {
    return ReadIntegral<std::uint64_t>("UInt64");
  }

  std::int64_t ReadI64() {
    const auto raw = ReadU64();
    std::int64_t out = 0;
    static_assert(sizeof(out) == sizeof(raw));
    std::memcpy(&out, &raw, sizeof(out));
    return out;
  }

  float ReadF32() {
    const auto raw = ReadU32();
    float out = 0.0F;
    static_assert(sizeof(out) == sizeof(raw));
    std::memcpy(&out, &raw, sizeof(out));
    return out;
  }

  std::vector<std::byte> ReadFixed(std::size_t count, const char* context) {
    EnsureAvailable(count, context);
    std::vector<std::byte> out(count);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(cursor_), count, out.begin());
    cursor_ += count;
    return out;
  }

  void Skip(std::size_t count, const char* context) {
    EnsureAvailable(count, context);
    cursor_ += count;
  }

  void SkipBytesLen32(std::size_t max_bytes, const char* context) {
    const auto length = static_cast<std::size_t>(ReadU32());
    if (length > max_bytes) {
      throw WalError(std::string(context) + " exceeds limit");
    }
    Skip(length, context);
  }

  void SkipString(const char* context) {
    SkipBytesLen32(core::mv2s::kMaxStringBytes, context);
  }

  std::string ReadString(const char* context) {
    const auto length = static_cast<std::size_t>(ReadU32());
    if (length > core::mv2s::kMaxStringBytes) {
      throw WalError(std::string(context) + " exceeds limit");
    }
    EnsureAvailable(length, context);
    std::string result(reinterpret_cast<const char*>(bytes_.data() + cursor_), length);
    cursor_ += length;
    return result;
  }

  void SkipOptionalString(const char* field) {
    ReadOptional([&]() { SkipString(field); }, field);
  }

  void SkipOptionalU8(const char* field) {
    ReadOptional([&]() { (void)ReadU8(); }, field);
  }

  void SkipOptionalU32(const char* field) {
    ReadOptional([&]() { (void)ReadU32(); }, field);
  }

  void SkipOptionalU64(const char* field) {
    ReadOptional([&]() { (void)ReadU64(); }, field);
  }

  void SkipOptionalBytes(std::size_t max_bytes, const char* field) {
    ReadOptional([&]() { SkipBytesLen32(max_bytes, field); }, field);
  }

  template <typename Fn>
  void ReadOptional(Fn&& reader, const char* field) {
    const auto tag = ReadU8();
    switch (tag) {
      case 0:
        return;
      case 1:
        reader();
        return;
      default:
        throw WalError(std::string("invalid optional tag for ") + field);
    }
  }

  void Finalize() const {
    if (cursor_ != bytes_.size()) {
      throw WalError("excess bytes while decoding WAL entry");
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
      throw WalError(std::string("truncated buffer while reading ") + context);
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t cursor_ = 0;
};

void SkipStringArray(PayloadCursor& cursor, const char* field) {
  const auto count = static_cast<std::size_t>(cursor.ReadU32());
  if (count > core::mv2s::kMaxArrayCount) {
    throw WalError(std::string(field) + " count exceeds limit");
  }
  for (std::size_t i = 0; i < count; ++i) {
    cursor.SkipString(field);
  }
}

void SkipTagPairs(PayloadCursor& cursor) {
  const auto count = static_cast<std::size_t>(cursor.ReadU32());
  if (count > core::mv2s::kMaxArrayCount) {
    throw WalError("tags count exceeds limit");
  }
  for (std::size_t i = 0; i < count; ++i) {
    cursor.SkipString("tags.key");
    cursor.SkipString("tags.value");
  }
}

void SkipMetadata(PayloadCursor& cursor) {
  const auto count = static_cast<std::size_t>(cursor.ReadU32());
  if (count > core::mv2s::kMaxArrayCount) {
    throw WalError("metadata count exceeds limit");
  }
  for (std::size_t i = 0; i < count; ++i) {
    cursor.SkipString("metadata.key");
    cursor.SkipString("metadata.value");
  }
}

void SkipFrameMetaSubset(PayloadCursor& cursor) {
  cursor.SkipOptionalString("subset.uri");
  cursor.SkipOptionalString("subset.title");
  cursor.SkipOptionalString("subset.kind");
  cursor.SkipOptionalString("subset.track");
  SkipTagPairs(cursor);
  SkipStringArray(cursor, "subset.labels");
  SkipStringArray(cursor, "subset.content_dates");
  cursor.SkipOptionalU8("subset.role");
  cursor.SkipOptionalU64("subset.parent_id");
  cursor.SkipOptionalU32("subset.chunk_index");
  cursor.SkipOptionalU32("subset.chunk_count");
  cursor.SkipOptionalBytes(core::mv2s::kMaxBlobBytes, "subset.chunk_manifest");
  cursor.SkipOptionalU8("subset.status");
  cursor.SkipOptionalU64("subset.supersedes");
  cursor.SkipOptionalU64("subset.superseded_by");
  cursor.SkipOptionalString("subset.search_text");

  const auto metadata_tag = cursor.ReadU8();
  switch (metadata_tag) {
    case 0:
      break;
    case 1:
      SkipMetadata(cursor);
      break;
    default:
      throw WalError("invalid optional tag for subset.metadata");
  }
}

/// Read FrameMetaSubset fields into WalPutFrameInfo, preserving kind/metadata/tags/labels.
void ReadFrameMetaSubset(PayloadCursor& cursor, WalPutFrameInfo& put) {
  cursor.SkipOptionalString("subset.uri");
  cursor.SkipOptionalString("subset.title");

  // kind
  cursor.ReadOptional([&]() { put.kind = cursor.ReadString("subset.kind"); }, "subset.kind");

  cursor.SkipOptionalString("subset.track");

  // tags
  {
    const auto count = static_cast<std::size_t>(cursor.ReadU32());
    if (count > core::mv2s::kMaxArrayCount) {
      throw WalError("tags count exceeds limit");
    }
    put.tags.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      auto key = cursor.ReadString("tags.key");
      auto value = cursor.ReadString("tags.value");
      put.tags.emplace_back(std::move(key), std::move(value));
    }
  }

  // labels
  {
    const auto count = static_cast<std::size_t>(cursor.ReadU32());
    if (count > core::mv2s::kMaxArrayCount) {
      throw WalError("subset.labels count exceeds limit");
    }
    put.labels.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      put.labels.push_back(cursor.ReadString("subset.labels"));
    }
  }

  SkipStringArray(cursor, "subset.content_dates");
  cursor.SkipOptionalU8("subset.role");
  cursor.SkipOptionalU64("subset.parent_id");
  cursor.SkipOptionalU32("subset.chunk_index");
  cursor.SkipOptionalU32("subset.chunk_count");
  cursor.SkipOptionalBytes(core::mv2s::kMaxBlobBytes, "subset.chunk_manifest");
  cursor.SkipOptionalU8("subset.status");
  cursor.SkipOptionalU64("subset.supersedes");
  cursor.SkipOptionalU64("subset.superseded_by");
  cursor.SkipOptionalString("subset.search_text");

  // metadata
  const auto metadata_tag = cursor.ReadU8();
  switch (metadata_tag) {
    case 0:
      break;
    case 1: {
      const auto count = static_cast<std::size_t>(cursor.ReadU32());
      if (count > core::mv2s::kMaxArrayCount) {
        throw WalError("metadata count exceeds limit");
      }
      put.metadata.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        auto key = cursor.ReadString("metadata.key");
        auto value = cursor.ReadString("metadata.value");
        if (put.metadata.find(key) != put.metadata.end()) {
          throw WalError("duplicate metadata key");
        }
        put.metadata.emplace(std::move(key), std::move(value));
      }
      break;
    }
    default:
      throw WalError("invalid optional tag for subset.metadata");
  }
}

WalPendingMutationInfo DecodeWalMutationPayload(std::uint64_t sequence, std::span<const std::byte> payload) {
  PayloadCursor cursor(payload);
  const auto opcode = cursor.ReadU8();

  WalPendingMutationInfo mutation{};
  mutation.sequence = sequence;
  switch (opcode) {
    case 0x01: {  // putFrame
      mutation.kind = WalMutationKind::kPutFrame;
      WalPutFrameInfo put{};
      put.frame_id = cursor.ReadU64();
      put.timestamp_ms = cursor.ReadI64();
      ReadFrameMetaSubset(cursor, put);
      put.payload_offset = cursor.ReadU64();
      put.payload_length = cursor.ReadU64();
      put.canonical_encoding = cursor.ReadU8();
      if (put.canonical_encoding > 3) {
        throw WalError("invalid canonical encoding in WAL putFrame");
      }
      put.canonical_length = cursor.ReadU64();
      {
        const auto canonical_checksum = cursor.ReadFixed(32, "canonicalChecksum");
        std::copy(canonical_checksum.begin(), canonical_checksum.end(), put.canonical_checksum.begin());
      }
      {
        const auto stored_checksum = cursor.ReadFixed(32, "storedChecksum");
        std::copy(stored_checksum.begin(), stored_checksum.end(), put.stored_checksum.begin());
      }
      mutation.put_frame = put;
      break;
    }
    case 0x02: {  // deleteFrame
      mutation.kind = WalMutationKind::kDeleteFrame;
      WalDeleteFrameInfo del{};
      del.frame_id = cursor.ReadU64();
      mutation.delete_frame = del;
      break;
    }
    case 0x03: {  // supersedeFrame
      mutation.kind = WalMutationKind::kSupersedeFrame;
      WalSupersedeFrameInfo supersede{};
      supersede.superseded_id = cursor.ReadU64();
      supersede.superseding_id = cursor.ReadU64();
      mutation.supersede_frame = supersede;
      break;
    }
    case 0x04: {  // putEmbedding
      mutation.kind = WalMutationKind::kPutEmbedding;
      WalPutEmbeddingInfo put_embedding{};
      put_embedding.frame_id = cursor.ReadU64();
      const auto dimension = static_cast<std::size_t>(cursor.ReadU32());
      if (dimension > core::mv2s::kMaxArrayCount) {
        throw WalError("embedding dimension exceeds limit");
      }
      if (dimension > std::numeric_limits<std::size_t>::max() / 4) {
        throw WalError("embedding dimension overflows byte length");
      }
      put_embedding.dimension = static_cast<std::uint32_t>(dimension);
      put_embedding.vector.reserve(dimension);
      for (std::size_t i = 0; i < dimension; ++i) {
        put_embedding.vector.push_back(cursor.ReadF32());
      }
      mutation.put_embedding = put_embedding;
      break;
    }
    default:
      throw WalError("unknown WAL opcode");
  }

  cursor.Finalize();
  return mutation;
}

}  // namespace

WalRingWriter::WalRingWriter(std::filesystem::path path,
                             std::uint64_t wal_offset,
                             std::uint64_t wal_size,
                             std::uint64_t write_pos,
                             std::uint64_t checkpoint_pos,
                             std::uint64_t pending_bytes,
                             std::uint64_t last_sequence,
                             std::uint64_t wrap_count,
                             std::uint64_t checkpoint_count,
                             std::uint64_t sentinel_write_count,
                             std::uint64_t write_call_count)
    : path_(std::move(path)),
      wal_offset_(wal_offset),
      wal_size_(wal_size),
      pending_bytes_(pending_bytes),
      last_sequence_(last_sequence),
      wrap_count_(wrap_count),
      checkpoint_count_(checkpoint_count),
      sentinel_write_count_(sentinel_write_count),
      write_call_count_(write_call_count) {
  if (wal_size_ == 0) {
    write_pos_ = 0;
    checkpoint_pos_ = 0;
    return;
  }
  write_pos_ = write_pos % wal_size_;
  checkpoint_pos_ = checkpoint_pos % wal_size_;
}

bool WalRingWriter::CanAppend(std::size_t payload_size) const {
  if (payload_size == 0) {
    return false;
  }
  if (last_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  if (wal_size_ == 0) {
    return false;
  }
  if (payload_size > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  const auto header_size = kRecordHeaderSize;
  const auto entry_size = header_size + static_cast<std::uint64_t>(payload_size);
  if (entry_size > wal_size_) {
    return false;
  }

  std::uint64_t extra_padding = 0;
  std::uint64_t probe_write_pos = write_pos_;
  std::uint64_t remaining = wal_size_ - probe_write_pos;

  if (remaining < header_size) {
    extra_padding += remaining;
    probe_write_pos = 0;
    remaining = wal_size_;
  }
  if (remaining < entry_size) {
    extra_padding += remaining;
    probe_write_pos = 0;
    remaining = wal_size_;
  }

  const auto predicted_write_pos = probe_write_pos + entry_size;
  if (wal_size_ - predicted_write_pos < header_size) {
    extra_padding += wal_size_ - predicted_write_pos;
  }

  if (entry_size > std::numeric_limits<std::uint64_t>::max() - extra_padding) {
    return false;
  }
  const auto total_needed = entry_size + extra_padding;
  if (total_needed > wal_size_) {
    return false;
  }
  return pending_bytes_ <= wal_size_ - total_needed;
}

std::uint64_t WalRingWriter::Append(std::span<const std::byte> payload, std::uint32_t flags) {
  if (payload.empty()) {
    throw WalError("wal payload must be non-empty");
  }
  if (wal_size_ == 0) {
    throw WalError("wal_size is zero");
  }
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw WalError("wal payload exceeds UInt32.max");
  }
  if (last_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    throw WalError("sequence overflow");
  }

  const auto header_size = kRecordHeaderSize;
  const auto entry_size = header_size + static_cast<std::uint64_t>(payload.size());
  if (entry_size > wal_size_) {
    throw WalError("entry size exceeds wal_size");
  }

  std::uint64_t extra_padding = 0;
  std::uint64_t probe_write_pos = write_pos_;
  std::uint64_t remaining = wal_size_ - probe_write_pos;
  if (remaining < header_size) {
    extra_padding += remaining;
    probe_write_pos = 0;
    remaining = wal_size_;
  }
  if (remaining < entry_size) {
    extra_padding += remaining;
    probe_write_pos = 0;
    remaining = wal_size_;
  }
  const auto predicted_write_pos = probe_write_pos + entry_size;
  if (wal_size_ - predicted_write_pos < header_size) {
    extra_padding += wal_size_ - predicted_write_pos;
  }

  if (entry_size > std::numeric_limits<std::uint64_t>::max() - extra_padding) {
    throw WalError("entry size overflow");
  }
  const auto total_needed = entry_size + extra_padding;
  if (total_needed > wal_size_ || pending_bytes_ > wal_size_ - total_needed) {
    throw WalError("wal capacity exceeded");
  }

  remaining = wal_size_ - write_pos_;
  if (remaining < header_size) {
    if (remaining > 0) {
      std::vector<std::byte> zero_tail(static_cast<std::size_t>(remaining), std::byte{0});
      WriteAll(zero_tail, wal_offset_ + write_pos_);
      pending_bytes_ += remaining;
    }
    if (write_pos_ != 0) {
      wrap_count_ += 1;
    }
    write_pos_ = 0;
    remaining = wal_size_;
  }

  if (remaining < entry_size && remaining >= header_size) {
    const auto skip_bytes_64 = remaining - header_size;
    if (skip_bytes_64 > std::numeric_limits<std::uint32_t>::max()) {
      throw WalError("padding skip bytes exceeds UInt32.max");
    }
    const auto padding_sequence = NextSequenceOrThrow(last_sequence_);
    const auto padding_header = BuildWalPaddingRecord(padding_sequence,
                                                      static_cast<std::uint32_t>(skip_bytes_64));
    WriteAll(padding_header, wal_offset_ + write_pos_);
    last_sequence_ = padding_sequence;
    pending_bytes_ += remaining;
    if (write_pos_ != 0) {
      wrap_count_ += 1;
    }
    write_pos_ = 0;
  }

  const auto sequence = NextSequenceOrThrow(last_sequence_);
  const auto record_data = BuildWalDataRecord(sequence, flags, payload);
  const auto record_start = write_pos_;
  const auto record_end = record_start + entry_size;
  const bool can_inline_sentinel =
      record_end < wal_size_ &&
      (wal_size_ - record_end) >= header_size &&
      (pending_bytes_ + entry_size) < wal_size_;

  if (can_inline_sentinel) {
    std::vector<std::byte> combined{};
    combined.reserve(record_data.size() + static_cast<std::size_t>(header_size));
    combined.insert(combined.end(), record_data.begin(), record_data.end());
    combined.insert(combined.end(), static_cast<std::size_t>(header_size), std::byte{0});
    WriteAll(combined, wal_offset_ + record_start);
    sentinel_write_count_ += 1;
  } else {
    WriteAll(record_data, wal_offset_ + record_start);
  }

  last_sequence_ = sequence;
  pending_bytes_ += entry_size;
  write_pos_ = (record_end == wal_size_) ? 0 : record_end;

  if (!can_inline_sentinel) {
    WriteSentinel();
  }
  return sequence;
}

std::vector<std::uint64_t> WalRingWriter::AppendBatch(const std::vector<std::vector<std::byte>>& payloads,
                                                      std::uint32_t flags) {
  if (payloads.empty()) {
    return {};
  }

  auto sim_write_pos = write_pos_;
  auto sim_pending_bytes = pending_bytes_;
  auto sim_last_sequence = last_sequence_;

  auto simulate_append = [&](std::size_t payload_size) -> bool {
    if (payload_size == 0 || payload_size > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    const auto header_size = kRecordHeaderSize;
    const auto entry_size = header_size + static_cast<std::uint64_t>(payload_size);
    if (entry_size > wal_size_) {
      return false;
    }

    std::uint64_t extra_padding = 0;
    std::uint64_t probe_write_pos = sim_write_pos;
    std::uint64_t remaining = wal_size_ - probe_write_pos;
    if (remaining < header_size) {
      extra_padding += remaining;
      probe_write_pos = 0;
      remaining = wal_size_;
    }
    if (remaining < entry_size) {
      extra_padding += remaining;
      probe_write_pos = 0;
      remaining = wal_size_;
    }
    const auto predicted_write_pos = probe_write_pos + entry_size;
    if (wal_size_ - predicted_write_pos < header_size) {
      extra_padding += wal_size_ - predicted_write_pos;
    }
    if (entry_size > std::numeric_limits<std::uint64_t>::max() - extra_padding) {
      return false;
    }
    const auto total_needed = entry_size + extra_padding;
    if (total_needed > wal_size_ || sim_pending_bytes > wal_size_ - total_needed) {
      return false;
    }

    remaining = wal_size_ - sim_write_pos;
    if (remaining < header_size) {
      sim_pending_bytes += remaining;
      sim_write_pos = 0;
      remaining = wal_size_;
    }
    if (remaining < entry_size && remaining >= header_size) {
      sim_pending_bytes += remaining;
      sim_write_pos = 0;
    }

    const auto record_start = sim_write_pos;
    const auto record_end = record_start + entry_size;
    const bool can_inline_sentinel =
        record_end < wal_size_ &&
        (wal_size_ - record_end) >= header_size &&
        (sim_pending_bytes + entry_size) < wal_size_;
    sim_pending_bytes += entry_size;
    sim_write_pos = (record_end == wal_size_) ? 0 : record_end;

    if (!can_inline_sentinel) {
      auto sentinel_remaining = wal_size_ - sim_write_pos;
      if (sentinel_remaining < header_size) {
        sim_pending_bytes += sentinel_remaining;
        sim_write_pos = 0;
      }
      if (sim_pending_bytes >= wal_size_) {
        return false;
      }
    }

    if (sim_last_sequence == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    sim_last_sequence += 1;
    return true;
  };

  for (const auto& payload : payloads) {
    if (!simulate_append(payload.size())) {
      throw WalError("wal capacity exceeded");
    }
  }

  std::vector<std::uint64_t> sequences{};
  sequences.reserve(payloads.size());
  for (const auto& payload : payloads) {
    sequences.push_back(Append(payload, flags));
  }
  return sequences;
}

void WalRingWriter::RecordCheckpoint() {
  checkpoint_pos_ = write_pos_;
  pending_bytes_ = 0;
  checkpoint_count_ += 1;
}

void WalRingWriter::WriteSentinel() {
  const auto header_size = kRecordHeaderSize;
  if (wal_size_ < header_size) {
    return;
  }

  auto remaining = wal_size_ - write_pos_;
  if (remaining < header_size) {
    if (remaining > 0) {
      std::vector<std::byte> zero_tail(static_cast<std::size_t>(remaining), std::byte{0});
      WriteAll(zero_tail, wal_offset_ + write_pos_);
      pending_bytes_ += remaining;
    }
    if (write_pos_ != 0) {
      wrap_count_ += 1;
    }
    write_pos_ = 0;
    remaining = wal_size_;
  }
  (void)remaining;

  if (pending_bytes_ >= wal_size_) {
    return;
  }
  std::array<std::byte, kRecordHeaderSize> sentinel{};
  WriteAll(sentinel, wal_offset_ + write_pos_);
  sentinel_write_count_ += 1;
}

void WalRingWriter::WriteAll(std::span<const std::byte> data, std::uint64_t file_offset) {
  if (data.empty()) {
    return;
  }

  std::fstream out(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!out) {
    std::ofstream create(path_, std::ios::binary | std::ios::trunc);
    if (!create) {
      throw WalError("failed to create file for write: " + path_.string());
    }
    create.close();
    out.open(path_, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!out) {
    throw WalError("failed to open file for write: " + path_.string());
  }

  out.seekp(static_cast<std::streamoff>(file_offset), std::ios::beg);
  if (!out) {
    throw WalError("failed to seek file for write");
  }
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) {
    throw WalError("failed to write WAL bytes");
  }
  write_call_count_ += 1;
}

bool WalRecordHeader::IsSentinel() const {
  return sequence == 0 && length == 0 && flags == 0 &&
         std::all_of(checksum.begin(), checksum.end(), [](std::byte b) { return b == std::byte{0}; });
}

bool WalRecordHeader::IsPadding() const {
  return (flags & kFlagIsPadding) != 0;
}

WalRecordHeader DecodeWalRecordHeader(std::span<const std::byte> bytes) {
  if (bytes.size() != kRecordHeaderSize) {
    throw WalError("record header size mismatch");
  }
  WalRecordHeader header{};
  header.sequence = ReadLE<std::uint64_t>(bytes, 0);
  header.length = ReadLE<std::uint32_t>(bytes, 8);
  header.flags = ReadLE<std::uint32_t>(bytes, 12);
  std::copy_n(bytes.begin() + 16, header.checksum.size(), header.checksum.begin());
  return header;
}

bool IsTerminalMarker(const std::filesystem::path& path,
                      std::uint64_t wal_offset,
                      std::uint64_t wal_size,
                      std::uint64_t cursor) {
  if (wal_size == 0) {
    return true;
  }
  const auto normalized = cursor % wal_size;
  const auto remaining = wal_size - normalized;
  if (remaining < kRecordHeaderSize) {
    return false;
  }
  try {
    const auto header_bytes = ReadExactly(path, wal_offset + normalized, static_cast<std::size_t>(kRecordHeaderSize));
    const auto header = DecodeWalRecordHeader(header_bytes);
    return header.IsSentinel() || header.sequence == 0;
  } catch (...) {
    return false;
  }
}

WalScanState ScanWalState(const std::filesystem::path& path,
                          std::uint64_t wal_offset,
                          std::uint64_t wal_size,
                          std::uint64_t checkpoint_pos) {
  return ScanPendingMutationsWithState(path,
                                       wal_offset,
                                       wal_size,
                                       checkpoint_pos,
                                       std::numeric_limits<std::uint64_t>::max())
      .state;
}

WalPendingScanResult ScanPendingMutationsWithState(const std::filesystem::path& path,
                                                   std::uint64_t wal_offset,
                                                   std::uint64_t wal_size,
                                                   std::uint64_t checkpoint_pos,
                                                   std::uint64_t committed_seq) {
  if (wal_size == 0) {
    return WalPendingScanResult{};
  }
  if (wal_size < kRecordHeaderSize) {
    throw WalError("wal_size smaller than record header");
  }

  const auto start = checkpoint_pos % wal_size;
  auto cursor = start;
  std::uint64_t last_sequence = 0;
  std::uint64_t pending_bytes = 0;
  bool wrapped = false;
  bool stop_decoding_pending = false;
  std::vector<WalPendingMutationInfo> pending_mutations;

  while (true) {
    const auto remaining = wal_size - cursor;
    if (remaining < kRecordHeaderSize) {
      if (wrapped) {
        break;
      }
      pending_bytes += remaining;
      cursor = 0;
      wrapped = true;
      if (cursor == start) {
        break;
      }
      continue;
    }

    const auto header_bytes = ReadExactly(path, wal_offset + cursor, static_cast<std::size_t>(kRecordHeaderSize));
    WalRecordHeader header{};
    try {
      header = DecodeWalRecordHeader(header_bytes);
    } catch (...) {
      break;
    }

    if (header.IsSentinel() || header.sequence == 0) {
      break;
    }
    if (last_sequence != 0 && header.sequence <= last_sequence) {
      break;
    }

    if (header.IsPadding()) {
      const auto expected = EmptyPayloadChecksum();
      if (!std::equal(expected.begin(), expected.end(), header.checksum.begin())) {
        break;
      }
      const auto skip_bytes = static_cast<std::uint64_t>(header.length);
      if (cursor > std::numeric_limits<std::uint64_t>::max() - (kRecordHeaderSize + skip_bytes)) {
        break;
      }
      const auto advance = kRecordHeaderSize + skip_bytes;
      if (cursor + advance > wal_size) {
        break;
      }
      cursor = (cursor + advance) % wal_size;
      pending_bytes += advance;
      last_sequence = header.sequence;
      if (cursor == 0) {
        wrapped = true;
      }
      if (cursor == start) {
        break;
      }
      continue;
    }

    const auto payload_len = static_cast<std::uint64_t>(header.length);
    if (payload_len == 0) {
      break;
    }

    const auto max_payload = wal_size >= kRecordHeaderSize ? wal_size - kRecordHeaderSize : 0;
    if (payload_len > max_payload) {
      break;
    }
    if (payload_len > remaining - kRecordHeaderSize) {
      break;
    }
    if (payload_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      break;
    }

    const auto payload = ReadExactly(path, wal_offset + cursor + kRecordHeaderSize, static_cast<std::size_t>(payload_len));
    const auto computed = Sha256Digest(payload);
    if (!std::equal(computed.begin(), computed.end(), header.checksum.begin())) {
      break;
    }

    if (!stop_decoding_pending && header.sequence > committed_seq) {
      try {
        pending_mutations.push_back(DecodeWalMutationPayload(header.sequence, payload));
      } catch (...) {
        // Preserve Swift open-path behavior: continue state scan even if entry decode fails.
        stop_decoding_pending = true;
      }
    }

    const auto advance = kRecordHeaderSize + payload_len;
    cursor += advance;
    if (cursor == wal_size) {
      cursor = 0;
      wrapped = true;
    }
    pending_bytes += advance;
    last_sequence = header.sequence;
    if (cursor == start) {
      break;
    }
  }

  WalPendingScanResult result{};
  result.pending_mutations = std::move(pending_mutations);
  result.state.last_sequence = last_sequence;
  result.state.write_pos = cursor;
  result.state.pending_bytes = pending_bytes;
  return result;
}

}  // namespace waxcpp::core::wal
