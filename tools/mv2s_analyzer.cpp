// mv2s_analyzer — binary analysis tool for .mv2s store files.
//
// Usage:  waxcpp_mv2s_analyzer <path.mv2s> [options]
//
// Options:
//   --summary         Show file summary only (default if no options)
//   --headers         Dump both header pages in detail
//   --footer          Dump the current footer
//   --toc             Dump the TOC summary (frame count, segment catalog)
//   --frames          Dump every frame summary (can be huge)
//   --frames=N        Dump the first N frames
//   --dead-toc        Scan for dead/superseded TOC regions in the file
//   --validate        Full validation: reparse TOC, verify checksums
//   --all             Enable all dumps
//   --json            Output in JSON format (for programmatic consumption)
//
// Exit codes:
//   0 = success
//   1 = usage error
//   2 = file open / read error
//   3 = corruption detected

#include "waxcpp/wax_store.hpp"
#include "../src/core/mv2s_format.hpp"
#include "../src/core/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mv2s = waxcpp::core::mv2s;

// ── Helpers ───────────────────────────────────────────────────────────────────

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

std::string HexBytes(const std::byte* data, std::size_t length, std::size_t max_show = 16) {
  std::ostringstream out;
  const auto show = std::min(length, max_show);
  for (std::size_t i = 0; i < show; ++i) {
    if (i > 0) out << ' ';
    out << std::hex << std::setfill('0') << std::setw(2) << std::to_integer<int>(data[i]);
  }
  if (length > max_show) {
    out << " ... (" << std::dec << length << " bytes total)";
  }
  return out.str();
}

std::string HashHex(const std::array<std::byte, 32>& hash) {
  std::ostringstream out;
  for (const auto b : hash) {
    out << std::hex << std::setfill('0') << std::setw(2) << std::to_integer<int>(b);
  }
  return out.str();
}

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open file: " + path.string());
  }
  file.seekg(static_cast<std::streamoff>(offset));
  if (!file) {
    throw std::runtime_error("seek failed to offset " + std::to_string(offset));
  }
  std::vector<std::byte> buffer(length);
  file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(length));
  const auto read_count = static_cast<std::size_t>(file.gcount());
  if (read_count != length) {
    throw std::runtime_error("short read: expected " + std::to_string(length) +
                             " bytes, got " + std::to_string(read_count));
  }
  return buffer;
}

// ── Options ───────────────────────────────────────────────────────────────────

struct AnalyzerOptions {
  std::filesystem::path file_path{};
  bool show_summary = false;
  bool show_headers = false;
  bool show_footer = false;
  bool show_toc = false;
  bool show_frames = false;
  std::uint64_t max_frames = 0;  // 0 = all
  bool show_dead_toc = false;
  bool validate = false;
  bool json_output = false;
};

AnalyzerOptions ParseOptions(int argc, char** argv) {
  AnalyzerOptions opts;
  if (argc < 2) {
    std::cerr << "Usage: waxcpp_mv2s_analyzer <file.mv2s> [options]\n"
              << "  --summary     File summary (default)\n"
              << "  --headers     Header page details\n"
              << "  --footer      Footer dump\n"
              << "  --toc         TOC summary\n"
              << "  --frames      All frame summaries\n"
              << "  --frames=N    First N frame summaries\n"
              << "  --dead-toc    Scan for dead TOC regions\n"
              << "  --validate    Full validation pass\n"
              << "  --all         Enable all outputs\n"
              << "  --json        JSON output\n";
    std::exit(1);
  }
  opts.file_path = argv[1];

  bool any_explicit = false;
  for (int i = 2; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--summary") { opts.show_summary = true; any_explicit = true; }
    else if (arg == "--headers") { opts.show_headers = true; any_explicit = true; }
    else if (arg == "--footer") { opts.show_footer = true; any_explicit = true; }
    else if (arg == "--toc") { opts.show_toc = true; any_explicit = true; }
    else if (arg == "--frames") { opts.show_frames = true; opts.max_frames = 0; any_explicit = true; }
    else if (arg.starts_with("--frames=")) {
      opts.show_frames = true;
      opts.max_frames = std::stoull(std::string(arg.substr(9)));
      any_explicit = true;
    }
    else if (arg == "--dead-toc") { opts.show_dead_toc = true; any_explicit = true; }
    else if (arg == "--validate") { opts.validate = true; any_explicit = true; }
    else if (arg == "--json") { opts.json_output = true; }
    else if (arg == "--all") {
      opts.show_summary = true;
      opts.show_headers = true;
      opts.show_footer = true;
      opts.show_toc = true;
      opts.show_frames = true;
      opts.show_dead_toc = true;
      opts.validate = true;
      any_explicit = true;
    }
    else {
      std::cerr << "Unknown option: " << arg << "\n";
      std::exit(1);
    }
  }

  if (!any_explicit) {
    opts.show_summary = true;
  }
  return opts;
}

// ── Printers ──────────────────────────────────────────────────────────────────

void PrintSeparator(const std::string& title) {
  std::cout << "\n=== " << title << " ===" << std::endl;
}

void PrintHeaderPage(const std::string& label, const mv2s::HeaderPage& page) {
  std::cout << "  " << label << ":" << std::endl;
  std::cout << "    format_version:         0x" << std::hex << page.format_version << std::dec << std::endl;
  std::cout << "    spec:                   " << static_cast<int>(page.spec_major) << "." << static_cast<int>(page.spec_minor) << std::endl;
  std::cout << "    header_page_generation: " << page.header_page_generation << std::endl;
  std::cout << "    file_generation:        " << page.file_generation << std::endl;
  std::cout << "    footer_offset:          " << page.footer_offset << " (" << HumanBytes(page.footer_offset) << ")" << std::endl;
  std::cout << "    wal_offset:             " << page.wal_offset << " (" << HumanBytes(page.wal_offset) << ")" << std::endl;
  std::cout << "    wal_size:               " << page.wal_size << " (" << HumanBytes(page.wal_size) << ")" << std::endl;
  std::cout << "    wal_write_pos:          " << page.wal_write_pos << std::endl;
  std::cout << "    wal_checkpoint_pos:     " << page.wal_checkpoint_pos << std::endl;
  std::cout << "    wal_committed_seq:      " << page.wal_committed_seq << std::endl;
  std::cout << "    toc_checksum:           " << HashHex(page.toc_checksum) << std::endl;
  std::cout << "    header_checksum:        " << HashHex(page.header_checksum) << std::endl;
  if (page.replay_snapshot.has_value()) {
    const auto& snap = *page.replay_snapshot;
    std::cout << "    replay_snapshot:" << std::endl;
    std::cout << "      file_generation:      " << snap.file_generation << std::endl;
    std::cout << "      wal_committed_seq:    " << snap.wal_committed_seq << std::endl;
    std::cout << "      footer_offset:        " << snap.footer_offset << " (" << HumanBytes(snap.footer_offset) << ")" << std::endl;
    std::cout << "      wal_write_pos:        " << snap.wal_write_pos << std::endl;
    std::cout << "      wal_checkpoint_pos:   " << snap.wal_checkpoint_pos << std::endl;
    std::cout << "      wal_pending_bytes:    " << snap.wal_pending_bytes << std::endl;
    std::cout << "      wal_last_sequence:    " << snap.wal_last_sequence << std::endl;
  } else {
    std::cout << "    replay_snapshot:        (none)" << std::endl;
  }
}

void PrintFooter(const mv2s::Footer& footer, std::uint64_t footer_offset) {
  std::cout << "  footer_offset:      " << footer_offset << " (" << HumanBytes(footer_offset) << ")" << std::endl;
  std::cout << "  toc_len:            " << footer.toc_len << " (" << HumanBytes(footer.toc_len) << ")" << std::endl;
  std::cout << "  toc_hash:           " << HashHex(footer.toc_hash) << std::endl;
  std::cout << "  generation:         " << footer.generation << std::endl;
  std::cout << "  wal_committed_seq:  " << footer.wal_committed_seq << std::endl;
  const auto toc_offset = footer_offset - footer.toc_len;
  std::cout << "  toc_offset:         " << toc_offset << " (" << HumanBytes(toc_offset) << ")" << std::endl;
  const auto kMaxToc = mv2s::kMaxTocBytes;
  const double pct = static_cast<double>(footer.toc_len) / static_cast<double>(kMaxToc) * 100.0;
  std::cout << "  toc_capacity_used:  " << std::fixed << std::setprecision(1) << pct << "% of "
            << HumanBytes(kMaxToc) << " kMaxTocBytes" << std::endl;
}

void PrintFrameSummary(const mv2s::FrameSummary& frame) {
  std::cout << "  frame[" << frame.id << "]:" << std::endl;
  std::cout << "    timestamp_ms:      " << frame.timestamp_ms << std::endl;
  std::cout << "    payload_offset:    " << frame.payload_offset << " (" << HumanBytes(frame.payload_offset) << ")" << std::endl;
  std::cout << "    payload_length:    " << frame.payload_length << " (" << HumanBytes(frame.payload_length) << ")" << std::endl;
  std::cout << "    payload_checksum:  " << HashHex(frame.payload_checksum) << std::endl;
  std::cout << "    canonical_enc:     " << static_cast<int>(frame.canonical_encoding) << std::endl;
  if (frame.kind.has_value()) {
    std::cout << "    kind:              " << *frame.kind << std::endl;
  }
  std::cout << "    status:            " << static_cast<int>(frame.status) << std::endl;
  if (!frame.metadata.empty()) {
    std::cout << "    metadata (" << frame.metadata.size() << " entries):" << std::endl;
    for (const auto& [k, v] : frame.metadata) {
      const auto display_v = v.size() > 80 ? v.substr(0, 77) + "..." : v;
      std::cout << "      " << k << " = " << display_v << std::endl;
    }
  }
  if (!frame.tags.empty()) {
    std::cout << "    tags (" << frame.tags.size() << "):" << std::endl;
    for (const auto& [k, v] : frame.tags) {
      std::cout << "      " << k << " = " << v << std::endl;
    }
  }
  if (!frame.labels.empty()) {
    std::cout << "    labels: ";
    for (std::size_t i = 0; i < frame.labels.size(); ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << frame.labels[i];
    }
    std::cout << std::endl;
  }
  if (frame.supersedes.has_value()) {
    std::cout << "    supersedes:        " << *frame.supersedes << std::endl;
  }
  if (frame.superseded_by.has_value()) {
    std::cout << "    superseded_by:     " << *frame.superseded_by << std::endl;
  }
}

// ── Dead TOC scanner ─────────────────────────────────────────────────────────

struct DeadTocRegion {
  std::uint64_t toc_offset = 0;
  std::uint64_t toc_len = 0;
  std::uint64_t footer_offset = 0;
  std::uint64_t generation = 0;
  std::uint64_t frame_count = 0;
  bool checksum_valid = false;
};

std::vector<DeadTocRegion> ScanDeadTocRegions(const std::filesystem::path& path,
                                               std::uint64_t file_size,
                                               std::uint64_t live_footer_offset) {
  std::vector<DeadTocRegion> dead_regions;

  // Scan backwards from live_footer_offset looking for footer magic at 64-byte aligned positions
  // Actually, footers are appended sequentially in append-only format. Scan for footer magic patterns.
  const auto kFooterMagic = mv2s::kFooterMagic;
  const auto kFooterSize = mv2s::kFooterSize;

  // Read the data region in chunks
  const auto data_start = mv2s::kWalOffset + mv2s::kDefaultWalSize; // approximate
  if (data_start >= file_size) {
    return dead_regions;
  }

  const std::size_t kScanChunkSize = 64ULL * 1024ULL * 1024ULL; // 64MB chunks
  std::uint64_t scan_pos = data_start;

  while (scan_pos < file_size) {
    const auto chunk_size = std::min(static_cast<std::uint64_t>(kScanChunkSize), file_size - scan_pos);
    if (chunk_size < kFooterSize) break;

    try {
      const auto chunk = ReadFileBytes(path, scan_pos, static_cast<std::size_t>(chunk_size));

      // Search for footer magic in this chunk
      for (std::size_t i = 0; i + kFooterSize <= chunk.size(); ++i) {
        bool magic_match = true;
        for (std::size_t j = 0; j < kFooterMagic.size(); ++j) {
          if (chunk[i + j] != kFooterMagic[j]) {
            magic_match = false;
            break;
          }
        }
        if (!magic_match) continue;

        const auto candidate_footer_offset = scan_pos + i;
        if (candidate_footer_offset == live_footer_offset) continue;

        // Try to decode this as a footer
        try {
          std::array<std::byte, 64> footer_bytes{};
          std::copy_n(chunk.begin() + static_cast<std::ptrdiff_t>(i), kFooterSize, footer_bytes.begin());
          const auto footer = mv2s::DecodeFooter(footer_bytes);

          if (footer.toc_len < 32 || footer.toc_len > candidate_footer_offset) {
            continue;
          }

          DeadTocRegion region{};
          region.footer_offset = candidate_footer_offset;
          region.toc_len = footer.toc_len;
          region.toc_offset = candidate_footer_offset - footer.toc_len;
          region.generation = footer.generation;

          // Try to verify TOC checksum
          if (footer.toc_len <= 256ULL * 1024ULL * 1024ULL) {
            try {
              const auto toc_bytes = ReadFileBytes(path, region.toc_offset, static_cast<std::size_t>(footer.toc_len));
              region.checksum_valid = mv2s::TocHashMatches(toc_bytes, footer.toc_hash);

              if (region.checksum_valid) {
                try {
                  const auto toc = mv2s::DecodeToc(toc_bytes);
                  region.frame_count = toc.frame_count;
                } catch (...) {
                  // TOC parse failed but checksum was valid — interesting
                }
              }
            } catch (...) {
              // Could not read TOC bytes
            }
          }

          dead_regions.push_back(region);
        } catch (...) {
          // Not a valid footer
        }
      }
    } catch (...) {
      // Read failed for this chunk
    }

    scan_pos += chunk_size;
    if (chunk_size < kScanChunkSize) break;
  }

  std::sort(dead_regions.begin(), dead_regions.end(), [](const auto& a, const auto& b) {
    return a.footer_offset < b.footer_offset;
  });
  return dead_regions;
}

// ── Main analysis ────────────────────────────────────────────────────────────

int RunAnalysis(const AnalyzerOptions& opts) {
  const auto& path = opts.file_path;
  std::error_code ec;

  if (!std::filesystem::exists(path, ec) || ec) {
    std::cerr << "ERROR: File does not exist: " << path << std::endl;
    return 2;
  }

  const auto file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    std::cerr << "ERROR: Cannot get file size: " << ec.message() << std::endl;
    return 2;
  }

  int exit_code = 0;

  // ── Read headers ────────────────────────────────────────────────────────
  std::optional<mv2s::HeaderPage> header_a;
  std::optional<mv2s::HeaderPage> header_b;
  bool header_a_valid = false;
  bool header_b_valid = false;

  if (file_size >= mv2s::kHeaderRegionSize) {
    try {
      const auto bytes_a = ReadFileBytes(path, 0, static_cast<std::size_t>(mv2s::kHeaderPageSize));
      header_a = mv2s::DecodeHeaderPage(bytes_a);
      header_a_valid = true;
    } catch (const std::exception& e) {
      std::cerr << "WARNING: Header A decode failed: " << e.what() << std::endl;
    }

    try {
      const auto bytes_b = ReadFileBytes(path, mv2s::kHeaderPageSize, static_cast<std::size_t>(mv2s::kHeaderPageSize));
      header_b = mv2s::DecodeHeaderPage(bytes_b);
      header_b_valid = true;
    } catch (const std::exception& e) {
      std::cerr << "WARNING: Header B decode failed: " << e.what() << std::endl;
    }
  } else {
    std::cerr << "ERROR: File too small for header region (" << file_size << " < " << mv2s::kHeaderRegionSize << ")" << std::endl;
    return 3;
  }

  // Choose the canonical header (highest header_page_generation)
  const mv2s::HeaderPage* canonical_header = nullptr;
  std::string canonical_label = "none";
  if (header_a_valid && header_b_valid) {
    if (header_a->header_page_generation >= header_b->header_page_generation) {
      canonical_header = &*header_a;
      canonical_label = "A";
    } else {
      canonical_header = &*header_b;
      canonical_label = "B";
    }
  } else if (header_a_valid) {
    canonical_header = &*header_a;
    canonical_label = "A";
  } else if (header_b_valid) {
    canonical_header = &*header_b;
    canonical_label = "B";
  }

  if (!canonical_header) {
    std::cerr << "ERROR: No valid header pages found. File may be corrupt." << std::endl;
    return 3;
  }

  // ── Read footer ─────────────────────────────────────────────────────────
  const auto footer_offset = canonical_header->footer_offset;
  std::optional<mv2s::Footer> footer;
  bool footer_valid = false;

  if (footer_offset + mv2s::kFooterSize <= file_size) {
    try {
      const auto footer_bytes = ReadFileBytes(path, footer_offset, static_cast<std::size_t>(mv2s::kFooterSize));
      footer = mv2s::DecodeFooter(footer_bytes);
      footer_valid = true;
    } catch (const std::exception& e) {
      std::cerr << "WARNING: Footer decode failed at offset " << footer_offset << ": " << e.what() << std::endl;
    }
  } else {
    std::cerr << "WARNING: Footer offset (" << footer_offset << ") + 64 exceeds file size (" << file_size << ")" << std::endl;
  }

  // ── Read TOC ────────────────────────────────────────────────────────────
  std::optional<mv2s::TocSummary> toc_summary;
  bool toc_valid = false;
  std::uint64_t toc_offset = 0;
  bool toc_checksum_ok = false;

  if (footer_valid && footer->toc_len >= 32 && footer->toc_len <= footer_offset) {
    toc_offset = footer_offset - footer->toc_len;
    try {
      const auto toc_bytes = ReadFileBytes(path, toc_offset, static_cast<std::size_t>(footer->toc_len));
      toc_checksum_ok = mv2s::TocHashMatches(toc_bytes, footer->toc_hash);

      if (toc_checksum_ok) {
        try {
          toc_summary = mv2s::DecodeToc(toc_bytes);
          toc_valid = true;
        } catch (const std::exception& e) {
          std::cerr << "WARNING: TOC decode failed: " << e.what() << std::endl;
        }
      } else {
        std::cerr << "WARNING: TOC checksum mismatch!" << std::endl;
      }
    } catch (const std::exception& e) {
      std::cerr << "WARNING: TOC read failed: " << e.what() << std::endl;
    }
  }

  // ── Summary ─────────────────────────────────────────────────────────────
  if (opts.show_summary) {
    PrintSeparator("File Summary");
    std::cout << "  path:               " << path.string() << std::endl;
    std::cout << "  file_size:          " << file_size << " (" << HumanBytes(file_size) << ")" << std::endl;
    std::cout << "  header_region:      " << HumanBytes(mv2s::kHeaderRegionSize) << std::endl;
    std::cout << "  header_a:           " << (header_a_valid ? "valid" : "INVALID") << std::endl;
    std::cout << "  header_b:           " << (header_b_valid ? "valid" : "INVALID") << std::endl;
    std::cout << "  canonical_header:   " << canonical_label << std::endl;

    if (canonical_header) {
      std::cout << "  wal_region:         offset=" << canonical_header->wal_offset
                << " size=" << HumanBytes(canonical_header->wal_size) << std::endl;
      const auto data_start = canonical_header->wal_offset + canonical_header->wal_size;
      std::cout << "  data_region_start:  " << data_start << " (" << HumanBytes(data_start) << ")" << std::endl;
    }

    std::cout << "  footer:             " << (footer_valid ? "valid" : "INVALID") << std::endl;
    std::cout << "  footer_offset:      " << footer_offset << " (" << HumanBytes(footer_offset) << ")" << std::endl;

    if (footer_valid) {
      std::cout << "  generation:         " << footer->generation << std::endl;
      std::cout << "  toc_len:            " << footer->toc_len << " (" << HumanBytes(footer->toc_len) << ")" << std::endl;
      const double pct = static_cast<double>(footer->toc_len) / static_cast<double>(mv2s::kMaxTocBytes) * 100.0;
      std::cout << "  toc_capacity:       " << std::fixed << std::setprecision(1) << pct << "% of "
                << HumanBytes(mv2s::kMaxTocBytes) << std::endl;
      std::cout << "  toc_checksum:       " << (toc_checksum_ok ? "OK" : "MISMATCH") << std::endl;
    }

    if (toc_valid) {
      std::cout << "  toc_version:        " << toc_summary->toc_version << std::endl;
      std::cout << "  frame_count:        " << toc_summary->frame_count << std::endl;
      std::cout << "  segments:           " << toc_summary->segments.size() << std::endl;

      if (toc_summary->lex_index.has_value()) {
        std::cout << "  lex_index:          offset=" << toc_summary->lex_index->bytes_offset
                  << " len=" << HumanBytes(toc_summary->lex_index->bytes_length) << std::endl;
      } else {
        std::cout << "  lex_index:          (none)" << std::endl;
      }
      if (toc_summary->vec_index.has_value()) {
        std::cout << "  vec_index:          offset=" << toc_summary->vec_index->bytes_offset
                  << " len=" << HumanBytes(toc_summary->vec_index->bytes_length) << std::endl;
      } else {
        std::cout << "  vec_index:          (none)" << std::endl;
      }

      // Compute payload statistics
      std::uint64_t total_payload = 0;
      std::uint64_t min_payload = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t max_payload = 0;
      std::uint64_t active_count = 0;
      std::uint64_t deleted_count = 0;
      std::size_t total_metadata_keys = 0;
      std::unordered_map<std::string, std::size_t> meta_key_counts;
      std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> meta_value_counts;

      for (const auto& f : toc_summary->frames) {
        total_payload += f.payload_length;
        if (f.payload_length < min_payload) min_payload = f.payload_length;
        if (f.payload_length > max_payload) max_payload = f.payload_length;
        if (f.status == 0) ++active_count;
        else ++deleted_count;
        total_metadata_keys += f.metadata.size();
        for (const auto& [k, v] : f.metadata) {
          ++meta_key_counts[k];
          ++meta_value_counts[k][v];
        }
      }

      if (toc_summary->frame_count > 0) {
        const auto avg_payload = total_payload / toc_summary->frame_count;
        const auto avg_toc_per_frame = footer->toc_len / toc_summary->frame_count;
        std::cout << "  total_payload:      " << HumanBytes(total_payload) << std::endl;
        std::cout << "  avg_payload/frame:  " << HumanBytes(avg_payload) << std::endl;
        std::cout << "  min_payload:        " << HumanBytes(min_payload) << std::endl;
        std::cout << "  max_payload:        " << HumanBytes(max_payload) << std::endl;
        std::cout << "  active_frames:      " << active_count << std::endl;
        std::cout << "  deleted_frames:     " << deleted_count << std::endl;
        std::cout << "  avg_toc/frame:      " << avg_toc_per_frame << " bytes" << std::endl;
        std::cout << "  total_meta_keys:    " << total_metadata_keys
                  << " (avg " << (total_metadata_keys / toc_summary->frame_count) << "/frame)" << std::endl;

        // Estimate maximum frames before hitting kMaxTocBytes
        const auto max_frames_estimate = mv2s::kMaxTocBytes / avg_toc_per_frame;
        std::cout << "  est_max_frames:     ~" << max_frames_estimate << " before kMaxTocBytes limit" << std::endl;

        // Metadata key histogram
        std::cout << "\n  --- Metadata Keys ---" << std::endl;
        std::vector<std::pair<std::string, std::size_t>> sorted_keys(meta_key_counts.begin(), meta_key_counts.end());
        std::sort(sorted_keys.begin(), sorted_keys.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& [key, count] : sorted_keys) {
          const auto pct = (100.0 * count) / toc_summary->frame_count;
          const auto distinct = meta_value_counts[key].size();
          std::cout << "    " << std::left << std::setw(24) << key
                    << std::right << std::setw(8) << count
                    << "  (" << std::fixed << std::setprecision(1) << pct << "%)"
                    << "  distinct=" << distinct;
          // Show top 3 values if distinct <= 20
          if (distinct <= 20) {
            std::vector<std::pair<std::string, std::size_t>> top_vals(meta_value_counts[key].begin(), meta_value_counts[key].end());
            std::sort(top_vals.begin(), top_vals.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::cout << "  [";
            for (std::size_t i = 0; i < std::min<std::size_t>(top_vals.size(), 3); ++i) {
              if (i > 0) std::cout << ", ";
              auto val = top_vals[i].first;
              if (val.size() > 30) val = val.substr(0, 27) + "...";
              std::cout << "\"" << val << "\"=" << top_vals[i].second;
            }
            std::cout << "]";
          }
          std::cout << std::endl;
        }
      }

      // File layout summary
      const auto data_start = canonical_header->wal_offset + canonical_header->wal_size;
      const auto data_region_size = (toc_offset > data_start) ? (toc_offset - data_start) : 0;
      const auto overhead = file_size - total_payload;
      std::cout << "\n  --- Layout ---" << std::endl;
      std::cout << "  [Header A]          0x0000 .. 0x" << std::hex << mv2s::kHeaderPageSize << std::dec
                << "  (" << HumanBytes(mv2s::kHeaderPageSize) << ")" << std::endl;
      std::cout << "  [Header B]          0x" << std::hex << mv2s::kHeaderPageSize
                << " .. 0x" << mv2s::kHeaderRegionSize << std::dec
                << "  (" << HumanBytes(mv2s::kHeaderPageSize) << ")" << std::endl;
      std::cout << "  [WAL]               0x" << std::hex << canonical_header->wal_offset
                << " .. 0x" << (canonical_header->wal_offset + canonical_header->wal_size) << std::dec
                << "  (" << HumanBytes(canonical_header->wal_size) << ")" << std::endl;
      std::cout << "  [Data + Dead TOCs]  0x" << std::hex << data_start
                << " .. 0x" << toc_offset << std::dec
                << "  (" << HumanBytes(data_region_size) << ")" << std::endl;
      std::cout << "  [Live TOC]          0x" << std::hex << toc_offset
                << " .. 0x" << footer_offset << std::dec
                << "  (" << HumanBytes(footer->toc_len) << ")" << std::endl;
      std::cout << "  [Footer]            0x" << std::hex << footer_offset
                << " .. 0x" << (footer_offset + mv2s::kFooterSize) << std::dec
                << "  (" << HumanBytes(mv2s::kFooterSize) << ")" << std::endl;
      std::cout << "  [Overhead]          " << HumanBytes(overhead) << " ("
                << std::fixed << std::setprecision(1) << (100.0 * static_cast<double>(overhead) / static_cast<double>(file_size)) << "%)" << std::endl;
    }
  }

  // ── Headers detail ──────────────────────────────────────────────────────
  if (opts.show_headers) {
    PrintSeparator("Header Pages");
    if (header_a_valid) {
      PrintHeaderPage("Header A (offset 0x0000)", *header_a);
    } else {
      std::cout << "  Header A: INVALID / UNREADABLE" << std::endl;
    }
    std::cout << std::endl;
    if (header_b_valid) {
      PrintHeaderPage("Header B (offset 0x1000)", *header_b);
    } else {
      std::cout << "  Header B: INVALID / UNREADABLE" << std::endl;
    }

    if (header_a_valid && header_b_valid) {
      std::cout << "\n  --- Header Consistency ---" << std::endl;
      if (header_a->file_generation == header_b->file_generation) {
        std::cout << "  file_generation:     MATCH (" << header_a->file_generation << ")" << std::endl;
      } else {
        std::cout << "  file_generation:     MISMATCH A=" << header_a->file_generation
                  << " B=" << header_b->file_generation << std::endl;
      }
      if (header_a->footer_offset == header_b->footer_offset) {
        std::cout << "  footer_offset:       MATCH (" << header_a->footer_offset << ")" << std::endl;
      } else {
        std::cout << "  footer_offset:       MISMATCH A=" << header_a->footer_offset
                  << " B=" << header_b->footer_offset << std::endl;
      }
      const auto gen_diff = static_cast<std::int64_t>(header_a->header_page_generation) -
                            static_cast<std::int64_t>(header_b->header_page_generation);
      if (std::abs(gen_diff) == 1) {
        std::cout << "  page_generation:     OK (diff=1, A=" << header_a->header_page_generation
                  << " B=" << header_b->header_page_generation << ")" << std::endl;
      } else {
        std::cout << "  page_generation:     UNUSUAL (diff=" << gen_diff << ")" << std::endl;
      }
    }
  }

  // ── Footer detail ───────────────────────────────────────────────────────
  if (opts.show_footer && footer_valid) {
    PrintSeparator("Footer");
    PrintFooter(*footer, footer_offset);
  }

  // ── TOC detail ──────────────────────────────────────────────────────────
  if (opts.show_toc && toc_valid) {
    PrintSeparator("TOC Summary");
    std::cout << "  toc_version:   " << toc_summary->toc_version << std::endl;
    std::cout << "  frame_count:   " << toc_summary->frame_count << std::endl;
    std::cout << "  toc_checksum:  " << HashHex(toc_summary->toc_checksum) << std::endl;

    if (!toc_summary->segments.empty()) {
      std::cout << "\n  Segment Catalog (" << toc_summary->segments.size() << " segments):" << std::endl;
      for (const auto& seg : toc_summary->segments) {
        const char* kind_names[] = {"lex", "vec", "time", "unknown"};
        const char* comp_names[] = {"none", "zstd", "lz4", "unknown"};
        std::cout << "    segment[" << seg.id << "]  kind=" << kind_names[std::min(static_cast<int>(seg.kind), 3)]
                  << " compression=" << comp_names[std::min(static_cast<int>(seg.compression), 3)]
                  << " offset=" << seg.bytes_offset << " len=" << HumanBytes(seg.bytes_length) << std::endl;
      }
    }

    // Metadata key frequency analysis
    if (toc_summary->frame_count > 0) {
      std::unordered_map<std::string, std::uint64_t> key_freq;
      for (const auto& f : toc_summary->frames) {
        for (const auto& [k, v] : f.metadata) {
          key_freq[k]++;
        }
      }
      if (!key_freq.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> sorted_keys(key_freq.begin(), key_freq.end());
        std::sort(sorted_keys.begin(), sorted_keys.end(), [](const auto& a, const auto& b) {
          return a.second > b.second;
        });
        std::cout << "\n  Metadata Key Frequency:" << std::endl;
        for (const auto& [k, count] : sorted_keys) {
          std::cout << "    " << std::setw(30) << std::left << k << " " << count << " ("
                    << std::fixed << std::setprecision(0) << (100.0 * static_cast<double>(count) / static_cast<double>(toc_summary->frame_count))
                    << "%)" << std::endl;
        }
      }
    }
  }

  // ── Frame dump ──────────────────────────────────────────────────────────
  if (opts.show_frames && toc_valid) {
    const auto limit = opts.max_frames > 0
                           ? std::min(opts.max_frames, toc_summary->frame_count)
                           : toc_summary->frame_count;
    PrintSeparator("Frames (showing " + std::to_string(limit) + " of " + std::to_string(toc_summary->frame_count) + ")");
    for (std::uint64_t i = 0; i < limit; ++i) {
      PrintFrameSummary(toc_summary->frames[i]);
    }
  }

  // ── Dead TOC scan ──────────────────────────────────────────────────────
  if (opts.show_dead_toc) {
    PrintSeparator("Dead TOC Scan");
    std::cout << "  Scanning for superseded footer magic patterns..." << std::endl;
    const auto dead_regions = ScanDeadTocRegions(path, file_size, footer_offset);
    std::cout << "  Found " << dead_regions.size() << " dead/superseded footer(s):" << std::endl;

    std::uint64_t total_dead_toc_bytes = 0;
    for (const auto& region : dead_regions) {
      std::cout << "\n  dead_footer @ " << region.footer_offset << " (" << HumanBytes(region.footer_offset) << "):" << std::endl;
      std::cout << "    generation:    " << region.generation << std::endl;
      std::cout << "    toc_len:       " << region.toc_len << " (" << HumanBytes(region.toc_len) << ")" << std::endl;
      std::cout << "    toc_offset:    " << region.toc_offset << std::endl;
      std::cout << "    checksum:      " << (region.checksum_valid ? "VALID" : "invalid/unverified") << std::endl;
      if (region.frame_count > 0) {
        std::cout << "    frame_count:   " << region.frame_count << std::endl;
      }
      total_dead_toc_bytes += region.toc_len + mv2s::kFooterSize;
    }

    std::cout << "\n  Total dead TOC+footer bytes: " << HumanBytes(total_dead_toc_bytes);
    if (file_size > 0) {
      std::cout << " (" << std::fixed << std::setprecision(1)
                << (100.0 * static_cast<double>(total_dead_toc_bytes) / static_cast<double>(file_size)) << "% of file)";
    }
    std::cout << std::endl;
  }

  // ── Validation ──────────────────────────────────────────────────────────
  if (opts.validate) {
    PrintSeparator("Validation");
    int issues = 0;

    // Check header consistency
    if (!header_a_valid) {
      std::cout << "  [FAIL] Header A is invalid" << std::endl;
      ++issues;
    } else {
      std::cout << "  [OK]   Header A valid (gen=" << header_a->header_page_generation << ")" << std::endl;
    }
    if (!header_b_valid) {
      std::cout << "  [FAIL] Header B is invalid" << std::endl;
      ++issues;
    } else {
      std::cout << "  [OK]   Header B valid (gen=" << header_b->header_page_generation << ")" << std::endl;
    }

    // Footer
    if (!footer_valid) {
      std::cout << "  [FAIL] Footer at offset " << footer_offset << " is invalid" << std::endl;
      ++issues;
    } else {
      std::cout << "  [OK]   Footer valid (gen=" << footer->generation << ")" << std::endl;
    }

    // TOC checksum
    if (footer_valid && !toc_checksum_ok) {
      std::cout << "  [FAIL] TOC checksum mismatch" << std::endl;
      ++issues;
    } else if (toc_checksum_ok) {
      std::cout << "  [OK]   TOC checksum matches" << std::endl;
    }

    // TOC decode
    if (!toc_valid && footer_valid && toc_checksum_ok) {
      std::cout << "  [FAIL] TOC decode failed despite valid checksum" << std::endl;
      ++issues;
    } else if (toc_valid) {
      std::cout << "  [OK]   TOC decoded (" << toc_summary->frame_count << " frames)" << std::endl;
    }

    // TOC size vs kMaxTocBytes
    if (footer_valid) {
      if (footer->toc_len > mv2s::kMaxTocBytes) {
        std::cout << "  [FAIL] TOC exceeds kMaxTocBytes: " << HumanBytes(footer->toc_len)
                  << " > " << HumanBytes(mv2s::kMaxTocBytes) << std::endl;
        ++issues;
      } else {
        const double pct = 100.0 * static_cast<double>(footer->toc_len) / static_cast<double>(mv2s::kMaxTocBytes);
        std::cout << "  [OK]   TOC within kMaxTocBytes (" << std::fixed << std::setprecision(1) << pct << "% used)" << std::endl;
        if (pct > 80.0) {
          std::cout << "  [WARN] TOC usage above 80% — consider increasing kMaxTocBytes or reducing metadata" << std::endl;
        }
      }
    }

    // Frame ID density check
    if (toc_valid) {
      bool ids_dense = true;
      for (std::uint64_t i = 0; i < toc_summary->frame_count; ++i) {
        if (toc_summary->frames[i].id != i) {
          ids_dense = false;
          std::cout << "  [FAIL] Frame IDs not dense: frame[" << i << "].id = " << toc_summary->frames[i].id << std::endl;
          ++issues;
          break;
        }
      }
      if (ids_dense) {
        std::cout << "  [OK]   Frame IDs are dense [0.." << toc_summary->frame_count << ")" << std::endl;
      }
    }

    // Footer offset consistency
    if (footer_valid && canonical_header) {
      if (footer_offset + mv2s::kFooterSize > file_size) {
        std::cout << "  [FAIL] Footer extends beyond file end" << std::endl;
        ++issues;
      } else if (footer_offset + mv2s::kFooterSize == file_size) {
        std::cout << "  [OK]   Footer ends exactly at file end" << std::endl;
      } else {
        std::cout << "  [WARN] " << (file_size - footer_offset - mv2s::kFooterSize)
                  << " trailing bytes after footer" << std::endl;
      }
    }

    // Generation consistency
    if (canonical_header && footer_valid) {
      if (canonical_header->file_generation != footer->generation) {
        std::cout << "  [WARN] Header file_generation (" << canonical_header->file_generation
                  << ") != footer generation (" << footer->generation << ")" << std::endl;
      } else {
        std::cout << "  [OK]   Header and footer generation match (" << footer->generation << ")" << std::endl;
      }
    }

    // Payload range validation
    if (toc_valid && canonical_header) {
      const auto data_start = canonical_header->wal_offset + canonical_header->wal_size;
      bool payload_ok = true;
      for (const auto& f : toc_summary->frames) {
        if (f.payload_length == 0) continue;
        if (f.payload_offset < data_start) {
          std::cout << "  [FAIL] Frame " << f.id << " payload_offset (" << f.payload_offset
                    << ") precedes data region start (" << data_start << ")" << std::endl;
          ++issues;
          payload_ok = false;
          break;
        }
        const auto end = f.payload_offset + f.payload_length;
        if (end > toc_offset) {
          std::cout << "  [FAIL] Frame " << f.id << " payload extends into TOC region" << std::endl;
          ++issues;
          payload_ok = false;
          break;
        }
      }
      if (payload_ok) {
        std::cout << "  [OK]   All frame payloads within data region" << std::endl;
      }
    }

    std::cout << "\n  === Validation result: " << issues << " issue(s) ===" << std::endl;
    if (issues > 0) {
      exit_code = 3;
    }
  }

  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto opts = ParseOptions(argc, argv);
    return RunAnalysis(opts);
  } catch (const std::exception& e) {
    std::cerr << "FATAL: " << e.what() << std::endl;
    return 2;
  }
}
