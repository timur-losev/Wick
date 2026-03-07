#include "../../src/core/mv2s_format.hpp"
#include "../../src/core/sha256.hpp"
#include "../test_logger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using waxcpp::core::Sha256Digest;
using waxcpp::core::mv2s::EncodeFooter;
using waxcpp::core::mv2s::EncodeHeaderPage;
using waxcpp::core::mv2s::EncodeTocV1;
using waxcpp::core::mv2s::Footer;
using waxcpp::core::mv2s::FrameSummary;
using waxcpp::core::mv2s::HeaderPage;

void WriteAt(std::ofstream& out, std::uint64_t offset, std::span<const std::byte> bytes) {
  out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!out) {
    throw std::runtime_error("generator: seekp failed");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw std::runtime_error("generator: write failed");
  }
}

void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    throw std::runtime_error("generator: copy_file failed: " + ec.message());
  }
}

void FlipByte(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("generator: failed to open file for flip");
  }
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  char value = 0;
  file.read(&value, 1);
  if (file.gcount() != 1) {
    throw std::runtime_error("generator: failed to read byte for flip");
  }
  value ^= static_cast<char>(0x01);
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  file.write(&value, 1);
  if (!file) {
    throw std::runtime_error("generator: failed to write flipped byte");
  }
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("generator: failed to open text output");
  }
  out << text;
  if (!out) {
    throw std::runtime_error("generator: failed to write text output");
  }
}

struct StoreBuild {
  std::vector<FrameSummary> frames;
  std::vector<std::vector<std::byte>> payloads;
  std::uint64_t wal_size = 64 * 1024;
  std::uint64_t generation = 0;
  std::uint64_t wal_committed_seq = 0;
};

struct StoreLayout {
  std::uint64_t data_start = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t toc_offset = 0;
  std::uint64_t toc_size = 0;
  std::uint64_t footer_offset = 0;
  std::uint64_t file_size = 0;
  std::uint64_t frame_count = 0;
};

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

std::vector<std::byte> BuildLexManifestWithoutSegmentToc() {
  std::array<std::byte, 32> checksum{};
  checksum.fill(std::byte{0x5A});

  TocBuilder builder;
  builder.AppendU64(1);        // toc_version
  builder.AppendU32(0);        // frame_count
  builder.AppendU8(1);         // index.lex present
  builder.AppendU64(10);       // doc_count
  builder.AppendU64(123'000);  // bytes_offset
  builder.AppendU64(4'096);    // bytes_length
  builder.AppendFixed(checksum);
  builder.AppendU32(1);        // version
  builder.AppendU8(0);         // index.vec absent
  builder.AppendU8(0);         // clip absent
  builder.AppendU8(0);         // time index absent
  builder.AppendU8(0);         // memories track absent
  builder.AppendU8(0);         // logic mesh absent
  builder.AppendU8(0);         // sketch track absent
  builder.AppendU32(0);        // segment_count (missing required lex entry)
  builder.AppendString("");    // ticket issuer
  builder.AppendU64(0);        // seq_no
  builder.AppendU64(0);        // expires_in_secs
  builder.AppendU64(0);        // capacity_bytes
  builder.AppendU8(0);         // verified
  builder.AppendU8(0);         // memory_binding absent
  builder.AppendU8(0);         // replay_manifest absent
  builder.AppendU8(0);         // enrichment_queue absent
  std::array<std::byte, 32> merkle_root{};
  builder.AppendFixed(merkle_root);
  return builder.BuildSigned();
}

StoreLayout BuildStore(const std::filesystem::path& path,
                       const StoreBuild& build,
                       std::uint64_t* first_payload_offset_out,
                       const std::vector<std::byte>* toc_override_bytes = nullptr) {
  if (build.frames.size() != build.payloads.size()) {
    throw std::runtime_error("generator: frames/payloads size mismatch");
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("generator: failed to create store");
  }

  const std::uint64_t data_start = waxcpp::core::mv2s::kWalOffset + build.wal_size;
  std::uint64_t cursor = data_start;
  std::uint64_t payload_bytes = 0;
  std::vector<FrameSummary> frames = build.frames;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto& payload = build.payloads[i];
    if (frames[i].id != i) {
      throw std::runtime_error("generator: frame ids must be dense");
    }
    frames[i].payload_offset = cursor;
    frames[i].payload_length = payload.size();
    frames[i].payload_checksum = Sha256Digest(payload);
    if (!payload.empty()) {
      WriteAt(out, cursor, payload);
      cursor += payload.size();
      payload_bytes += payload.size();
    }
  }
  if (!frames.empty() && first_payload_offset_out != nullptr) {
    *first_payload_offset_out = frames.front().payload_offset;
  }

  std::vector<std::byte> toc_bytes;
  if (toc_override_bytes != nullptr) {
    toc_bytes = *toc_override_bytes;
  } else {
    toc_bytes = EncodeTocV1(frames);
  }
  const std::uint64_t toc_offset = cursor;
  WriteAt(out, toc_offset, toc_bytes);

  Footer footer{};
  footer.toc_len = toc_bytes.size();
  std::copy(toc_bytes.end() - 32, toc_bytes.end(), footer.toc_hash.begin());
  footer.generation = build.generation;
  footer.wal_committed_seq = build.wal_committed_seq;
  const auto footer_bytes = EncodeFooter(footer);
  const std::uint64_t footer_offset = toc_offset + toc_bytes.size();
  WriteAt(out, footer_offset, footer_bytes);

  HeaderPage page_a{};
  page_a.header_page_generation = 1;
  page_a.file_generation = build.generation;
  page_a.footer_offset = footer_offset;
  page_a.wal_offset = waxcpp::core::mv2s::kWalOffset;
  page_a.wal_size = build.wal_size;
  page_a.wal_write_pos = 0;
  page_a.wal_checkpoint_pos = 0;
  page_a.wal_committed_seq = build.wal_committed_seq;
  page_a.toc_checksum = footer.toc_hash;

  HeaderPage page_b = page_a;
  page_b.header_page_generation = 0;

  const auto page_a_bytes = EncodeHeaderPage(page_a);
  const auto page_b_bytes = EncodeHeaderPage(page_b);
  WriteAt(out, 0, page_a_bytes);
  WriteAt(out, waxcpp::core::mv2s::kHeaderPageSize, page_b_bytes);

  out.flush();
  if (!out) {
    throw std::runtime_error("generator: flush failed");
  }

  const std::uint64_t file_size = footer_offset + footer_bytes.size();
  return StoreLayout{
      .data_start = data_start,
      .payload_bytes = payload_bytes,
      .toc_offset = toc_offset,
      .toc_size = static_cast<std::uint64_t>(toc_bytes.size()),
      .footer_offset = footer_offset,
      .file_size = file_size,
      .frame_count = static_cast<std::uint64_t>(frames.size()),
  };
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("mv2s_fixture_generator: start");
    const std::filesystem::path output_dir = WAXCPP_PARITY_FIXTURES_DIR;
    waxcpp::tests::LogKV("output_dir", output_dir.string());
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
      throw std::runtime_error("generator: failed to create output dir: " + ec.message());
    }

    const auto valid_empty = output_dir / "synthetic_valid_empty.mv2s";
    {
      StoreBuild build{};
      build.generation = 0;
      build.wal_committed_seq = 0;
      const auto layout = BuildStore(valid_empty, build, nullptr);
      const auto expected_path = std::filesystem::path(valid_empty.string() + ".expected");
      WriteText(expected_path,
                "mode=pass\n"
                "verify_deep=true\n"
                "frame_count=0\n"
                "generation=0\n");
      waxcpp::tests::Log("fixture generated");
      waxcpp::tests::LogKV("name", "synthetic_valid_empty");
      waxcpp::tests::LogKV("mode", "pass");
      waxcpp::tests::LogKV("path", valid_empty.string());
      waxcpp::tests::LogKV("expected", expected_path.string());
      waxcpp::tests::LogKV("wal_size", build.wal_size);
      waxcpp::tests::LogKV("generation", build.generation);
      waxcpp::tests::LogKV("wal_committed_seq", build.wal_committed_seq);
      waxcpp::tests::LogKV("frame_count", layout.frame_count);
      waxcpp::tests::LogKV("data_start", layout.data_start);
      waxcpp::tests::LogKV("toc_offset", layout.toc_offset);
      waxcpp::tests::LogKV("toc_size", layout.toc_size);
      waxcpp::tests::LogKV("footer_offset", layout.footer_offset);
      waxcpp::tests::LogKV("file_size", layout.file_size);
    }

    const auto valid_payload = output_dir / "synthetic_valid_payload.mv2s";
    std::uint64_t first_payload_offset = 0;
    {
      StoreBuild build{};
      build.generation = 5;
      build.wal_committed_seq = 9;
      build.frames.push_back(FrameSummary{.id = 0});
      build.payloads.push_back({
          std::byte{0x57}, std::byte{0x61}, std::byte{0x78}, std::byte{0x43}, std::byte{0x50},
          std::byte{0x50}, std::byte{0x21}, std::byte{0x21},
      });
      const auto layout = BuildStore(valid_payload, build, &first_payload_offset);
      const auto expected_path = std::filesystem::path(valid_payload.string() + ".expected");
      WriteText(expected_path,
                "mode=pass\n"
                "verify_deep=true\n"
                "frame_count=1\n"
                "generation=5\n"
                "wal_pending_bytes=0\n"
                "wal_last_seq=9\n"
                "wal_committed_seq=9\n"
                "wal_pending_embedding_mutations=0\n"
                "wal_pending_delete_mutations=0\n"
                "wal_pending_supersede_mutations=0\n"
                "frame_payload_len.0=8\n"
                "frame_status.0=0\n"
                "frame_payload_utf8.0=WaxCPP!!\n");
      waxcpp::tests::Log("fixture generated");
      waxcpp::tests::LogKV("name", "synthetic_valid_payload");
      waxcpp::tests::LogKV("mode", "pass");
      waxcpp::tests::LogKV("path", valid_payload.string());
      waxcpp::tests::LogKV("expected", expected_path.string());
      waxcpp::tests::LogKV("wal_size", build.wal_size);
      waxcpp::tests::LogKV("generation", build.generation);
      waxcpp::tests::LogKV("wal_committed_seq", build.wal_committed_seq);
      waxcpp::tests::LogKV("frame_count", layout.frame_count);
      waxcpp::tests::LogKV("payload_bytes", layout.payload_bytes);
      waxcpp::tests::LogKV("first_payload_offset", first_payload_offset);
      waxcpp::tests::LogKV("toc_offset", layout.toc_offset);
      waxcpp::tests::LogKV("toc_size", layout.toc_size);
      waxcpp::tests::LogKV("footer_offset", layout.footer_offset);
      waxcpp::tests::LogKV("file_size", layout.file_size);
    }

    const auto open_fail = output_dir / "synthetic_open_fail_bad_footer_magic.mv2s";
    {
      CopyFile(valid_empty, open_fail);
      const auto footer_offset = std::filesystem::file_size(open_fail) - waxcpp::core::mv2s::kFooterSize;
      FlipByte(open_fail, footer_offset);
      const auto expected_path = std::filesystem::path(open_fail.string() + ".expected");
      WriteText(expected_path,
                "mode=open_fail\n"
                "error_contains=no valid footer\n");
      waxcpp::tests::Log("fixture generated");
      waxcpp::tests::LogKV("name", "synthetic_open_fail_bad_footer_magic");
      waxcpp::tests::LogKV("mode", "open_fail");
      waxcpp::tests::LogKV("path", open_fail.string());
      waxcpp::tests::LogKV("expected", expected_path.string());
      waxcpp::tests::LogKV("mutated_footer_offset", footer_offset);
    }

    const auto verify_fail = output_dir / "synthetic_verify_fail_payload_checksum.mv2s";
    {
      CopyFile(valid_payload, verify_fail);
      FlipByte(verify_fail, first_payload_offset);
      const auto expected_path = std::filesystem::path(verify_fail.string() + ".expected");
      WriteText(expected_path,
                "mode=verify_fail\n"
                "verify_deep=true\n"
                "error_contains=stored checksum mismatch\n");
      waxcpp::tests::Log("fixture generated");
      waxcpp::tests::LogKV("name", "synthetic_verify_fail_payload_checksum");
      waxcpp::tests::LogKV("mode", "verify_fail");
      waxcpp::tests::LogKV("path", verify_fail.string());
      waxcpp::tests::LogKV("expected", expected_path.string());
      waxcpp::tests::LogKV("mutated_payload_offset", first_payload_offset);
    }

    const auto open_fail_manifest = output_dir / "synthetic_open_fail_lex_manifest_missing_segment.mv2s";
    {
      StoreBuild build{};
      build.generation = 2;
      build.wal_committed_seq = 3;
      const auto bad_toc = BuildLexManifestWithoutSegmentToc();
      const auto layout = BuildStore(open_fail_manifest, build, nullptr, &bad_toc);
      const auto expected_path = std::filesystem::path(open_fail_manifest.string() + ".expected");
      WriteText(expected_path,
                "mode=open_fail\n"
                "error_contains=lex index manifest missing matching segment catalog entry\n");
      waxcpp::tests::Log("fixture generated");
      waxcpp::tests::LogKV("name", "synthetic_open_fail_lex_manifest_missing_segment");
      waxcpp::tests::LogKV("mode", "open_fail");
      waxcpp::tests::LogKV("path", open_fail_manifest.string());
      waxcpp::tests::LogKV("expected", expected_path.string());
      waxcpp::tests::LogKV("wal_size", build.wal_size);
      waxcpp::tests::LogKV("generation", build.generation);
      waxcpp::tests::LogKV("wal_committed_seq", build.wal_committed_seq);
      waxcpp::tests::LogKV("frame_count", layout.frame_count);
      waxcpp::tests::LogKV("toc_offset", layout.toc_offset);
      waxcpp::tests::LogKV("toc_size", layout.toc_size);
      waxcpp::tests::LogKV("footer_offset", layout.footer_offset);
      waxcpp::tests::LogKV("file_size", layout.file_size);
    }

    waxcpp::tests::Log("mv2s_fixture_generator: finished");
    return 0;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::cerr << ex.what() << "\n";
    return 1;
  }
}
