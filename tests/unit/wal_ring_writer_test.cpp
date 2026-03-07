#include "../../src/core/wal_ring.hpp"
#include "../test_logger.hpp"

#include <chrono>
#include <cstddef>
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
         ("waxcpp_wal_ring_writer_" + std::to_string(static_cast<long long>(now)) + ".bin");
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void CreateSizedFile(const std::filesystem::path& path, std::size_t size) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to create WAL file");
  }
  if (size > 0) {
    std::vector<std::byte> zeros(size, std::byte{0});
    out.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(zeros.size()));
  }
}

std::vector<std::byte> BuildDeletePayload(std::uint64_t frame_id) {
  std::vector<std::byte> payload(9, std::byte{0});
  payload[0] = static_cast<std::byte>(0x02);  // WALEntryCodec.OpCode.deleteFrame
  for (std::size_t i = 0; i < 8; ++i) {
    payload[1 + i] = static_cast<std::byte>((frame_id >> (8U * i)) & 0xFFU);
  }
  return payload;
}

void RunScenarioInlineSentinel(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: append inline sentinel");
  constexpr std::uint64_t kWalSize = 512;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path, 0, kWalSize);
  const auto seq = writer.Append(BuildDeletePayload(42));
  Require(seq == 1, "expected first append sequence=1");
  Require(writer.write_pos() == 57, "unexpected write_pos after append");
  Require(writer.pending_bytes() == 57, "unexpected pending_bytes after append");
  Require(writer.last_sequence() == 1, "unexpected last_sequence after append");
  Require(writer.sentinel_write_count() == 1, "expected inline sentinel write count");
  Require(writer.write_call_count() == 1, "expected single coalesced write call");

  const auto scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  Require(scan.pending_mutations.size() == 1, "expected one decoded mutation");
  Require(scan.pending_mutations[0].delete_frame.has_value(), "expected decoded delete payload");
  Require(scan.pending_mutations[0].delete_frame->frame_id == 42, "unexpected delete frame id");
  Require(scan.state.last_sequence == 1, "unexpected scanned last_sequence");
  Require(scan.state.write_pos == 57, "unexpected scanned write_pos");
  Require(scan.state.pending_bytes == 57, "unexpected scanned pending_bytes");

  const bool terminal = waxcpp::core::wal::IsTerminalMarker(path, 0, kWalSize, writer.write_pos());
  Require(terminal, "expected terminal marker at post-append cursor");
}

void RunScenarioWrapPaddingAndCheckpoint(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: append with padding wrap and checkpoint");
  constexpr std::uint64_t kWalSize = 256;
  constexpr std::uint64_t kStart = 200;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path,
                                          0,
                                          kWalSize,
                                          kStart,
                                          kStart,
                                          0,
                                          9);
  const auto seq = writer.Append(BuildDeletePayload(77));
  Require(seq == 11, "expected sequence jump due padding record");
  Require(writer.wrap_count() == 1, "expected wrap_count=1");
  Require(writer.write_pos() == 57, "unexpected write_pos after wrap append");
  Require(writer.pending_bytes() == 113, "unexpected pending_bytes after wrap append");
  Require(writer.sentinel_write_count() == 1, "expected inline sentinel");
  Require(writer.write_call_count() == 2, "expected two writes (padding + data/sentinel)");

  const auto scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, kStart, 9);
  Require(scan.pending_mutations.size() == 1, "expected one pending decoded mutation after wrap");
  Require(scan.pending_mutations[0].sequence == 11, "unexpected decoded sequence after wrap");
  Require(scan.pending_mutations[0].delete_frame.has_value(), "expected delete mutation after wrap");
  Require(scan.pending_mutations[0].delete_frame->frame_id == 77, "unexpected wrapped delete frame id");
  Require(scan.state.last_sequence == 11, "unexpected scan last_sequence after wrap");
  Require(scan.state.write_pos == 57, "unexpected scan write_pos after wrap");
  Require(scan.state.pending_bytes == 113, "unexpected scan pending_bytes after wrap");

  writer.RecordCheckpoint();
  Require(writer.checkpoint_pos() == 57, "checkpoint must advance to write_pos");
  Require(writer.pending_bytes() == 0, "pending_bytes must reset on checkpoint");
  Require(writer.checkpoint_count() == 1, "checkpoint_count must increment");
}

void RunScenarioCapacityGuard(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: append capacity guard");
  constexpr std::uint64_t kWalSize = 256;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path,
                                          0,
                                          kWalSize,
                                          240,
                                          240,
                                          240,
                                          5);
  Require(!writer.CanAppend(9), "CanAppend must reject capacity overflow");
  bool threw = false;
  try {
    (void)writer.Append(BuildDeletePayload(1));
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "Append must throw on capacity overflow");
}

void RunScenarioSeparateSentinelWrite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: append reaching end uses separate sentinel write");
  constexpr std::uint64_t kWalSize = 128;
  constexpr std::uint64_t kWritePos = 48;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path,
                                          0,
                                          kWalSize,
                                          kWritePos,
                                          kWritePos,
                                          0,
                                          0);
  const std::vector<std::byte> payload(32, std::byte{0x11});
  const auto seq = writer.Append(payload);
  Require(seq == 1, "expected first sequence=1");
  Require(writer.write_pos() == 0, "write_pos must wrap to 0 at ring end");
  Require(writer.pending_bytes() == 80, "pending_bytes mismatch for end-of-ring append");
  Require(writer.sentinel_write_count() == 1, "sentinel write count mismatch");
  Require(writer.write_call_count() == 2, "expected record write + separate sentinel write");

  const auto scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, kWritePos, 0);
  Require(scan.pending_mutations.empty(), "non-decodable payload should not produce pending mutations");
  Require(scan.state.last_sequence == 1, "scan should observe written record");
  Require(scan.state.write_pos == 0, "scan write_pos mismatch for end-of-ring append");
  Require(scan.state.pending_bytes == 80, "scan pending_bytes mismatch for end-of-ring append");
}

void RunScenarioAppendBatch(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: append batch");
  constexpr std::uint64_t kWalSize = 512;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path, 0, kWalSize);
  const std::vector<std::vector<std::byte>> payloads = {
      BuildDeletePayload(5),
      BuildDeletePayload(6),
  };
  const auto sequences = writer.AppendBatch(payloads);
  Require(sequences.size() == 2, "AppendBatch must return sequence for each payload");
  Require(sequences[0] == 1 && sequences[1] == 2, "AppendBatch sequences must be monotonic");
  Require(writer.last_sequence() == 2, "AppendBatch should advance last_sequence");
  Require(writer.pending_bytes() == 114, "AppendBatch pending bytes mismatch");

  const auto scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  Require(scan.pending_mutations.size() == 2, "expected two decoded mutations after AppendBatch");
  Require(scan.pending_mutations[0].delete_frame.has_value(), "expected first delete payload");
  Require(scan.pending_mutations[1].delete_frame.has_value(), "expected second delete payload");
  Require(scan.pending_mutations[0].delete_frame->frame_id == 5, "unexpected first delete frame id");
  Require(scan.pending_mutations[1].delete_frame->frame_id == 6, "unexpected second delete frame id");
}

void RunScenarioAppendBatchCapacityAtomicity(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: append batch capacity atomicity");
  constexpr std::uint64_t kWalSize = 128;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path, 0, kWalSize);
  (void)writer.Append(BuildDeletePayload(10));

  const auto before_write_pos = writer.write_pos();
  const auto before_pending_bytes = writer.pending_bytes();
  const auto before_last_sequence = writer.last_sequence();
  const auto before_write_calls = writer.write_call_count();
  const auto before_scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  Require(before_scan.pending_mutations.size() == 1, "expected one mutation before overflow batch");
  Require(before_scan.pending_mutations[0].delete_frame.has_value(), "expected delete payload before overflow batch");
  Require(before_scan.pending_mutations[0].delete_frame->frame_id == 10, "unexpected frame id before overflow batch");

  bool threw = false;
  try {
    (void)writer.AppendBatch({
        BuildDeletePayload(11),
        BuildDeletePayload(12),
    });
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "AppendBatch must throw when payload set exceeds WAL capacity");
  Require(writer.write_pos() == before_write_pos, "AppendBatch overflow must not advance write_pos");
  Require(writer.pending_bytes() == before_pending_bytes, "AppendBatch overflow must not change pending_bytes");
  Require(writer.last_sequence() == before_last_sequence, "AppendBatch overflow must not advance sequence");
  Require(writer.write_call_count() == before_write_calls, "AppendBatch overflow must not write bytes");

  const auto after_scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  Require(after_scan.pending_mutations.size() == 1, "overflow batch must not append partial mutations");
  Require(after_scan.pending_mutations[0].delete_frame.has_value(),
          "overflow batch must preserve existing mutation payload");
  Require(after_scan.pending_mutations[0].delete_frame->frame_id == 10,
          "overflow batch must preserve original mutation ordering");
  Require(after_scan.state.last_sequence == before_scan.state.last_sequence,
          "overflow batch must preserve scanned last_sequence");
  Require(after_scan.state.write_pos == before_scan.state.write_pos, "overflow batch must preserve scanned write_pos");
  Require(after_scan.state.pending_bytes == before_scan.state.pending_bytes,
          "overflow batch must preserve scanned pending_bytes");
}

void RunScenarioSequenceOverflowGuard(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: sequence overflow guard");
  constexpr std::uint64_t kWalSize = 512;
  CreateSizedFile(path, static_cast<std::size_t>(kWalSize));

  waxcpp::core::wal::WalRingWriter writer(path,
                                          0,
                                          kWalSize,
                                          0,
                                          0,
                                          0,
                                          std::numeric_limits<std::uint64_t>::max());
  Require(!writer.CanAppend(9), "CanAppend must fail when sequence is exhausted");

  const auto before_scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  bool append_threw = false;
  try {
    (void)writer.Append(BuildDeletePayload(123));
  } catch (const std::exception&) {
    append_threw = true;
  }
  Require(append_threw, "Append must throw on sequence overflow");

  bool batch_threw = false;
  try {
    (void)writer.AppendBatch({BuildDeletePayload(124)});
  } catch (const std::exception&) {
    batch_threw = true;
  }
  Require(batch_threw, "AppendBatch must throw on sequence overflow");

  const auto after_scan = waxcpp::core::wal::ScanPendingMutationsWithState(path, 0, kWalSize, 0, 0);
  Require(after_scan.pending_mutations.empty(), "sequence overflow must not append mutations");
  Require(after_scan.state.last_sequence == before_scan.state.last_sequence,
          "sequence overflow must not change scanned last_sequence");
  Require(after_scan.state.write_pos == before_scan.state.write_pos,
          "sequence overflow must not move scanned write_pos");
  Require(after_scan.state.pending_bytes == before_scan.state.pending_bytes,
          "sequence overflow must not change scanned pending_bytes");
}

}  // namespace

int main() {
  const auto path = UniquePath();
  try {
    waxcpp::tests::Log("wal_ring_writer_test: start");
    waxcpp::tests::LogKV("wal_ring_writer_test_path", path.string());

    RunScenarioInlineSentinel(path);
    RunScenarioWrapPaddingAndCheckpoint(path);
    RunScenarioCapacityGuard(path);
    RunScenarioSeparateSentinelWrite(path);
    RunScenarioAppendBatch(path);
    RunScenarioAppendBatchCapacityAtomicity(path);
    RunScenarioSequenceOverflowGuard(path);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    waxcpp::tests::Log("wal_ring_writer_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return EXIT_FAILURE;
  }
}
