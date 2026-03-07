#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace waxcpp {

enum class VecSimilarity : std::uint8_t {
  kCosine = 0,
  kDot = 1,
  kL2 = 2,
};

enum class VecEncoding : std::uint8_t {
  kUSearch = 1,
  kMetal = 2,
};

struct VecSegmentInfo {
  VecSimilarity similarity = VecSimilarity::kCosine;
  std::uint32_t dimension = 0;
  std::uint64_t vector_count = 0;
  std::uint64_t payload_length = 0;
};

struct VecUSearchPayload {
  VecSegmentInfo info{};
  std::vector<std::byte> payload{};
};

struct VecMetalPayload {
  VecSegmentInfo info{};
  std::vector<float> vectors{};
  std::vector<std::uint64_t> frame_ids{};
};

using DecodedVecSegment = std::variant<VecUSearchPayload, VecMetalPayload>;

[[nodiscard]] VecEncoding DetectVecEncoding(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> EncodeUSearchVecSegment(const VecSegmentInfo& info,
                                                             std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> EncodeMetalVecSegment(const VecSegmentInfo& info,
                                                           std::span<const float> vectors,
                                                           std::span<const std::uint64_t> frame_ids);
[[nodiscard]] VecUSearchPayload DecodeUSearchVecPayload(std::span<const std::byte> bytes);
[[nodiscard]] DecodedVecSegment DecodeVecSegment(std::span<const std::byte> bytes);

}  // namespace waxcpp

