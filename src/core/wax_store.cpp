#include "waxcpp/wax_store.hpp"

#include "mv2s_format.hpp"
#include "sha256.hpp"
#include "wal_ring.hpp"
#include "wax_store_test_hooks.hpp"
#include "waxcpp/mv2v_format.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <mutex>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace waxcpp {
namespace {

using core::mv2s::Footer;
using core::mv2s::FooterSlice;
using core::mv2s::HeaderPage;
std::atomic<std::uint32_t> g_test_commit_fail_step{0};

std::runtime_error StoreError(const std::string& message) {
  return std::runtime_error("wax_store: " + message);
}

std::string NormalizeStorePathKey(const std::filesystem::path& store_path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(store_path, ec);
  if (ec) {
    return store_path.lexically_normal().string();
  }
  return absolute.lexically_normal().string();
}

std::mutex g_process_writer_leases_mutex;
std::unordered_set<std::string> g_process_writer_leases;

std::string AcquireProcessWriterLease(const std::filesystem::path& store_path) {
  const auto key = NormalizeStorePathKey(store_path);
  std::lock_guard<std::mutex> lock(g_process_writer_leases_mutex);
  if (!g_process_writer_leases.insert(key).second) {
    throw StoreError("writer lease already held: " + key);
  }
  return key;
}

void ReleaseProcessWriterLease(const std::string& key) {
  std::lock_guard<std::mutex> lock(g_process_writer_leases_mutex);
  g_process_writer_leases.erase(key);
}

std::filesystem::path WriterLeasePathForStore(const std::filesystem::path& store_path) {
  auto lease_path = store_path;
  lease_path += ".writer.lease";
  return lease_path;
}

std::string OsErrorMessage(int code) {
#ifdef _WIN32
  LPSTR buffer = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD lang_id = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
  const auto size =
      ::FormatMessageA(flags, nullptr, static_cast<DWORD>(code), lang_id, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  if (size == 0 || buffer == nullptr) {
    return "win32_error=" + std::to_string(code);
  }
  std::string message(buffer, size);
  ::LocalFree(buffer);
  while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
    message.pop_back();
  }
  return message;
#else
  const char* text = std::strerror(code);
  if (text == nullptr) {
    return "errno=" + std::to_string(code);
  }
  return std::string(text);
#endif
}

std::shared_ptr<void> AcquireWriterLease(const std::filesystem::path& store_path) {
  // POSIX fcntl locks are process-scoped, so enforce same-process exclusivity explicitly.
  const auto process_lease_key = AcquireProcessWriterLease(store_path);
  const auto lease_path = WriterLeasePathForStore(store_path);
#ifdef _WIN32
  HANDLE handle = ::CreateFileW(lease_path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const auto code = static_cast<int>(::GetLastError());
    ReleaseProcessWriterLease(process_lease_key);
    throw StoreError("failed to open writer lease file at " + lease_path.string() + ": " + OsErrorMessage(code));
  }

  OVERLAPPED overlapped{};
  if (!::LockFileEx(handle,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0,
                    std::numeric_limits<DWORD>::max(),
                    std::numeric_limits<DWORD>::max(),
                    &overlapped)) {
    const auto code = static_cast<int>(::GetLastError());
    ::CloseHandle(handle);
    ReleaseProcessWriterLease(process_lease_key);
    if (code == ERROR_LOCK_VIOLATION) {
      throw StoreError("writer lease already held: " + lease_path.string());
    }
    throw StoreError("failed to acquire writer lease at " + lease_path.string() + ": " + OsErrorMessage(code));
  }

  return std::shared_ptr<void>(new HANDLE(handle), [process_lease_key](void* raw) {
    auto* handle_ptr = static_cast<HANDLE*>(raw);
    if (handle_ptr != nullptr && *handle_ptr != INVALID_HANDLE_VALUE) {
      OVERLAPPED overlapped{};
      (void)::UnlockFileEx(*handle_ptr,
                           0,
                           std::numeric_limits<DWORD>::max(),
                           std::numeric_limits<DWORD>::max(),
                           &overlapped);
      (void)::CloseHandle(*handle_ptr);
    }
    ReleaseProcessWriterLease(process_lease_key);
    delete handle_ptr;
  });
#else
  const int fd = ::open(lease_path.c_str(), O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    const int code = errno;
    ReleaseProcessWriterLease(process_lease_key);
    throw StoreError("failed to open writer lease file at " + lease_path.string() + ": " + OsErrorMessage(code));
  }

  struct flock lock {};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;

  if (::fcntl(fd, F_SETLK, &lock) == -1) {
    const int code = errno;
    (void)::close(fd);
    ReleaseProcessWriterLease(process_lease_key);
    if (code == EACCES || code == EAGAIN) {
      throw StoreError("writer lease already held: " + lease_path.string());
    }
    throw StoreError("failed to acquire writer lease at " + lease_path.string() + ": " + OsErrorMessage(code));
  }

  return std::shared_ptr<void>(new int(fd), [lease_path, process_lease_key](void* raw) {
    auto* fd_ptr = static_cast<int*>(raw);
    if (fd_ptr != nullptr && *fd_ptr >= 0) {
      struct flock unlock {};
      unlock.l_type = F_UNLCK;
      unlock.l_whence = SEEK_SET;
      unlock.l_start = 0;
      unlock.l_len = 0;
      (void)::fcntl(*fd_ptr, F_SETLK, &unlock);
      (void)::close(*fd_ptr);
      (void)::unlink(lease_path.c_str());
    }
    ReleaseProcessWriterLease(process_lease_key);
    delete fd_ptr;
  });
#endif
}

void MaybeInjectCommitCrash(std::uint32_t step) {
  const auto requested_step = g_test_commit_fail_step.load(std::memory_order_relaxed);
  if (requested_step == 0) {
    return;
  }
  if (requested_step == step) {
    throw StoreError("injected crash-window failure at commit step " + std::to_string(step));
  }
}

std::uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    throw StoreError("failed to read file size: " + ec.message());
  }
  return size;
}

std::vector<std::byte> ReadExactly(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  if (length == 0) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StoreError("failed to open file for read: " + path.string());
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw StoreError("failed to seek for read");
  }
  std::vector<std::byte> out(length);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    throw StoreError("short read");
  }
  return out;
}

void WriteAt(std::ofstream& out, std::uint64_t offset, std::span<const std::byte> bytes) {
  out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!out) {
    throw StoreError("failed to seek for write");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw StoreError("failed to write bytes");
  }
}

void WriteBytesAt(const std::filesystem::path& path, std::uint64_t offset, std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return;
  }
  std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!out) {
    std::ofstream create(path, std::ios::binary | std::ios::trunc);
    if (!create) {
      throw StoreError("failed to create file for write: " + path.string());
    }
    create.close();
    out.open(path, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!out) {
    throw StoreError("failed to open file for write: " + path.string());
  }
  out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!out) {
    throw StoreError("failed to seek for write");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw StoreError("failed to write bytes");
  }
}

void ResizeFile(const std::filesystem::path& path, std::uint64_t size) {
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  if (ec) {
    throw StoreError("failed to resize file: " + ec.message());
  }
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

void AppendFixed(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendString(std::vector<std::byte>& out, std::string_view str) {
  AppendLE32(out, static_cast<std::uint32_t>(str.size()));
  out.insert(out.end(),
             reinterpret_cast<const std::byte*>(str.data()),
             reinterpret_cast<const std::byte*>(str.data()) + str.size());
}

std::vector<std::byte> BuildWalPutFramePayload(std::uint64_t frame_id,
                                               std::int64_t timestamp_ms,
                                               const std::optional<std::string>& kind,
                                               const Metadata& metadata,
                                               const std::vector<std::string>& labels,
                                               std::uint64_t payload_offset,
                                               std::uint64_t payload_length,
                                               std::uint8_t canonical_encoding,
                                               std::uint64_t canonical_length,
                                               std::span<const std::byte, 32> canonical_checksum,
                                               std::span<const std::byte, 32> stored_checksum) {
  std::vector<std::byte> payload{};
  payload.reserve(256);
  AppendU8(payload, 0x01);  // putFrame
  AppendLE64(payload, frame_id);
  AppendLE64(payload, static_cast<std::uint64_t>(timestamp_ms));

  // FrameMetaSubset optional fields.
  AppendU8(payload, 0);   // uri?
  AppendU8(payload, 0);   // title?

  // kind
  if (kind.has_value() && !kind->empty()) {
    AppendU8(payload, 1);
    AppendString(payload, *kind);
  } else {
    AppendU8(payload, 0);
  }

  AppendU8(payload, 0);   // track?
  AppendLE32(payload, 0); // tags.count

  // labels
  if (labels.size() > core::mv2s::kMaxArrayCount) {
    throw StoreError("labels count exceeds replay safety limit");
  }
  if (labels.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw StoreError("labels count exceeds UInt32.max");
  }
  AppendLE32(payload, static_cast<std::uint32_t>(labels.size()));
  for (const auto& label : labels) {
    AppendString(payload, label);
  }

  AppendLE32(payload, 0); // contentDates.count
  AppendU8(payload, 0);   // role?
  AppendU8(payload, 0);   // parentId?
  AppendU8(payload, 0);   // chunkIndex?
  AppendU8(payload, 0);   // chunkCount?
  AppendU8(payload, 0);   // chunkManifest?
  AppendU8(payload, 0);   // status?
  AppendU8(payload, 0);   // supersedes?
  AppendU8(payload, 0);   // supersededBy?
  AppendU8(payload, 0);   // searchText?

  // metadata
  if (!metadata.empty()) {
    if (metadata.size() > core::mv2s::kMaxArrayCount) {
      throw StoreError("metadata count exceeds replay safety limit");
    }
    if (metadata.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw StoreError("metadata count exceeds UInt32.max");
    }
    AppendU8(payload, 1);
    AppendLE32(payload, static_cast<std::uint32_t>(metadata.size()));
    std::vector<const Metadata::value_type*> sorted_metadata_entries{};
    sorted_metadata_entries.reserve(metadata.size());
    for (const auto& entry : metadata) {
      sorted_metadata_entries.push_back(&entry);
    }
    std::sort(sorted_metadata_entries.begin(),
              sorted_metadata_entries.end(),
              [](const Metadata::value_type* lhs, const Metadata::value_type* rhs) {
                if (lhs->first != rhs->first) {
                  return lhs->first < rhs->first;
                }
                return lhs->second < rhs->second;
              });
    for (const auto* entry : sorted_metadata_entries) {
      const auto& [key, value] = *entry;
      AppendString(payload, key);
      AppendString(payload, value);
    }
  } else {
    AppendU8(payload, 0);
  }

  AppendLE64(payload, payload_offset);
  AppendLE64(payload, payload_length);
  AppendU8(payload, canonical_encoding);
  AppendLE64(payload, canonical_length);
  AppendFixed(payload, canonical_checksum);
  AppendFixed(payload, stored_checksum);
  return payload;
}

std::vector<std::byte> BuildWalDeletePayload(std::uint64_t frame_id) {
  std::vector<std::byte> payload{};
  payload.reserve(1 + 8);
  AppendU8(payload, 0x02);  // deleteFrame
  AppendLE64(payload, frame_id);
  return payload;
}

std::vector<std::byte> BuildWalSupersedePayload(std::uint64_t superseded_id, std::uint64_t superseding_id) {
  std::vector<std::byte> payload{};
  payload.reserve(1 + 8 + 8);
  AppendU8(payload, 0x03);  // supersedeFrame
  AppendLE64(payload, superseded_id);
  AppendLE64(payload, superseding_id);
  return payload;
}

std::vector<std::byte> BuildWalPutEmbeddingPayload(std::uint64_t frame_id, std::span<const float> vector) {
  if (vector.empty()) {
    throw StoreError("embedding vector must be non-empty");
  }
  if (vector.size() > core::mv2s::kMaxArrayCount) {
    throw StoreError("embedding vector dimension exceeds max");
  }
  if (vector.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw StoreError("embedding vector dimension exceeds UInt32.max");
  }

  std::vector<std::byte> payload{};
  payload.reserve(1 + 8 + 4 + vector.size() * sizeof(float));
  AppendU8(payload, 0x04);  // putEmbedding
  AppendLE64(payload, frame_id);
  AppendLE32(payload, static_cast<std::uint32_t>(vector.size()));
  for (const float value : vector) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLE32(payload, bits);
  }
  return payload;
}

std::optional<HeaderPage> TryDecodeHeader(const std::filesystem::path& path, std::uint64_t offset) {
  try {
    const auto bytes = ReadExactly(path, offset, static_cast<std::size_t>(core::mv2s::kHeaderPageSize));
    return core::mv2s::DecodeHeaderPage(bytes);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<FooterSlice> TryReadFooterAt(const std::filesystem::path& path,
                                           std::uint64_t file_size,
                                           std::uint64_t footer_offset) {
  try {
    if (footer_offset + core::mv2s::kFooterSize > file_size) {
      return std::nullopt;
    }
    const auto footer_bytes = ReadExactly(path, footer_offset, static_cast<std::size_t>(core::mv2s::kFooterSize));
    const Footer footer = core::mv2s::DecodeFooter(footer_bytes);
    if (footer.toc_len < 32 || footer.toc_len > core::mv2s::kMaxTocBytes || footer.toc_len > footer_offset) {
      return std::nullopt;
    }
    if (footer.toc_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return std::nullopt;
    }
    const auto toc_offset = footer_offset - footer.toc_len;
    const auto toc_bytes = ReadExactly(path, toc_offset, static_cast<std::size_t>(footer.toc_len));
    if (!core::mv2s::TocHashMatches(toc_bytes, footer.toc_hash)) {
      return std::nullopt;
    }
    return FooterSlice{
        .footer_offset = footer_offset,
        .toc_offset = toc_offset,
        .footer = footer,
        .toc_bytes = std::move(toc_bytes),
    };
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<FooterSlice> ScanForLatestFooter(const std::filesystem::path& path, std::uint64_t file_size) {
  if (file_size < core::mv2s::kFooterSize) {
    return std::nullopt;
  }
  const auto scan_start = file_size > core::mv2s::kMaxFooterScanBytes ? file_size - core::mv2s::kMaxFooterScanBytes : 0;
  const auto scan_len = file_size - scan_start;
  if (scan_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw StoreError("scan window too large for memory");
  }

  const auto window = ReadExactly(path, scan_start, static_cast<std::size_t>(scan_len));
  std::optional<FooterSlice> best;

  if (window.size() < core::mv2s::kFooterSize) {
    return std::nullopt;
  }

  const auto last = window.size() - static_cast<std::size_t>(core::mv2s::kFooterSize);
  for (std::size_t pos = last + 1; pos-- > 0;) {
    if (window[pos] != core::mv2s::kFooterMagic[0]) {
      continue;
    }
    const std::span<const std::byte> possible_magic(window.data() + static_cast<std::ptrdiff_t>(pos),
                                                     core::mv2s::kFooterMagic.size());
    if (!std::equal(core::mv2s::kFooterMagic.begin(), core::mv2s::kFooterMagic.end(), possible_magic.begin())) {
      continue;
    }

    const auto footer_offset = scan_start + pos;
    const auto candidate = TryReadFooterAt(path, file_size, footer_offset);
    if (!candidate.has_value()) {
      continue;
    }
    if (!best.has_value()) {
      best = candidate;
      continue;
    }
    if (candidate->footer.generation > best->footer.generation ||
        (candidate->footer.generation == best->footer.generation &&
         candidate->footer_offset > best->footer_offset)) {
      best = candidate;
    }
  }
  return best;
}

std::optional<FooterSlice> SelectPreferredFooter(std::optional<FooterSlice> from_header,
                                                 std::optional<FooterSlice> from_scan) {
  if (!from_header.has_value()) {
    return from_scan;
  }
  if (!from_scan.has_value()) {
    return from_header;
  }
  const auto& header = *from_header;
  const auto& scan = *from_scan;
  if (scan.footer.generation > header.footer.generation) {
    return from_scan;
  }
  if (scan.footer.generation == header.footer.generation &&
      scan.footer_offset > header.footer_offset) {
    return from_scan;
  }
  return from_header;
}

std::array<std::byte, 32> ComputePayloadHash(const std::filesystem::path& path,
                                             std::uint64_t offset,
                                             std::uint64_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StoreError("failed to open payload for hash");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw StoreError("failed to seek payload for hash");
  }

  constexpr std::size_t kBufferSize = 1U << 20U;
  std::vector<std::byte> buffer(kBufferSize);
  core::Sha256 hasher;

  std::uint64_t remaining = length;
  while (remaining > 0) {
    const auto chunk_size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kBufferSize));
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(chunk_size));
    if (in.gcount() != static_cast<std::streamsize>(chunk_size)) {
      throw StoreError("short read while hashing payload");
    }
    hasher.Update(std::span<const std::byte>(buffer.data(), chunk_size));
    remaining -= chunk_size;
  }

  return hasher.Finalize();
}

void DeepVerifyFrames(const std::filesystem::path& path, const std::vector<core::mv2s::FrameSummary>& frames) {
  for (const auto& frame : frames) {
    if (frame.payload_length == 0) {
      continue;
    }
    if (!frame.stored_checksum.has_value()) {
      throw StoreError("frame missing stored checksum");
    }
    const auto computed = ComputePayloadHash(path, frame.payload_offset, frame.payload_length);
    if (!std::equal(computed.begin(), computed.end(), frame.stored_checksum->begin())) {
      throw StoreError("frame stored checksum mismatch");
    }
    // Canonical checksum equality is guaranteed for plain payloads.
    // Compressed canonical checksum verification requires decompression support (tracked post-M2).
    if (frame.canonical_encoding == 0 &&
        !std::equal(computed.begin(), computed.end(), frame.payload_checksum.begin())) {
      throw StoreError("frame canonical checksum mismatch");
    }
  }
}

void DeepVerifySegments(const std::filesystem::path& path, const std::vector<core::mv2s::SegmentSummary>& segments) {
  for (const auto& segment : segments) {
    if (segment.bytes_length == 0) {
      continue;
    }
    const auto computed = ComputePayloadHash(path, segment.bytes_offset, segment.bytes_length);
    if (!std::equal(computed.begin(), computed.end(), segment.checksum.begin())) {
      throw StoreError("segment checksum mismatch");
    }
    // M6 parity: validate uncompressed vec segment layout against MV2V contract.
    if (segment.kind == 1 && segment.compression == 0) {
      if (segment.bytes_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw StoreError("vec segment length exceeds addressable memory");
      }
      const auto bytes =
          ReadExactly(path, segment.bytes_offset, static_cast<std::size_t>(segment.bytes_length));
      try {
        (void)DecodeVecSegment(bytes);
      } catch (const std::exception& ex) {
        throw StoreError(std::string("vec segment decode failed: ") + ex.what());
      }
    }
  }
}

void ValidateDataRanges(const std::vector<core::mv2s::FrameSummary>& frames,
                        const std::vector<core::mv2s::SegmentSummary>& segments,
                        std::uint64_t data_start,
                        std::uint64_t data_end) {
  struct Range {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool is_frame = true;
  };
  std::vector<Range> ranges{};
  ranges.reserve(frames.size() + segments.size());

  for (const auto& frame : frames) {
    if (frame.payload_length == 0) {
      continue;
    }
    if (frame.payload_offset < data_start) {
      throw StoreError("frame payload below data region");
    }
    if (frame.payload_offset > std::numeric_limits<std::uint64_t>::max() - frame.payload_length) {
      throw StoreError("frame payload range overflow");
    }
    const auto end = frame.payload_offset + frame.payload_length;
    if (end > data_end) {
      throw StoreError("frame payload exceeds committed data end");
    }
    ranges.push_back({.start = frame.payload_offset, .end = end, .is_frame = true});
  }

  for (const auto& segment : segments) {
    if (segment.bytes_length == 0) {
      continue;
    }
    if (segment.bytes_offset < data_start) {
      throw StoreError("segment below data region");
    }
    if (segment.bytes_offset > std::numeric_limits<std::uint64_t>::max() - segment.bytes_length) {
      throw StoreError("segment range overflow");
    }
    const auto end = segment.bytes_offset + segment.bytes_length;
    if (end > data_end) {
      throw StoreError("segment exceeds committed data end");
    }
    ranges.push_back({.start = segment.bytes_offset, .end = end, .is_frame = false});
  }

  std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.start < rhs.start;
  });
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i - 1].end > ranges[i].start) {
      if (ranges[i - 1].is_frame && ranges[i].is_frame) {
        throw StoreError("overlapping frame payload ranges");
      }
      if (!ranges[i - 1].is_frame && !ranges[i].is_frame) {
        throw StoreError("overlapping segment ranges");
      }
      throw StoreError("overlap between frame payload and segment range");
    }
  }
}

bool WouldCreateSupersedeCycle(const std::vector<core::mv2s::FrameSummary>& frames,
                               std::uint64_t superseded_id,
                               std::uint64_t superseding_id) {
  std::uint64_t cursor = superseded_id;
  for (std::size_t hops = 0; hops < frames.size(); ++hops) {
    const auto& frame = frames[static_cast<std::size_t>(cursor)];
    if (!frame.supersedes.has_value()) {
      return false;
    }
    cursor = *frame.supersedes;
    if (cursor == superseding_id) {
      return true;
    }
    if (cursor >= frames.size()) {
      return false;
    }
  }
  return true;
}

struct PendingWalApplySummary {
  std::vector<core::mv2s::FrameSummary> frames_after_apply{};
  std::uint64_t pending_put_frames = 0;
  std::uint64_t pending_embedding_mutations = 0;
  std::uint64_t pending_delete_mutations = 0;
  std::uint64_t pending_supersede_mutations = 0;
  std::uint64_t max_frame_id_plus_one = 0;
  std::uint64_t required_end = 0;
};

PendingWalApplySummary AnalyzePendingWalMutations(const std::vector<core::mv2s::FrameSummary>& committed_frames,
                                                  const std::vector<core::wal::WalPendingMutationInfo>& pending_mutations,
                                                  bool strict_apply,
                                                  std::uint64_t initial_required_end) {
  PendingWalApplySummary summary{};
  summary.frames_after_apply = committed_frames;
  summary.max_frame_id_plus_one = static_cast<std::uint64_t>(committed_frames.size());
  summary.required_end = initial_required_end;

  for (const auto& mutation : pending_mutations) {
    switch (mutation.kind) {
      case core::wal::WalMutationKind::kPutFrame: {
        if (!mutation.put_frame.has_value()) {
          if (strict_apply) {
            throw StoreError("wal putFrame mutation missing payload");
          }
          break;
        }
        ++summary.pending_put_frames;
        const auto& put = *mutation.put_frame;
        if (put.frame_id == std::numeric_limits<std::uint64_t>::max()) {
          throw StoreError("pending WAL putFrame frame_id overflow");
        }
        const auto put_next = put.frame_id + 1;
        if (put_next > summary.max_frame_id_plus_one) {
          summary.max_frame_id_plus_one = put_next;
        }
        if (put.payload_offset > std::numeric_limits<std::uint64_t>::max() - put.payload_length) {
          throw StoreError("pending WAL putFrame payload range overflow");
        }
        const auto end = put.payload_offset + put.payload_length;
        if (end > summary.required_end) {
          summary.required_end = end;
        }

        if (!strict_apply) {
          break;
        }
        if (put.frame_id != summary.frames_after_apply.size()) {
          throw StoreError("wal putFrame frame_id is not dense");
        }
        core::mv2s::FrameSummary frame{};
        frame.id = put.frame_id;
        frame.timestamp_ms = put.timestamp_ms;
        frame.payload_offset = put.payload_offset;
        frame.payload_length = put.payload_length;
        frame.payload_checksum = put.canonical_checksum;
        frame.canonical_encoding = put.canonical_encoding;
        if (put.canonical_encoding != 0) {
          frame.canonical_length = put.canonical_length;
        }
        if (put.payload_length > 0) {
          frame.stored_checksum = put.stored_checksum;
        }
        frame.kind = put.kind;
        frame.metadata = put.metadata;
        frame.tags = put.tags;
        frame.labels = put.labels;
        frame.status = 0;
        summary.frames_after_apply.push_back(std::move(frame));
        break;
      }
      case core::wal::WalMutationKind::kDeleteFrame: {
        ++summary.pending_delete_mutations;
        if (!strict_apply) {
          break;
        }
        if (!mutation.delete_frame.has_value()) {
          throw StoreError("wal delete mutation missing payload");
        }
        const auto frame_id = mutation.delete_frame->frame_id;
        if (frame_id >= summary.frames_after_apply.size()) {
          throw StoreError("wal delete references unknown frame_id");
        }
        summary.frames_after_apply[static_cast<std::size_t>(frame_id)].status = 1;
        break;
      }
      case core::wal::WalMutationKind::kSupersedeFrame: {
        ++summary.pending_supersede_mutations;
        if (!strict_apply) {
          break;
        }
        if (!mutation.supersede_frame.has_value()) {
          throw StoreError("wal supersede mutation missing payload");
        }
        const auto superseded_id = mutation.supersede_frame->superseded_id;
        const auto superseding_id = mutation.supersede_frame->superseding_id;
        if (superseded_id >= summary.frames_after_apply.size() ||
            superseding_id >= summary.frames_after_apply.size()) {
          throw StoreError("wal supersede references unknown frame_id");
        }
        if (superseded_id == superseding_id) {
          throw StoreError("wal supersede self-reference");
        }
        auto& superseded = summary.frames_after_apply[static_cast<std::size_t>(superseded_id)];
        auto& superseding = summary.frames_after_apply[static_cast<std::size_t>(superseding_id)];
        if (superseded.superseded_by.has_value() && *superseded.superseded_by != superseding_id) {
          throw StoreError("wal supersede conflict: superseded frame already has different superseding frame");
        }
        if (superseding.supersedes.has_value() && *superseding.supersedes != superseded_id) {
          throw StoreError("wal supersede conflict: superseding frame already supersedes different frame");
        }
        if (WouldCreateSupersedeCycle(summary.frames_after_apply, superseded_id, superseding_id)) {
          throw StoreError("wal supersede cycle detected");
        }
        superseded.superseded_by = superseding_id;
        superseding.supersedes = superseded_id;
        break;
      }
      case core::wal::WalMutationKind::kPutEmbedding: {
        ++summary.pending_embedding_mutations;
        if (!strict_apply) {
          break;
        }
        if (!mutation.put_embedding.has_value()) {
          throw StoreError("wal putEmbedding mutation missing payload");
        }
        if (mutation.put_embedding->frame_id >= summary.frames_after_apply.size()) {
          throw StoreError("wal putEmbedding references unknown frame_id");
        }
        if (mutation.put_embedding->dimension == 0 ||
            mutation.put_embedding->vector.size() != static_cast<std::size_t>(mutation.put_embedding->dimension)) {
          throw StoreError("wal putEmbedding payload dimension mismatch");
        }
        // Embedding payload is WAL-validated here; persistence into index manifests is handled outside TOC.
        break;
      }
    }
  }
  return summary;
}

std::vector<WaxFrameMeta> ToWaxFrameMetas(std::span<const core::mv2s::FrameSummary> frames) {
  std::vector<WaxFrameMeta> metas{};
  metas.reserve(frames.size());
  for (const auto& frame : frames) {
    WaxFrameMeta meta{};
    meta.id = frame.id;
    meta.timestamp_ms = frame.timestamp_ms;
    meta.payload_offset = frame.payload_offset;
    meta.payload_length = frame.payload_length;
    meta.canonical_encoding = frame.canonical_encoding;
    meta.status = frame.status;
    meta.kind = frame.kind;
    meta.metadata = frame.metadata;
    meta.tags = frame.tags;
    meta.labels = frame.labels;
    meta.supersedes = frame.supersedes;
    meta.superseded_by = frame.superseded_by;
    metas.push_back(std::move(meta));
  }
  return metas;
}

}  // namespace

namespace core::testing {

void SetCommitFailStep(std::uint32_t step) {
  g_test_commit_fail_step.store(step, std::memory_order_relaxed);
}

void ClearCommitFailStep() {
  g_test_commit_fail_step.store(0, std::memory_order_relaxed);
}

}  // namespace core::testing

WaxStore WaxStore::Create(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      throw StoreError("failed to create parent directories: " + ec.message());
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw StoreError("failed to create file: " + path.string());
  }

  auto toc_bytes = core::mv2s::EncodeEmptyTocV1();
  std::array<std::byte, 32> toc_checksum{};
  std::copy(toc_bytes.end() - 32, toc_bytes.end(), toc_checksum.begin());

  const std::uint64_t toc_offset = core::mv2s::kWalOffset + core::mv2s::kDefaultWalSize;
  const std::uint64_t footer_offset = toc_offset + toc_bytes.size();

  Footer footer;
  footer.toc_len = toc_bytes.size();
  footer.toc_hash = toc_checksum;
  footer.generation = 0;
  footer.wal_committed_seq = 0;
  const auto footer_bytes = core::mv2s::EncodeFooter(footer);

  HeaderPage page_a;
  page_a.header_page_generation = 1;
  page_a.file_generation = 0;
  page_a.footer_offset = footer_offset;
  page_a.wal_offset = core::mv2s::kWalOffset;
  page_a.wal_size = core::mv2s::kDefaultWalSize;
  page_a.wal_write_pos = 0;
  page_a.wal_checkpoint_pos = 0;
  page_a.wal_committed_seq = 0;
  page_a.toc_checksum = toc_checksum;
  const auto page_a_bytes = core::mv2s::EncodeHeaderPage(page_a);

  HeaderPage page_b = page_a;
  page_b.header_page_generation = 0;
  const auto page_b_bytes = core::mv2s::EncodeHeaderPage(page_b);

  WriteAt(out, 0, page_a_bytes);
  WriteAt(out, core::mv2s::kHeaderPageSize, page_b_bytes);
  WriteAt(out, toc_offset, toc_bytes);
  WriteAt(out, footer_offset, footer_bytes);
  out.flush();
  if (!out) {
    throw StoreError("flush failed");
  }

  return Open(path, true);
}

WaxStore WaxStore::Open(const std::filesystem::path& path) {
  return Open(path, true);
}

WaxStore WaxStore::Open(const std::filesystem::path& path, bool repair) {
  WaxStore store(path);
  store.writer_lease_ = AcquireWriterLease(path);
  store.LoadState(false, repair);
  return store;
}

void WaxStore::Verify(bool deep) {
  LoadState(deep, false);
}

std::uint64_t WaxStore::Put(const std::vector<std::byte>& content, const Metadata& metadata) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }

  const auto frame_id = next_frame_id_;
  const auto payload_offset = FileSize(path_);
  const auto payload_length = static_cast<std::uint64_t>(content.size());
  const auto stored_checksum = core::Sha256Digest(content);
  const auto canonical_checksum = stored_checksum;
  const std::uint8_t canonical_encoding = 0;  // plain
  const auto canonical_length = payload_length;

  // Generate timestamp.
  const auto now = std::chrono::system_clock::now();
  const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();

  // Extract kind from metadata if present.
  std::optional<std::string> kind;
  auto kind_it = metadata.find("kind");
  if (kind_it != metadata.end() && !kind_it->second.empty()) {
    kind = kind_it->second;
  }

  // Extract labels from metadata if present (comma-separated).
  std::vector<std::string> labels;
  auto labels_it = metadata.find("labels");
  if (labels_it != metadata.end() && !labels_it->second.empty()) {
    std::string_view sv = labels_it->second;
    while (!sv.empty()) {
      auto pos = sv.find(',');
      auto token = sv.substr(0, pos);
      if (!token.empty()) {
        labels.emplace_back(token);
      }
      if (pos == std::string_view::npos) break;
      sv.remove_prefix(pos + 1);
    }
  }

  if (!content.empty()) {
    WriteBytesAt(path_, payload_offset, content);
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  const auto wal_payload = BuildWalPutFramePayload(frame_id,
                                                   timestamp_ms,
                                                   kind,
                                                   metadata,
                                                   labels,
                                                   payload_offset,
                                                   payload_length,
                                                   canonical_encoding,
                                                   canonical_length,
                                                   canonical_checksum,
                                                   stored_checksum);
  const auto sequence = writer.Append(wal_payload);

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  pending_mutation_order_cache_.emplace_back(sequence, PendingMutationKind::kPutFrame);
  const auto put_inserted = pending_put_frame_cache_.emplace(sequence,
                                                              PendingPutFrameCache{
                                                                  .frame_id = frame_id,
                                                                  .timestamp_ms = timestamp_ms,
                                                                  .payload_offset = payload_offset,
                                                                  .payload_length = payload_length,
                                                                  .canonical_encoding = canonical_encoding,
                                                                  .canonical_length = canonical_length,
                                                                  .canonical_checksum = canonical_checksum,
                                                                  .stored_checksum = stored_checksum,
                                                                  .kind = kind,
                                                                  .metadata = metadata,
                                                                  .tags = {},
                                                                  .labels = labels,
                                                              });
  if (!put_inserted.second) {
    throw StoreError("duplicate pending putFrame sequence in cache");
  }

  stats_.pending_frames += 1;
  next_frame_id_ = frame_id + 1;
  dirty_ = true;
  has_local_mutations_ = true;
  return frame_id;
}

std::vector<std::uint64_t> WaxStore::PutBatch(const std::vector<std::vector<std::byte>>& contents,
                                              const std::vector<Metadata>& metadatas) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (!metadatas.empty() && metadatas.size() != contents.size()) {
    throw StoreError("PutBatch metadatas size must be zero or match contents size");
  }
  if (contents.empty()) {
    return {};
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);

  // Generate a shared timestamp for the batch.
  const auto now = std::chrono::system_clock::now();
  const auto batch_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();

  std::vector<std::uint64_t> ids{};
  ids.reserve(contents.size());
  std::uint64_t payload_offset = FileSize(path_);
  std::vector<std::vector<std::byte>> wal_payloads{};
  wal_payloads.reserve(contents.size());
  std::vector<PendingPutFrameCache> pending_puts{};
  pending_puts.reserve(contents.size());
  for (std::size_t i = 0; i < contents.size(); ++i) {
    const auto frame_id = next_frame_id_ + static_cast<std::uint64_t>(i);
    const auto payload_length = static_cast<std::uint64_t>(contents[i].size());
    const auto stored_checksum = core::Sha256Digest(contents[i]);
    const auto canonical_checksum = stored_checksum;
    const std::uint8_t canonical_encoding = 0;  // plain
    const auto canonical_length = payload_length;

    // Per-frame metadata (if available).
    const auto& frame_meta = (!metadatas.empty()) ? metadatas[i] : Metadata{};
    std::optional<std::string> frame_kind;
    auto kind_it = frame_meta.find("kind");
    if (kind_it != frame_meta.end() && !kind_it->second.empty()) {
      frame_kind = kind_it->second;
    }
    std::vector<std::string> frame_labels;
    auto labels_it = frame_meta.find("labels");
    if (labels_it != frame_meta.end() && !labels_it->second.empty()) {
      std::string_view sv = labels_it->second;
      while (!sv.empty()) {
        auto pos = sv.find(',');
        auto token = sv.substr(0, pos);
        if (!token.empty()) frame_labels.emplace_back(token);
        if (pos == std::string_view::npos) break;
        sv.remove_prefix(pos + 1);
      }
    }

    if (!contents[i].empty()) {
      WriteBytesAt(path_, payload_offset, contents[i]);
    }
    wal_payloads.emplace_back(BuildWalPutFramePayload(frame_id,
                                                      batch_timestamp_ms,
                                                      frame_kind,
                                                      frame_meta,
                                                      frame_labels,
                                                      payload_offset,
                                                      payload_length,
                                                      canonical_encoding,
                                                      canonical_length,
                                                      canonical_checksum,
                                                      stored_checksum));
    pending_puts.push_back(PendingPutFrameCache{
        .frame_id = frame_id,
        .timestamp_ms = batch_timestamp_ms,
        .payload_offset = payload_offset,
        .payload_length = payload_length,
        .canonical_encoding = canonical_encoding,
        .canonical_length = canonical_length,
        .canonical_checksum = canonical_checksum,
        .stored_checksum = stored_checksum,
        .kind = frame_kind,
        .metadata = frame_meta,
        .tags = {},
        .labels = frame_labels,
    });
    ids.push_back(frame_id);
    if (payload_offset > std::numeric_limits<std::uint64_t>::max() - payload_length) {
      throw StoreError("putBatch payload offset overflow");
    }
    payload_offset += payload_length;
  }
  const auto sequences = writer.AppendBatch(wal_payloads);
  if (sequences.size() != contents.size()) {
    throw StoreError("wal putBatch append sequence count mismatch");
  }

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  for (std::size_t i = 0; i < contents.size(); ++i) {
    const auto sequence = sequences[i];
    pending_mutation_order_cache_.emplace_back(sequence, PendingMutationKind::kPutFrame);
    const auto put_inserted = pending_put_frame_cache_.emplace(sequence, pending_puts[i]);
    if (!put_inserted.second) {
      throw StoreError("duplicate pending putFrame sequence in cache");
    }
  }

  stats_.pending_frames += static_cast<std::uint64_t>(contents.size());
  next_frame_id_ += static_cast<std::uint64_t>(contents.size());
  dirty_ = true;
  has_local_mutations_ = true;
  return ids;
}

void WaxStore::PutEmbedding(std::uint64_t frame_id, const std::vector<float>& vector) {
  PutEmbeddingBatch({frame_id}, {vector});
}

void WaxStore::PutEmbeddingBatch(const std::vector<std::uint64_t>& frame_ids,
                                 const std::vector<std::vector<float>>& vectors) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (frame_ids.size() != vectors.size()) {
    throw StoreError("PutEmbeddingBatch frame_ids size must match vectors size");
  }
  if (frame_ids.empty()) {
    return;
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);

  std::vector<std::vector<std::byte>> wal_payloads{};
  wal_payloads.reserve(frame_ids.size());
  std::optional<std::size_t> expected_dimension{};
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    if (vectors[i].empty()) {
      throw StoreError("embedding vector must be non-empty");
    }
    if (!expected_dimension.has_value()) {
      expected_dimension = vectors[i].size();
    } else if (*expected_dimension != vectors[i].size()) {
      throw StoreError("PutEmbeddingBatch vectors must all have same dimension");
    }
    wal_payloads.push_back(BuildWalPutEmbeddingPayload(frame_ids[i], vectors[i]));
  }
  const auto sequences = writer.AppendBatch(wal_payloads);
  if (sequences.size() != frame_ids.size()) {
    throw StoreError("wal embedding appendBatch sequence count mismatch");
  }

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  wal_pending_embedding_mutations_ += static_cast<std::uint64_t>(frame_ids.size());
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    const auto sequence = sequences[i];
    pending_mutation_order_cache_.emplace_back(sequence, PendingMutationKind::kPutEmbedding);
    WaxPendingEmbedding entry{};
    entry.frame_id = frame_ids[i];
    entry.dimension = static_cast<std::uint32_t>(vectors[i].size());
    entry.vector = vectors[i];
    const auto embedding_inserted = pending_embedding_cache_.emplace(sequence, std::move(entry));
    if (!embedding_inserted.second) {
      throw StoreError("duplicate pending putEmbedding sequence in cache");
    }
  }
  dirty_ = true;
  has_local_mutations_ = true;
}

void WaxStore::Delete(std::uint64_t frame_id) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (frame_id >= next_frame_id_) {
    throw StoreError("delete frame_id out of range");
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  const auto wal_payload = BuildWalDeletePayload(frame_id);
  const auto sequence = writer.Append(wal_payload);

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  pending_mutation_order_cache_.emplace_back(sequence, PendingMutationKind::kDeleteFrame);
  const auto delete_inserted =
      pending_delete_frame_cache_.emplace(sequence, PendingDeleteFrameCache{.frame_id = frame_id});
  if (!delete_inserted.second) {
    throw StoreError("duplicate pending deleteFrame sequence in cache");
  }
  wal_pending_delete_mutations_ += 1;
  dirty_ = true;
  has_local_mutations_ = true;
}

void WaxStore::Supersede(std::uint64_t superseded_id, std::uint64_t superseding_id) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (superseded_id == superseding_id) {
    throw StoreError("supersede self-reference is not allowed");
  }
  if (superseded_id >= next_frame_id_ || superseding_id >= next_frame_id_) {
    throw StoreError("supersede frame_id out of range");
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  const auto wal_payload = BuildWalSupersedePayload(superseded_id, superseding_id);
  const auto sequence = writer.Append(wal_payload);

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  pending_mutation_order_cache_.emplace_back(sequence, PendingMutationKind::kSupersedeFrame);
  const auto supersede_inserted = pending_supersede_frame_cache_.emplace(
      sequence,
      PendingSupersedeFrameCache{
          .superseded_id = superseded_id,
          .superseding_id = superseding_id,
      });
  if (!supersede_inserted.second) {
    throw StoreError("duplicate pending supersedeFrame sequence in cache");
  }
  wal_pending_supersede_mutations_ += 1;
  dirty_ = true;
  has_local_mutations_ = true;
}

void WaxStore::Commit() {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (!dirty_) {
    return;
  }

  const auto file_size = FileSize(path_);
  const auto footer_slice = TryReadFooterAt(path_, file_size, footer_offset_);
  if (!footer_slice.has_value()) {
    throw StoreError("current footer is missing or invalid");
  }
  auto toc_summary = core::mv2s::DecodeToc(footer_slice->toc_bytes);
  auto frames = toc_summary.frames;

  std::vector<core::wal::WalPendingMutationInfo> pending_mutations{};
  pending_mutations.reserve(pending_mutation_order_cache_.size());
  for (const auto& [sequence, kind] : pending_mutation_order_cache_) {
    core::wal::WalPendingMutationInfo mutation{};
    mutation.sequence = sequence;
    switch (kind) {
      case PendingMutationKind::kPutFrame: {
        const auto it = pending_put_frame_cache_.find(sequence);
        if (it == pending_put_frame_cache_.end()) {
          throw StoreError("pending mutation cache missing putFrame payload");
        }
        const auto& cached = it->second;
        mutation.kind = core::wal::WalMutationKind::kPutFrame;
        mutation.put_frame = core::wal::WalPutFrameInfo{
            .frame_id = cached.frame_id,
            .timestamp_ms = cached.timestamp_ms,
            .payload_offset = cached.payload_offset,
            .payload_length = cached.payload_length,
            .canonical_encoding = cached.canonical_encoding,
            .canonical_length = cached.canonical_length,
            .canonical_checksum = cached.canonical_checksum,
            .stored_checksum = cached.stored_checksum,
            .kind = cached.kind,
            .metadata = cached.metadata,
            .tags = cached.tags,
            .labels = cached.labels,
        };
        break;
      }
      case PendingMutationKind::kDeleteFrame: {
        const auto it = pending_delete_frame_cache_.find(sequence);
        if (it == pending_delete_frame_cache_.end()) {
          throw StoreError("pending mutation cache missing deleteFrame payload");
        }
        mutation.kind = core::wal::WalMutationKind::kDeleteFrame;
        mutation.delete_frame = core::wal::WalDeleteFrameInfo{
            .frame_id = it->second.frame_id,
        };
        break;
      }
      case PendingMutationKind::kSupersedeFrame: {
        const auto it = pending_supersede_frame_cache_.find(sequence);
        if (it == pending_supersede_frame_cache_.end()) {
          throw StoreError("pending mutation cache missing supersedeFrame payload");
        }
        mutation.kind = core::wal::WalMutationKind::kSupersedeFrame;
        mutation.supersede_frame = core::wal::WalSupersedeFrameInfo{
            .superseded_id = it->second.superseded_id,
            .superseding_id = it->second.superseding_id,
        };
        break;
      }
      case PendingMutationKind::kPutEmbedding: {
        const auto it = pending_embedding_cache_.find(sequence);
        if (it == pending_embedding_cache_.end()) {
          throw StoreError("pending mutation cache missing putEmbedding payload");
        }
        mutation.kind = core::wal::WalMutationKind::kPutEmbedding;
        mutation.put_embedding = core::wal::WalPutEmbeddingInfo{
            .frame_id = it->second.frame_id,
            .dimension = it->second.dimension,
            .vector = it->second.vector,
        };
        break;
      }
    }
    pending_mutations.push_back(std::move(mutation));
  }

  const auto committed_seq_after_scan = std::max(wal_committed_seq_, wal_last_sequence_);
  const auto pending_apply = AnalyzePendingWalMutations(frames,
                                                        pending_mutations,
                                                        true,
                                                        footer_slice->footer_offset + core::mv2s::kFooterSize);
  frames = pending_apply.frames_after_apply;

  const auto toc_bytes = core::mv2s::EncodeTocV1(frames);
  if (toc_bytes.size() > core::mv2s::kMaxTocBytes) {
    throw StoreError("TOC size (" + std::to_string(toc_bytes.size() / (1024ULL * 1024ULL)) +
                     " MB) exceeds kMaxTocBytes (" +
                     std::to_string(core::mv2s::kMaxTocBytes / (1024ULL * 1024ULL)) +
                     " MB) with " + std::to_string(frames.size()) + " frames");
  }
  std::uint64_t data_end = wal_offset_ + wal_size_;
  for (const auto& frame : frames) {
    if (frame.payload_length == 0) {
      continue;
    }
    if (frame.payload_offset > std::numeric_limits<std::uint64_t>::max() - frame.payload_length) {
      throw StoreError("frame payload range overflow during commit");
    }
    const auto frame_end = frame.payload_offset + frame.payload_length;
    if (frame_end > data_end) {
      data_end = frame_end;
    }
  }

  std::uint64_t toc_offset = data_end;
  if (footer_offset_ > std::numeric_limits<std::uint64_t>::max() - core::mv2s::kFooterSize) {
    throw StoreError("footer offset overflow while computing append-only TOC placement");
  }
  const auto previous_committed_end = footer_offset_ + core::mv2s::kFooterSize;
  if (toc_offset < previous_committed_end) {
    toc_offset = previous_committed_end;
  }
  const auto footer_offset = toc_offset + toc_bytes.size();
  Footer footer{};
  footer.toc_len = toc_bytes.size();
  std::copy(toc_bytes.end() - 32, toc_bytes.end(), footer.toc_hash.begin());
  footer.generation = file_generation_ + 1;
  footer.wal_committed_seq = committed_seq_after_scan;
  const auto footer_bytes = core::mv2s::EncodeFooter(footer);

  WriteBytesAt(path_, toc_offset, toc_bytes);
  MaybeInjectCommitCrash(1);
  WriteBytesAt(path_, footer_offset, footer_bytes);
  ResizeFile(path_, footer_offset + core::mv2s::kFooterSize);
  MaybeInjectCommitCrash(2);

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  committed_seq_after_scan,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  writer.RecordCheckpoint();
  MaybeInjectCommitCrash(5);

  HeaderPage page_a{};
  page_a.header_page_generation = header_page_generation_ + 1;
  page_a.file_generation = footer.generation;
  page_a.footer_offset = footer_offset;
  page_a.wal_offset = wal_offset_;
  page_a.wal_size = wal_size_;
  page_a.wal_write_pos = writer.write_pos();
  page_a.wal_checkpoint_pos = writer.checkpoint_pos();
  page_a.wal_committed_seq = footer.wal_committed_seq;
  page_a.toc_checksum = footer.toc_hash;
  page_a.replay_snapshot = core::mv2s::ReplaySnapshot{
      .file_generation = footer.generation,
      .wal_committed_seq = footer.wal_committed_seq,
      .footer_offset = footer_offset,
      .wal_write_pos = writer.write_pos(),
      .wal_checkpoint_pos = writer.checkpoint_pos(),
      .wal_pending_bytes = writer.pending_bytes(),
      .wal_last_sequence = writer.last_sequence(),
  };

  auto page_b = page_a;
  page_b.header_page_generation = header_page_generation_;
  const auto page_a_bytes = core::mv2s::EncodeHeaderPage(page_a);
  const auto page_b_bytes = core::mv2s::EncodeHeaderPage(page_b);
  WriteBytesAt(path_, 0, page_a_bytes);
  MaybeInjectCommitCrash(3);
  WriteBytesAt(path_, core::mv2s::kHeaderPageSize, page_b_bytes);
  MaybeInjectCommitCrash(4);

  file_generation_ = footer.generation;
  header_page_generation_ = page_a.header_page_generation;
  wal_committed_seq_ = footer.wal_committed_seq;
  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  wal_pending_embedding_mutations_ = 0;
  wal_pending_delete_mutations_ = 0;
  wal_pending_supersede_mutations_ = 0;
  pending_mutation_order_cache_.clear();
  pending_put_frame_cache_.clear();
  pending_delete_frame_cache_.clear();
  pending_supersede_frame_cache_.clear();
  pending_embedding_cache_.clear();
  footer_offset_ = footer_offset;
  dirty_ = false;
  has_local_mutations_ = false;

  stats_.generation = file_generation_;
  stats_.frame_count = frames.size();
  stats_.pending_frames = 0;
  next_frame_id_ = frames.size();
  committed_frame_metas_ = ToWaxFrameMetas(frames);
}

void WaxStore::Close() {
  bool performed_auto_commit = false;
  if (is_open_ && dirty_ && has_local_mutations_) {
    Commit();
    performed_auto_commit = true;
  }
  if (performed_auto_commit) {
    wal_auto_commit_count_ += 1;
  }
  is_open_ = false;
  writer_lease_.reset();
}

bool WaxStore::TryRefreshIfPublishedCommitVisible() {
  if (!is_open_) {
    throw StoreError("store is closed");
  }

  const auto file_size = FileSize(path_);
  const auto scanned_footer = ScanForLatestFooter(path_, file_size);
  if (!scanned_footer.has_value()) {
    return false;
  }
  if (scanned_footer->footer.generation <= file_generation_) {
    return false;
  }

  LoadState(false, false);
  return true;
}

WaxStats WaxStore::Stats() const {
  return stats_;
}

WaxWALStats WaxStore::WalStats() const {
  WaxWALStats stats{};
  stats.wal_size = wal_size_;
  stats.write_pos = wal_write_pos_;
  stats.checkpoint_pos = wal_checkpoint_pos_;
  stats.pending_bytes = wal_pending_bytes_;
  stats.committed_seq = wal_committed_seq_;
  stats.last_seq = wal_last_sequence_;
  stats.wrap_count = wal_wrap_count_;
  stats.checkpoint_count = wal_checkpoint_count_;
  stats.sentinel_write_count = wal_sentinel_write_count_;
  stats.write_call_count = wal_write_call_count_;
  stats.auto_commit_count = wal_auto_commit_count_;
  stats.pending_embedding_mutations = wal_pending_embedding_mutations_;
  stats.pending_delete_mutations = wal_pending_delete_mutations_;
  stats.pending_supersede_mutations = wal_pending_supersede_mutations_;
  stats.replay_snapshot_hit_count = wal_replay_snapshot_hit_count_;
  return stats;
}

std::optional<WaxFrameMeta> WaxStore::FrameMeta(std::uint64_t frame_id, bool include_pending) const {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (include_pending && !pending_mutation_order_cache_.empty()) {
    const auto metas = FrameMetas(true);
    if (frame_id >= metas.size()) return std::nullopt;
    return metas[static_cast<std::size_t>(frame_id)];
  }
  if (frame_id >= committed_frame_metas_.size()) {
    return std::nullopt;
  }
  return committed_frame_metas_[static_cast<std::size_t>(frame_id)];
}

const std::vector<WaxFrameMeta>& WaxStore::CommittedFrameMetasRef() const {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  return committed_frame_metas_;
}

std::vector<WaxFrameMeta> WaxStore::FrameMetas(bool include_pending) const {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (!include_pending || pending_mutation_order_cache_.empty()) {
    return committed_frame_metas_;
  }

  std::vector<WaxFrameMeta> metas = committed_frame_metas_;
  for (const auto& [sequence, kind] : pending_mutation_order_cache_) {
    switch (kind) {
      case PendingMutationKind::kPutFrame: {
        const auto it = pending_put_frame_cache_.find(sequence);
        if (it == pending_put_frame_cache_.end()) {
          throw StoreError("pending putFrame cache entry missing payload");
        }
        const auto& put = it->second;
        WaxFrameMeta meta{};
        meta.id = put.frame_id;
        meta.timestamp_ms = put.timestamp_ms;
        meta.payload_offset = put.payload_offset;
        meta.payload_length = put.payload_length;
        meta.canonical_encoding = put.canonical_encoding;
        meta.status = 0;
        meta.kind = put.kind;
        meta.metadata = put.metadata;
        meta.tags = put.tags;
        meta.labels = put.labels;

        if (put.frame_id == metas.size()) {
          metas.push_back(std::move(meta));
        } else if (put.frame_id < metas.size()) {
          metas[static_cast<std::size_t>(put.frame_id)] = std::move(meta);
        } else {
          throw StoreError("pending putFrame frame_id is not dense");
        }
        break;
      }
      case PendingMutationKind::kDeleteFrame: {
        const auto it = pending_delete_frame_cache_.find(sequence);
        if (it == pending_delete_frame_cache_.end()) {
          throw StoreError("pending deleteFrame cache entry missing payload");
        }
        if (it->second.frame_id < metas.size()) {
          metas[static_cast<std::size_t>(it->second.frame_id)].status = 1;
        }
        break;
      }
      case PendingMutationKind::kSupersedeFrame: {
        const auto it = pending_supersede_frame_cache_.find(sequence);
        if (it == pending_supersede_frame_cache_.end()) {
          throw StoreError("pending supersedeFrame cache entry missing payload");
        }
        if (it->second.superseded_id < metas.size() && it->second.superseding_id < metas.size()) {
          metas[static_cast<std::size_t>(it->second.superseded_id)].superseded_by = it->second.superseding_id;
          metas[static_cast<std::size_t>(it->second.superseding_id)].supersedes = it->second.superseded_id;
        }
        break;
      }
      case PendingMutationKind::kPutEmbedding:
        break;
    }
  }
  return metas;
}

std::vector<std::byte> WaxStore::FrameContent(std::uint64_t frame_id, bool include_pending) const {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  // Avoid copying entire vector when no pending mutations exist.
  const std::vector<WaxFrameMeta>* metas_ptr = &committed_frame_metas_;
  std::vector<WaxFrameMeta> pending_metas_storage;
  if (include_pending && !pending_mutation_order_cache_.empty()) {
    pending_metas_storage = FrameMetas(true);
    metas_ptr = &pending_metas_storage;
  }
  const auto& metas = *metas_ptr;
  if (frame_id >= metas.size()) {
    throw StoreError("frame_id out of range");
  }
  const auto& meta = metas[static_cast<std::size_t>(frame_id)];
  if (meta.payload_length == 0) {
    return {};
  }
  if (meta.payload_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw StoreError("payload_length exceeds addressable memory");
  }
  return ReadExactly(path_, meta.payload_offset, static_cast<std::size_t>(meta.payload_length));
}

std::unordered_map<std::uint64_t, std::vector<std::byte>> WaxStore::FrameContents(
    const std::vector<std::uint64_t>& frame_ids) const {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  std::unordered_map<std::uint64_t, std::vector<std::byte>> out{};
  out.reserve(frame_ids.size());
  for (const auto frame_id : frame_ids) {
    out.emplace(frame_id, FrameContent(frame_id));
  }
  return out;
}

WaxPendingEmbeddingSnapshot WaxStore::PendingEmbeddingMutations(
    std::optional<std::uint64_t> since_sequence) const {
  if (!is_open_) {
    throw StoreError("store is closed");
  }

  WaxPendingEmbeddingSnapshot snapshot{};
  for (const auto& [sequence, kind] : pending_mutation_order_cache_) {
    if (kind != PendingMutationKind::kPutEmbedding) {
      continue;
    }
    const auto it = pending_embedding_cache_.find(sequence);
    if (it == pending_embedding_cache_.end()) {
      throw StoreError("pending embedding cache entry missing payload");
    }
    snapshot.latest_sequence = sequence;
    if (since_sequence.has_value() && sequence <= *since_sequence) {
      continue;
    }
    snapshot.embeddings.push_back(it->second);
  }
  return snapshot;
}

WaxStore::WaxStore(std::filesystem::path path) : path_(std::move(path)) {}

void WaxStore::LoadState(bool deep_verify, bool repair_trailing_bytes) {
  if (!std::filesystem::exists(path_)) {
    throw StoreError("store file does not exist: " + path_.string());
  }
  auto file_size = FileSize(path_);
  std::cerr << "[STORE-TRACE] LoadState: file_size=" << (file_size / (1024ULL * 1024ULL)) << " MB"
            << " deep_verify=" << deep_verify << " repair=" << repair_trailing_bytes << std::endl;
  if (file_size < core::mv2s::kHeaderRegionSize + core::mv2s::kFooterSize) {
    throw StoreError("file is too small to be a valid mv2s store");
  }

  auto phase_start = std::chrono::steady_clock::now();
  auto PhaseMs = [&]() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_start).count();
    phase_start = now;
    return ms;
  };

  const auto page_a = TryDecodeHeader(path_, 0);
  const auto page_b = TryDecodeHeader(path_, core::mv2s::kHeaderPageSize);
  std::cerr << "[STORE-TRACE] headers decoded (" << PhaseMs() << " ms)" << std::endl;
  if (!page_a.has_value() && !page_b.has_value()) {
    throw StoreError("no valid header pages");
  }

  HeaderPage selected{};
  if (page_a.has_value() && page_b.has_value()) {
    selected = page_a->header_page_generation >= page_b->header_page_generation ? *page_a : *page_b;
  } else if (page_a.has_value()) {
    selected = *page_a;
  } else {
    selected = *page_b;
  }

  std::cerr << "[STORE-TRACE] >> TryReadFooterAt (header offset="
            << selected.footer_offset << ")..." << std::endl;
  const auto footer_from_header = TryReadFooterAt(path_, file_size, selected.footer_offset);
  std::cerr << "[STORE-TRACE] << TryReadFooterAt done (" << PhaseMs() << " ms)"
            << " found=" << footer_from_header.has_value() << std::endl;

  std::optional<FooterSlice> footer_from_snapshot;
  if (selected.replay_snapshot.has_value()) {
    std::cerr << "[STORE-TRACE] >> TryReadFooterAt (snapshot offset="
              << selected.replay_snapshot->footer_offset << ")..." << std::endl;
    footer_from_snapshot = TryReadFooterAt(path_, file_size, selected.replay_snapshot->footer_offset);
    std::cerr << "[STORE-TRACE] << TryReadFooterAt snapshot done (" << PhaseMs() << " ms)" << std::endl;
  }

  std::cerr << "[STORE-TRACE] >> ScanForLatestFooter (file_size=" << (file_size / (1024ULL*1024ULL))
            << " MB)..." << std::endl;
  const auto footer_from_scan = ScanForLatestFooter(path_, file_size);
  std::cerr << "[STORE-TRACE] << ScanForLatestFooter done (" << PhaseMs() << " ms)"
            << " found=" << footer_from_scan.has_value() << std::endl;

  auto footer_slice = SelectPreferredFooter(footer_from_header, footer_from_snapshot);
  footer_slice = SelectPreferredFooter(footer_slice, footer_from_scan);
  if (!footer_slice.has_value()) {
    throw StoreError("no valid footer slice found");
  }

  std::cerr << "[STORE-TRACE] >> DecodeToc (toc_bytes=" << (footer_slice->toc_bytes.size() / (1024ULL*1024ULL))
            << " MB)..." << std::endl;
  const auto toc_summary = core::mv2s::DecodeToc(footer_slice->toc_bytes);
  std::cerr << "[STORE-TRACE] << DecodeToc done (" << PhaseMs() << " ms)"
            << " frames=" << toc_summary.frames.size() << std::endl;
  const auto data_start = selected.wal_offset + selected.wal_size;
  const auto data_end = footer_slice->footer_offset;
  ValidateDataRanges(toc_summary.frames, toc_summary.segments, data_start, data_end);
  if (deep_verify) {
    DeepVerifyFrames(path_, toc_summary.frames);
    DeepVerifySegments(path_, toc_summary.segments);
  }

  const auto committed_seq = footer_slice->footer.wal_committed_seq;
  const auto selected_header_was_stale = selected.file_generation != footer_slice->footer.generation;
  bool used_replay_snapshot = false;
  std::vector<core::wal::WalPendingMutationInfo> pending_mutations{};
  core::wal::WalScanState wal_scan_state{};

  try {
    const auto replay_snapshot_matches_footer = selected.replay_snapshot.has_value() &&
                                                selected.replay_snapshot->file_generation == footer_slice->footer.generation &&
                                                selected.replay_snapshot->wal_committed_seq == committed_seq &&
                                                selected.replay_snapshot->footer_offset == footer_slice->footer_offset;

    if (replay_snapshot_matches_footer &&
        selected.replay_snapshot->wal_checkpoint_pos == selected.replay_snapshot->wal_write_pos &&
        core::wal::IsTerminalMarker(path_,
                                    selected.wal_offset,
                                    selected.wal_size,
                                    selected.replay_snapshot->wal_write_pos)) {
      used_replay_snapshot = true;
      wal_scan_state.last_sequence = std::max(committed_seq, selected.replay_snapshot->wal_last_sequence);
      wal_scan_state.write_pos = selected.replay_snapshot->wal_write_pos % selected.wal_size;
      wal_scan_state.pending_bytes = 0;
    } else if (!selected_header_was_stale &&
               selected.wal_checkpoint_pos == selected.wal_write_pos &&
               core::wal::IsTerminalMarker(path_,
                                           selected.wal_offset,
                                           selected.wal_size,
                                           selected.wal_write_pos)) {
      used_replay_snapshot = true;
      wal_scan_state.last_sequence = committed_seq;
      wal_scan_state.write_pos = selected.wal_write_pos % selected.wal_size;
      wal_scan_state.pending_bytes = 0;
    } else {
      auto pending_scan = core::wal::ScanPendingMutationsWithState(path_,
                                                                   selected.wal_offset,
                                                                   selected.wal_size,
                                                                   selected.wal_checkpoint_pos,
                                                                   committed_seq);
      wal_scan_state = pending_scan.state;
      pending_mutations = std::move(pending_scan.pending_mutations);
    }
  } catch (const std::exception& ex) {
    throw StoreError(std::string("wal scan failed: ") + ex.what());
  }

  const auto last_sequence = std::max(committed_seq, wal_scan_state.last_sequence);
  std::uint64_t effective_checkpoint_pos = 0;
  std::uint64_t effective_pending_bytes = 0;
  if (wal_scan_state.last_sequence <= committed_seq) {
    effective_checkpoint_pos = wal_scan_state.write_pos;
    effective_pending_bytes = 0;
  } else {
    effective_checkpoint_pos = selected.wal_checkpoint_pos % selected.wal_size;
    effective_pending_bytes = wal_scan_state.pending_bytes;
  }

  const auto pending_analysis = AnalyzePendingWalMutations(toc_summary.frames,
                                                            pending_mutations,
                                                            false,
                                                            footer_slice->footer_offset + core::mv2s::kFooterSize);
  const auto required_end = pending_analysis.required_end;
  if (required_end > file_size) {
    throw StoreError("pending WAL references bytes beyond file size");
  }
  if (repair_trailing_bytes && file_size > required_end) {
    std::error_code ec;
    std::filesystem::resize_file(path_, required_end, ec);
    if (ec) {
      throw StoreError("failed to truncate trailing bytes: " + ec.message());
    }
    file_size = required_end;
  }

  file_generation_ = footer_slice->footer.generation;
  header_page_generation_ = selected.header_page_generation;
  wal_offset_ = selected.wal_offset;
  wal_size_ = selected.wal_size;
  wal_committed_seq_ = committed_seq;
  wal_write_pos_ = wal_scan_state.write_pos;
  wal_checkpoint_pos_ = effective_checkpoint_pos;
  wal_pending_bytes_ = effective_pending_bytes;
  wal_last_sequence_ = last_sequence;
  wal_wrap_count_ = 0;
  wal_checkpoint_count_ = 0;
  wal_sentinel_write_count_ = 0;
  wal_write_call_count_ = 0;
  wal_auto_commit_count_ = 0;
  wal_pending_embedding_mutations_ = pending_analysis.pending_embedding_mutations;
  wal_pending_delete_mutations_ = pending_analysis.pending_delete_mutations;
  wal_pending_supersede_mutations_ = pending_analysis.pending_supersede_mutations;
  wal_replay_snapshot_hit_count_ = used_replay_snapshot ? 1U : 0U;
  footer_offset_ = footer_slice->footer_offset;
  next_frame_id_ = pending_analysis.max_frame_id_plus_one;
  pending_mutation_order_cache_.clear();
  pending_put_frame_cache_.clear();
  pending_delete_frame_cache_.clear();
  pending_supersede_frame_cache_.clear();
  pending_embedding_cache_.clear();

  if (pending_mutations.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
    throw StoreError("pending mutation count exceeds UInt64.max");
  }
  pending_mutation_order_cache_.reserve(pending_mutations.size());
  for (const auto& mutation : pending_mutations) {
    switch (mutation.kind) {
      case core::wal::WalMutationKind::kPutFrame: {
        if (!mutation.put_frame.has_value()) {
          throw StoreError("pending putFrame mutation missing payload");
        }
        const auto& put = *mutation.put_frame;
        pending_mutation_order_cache_.emplace_back(mutation.sequence, PendingMutationKind::kPutFrame);
        const auto put_inserted = pending_put_frame_cache_.emplace(
            mutation.sequence,
            PendingPutFrameCache{
                .frame_id = put.frame_id,
                .timestamp_ms = put.timestamp_ms,
                .payload_offset = put.payload_offset,
                .payload_length = put.payload_length,
                .canonical_encoding = put.canonical_encoding,
                .canonical_length = put.canonical_length,
                .canonical_checksum = put.canonical_checksum,
                .stored_checksum = put.stored_checksum,
                .kind = put.kind,
                .metadata = put.metadata,
                .tags = put.tags,
                .labels = put.labels,
            });
        if (!put_inserted.second) {
          throw StoreError("duplicate pending putFrame sequence in recovery cache");
        }
        break;
      }
      case core::wal::WalMutationKind::kDeleteFrame: {
        if (!mutation.delete_frame.has_value()) {
          throw StoreError("pending deleteFrame mutation missing payload");
        }
        pending_mutation_order_cache_.emplace_back(mutation.sequence, PendingMutationKind::kDeleteFrame);
        const auto delete_inserted = pending_delete_frame_cache_.emplace(
            mutation.sequence,
            PendingDeleteFrameCache{
                .frame_id = mutation.delete_frame->frame_id,
            });
        if (!delete_inserted.second) {
          throw StoreError("duplicate pending deleteFrame sequence in recovery cache");
        }
        break;
      }
      case core::wal::WalMutationKind::kSupersedeFrame: {
        if (!mutation.supersede_frame.has_value()) {
          throw StoreError("pending supersedeFrame mutation missing payload");
        }
        pending_mutation_order_cache_.emplace_back(mutation.sequence, PendingMutationKind::kSupersedeFrame);
        const auto supersede_inserted = pending_supersede_frame_cache_.emplace(
            mutation.sequence,
            PendingSupersedeFrameCache{
                .superseded_id = mutation.supersede_frame->superseded_id,
                .superseding_id = mutation.supersede_frame->superseding_id,
            });
        if (!supersede_inserted.second) {
          throw StoreError("duplicate pending supersedeFrame sequence in recovery cache");
        }
        break;
      }
      case core::wal::WalMutationKind::kPutEmbedding: {
        if (!mutation.put_embedding.has_value()) {
          throw StoreError("pending putEmbedding mutation missing payload");
        }
        pending_mutation_order_cache_.emplace_back(mutation.sequence, PendingMutationKind::kPutEmbedding);
        WaxPendingEmbedding entry{};
        entry.frame_id = mutation.put_embedding->frame_id;
        entry.dimension = mutation.put_embedding->dimension;
        entry.vector = mutation.put_embedding->vector;
        const auto embedding_inserted = pending_embedding_cache_.emplace(mutation.sequence, std::move(entry));
        if (!embedding_inserted.second) {
          throw StoreError("duplicate pending putEmbedding sequence in recovery cache");
        }
        break;
      }
    }
  }
  dirty_ = wal_scan_state.last_sequence > committed_seq;
  has_local_mutations_ = false;
  is_open_ = true;
  (void)file_size;
  (void)used_replay_snapshot;

  stats_.generation = file_generation_;
  stats_.frame_count = toc_summary.frame_count;
  stats_.pending_frames = pending_analysis.pending_put_frames;
  committed_frame_metas_ = ToWaxFrameMetas(toc_summary.frames);
}

}  // namespace waxcpp
