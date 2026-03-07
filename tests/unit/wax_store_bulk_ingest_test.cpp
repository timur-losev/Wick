// Stress test: bulk Put+Commit simulating UE5 indexing to find footer/TOC limits.
//
// Reproduces "wax_store: current footer is missing or invalid" by:
// 1. Creating a store
// 2. Putting many frames with UE5-like metadata
// 3. Committing periodically (every N puts)
// 4. Verifying the footer stays valid after each commit

#include "waxcpp/wax_store.hpp"
#include "../../src/core/mv2s_format.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_bulk_ingest_test_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

std::vector<std::byte> MakePayload(std::size_t size) {
  std::vector<std::byte> out(size, std::byte{0x41});
  return out;
}

waxcpp::Metadata MakeUE5Metadata(std::uint64_t frame_index) {
  // Simulate UE5 indexing metadata with realistic key/value sizes.
  // Average UE5 source path: ~70 chars
  const std::string path_prefix = "Runtime/Engine/Private/Components/Rendering/SubSystems/";
  const std::string file_name = "SkeletalMeshComponent_" + std::to_string(frame_index % 10000) + ".cpp";
  const std::string file_path = path_prefix + file_name;

  return {
      {"file_path", file_path},
      {"language", "cpp"},
      {"symbol", "USkeletalMeshComponent::RenderSection_" + std::to_string(frame_index)},
      {"chunk_id", "a1b2c3d4e5f6" + std::to_string(frame_index)},
      {"chunk_hash", "f6e5d4c3b2a1" + std::to_string(frame_index)},
      {"token_estimate", std::to_string(400 + (frame_index % 200))},
  };
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error("FAIL: " + message);
  }
}

struct TestResult {
  std::uint64_t total_puts = 0;
  std::uint64_t total_commits = 0;
  std::uint64_t file_size_bytes = 0;
  bool passed = false;
  std::string error;
};

TestResult RunBulkIngestTest(std::uint64_t target_frames,
                             std::uint64_t flush_every,
                             std::size_t payload_size) {
  TestResult result{};
  const auto path = UniquePath();

  try {
    auto store = waxcpp::WaxStore::Create(path);
    std::cout << "  Store created: " << path << std::endl;
    std::cout << "  Target frames: " << target_frames
              << "  flush_every: " << flush_every
              << "  payload_size: " << payload_size << std::endl;

    for (std::uint64_t i = 0; i < target_frames; ++i) {
      const auto payload = MakePayload(payload_size);
      const auto metadata = MakeUE5Metadata(i);
      store.Put(payload, metadata);
      result.total_puts++;

      if ((i + 1) % flush_every == 0) {
        store.Commit();
        result.total_commits++;

        const auto stats = store.Stats();
        const auto file_sz = std::filesystem::file_size(path);

        if ((i + 1) % (flush_every * 4) == 0 || i + 1 == target_frames) {
          std::cout << "  [commit " << result.total_commits << "] "
                    << "frames=" << stats.frame_count + stats.pending_frames
                    << " file=" << (file_sz / (1024 * 1024)) << "MB"
                    << std::endl;
        }
      }
    }

    // Final commit
    store.Commit();
    result.total_commits++;

    const auto final_stats = store.Stats();
    result.file_size_bytes = std::filesystem::file_size(path);

    std::cout << "  Final: frames=" << final_stats.frame_count
              << " file=" << (result.file_size_bytes / (1024 * 1024)) << "MB"
              << " commits=" << result.total_commits
              << std::endl;

    // Verify: reopen and check
    store.Close();
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto reopen_stats = reopened.Stats();
    Require(reopen_stats.frame_count == final_stats.frame_count,
            "frame count mismatch after reopen: " +
                std::to_string(reopen_stats.frame_count) + " != " +
                std::to_string(final_stats.frame_count));

    reopened.Close();
    result.passed = true;

  } catch (const std::exception& e) {
    result.error = e.what();
    std::cerr << "  ERROR at put=" << result.total_puts
              << " commits=" << result.total_commits
              << ": " << e.what() << std::endl;
  }

  // Cleanup
  std::error_code ec;
  std::filesystem::remove(path, ec);
  for (const auto& suffix : {".writer.lease", ".index.checkpoint"}) {
    std::filesystem::remove(std::filesystem::path(path.string() + suffix), ec);
  }

  return result;
}

}  // namespace

int main() {
  int failures = 0;

  // Test 1: Moderate scale — 10K frames, flush every 1000
  {
    std::cout << "\n=== Test 1: 10K frames, flush every 1000 ===" << std::endl;
    const auto r = RunBulkIngestTest(10'000, 1000, 800);
    if (!r.passed) {
      std::cerr << "  FAILED: " << r.error << std::endl;
      ++failures;
    } else {
      std::cout << "  PASSED" << std::endl;
    }
  }

  // Test 2: Large scale — 50K frames, flush every 4096
  // This tests TOC near ~18MB (50K * ~360B)
  {
    std::cout << "\n=== Test 2: 50K frames, flush every 4096 ===" << std::endl;
    const auto r = RunBulkIngestTest(50'000, 4096, 800);
    if (!r.passed) {
      std::cerr << "  FAILED: " << r.error << std::endl;
      ++failures;
    } else {
      std::cout << "  PASSED" << std::endl;
    }
  }

  // Test 3: Boundary test — push TOC past 64MB limit
  // At ~370 bytes per frame, 64MB / 370 ≈ 181K frames
  // Use 180K frames to get close to the limit
  {
    std::cout << "\n=== Test 3: 180K frames, flush every 4096 (TOC ~64MB boundary) ===" << std::endl;
    const auto r = RunBulkIngestTest(180'000, 4096, 200);
    if (!r.passed) {
      std::cerr << "  FAILED at " << r.total_puts << " puts: " << r.error << std::endl;
      ++failures;

      // Also report the critical info
      std::cout << "  >>> kMaxTocBytes = " << waxcpp::core::mv2s::kMaxTocBytes
                << " (" << (waxcpp::core::mv2s::kMaxTocBytes / (1024 * 1024)) << " MB)"
                << std::endl;
      std::cout << "  >>> Estimated TOC size at failure: ~"
                << (r.total_puts * 370 / (1024 * 1024)) << " MB" << std::endl;
    } else {
      std::cout << "  PASSED" << std::endl;
    }
  }

  // Test 4: Beyond limit — 200K frames
  {
    std::cout << "\n=== Test 4: 200K frames, flush every 4096 (TOC > 64MB) ===" << std::endl;
    const auto r = RunBulkIngestTest(200'000, 4096, 200);
    if (!r.passed) {
      std::cerr << "  FAILED at " << r.total_puts << " puts: " << r.error << std::endl;
      std::cerr << "  >>> This confirms kMaxTocBytes limit hit" << std::endl;
      // This failure is EXPECTED - it proves the bug
    } else {
      std::cout << "  PASSED (kMaxTocBytes was sufficient)" << std::endl;
    }
  }

  std::cout << "\n=== Summary ===" << std::endl;
  std::cout << "Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
