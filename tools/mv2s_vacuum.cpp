// mv2s_vacuum — rewrite .mv2s store file without dead TOCs.
//
// Usage:  waxcpp_mv2s_vacuum <source.mv2s> [destination.mv2s]
//
// If destination is omitted, rewrites in-place:
//   source.mv2s → source.mv2s.bak (backup)
//   new file    → source.mv2s
//
// This tool:
//   1. Opens the source store (read-only)
//   2. Reads all frame metadata + payloads
//   3. Creates a fresh store at destination
//   4. Copies all active frames with their metadata
//   5. Commits the new store
//
// The resulting file contains zero dead TOCs and is typically
// 90-97% smaller than the original.
//
// Exit codes:
//   0 = success
//   1 = usage error
//   2 = file error
//   3 = data error

#include "waxcpp/wax_store.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string HumanBytes(std::uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit_idx = 0;
  while (value >= 1024.0 && unit_idx < 4) {
    value /= 1024.0;
    ++unit_idx;
  }
  std::ostringstream out;
  if (unit_idx == 0) {
    out << bytes << " B";
  } else {
    out << std::fixed << std::setprecision(2) << value << " " << units[unit_idx];
  }
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: waxcpp_mv2s_vacuum <source.mv2s> [destination.mv2s]\n"
              << "\n"
              << "Rewrites the store without dead TOCs.\n"
              << "If destination is omitted, rewrites in-place (creates .bak backup).\n";
    return 1;
  }

  const std::filesystem::path source_path = argv[1];
  const bool in_place = (argc == 2);
  const std::filesystem::path dest_path = in_place
      ? std::filesystem::path(source_path.string() + ".vacuum.tmp")
      : std::filesystem::path(argv[2]);

  try {
    // ── Validate source ──────────────────────────────────────────
    std::error_code ec;
    if (!std::filesystem::exists(source_path, ec) || ec) {
      std::cerr << "ERROR: Source file does not exist: " << source_path << "\n";
      return 2;
    }
    const auto source_size = std::filesystem::file_size(source_path, ec);
    if (ec) {
      std::cerr << "ERROR: Cannot get source file size: " << ec.message() << "\n";
      return 2;
    }

    if (std::filesystem::exists(dest_path, ec) && !ec) {
      std::cerr << "ERROR: Destination already exists: " << dest_path << "\n"
                << "Remove it first or choose a different path.\n";
      return 2;
    }

    std::cout << "=== mv2s Vacuum ===" << std::endl;
    std::cout << "  Source:      " << source_path << "  (" << HumanBytes(source_size) << ")" << std::endl;
    std::cout << "  Destination: " << dest_path << std::endl;
    std::cout << std::endl;

    // ── Open source store ────────────────────────────────────────
    std::cout << "  [1/4] Opening source store..." << std::flush;
    auto source = waxcpp::WaxStore::Open(source_path);
    const auto stats = source.Stats();
    std::cout << " OK (" << stats.frame_count << " frames)" << std::endl;

    // ── Read all frame metadata ──────────────────────────────────
    std::cout << "  [2/4] Reading frame metadata..." << std::flush;
    const auto& metas = source.CommittedFrameMetasRef();
    std::cout << " OK (" << metas.size() << " frames)" << std::endl;

    // ── Create destination store ─────────────────────────────────
    std::cout << "  [3/4] Creating destination store..." << std::flush;
    auto dest = waxcpp::WaxStore::Create(dest_path);
    std::cout << " OK" << std::endl;

    // ── Copy frames in batches ───────────────────────────────────
    std::cout << "  [4/4] Copying frames..." << std::flush;
    const auto t0 = std::chrono::steady_clock::now();

    constexpr std::size_t kBatchSize = 256;
    std::uint64_t copied = 0;
    std::uint64_t active_count = 0;
    std::uint64_t skipped_deleted = 0;

    std::vector<std::uint64_t> batch_ids{};
    batch_ids.reserve(kBatchSize);

    for (std::size_t i = 0; i < metas.size(); ++i) {
      const auto& meta = metas[i];

      // Skip deleted/superseded frames
      if (meta.status != 0) {
        ++skipped_deleted;
        continue;
      }

      batch_ids.push_back(meta.id);

      if (batch_ids.size() >= kBatchSize || i + 1 == metas.size()) {
        // Bulk-read payloads
        const auto contents = source.FrameContents(batch_ids);

        std::vector<std::vector<std::byte>> payloads{};
        std::vector<waxcpp::Metadata> metadatas{};
        payloads.reserve(batch_ids.size());
        metadatas.reserve(batch_ids.size());

        for (const auto frame_id : batch_ids) {
          const auto it = contents.find(frame_id);
          if (it == contents.end()) {
            std::cerr << "\nWARNING: Frame " << frame_id << " content not found, skipping\n";
            continue;
          }
          payloads.push_back(it->second);

          // Reconstruct metadata from frame meta
          const auto& fm = metas[static_cast<std::size_t>(frame_id)];
          waxcpp::Metadata md = fm.metadata;
          metadatas.push_back(std::move(md));
        }

        if (!payloads.empty()) {
          dest.PutBatch(payloads, metadatas);
          copied += payloads.size();
        }

        batch_ids.clear();

        // Progress every 50K frames
        if (copied % 50000 < kBatchSize) {
          const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - t0).count();
          std::cout << "\r  [4/4] Copying frames... " << copied << " / " << active_count + (metas.size() - i)
                    << "  (" << elapsed << "s)" << std::flush;
        }
      }

      ++active_count;
    }

    // ── Commit ───────────────────────────────────────────────────
    std::cout << "\r  [4/4] Committing " << copied << " frames..." << std::flush;
    dest.Commit();
    dest.Close();
    source.Close();

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    const auto dest_size = std::filesystem::file_size(dest_path, ec);

    std::cout << " OK" << std::endl;
    std::cout << std::endl;

    // ── In-place rename ──────────────────────────────────────────
    if (in_place) {
      const auto backup_path = std::filesystem::path(source_path.string() + ".bak");
      if (std::filesystem::exists(backup_path, ec)) {
        std::filesystem::remove(backup_path, ec);
      }
      std::filesystem::rename(source_path, backup_path, ec);
      if (ec) {
        std::cerr << "ERROR: Failed to rename source to backup: " << ec.message() << "\n";
        std::cerr << "Vacuumed file is at: " << dest_path << "\n";
        return 2;
      }
      std::filesystem::rename(dest_path, source_path, ec);
      if (ec) {
        std::cerr << "ERROR: Failed to rename vacuum result to source: " << ec.message() << "\n";
        std::cerr << "Backup is at: " << backup_path << "\n";
        std::cerr << "Vacuumed file is at: " << dest_path << "\n";
        return 2;
      }
      std::cout << "  In-place rename complete." << std::endl;
      std::cout << "  Backup: " << backup_path << std::endl;
    }

    // ── Summary ──────────────────────────────────────────────────
    std::cout << std::endl;
    std::cout << "=== Vacuum Complete ===" << std::endl;
    std::cout << "  Source size:   " << HumanBytes(source_size) << std::endl;
    std::cout << "  Result size:   " << HumanBytes(dest_size) << std::endl;
    const double ratio = (source_size > 0) ? (1.0 - static_cast<double>(dest_size) / static_cast<double>(source_size)) * 100.0 : 0.0;
    std::cout << "  Reduction:     " << std::fixed << std::setprecision(1) << ratio << "%" << std::endl;
    std::cout << "  Frames:        " << copied << " active, " << skipped_deleted << " deleted/skipped" << std::endl;
    std::cout << "  Elapsed:       " << (elapsed_ms / 1000) << "." << ((elapsed_ms % 1000) / 100) << "s" << std::endl;

    return 0;

  } catch (const std::exception& e) {
    std::cerr << "FATAL: " << e.what() << std::endl;
    return 3;
  }
}
