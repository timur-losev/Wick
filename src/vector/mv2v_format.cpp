#include "waxcpp/mv2v_format.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace waxcpp {
namespace {

constexpr std::size_t kHeaderSize = 36;
constexpr std::array<std::byte, 4> kMagic = {
    std::byte{0x4D},  // 'M'
    std::byte{0x56},  // 'V'
    std::byte{0x32},  // '2'
    std::byte{0x56},  // 'V'
};
constexpr std::uint16_t kVersion = 1;

std::runtime_error VecError(const std::string& message) {
  return std::runtime_error("mv2v_format: " + message);
}

std::uint16_t ReadU16LE(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + sizeof(std::uint16_t) > bytes.size()) {
    throw VecError("u16 read out of bounds");
  }
  std::uint16_t out = 0;
  for (std::size_t i = 0; i < sizeof(out); ++i) {
    out |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
  }
  return out;
}

std::uint32_t ReadU32LE(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + sizeof(std::uint32_t) > bytes.size()) {
    throw VecError("u32 read out of bounds");
  }
  std::uint32_t out = 0;
  for (std::size_t i = 0; i < sizeof(out); ++i) {
    out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
  }
  return out;
}

std::uint64_t ReadU64LE(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset + sizeof(std::uint64_t) > bytes.size()) {
    throw VecError("u64 read out of bounds");
  }
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < sizeof(out); ++i) {
    out |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
  }
  return out;
}

void AppendU16LE(std::vector<std::byte>& out, std::uint16_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendU32LE(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendU64LE(std::vector<std::byte>& out, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

bool TryMul(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) {
  if (lhs == 0 || rhs == 0) {
    out = 0;
    return true;
  }
  if (lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

bool TryAdd(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& out) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

struct VecHeaderV1 {
  VecEncoding encoding = VecEncoding::kUSearch;
  VecSimilarity similarity = VecSimilarity::kCosine;
  std::uint32_t dimension = 0;
  std::uint64_t vector_count = 0;
  std::uint64_t payload_length = 0;
};

void ValidateHeader(std::span<const std::byte> bytes, VecHeaderV1& out) {
  if (bytes.size() < kHeaderSize) {
    throw VecError("segment too small");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    throw VecError("magic mismatch");
  }

  const auto version = ReadU16LE(bytes, 4);
  if (version != kVersion) {
    throw VecError("unsupported version");
  }

  const auto encoding_raw = std::to_integer<std::uint8_t>(bytes[6]);
  if (encoding_raw != static_cast<std::uint8_t>(VecEncoding::kUSearch) &&
      encoding_raw != static_cast<std::uint8_t>(VecEncoding::kMetal)) {
    throw VecError("unsupported encoding");
  }
  out.encoding = static_cast<VecEncoding>(encoding_raw);

  const auto similarity_raw = std::to_integer<std::uint8_t>(bytes[7]);
  if (similarity_raw > static_cast<std::uint8_t>(VecSimilarity::kL2)) {
    throw VecError("invalid similarity");
  }
  out.similarity = static_cast<VecSimilarity>(similarity_raw);
  out.dimension = ReadU32LE(bytes, 8);
  out.vector_count = ReadU64LE(bytes, 12);
  out.payload_length = ReadU64LE(bytes, 20);

  for (std::size_t i = 0; i < 8; ++i) {
    if (std::to_integer<std::uint8_t>(bytes[28 + i]) != 0U) {
      throw VecError("reserved bytes must be zero");
    }
  }
}

std::vector<std::byte> EncodeHeader(const VecSegmentInfo& info, VecEncoding encoding) {
  std::vector<std::byte> out{};
  out.reserve(kHeaderSize);
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  AppendU16LE(out, kVersion);
  out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(encoding)));
  out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(info.similarity)));
  AppendU32LE(out, info.dimension);
  AppendU64LE(out, info.vector_count);
  AppendU64LE(out, info.payload_length);
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(std::byte{0});
  }
  return out;
}

}  // namespace

VecEncoding DetectVecEncoding(std::span<const std::byte> bytes) {
  VecHeaderV1 header{};
  ValidateHeader(bytes, header);
  return header.encoding;
}

std::vector<std::byte> EncodeUSearchVecSegment(const VecSegmentInfo& info, std::span<const std::byte> payload) {
  if (info.payload_length != payload.size()) {
    throw VecError("payload_length mismatch for usearch encoding");
  }
  auto out = EncodeHeader(info, VecEncoding::kUSearch);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::byte> EncodeMetalVecSegment(const VecSegmentInfo& info,
                                             std::span<const float> vectors,
                                             std::span<const std::uint64_t> frame_ids) {
  if (frame_ids.size() != info.vector_count) {
    throw VecError("frame_ids count mismatch for metal encoding");
  }
  std::uint64_t expected_vector_values = 0;
  if (!TryMul(info.vector_count, static_cast<std::uint64_t>(info.dimension), expected_vector_values)) {
    throw VecError("vector size overflow");
  }
  if (vectors.size() != expected_vector_values) {
    throw VecError("vector value count mismatch for metal encoding");
  }
  std::uint64_t expected_vector_bytes = 0;
  if (!TryMul(expected_vector_values, sizeof(float), expected_vector_bytes)) {
    throw VecError("vector byte size overflow");
  }
  if (info.payload_length != expected_vector_bytes) {
    throw VecError("payload_length mismatch for metal encoding");
  }

  auto out = EncodeHeader(info, VecEncoding::kMetal);
  out.reserve(out.size() + static_cast<std::size_t>(expected_vector_bytes) +
              sizeof(std::uint64_t) + frame_ids.size() * sizeof(std::uint64_t));

  for (const float value : vectors) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    AppendU32LE(out, bits);
  }

  std::uint64_t frame_bytes = 0;
  if (!TryMul(static_cast<std::uint64_t>(frame_ids.size()), sizeof(std::uint64_t), frame_bytes)) {
    throw VecError("frame id byte size overflow");
  }
  AppendU64LE(out, frame_bytes);
  for (const auto frame_id : frame_ids) {
    AppendU64LE(out, frame_id);
  }
  return out;
}

VecUSearchPayload DecodeUSearchVecPayload(std::span<const std::byte> bytes) {
  auto decoded = DecodeVecSegment(bytes);
  if (!std::holds_alternative<VecUSearchPayload>(decoded)) {
    throw VecError("segment encoding is not usearch");
  }
  return std::get<VecUSearchPayload>(std::move(decoded));
}

DecodedVecSegment DecodeVecSegment(std::span<const std::byte> bytes) {
  VecHeaderV1 header{};
  ValidateHeader(bytes, header);

  VecSegmentInfo info{};
  info.similarity = header.similarity;
  info.dimension = header.dimension;
  info.vector_count = header.vector_count;
  info.payload_length = header.payload_length;

  if (header.encoding == VecEncoding::kUSearch) {
    std::uint64_t expected_total = 0;
    if (!TryAdd(kHeaderSize, header.payload_length, expected_total)) {
      throw VecError("segment size overflow");
    }
    if (expected_total != bytes.size()) {
      throw VecError("segment length mismatch");
    }
    VecUSearchPayload out{};
    out.info = info;
    out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), bytes.end());
    return out;
  }

  std::uint64_t expected_vector_values = 0;
  if (!TryMul(header.vector_count, static_cast<std::uint64_t>(header.dimension), expected_vector_values)) {
    throw VecError("vector size overflow");
  }
  std::uint64_t expected_vector_bytes = 0;
  if (!TryMul(expected_vector_values, sizeof(float), expected_vector_bytes)) {
    throw VecError("vector byte size overflow");
  }
  if (header.payload_length != expected_vector_bytes) {
    throw VecError("vector payload length mismatch");
  }

  std::uint64_t cursor = kHeaderSize;
  std::uint64_t min_total = 0;
  if (!TryAdd(cursor, header.payload_length, min_total) || !TryAdd(min_total, sizeof(std::uint64_t), min_total)) {
    throw VecError("segment size overflow");
  }
  if (bytes.size() < min_total) {
    throw VecError("segment too small for metal payload");
  }
  cursor += header.payload_length;

  const auto frame_length = ReadU64LE(bytes, static_cast<std::size_t>(cursor));
  cursor += sizeof(std::uint64_t);

  std::uint64_t expected_frame_length = 0;
  if (!TryMul(header.vector_count, sizeof(std::uint64_t), expected_frame_length)) {
    throw VecError("frame id length overflow");
  }
  if (frame_length != expected_frame_length) {
    throw VecError("frame id payload length mismatch");
  }

  std::uint64_t expected_total = 0;
  if (!TryAdd(cursor, frame_length, expected_total)) {
    throw VecError("segment size overflow");
  }
  if (bytes.size() != expected_total) {
    throw VecError("segment length mismatch");
  }

  VecMetalPayload out{};
  out.info = info;
  out.vectors.reserve(static_cast<std::size_t>(expected_vector_values));
  for (std::uint64_t i = 0; i < expected_vector_values; ++i) {
    const auto bits = ReadU32LE(bytes, kHeaderSize + static_cast<std::size_t>(i * sizeof(std::uint32_t)));
    float value = 0.0F;
    std::uint32_t raw = bits;
    std::memcpy(&value, &raw, sizeof(value));
    out.vectors.push_back(value);
  }

  out.frame_ids.reserve(static_cast<std::size_t>(header.vector_count));
  for (std::uint64_t i = 0; i < header.vector_count; ++i) {
    const auto offset = static_cast<std::size_t>(cursor + i * sizeof(std::uint64_t));
    out.frame_ids.push_back(ReadU64LE(bytes, offset));
  }
  return out;
}

}  // namespace waxcpp

