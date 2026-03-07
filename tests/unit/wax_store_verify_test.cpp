#include "waxcpp/wax_store.hpp"
#include "waxcpp/mv2v_format.hpp"

#include "../../src/core/mv2s_format.hpp"
#include "../../src/core/sha256.hpp"
#include "../test_logger.hpp"
#include "../temp_artifacts.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_m2_verify_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

void WriteZeros(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for corruption write");
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) {
    throw std::runtime_error("failed to seek file for corruption write");
  }
  std::vector<char> zeros(length, 0);
  file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  if (!file) {
    throw std::runtime_error("failed to write corruption bytes");
  }
}

std::vector<std::byte> ReadBytesAt(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for byte read");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("failed to seek file for byte read");
  }
  std::vector<std::byte> bytes(length);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("short read for bytes");
  }
  return bytes;
}

void WriteBytesAt(const std::filesystem::path& path, std::uint64_t offset, const std::vector<std::byte>& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for bytes write");
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) {
    throw std::runtime_error("failed to seek file for bytes write");
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    throw std::runtime_error("failed to write bytes");
  }
}

void WriteBytesAt(const std::filesystem::path& path,
                  std::uint64_t offset,
                  const std::array<std::byte, waxcpp::core::mv2s::kHeaderPageSize>& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for page write");
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) {
    throw std::runtime_error("failed to seek file for page write");
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    throw std::runtime_error("failed to write page bytes");
  }
}

void AppendBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    throw std::runtime_error("failed to open file for append");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw std::runtime_error("failed to append bytes");
  }
}

void AppendBytes(const std::filesystem::path& path,
                 const std::array<std::byte, waxcpp::core::mv2s::kFooterSize>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    throw std::runtime_error("failed to open file for append");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw std::runtime_error("failed to append bytes");
  }
}

void ExtendFileSparse(const std::filesystem::path& path, std::uint64_t extra_bytes) {
  if (extra_bytes == 0) {
    return;
  }
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for sparse extension");
  }
  file.seekp(static_cast<std::streamoff>(extra_bytes - 1), std::ios::end);
  if (!file) {
    throw std::runtime_error("failed to seek file for sparse extension");
  }
  const char zero = 0;
  file.write(&zero, 1);
  if (!file) {
    throw std::runtime_error("failed to extend file");
  }
}

std::uint64_t ReadLE64At(const std::filesystem::path& path, std::uint64_t offset) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for read");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("failed to seek file");
  }
  std::array<unsigned char, 8> bytes{};
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("short read for uint64");
  }
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
  }
  return out;
}

void FlipByteAt(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for byte flip");
  }
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  char value = 0;
  file.read(&value, 1);
  if (file.gcount() != 1) {
    throw std::runtime_error("short read for byte flip");
  }
  value ^= static_cast<char>(0x01);
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  file.write(&value, 1);
  if (!file) {
    throw std::runtime_error("failed to write flipped byte");
  }
}

void ExpectThrow(const std::string& name, const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception& ex) {
    waxcpp::tests::Log("expected exception in " + name + ": " + ex.what());
    return;
  } catch (...) {
    waxcpp::tests::Log("expected non-std exception in " + name);
    return;
  }
  throw std::runtime_error("expected throw: " + name);
}

void ExpectThrowContains(const std::string& name,
                         const std::function<void()>& fn,
                         const std::string& expected_substring) {
  try {
    fn();
  } catch (const std::exception& ex) {
    const std::string message = ex.what();
    waxcpp::tests::Log("expected exception in " + name + ": " + message);
    if (message.find(expected_substring) == std::string::npos) {
      throw std::runtime_error("unexpected error for " + name + ": " + message);
    }
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

std::vector<std::byte> BuildWalDataRecord(std::uint64_t sequence, const std::vector<std::byte>& payload) {
  constexpr std::size_t kWalRecordHeaderSize = 48;
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("payload too large for WAL record");
  }
  std::vector<std::byte> out(kWalRecordHeaderSize + payload.size(), std::byte{0});
  WriteLE64(out, 0, sequence);
  WriteLE32(out, 8, static_cast<std::uint32_t>(payload.size()));
  WriteLE32(out, 12, 0);  // flags
  const auto checksum = waxcpp::core::Sha256Digest(payload);
  std::copy(checksum.begin(), checksum.end(), out.begin() + 16);
  std::copy(payload.begin(), payload.end(), out.begin() + static_cast<std::ptrdiff_t>(kWalRecordHeaderSize));
  return out;
}

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendLE32(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendLE64(std::vector<std::byte>& out, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendZeros(std::vector<std::byte>& out, std::size_t count) {
  out.insert(out.end(), count, std::byte{0});
}

std::vector<std::byte> BuildWalPutFramePayload(std::uint64_t frame_id,
                                               std::uint64_t payload_offset,
                                               std::uint64_t payload_length) {
  std::vector<std::byte> payload{};
  payload.reserve(256);
  AppendU8(payload, 0x01);  // WALEntryCodec.OpCode.putFrame
  AppendLE64(payload, frame_id);
  AppendLE64(payload, 0);   // timestampMs (Int64 LE bits)

  // FrameMetaSubset with all optional fields = nil and arrays = [].
  AppendU8(payload, 0);     // uri?
  AppendU8(payload, 0);     // title?
  AppendU8(payload, 0);     // kind?
  AppendU8(payload, 0);     // track?
  AppendLE32(payload, 0);   // tags.count
  AppendLE32(payload, 0);   // labels.count
  AppendLE32(payload, 0);   // contentDates.count
  AppendU8(payload, 0);     // role?
  AppendU8(payload, 0);     // parentId?
  AppendU8(payload, 0);     // chunkIndex?
  AppendU8(payload, 0);     // chunkCount?
  AppendU8(payload, 0);     // chunkManifest?
  AppendU8(payload, 0);     // status?
  AppendU8(payload, 0);     // supersedes?
  AppendU8(payload, 0);     // supersededBy?
  AppendU8(payload, 0);     // searchText?
  AppendU8(payload, 0);     // metadata?

  AppendLE64(payload, payload_offset);
  AppendLE64(payload, payload_length);
  AppendU8(payload, 0);     // canonicalEncoding = plain
  AppendLE64(payload, payload_length);
  AppendZeros(payload, 32); // canonicalChecksum
  AppendZeros(payload, 32); // storedChecksum
  return payload;
}

}  // namespace

int main() {
  waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_m2_verify_");
  const auto path = UniquePath();

  try {
    waxcpp::tests::Log("wax_store_verify_test: start");
    waxcpp::tests::LogKV("test_store_path", path.string());
    {
      waxcpp::tests::Log("scenario: create + verify empty store");
      auto store = waxcpp::WaxStore::Create(path);
      store.Verify(false);
      const auto stats = store.Stats();
      const auto wal_stats = store.WalStats();
      waxcpp::tests::LogKV("create_stats_frame_count", stats.frame_count);
      waxcpp::tests::LogKV("create_stats_generation", stats.generation);
      waxcpp::tests::LogKV("create_wal_size", wal_stats.wal_size);
      waxcpp::tests::LogKV("create_wal_write_pos", wal_stats.write_pos);
      waxcpp::tests::LogKV("create_wal_checkpoint_pos", wal_stats.checkpoint_pos);
      waxcpp::tests::LogKV("create_wal_pending_bytes", wal_stats.pending_bytes);
      waxcpp::tests::LogKV("create_wal_last_seq", wal_stats.last_seq);
      if (stats.frame_count != 0) {
        throw std::runtime_error("expected empty frame_count after create");
      }
      if (wal_stats.wal_size != waxcpp::core::mv2s::kDefaultWalSize) {
        throw std::runtime_error("unexpected wal_size for newly created store");
      }
      if (wal_stats.write_pos != 0 || wal_stats.checkpoint_pos != 0 ||
          wal_stats.pending_bytes != 0 || wal_stats.last_seq != 0 ||
          wal_stats.committed_seq != 0) {
        throw std::runtime_error("expected zeroed WAL state after create");
      }
      store.Close();
    }

    {
      waxcpp::tests::Log("scenario: reopen + verify");
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      reopened.Close();
    }

    // Verify should not mutate file size (no trailing-byte repair in verify path).
    {
      waxcpp::tests::Log("scenario: verify does not repair trailing bytes");
      auto store = waxcpp::WaxStore::Open(path);
      const auto before_extend = std::filesystem::file_size(path);
      ExtendFileSparse(path, 4096);
      const auto after_extend = std::filesystem::file_size(path);
      if (after_extend <= before_extend) {
        throw std::runtime_error("failed to extend file for verify non-repair scenario");
      }

      store.Verify(false);
      const auto after_verify = std::filesystem::file_size(path);
      waxcpp::tests::LogKV("verify_no_repair_before_extend", before_extend);
      waxcpp::tests::LogKV("verify_no_repair_after_extend", after_extend);
      waxcpp::tests::LogKV("verify_no_repair_after_verify", after_verify);
      if (after_verify != after_extend) {
        throw std::runtime_error("verify should not truncate trailing bytes");
      }
      store.Close();
    }

    // Open(repair=false) should not mutate file size.
    {
      waxcpp::tests::Log("scenario: open(repair=false) does not truncate trailing bytes");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();

      const auto before_extend = std::filesystem::file_size(path);
      ExtendFileSparse(path, 4096);
      const auto after_extend = std::filesystem::file_size(path);
      if (after_extend <= before_extend) {
        throw std::runtime_error("failed to extend file for open(repair=false) scenario");
      }

      auto opened_no_repair = waxcpp::WaxStore::Open(path, false);
      opened_no_repair.Verify(false);
      opened_no_repair.Close();

      const auto after_open_no_repair = std::filesystem::file_size(path);
      waxcpp::tests::LogKV("open_no_repair_before_extend", before_extend);
      waxcpp::tests::LogKV("open_no_repair_after_extend", after_extend);
      waxcpp::tests::LogKV("open_no_repair_after_open", after_open_no_repair);
      if (after_open_no_repair != after_extend) {
        throw std::runtime_error("open(repair=false) should not truncate trailing bytes");
      }
    }

    // Recreate clean file before corruption scenarios.
    {
      waxcpp::tests::Log("scenario: recreate clean store after verify non-repair");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    // Corrupt TOC checksum and ensure verify/open fail.
    waxcpp::tests::Log("scenario: corrupt TOC checksum -> open should fail");
    const auto footer_offset = ReadLE64At(path, 24);            // header.footer_offset
    const auto toc_len = ReadLE64At(path, footer_offset + 8);   // footer.toc_len
    const auto toc_checksum_last_byte = (footer_offset - toc_len) + toc_len - 1;
    waxcpp::tests::LogKV("corrupt_toc_footer_offset", footer_offset);
    waxcpp::tests::LogKV("corrupt_toc_len", toc_len);
    waxcpp::tests::LogKV("corrupt_toc_checksum_last_byte", toc_checksum_last_byte);
    FlipByteAt(path, toc_checksum_last_byte);
    ExpectThrow("open_with_corrupt_toc_checksum", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
    });

    // Recreate clean file for header-corruption tests.
    {
      waxcpp::tests::Log("scenario: recreate clean store");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    // Append a newer valid footer and ensure scan picks it when active header is stale.
    waxcpp::tests::Log("scenario: stale header footer pointer -> choose latest scanned footer");
    const auto original_footer_offset = ReadLE64At(path, 24);                 // header.footer_offset
    const auto original_toc_len = ReadLE64At(path, original_footer_offset + 8);
    const auto original_toc_offset = original_footer_offset - original_toc_len;
    waxcpp::tests::LogKV("stale_header_original_footer_offset", original_footer_offset);
    waxcpp::tests::LogKV("stale_header_original_toc_len", original_toc_len);
    const auto toc_bytes = ReadBytesAt(path, original_toc_offset, static_cast<std::size_t>(original_toc_len));
    waxcpp::core::mv2s::Footer appended_footer{};
    appended_footer.toc_len = toc_bytes.size();
    std::copy(toc_bytes.end() - 32, toc_bytes.end(), appended_footer.toc_hash.begin());
    appended_footer.generation = 7;
    appended_footer.wal_committed_seq = 11;

    AppendBytes(path, toc_bytes);
    AppendBytes(path, waxcpp::core::mv2s::EncodeFooter(appended_footer));
    waxcpp::tests::LogKV("stale_header_appended_footer_generation", appended_footer.generation);

    // Corrupt header page A (newer header) so open falls back to stale header B.
    WriteZeros(path, 0, static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      waxcpp::tests::LogKV("stale_header_recovered_generation", reopened.Stats().generation);
      if (reopened.Stats().generation != 7) {
        throw std::runtime_error("expected open() to select latest scanned footer generation");
      }
      reopened.Close();
    }

    // Recreate clean file for deep-verify checksum tests.
    {
      waxcpp::tests::Log("scenario: deep verify detects payload corruption");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    // Build one-frame TOC and payload, invalidate old footer path, and verify deep checksum.
    const std::uint64_t data_start = waxcpp::core::mv2s::kWalOffset + waxcpp::core::mv2s::kDefaultWalSize;
    std::vector<std::byte> payload = {
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}, std::byte{0x55},
        std::byte{0x66}, std::byte{0x77}, std::byte{0x88}, std::byte{0x99}, std::byte{0xAB},
    };
    WriteBytesAt(path, data_start, payload);

    waxcpp::core::mv2s::FrameSummary frame{};
    frame.id = 0;
    frame.payload_offset = data_start;
    frame.payload_length = payload.size();
    frame.payload_checksum = waxcpp::core::Sha256Digest(payload);

    const auto frame_toc = waxcpp::core::mv2s::EncodeTocV1({&frame, 1});
    const auto frame_footer_offset = data_start + payload.size() + frame_toc.size();
    waxcpp::tests::LogKV("deep_verify_data_start", data_start);
    waxcpp::tests::LogKV("deep_verify_payload_size", static_cast<std::uint64_t>(payload.size()));
    waxcpp::tests::LogKV("deep_verify_toc_size", static_cast<std::uint64_t>(frame_toc.size()));
    waxcpp::tests::LogKV("deep_verify_footer_offset", frame_footer_offset);

    waxcpp::core::mv2s::Footer frame_footer{};
    frame_footer.toc_len = frame_toc.size();
    std::copy(frame_toc.end() - 32, frame_toc.end(), frame_footer.toc_hash.begin());
    frame_footer.generation = 21;
    frame_footer.wal_committed_seq = 34;

    WriteBytesAt(path, data_start + payload.size(), frame_toc);
    const auto footer_bytes = waxcpp::core::mv2s::EncodeFooter(frame_footer);
    WriteBytesAt(path, frame_footer_offset, std::vector<std::byte>(footer_bytes.begin(), footer_bytes.end()));

    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
      reopened.Close();
    }

    FlipByteAt(path, data_start + 1);
    ExpectThrow("verify_deep_detects_payload_corruption", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
    });

    // Recreate clean file for compressed-frame deep verify semantics.
    {
      waxcpp::tests::Log("scenario: deep verify compressed frame uses stored checksum");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    std::vector<std::byte> compressed_like_payload = {
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
        std::byte{0xBA}, std::byte{0xAD}, std::byte{0xF0}, std::byte{0x0D},
    };
    WriteBytesAt(path, data_start, compressed_like_payload);

    waxcpp::core::mv2s::FrameSummary compressed_frame{};
    compressed_frame.id = 0;
    compressed_frame.payload_offset = data_start;
    compressed_frame.payload_length = compressed_like_payload.size();
    compressed_frame.payload_checksum.fill(std::byte{0xCC});  // canonical checksum placeholder
    compressed_frame.canonical_encoding = 2;                  // lz4
    compressed_frame.canonical_length = 64;
    compressed_frame.stored_checksum = waxcpp::core::Sha256Digest(compressed_like_payload);

    const auto compressed_toc = waxcpp::core::mv2s::EncodeTocV1({&compressed_frame, 1});
    const auto compressed_footer_offset = data_start + compressed_like_payload.size() + compressed_toc.size();
    waxcpp::tests::LogKV("compressed_verify_toc_size", static_cast<std::uint64_t>(compressed_toc.size()));
    waxcpp::tests::LogKV("compressed_verify_footer_offset", compressed_footer_offset);

    waxcpp::core::mv2s::Footer compressed_footer{};
    compressed_footer.toc_len = compressed_toc.size();
    std::copy(compressed_toc.end() - 32, compressed_toc.end(), compressed_footer.toc_hash.begin());
    compressed_footer.generation = 31;
    compressed_footer.wal_committed_seq = 45;

    WriteBytesAt(path, data_start + compressed_like_payload.size(), compressed_toc);
    const auto compressed_footer_bytes = waxcpp::core::mv2s::EncodeFooter(compressed_footer);
    WriteBytesAt(path,
                 compressed_footer_offset,
                 std::vector<std::byte>(compressed_footer_bytes.begin(), compressed_footer_bytes.end()));

    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
      reopened.Close();
    }

    FlipByteAt(path, data_start + 2);
    ExpectThrow("verify_deep_detects_compressed_stored_checksum_corruption", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
    });

    // Recreate clean file for vec segment deep-verify layout checks.
    {
      waxcpp::tests::Log("scenario: deep verify validates mv2v vec segment layout");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    {
      const std::uint64_t vec_data_start = waxcpp::core::mv2s::kWalOffset + waxcpp::core::mv2s::kDefaultWalSize;
      const std::vector<float> metal_vectors = {0.125F, -0.250F};
      const std::vector<std::uint64_t> metal_ids = {123};
      waxcpp::VecSegmentInfo vec_info{};
      vec_info.similarity = waxcpp::VecSimilarity::kCosine;
      vec_info.dimension = 2;
      vec_info.vector_count = 1;
      vec_info.payload_length = static_cast<std::uint64_t>(metal_vectors.size() * sizeof(float));

      auto vec_segment = waxcpp::EncodeMetalVecSegment(vec_info, metal_vectors, metal_ids);
      WriteBytesAt(path, vec_data_start, vec_segment);

      waxcpp::core::mv2s::SegmentSummary vec_summary{};
      vec_summary.id = 0;
      vec_summary.bytes_offset = vec_data_start;
      vec_summary.bytes_length = vec_segment.size();
      vec_summary.checksum = waxcpp::core::Sha256Digest(vec_segment);
      vec_summary.compression = 0;
      vec_summary.kind = 1;  // vec

      const std::array<waxcpp::core::mv2s::SegmentSummary, 1> vec_segments = {vec_summary};
      auto vec_toc = waxcpp::core::mv2s::EncodeTocV1({}, vec_segments);
      const auto vec_footer_offset = vec_data_start + vec_segment.size() + vec_toc.size();
      waxcpp::core::mv2s::Footer vec_footer{};
      vec_footer.toc_len = vec_toc.size();
      std::copy(vec_toc.end() - 32, vec_toc.end(), vec_footer.toc_hash.begin());
      vec_footer.generation = 44;
      vec_footer.wal_committed_seq = 0;

      WriteBytesAt(path, vec_data_start + vec_segment.size(), vec_toc);
      const auto vec_footer_bytes = waxcpp::core::mv2s::EncodeFooter(vec_footer);
      WriteBytesAt(path,
                   vec_footer_offset,
                   std::vector<std::byte>(vec_footer_bytes.begin(), vec_footer_bytes.end()));

      {
        auto reopened = waxcpp::WaxStore::Open(path);
        reopened.Verify(true);
        reopened.Close();
      }

      // Corrupt MV2V reserved byte and refresh segment checksum/TOC so deep verify fails on layout validation.
      vec_segment[28] = std::byte{0x01};
      WriteBytesAt(path, vec_data_start, vec_segment);
      vec_summary.checksum = waxcpp::core::Sha256Digest(vec_segment);

      const std::array<waxcpp::core::mv2s::SegmentSummary, 1> bad_vec_segments = {vec_summary};
      auto bad_vec_toc = waxcpp::core::mv2s::EncodeTocV1({}, bad_vec_segments);
      const auto bad_vec_footer_offset = vec_data_start + vec_segment.size() + bad_vec_toc.size();
      waxcpp::core::mv2s::Footer bad_vec_footer{};
      bad_vec_footer.toc_len = bad_vec_toc.size();
      std::copy(bad_vec_toc.end() - 32, bad_vec_toc.end(), bad_vec_footer.toc_hash.begin());
      bad_vec_footer.generation = 45;
      bad_vec_footer.wal_committed_seq = 0;

      WriteBytesAt(path, vec_data_start + vec_segment.size(), bad_vec_toc);
      const auto bad_vec_footer_bytes = waxcpp::core::mv2s::EncodeFooter(bad_vec_footer);
      WriteBytesAt(path,
                   bad_vec_footer_offset,
                   std::vector<std::byte>(bad_vec_footer_bytes.begin(), bad_vec_footer_bytes.end()));

      ExpectThrowContains("verify_deep_detects_invalid_vec_segment_layout", [&]() {
        auto reopened = waxcpp::WaxStore::Open(path);
        reopened.Verify(true);
      }, "vec segment decode failed");
    }

    // Recreate clean file for replay snapshot footer lookup test.
    {
      waxcpp::tests::Log("scenario: replay snapshot footer fallback");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    const auto committed_footer_offset = ReadLE64At(path, 24);
    waxcpp::tests::LogKV("snapshot_committed_footer_offset", committed_footer_offset);
    const auto header_page_a = ReadBytesAt(path, 0, static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    auto header = waxcpp::core::mv2s::DecodeHeaderPage(header_page_a);

    // Break fast footer pointer and keep the valid committed offset only in replay snapshot.
    header.footer_offset += 123;
    waxcpp::core::mv2s::ReplaySnapshot snapshot{};
    snapshot.file_generation = header.file_generation;
    snapshot.wal_committed_seq = header.wal_committed_seq;
    snapshot.footer_offset = committed_footer_offset;
    snapshot.wal_write_pos = header.wal_write_pos;
    snapshot.wal_checkpoint_pos = header.wal_checkpoint_pos;
    snapshot.wal_pending_bytes = 0;
    snapshot.wal_last_sequence = header.wal_committed_seq;
    header.replay_snapshot = snapshot;

    WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header));

    // Move EOF far enough so footer scan window cannot see the committed footer.
    ExtendFileSparse(path, waxcpp::core::mv2s::kMaxFooterScanBytes + 1024);
    waxcpp::tests::LogKV("snapshot_sparse_extension", waxcpp::core::mv2s::kMaxFooterScanBytes + 1024);

    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      const auto wal_stats = reopened.WalStats();
      waxcpp::tests::LogKV("snapshot_replay_hit_count", wal_stats.replay_snapshot_hit_count);
      if (wal_stats.replay_snapshot_hit_count != 1) {
        throw std::runtime_error("expected replay snapshot fast-path to be used");
      }
      if (wal_stats.pending_bytes != 0 || wal_stats.write_pos != wal_stats.checkpoint_pos) {
        throw std::runtime_error("expected clean WAL state after replay snapshot recovery");
      }
      reopened.Close();
    }
    {
      const auto repaired_size = std::filesystem::file_size(path);
      const auto expected_size = committed_footer_offset + waxcpp::core::mv2s::kFooterSize;
      waxcpp::tests::LogKV("snapshot_repaired_file_size", repaired_size);
      waxcpp::tests::LogKV("snapshot_expected_file_size", expected_size);
      if (repaired_size != expected_size) {
        throw std::runtime_error("expected open() to truncate trailing bytes to committed footer end");
      }
    }

    // Recreate clean file for pending WAL scan scenarios.
    {
      waxcpp::tests::Log("scenario: pending WAL data with undecodable payload does not block open");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    // Recreate clean file for clean-scan WAL cursor normalization scenario.
    {
      waxcpp::tests::Log("scenario: clean WAL scan normalizes checkpoint to write cursor");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    {
      const auto clean_scan_header_page_a = ReadBytesAt(path,
                                                        0,
                                                        static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
      auto header_a = waxcpp::core::mv2s::DecodeHeaderPage(clean_scan_header_page_a);
      auto header_b = header_a;
      header_b.header_page_generation = header_a.header_page_generation > 0 ? header_a.header_page_generation - 1 : 0;

      const std::uint64_t checkpoint_pos = 123;
      header_a.wal_checkpoint_pos = checkpoint_pos;
      header_a.wal_write_pos = 77;  // force scan path (no fast terminal path)
      header_b.wal_checkpoint_pos = checkpoint_pos;
      header_b.wal_write_pos = 77;

      WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header_a));
      WriteBytesAt(path, waxcpp::core::mv2s::kHeaderPageSize, waxcpp::core::mv2s::EncodeHeaderPage(header_b));

      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      const auto wal_stats = reopened.WalStats();
      reopened.Close();

      waxcpp::tests::LogKV("clean_scan_write_pos", wal_stats.write_pos);
      waxcpp::tests::LogKV("clean_scan_checkpoint_pos", wal_stats.checkpoint_pos);
      waxcpp::tests::LogKV("clean_scan_pending_bytes", wal_stats.pending_bytes);
      if (wal_stats.write_pos != checkpoint_pos || wal_stats.checkpoint_pos != checkpoint_pos) {
        throw std::runtime_error("expected clean scan to align write/checkpoint to scanned cursor");
      }
      if (wal_stats.pending_bytes != 0 || wal_stats.last_seq != 0 || wal_stats.committed_seq != 0) {
        throw std::runtime_error("expected zero pending WAL state after clean scan normalization");
      }
    }

    {
      const auto pending_header_page_a = ReadBytesAt(path,
                                                     0,
                                                     static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
      auto header_a = waxcpp::core::mv2s::DecodeHeaderPage(pending_header_page_a);
      auto header_b = header_a;
      header_b.header_page_generation = header_a.header_page_generation > 0 ? header_a.header_page_generation - 1 : 0;

      const std::vector<std::byte> wal_payload = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
      const auto wal_record = BuildWalDataRecord(1, wal_payload);
      WriteBytesAt(path, header_a.wal_offset, wal_record);

      header_a.wal_checkpoint_pos = 0;
      header_a.wal_write_pos = 0;
      header_b.wal_checkpoint_pos = 0;
      header_b.wal_write_pos = 0;

      WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header_a));
      WriteBytesAt(path, waxcpp::core::mv2s::kHeaderPageSize, waxcpp::core::mv2s::EncodeHeaderPage(header_b));

      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      const auto stats = reopened.Stats();
      const auto wal_stats = reopened.WalStats();
      waxcpp::tests::LogKV("pending_undecodable_pending_frames", stats.pending_frames);
      waxcpp::tests::LogKV("pending_undecodable_wal_write_pos", wal_stats.write_pos);
      waxcpp::tests::LogKV("pending_undecodable_wal_checkpoint_pos", wal_stats.checkpoint_pos);
      waxcpp::tests::LogKV("pending_undecodable_wal_pending_bytes", wal_stats.pending_bytes);
      waxcpp::tests::LogKV("pending_undecodable_wal_last_seq", wal_stats.last_seq);
      if (stats.pending_frames != 0) {
        throw std::runtime_error("expected no decoded pending putFrame mutations");
      }
      if (wal_stats.last_seq != 1) {
        throw std::runtime_error("expected last WAL sequence to reflect scanned undecodable record");
      }
      if (wal_stats.pending_bytes != wal_record.size() || wal_stats.write_pos != wal_record.size()) {
        throw std::runtime_error("unexpected WAL scan state for undecodable pending payload");
      }
      if (wal_stats.checkpoint_pos != 0) {
        throw std::runtime_error("expected checkpoint to remain at committed cursor when pending WAL exists");
      }
      reopened.Close();
    }

    // Recreate clean file for decodable pending putFrame.
    {
      waxcpp::tests::Log("scenario: pending WAL putFrame is decoded and counted");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    {
      const auto pending_header_page_a = ReadBytesAt(path,
                                                     0,
                                                     static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
      auto header_a = waxcpp::core::mv2s::DecodeHeaderPage(pending_header_page_a);
      auto header_b = header_a;
      header_b.header_page_generation = header_a.header_page_generation > 0 ? header_a.header_page_generation - 1 : 0;

      const auto wal_data_start = header_a.wal_offset + header_a.wal_size;
      const auto wal_payload = BuildWalPutFramePayload(42, wal_data_start, 8);
      const auto wal_record = BuildWalDataRecord(1, wal_payload);
      WriteBytesAt(path, header_a.wal_offset, wal_record);

      header_a.wal_checkpoint_pos = 0;
      header_a.wal_write_pos = 0;
      header_b.wal_checkpoint_pos = 0;
      header_b.wal_write_pos = 0;

      WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header_a));
      WriteBytesAt(path, waxcpp::core::mv2s::kHeaderPageSize, waxcpp::core::mv2s::EncodeHeaderPage(header_b));

      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      const auto stats = reopened.Stats();
      const auto wal_stats = reopened.WalStats();
      waxcpp::tests::LogKV("pending_putframe_pending_frames", stats.pending_frames);
      waxcpp::tests::LogKV("pending_putframe_wal_pending_bytes", wal_stats.pending_bytes);
      waxcpp::tests::LogKV("pending_putframe_wal_last_seq", wal_stats.last_seq);
      if (stats.pending_frames != 1) {
        throw std::runtime_error("expected exactly one pending putFrame mutation");
      }
      if (wal_stats.last_seq != 1 || wal_stats.checkpoint_pos != 0) {
        throw std::runtime_error("unexpected WAL sequencing state for pending putFrame");
      }
      if (wal_stats.pending_bytes != wal_record.size() || wal_stats.write_pos != wal_record.size()) {
        throw std::runtime_error("unexpected WAL cursor state for pending putFrame");
      }
      reopened.Close();
    }

    // Recreate clean file for pending putFrame out-of-file range validation.
    {
      waxcpp::tests::Log("scenario: pending WAL putFrame range beyond file size fails open");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    {
      const auto pending_header_page_a = ReadBytesAt(path,
                                                     0,
                                                     static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
      auto header_a = waxcpp::core::mv2s::DecodeHeaderPage(pending_header_page_a);
      auto header_b = header_a;
      header_b.header_page_generation = header_a.header_page_generation > 0 ? header_a.header_page_generation - 1 : 0;

      const auto file_size = std::filesystem::file_size(path);
      const auto wal_payload = BuildWalPutFramePayload(7, file_size - 4, 64);
      const auto wal_record = BuildWalDataRecord(1, wal_payload);
      WriteBytesAt(path, header_a.wal_offset, wal_record);

      header_a.wal_checkpoint_pos = 0;
      header_a.wal_write_pos = 0;
      header_b.wal_checkpoint_pos = 0;
      header_b.wal_write_pos = 0;

      WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header_a));
      WriteBytesAt(path, waxcpp::core::mv2s::kHeaderPageSize, waxcpp::core::mv2s::EncodeHeaderPage(header_b));

      ExpectThrowContains("open_detects_pending_wal_payload_beyond_file_size", [&]() {
        auto reopened = waxcpp::WaxStore::Open(path);
        reopened.Verify(false);
      }, "pending WAL references bytes beyond file size");
    }

    // Recreate clean file for pending putFrame trailing-byte preservation.
    {
      waxcpp::tests::Log("scenario: pending WAL putFrame preserves referenced trailing bytes");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    {
      const auto pending_header_page_a = ReadBytesAt(path,
                                                     0,
                                                     static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
      auto header_a = waxcpp::core::mv2s::DecodeHeaderPage(pending_header_page_a);
      auto header_b = header_a;
      header_b.header_page_generation = header_a.header_page_generation > 0 ? header_a.header_page_generation - 1 : 0;

      const auto trailing_committed_footer_offset = ReadLE64At(path, 24);
      const auto committed_end = trailing_committed_footer_offset + waxcpp::core::mv2s::kFooterSize;
      ExtendFileSparse(path, 4096);

      std::vector<std::byte> trailing_payload = {
          std::byte{0xFA}, std::byte{0xCE}, std::byte{0xB0}, std::byte{0x0C},
          std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
      };
      const auto payload_offset = committed_end + 512;
      WriteBytesAt(path, payload_offset, trailing_payload);
      const auto wal_payload = BuildWalPutFramePayload(77, payload_offset, trailing_payload.size());
      const auto wal_record = BuildWalDataRecord(1, wal_payload);
      WriteBytesAt(path, header_a.wal_offset, wal_record);

      header_a.wal_checkpoint_pos = 0;
      header_a.wal_write_pos = 0;
      header_b.wal_checkpoint_pos = 0;
      header_b.wal_write_pos = 0;

      WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header_a));
      WriteBytesAt(path, waxcpp::core::mv2s::kHeaderPageSize, waxcpp::core::mv2s::EncodeHeaderPage(header_b));

      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      const auto stats = reopened.Stats();
      reopened.Close();
      if (stats.pending_frames != 1) {
        throw std::runtime_error("expected one pending putFrame in trailing-byte preservation scenario");
      }

      const auto expected_end = payload_offset + trailing_payload.size();
      const auto repaired_size = std::filesystem::file_size(path);
      waxcpp::tests::LogKV("pending_trailing_expected_end", expected_end);
      waxcpp::tests::LogKV("pending_trailing_repaired_size", repaired_size);
      if (repaired_size != expected_end) {
        throw std::runtime_error("expected open() to preserve bytes referenced by pending putFrame");
      }
    }

    // Recreate clean file for checkpoint preservation with pending WAL.
    {
      waxcpp::tests::Log("scenario: pending WAL keeps checkpoint cursor from header");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    {
      const auto pending_header_page_a = ReadBytesAt(path,
                                                     0,
                                                     static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
      auto header_a = waxcpp::core::mv2s::DecodeHeaderPage(pending_header_page_a);
      auto header_b = header_a;
      header_b.header_page_generation = header_a.header_page_generation > 0 ? header_a.header_page_generation - 1 : 0;

      const std::uint64_t checkpoint_pos = 128;
      const auto wal_payload = BuildWalPutFramePayload(300, header_a.wal_offset + header_a.wal_size, 8);
      const auto wal_record = BuildWalDataRecord(1, wal_payload);
      WriteBytesAt(path, header_a.wal_offset + checkpoint_pos, wal_record);

      header_a.wal_checkpoint_pos = checkpoint_pos;
      header_a.wal_write_pos = checkpoint_pos + 1;  // avoid terminal fast-path branch
      header_b.wal_checkpoint_pos = checkpoint_pos;
      header_b.wal_write_pos = checkpoint_pos + 1;

      WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header_a));
      WriteBytesAt(path, waxcpp::core::mv2s::kHeaderPageSize, waxcpp::core::mv2s::EncodeHeaderPage(header_b));

      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      const auto stats = reopened.Stats();
      const auto wal_stats = reopened.WalStats();
      reopened.Close();

      waxcpp::tests::LogKV("checkpoint_pending_frames", stats.pending_frames);
      waxcpp::tests::LogKV("checkpoint_write_pos", wal_stats.write_pos);
      waxcpp::tests::LogKV("checkpoint_checkpoint_pos", wal_stats.checkpoint_pos);
      waxcpp::tests::LogKV("checkpoint_pending_bytes", wal_stats.pending_bytes);
      if (stats.pending_frames != 1) {
        throw std::runtime_error("expected one pending putFrame for checkpoint preservation scenario");
      }
      if (wal_stats.checkpoint_pos != checkpoint_pos) {
        throw std::runtime_error("expected checkpoint cursor to remain at header checkpoint while pending WAL exists");
      }
      const auto expected_write_pos = checkpoint_pos + wal_record.size();
      if (wal_stats.write_pos != expected_write_pos || wal_stats.pending_bytes != wal_record.size()) {
        throw std::runtime_error("unexpected WAL write/pending state for checkpoint preservation scenario");
      }
    }

    // Corrupt both header pages; open must fail.
    waxcpp::tests::Log("scenario: both headers corrupt -> open should fail");
    WriteZeros(path, 0, static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    WriteZeros(path, waxcpp::core::mv2s::kHeaderPageSize,
               static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    ExpectThrow("open_with_both_headers_corrupt", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
    });

    waxcpp::tests::CleanupStoreArtifacts(path);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_m2_verify_");
    waxcpp::tests::Log("wax_store_verify_test: finished");
    std::cout << "wax_store_verify_test passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::cerr << "wax_store_verify_test failed: " << ex.what() << "\n";
    waxcpp::tests::CleanupStoreArtifacts(path);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_m2_verify_");
    return EXIT_FAILURE;
  }
}
