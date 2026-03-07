#include "waxcpp/wax_store.hpp"

#include "../../src/core/mv2s_format.hpp"
#include "../../src/core/wal_ring.hpp"
#include "../../src/core/wax_store_test_hooks.hpp"
#include "../test_logger.hpp"
#include "../temp_artifacts.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_write_test_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
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

std::vector<std::byte> ReadExactly(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for read");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("failed to seek for read");
  }
  std::vector<std::byte> out(length);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    throw std::runtime_error("short read");
  }
  return out;
}

void WriteBytesAt(const std::filesystem::path& path, std::uint64_t offset, std::span<const std::byte> bytes) {
  std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!io) {
    throw std::runtime_error("failed to open file for write");
  }
  io.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!io) {
    throw std::runtime_error("failed to seek for write");
  }
  io.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!io) {
    throw std::runtime_error("failed to write bytes");
  }
}

std::string QuoteShellArg(const std::string& raw) {
  std::string quoted{};
  quoted.reserve(raw.size() + 2);
  quoted.push_back('"');
  for (const char ch : raw) {
    if (ch == '"') {
      quoted.push_back('\\');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('"');
  return quoted;
}

std::filesystem::path CrossProcessLeaseReadyPath(const std::filesystem::path& store_path) {
  auto ready = store_path;
  ready += ".cross-process-lease.ready";
  return ready;
}

#ifdef _WIN32
HANDLE LaunchHoldWriterLeaseHelper(const std::filesystem::path& executable,
                                   const std::filesystem::path& store_path,
                                   const std::filesystem::path& ready_path) {
  std::wstring command_line = L"\"" + executable.wstring() + L"\" --hold-writer-lease \"" + store_path.wstring() +
                              L"\" \"" + ready_path.wstring() + L"\"";
  std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process_info{};
  const BOOL created = ::CreateProcessW(nullptr,
                                        mutable_command.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &process_info);
  if (created == FALSE) {
    throw std::runtime_error("failed to launch cross-process writer lease helper");
  }
  ::CloseHandle(process_info.hThread);
  return process_info.hProcess;
}
#endif

int RunHoldWriterLeaseHelper(const std::filesystem::path& store_path, const std::filesystem::path& ready_path) {
  try {
    auto store = waxcpp::WaxStore::Open(store_path);
    {
      std::ofstream out(ready_path, std::ios::binary | std::ios::trunc);
      Require(static_cast<bool>(out), "failed to create cross-process lease ready file");
      const char marker[] = "ready";
      out.write(marker, static_cast<std::streamsize>(sizeof(marker) - 1));
      Require(static_cast<bool>(out), "failed to write cross-process lease ready file");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    store.Close();
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(std::string("cross-process lease helper failed: ") + ex.what());
    return EXIT_FAILURE;
  }
}

class ScopedCommitFailStep {
 public:
  explicit ScopedCommitFailStep(std::optional<std::uint32_t> step) {
    if (step.has_value()) {
      waxcpp::core::testing::SetCommitFailStep(*step);
    } else {
      waxcpp::core::testing::ClearCommitFailStep();
    }
  }

  ~ScopedCommitFailStep() {
    waxcpp::core::testing::ClearCommitFailStep();
  }
};

waxcpp::core::mv2s::TocSummary ReadCommittedToc(const std::filesystem::path& path) {
  const auto footer_offset = ReadLE64At(path, 24);  // header.footer_offset
  const auto footer_bytes = ReadExactly(path, footer_offset, static_cast<std::size_t>(waxcpp::core::mv2s::kFooterSize));
  const auto footer = waxcpp::core::mv2s::DecodeFooter(footer_bytes);
  const auto toc_offset = footer_offset - footer.toc_len;
  const auto toc_bytes = ReadExactly(path, toc_offset, static_cast<std::size_t>(footer.toc_len));
  return waxcpp::core::mv2s::DecodeToc(toc_bytes);
}

void RunScenarioPutCommitReopen(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: put -> commit -> reopen");
  auto store = waxcpp::WaxStore::Create(path);

  const std::vector<std::byte> payload = {
      std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
  };
  const auto frame_id = store.Put(payload);
  Require(frame_id == 0, "first frame_id must be 0");
  Require(store.Stats().pending_frames == 1, "pending_frames must increment after put");

  store.Commit();
  auto stats = store.Stats();
  Require(stats.frame_count == 1, "frame_count must be 1 after commit");
  Require(stats.pending_frames == 0, "pending_frames must reset after commit");
  Require(stats.generation > 0, "generation must advance after commit");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  auto reopened_stats = reopened.Stats();
  Require(reopened_stats.frame_count == 1, "reopened frame_count must be 1");
  Require(reopened_stats.pending_frames == 0, "reopened pending_frames must be 0");
  reopened.Close();
}

void RunScenarioPutBatchContracts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putBatch contracts");
  auto store = waxcpp::WaxStore::Create(path);

  const std::vector<std::vector<std::byte>> payloads = {
      {std::byte{0x11}},
      {std::byte{0x12}, std::byte{0x13}},
      {std::byte{0x14}, std::byte{0x15}, std::byte{0x16}},
  };

  const auto ids = store.PutBatch(payloads, {});
  Require(ids.size() == payloads.size(), "PutBatch must return id for each payload");
  Require(ids[0] == 0 && ids[1] == 1 && ids[2] == 2, "PutBatch must allocate dense monotonic frame ids");
  Require(store.Stats().pending_frames == payloads.size(), "PutBatch must stage all mutations as pending");
  store.Commit();
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  Require(reopened.Stats().frame_count == payloads.size(), "PutBatch commit must persist all frames");
  reopened.Close();

  bool threw = false;
  try {
    auto mismatch_store = waxcpp::WaxStore::Open(path);
    const std::vector<waxcpp::Metadata> mismatched_metadata = {
        {{"k", "v"}},
    };
    (void)mismatch_store.PutBatch(payloads, mismatched_metadata);
    mismatch_store.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutBatch must reject metadata size mismatch");
}

void RunScenarioPutEmbeddingContracts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding contracts");
  auto store = waxcpp::WaxStore::Create(path);
  const auto frame_id = store.Put({std::byte{0xE1}});
  Require(frame_id == 0, "expected initial frame id for putEmbedding contract scenario");
  const auto pre_stats = store.Stats();
  const auto pre_wal = store.WalStats();

  store.PutEmbedding(frame_id, {0.25F, 0.50F, 0.75F});
  const auto staged_stats = store.Stats();
  const auto staged_wal = store.WalStats();
  Require(staged_stats.pending_frames == pre_stats.pending_frames,
          "putEmbedding must not affect pending_frames counter");
  Require(staged_wal.last_seq > pre_wal.last_seq, "putEmbedding must append WAL mutation");
  Require(staged_wal.pending_embedding_mutations == 1,
          "putEmbedding must increase pending_embedding_mutations");

  store.Commit();
  const auto after_commit = store.Stats();
  const auto after_commit_wal = store.WalStats();
  Require(after_commit.frame_count == pre_stats.frame_count + 1,
          "commit should persist the one staged putFrame alongside putEmbedding");
  Require(after_commit.pending_frames == 0, "commit should clear pending WAL state");
  Require(after_commit_wal.committed_seq >= staged_wal.last_seq,
          "commit should checkpoint putEmbedding WAL sequence");
  Require(after_commit_wal.pending_embedding_mutations == 0,
          "commit should clear pending_embedding_mutations");
  store.Close();

  bool threw = false;
  try {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.PutEmbeddingBatch({frame_id, frame_id}, {{0.1F}, {0.2F, 0.3F}});
    reopened.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutEmbeddingBatch must reject mixed embedding dimensions");

  threw = false;
  try {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.PutEmbeddingBatch({frame_id, frame_id}, {{0.1F}});
    reopened.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutEmbeddingBatch must reject frame_ids/vectors size mismatch");

  threw = false;
  try {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.PutEmbedding(frame_id, {});
    reopened.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutEmbedding must reject empty vector");
}

void RunScenarioPendingEmbeddingSnapshot(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending embedding snapshot");
  auto store = waxcpp::WaxStore::Create(path);
  const auto frame_ids = store.PutBatch(
      {{std::byte{0xD1}}, {std::byte{0xD2}}, {std::byte{0xD3}}},
      {});
  Require(frame_ids.size() == 3, "expected 3 frame ids in pending embedding snapshot scenario");
  store.Commit();

  store.PutEmbedding(frame_ids[0], {0.1F, 0.2F});
  store.PutEmbeddingBatch({frame_ids[1], frame_ids[2]}, {{0.3F, 0.4F}, {0.5F, 0.6F}});

  const auto snapshot = store.PendingEmbeddingMutations();
  const auto wal_stats = store.WalStats();
  Require(snapshot.embeddings.size() == 3, "expected three pending embeddings");
  Require(snapshot.latest_sequence.has_value(), "expected latest_sequence for pending embeddings");
  Require(wal_stats.pending_embedding_mutations == 3,
          "pending_embedding_mutations should match pending embedding snapshot size");
  bool has_frame0 = false;
  bool has_frame1 = false;
  bool has_frame2 = false;
  bool has_expected_value = false;
  for (const auto& embedding : snapshot.embeddings) {
    if (embedding.frame_id == frame_ids[0]) {
      has_frame0 = true;
      if (embedding.dimension == 2 &&
          embedding.vector.size() == 2 &&
          std::fabs(embedding.vector[0] - 0.1F) < 1e-6F &&
          std::fabs(embedding.vector[1] - 0.2F) < 1e-6F) {
        has_expected_value = true;
      }
    } else if (embedding.frame_id == frame_ids[1]) {
      has_frame1 = true;
    } else if (embedding.frame_id == frame_ids[2]) {
      has_frame2 = true;
    }
  }
  Require(has_frame0 && has_frame1 && has_frame2, "pending embedding snapshot missing expected frame ids");
  Require(has_expected_value, "pending embedding snapshot missing expected vector payload for first frame");

  const auto filtered = store.PendingEmbeddingMutations(snapshot.latest_sequence);
  Require(filtered.embeddings.empty(), "since=latest_sequence should return empty pending embedding set");
  Require(filtered.latest_sequence == snapshot.latest_sequence,
          "latest_sequence should still reflect current pending embedding head");

  store.Commit();
  const auto after_commit = store.PendingEmbeddingMutations();
  const auto after_commit_wal = store.WalStats();
  Require(after_commit.embeddings.empty(), "commit should clear pending embedding mutations");
  Require(!after_commit.latest_sequence.has_value(), "latest_sequence should be empty after embedding commit");
  Require(after_commit_wal.pending_embedding_mutations == 0,
          "commit should reset pending_embedding_mutations");
  store.Close();
}

void RunScenarioPendingEmbeddingSnapshotReopenRecovery(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending embedding snapshot survives reopen recovery");
  std::uint64_t persisted_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    persisted_frame_id = store.Put({std::byte{0xE2}});
    store.Commit();
    store.PutEmbedding(persisted_frame_id, {1.25F, 2.5F});
    // Simulate crash: no explicit Close()/Commit().
  }

  std::optional<std::uint64_t> first_latest{};
  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    Require(stats.frame_count == 1, "reopen should preserve previously committed frame_count");
    Require(stats.pending_frames == 0, "embedding-only pending should not affect pending_frames counter");

    const auto snapshot = reopened.PendingEmbeddingMutations();
    const auto wal_stats = reopened.WalStats();
    Require(snapshot.embeddings.size() == 1, "expected one recovered pending embedding");
    Require(snapshot.latest_sequence.has_value(), "expected latest sequence for recovered pending embedding");
    Require(wal_stats.pending_embedding_mutations == 1,
            "reopen should restore pending_embedding_mutations from WAL state");
    Require(snapshot.embeddings[0].frame_id == persisted_frame_id, "unexpected recovered pending embedding frame_id");
    Require(snapshot.embeddings[0].vector.size() == 2, "unexpected recovered embedding vector size");
    first_latest = snapshot.latest_sequence;

    // Recovered-only pending state must not auto-commit on close.
    reopened.Close();
  }

  {
    auto reopened_again = waxcpp::WaxStore::Open(path);
    const auto snapshot = reopened_again.PendingEmbeddingMutations();
    Require(snapshot.embeddings.size() == 1, "recovered pending embedding must remain after close");
    Require(snapshot.latest_sequence == first_latest, "recovered pending embedding sequence must remain stable");
    reopened_again.Commit();
    const auto after_commit = reopened_again.PendingEmbeddingMutations();
    const auto after_commit_wal = reopened_again.WalStats();
    Require(after_commit.embeddings.empty(), "commit should clear recovered pending embedding");
    Require(after_commit_wal.pending_embedding_mutations == 0,
            "commit should clear recovered pending_embedding_mutations");
    reopened_again.Close();
  }
}

void RunScenarioPendingEmbeddingSnapshotUsesInMemoryCache(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending embedding snapshot uses in-memory cache");
  auto store = waxcpp::WaxStore::Create(path);
  const auto frame_id = store.Put({std::byte{0xE7}});
  store.Commit();
  store.PutEmbedding(frame_id, {9.1F, 9.2F});

  const auto before = store.PendingEmbeddingMutations();
  const auto wal_before = store.WalStats();
  Require(before.embeddings.size() == 1, "expected one pending embedding before wal corruption");
  Require(before.latest_sequence.has_value(), "expected latest_sequence before wal corruption");
  Require(wal_before.pending_embedding_mutations == 1, "expected one pending embedding mutation before wal corruption");
  Require(wal_before.pending_bytes > 0, "expected pending WAL bytes before wal corruption");

  // Corrupt the first pending WAL record header after state has been materialized in-memory.
  const auto corrupt_offset = waxcpp::core::mv2s::kWalOffset + wal_before.checkpoint_pos;
  const std::array<std::byte, 1> corrupt = {std::byte{0xFF}};
  WriteBytesAt(path, corrupt_offset, std::span<const std::byte>(corrupt.data(), corrupt.size()));

  const auto after = store.PendingEmbeddingMutations();
  Require(after.embeddings.size() == 1,
          "pending embedding snapshot should remain stable from in-memory cache after wal corruption");
  Require(after.latest_sequence == before.latest_sequence,
          "pending embedding latest_sequence should remain stable from in-memory cache");
  Require(after.embeddings[0].frame_id == before.embeddings[0].frame_id,
          "pending embedding frame_id should remain stable from in-memory cache");
  Require(after.embeddings[0].dimension == before.embeddings[0].dimension,
          "pending embedding dimension should remain stable from in-memory cache");
  Require(after.embeddings[0].vector == before.embeddings[0].vector,
          "pending embedding payload should remain stable from in-memory cache");

  store.Close();
  auto reopened = waxcpp::WaxStore::Open(path);
  const auto reopened_stats = reopened.Stats();
  const auto reopened_wal = reopened.WalStats();
  Require(reopened_stats.frame_count == 1, "corrupted pending WAL should still allow close auto-commit via in-memory cache");
  Require(reopened_stats.pending_frames == 0, "close auto-commit should clear pending_frames");
  Require(reopened_wal.pending_embedding_mutations == 0, "close auto-commit should clear pending embedding counter");
  reopened.Close();
}

void RunScenarioCloseAutoCommitsLocalEmbeddingMutations(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: close auto-commits local embedding mutations");
  std::uint64_t frame_id = 0;
  waxcpp::WaxWALStats closed_wal_stats{};
  {
    auto store = waxcpp::WaxStore::Create(path);
    frame_id = store.Put({std::byte{0xE5}});
    store.Commit();
    store.PutEmbedding(frame_id, {0.11F, 0.22F});
    Require(store.WalStats().pending_embedding_mutations == 1,
            "expected one local pending embedding before close");
    store.Close();
    closed_wal_stats = store.WalStats();
  }
  Require(closed_wal_stats.auto_commit_count == 1,
          "Close() should auto-commit local embedding mutation");
  Require(closed_wal_stats.pending_embedding_mutations == 0,
          "Close() auto-commit should clear pending embedding count");

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto snapshot = reopened.PendingEmbeddingMutations();
    Require(snapshot.embeddings.empty(), "reopen after close auto-commit should have no pending embeddings");
    reopened.Close();
  }
}

void RunScenarioRecoveredPendingEmbeddingPlusLocalMutationCloseCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recovered pending embedding plus local mutation close-commit");
  std::uint64_t base_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    base_frame_id = store.Put({std::byte{0xD1}});
    store.Commit();
    store.PutEmbedding(base_frame_id, {0.31F, 0.62F});
    // Simulate crash: leave recovered pending embedding in WAL.
  }

  waxcpp::WaxWALStats closed_wal{};
  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto pending_before = reopened.PendingEmbeddingMutations();
    Require(pending_before.embeddings.size() == 1, "expected one recovered pending embedding before local mutation");
    Require(reopened.WalStats().pending_embedding_mutations == 1,
            "expected pending embedding count before local mutation");

    const auto new_frame_id = reopened.Put({std::byte{0xD2}});
    Require(new_frame_id == base_frame_id + 1, "local put frame id should advance from committed baseline");

    reopened.Close();
    closed_wal = reopened.WalStats();
  }

  Require(closed_wal.auto_commit_count == 1,
          "Close() should auto-commit when local mutation is mixed with recovered pending embedding");
  Require(closed_wal.pending_embedding_mutations == 0,
          "Close() auto-commit should clear recovered pending embedding state");

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    Require(stats.frame_count == 2, "mixed recovered pending embedding + local put should commit local frame");
    const auto pending_after = reopened.PendingEmbeddingMutations();
    Require(pending_after.embeddings.empty(),
            "mixed close auto-commit should leave no pending embedding mutations after reopen");
    reopened.Close();
  }
}

void RunScenarioLocalMixedPendingCorruptWalCloseCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: local mixed pending survives wal corruption via in-memory cache");
  waxcpp::WaxWALStats closed_wal{};
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto id0 = store.Put({std::byte{0xA0}});
    const auto id1 = store.Put({std::byte{0xA1}});
    const auto id2 = store.Put({std::byte{0xA2}});
    store.Commit();

    const auto id3 = store.Put({std::byte{0xA3}});
    Require(id3 == 3, "expected dense id for local pending put");
    store.Delete(id0);
    store.Supersede(id1, id2);
    store.PutEmbedding(id2, {7.0F, 8.0F});

    const auto before = store.WalStats();
    Require(before.pending_bytes > 0, "expected pending WAL bytes before corruption");
    Require(before.pending_delete_mutations == 1, "expected one pending delete before corruption");
    Require(before.pending_supersede_mutations == 1, "expected one pending supersede before corruption");
    Require(before.pending_embedding_mutations == 1, "expected one pending embedding before corruption");
    Require(store.Stats().pending_frames == 1, "expected one pending put before corruption");

    const auto corrupt_offset = waxcpp::core::mv2s::kWalOffset + before.checkpoint_pos;
    const std::array<std::byte, 1> corrupt = {std::byte{0xFF}};
    WriteBytesAt(path, corrupt_offset, std::span<const std::byte>(corrupt.data(), corrupt.size()));

    store.Close();
    closed_wal = store.WalStats();
  }

  Require(closed_wal.auto_commit_count == 1, "close should auto-commit local mixed pending state");
  Require(closed_wal.pending_delete_mutations == 0, "close auto-commit should clear pending delete counter");
  Require(closed_wal.pending_supersede_mutations == 0, "close auto-commit should clear pending supersede counter");
  Require(closed_wal.pending_embedding_mutations == 0, "close auto-commit should clear pending embedding counter");

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 4, "close auto-commit should persist pending put despite wal corruption");
  Require(stats.pending_frames == 0, "no pending frames expected after close auto-commit");
  const auto toc = ReadCommittedToc(path);
  Require(toc.frames.size() == 4, "expected four committed frames after mixed close auto-commit");
  Require(toc.frames[0].status == 1, "pending delete must be applied during close auto-commit");
  Require(toc.frames[1].superseded_by.has_value() && *toc.frames[1].superseded_by == 2,
          "pending supersede must set superseded_by edge");
  Require(toc.frames[2].supersedes.has_value() && *toc.frames[2].supersedes == 1,
          "pending supersede must set supersedes edge");
  reopened.Close();
}

void RunScenarioRecoveredPendingDeletePlusLocalMutationCloseCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recovered pending delete plus local mutation close-commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0xA1}});
    (void)store.Put({std::byte{0xA2}});
    store.Commit();
    store.Delete(0);
    // Simulate crash: recovered pending delete stays in WAL.
  }

  waxcpp::WaxWALStats closed_wal{};
  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats_before = reopened.Stats();
    Require(stats_before.frame_count == 2, "expected committed frame_count before local mutation");
    const auto local_id = reopened.Put({std::byte{0xA3}});
    Require(local_id == 2, "local put should continue dense ids with recovered non-put pending mutations");
    reopened.Close();
    closed_wal = reopened.WalStats();
  }
  Require(closed_wal.auto_commit_count == 1,
          "Close() should auto-commit when local mutation is mixed with recovered pending delete");

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto toc = ReadCommittedToc(path);
  Require(toc.frames.size() == 3, "expected three committed frames after mixed delete+put close-commit");
  Require(toc.frames[0].status == 1, "recovered pending delete must be applied during mixed close-commit");
  Require(toc.frames[1].status == 0, "unrelated frame status should remain active");
  Require(toc.frames[2].status == 0, "new local frame should be active");
  reopened.Close();
}

void RunScenarioRecoveredPendingSupersedePlusLocalMutationCloseCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recovered pending supersede plus local mutation close-commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0xB1}});
    (void)store.Put({std::byte{0xB2}});
    store.Commit();
    store.Supersede(0, 1);
    // Simulate crash: recovered pending supersede stays in WAL.
  }

  waxcpp::WaxWALStats closed_wal{};
  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto local_id = reopened.Put({std::byte{0xB3}});
    Require(local_id == 2, "local put should continue dense ids after recovered pending supersede");
    reopened.Close();
    closed_wal = reopened.WalStats();
  }
  Require(closed_wal.auto_commit_count == 1,
          "Close() should auto-commit when local mutation is mixed with recovered pending supersede");

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto toc = ReadCommittedToc(path);
  Require(toc.frames.size() == 3, "expected three committed frames after mixed supersede+put close-commit");
  Require(toc.frames[0].superseded_by.has_value() && *toc.frames[0].superseded_by == 1,
          "recovered pending supersede must set superseded_by edge");
  Require(toc.frames[1].supersedes.has_value() && *toc.frames[1].supersedes == 0,
          "recovered pending supersede must set supersedes edge");
  Require(toc.frames[2].status == 0, "new local frame should be active");
  reopened.Close();
}

void RunScenarioPutEmbeddingUnknownFrameRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding unknown frame rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    store.PutEmbedding(999, {0.9F, 1.1F});
    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject putEmbedding targeting unknown frame_id");
    // Simulate abrupt stop; avoid Close() auto-commit retry path.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 0, "rejected putEmbedding commit must not change frame_count");
  reopened.Close();
}

void RunScenarioPutEmbeddingBatchUnknownFrameRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding batch with unknown frame rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto valid_frame = store.Put({std::byte{0xE3}});
    store.PutEmbeddingBatch({valid_frame, 777}, {{0.2F, 0.4F}, {0.6F, 0.8F}});
    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject putEmbeddingBatch containing unknown frame_id");
    // Simulate abrupt stop; avoid Close() auto-commit retry path.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 0, "rejected putEmbeddingBatch commit must not change frame_count");
  reopened.Close();
}

void RunScenarioPutEmbeddingForwardReferenceRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding forward reference rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    // Sequence order is important: putEmbedding(frame=0) comes before putFrame(frame=0).
    store.PutEmbedding(0, {0.7F, 0.9F});
    (void)store.Put({std::byte{0xE4}});
    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject forward-reference putEmbedding that precedes putFrame");
    // Simulate abrupt stop; avoid Close() auto-commit retry path.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 0, "forward-reference reject must keep committed frame_count unchanged");
  reopened.Close();
}

void RunScenarioPendingRecoveryCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending WAL recovery then commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    };
    (void)store.Put(payload);
    // Simulate crash/no-graceful-close: do not call Close() so WAL remains pending.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  auto stats_before = reopened.Stats();
  Require(stats_before.frame_count == 0, "uncommitted put must not change committed frame_count");
  Require(stats_before.pending_frames == 1, "pending put must be visible after reopen");
  reopened.Commit();
  auto stats_after = reopened.Stats();
  Require(stats_after.frame_count == 1, "frame_count must be 1 after committing recovered pending put");
  Require(stats_after.pending_frames == 0, "pending_frames must be 0 after commit");
  reopened.Close();
}

void RunScenarioPendingRecoverySkipsUndecodableTail(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending WAL recovery skips undecodable tail record");
  const std::vector<std::byte> payload = {
      std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
  };
  waxcpp::WaxWALStats crashed_wal{};

  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put(payload);
    crashed_wal = store.WalStats();
    // Simulate abrupt crash: leave one valid pending record in WAL.
  }

  Require(crashed_wal.last_seq >= 1, "expected wal last_seq >= 1 after pending put");
  Require(crashed_wal.wal_size > 0, "expected non-zero wal_size");

  {
    waxcpp::core::wal::WalRingWriter writer(path,
                                            waxcpp::core::mv2s::kWalOffset,
                                            crashed_wal.wal_size,
                                            crashed_wal.write_pos,
                                            crashed_wal.checkpoint_pos,
                                            crashed_wal.pending_bytes,
                                            crashed_wal.last_seq);
    const std::vector<std::byte> unknown_opcode_payload = {std::byte{0xFF}};
    (void)writer.Append(unknown_opcode_payload);
    // Do not publish updated header state to simulate torn process after WAL append.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto before = reopened.Stats();
  const auto before_wal = reopened.WalStats();
  Require(before.frame_count == 0, "undecodable WAL tail must not change committed frame_count");
  Require(before.pending_frames == 1, "only decodable pending putFrame should be exposed");
  Require(before_wal.last_seq >= 2, "scan state should advance through undecodable tail record");

  reopened.Commit();
  const auto after = reopened.Stats();
  Require(after.frame_count == 1, "commit should apply decodable pending putFrame");
  Require(after.pending_frames == 0, "pending WAL state should clear after commit");
  Require(reopened.FrameContent(0) == payload, "committed frame payload mismatch after decode-stop recovery");
  reopened.Close();
}

void RunScenarioPendingRecoveryIgnoresPartialTail(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending WAL recovery ignores partial/corrupt tail");
  const std::vector<std::byte> payload = {
      std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F},
  };
  waxcpp::WaxWALStats crashed_wal{};

  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put(payload);
    crashed_wal = store.WalStats();
    // Simulate abrupt crash with one valid pending putFrame.
  }

  Require(crashed_wal.last_seq >= 1, "expected wal last_seq >= 1 before partial-tail injection");
  Require(crashed_wal.pending_bytes > 0, "expected pending WAL bytes before partial-tail injection");

  // Overwrite the sentinel at write_pos with an intentionally partial/corrupt next header prefix:
  // - sequence = last_seq + 1 (non-zero)
  // - length = 16
  // Remaining header bytes stay zero (truncated-style torn write).
  std::array<std::byte, 12> partial_prefix{};
  const auto next_seq = crashed_wal.last_seq + 1;
  for (std::size_t i = 0; i < 8; ++i) {
    partial_prefix[i] = static_cast<std::byte>((next_seq >> (8U * i)) & 0xFFU);
  }
  constexpr std::uint32_t kClaimedLength = 16;
  for (std::size_t i = 0; i < 4; ++i) {
    partial_prefix[8 + i] = static_cast<std::byte>((kClaimedLength >> (8U * i)) & 0xFFU);
  }

  const auto inject_offset = waxcpp::core::mv2s::kWalOffset + crashed_wal.write_pos;
  WriteBytesAt(path, inject_offset, partial_prefix);

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto before = reopened.Stats();
  const auto before_wal = reopened.WalStats();
  Require(before.frame_count == 0, "partial tail must not change committed frame_count");
  Require(before.pending_frames == 1, "partial tail must preserve valid pending putFrame");
  Require(before_wal.last_seq == crashed_wal.last_seq,
          "partial/corrupt tail must not advance scanned wal last_seq");
  Require(before_wal.pending_bytes == crashed_wal.pending_bytes,
          "partial/corrupt tail must not inflate scanned pending_bytes");
  Require(before_wal.write_pos == crashed_wal.write_pos,
          "partial/corrupt tail must keep write_pos at first invalid header");

  reopened.Commit();
  const auto after = reopened.Stats();
  Require(after.frame_count == 1, "commit should apply valid pending putFrame despite partial tail");
  Require(after.pending_frames == 0, "pending state should clear after commit");
  Require(reopened.FrameContent(0) == payload, "committed payload mismatch after partial-tail recovery");
  reopened.Close();
}

void RunScenarioPendingLifecycleRecoverySkipsUndecodableTail(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending lifecycle recovery skips undecodable tail record");
  waxcpp::WaxWALStats crashed_wal{};
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0x81}});
    (void)store.Put({std::byte{0x82}});
    store.Commit();
    store.Delete(0);
    crashed_wal = store.WalStats();
    // Simulate crash with one valid pending delete.
  }

  {
    waxcpp::core::wal::WalRingWriter writer(path,
                                            waxcpp::core::mv2s::kWalOffset,
                                            crashed_wal.wal_size,
                                            crashed_wal.write_pos,
                                            crashed_wal.checkpoint_pos,
                                            crashed_wal.pending_bytes,
                                            crashed_wal.last_seq);
    const std::vector<std::byte> unknown_opcode_payload = {std::byte{0xFF}};
    (void)writer.Append(unknown_opcode_payload);
    // Do not publish updated header state to simulate torn process after WAL append.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto before_stats = reopened.Stats();
  const auto before_wal = reopened.WalStats();
  Require(before_stats.frame_count == 2, "undecodable tail should not alter committed frame_count");
  Require(before_stats.pending_frames == 0, "delete-only pending mutation should not increase pending put counter");
  Require(before_wal.pending_delete_mutations == 1,
          "recovered pending delete counter should retain decodable lifecycle mutation");
  Require(before_wal.pending_supersede_mutations == 0,
          "recovered pending supersede counter should remain zero");
  Require(before_wal.last_seq >= crashed_wal.last_seq + 1,
          "scan state should advance through undecodable lifecycle tail record");

  reopened.Commit();
  const auto after_wal = reopened.WalStats();
  const auto toc = ReadCommittedToc(path);
  Require(toc.frames.size() == 2, "commit should preserve two committed frames");
  Require(toc.frames[0].status == 1, "commit should apply recovered pending delete despite undecodable tail");
  Require(after_wal.pending_delete_mutations == 0, "commit should clear pending delete counter");
  reopened.Close();
}

void RunScenarioPendingMixedLifecycleReplayDeterminism(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending mixed lifecycle replay determinism");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0x91}});
    (void)store.Put({std::byte{0x92}});
    store.Commit();

    // Stage a deterministic mixed sequence in WAL without committing:
    // put(2), delete(0), supersede(1->2), putEmbedding(1), put(3), supersede(2->3), delete(3)
    const auto id2 = store.Put({std::byte{0x93}});
    Require(id2 == 2, "expected dense id for first pending put");
    store.Delete(0);
    store.Supersede(1, 2);
    store.PutEmbedding(1, {0.25F, 0.5F, 0.75F});
    const auto id3 = store.Put({std::byte{0x94}});
    Require(id3 == 3, "expected dense id for second pending put");
    store.Supersede(2, 3);
    store.Delete(3);
    // Simulate crash: no Close()/Commit().
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    const auto wal = reopened.WalStats();
    Require(stats.frame_count == 2, "reopen should keep committed frame_count before replay commit");
    Require(stats.pending_frames == 2, "reopen should detect two pending put mutations");
    Require(wal.pending_delete_mutations == 2, "reopen should detect two pending delete mutations");
    Require(wal.pending_supersede_mutations == 2, "reopen should detect two pending supersede mutations");
    Require(wal.pending_embedding_mutations == 1, "reopen should detect one pending embedding mutation");
    reopened.Commit();

    const auto after_wal = reopened.WalStats();
    Require(after_wal.pending_delete_mutations == 0, "commit should clear pending delete counter");
    Require(after_wal.pending_supersede_mutations == 0, "commit should clear pending supersede counter");
    Require(after_wal.pending_embedding_mutations == 0, "commit should clear pending embedding counter");

    const auto toc = ReadCommittedToc(path);
    Require(toc.frames.size() == 4, "commit should apply both pending put mutations");
    Require(toc.frames[0].status == 1, "frame 0 should be deleted");
    Require(toc.frames[3].status == 1, "frame 3 should be deleted");
    Require(toc.frames[1].superseded_by.has_value() && *toc.frames[1].superseded_by == 2,
            "frame 1 superseded_by edge mismatch");
    Require(toc.frames[2].supersedes.has_value() && *toc.frames[2].supersedes == 1,
            "frame 2 supersedes edge mismatch");
    Require(toc.frames[2].superseded_by.has_value() && *toc.frames[2].superseded_by == 3,
            "frame 2 superseded_by edge mismatch");
    Require(toc.frames[3].supersedes.has_value() && *toc.frames[3].supersedes == 2,
            "frame 3 supersedes edge mismatch");
    reopened.Close();
  }
}

void RunScenarioMixedRecoveredAndLocalReplayAcrossReopenCycles(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: mixed recovered+local replay across reopen cycles");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto ids = store.PutBatch(
        {{std::byte{0xA1}}, {std::byte{0xA2}}, {std::byte{0xA3}}},
        {});
    Require(ids.size() == 3 && ids[0] == 0 && ids[1] == 1 && ids[2] == 2, "expected dense baseline ids");
    store.Commit();

    const auto id3 = store.Put({std::byte{0xA4}});
    Require(id3 == 3, "expected dense pending id3 in first cycle");
    store.Delete(0);
    store.Supersede(1, 2);
    store.PutEmbedding(2, {0.11F, 0.22F});
    // Crash simulation: no close/commit.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto before_stats = reopened.Stats();
    const auto before_wal = reopened.WalStats();
    Require(before_stats.pending_frames == 1, "reopen must recover one pending put in first cycle");
    Require(before_wal.pending_delete_mutations == 1, "reopen must recover one pending delete in first cycle");
    Require(before_wal.pending_supersede_mutations == 1, "reopen must recover one pending supersede in first cycle");
    Require(before_wal.pending_embedding_mutations == 1, "reopen must recover one pending embedding in first cycle");

    const auto id4 = reopened.Put({std::byte{0xA5}});
    Require(id4 == 4, "expected dense local id4 in mixed first cycle");
    reopened.Supersede(2, id4);
    reopened.PutEmbedding(id4, {0.33F, 0.44F});
    reopened.Commit();
    reopened.Close();
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    const auto wal = reopened.WalStats();
    Require(stats.frame_count == 5, "first mixed replay commit must persist five frames");
    Require(stats.pending_frames == 0, "first mixed replay commit must clear pending put state");
    Require(wal.pending_delete_mutations == 0, "first mixed replay commit must clear pending delete counter");
    Require(wal.pending_supersede_mutations == 0, "first mixed replay commit must clear pending supersede counter");
    Require(wal.pending_embedding_mutations == 0, "first mixed replay commit must clear pending embedding counter");

    const auto toc = ReadCommittedToc(path);
    Require(toc.frames.size() == 5, "first mixed replay commit TOC frame count mismatch");
    Require(toc.frames[0].status == 1, "first mixed replay commit should apply delete on frame 0");
    Require(toc.frames[1].superseded_by.has_value() && *toc.frames[1].superseded_by == 2,
            "first mixed replay commit should apply supersede edge 1<-2");
    Require(toc.frames[2].supersedes.has_value() && *toc.frames[2].supersedes == 1,
            "first mixed replay commit should apply supersede edge 2->1");
    Require(toc.frames[2].superseded_by.has_value() && *toc.frames[2].superseded_by == 4,
            "first mixed replay commit should apply supersede edge 2<-4");
    Require(toc.frames[4].supersedes.has_value() && *toc.frames[4].supersedes == 2,
            "first mixed replay commit should apply supersede edge 4->2");

    reopened.Delete(4);
    reopened.PutEmbedding(2, {0.55F, 0.66F});
    const auto id5 = reopened.Put({std::byte{0xA6}});
    Require(id5 == 5, "expected dense pending id5 in second cycle");
    // Crash simulation: no close/commit.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto before_stats = reopened.Stats();
    const auto before_wal = reopened.WalStats();
    Require(before_stats.pending_frames == 1, "second-cycle reopen must recover one pending put");
    Require(before_wal.pending_delete_mutations == 1, "second-cycle reopen must recover one pending delete");
    Require(before_wal.pending_embedding_mutations == 1, "second-cycle reopen must recover one pending embedding");
    Require(before_wal.pending_supersede_mutations == 0, "second-cycle reopen should start with zero supersede");

    reopened.Supersede(3, 5);
    reopened.Commit();
    reopened.Close();
  }

  {
    auto final_store = waxcpp::WaxStore::Open(path);
    const auto final_stats = final_store.Stats();
    const auto final_wal = final_store.WalStats();
    Require(final_stats.frame_count == 6, "second mixed replay commit must persist six frames");
    Require(final_stats.pending_frames == 0, "second mixed replay commit must clear pending put state");
    Require(final_wal.pending_delete_mutations == 0, "second mixed replay commit must clear pending delete counter");
    Require(final_wal.pending_supersede_mutations == 0,
            "second mixed replay commit must clear pending supersede counter");
    Require(final_wal.pending_embedding_mutations == 0,
            "second mixed replay commit must clear pending embedding counter");

    const auto toc = ReadCommittedToc(path);
    Require(toc.frames.size() == 6, "second mixed replay commit TOC frame count mismatch");
    Require(toc.frames[4].status == 1, "second mixed replay commit should apply delete on frame 4");
    Require(toc.frames[3].superseded_by.has_value() && *toc.frames[3].superseded_by == 5,
            "second mixed replay commit should apply supersede edge 3<-5");
    Require(toc.frames[5].supersedes.has_value() && *toc.frames[5].supersedes == 3,
            "second mixed replay commit should apply supersede edge 5->3");
    final_store.Close();
  }
}

void RunScenarioRecoveredMixedLifecycleCorruptWalAcrossReopenCycles(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recovered mixed lifecycle survives wal corruption across reopen cycles");

  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto ids = store.PutBatch(
        {{std::byte{0xB0}}, {std::byte{0xB1}}, {std::byte{0xB2}}, {std::byte{0xB3}}},
        {});
    Require(ids.size() == 4 && ids[0] == 0 && ids[1] == 1 && ids[2] == 2 && ids[3] == 3,
            "expected dense baseline ids for recovered mixed lifecycle scenario");
    store.Commit();

    const auto id4 = store.Put({std::byte{0xB4}});
    Require(id4 == 4, "expected dense recovered pending put id4");
    store.Delete(0);
    store.Supersede(1, 2);
    store.PutEmbedding(2, {1.1F, 1.2F, 1.3F});
    // Crash simulation: recovered pending set remains in WAL.
  }

  waxcpp::WaxWALStats cycle1_closed_wal{};
  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto before_stats = reopened.Stats();
    const auto before_wal = reopened.WalStats();
    Require(before_stats.pending_frames == 1, "cycle1 reopen must recover one pending put");
    Require(before_wal.pending_delete_mutations == 1, "cycle1 reopen must recover one pending delete");
    Require(before_wal.pending_supersede_mutations == 1, "cycle1 reopen must recover one pending supersede");
    Require(before_wal.pending_embedding_mutations == 1, "cycle1 reopen must recover one pending embedding");
    Require(before_wal.pending_bytes > 0, "cycle1 reopen must expose pending wal bytes");

    const auto corrupt_offset = waxcpp::core::mv2s::kWalOffset + before_wal.checkpoint_pos;
    const std::array<std::byte, 1> corrupt = {std::byte{0xFF}};
    WriteBytesAt(path, corrupt_offset, std::span<const std::byte>(corrupt.data(), corrupt.size()));

    const auto id5 = reopened.Put({std::byte{0xB5}});
    Require(id5 == 5, "cycle1 local put must continue dense ids after recovered pending");
    reopened.Delete(3);
    reopened.Supersede(2, id5);
    reopened.PutEmbedding(id5, {1.5F, 1.6F});
    reopened.Close();
    cycle1_closed_wal = reopened.WalStats();
  }

  Require(cycle1_closed_wal.auto_commit_count == 1,
          "cycle1 close should auto-commit mixed recovered+local state");
  Require(cycle1_closed_wal.pending_delete_mutations == 0,
          "cycle1 close auto-commit should clear pending delete counter");
  Require(cycle1_closed_wal.pending_supersede_mutations == 0,
          "cycle1 close auto-commit should clear pending supersede counter");
  Require(cycle1_closed_wal.pending_embedding_mutations == 0,
          "cycle1 close auto-commit should clear pending embedding counter");

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    Require(stats.frame_count == 6, "cycle1 close auto-commit must persist six frames");
    Require(stats.pending_frames == 0, "cycle1 close auto-commit must clear pending puts");

    const auto toc = ReadCommittedToc(path);
    Require(toc.frames.size() == 6, "cycle1 TOC frame count mismatch");
    Require(toc.frames[0].status == 1, "cycle1 should apply recovered delete on frame 0");
    Require(toc.frames[3].status == 1, "cycle1 should apply local delete on frame 3");
    Require(toc.frames[1].superseded_by.has_value() && *toc.frames[1].superseded_by == 2,
            "cycle1 should apply recovered supersede edge 1<-2");
    Require(toc.frames[2].supersedes.has_value() && *toc.frames[2].supersedes == 1,
            "cycle1 should apply recovered supersede edge 2->1");
    Require(toc.frames[2].superseded_by.has_value() && *toc.frames[2].superseded_by == 5,
            "cycle1 should apply local supersede edge 2<-5");
    Require(toc.frames[5].supersedes.has_value() && *toc.frames[5].supersedes == 2,
            "cycle1 should apply local supersede edge 5->2");
    reopened.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    store.Delete(1);
    store.PutEmbedding(2, {2.1F, 2.2F});
    const auto id6 = store.Put({std::byte{0xB6}});
    Require(id6 == 6, "cycle2 pending put must continue dense ids");
    store.Supersede(5, id6);
    // Crash simulation: cycle2 pending mutations remain in WAL.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto before_stats = reopened.Stats();
    const auto before_wal = reopened.WalStats();
    Require(before_stats.pending_frames == 1, "cycle2 reopen must recover one pending put");
    Require(before_wal.pending_delete_mutations == 1, "cycle2 reopen must recover one pending delete");
    Require(before_wal.pending_supersede_mutations == 1, "cycle2 reopen must recover one pending supersede");
    Require(before_wal.pending_embedding_mutations == 1, "cycle2 reopen must recover one pending embedding");

    const auto corrupt_offset = waxcpp::core::mv2s::kWalOffset + before_wal.checkpoint_pos;
    const std::array<std::byte, 1> corrupt = {std::byte{0xFF}};
    WriteBytesAt(path, corrupt_offset, std::span<const std::byte>(corrupt.data(), corrupt.size()));

    reopened.Commit();
    const auto after_wal = reopened.WalStats();
    Require(after_wal.pending_delete_mutations == 0, "cycle2 commit must clear pending delete counter");
    Require(after_wal.pending_supersede_mutations == 0, "cycle2 commit must clear pending supersede counter");
    Require(after_wal.pending_embedding_mutations == 0, "cycle2 commit must clear pending embedding counter");
    reopened.Close();
  }

  {
    auto final_store = waxcpp::WaxStore::Open(path);
    const auto final_stats = final_store.Stats();
    Require(final_stats.frame_count == 7, "cycle2 commit must persist seven frames");
    Require(final_stats.pending_frames == 0, "cycle2 commit must clear pending put state");

    const auto toc = ReadCommittedToc(path);
    Require(toc.frames.size() == 7, "cycle2 TOC frame count mismatch");
    Require(toc.frames[1].status == 1, "cycle2 should apply recovered delete on frame 1");
    Require(toc.frames[5].superseded_by.has_value() && *toc.frames[5].superseded_by == 6,
            "cycle2 should apply recovered supersede edge 5<-6");
    Require(toc.frames[6].supersedes.has_value() && *toc.frames[6].supersedes == 5,
            "cycle2 should apply recovered supersede edge 6->5");
    final_store.Close();
  }
}

void RunScenarioDeleteAndSupersedePersist(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: delete/supersede persist in TOC");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload_a = {std::byte{0x10}, std::byte{0x11}};
  const std::vector<std::byte> payload_b = {std::byte{0x20}, std::byte{0x21}};
  const auto id0 = store.Put(payload_a);
  const auto id1 = store.Put(payload_b);
  Require(id0 == 0 && id1 == 1, "expected dense frame ids 0,1");

  store.Supersede(id0, id1);
  store.Delete(id1);
  store.Commit();
  store.Close();

  const auto toc = ReadCommittedToc(path);
  Require(toc.frames.size() == 2, "expected two committed frames");
  Require(toc.frames[0].superseded_by.has_value(), "frame 0 must have superseded_by");
  Require(*toc.frames[0].superseded_by == 1, "frame 0 superseded_by must be 1");
  Require(toc.frames[1].supersedes.has_value(), "frame 1 must have supersedes");
  Require(*toc.frames[1].supersedes == 0, "frame 1 supersedes must be 0");
  Require(toc.frames[1].status == 1, "frame 1 status must be deleted");
}

void RunScenarioPendingLifecycleMutationCountersLocal(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending lifecycle mutation counters (local)");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0x61}});
  (void)store.Put({std::byte{0x62}});
  store.Commit();

  store.Delete(0);
  store.Supersede(0, 1);
  store.PutEmbedding(1, {0.4F, 0.8F});
  const auto staged_wal = store.WalStats();
  Require(staged_wal.pending_delete_mutations == 1,
          "local pending delete counter mismatch");
  Require(staged_wal.pending_supersede_mutations == 1,
          "local pending supersede counter mismatch");
  Require(staged_wal.pending_embedding_mutations == 1,
          "local pending embedding counter mismatch");

  store.Commit();
  const auto committed_wal = store.WalStats();
  Require(committed_wal.pending_delete_mutations == 0,
          "commit should clear pending delete counter");
  Require(committed_wal.pending_supersede_mutations == 0,
          "commit should clear pending supersede counter");
  Require(committed_wal.pending_embedding_mutations == 0,
          "commit should clear pending embedding counter");
  store.Close();
}

void RunScenarioPendingLifecycleMutationCountersRecovered(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending lifecycle mutation counters (recovered)");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0x71}});
    (void)store.Put({std::byte{0x72}});
    store.Commit();
    store.Delete(0);
    store.Supersede(0, 1);
    // Simulate crash with recovered pending lifecycle mutations.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto wal = reopened.WalStats();
    Require(wal.pending_delete_mutations == 1,
            "recovered pending delete counter mismatch");
    Require(wal.pending_supersede_mutations == 1,
            "recovered pending supersede counter mismatch");
    reopened.Close();
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto wal = reopened.WalStats();
    Require(wal.pending_delete_mutations == 1,
            "recovered pending delete counter should survive close");
    Require(wal.pending_supersede_mutations == 1,
            "recovered pending supersede counter should survive close");
    reopened.Commit();
    const auto after_commit_wal = reopened.WalStats();
    Require(after_commit_wal.pending_delete_mutations == 0,
            "commit should clear recovered pending delete counter");
    Require(after_commit_wal.pending_supersede_mutations == 0,
            "commit should clear recovered pending supersede counter");
    reopened.Close();
  }
}

void RunScenarioCrashWindowStep2WithMixedRecoveredAndLocalPending(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window step2 with mixed recovered+local pending");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto ids = store.PutBatch(
        {{std::byte{0xB1}}, {std::byte{0xB2}}, {std::byte{0xB3}}},
        {});
    Require(ids.size() == 3 && ids[0] == 0 && ids[1] == 1 && ids[2] == 2, "expected dense ids for mixed step2");
    store.Commit();

    store.Delete(0);
    const auto id3 = store.Put({std::byte{0xB4}});
    Require(id3 == 3, "expected dense pending id3 before mixed step2 crash");
    store.PutEmbedding(2, {0.1F, 0.2F, 0.3F});
    // Crash simulation: recovered pending set should be reconstructed on reopen.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto before_stats = reopened.Stats();
    const auto before_wal = reopened.WalStats();
    Require(before_stats.pending_frames == 1, "reopen should recover one pending put before mixed step2");
    Require(before_wal.pending_delete_mutations == 1, "reopen should recover one pending delete before mixed step2");
    Require(before_wal.pending_embedding_mutations == 1,
            "reopen should recover one pending embedding before mixed step2");

    reopened.Supersede(1, 2);
    reopened.PutEmbedding(3, {0.4F, 0.5F, 0.6F});
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(2);
      try {
        reopened.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at step2 for mixed recovered/local scenario");
    // Simulate crash: do not close (avoid auto-commit path masking publication semantics).
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.Verify(true);
    const auto stats = reopened.Stats();
    const auto wal = reopened.WalStats();
    Require(stats.frame_count == 4, "step2 mixed recovered/local crash should publish new frame");
    Require(stats.pending_frames == 0, "step2 mixed recovered/local crash should clear pending put state");
    Require(wal.pending_delete_mutations == 0, "step2 mixed recovered/local crash should clear delete counter");
    Require(wal.pending_supersede_mutations == 0,
            "step2 mixed recovered/local crash should clear supersede counter");
    Require(wal.pending_embedding_mutations == 0,
            "step2 mixed recovered/local crash should clear embedding counter");

    const auto frame0 = reopened.FrameMeta(0);
    const auto frame1 = reopened.FrameMeta(1);
    const auto frame2 = reopened.FrameMeta(2);
    Require(frame0.has_value() && frame1.has_value() && frame2.has_value(),
            "step2 mixed recovered/local should expose frame metas after reopen");
    Require(frame0->status == 1, "step2 mixed recovered/local should apply delete on frame 0");
    Require(frame1->superseded_by.has_value() && *frame1->superseded_by == 2,
            "step2 mixed recovered/local should apply supersede edge 1<-2");
    Require(frame2->supersedes.has_value() && *frame2->supersedes == 1,
            "step2 mixed recovered/local should apply supersede edge 2->1");
    reopened.Close();
  }
}

void RunScenarioCrashWindowAfterTocWrite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after TOC write (before footer)");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x31}};
    const std::vector<std::byte> payload1 = {std::byte{0x32}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(1);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 1");
    // Simulate crash: do not call Close() to avoid graceful auto-commit.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 1, "expected old committed frame_count after TOC-only crash");
  Require(stats.pending_frames == 1, "expected pending WAL mutation after TOC-only crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterFooterWrite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after footer write (before headers)");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x41}};
    const std::vector<std::byte> payload1 = {std::byte{0x42}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(2);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 2");
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after footer-published crash");
  Require(stats.pending_frames == 0, "expected no pending put after footer-published crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterHeaderA(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after header A write (before header B)");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x51}};
    const std::vector<std::byte> payload1 = {std::byte{0x52}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(3);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 3");
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after header-A crash");
  Require(stats.pending_frames == 0, "expected no pending put after header-A crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterCheckpointBeforeHeaders(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after checkpoint (before headers)");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x57}};
    const std::vector<std::byte> payload1 = {std::byte{0x58}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(5);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 5");
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after checkpoint-before-headers crash");
  Require(stats.pending_frames == 0, "expected no pending put after checkpoint-before-headers crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterHeaderB(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after header B write");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x53}};
    const std::vector<std::byte> payload1 = {std::byte{0x54}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(4);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 4");
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after header-B crash");
  Require(stats.pending_frames == 0, "expected no pending put after header-B crash");
  reopened.Close();
}

void RunScenarioVisibleCommitProbeStep1NoRefresh(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe does not refresh after step1 failure");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xA1}});
  store.Commit();
  (void)store.Put({std::byte{0xA2}});

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(1);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 1");

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(!refreshed, "step1 failure must not report externally visible commit");

  const auto stats_after_probe = store.Stats();
  Require(stats_after_probe.frame_count == 1, "step1 probe must keep committed frame_count unchanged");
  Require(stats_after_probe.pending_frames == 1, "step1 probe must preserve pending WAL mutations");

  store.Commit();
  const auto stats_after_retry = store.Stats();
  Require(stats_after_retry.frame_count == 2, "retry commit after step1 failure should apply pending mutation");
  Require(stats_after_retry.pending_frames == 0, "retry commit should clear pending state");
  store.Close();
}

void RunScenarioVisibleCommitProbeStep1PreservesLifecyclePendingCounters(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe step1 preserves lifecycle pending counters");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xA7}});
  (void)store.Put({std::byte{0xA8}});
  store.Commit();
  store.Delete(0);

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(1);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 1 for lifecycle pending probe");

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(!refreshed, "step1 failure must not publish lifecycle pending commit");

  const auto wal_after_probe = store.WalStats();
  Require(wal_after_probe.pending_delete_mutations == 1,
          "step1 probe must preserve pending delete counter");
  Require(wal_after_probe.pending_supersede_mutations == 0,
          "step1 probe must preserve pending supersede counter");

  store.Commit();
  const auto wal_after_retry = store.WalStats();
  Require(wal_after_retry.pending_delete_mutations == 0,
          "retry commit after step1 must clear pending delete counter");
  store.Close();
}

void RunScenarioVisibleCommitProbeStep2Refreshes(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe refreshes after step2 failure");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xB1}});
  store.Commit();
  (void)store.Put({std::byte{0xB2}});

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(2);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 2");

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(refreshed, "step2 failure should report externally visible commit");

  const auto stats_after_probe = store.Stats();
  Require(stats_after_probe.frame_count == 2, "step2 probe should refresh committed frame_count");
  Require(stats_after_probe.pending_frames == 0, "step2 probe should clear pending WAL state");
  store.Close();
}

void RunScenarioVisibleCommitProbeStep5Refreshes(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe refreshes after step5 failure");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xC1}});
  store.Commit();
  (void)store.Put({std::byte{0xC2}});

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(5);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 5");

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(refreshed, "step5 failure should report externally visible commit");

  const auto stats_after_probe = store.Stats();
  Require(stats_after_probe.frame_count == 2, "step5 probe should refresh committed frame_count");
  Require(stats_after_probe.pending_frames == 0, "step5 probe should clear pending WAL state");
  store.Close();
}

void RunScenarioVisibleCommitProbeStep3Refreshes(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe refreshes after step3 failure");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xD1}});
  store.Commit();
  (void)store.Put({std::byte{0xD2}});

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(3);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 3");

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(refreshed, "step3 failure should report externally visible commit");

  const auto stats_after_probe = store.Stats();
  Require(stats_after_probe.frame_count == 2, "step3 probe should refresh committed frame_count");
  Require(stats_after_probe.pending_frames == 0, "step3 probe should clear pending WAL state");
  store.Close();
}

void RunScenarioVisibleCommitProbeStep4Refreshes(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe refreshes after step4 failure");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xE1}});
  store.Commit();
  (void)store.Put({std::byte{0xE2}});

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(4);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 4");

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(refreshed, "step4 failure should report externally visible commit");

  const auto stats_after_probe = store.Stats();
  Require(stats_after_probe.frame_count == 2, "step4 probe should refresh committed frame_count");
  Require(stats_after_probe.pending_frames == 0, "step4 probe should clear pending WAL state");
  store.Close();
}

void RunScenarioVisibleCommitProbeNoNewGenerationNoRefresh(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe no-op when no newer generation exists");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xF1}});
  store.Commit();

  const auto before = store.Stats();
  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  const auto after = store.Stats();

  Require(!refreshed, "probe should not refresh when no newer footer generation exists");
  Require(after.frame_count == before.frame_count, "no-op probe should keep frame_count stable");
  Require(after.pending_frames == before.pending_frames, "no-op probe should keep pending state stable");
  store.Close();
}

void RunScenarioVisibleCommitProbeClosedStoreThrows(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe closed store throws");
  auto store = waxcpp::WaxStore::Create(path);
  store.Close();

  bool threw = false;
  try {
    (void)store.TryRefreshIfPublishedCommitVisible();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "probe on closed store must throw");
}

void RunScenarioVisibleCommitProbeRefreshIsIdempotent(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe refresh is idempotent");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xAB}});
  store.Commit();
  (void)store.Put({std::byte{0xAC}});

  bool threw = false;
  {
    ScopedCommitFailStep fail_step(2);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 2");

  const bool first_refresh = store.TryRefreshIfPublishedCommitVisible();
  Require(first_refresh, "first probe should refresh externally visible commit");
  const auto first_stats = store.Stats();
  Require(first_stats.frame_count == 2, "first probe should observe refreshed frame_count");
  Require(first_stats.pending_frames == 0, "first probe should clear pending state");

  const bool second_refresh = store.TryRefreshIfPublishedCommitVisible();
  Require(!second_refresh, "second probe should be no-op after state is already refreshed");
  const auto second_stats = store.Stats();
  Require(second_stats.frame_count == 2, "second probe should keep committed frame_count stable");
  Require(second_stats.pending_frames == 0, "second probe should keep pending state stable");
  store.Close();
}

void RunScenarioVisibleCommitProbeIgnoresCorruptFooterMagicTail(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: published-commit probe ignores corrupt footer-magic tail");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0xBA}});
  store.Commit();

  const auto before_stats = store.Stats();
  std::error_code ec;
  const auto file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    throw std::runtime_error("failed to read file size for corrupt tail scenario");
  }

  std::vector<std::byte> fake_footer_tail(static_cast<std::size_t>(waxcpp::core::mv2s::kFooterSize), std::byte{0});
  std::copy(waxcpp::core::mv2s::kFooterMagic.begin(),
            waxcpp::core::mv2s::kFooterMagic.end(),
            fake_footer_tail.begin());
  // Intentionally keep the payload invalid (zero/garbage fields) so decode must fail.
  WriteBytesAt(path, file_size, fake_footer_tail);

  const bool refreshed = store.TryRefreshIfPublishedCommitVisible();
  Require(!refreshed, "probe must ignore corrupt footer-like tail bytes");
  const auto after_stats = store.Stats();
  Require(after_stats.frame_count == before_stats.frame_count,
          "corrupt-tail probe must keep committed frame_count unchanged");
  Require(after_stats.pending_frames == before_stats.pending_frames,
          "corrupt-tail probe must keep pending state unchanged");
  store.Close();
}

void RunScenarioCommitDoesNotRegressCommittedSequenceOnCorruptPendingHeader(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: local pending commit survives corrupt WAL header via in-memory cache");
  auto store = waxcpp::WaxStore::Create(path);
  (void)store.Put({std::byte{0x01}});
  store.Commit();
  const auto baseline_wal = store.WalStats();
  Require(baseline_wal.committed_seq > 0, "baseline committed_seq must be positive after first commit");
  const auto baseline_stats = store.Stats();

  (void)store.Put({std::byte{0x02}});
  const auto staged_wal = store.WalStats();
  const auto pending_header_offset = waxcpp::core::mv2s::kWalOffset + staged_wal.checkpoint_pos;
  const std::array<std::byte, 8> zero_sequence = {};
  WriteBytesAt(path, pending_header_offset, std::span<const std::byte>(zero_sequence.data(), zero_sequence.size()));

  store.Commit();
  const auto after_commit_stats = store.Stats();
  const auto after_commit_wal = store.WalStats();
  Require(after_commit_stats.frame_count == baseline_stats.frame_count + 1,
          "commit must apply local pending mutation even when WAL pending header is corrupt");
  Require(after_commit_stats.pending_frames == 0,
          "commit must clear pending_frames after in-memory pending apply");
  Require(after_commit_wal.committed_seq >= staged_wal.last_seq,
          "commit must advance committed_seq to include local pending WAL sequence");
  Require(after_commit_wal.committed_seq >= baseline_wal.committed_seq,
          "commit must keep committed_seq monotonic across corrupt pending header scenario");
  Require(after_commit_wal.committed_seq <= after_commit_wal.last_seq,
          "wal last_seq must stay >= committed_seq after corruption-tolerant commit");
  store.Close();
}

void RunScenarioSupersedeCycleRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: supersede cycle rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload_a = {std::byte{0x61}};
    const std::vector<std::byte> payload_b = {std::byte{0x62}};
    const auto a = store.Put(payload_a);
    const auto b = store.Put(payload_b);
    Require(a == 0 && b == 1, "expected dense ids for cycle scenario");

    store.Supersede(a, b);  // b -> a
    store.Supersede(b, a);  // a -> b (cycle)

    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject supersede cycle");
    // Simulate abrupt end; avoid Close() auto-commit retry.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  // Nothing committed, both puts and supersede ops remain pending.
  Require(reopened.Stats().frame_count == 0, "cycle-rejected commit must not advance committed frame_count");
  reopened.Close();
}

void RunScenarioSupersedeConflictRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: supersede conflict rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto a = store.Put({std::byte{0x71}});
    const auto b = store.Put({std::byte{0x72}});
    const auto c = store.Put({std::byte{0x73}});
    Require(a == 0 && b == 1 && c == 2, "expected dense ids for supersede conflict scenario");

    store.Supersede(a, b);  // b -> a
    store.Supersede(c, b);  // b -> c (conflict: b already supersedes a)

    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject supersede conflict");
    // Simulate abrupt end; avoid Close() auto-commit retry.
  }
}

void RunScenarioCloseAutoCommitsPending(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: close auto-commits pending mutations");
  auto store = waxcpp::WaxStore::Create(path);
  const auto id = store.Put({std::byte{0x81}, std::byte{0x82}});
  Require(id == 0, "expected first id=0 in close auto-commit scenario");
  Require(store.Stats().pending_frames == 1, "pending_frames should be 1 before close");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 1, "close should commit pending put");
  Require(stats.pending_frames == 0, "no pending frames expected after close auto-commit");
  reopened.Close();

  const auto closed_wal_stats = store.WalStats();
  Require(closed_wal_stats.auto_commit_count == 1,
          "Close() with local pending mutations must increment wal auto_commit_count");
}

void RunScenarioFrameReadApis(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: frame read APIs expose committed payloads");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload0 = {std::byte{0x91}, std::byte{0x92}};
  const std::vector<std::byte> payload1 = {std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}};
  const auto id0 = store.Put(payload0);
  const auto id1 = store.Put(payload1);
  Require(id0 == 0 && id1 == 1, "expected dense ids for frame read API scenario");
  store.Delete(id0);
  store.Commit();

  const auto maybe_meta0 = store.FrameMeta(id0);
  Require(maybe_meta0.has_value(), "FrameMeta(0) must exist");
  Require(maybe_meta0->status == 1, "FrameMeta(0).status must reflect delete");

  const auto maybe_meta1 = store.FrameMeta(id1);
  Require(maybe_meta1.has_value(), "FrameMeta(1) must exist");
  Require(maybe_meta1->payload_length == payload1.size(), "FrameMeta(1).payload_length mismatch");

  const auto metas = store.FrameMetas();
  Require(metas.size() == 2, "FrameMetas size mismatch");

  const auto content1 = store.FrameContent(id1);
  Require(content1 == payload1, "FrameContent(1) mismatch");

  const auto contents = store.FrameContents({id0, id1});
  Require(contents.size() == 2, "FrameContents size mismatch");
  Require(contents.at(id0) == payload0, "FrameContents(0) mismatch");
  Require(contents.at(id1) == payload1, "FrameContents(1) mismatch");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto reopened_content1 = reopened.FrameContent(id1);
  Require(reopened_content1 == payload1, "reopened FrameContent(1) mismatch");
  reopened.Close();
}

void RunScenarioCloseDoesNotCommitRecoveredPending(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: close does not auto-commit recovered pending WAL");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0xB1}});
    // Simulate crash/no graceful close.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    Require(stats.frame_count == 0, "recovered pending scenario should still have 0 committed frames");
    Require(stats.pending_frames == 1, "recovered pending scenario should expose pending frame");
    // Close must not auto-commit recovery-only pending mutations.
    reopened.Close();

    const auto closed_wal_stats = reopened.WalStats();
    Require(closed_wal_stats.auto_commit_count == 0,
            "Close() on recovered pending-only state must not increment wal auto_commit_count");
  }

  {
    auto reopened_again = waxcpp::WaxStore::Open(path);
    const auto stats_again = reopened_again.Stats();
    Require(stats_again.frame_count == 0, "close must not commit recovery-only pending WAL");
    Require(stats_again.pending_frames == 1, "pending WAL should remain after close on recovered state");
    reopened_again.Close();
  }
}

void RunScenarioRecoveredPendingPlusLocalMutationsCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recovered pending plus local mutations commit together");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0xC1}});  // pending from crashed process
    // Simulate crash/no graceful close.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    auto before = reopened.Stats();
    Require(before.frame_count == 0, "expected no committed frames before merged commit");
    Require(before.pending_frames == 1, "expected one recovered pending frame");

    const auto local_id = reopened.Put({std::byte{0xC2}});
    Require(local_id == 1, "local frame id should continue after recovered pending frame id");
    reopened.Commit();

    auto after = reopened.Stats();
    Require(after.frame_count == 2, "expected two committed frames after merged commit");
    Require(after.pending_frames == 0, "expected no pending frames after merged commit");

    const auto c0 = reopened.FrameContent(0);
    const auto c1 = reopened.FrameContent(1);
    Require(c0 == std::vector<std::byte>{std::byte{0xC1}}, "frame 0 content mismatch after merged commit");
    Require(c1 == std::vector<std::byte>{std::byte{0xC2}}, "frame 1 content mismatch after merged commit");
    reopened.Close();
  }
}

void RunScenarioWriterLeaseExclusion(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: writer lease excludes concurrent open on same store path");
  auto primary = waxcpp::WaxStore::Create(path);

  bool threw = false;
  try {
    auto competing = waxcpp::WaxStore::Open(path);
    competing.Close();
  } catch (const std::exception& ex) {
    threw = true;
    waxcpp::tests::Log(std::string("expected competing-open rejection: ") + ex.what());
  }
  Require(threw, "competing open must fail while primary writer lease is held");

  primary.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Close();
}

void RunScenarioWriterLeaseArtifactDoesNotBlock(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: stale writer lease artifact file does not block open");
  auto lease_path = path;
  lease_path += ".writer.lease";

  {
    std::ofstream out(lease_path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(out), "failed to create synthetic stale writer lease artifact");
    const char marker[] = "stale-lease-artifact";
    out.write(marker, static_cast<std::streamsize>(sizeof(marker) - 1));
    Require(static_cast<bool>(out), "failed to write synthetic stale writer lease artifact");
  }

  auto store = waxcpp::WaxStore::Create(path);

  bool threw = false;
  try {
    auto competing = waxcpp::WaxStore::Open(path);
    competing.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "competing open must fail while stale-artifact-backed lease is actively held");

  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Close();
}

void RunScenarioWriterLeaseCleansUpArtifactOnClose(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: writer lease artifact cleans up on close");
  auto lease_path = path;
  lease_path += ".writer.lease";

  {
    auto store = waxcpp::WaxStore::Create(path);
    store.Close();
  }
  Require(!std::filesystem::exists(lease_path), "writer lease artifact should not remain after close");

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.Close();
  }
  Require(!std::filesystem::exists(lease_path), "writer lease artifact should not remain after reopen close");
}

void RunScenarioWriterLeaseCrossProcessExclusion(const std::filesystem::path& path,
                                                 const std::filesystem::path& test_executable) {
  waxcpp::tests::Log("scenario: writer lease excludes cross-process open on same store path");
  if (test_executable.empty()) {
    throw std::runtime_error("test executable path is empty");
  }

  {
    auto seed_store = waxcpp::WaxStore::Create(path);
    seed_store.Close();
  }

  const auto ready_path = CrossProcessLeaseReadyPath(path);
  std::error_code ec;
  std::filesystem::remove(ready_path, ec);

#ifdef _WIN32
  HANDLE helper_process = LaunchHoldWriterLeaseHelper(test_executable, path, ready_path);
#else
  const auto command = QuoteShellArg(test_executable.string()) + " --hold-writer-lease " +
                       QuoteShellArg(path.string()) + " " + QuoteShellArg(ready_path.string());
  auto helper = std::async(std::launch::async, [command]() { return std::system(command.c_str()); });
#endif

  bool helper_ready = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(ready_path)) {
      helper_ready = true;
      break;
    }
#ifdef _WIN32
    const auto wait_status = ::WaitForSingleObject(helper_process, 0);
    if (wait_status == WAIT_OBJECT_0) {
      break;
    }
#else
    if (helper.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      break;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  Require(helper_ready, "cross-process writer lease helper did not signal readiness");

  bool threw = false;
  try {
    auto competing = waxcpp::WaxStore::Open(path);
    competing.Close();
  } catch (const std::exception& ex) {
    threw = true;
    waxcpp::tests::Log(std::string("expected cross-process competing-open rejection: ") + ex.what());
  }
  Require(threw, "cross-process competing open must fail while helper writer lease is held");

#ifdef _WIN32
  const auto wait_exit = ::WaitForSingleObject(helper_process, 15000);
  Require(wait_exit == WAIT_OBJECT_0, "cross-process writer lease helper timed out");
  DWORD helper_exit = 1;
  const auto got_exit = ::GetExitCodeProcess(helper_process, &helper_exit);
  ::CloseHandle(helper_process);
  Require(got_exit != FALSE, "failed to read cross-process writer lease helper exit code");
#else
  const auto helper_exit = helper.get();
#endif
  Require(helper_exit == 0, "cross-process writer lease helper process must exit successfully");
  std::filesystem::remove(ready_path, ec);

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Close();
}

void RunScenarioFailedOpenReleasesWriterLease(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: failed open releases writer lease");
  {
    auto seed = waxcpp::WaxStore::Create(path);
    seed.Close();
  }

  const std::array<std::byte, 1> corrupt_magic = {std::byte{0x00}};
  WriteBytesAt(path, 0, std::span<const std::byte>(corrupt_magic.data(), corrupt_magic.size()));
  WriteBytesAt(path,
               waxcpp::core::mv2s::kHeaderPageSize,
               std::span<const std::byte>(corrupt_magic.data(), corrupt_magic.size()));

  bool threw = false;
  try {
    auto broken = waxcpp::WaxStore::Open(path);
    broken.Close();
  } catch (const std::exception& ex) {
    threw = true;
    waxcpp::tests::Log(std::string("expected open failure on corrupt header: ") + ex.what());
  }
  Require(threw, "open on corrupted header must fail");

  // If failed Open leaks writer lease, this Create/Open sequence will fail.
  auto recreated = waxcpp::WaxStore::Create(path);
  recreated.Close();
  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Close();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--hold-writer-lease") {
    if (argc != 4) {
      waxcpp::tests::LogError("helper mode expects: --hold-writer-lease <store-path> <ready-path>");
      return EXIT_FAILURE;
    }
    return RunHoldWriterLeaseHelper(std::filesystem::path(argv[2]), std::filesystem::path(argv[3]));
  }

  waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_write_test_");
  const auto path = UniquePath();
  std::filesystem::path executable_path{};
  if (argc > 0 && argv[0] != nullptr) {
    executable_path = std::filesystem::path(argv[0]);
    if (!executable_path.empty() && !executable_path.is_absolute()) {
      std::error_code ec;
      const auto abs = std::filesystem::absolute(executable_path, ec);
      if (!ec) {
        executable_path = abs;
      }
    }
  }
  try {
    waxcpp::tests::Log("wax_store_write_test: start");
    waxcpp::tests::LogKV("wax_store_write_test_path", path.string());

    RunScenarioPutCommitReopen(path);
    RunScenarioPutBatchContracts(path);
    RunScenarioPutEmbeddingContracts(path);
    RunScenarioPendingEmbeddingSnapshot(path);
    RunScenarioPendingEmbeddingSnapshotReopenRecovery(path);
    RunScenarioPendingEmbeddingSnapshotUsesInMemoryCache(path);
    RunScenarioCloseAutoCommitsLocalEmbeddingMutations(path);
    RunScenarioLocalMixedPendingCorruptWalCloseCommit(path);
    RunScenarioRecoveredPendingEmbeddingPlusLocalMutationCloseCommit(path);
    RunScenarioRecoveredPendingDeletePlusLocalMutationCloseCommit(path);
    RunScenarioRecoveredPendingSupersedePlusLocalMutationCloseCommit(path);
    RunScenarioPutEmbeddingUnknownFrameRejected(path);
    RunScenarioPutEmbeddingBatchUnknownFrameRejected(path);
    RunScenarioPutEmbeddingForwardReferenceRejected(path);
    RunScenarioPendingRecoveryCommit(path);
    RunScenarioPendingRecoverySkipsUndecodableTail(path);
    RunScenarioPendingRecoveryIgnoresPartialTail(path);
    RunScenarioPendingLifecycleRecoverySkipsUndecodableTail(path);
    RunScenarioPendingMixedLifecycleReplayDeterminism(path);
    RunScenarioMixedRecoveredAndLocalReplayAcrossReopenCycles(path);
    RunScenarioRecoveredMixedLifecycleCorruptWalAcrossReopenCycles(path);
    RunScenarioDeleteAndSupersedePersist(path);
    RunScenarioPendingLifecycleMutationCountersLocal(path);
    RunScenarioPendingLifecycleMutationCountersRecovered(path);
    RunScenarioCrashWindowStep2WithMixedRecoveredAndLocalPending(path);
    RunScenarioCrashWindowAfterTocWrite(path);
    RunScenarioCrashWindowAfterFooterWrite(path);
    RunScenarioCrashWindowAfterCheckpointBeforeHeaders(path);
    RunScenarioCrashWindowAfterHeaderA(path);
    RunScenarioCrashWindowAfterHeaderB(path);
    RunScenarioVisibleCommitProbeStep1NoRefresh(path);
    RunScenarioVisibleCommitProbeStep1PreservesLifecyclePendingCounters(path);
    RunScenarioVisibleCommitProbeStep2Refreshes(path);
    RunScenarioVisibleCommitProbeStep3Refreshes(path);
    RunScenarioVisibleCommitProbeStep4Refreshes(path);
    RunScenarioVisibleCommitProbeStep5Refreshes(path);
    RunScenarioVisibleCommitProbeNoNewGenerationNoRefresh(path);
    RunScenarioVisibleCommitProbeClosedStoreThrows(path);
    RunScenarioVisibleCommitProbeRefreshIsIdempotent(path);
    RunScenarioVisibleCommitProbeIgnoresCorruptFooterMagicTail(path);
    RunScenarioCommitDoesNotRegressCommittedSequenceOnCorruptPendingHeader(path);
    RunScenarioSupersedeCycleRejected(path);
    RunScenarioSupersedeConflictRejected(path);
    RunScenarioCloseAutoCommitsPending(path);
    RunScenarioFrameReadApis(path);
    RunScenarioCloseDoesNotCommitRecoveredPending(path);
    RunScenarioRecoveredPendingPlusLocalMutationsCommit(path);
    RunScenarioWriterLeaseExclusion(path);
    RunScenarioWriterLeaseArtifactDoesNotBlock(path);
    RunScenarioWriterLeaseCleansUpArtifactOnClose(path);
    RunScenarioWriterLeaseCrossProcessExclusion(path, executable_path);
    RunScenarioFailedOpenReleasesWriterLease(path);

    waxcpp::tests::CleanupStoreArtifacts(path);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_write_test_");
    waxcpp::tests::Log("wax_store_write_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    waxcpp::tests::CleanupStoreArtifacts(path);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_write_test_");
    return EXIT_FAILURE;
  }
}
