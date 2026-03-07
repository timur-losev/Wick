#include "../../src/core/sha256.hpp"
#include "../../src/core/wal_ring.hpp"
#include "../test_logger.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_wal_ring_" + std::to_string(static_cast<long long>(now)) + ".bin");
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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

std::vector<std::byte> BuildWalRecord(std::uint64_t sequence,
                                      std::uint32_t flags,
                                      const std::vector<std::byte>& payload) {
  constexpr std::size_t kWalRecordHeaderSize = 48;
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("payload too large for WAL record");
  }

  std::vector<std::byte> record(kWalRecordHeaderSize + payload.size(), std::byte{0});
  WriteLE64(record, 0, sequence);
  WriteLE32(record, 8, static_cast<std::uint32_t>(payload.size()));
  WriteLE32(record, 12, flags);
  const auto checksum = waxcpp::core::Sha256Digest(payload);
  std::copy(checksum.begin(), checksum.end(), record.begin() + 16);
  std::copy(payload.begin(),
            payload.end(),
            record.begin() + static_cast<std::ptrdiff_t>(kWalRecordHeaderSize));
  return record;
}

std::vector<std::byte> BuildWalPaddingRecord(std::uint64_t sequence, std::size_t payload_size) {
  constexpr std::size_t kWalRecordHeaderSize = 48;
  if (payload_size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("padding payload too large for WAL record");
  }

  std::vector<std::byte> record(kWalRecordHeaderSize + payload_size, std::byte{0});
  WriteLE64(record, 0, sequence);
  WriteLE32(record, 8, static_cast<std::uint32_t>(payload_size));
  WriteLE32(record, 12, waxcpp::core::wal::kFlagIsPadding);

  const std::vector<std::byte> empty_payload{};
  const auto checksum = waxcpp::core::Sha256Digest(empty_payload);
  std::copy(checksum.begin(), checksum.end(), record.begin() + 16);
  return record;
}

std::vector<std::byte> BuildWalDeletePayload(std::uint64_t frame_id) {
  std::vector<std::byte> payload(9, std::byte{0});
  payload[0] = static_cast<std::byte>(0x02);  // WALEntryCodec.OpCode.deleteFrame
  for (std::size_t i = 0; i < 8; ++i) {
    payload[1 + i] = static_cast<std::byte>((frame_id >> (8U * i)) & 0xFFU);
  }
  return payload;
}

std::vector<std::byte> BuildWalPutEmbeddingPayload(std::uint64_t frame_id, std::uint32_t dimension) {
  std::vector<std::byte> payload{};
  payload.reserve(1 + 8 + 4 + static_cast<std::size_t>(dimension) * 4);
  payload.push_back(static_cast<std::byte>(0x04));  // WALEntryCodec.OpCode.putEmbedding
  for (std::size_t i = 0; i < 8; ++i) {
    payload.push_back(static_cast<std::byte>((frame_id >> (8U * i)) & 0xFFU));
  }
  for (std::size_t i = 0; i < 4; ++i) {
    payload.push_back(static_cast<std::byte>((dimension >> (8U * i)) & 0xFFU));
  }
  for (std::size_t i = 0; i < static_cast<std::size_t>(dimension) * 4; ++i) {
    payload.push_back(std::byte{0});
  }
  return payload;
}

void PlaceBytes(std::vector<std::byte>& target, std::size_t offset, const std::vector<std::byte>& bytes) {
  if (offset > target.size() || bytes.size() > target.size() - offset) {
    throw std::runtime_error("PlaceBytes out of range");
  }
  std::copy(bytes.begin(), bytes.end(), target.begin() + static_cast<std::ptrdiff_t>(offset));
}

void WriteFile(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open file for write");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw std::runtime_error("failed to write file bytes");
  }
}

std::uint64_t NextPseudoRandom(std::uint64_t& state) {
  // Deterministic xorshift64* for reproducible fuzz coverage.
  state ^= (state >> 12U);
  state ^= (state << 25U);
  state ^= (state >> 27U);
  return state * 2685821657736338717ULL;
}

void FillPseudoRandomBytes(std::vector<std::byte>& bytes, std::uint64_t& state) {
  for (auto& value : bytes) {
    value = static_cast<std::byte>(NextPseudoRandom(state) & 0xFFU);
  }
}

void RunScenarioTerminalMarker(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: terminal marker detection");
  constexpr std::uint64_t kWalSize = 512;
  std::vector<std::byte> wal(kWalSize, std::byte{0});

  const auto record = BuildWalRecord(1, 0, BuildWalDeletePayload(7));
  PlaceBytes(wal, 0, record);
  WriteFile(path, wal);

  const bool terminal_at_zero = waxcpp::core::wal::IsTerminalMarker(path, 0, kWalSize, 0);
  const bool terminal_after_record =
      waxcpp::core::wal::IsTerminalMarker(path, 0, kWalSize, static_cast<std::uint64_t>(record.size()));

  waxcpp::tests::LogKV("terminal_at_zero", terminal_at_zero);
  waxcpp::tests::LogKV("terminal_after_record", terminal_after_record);
  Require(!terminal_at_zero, "expected non-terminal marker at start record");
  Require(terminal_after_record, "expected terminal marker after first record");
}

void RunScenarioDecodeStop(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending decode stop with continued state scan");
  constexpr std::uint64_t kWalSize = 1024;
  std::vector<std::byte> wal(kWalSize, std::byte{0});

  const auto invalid_payload = std::vector<std::byte>{static_cast<std::byte>(0xFF)};
  const auto record1 = BuildWalRecord(1, 0, invalid_payload);
  const auto record2 = BuildWalRecord(2, 0, BuildWalDeletePayload(99));
  PlaceBytes(wal, 0, record1);
  PlaceBytes(wal, record1.size(), record2);
  WriteFile(path, wal);

  const auto result = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  const std::uint64_t expected_bytes = static_cast<std::uint64_t>(record1.size() + record2.size());

  waxcpp::tests::LogKV("decode_stop_last_sequence", result.state.last_sequence);
  waxcpp::tests::LogKV("decode_stop_write_pos", result.state.write_pos);
  waxcpp::tests::LogKV("decode_stop_pending_bytes", result.state.pending_bytes);
  waxcpp::tests::LogKV("decode_stop_pending_mutations", static_cast<std::uint64_t>(result.pending_mutations.size()));

  Require(result.state.last_sequence == 2, "expected state scan to reach second record");
  Require(result.state.pending_bytes == expected_bytes, "unexpected pending_bytes after decode-stop scan");
  Require(result.state.write_pos == expected_bytes, "unexpected write_pos after decode-stop scan");
  Require(result.pending_mutations.empty(), "expected pending mutation decode to stop after first invalid record");
}

void RunScenarioWrapAndPadding(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: wrap + padding scan");
  constexpr std::uint64_t kWalSize = 256;
  constexpr std::uint64_t kCheckpointPos = 200;
  std::vector<std::byte> wal(kWalSize, std::byte{0});

  const auto padding_record = BuildWalPaddingRecord(10, 8);  // 48 + 8 = 56, exactly to ring end from 200
  Require(padding_record.size() == 56, "padding record size mismatch");
  const auto delete_record = BuildWalRecord(11, 0, BuildWalDeletePayload(123));
  PlaceBytes(wal, static_cast<std::size_t>(kCheckpointPos), padding_record);
  PlaceBytes(wal, 0, delete_record);
  WriteFile(path, wal);

  const auto result = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, kCheckpointPos, 9);
  const auto expected_write_pos = static_cast<std::uint64_t>(delete_record.size());
  const auto expected_pending_bytes = static_cast<std::uint64_t>(padding_record.size() + delete_record.size());

  waxcpp::tests::LogKV("wrap_last_sequence", result.state.last_sequence);
  waxcpp::tests::LogKV("wrap_write_pos", result.state.write_pos);
  waxcpp::tests::LogKV("wrap_pending_bytes", result.state.pending_bytes);
  waxcpp::tests::LogKV("wrap_pending_mutations", static_cast<std::uint64_t>(result.pending_mutations.size()));

  Require(result.state.last_sequence == 11, "expected last sequence to include wrapped data record");
  Require(result.state.write_pos == expected_write_pos, "unexpected write_pos for wrapped scan");
  Require(result.state.pending_bytes == expected_pending_bytes, "unexpected pending_bytes for wrapped scan");
  Require(result.pending_mutations.size() == 1, "expected exactly one decoded pending mutation");
  Require(result.pending_mutations[0].kind == waxcpp::core::wal::WalMutationKind::kDeleteFrame,
          "expected decoded wrapped mutation kind=deleteFrame");
  Require(result.pending_mutations[0].delete_frame.has_value(), "expected decoded delete frame payload");
  Require(result.pending_mutations[0].delete_frame->frame_id == 123, "unexpected delete frame id");
}

void RunScenarioPutEmbeddingDecode(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding decode fields");
  constexpr std::uint64_t kWalSize = 512;
  std::vector<std::byte> wal(kWalSize, std::byte{0});
  const auto record = BuildWalRecord(4, 0, BuildWalPutEmbeddingPayload(55, 8));
  PlaceBytes(wal, 0, record);
  WriteFile(path, wal);

  const auto result = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  Require(result.pending_mutations.size() == 1, "expected one putEmbedding mutation");
  const auto& mutation = result.pending_mutations[0];
  Require(mutation.kind == waxcpp::core::wal::WalMutationKind::kPutEmbedding, "expected putEmbedding kind");
  Require(mutation.put_embedding.has_value(), "expected putEmbedding payload");
  Require(mutation.put_embedding->frame_id == 55, "unexpected putEmbedding frame_id");
  Require(mutation.put_embedding->dimension == 8, "unexpected putEmbedding dimension");
  Require(mutation.put_embedding->vector.size() == 8, "unexpected putEmbedding vector size");
}

void RunScenarioScanStateParity(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: ScanWalState parity with pending scan state");
  constexpr std::uint64_t kWalSize = 512;
  std::vector<std::byte> wal(kWalSize, std::byte{0});
  const auto record = BuildWalRecord(5, 0, BuildWalDeletePayload(3));
  PlaceBytes(wal, 0, record);
  WriteFile(path, wal);

  const auto state_only = waxcpp::core::wal::ScanWalState(path, 0, kWalSize, 0);
  const auto with_pending = waxcpp::core::wal::ScanPendingMutationsWithState(
      path,
      0,
      kWalSize,
      0,
      std::numeric_limits<std::uint64_t>::max());

  Require(state_only.last_sequence == with_pending.state.last_sequence, "ScanWalState last_sequence mismatch");
  Require(state_only.write_pos == with_pending.state.write_pos, "ScanWalState write_pos mismatch");
  Require(state_only.pending_bytes == with_pending.state.pending_bytes, "ScanWalState pending_bytes mismatch");
}

void RunScenarioDeterministicFuzzScan(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: deterministic fuzz scan");
  constexpr std::uint64_t kWalSize = 1024;
  constexpr int kIterations = 256;
  std::uint64_t seed = 0x26B16D5A0A7E9C43ULL;
  waxcpp::tests::LogKV("fuzz_seed", seed);
  waxcpp::tests::LogKV("fuzz_iterations", static_cast<std::uint64_t>(kIterations));

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    std::vector<std::byte> wal(static_cast<std::size_t>(kWalSize), std::byte{0});
    FillPseudoRandomBytes(wal, seed);
    WriteFile(path, wal);

    const auto checkpoint_pos = NextPseudoRandom(seed) % kWalSize;
    const auto committed_seq = NextPseudoRandom(seed) % 1000ULL;

    const auto with_pending = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, checkpoint_pos, committed_seq);
    const auto state_only = waxcpp::core::wal::ScanWalState(path, 0, kWalSize, checkpoint_pos);

    Require(with_pending.state.write_pos < kWalSize,
            "fuzz scan write_pos out of wal range at iteration " + std::to_string(iteration));
    Require(with_pending.state.pending_bytes <= kWalSize,
            "fuzz scan pending_bytes out of wal range at iteration " + std::to_string(iteration));
    Require(state_only.write_pos == with_pending.state.write_pos,
            "fuzz scan state parity write_pos mismatch at iteration " + std::to_string(iteration));
    Require(state_only.pending_bytes == with_pending.state.pending_bytes,
            "fuzz scan state parity pending_bytes mismatch at iteration " + std::to_string(iteration));
    Require(state_only.last_sequence == with_pending.state.last_sequence,
            "fuzz scan state parity last_sequence mismatch at iteration " + std::to_string(iteration));
  }
}

void RunScenarioDeterministicFuzzValidChecksummedPayload(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: deterministic fuzz with valid-checksummed payload");
  constexpr std::uint64_t kWalSize = 512;
  constexpr int kIterations = 256;
  std::uint64_t seed = 0x0BADC0FFEE1234ABULL;
  waxcpp::tests::LogKV("payload_fuzz_seed", seed);
  waxcpp::tests::LogKV("payload_fuzz_iterations", static_cast<std::uint64_t>(kIterations));

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    std::vector<std::byte> wal(static_cast<std::size_t>(kWalSize), std::byte{0});

    const auto payload_size = static_cast<std::size_t>((NextPseudoRandom(seed) % 196ULL) + 1ULL);
    std::vector<std::byte> payload(payload_size, std::byte{0});
    FillPseudoRandomBytes(payload, seed);
    const auto record = BuildWalRecord(1, 0, payload);
    Require(record.size() < wal.size(), "fuzz record must fit WAL");
    PlaceBytes(wal, 0, record);
    WriteFile(path, wal);

    const auto result = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
    const auto expected_size = static_cast<std::uint64_t>(record.size());
    Require(result.state.last_sequence == 1,
            "valid-checksummed payload fuzz: last_sequence mismatch at iteration " + std::to_string(iteration));
    Require(result.state.write_pos == expected_size,
            "valid-checksummed payload fuzz: write_pos mismatch at iteration " + std::to_string(iteration));
    Require(result.state.pending_bytes == expected_size,
            "valid-checksummed payload fuzz: pending_bytes mismatch at iteration " + std::to_string(iteration));

    if (!result.pending_mutations.empty()) {
      Require(result.pending_mutations.size() == 1,
              "valid-checksummed payload fuzz: expected at most one decoded mutation");
      const auto& mutation = result.pending_mutations[0];
      Require(mutation.sequence == 1, "valid-checksummed payload fuzz: decoded sequence mismatch");
      switch (mutation.kind) {
        case waxcpp::core::wal::WalMutationKind::kPutFrame:
          Require(mutation.put_frame.has_value(), "decoded putFrame must carry payload");
          break;
        case waxcpp::core::wal::WalMutationKind::kDeleteFrame:
          Require(mutation.delete_frame.has_value(), "decoded deleteFrame must carry payload");
          break;
        case waxcpp::core::wal::WalMutationKind::kSupersedeFrame:
          Require(mutation.supersede_frame.has_value(), "decoded supersedeFrame must carry payload");
          break;
        case waxcpp::core::wal::WalMutationKind::kPutEmbedding:
          Require(mutation.put_embedding.has_value(), "decoded putEmbedding must carry payload");
          Require(mutation.put_embedding->vector.size() ==
                      static_cast<std::size_t>(mutation.put_embedding->dimension),
                  "decoded putEmbedding vector/dimension mismatch");
          break;
      }
    }
  }
}

}  // namespace

int main() {
  const auto path = UniquePath();
  try {
    waxcpp::tests::Log("wal_ring_test: start");
    waxcpp::tests::LogKV("wal_ring_test_path", path.string());

    RunScenarioTerminalMarker(path);
    RunScenarioDecodeStop(path);
    RunScenarioWrapAndPadding(path);
    RunScenarioPutEmbeddingDecode(path);
    RunScenarioScanStateParity(path);
    RunScenarioDeterministicFuzzScan(path);
    RunScenarioDeterministicFuzzValidChecksummedPayload(path);

    std::filesystem::remove(path);
    waxcpp::tests::Log("wal_ring_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return EXIT_FAILURE;
  }
}
