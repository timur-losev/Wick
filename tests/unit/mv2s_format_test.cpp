#include "../../src/core/mv2s_format.hpp"
#include "../test_logger.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using waxcpp::core::mv2s::DecodeToc;
using waxcpp::core::mv2s::EncodeEmptyTocV1;
using waxcpp::core::mv2s::EncodeTocV1;
using waxcpp::core::mv2s::FrameSummary;

void ExpectThrowContains(const std::string& name,
                         const std::function<void()>& fn,
                         const std::string& expected_substring) {
  try {
    fn();
  } catch (const std::exception& ex) {
    const std::string message = ex.what();
    if (message.find(expected_substring) == std::string::npos) {
      throw std::runtime_error("unexpected error for " + name + ": " + message);
    }
    waxcpp::tests::Log("expected exception in " + name + ": " + message);
    return;
  }
  throw std::runtime_error("expected throw: " + name);
}

void WriteLE64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  if (offset + 8 > bytes.size()) {
    throw std::runtime_error("WriteLE64 out of range");
  }
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
  }
}

void WriteLE32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  if (offset + 4 > bytes.size()) {
    throw std::runtime_error("WriteLE32 out of range");
  }
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
  }
}

void ResignToc(std::vector<std::byte>& toc) {
  if (toc.size() < 32) {
    throw std::runtime_error("TOC too small for checksum");
  }
  const auto checksum = waxcpp::core::mv2s::ComputeTocChecksum(toc);
  std::copy(checksum.begin(), checksum.end(), toc.end() - 32);
}

std::uint64_t NextRandom(std::uint64_t& state) {
  // Deterministic xorshift64* for reproducible fuzz mutations.
  state ^= state >> 12U;
  state ^= state << 25U;
  state ^= state >> 27U;
  return state * 2685821657736338717ULL;
}

class TocBuilder {
 public:
  void AppendU8(std::uint8_t value) {
    bytes_.push_back(static_cast<std::byte>(value));
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

  [[nodiscard]] std::vector<std::byte> BuildSigned() {
    std::array<std::byte, 32> checksum_placeholder{};
    AppendFixed(checksum_placeholder);
    auto out = std::move(bytes_);
    bytes_.clear();
    const auto checksum = waxcpp::core::mv2s::ComputeTocChecksum(out);
    std::copy(checksum.begin(), checksum.end(), out.end() - 32);
    return out;
  }

 private:
  template <typename T>
  void AppendIntegral(T value) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned v = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes_.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
    }
  }

  std::vector<std::byte> bytes_{};
};

std::vector<std::byte> BuildLexManifestToc(bool with_matching_segment) {
  std::array<std::byte, 32> lex_checksum{};
  lex_checksum.fill(std::byte{0x5A});

  constexpr std::uint64_t kLexOffset = 123'000;
  constexpr std::uint64_t kLexLength = 4'096;

  TocBuilder builder;
  builder.AppendU64(1);   // toc_version
  builder.AppendU32(0);   // frame_count

  builder.AppendU8(1);    // index.lex present
  builder.AppendU64(17);  // docCount
  builder.AppendU64(kLexOffset);
  builder.AppendU64(kLexLength);
  builder.AppendFixed(lex_checksum);
  builder.AppendU32(1);   // version

  builder.AppendU8(0);    // index.vec absent
  builder.AppendU8(0);    // clip absent
  builder.AppendU8(0);    // time index absent
  builder.AppendU8(0);    // memories track absent
  builder.AppendU8(0);    // logic mesh absent
  builder.AppendU8(0);    // sketch track absent

  builder.AppendU32(with_matching_segment ? 1U : 0U);
  if (with_matching_segment) {
    builder.AppendU64(0);           // segment id
    builder.AppendU64(kLexOffset);  // bytes offset
    builder.AppendU64(kLexLength);  // bytes length
    builder.AppendFixed(lex_checksum);
    builder.AppendU8(0);            // compression none
    builder.AppendU8(0);            // kind lex
  }

  builder.AppendString("");  // ticket issuer
  builder.AppendU64(0);      // seq_no
  builder.AppendU64(0);      // expires_in_secs
  builder.AppendU64(0);      // capacity_bytes
  builder.AppendU8(0);       // verified

  builder.AppendU8(0);       // memory_binding absent
  builder.AppendU8(0);       // replay_manifest absent
  builder.AppendU8(0);       // enrichment_queue absent

  std::array<std::byte, 32> merkle_root{};
  builder.AppendFixed(merkle_root);
  return builder.BuildSigned();
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("mv2s_format_test: start");

    {
      waxcpp::tests::Log("scenario: empty TOC roundtrip");
      const auto toc = EncodeEmptyTocV1();
      waxcpp::tests::LogKV("empty_toc_bytes", static_cast<std::uint64_t>(toc.size()));
      const auto summary = DecodeToc(toc);
      waxcpp::tests::LogKV("empty_toc_version", summary.toc_version);
      waxcpp::tests::LogKV("empty_toc_frame_count", summary.frame_count);
      if (summary.toc_version != 1 || summary.frame_count != 0 || !summary.frames.empty()) {
        throw std::runtime_error("empty TOC roundtrip mismatch");
      }
      waxcpp::tests::Log("scenario passed: empty TOC roundtrip");
    }

    {
      waxcpp::tests::Log("scenario: frame TOC roundtrip");
      FrameSummary first{};
      first.id = 0;
      first.payload_offset = 12'345;
      first.payload_length = 64;
      first.payload_checksum.fill(std::byte{0x11});
      waxcpp::tests::LogKV("frame0_payload_offset", first.payload_offset);
      waxcpp::tests::LogKV("frame0_payload_length", first.payload_length);

      FrameSummary second{};
      second.id = 1;
      second.payload_offset = 67'890;
      second.payload_length = 128;
      second.payload_checksum.fill(std::byte{0x22});
      waxcpp::tests::LogKV("frame1_payload_offset", second.payload_offset);
      waxcpp::tests::LogKV("frame1_payload_length", second.payload_length);

      const std::array frames{first, second};
      const auto toc = EncodeTocV1(frames);
      waxcpp::tests::LogKV("frame_toc_bytes", static_cast<std::uint64_t>(toc.size()));
      const auto summary = DecodeToc(toc);
      waxcpp::tests::LogKV("frame_toc_version", summary.toc_version);
      waxcpp::tests::LogKV("frame_toc_frame_count", summary.frame_count);
      if (summary.frame_count != 2 || summary.frames.size() != 2) {
        throw std::runtime_error("frame TOC count mismatch");
      }
      if (summary.frames[0].id != first.id ||
          summary.frames[0].payload_offset != first.payload_offset ||
          summary.frames[0].payload_length != first.payload_length ||
          summary.frames[1].id != second.id ||
          summary.frames[1].payload_offset != second.payload_offset ||
          summary.frames[1].payload_length != second.payload_length) {
        throw std::runtime_error("frame TOC roundtrip fields mismatch");
      }
      if (summary.frames[0].canonical_encoding != 0 ||
          summary.frames[1].canonical_encoding != 0 ||
          !summary.frames[0].stored_checksum.has_value() ||
          !summary.frames[1].stored_checksum.has_value()) {
        throw std::runtime_error("frame TOC roundtrip checksum metadata mismatch");
      }
      waxcpp::tests::Log("scenario passed: frame TOC roundtrip");
    }

    {
      waxcpp::tests::Log("scenario: compressed frame TOC roundtrip");
      FrameSummary frame{};
      frame.id = 0;
      frame.payload_offset = 5'000;
      frame.payload_length = 32;
      frame.payload_checksum.fill(std::byte{0xAA});
      frame.canonical_encoding = 2;   // lz4 in Swift enum map
      frame.canonical_length = 128;
      std::array<std::byte, 32> stored{};
      stored.fill(std::byte{0xBB});
      frame.stored_checksum = stored;

      const auto toc = EncodeTocV1(std::span<const FrameSummary>(&frame, 1));
      const auto summary = DecodeToc(toc);
      if (summary.frame_count != 1 || summary.frames.size() != 1) {
        throw std::runtime_error("compressed TOC count mismatch");
      }
      const auto& decoded = summary.frames[0];
      waxcpp::tests::LogKV("compressed_canonical_encoding", static_cast<std::uint64_t>(decoded.canonical_encoding));
      waxcpp::tests::LogKV("compressed_canonical_length", decoded.canonical_length.value_or(0));
      if (decoded.canonical_encoding != frame.canonical_encoding ||
          !decoded.canonical_length.has_value() ||
          *decoded.canonical_length != *frame.canonical_length ||
          !decoded.stored_checksum.has_value() ||
          *decoded.stored_checksum != *frame.stored_checksum) {
        throw std::runtime_error("compressed TOC roundtrip fields mismatch");
      }
      waxcpp::tests::Log("scenario passed: compressed frame TOC roundtrip");
    }

    {
      waxcpp::tests::Log("scenario: compressed frame missing stored checksum is rejected by encoder");
      FrameSummary frame{};
      frame.id = 0;
      frame.payload_offset = 7'000;
      frame.payload_length = 16;
      frame.payload_checksum.fill(std::byte{0xCD});
      frame.canonical_encoding = 1;  // compressed
      frame.canonical_length = 32;
      ExpectThrowContains(
          "encode_compressed_missing_stored_checksum",
          [&]() { (void)EncodeTocV1(std::span<const FrameSummary>(&frame, 1)); },
          "missing stored_checksum");
      waxcpp::tests::Log("scenario passed: compressed frame missing stored checksum");
    }

    {
      waxcpp::tests::Log("scenario: checksum mismatch");
      auto toc = EncodeEmptyTocV1();
      toc[0] ^= std::byte{0x01};
      waxcpp::tests::LogKV("corrupted_byte_offset", static_cast<std::uint64_t>(0));
      ExpectThrowContains("toc_checksum_mismatch", [&]() { (void)DecodeToc(toc); }, "TOC checksum mismatch");
      waxcpp::tests::Log("scenario passed: checksum mismatch");
    }

    {
      waxcpp::tests::Log("scenario: unsupported TOC version");
      auto toc = EncodeEmptyTocV1();
      WriteLE64(toc, 0, 2);
      ResignToc(toc);
      waxcpp::tests::LogKV("mutated_toc_version", static_cast<std::uint64_t>(2));
      ExpectThrowContains("toc_unsupported_version", [&]() { (void)DecodeToc(toc); }, "unsupported TOC version");
      waxcpp::tests::Log("scenario passed: unsupported TOC version");
    }

    {
      waxcpp::tests::Log("scenario: non-dense frame IDs");
      FrameSummary first{};
      first.id = 0;
      first.payload_offset = 1'000;
      first.payload_length = 0;
      first.payload_checksum.fill(std::byte{0x33});

      FrameSummary second{};
      second.id = 2;  // intentionally sparse
      second.payload_offset = 2'000;
      second.payload_length = 0;
      second.payload_checksum.fill(std::byte{0x44});

      const std::array frames{first, second};
      const auto toc = EncodeTocV1(frames);
      waxcpp::tests::LogKV("sparse_frame_id", second.id);
      ExpectThrowContains("toc_non_dense_frame_ids", [&]() { (void)DecodeToc(toc); }, "frame ids are not dense");
      waxcpp::tests::Log("scenario passed: non-dense frame IDs");
    }

    {
      waxcpp::tests::Log("scenario: invalid optional tag");
      FrameSummary frame{};
      frame.id = 0;
      frame.payload_offset = 3'000;
      frame.payload_length = 0;
      frame.payload_checksum.fill(std::byte{0x55});

      auto toc = EncodeTocV1(std::span<const FrameSummary>(&frame, 1));
      constexpr std::size_t kAnchorTsTagOffset = 28;  // version(8) + frameCount(4) + id(8) + ts(8)
      if (toc.size() <= kAnchorTsTagOffset) {
        throw std::runtime_error("unexpected TOC size for anchor tag mutation");
      }
      waxcpp::tests::LogKV("anchor_ts_tag_offset", static_cast<std::uint64_t>(kAnchorTsTagOffset));
      toc[kAnchorTsTagOffset] = std::byte{0x02};
      ResignToc(toc);
      ExpectThrowContains("toc_invalid_optional_tag", [&]() { (void)DecodeToc(toc); }, "invalid optional tag");
      waxcpp::tests::Log("scenario passed: invalid optional tag");
    }

    {
      waxcpp::tests::Log("scenario: lex manifest without segment catalog match fails");
      const auto toc = BuildLexManifestToc(false);
      ExpectThrowContains("toc_lex_manifest_missing_segment",
                          [&]() { (void)DecodeToc(toc); },
                          "lex index manifest missing matching segment catalog entry");
      waxcpp::tests::Log("scenario passed: lex manifest without segment catalog match fails");
    }

    {
      waxcpp::tests::Log("scenario: lex manifest with matching segment catalog entry succeeds");
      const auto toc = BuildLexManifestToc(true);
      const auto summary = DecodeToc(toc);
      if (!summary.lex_index.has_value() || summary.segments.size() != 1 || summary.segments[0].kind != 0) {
        throw std::runtime_error("lex manifest segment linkage decode mismatch");
      }
      waxcpp::tests::Log("scenario passed: lex manifest with matching segment catalog entry succeeds");
    }

    {
      waxcpp::tests::Log("scenario: deterministic TOC fuzz mutations");
      FrameSummary first{};
      first.id = 0;
      first.payload_offset = 8'000;
      first.payload_length = 64;
      first.payload_checksum.fill(std::byte{0x12});

      FrameSummary second{};
      second.id = 1;
      second.payload_offset = 9'000;
      second.payload_length = 80;
      second.payload_checksum.fill(std::byte{0x34});
      second.canonical_encoding = 2;
      second.canonical_length = 120;
      std::array<std::byte, 32> second_stored{};
      second_stored.fill(std::byte{0x56});
      second.stored_checksum = second_stored;

      const std::array base_frames{first, second};
      const auto baseline = EncodeTocV1(base_frames);
      constexpr std::uint64_t kFuzzSeed = 0xD17A5EEDBADC0FFEULL;
      constexpr std::size_t kFuzzIterations = 512;
      std::uint64_t rng = kFuzzSeed;
      std::size_t success_count = 0;
      std::size_t throw_count = 0;

      for (std::size_t i = 0; i < kFuzzIterations; ++i) {
        auto toc = baseline;
        const std::uint64_t mode = NextRandom(rng) % 5ULL;
        if (mode == 0ULL) {
          if (!toc.empty()) {
            const std::size_t idx = static_cast<std::size_t>(NextRandom(rng) % toc.size());
            toc[idx] ^= static_cast<std::byte>((NextRandom(rng) % 255ULL) + 1ULL);
          }
        } else if (mode == 1ULL) {
          if (!toc.empty()) {
            const std::size_t flips = static_cast<std::size_t>((NextRandom(rng) % 4ULL) + 2ULL);
            for (std::size_t flip = 0; flip < flips; ++flip) {
              const std::size_t idx = static_cast<std::size_t>(NextRandom(rng) % toc.size());
              toc[idx] ^= static_cast<std::byte>((NextRandom(rng) % 255ULL) + 1ULL);
            }
          }
        } else if (mode == 2ULL) {
          if (toc.size() > 1) {
            const std::size_t min_size = 1;
            const std::size_t max_trim = toc.size() - min_size;
            const std::size_t trim = static_cast<std::size_t>((NextRandom(rng) % max_trim) + 1ULL);
            toc.resize(toc.size() - trim);
          }
        } else if (mode == 3ULL) {
          const std::size_t append_count = static_cast<std::size_t>((NextRandom(rng) % 24ULL) + 1ULL);
          for (std::size_t a = 0; a < append_count; ++a) {
            toc.push_back(static_cast<std::byte>(NextRandom(rng) & 0xFFULL));
          }
        } else {
          if (toc.size() >= 12) {
            WriteLE32(toc, 8, static_cast<std::uint32_t>(NextRandom(rng) & 0xFFFFFFFFULL));
          }
        }

        if (toc.size() >= 32 && (NextRandom(rng) & 1ULL) == 0ULL) {
          ResignToc(toc);
        }

        try {
          const auto decoded = DecodeToc(toc);
          if (decoded.frames.size() != decoded.frame_count) {
            throw std::runtime_error("decoded frame count mismatch");
          }
          ++success_count;
        } catch (const std::exception&) {
          ++throw_count;
        }
      }

      waxcpp::tests::LogKV("toc_fuzz_seed", kFuzzSeed);
      waxcpp::tests::LogKV("toc_fuzz_iterations", static_cast<std::uint64_t>(kFuzzIterations));
      waxcpp::tests::LogKV("toc_fuzz_success_count", static_cast<std::uint64_t>(success_count));
      waxcpp::tests::LogKV("toc_fuzz_throw_count", static_cast<std::uint64_t>(throw_count));
      if (throw_count == 0 || success_count == 0) {
        throw std::runtime_error("TOC fuzz expected both valid decodes and rejected mutations");
      }
      waxcpp::tests::Log("scenario passed: deterministic TOC fuzz mutations");
    }

    waxcpp::tests::Log("mv2s_format_test: finished");
    std::cout << "mv2s_format_test passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::cerr << "mv2s_format_test failed: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
