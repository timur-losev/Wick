#include "waxcpp/mv2v_format.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ExpectThrows(const std::string& name, const auto& fn) {
  try {
    fn();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error("expected throw: " + name);
}

void WriteU64LE(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  if (offset + sizeof(std::uint64_t) > bytes.size()) {
    throw std::runtime_error("WriteU64LE out of range");
  }
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
  }
}

bool FloatEq(float lhs, float rhs, float eps = 1e-6F) {
  return std::fabs(lhs - rhs) <= eps;
}

std::uint64_t NextRandom(std::uint64_t& state) {
  // Deterministic xorshift64* for reproducible fuzzing.
  state ^= state >> 12U;
  state ^= state << 25U;
  state ^= state >> 27U;
  return state * 2685821657736338717ULL;
}

void ScenarioUSearchEncodeDecodeRoundtrip() {
  waxcpp::tests::Log("scenario: usearch vec segment roundtrip");
  const std::vector<std::byte> payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
  waxcpp::VecSegmentInfo info{};
  info.similarity = waxcpp::VecSimilarity::kCosine;
  info.dimension = 384;
  info.vector_count = 4;
  info.payload_length = payload.size();

  const auto bytes = waxcpp::EncodeUSearchVecSegment(info, payload);
  Require(waxcpp::DetectVecEncoding(bytes) == waxcpp::VecEncoding::kUSearch, "detect encoding mismatch");

  const auto usearch = waxcpp::DecodeUSearchVecPayload(bytes);
  Require(usearch.info.dimension == info.dimension, "decoded dimension mismatch");
  Require(usearch.info.vector_count == info.vector_count, "decoded vector_count mismatch");
  Require(usearch.info.payload_length == info.payload_length, "decoded payload_length mismatch");
  Require(usearch.info.similarity == info.similarity, "decoded similarity mismatch");
  Require(usearch.payload == payload, "decoded payload mismatch");

  const auto any = waxcpp::DecodeVecSegment(bytes);
  Require(std::holds_alternative<waxcpp::VecUSearchPayload>(any), "decoded variant type mismatch");
}

void ScenarioMetalEncodeDecodeRoundtrip() {
  waxcpp::tests::Log("scenario: metal vec segment roundtrip");
  waxcpp::VecSegmentInfo info{};
  info.similarity = waxcpp::VecSimilarity::kDot;
  info.dimension = 2;
  info.vector_count = 3;
  info.payload_length = 3 * 2 * sizeof(float);

  const std::vector<float> vectors = {0.1F, 0.2F, 0.3F, 0.4F, -0.5F, -0.6F};
  const std::vector<std::uint64_t> frame_ids = {7, 42, 99};

  const auto bytes = waxcpp::EncodeMetalVecSegment(info, vectors, frame_ids);
  Require(waxcpp::DetectVecEncoding(bytes) == waxcpp::VecEncoding::kMetal, "detect encoding mismatch");

  const auto any = waxcpp::DecodeVecSegment(bytes);
  Require(std::holds_alternative<waxcpp::VecMetalPayload>(any), "decoded variant type mismatch");
  const auto& decoded = std::get<waxcpp::VecMetalPayload>(any);
  Require(decoded.info.similarity == info.similarity, "decoded similarity mismatch");
  Require(decoded.info.dimension == info.dimension, "decoded dimension mismatch");
  Require(decoded.info.vector_count == info.vector_count, "decoded vector_count mismatch");
  Require(decoded.info.payload_length == info.payload_length, "decoded payload_length mismatch");
  Require(decoded.frame_ids == frame_ids, "decoded frame ids mismatch");
  Require(decoded.vectors.size() == vectors.size(), "decoded vector size mismatch");
  for (std::size_t i = 0; i < vectors.size(); ++i) {
    Require(FloatEq(decoded.vectors[i], vectors[i]), "decoded vector value mismatch");
  }
}

void ScenarioHeaderValidationFailures() {
  waxcpp::tests::Log("scenario: header validation failures");
  waxcpp::VecSegmentInfo info{};
  info.similarity = waxcpp::VecSimilarity::kCosine;
  info.dimension = 4;
  info.vector_count = 1;
  info.payload_length = 1;
  const std::vector<std::byte> one_byte_payload = {std::byte{0xAB}};
  auto bytes = waxcpp::EncodeUSearchVecSegment(info, one_byte_payload);

  auto bad_magic = bytes;
  bad_magic[0] = std::byte{0x00};
  ExpectThrows("magic mismatch", [&]() { (void)waxcpp::DetectVecEncoding(bad_magic); });

  auto bad_version = bytes;
  bad_version[4] = std::byte{0x02};
  ExpectThrows("version mismatch", [&]() { (void)waxcpp::DetectVecEncoding(bad_version); });

  auto bad_encoding = bytes;
  bad_encoding[6] = std::byte{0x03};
  ExpectThrows("encoding mismatch", [&]() { (void)waxcpp::DetectVecEncoding(bad_encoding); });

  auto bad_similarity = bytes;
  bad_similarity[7] = std::byte{0x7F};
  ExpectThrows("similarity mismatch", [&]() { (void)waxcpp::DetectVecEncoding(bad_similarity); });

  auto bad_reserved = bytes;
  bad_reserved[28] = std::byte{0x01};
  ExpectThrows("reserved mismatch", [&]() { (void)waxcpp::DetectVecEncoding(bad_reserved); });
}

void ScenarioLengthValidationFailures() {
  waxcpp::tests::Log("scenario: length validation failures");
  waxcpp::VecSegmentInfo usearch_info{};
  usearch_info.similarity = waxcpp::VecSimilarity::kCosine;
  usearch_info.dimension = 4;
  usearch_info.vector_count = 2;
  usearch_info.payload_length = 2;
  const std::vector<std::byte> usearch_payload = {std::byte{0x11}, std::byte{0x22}};
  auto usearch_bytes = waxcpp::EncodeUSearchVecSegment(usearch_info, usearch_payload);

  usearch_bytes.pop_back();
  ExpectThrows("usearch length mismatch", [&]() { (void)waxcpp::DecodeVecSegment(usearch_bytes); });

  waxcpp::VecSegmentInfo metal_info{};
  metal_info.similarity = waxcpp::VecSimilarity::kL2;
  metal_info.dimension = 2;
  metal_info.vector_count = 2;
  metal_info.payload_length = 2 * 2 * sizeof(float);
  const std::vector<float> metal_vectors = {1.0F, 2.0F, 3.0F, 4.0F};
  const std::vector<std::uint64_t> metal_frame_ids = {1, 2};
  auto metal_bytes = waxcpp::EncodeMetalVecSegment(metal_info, metal_vectors, metal_frame_ids);

  // Corrupt frame-id section length to force decode rejection.
  constexpr std::size_t kHeaderSize = 36;
  const std::size_t frame_len_offset = kHeaderSize + static_cast<std::size_t>(metal_info.payload_length);
  WriteU64LE(metal_bytes, frame_len_offset, 999);
  ExpectThrows("metal frame length mismatch", [&]() { (void)waxcpp::DecodeVecSegment(metal_bytes); });
}

void ScenarioEncodeValidationFailures() {
  waxcpp::tests::Log("scenario: encode validation failures");
  waxcpp::VecSegmentInfo usearch_info{};
  usearch_info.similarity = waxcpp::VecSimilarity::kCosine;
  usearch_info.dimension = 8;
  usearch_info.vector_count = 1;
  usearch_info.payload_length = 4;
  ExpectThrows("usearch payload mismatch", [&]() {
    const std::vector<std::byte> bad_payload = {std::byte{0x01}};
    (void)waxcpp::EncodeUSearchVecSegment(usearch_info, bad_payload);
  });

  waxcpp::VecSegmentInfo metal_info{};
  metal_info.similarity = waxcpp::VecSimilarity::kDot;
  metal_info.dimension = 3;
  metal_info.vector_count = 2;
  metal_info.payload_length = 2 * 3 * sizeof(float);
  ExpectThrows("metal frame count mismatch", [&]() {
    const std::vector<float> bad_vectors = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
    const std::vector<std::uint64_t> bad_ids = {1};
    (void)waxcpp::EncodeMetalVecSegment(metal_info, bad_vectors, bad_ids);
  });
}

void ScenarioDeterministicDecodeFuzz() {
  waxcpp::tests::Log("scenario: deterministic decode fuzz");

  waxcpp::VecSegmentInfo usearch_info{};
  usearch_info.similarity = waxcpp::VecSimilarity::kCosine;
  usearch_info.dimension = 8;
  usearch_info.vector_count = 3;
  usearch_info.payload_length = 7;
  const std::vector<std::byte> usearch_payload = {
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40},
      std::byte{0x50}, std::byte{0x60}, std::byte{0x70},
  };
  const auto base_usearch = waxcpp::EncodeUSearchVecSegment(usearch_info, usearch_payload);

  waxcpp::VecSegmentInfo metal_info{};
  metal_info.similarity = waxcpp::VecSimilarity::kDot;
  metal_info.dimension = 2;
  metal_info.vector_count = 2;
  metal_info.payload_length = 2 * 2 * sizeof(float);
  const std::vector<float> metal_vectors = {0.1F, -0.2F, 0.3F, -0.4F};
  const std::vector<std::uint64_t> metal_ids = {11, 12};
  const auto base_metal = waxcpp::EncodeMetalVecSegment(metal_info, metal_vectors, metal_ids);

  constexpr std::uint64_t kSeed = 0x4D563256F00DCAFEULL;  // "MV2V..."
  constexpr std::size_t kIterations = 512;
  std::uint64_t rng = kSeed;
  std::size_t success_count = 0;
  std::size_t throw_count = 0;

  for (std::size_t i = 0; i < kIterations; ++i) {
    std::vector<std::byte> bytes = ((NextRandom(rng) & 1ULL) == 0ULL) ? base_usearch : base_metal;
    const auto mode = NextRandom(rng) % 6ULL;
    if (mode == 0ULL) {
      if (!bytes.empty()) {
        const auto idx = static_cast<std::size_t>(NextRandom(rng) % bytes.size());
        bytes[idx] ^= static_cast<std::byte>((NextRandom(rng) % 255ULL) + 1ULL);
      }
    } else if (mode == 1ULL) {
      if (!bytes.empty()) {
        const auto flips = static_cast<std::size_t>((NextRandom(rng) % 5ULL) + 2ULL);
        for (std::size_t f = 0; f < flips; ++f) {
          const auto idx = static_cast<std::size_t>(NextRandom(rng) % bytes.size());
          bytes[idx] ^= static_cast<std::byte>((NextRandom(rng) % 255ULL) + 1ULL);
        }
      }
    } else if (mode == 2ULL) {
      if (bytes.size() > 1) {
        const std::size_t min_size = 1;
        const std::size_t max_trim = bytes.size() - min_size;
        const std::size_t trim = static_cast<std::size_t>((NextRandom(rng) % max_trim) + 1ULL);
        bytes.resize(bytes.size() - trim);
      }
    } else if (mode == 3ULL) {
      const auto append_count = static_cast<std::size_t>((NextRandom(rng) % 24ULL) + 1ULL);
      for (std::size_t a = 0; a < append_count; ++a) {
        bytes.push_back(static_cast<std::byte>(NextRandom(rng) & 0xFFULL));
      }
    } else if (mode == 4ULL) {
      if (bytes.size() >= 36) {
        // Corrupt header payload length field.
        WriteU64LE(bytes, 20, static_cast<std::uint64_t>(NextRandom(rng)));
      }
    } else {
      if (bytes.size() >= 36) {
        // Corrupt reserved field.
        WriteU64LE(bytes, 28, static_cast<std::uint64_t>(NextRandom(rng)));
      }
    }

    try {
      const auto encoding = waxcpp::DetectVecEncoding(bytes);
      const auto any = waxcpp::DecodeVecSegment(bytes);
      if (encoding == waxcpp::VecEncoding::kUSearch) {
        Require(std::holds_alternative<waxcpp::VecUSearchPayload>(any), "fuzz decode variant mismatch for usearch");
        const auto& decoded = std::get<waxcpp::VecUSearchPayload>(any);
        Require(decoded.payload.size() == decoded.info.payload_length,
                "fuzz usearch payload length mismatch");
      } else {
        Require(std::holds_alternative<waxcpp::VecMetalPayload>(any), "fuzz decode variant mismatch for metal");
        const auto& decoded = std::get<waxcpp::VecMetalPayload>(any);
        Require(decoded.frame_ids.size() == decoded.info.vector_count,
                "fuzz metal frame id count mismatch");
        Require(decoded.vectors.size() ==
                    static_cast<std::size_t>(decoded.info.dimension) *
                        static_cast<std::size_t>(decoded.info.vector_count),
                "fuzz metal vector length mismatch");
      }
      ++success_count;
    } catch (const std::exception&) {
      ++throw_count;
    }
  }

  waxcpp::tests::LogKV("mv2v_fuzz_seed", kSeed);
  waxcpp::tests::LogKV("mv2v_fuzz_iterations", static_cast<std::uint64_t>(kIterations));
  waxcpp::tests::LogKV("mv2v_fuzz_success_count", static_cast<std::uint64_t>(success_count));
  waxcpp::tests::LogKV("mv2v_fuzz_throw_count", static_cast<std::uint64_t>(throw_count));
  Require(success_count > 0, "mv2v fuzz expected at least one successful decode");
  Require(throw_count > 0, "mv2v fuzz expected at least one rejected mutation");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("mv2v_format_test: start");
    ScenarioUSearchEncodeDecodeRoundtrip();
    ScenarioMetalEncodeDecodeRoundtrip();
    ScenarioHeaderValidationFailures();
    ScenarioLengthValidationFailures();
    ScenarioEncodeValidationFailures();
    ScenarioDeterministicDecodeFuzz();
    waxcpp::tests::Log("mv2v_format_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
