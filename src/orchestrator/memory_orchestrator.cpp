#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/fts5_search_engine.hpp"
#include "waxcpp/live_set_rewrite.hpp"
#include "waxcpp/maintenance.hpp"
#include "waxcpp/query_analyzer.hpp"
#include "waxcpp/search.hpp"
#include "waxcpp/surrogate_generator.hpp"
#include "waxcpp/text_chunker.hpp"
#include "waxcpp/token_counter.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

namespace waxcpp {
namespace {

bool IsAsciiWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

bool HasNonWhitespace(std::string_view text) {
  for (const char ch : text) {
    if (!IsAsciiWhitespace(ch)) {
      return true;
    }
  }
  return false;
}

std::string ToAsciiLower(std::string_view text) {
  std::string out{};
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch >= 'A' && ch <= 'Z') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

bool HasAsciiControlChars(std::string_view text) {
  for (const char ch : text) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 0x20U || u == 0x7FU) {
      return true;
    }
  }
  return false;
}

inline constexpr std::array<std::byte, 6> kStructuredFactMagic = {
    std::byte{'W'},
    std::byte{'A'},
    std::byte{'X'},
    std::byte{'S'},
    std::byte{'M'},
    std::byte{'1'},
};

inline constexpr std::array<std::byte, 6> kEmbeddingRecordMagic = {
    std::byte{'W'},
    std::byte{'A'},
    std::byte{'X'},
    std::byte{'E'},
    std::byte{'M'},
    std::byte{'1'},
};

inline constexpr std::array<std::byte, 6> kEmbeddingRecordMagicV2 = {
    std::byte{'W'},
    std::byte{'A'},
    std::byte{'X'},
    std::byte{'E'},
    std::byte{'M'},
    std::byte{'2'},
};

constexpr std::uint32_t kMaxEmbeddingRecordValues = 16384;
constexpr std::uint32_t kMaxEmbeddingIdentityTagBytes = 4096;
constexpr std::uint32_t kMaxStructuredFactFieldBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kMaxStructuredFactMetadataPairs = 16384U;
constexpr std::uint32_t kMaxStructuredFactPayloadBytes = 8U * 1024U * 1024U;

enum class StructuredFactOpcode : std::uint8_t {
  kUpsert = 1,
  kRemove = 2,
};

struct StructuredFactRecord {
  StructuredFactOpcode opcode = StructuredFactOpcode::kUpsert;
  std::string entity;
  std::string attribute;
  std::string value;
  Metadata metadata;
};

struct EmbeddingRecord {
  std::uint64_t frame_id = 0;
  std::vector<float> embedding;
  std::optional<std::string> identity_tag{};
};

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendU64LE(std::vector<std::byte>& out, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendU32LE(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendF32LE(std::vector<std::byte>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32LE(out, bits);
}

void AppendString(std::vector<std::byte>& out, const std::string& value) {
  if (value.size() > static_cast<std::size_t>(kMaxStructuredFactFieldBytes)) {
    throw std::runtime_error("structured fact field exceeds replay safety limit");
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("structured fact field exceeds uint32 length");
  }
  AppendU32LE(out, static_cast<std::uint32_t>(value.size()));
  for (const char ch : value) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
}

std::vector<std::byte> BuildStructuredFactUpsertPayload(const std::string& entity,
                                                        const std::string& attribute,
                                                        const std::string& value,
                                                        const Metadata& metadata) {
  std::uint64_t estimated_size = static_cast<std::uint64_t>(kStructuredFactMagic.size()) + 1U + 4U +
                                 static_cast<std::uint64_t>(entity.size()) + 4U +
                                 static_cast<std::uint64_t>(attribute.size()) + 4U +
                                 static_cast<std::uint64_t>(value.size()) + 4U;
  if (estimated_size > kMaxStructuredFactPayloadBytes) {
    throw std::runtime_error("structured fact payload exceeds replay safety limit");
  }
  for (const auto& [key, val] : metadata) {
    estimated_size += 4U + static_cast<std::uint64_t>(key.size()) + 4U + static_cast<std::uint64_t>(val.size());
    if (estimated_size > kMaxStructuredFactPayloadBytes) {
      throw std::runtime_error("structured fact payload exceeds replay safety limit");
    }
  }

  std::vector<std::byte> out{};
  out.reserve(64 + entity.size() + attribute.size() + value.size());
  out.insert(out.end(), kStructuredFactMagic.begin(), kStructuredFactMagic.end());
  AppendU8(out, static_cast<std::uint8_t>(StructuredFactOpcode::kUpsert));
  AppendString(out, entity);
  AppendString(out, attribute);
  AppendString(out, value);
  if (metadata.size() > static_cast<std::size_t>(kMaxStructuredFactMetadataPairs)) {
    throw std::runtime_error("structured fact metadata count exceeds replay safety limit");
  }
  if (metadata.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("structured fact metadata count exceeds uint32");
  }
  AppendU32LE(out, static_cast<std::uint32_t>(metadata.size()));
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
    const auto& key = entry->first;
    const auto& val = entry->second;
    AppendString(out, key);
    AppendString(out, val);
  }
  if (out.size() > static_cast<std::size_t>(kMaxStructuredFactPayloadBytes)) {
    throw std::runtime_error("structured fact payload exceeds replay safety limit");
  }
  return out;
}

std::vector<std::byte> BuildStructuredFactRemovePayload(const std::string& entity,
                                                        const std::string& attribute) {
  const std::uint64_t estimated_size = static_cast<std::uint64_t>(kStructuredFactMagic.size()) + 1U + 4U +
                                       static_cast<std::uint64_t>(entity.size()) + 4U +
                                       static_cast<std::uint64_t>(attribute.size());
  if (estimated_size > kMaxStructuredFactPayloadBytes) {
    throw std::runtime_error("structured fact payload exceeds replay safety limit");
  }

  std::vector<std::byte> out{};
  out.reserve(32 + entity.size() + attribute.size());
  out.insert(out.end(), kStructuredFactMagic.begin(), kStructuredFactMagic.end());
  AppendU8(out, static_cast<std::uint8_t>(StructuredFactOpcode::kRemove));
  AppendString(out, entity);
  AppendString(out, attribute);
  if (out.size() > static_cast<std::size_t>(kMaxStructuredFactPayloadBytes)) {
    throw std::runtime_error("structured fact payload exceeds replay safety limit");
  }
  return out;
}

std::optional<std::string> BuildEmbeddingIdentityTag(const std::optional<EmbeddingIdentity>& identity) {
  if (!identity.has_value()) {
    return std::nullopt;
  }

  std::string out{};
  out.reserve(96);
  out.append("provider=");
  out.append(identity->provider.value_or(""));
  out.append(";model=");
  out.append(identity->model.value_or(""));
  out.append(";dimensions=");
  if (identity->dimensions.has_value()) {
    out.append(std::to_string(*identity->dimensions));
  }
  out.append(";normalized=");
  if (identity->normalized.has_value()) {
    out.append(*identity->normalized ? "true" : "false");
  }
  if (HasAsciiControlChars(out)) {
    // Preserve replay compatibility: control bytes in identity payload must not emit malformed WAXEM2 records.
    return std::nullopt;
  }
  if (out.size() > kMaxEmbeddingIdentityTagBytes) {
    // Keep replay-format compatibility: oversized identities must not be serialized as malformed WAXEM2.
    return std::nullopt;
  }
  return out;
}

std::vector<std::byte> BuildEmbeddingRecordPayload(std::uint64_t frame_id,
                                                   const std::vector<float>& embedding,
                                                   const std::optional<std::string>& identity_tag) {
  if (embedding.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("embedding record vector length exceeds uint32");
  }
  if (identity_tag.has_value() &&
      identity_tag->size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("embedding identity tag exceeds uint32 length");
  }
  std::vector<std::byte> out{};
  if (identity_tag.has_value()) {
    out.reserve(kEmbeddingRecordMagicV2.size() + 8 + 4 + 4 + identity_tag->size() + embedding.size() * sizeof(float));
    out.insert(out.end(), kEmbeddingRecordMagicV2.begin(), kEmbeddingRecordMagicV2.end());
  } else {
    out.reserve(kEmbeddingRecordMagic.size() + 8 + 4 + embedding.size() * sizeof(float));
    out.insert(out.end(), kEmbeddingRecordMagic.begin(), kEmbeddingRecordMagic.end());
  }
  AppendU64LE(out, frame_id);
  AppendU32LE(out, static_cast<std::uint32_t>(embedding.size()));
  if (identity_tag.has_value()) {
    AppendU32LE(out, static_cast<std::uint32_t>(identity_tag->size()));
    for (const char ch : *identity_tag) {
      out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
  }
  for (const float value : embedding) {
    AppendF32LE(out, value);
  }
  return out;
}

std::optional<StructuredFactRecord> ParseStructuredFactPayload(const std::vector<std::byte>& payload) {
  if (payload.size() > static_cast<std::size_t>(kMaxStructuredFactPayloadBytes)) {
    return std::nullopt;
  }
  if (payload.size() < kStructuredFactMagic.size() + 1 + 4 + 4) {
    return std::nullopt;
  }
  if (!std::equal(kStructuredFactMagic.begin(), kStructuredFactMagic.end(), payload.begin())) {
    return std::nullopt;
  }

  std::size_t cursor = kStructuredFactMagic.size();
  auto read_u8 = [&]() -> std::optional<std::uint8_t> {
    if (cursor >= payload.size()) {
      return std::nullopt;
    }
    return std::to_integer<std::uint8_t>(payload[cursor++]);
  };
  auto read_u32 = [&]() -> std::optional<std::uint32_t> {
    if (cursor + 4 > payload.size()) {
      return std::nullopt;
    }
    std::uint32_t out = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[cursor + i])) << (8U * i);
    }
    cursor += 4;
    return out;
  };
  auto read_string = [&]() -> std::optional<std::string> {
    const auto length = read_u32();
    if (!length.has_value()) {
      return std::nullopt;
    }
    if (*length > kMaxStructuredFactFieldBytes) {
      return std::nullopt;
    }
    if (cursor + *length > payload.size()) {
      return std::nullopt;
    }
    std::string out{};
    out.reserve(*length);
    for (std::size_t i = 0; i < *length; ++i) {
      out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(payload[cursor + i])));
    }
    cursor += *length;
    return out;
  };

  const auto opcode_u8 = read_u8();
  if (!opcode_u8.has_value()) {
    return std::nullopt;
  }
  StructuredFactRecord record{};
  if (*opcode_u8 == static_cast<std::uint8_t>(StructuredFactOpcode::kUpsert)) {
    const auto entity = read_string();
    const auto attribute = read_string();
    const auto value = read_string();
    const auto metadata_count = read_u32();
    if (!entity.has_value() || !attribute.has_value() || !value.has_value() || !metadata_count.has_value()) {
      return std::nullopt;
    }
    if (*metadata_count > kMaxStructuredFactMetadataPairs) {
      return std::nullopt;
    }
    Metadata metadata{};
    for (std::uint32_t i = 0; i < *metadata_count; ++i) {
      const auto key = read_string();
      const auto val = read_string();
      if (!key.has_value() || !val.has_value()) {
        return std::nullopt;
      }
      if (metadata.contains(*key)) {
        return std::nullopt;
      }
      metadata.emplace(*key, *val);
    }
    record.opcode = StructuredFactOpcode::kUpsert;
    record.entity = *entity;
    record.attribute = *attribute;
    if (record.entity.empty() || record.attribute.empty()) {
      return std::nullopt;
    }
    record.value = *value;
    record.metadata = std::move(metadata);
  } else if (*opcode_u8 == static_cast<std::uint8_t>(StructuredFactOpcode::kRemove)) {
    const auto entity = read_string();
    const auto attribute = read_string();
    if (!entity.has_value() || !attribute.has_value()) {
      return std::nullopt;
    }
    record.opcode = StructuredFactOpcode::kRemove;
    record.entity = *entity;
    record.attribute = *attribute;
    if (record.entity.empty() || record.attribute.empty()) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  if (cursor != payload.size()) {
    return std::nullopt;
  }
  return record;
}

std::optional<EmbeddingRecord> ParseEmbeddingRecordPayload(const std::vector<std::byte>& payload) {
  if (payload.size() < kEmbeddingRecordMagic.size() + 8 + 4) {
    return std::nullopt;
  }
  bool is_v1 = false;
  bool is_v2 = false;
  if (std::equal(kEmbeddingRecordMagic.begin(), kEmbeddingRecordMagic.end(), payload.begin())) {
    is_v1 = true;
  } else if (std::equal(kEmbeddingRecordMagicV2.begin(), kEmbeddingRecordMagicV2.end(), payload.begin())) {
    is_v2 = true;
  } else {
    return std::nullopt;
  }

  std::size_t cursor = is_v2 ? kEmbeddingRecordMagicV2.size() : kEmbeddingRecordMagic.size();
  auto read_u64 = [&]() -> std::optional<std::uint64_t> {
    if (cursor + 8 > payload.size()) {
      return std::nullopt;
    }
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      out |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(payload[cursor + i])) << (8U * i);
    }
    cursor += 8;
    return out;
  };
  auto read_u32 = [&]() -> std::optional<std::uint32_t> {
    if (cursor + 4 > payload.size()) {
      return std::nullopt;
    }
    std::uint32_t out = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[cursor + i])) << (8U * i);
    }
    cursor += 4;
    return out;
  };
  auto read_f32 = [&]() -> std::optional<float> {
    const auto bits = read_u32();
    if (!bits.has_value()) {
      return std::nullopt;
    }
    float value = 0.0F;
    std::uint32_t raw = *bits;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  };

  const auto frame_id = read_u64();
  const auto count = read_u32();
  if (!frame_id.has_value() || !count.has_value()) {
    return std::nullopt;
  }
  if (*count > kMaxEmbeddingRecordValues) {
    return std::nullopt;
  }

  std::optional<std::string> identity_tag{};
  if (is_v2) {
    const auto identity_len = read_u32();
    if (!identity_len.has_value()) {
      return std::nullopt;
    }
    if (*identity_len == 0) {
      return std::nullopt;
    }
    if (*identity_len > kMaxEmbeddingIdentityTagBytes) {
      return std::nullopt;
    }
    if (cursor + *identity_len > payload.size()) {
      return std::nullopt;
    }
    std::string tag{};
    tag.reserve(*identity_len);
    for (std::size_t i = 0; i < *identity_len; ++i) {
      tag.push_back(static_cast<char>(std::to_integer<std::uint8_t>(payload[cursor + i])));
    }
    if (HasAsciiControlChars(tag)) {
      return std::nullopt;
    }
    identity_tag = std::move(tag);
    cursor += *identity_len;
  }

  const auto remaining_bytes = payload.size() - cursor;
  if (remaining_bytes / sizeof(std::uint32_t) < *count) {
    return std::nullopt;
  }

  std::vector<float> embedding{};
  embedding.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const auto value = read_f32();
    if (!value.has_value()) {
      return std::nullopt;
    }
    embedding.push_back(*value);
  }
  if (cursor != payload.size()) {
    return std::nullopt;
  }

  EmbeddingRecord record{};
  record.frame_id = *frame_id;
  record.embedding = std::move(embedding);
  record.identity_tag = std::move(identity_tag);
  return record;
}

std::string BytesToString(const std::vector<std::byte>& payload) {
  std::string out{};
  out.reserve(payload.size());
  for (const auto b : payload) {
    out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return out;
}

bool AllFinite(const std::vector<float>& values) {
  for (const float value : values) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

bool StartsWithMagic(std::span<const std::byte> payload, std::span<const std::byte> magic) {
  if (payload.size() < magic.size()) {
    return false;
  }
  return std::equal(magic.begin(), magic.end(), payload.begin());
}

bool IsInternalOrchestratorPayload(const std::vector<std::byte>& payload) {
  if (StartsWithMagic(payload, kStructuredFactMagic) ||
      StartsWithMagic(payload, kEmbeddingRecordMagic) ||
      StartsWithMagic(payload, kEmbeddingRecordMagicV2)) {
    // Reserved internal payload namespace: malformed internal records must never leak to user channels.
    return true;
  }
  return false;
}

void ThrowIfClosed(bool closed) {
  if (closed) {
    throw std::runtime_error("memory orchestrator is closed");
  }
}

struct StoreSearchChannels {
  std::vector<SearchResult> text_results;
  std::vector<SearchResult> vector_results;
};

/// Returns true if the frame metadata indicates a user-content frame
/// (e.g. UE5 chunk, remembered text) which is guaranteed NOT to be
/// a structured fact payload. This avoids a disk read for the payload.
///
/// IMPORTANT: Enricher-produced fact frames use "source_chunk_id" (not "chunk_id")
/// and "enricher_kind" (not "source_kind") to avoid matching these heuristics.
/// If you add metadata keys to fact frames, make sure they don't collide with
/// the keys checked here, or facts will be silently dropped during replay.
bool IsKnownUserContentFrame(const WaxFrameMeta& meta) {
  // User-content frames have application metadata keys that internal
  // structured-fact frames never have.
  const auto& md = meta.metadata;
  // Enricher fact frames have "enricher_kind" — never skip those, even if
  // they also carry legacy "chunk_id" (pre-fix enrichers used "chunk_id"
  // instead of "source_chunk_id").
  if (md.find("enricher_kind") != md.end()) return false;
  if (md.find("source_kind") != md.end()) return true;
  if (md.find("chunk_id") != md.end()) return true;
  if (md.find("language") != md.end()) return true;
  if (md.find("symbol") != md.end()) return true;
  return false;
}

void ReplayStructuredFactsFromStore(WaxStore& store, StructuredMemoryStore& structured_memory) {
  const auto t_start = std::chrono::steady_clock::now();
  const auto& metas = store.CommittedFrameMetasRef();

  // ── Pass 1: collect candidate frame indices (fast metadata scan, no I/O) ──
  struct CandidateRead {
    std::uint64_t payload_offset;
    std::uint64_t payload_length;
    std::size_t   meta_index;
  };
  std::vector<CandidateRead> candidates;
  candidates.reserve(metas.size() / 2);  // rough estimate: ~50% are facts
  std::uint64_t skipped_user = 0;
  std::uint64_t skipped_status = 0;
  for (std::size_t i = 0; i < metas.size(); ++i) {
    const auto& meta = metas[i];
    if (meta.status != 0) { ++skipped_status; continue; }
    if (IsKnownUserContentFrame(meta)) { ++skipped_user; continue; }
    if (meta.payload_length == 0) continue;
    candidates.push_back({meta.payload_offset, meta.payload_length, i});
  }

  // Sort by payload_offset → sequential disk reads instead of random I/O.
  std::sort(candidates.begin(), candidates.end(),
            [](const CandidateRead& a, const CandidateRead& b) {
              return a.payload_offset < b.payload_offset;
            });

  const auto t_pass1 = std::chrono::steady_clock::now();
  const auto pass1_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_pass1 - t_start).count();
  std::cerr << "[REBUILD-TRACE] ReplayStructuredFacts pass1: candidates=" << candidates.size()
            << " skipped_user=" << skipped_user << " skipped_status=" << skipped_status
            << " (" << pass1_ms << " ms)" << std::endl;

  // ── Pass 2: buffered sequential reads (4MB look-ahead buffer) ──
  // Instead of 649K individual seekg+read calls (each ~2-4ms on Windows),
  // read large contiguous blocks and extract payloads from the buffer.
  std::uint64_t facts_found = 0;
  std::uint64_t payload_reads = 0;
  std::uint64_t not_fact = 0;
  std::uint64_t block_reads = 0;

  std::ifstream in(store.Path(), std::ios::binary);
  if (!in) {
    std::cerr << "[REBUILD-TRACE] ReplayStructuredFacts: FAILED to open store file" << std::endl;
    return;
  }

  constexpr std::size_t kBlockSize = 4 * 1024 * 1024;  // 4 MB read-ahead
  constexpr std::size_t kMaxFactPayload = 32 * 1024;
  std::vector<char> block_buf(kBlockSize);
  std::uint64_t buf_start = 0;  // file offset of block_buf[0]
  std::uint64_t buf_end = 0;    // file offset of block_buf[bytes_in_buf]

  auto t_last_log = t_pass1;
  for (const auto& c : candidates) {
    const auto read_len = static_cast<std::size_t>(
        std::min(c.payload_length, static_cast<std::uint64_t>(kMaxFactPayload)));
    const auto end_offset = c.payload_offset + read_len;

    // Check if payload is within current buffer
    if (c.payload_offset < buf_start || end_offset > buf_end) {
      // Need new block read — position buffer so this payload starts near the beginning
      buf_start = c.payload_offset;
      in.seekg(static_cast<std::streamoff>(buf_start), std::ios::beg);
      in.read(block_buf.data(), static_cast<std::streamsize>(kBlockSize));
      const auto got = static_cast<std::uint64_t>(in.gcount());
      buf_end = buf_start + got;
      in.clear();  // clear eofbit if we hit end of file
      ++block_reads;
      if (end_offset > buf_end) {
        continue;  // payload extends past EOF — skip
      }
    }

    // Extract payload from buffer (zero-copy parse)
    const auto buf_offset = static_cast<std::size_t>(c.payload_offset - buf_start);
    std::vector<std::byte> payload(read_len);
    std::memcpy(payload.data(), block_buf.data() + buf_offset, read_len);
    ++payload_reads;

    const auto fact = ParseStructuredFactPayload(payload);
    if (!fact.has_value()) {
      ++not_fact;
      continue;
    }
    ++facts_found;
    // Use StageUpsert (not Upsert!) to avoid O(n²) copy-on-each-commit.
    // Upsert() calls StageUpsert + CommitStaged, and each CommitStaged
    // clears pending_mutations, so the NEXT StageUpsert copies the entire
    // entries_ map into staged_entries_. With 649K facts this is catastrophic.
    // Instead: stage all facts, commit once at the end.
    if (fact->opcode == StructuredFactOpcode::kUpsert) {
      (void)structured_memory.StageUpsert(fact->entity, fact->attribute, fact->value, fact->metadata);
    } else if (fact->opcode == StructuredFactOpcode::kRemove) {
      (void)structured_memory.StageRemove(fact->entity, fact->attribute);
    }

    // Progress logging every 5 seconds
    const auto t_now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(t_now - t_last_log).count() >= 5) {
      t_last_log = t_now;
      const auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(t_now - t_start).count();
      std::cerr << "[REBUILD-TRACE] ReplayStructuredFacts progress: reads="
                << payload_reads << "/" << candidates.size()
                << " facts=" << facts_found
                << " blocks=" << block_reads
                << " elapsed=" << elapsed_s << "s" << std::endl;
    }
  }
  // Single commit after all facts are staged.
  structured_memory.CommitStaged();

  const auto t_end = std::chrono::steady_clock::now();
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
  std::cerr << "[REBUILD-TRACE] ReplayStructuredFacts: total=" << metas.size()
            << " skipped_user=" << skipped_user
            << " skipped_status=" << skipped_status
            << " payload_reads=" << payload_reads
            << " not_fact=" << not_fact
            << " facts_found=" << facts_found
            << " block_reads=" << block_reads
            << " (" << total_ms << " ms)" << std::endl;
}

StoreSearchChannels BuildStoreChannels(WaxStore& store,
                                       const FTS5SearchEngine* store_text_index,
                                       const FTS5SearchEngine* structured_text_index,
                                       const VectorSearchEngine* vector_index,
                                       std::shared_ptr<EmbeddingProvider> embedder,
                                       bool enable_text_search,
                                       bool enable_vector_search,
                                       const SearchRequest& request,
                                       bool include_pending_frames) {
  StoreSearchChannels channels{};
  if (request.top_k <= 0) {
    return channels;
  }
  const bool text_mode_enabled = request.mode.kind != SearchModeKind::kVectorOnly;
  const bool vector_mode_enabled = request.mode.kind != SearchModeKind::kTextOnly;
  const bool has_query_text = request.query.has_value() && HasNonWhitespace(*request.query);
  const bool text_channel_enabled = enable_text_search && text_mode_enabled && has_query_text;

  std::cerr << "[CHANNELS-TRACE] text_channel=" << text_channel_enabled
            << " text_index=" << (store_text_index != nullptr)
            << " structured_index=" << (structured_text_index != nullptr)
            << std::endl;

  std::optional<std::vector<float>> query_embedding = request.embedding;
  if (!query_embedding.has_value() && enable_vector_search && vector_mode_enabled && embedder != nullptr &&
      has_query_text) {
    std::cerr << "[CHANNELS-TRACE] >> computing query embedding..." << std::endl;
    query_embedding = embedder->Embed(*request.query);
    std::cerr << "[CHANNELS-TRACE] << query embedding done" << std::endl;
    if (!AllFinite(*query_embedding)) {
      throw std::runtime_error("recall: query embedding contains non-finite values");
    }
  }
  const bool vector_channel_enabled =
      enable_vector_search && vector_mode_enabled && query_embedding.has_value();
  std::cerr << "[CHANNELS-TRACE] vector_channel=" << vector_channel_enabled << std::endl;
  if (!text_channel_enabled && !vector_channel_enabled) {
    std::cerr << "[CHANNELS-TRACE] no channels enabled, returning empty" << std::endl;
    return channels;
  }

  if (text_channel_enabled && store_text_index != nullptr) {
    std::cerr << "[CHANNELS-TRACE] >> store_text_index->Search()..." << std::endl;
    const auto t0 = std::chrono::steady_clock::now();
    const auto indexed_text_results = store_text_index->Search(*request.query, request.top_k);
    const auto search_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cerr << "[CHANNELS-TRACE] << text search returned " << indexed_text_results.size()
              << " results in " << search_ms << " ms" << std::endl;

    channels.text_results.reserve(indexed_text_results.size());
    std::cerr << "[CHANNELS-TRACE] >> reading " << indexed_text_results.size()
              << " payloads from store..." << std::endl;
    const auto pay_t0 = std::chrono::steady_clock::now();
    for (const auto& indexed : indexed_text_results) {
      const auto meta = store.FrameMeta(indexed.frame_id, include_pending_frames);
      if (!meta.has_value() || meta->status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(indexed.frame_id, include_pending_frames);
      if (IsInternalOrchestratorPayload(payload)) {
        continue;
      }
      SearchResult store_text_result{};
      store_text_result.frame_id = indexed.frame_id;
      store_text_result.score = indexed.score;
      store_text_result.preview_text = BytesToString(payload);
      store_text_result.sources = {SearchSource::kText};
      channels.text_results.push_back(std::move(store_text_result));
    }
    const auto pay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - pay_t0).count();
    std::cerr << "[CHANNELS-TRACE] << payload reads done in " << pay_ms << " ms"
              << " kept=" << channels.text_results.size() << std::endl;
  }

  if (vector_channel_enabled && vector_index != nullptr) {
    std::cerr << "[CHANNELS-TRACE] >> vector_index->Search()..." << std::endl;
    const auto vector_hits = vector_index->Search(*query_embedding, request.top_k);
    std::cerr << "[CHANNELS-TRACE] << vector search returned " << vector_hits.size()
              << " results" << std::endl;
    channels.vector_results.reserve(vector_hits.size());
    for (const auto& [frame_id, score] : vector_hits) {
      const auto meta = store.FrameMeta(frame_id, include_pending_frames);
      if (!meta.has_value() || meta->status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(frame_id, include_pending_frames);
      if (IsInternalOrchestratorPayload(payload)) {
        continue;
      }
      SearchResult vector_result{};
      vector_result.frame_id = frame_id;
      vector_result.score = score;
      vector_result.preview_text = BytesToString(payload);
      vector_result.sources = {SearchSource::kVector};
      channels.vector_results.push_back(std::move(vector_result));
    }
  }

  if (text_channel_enabled && structured_text_index != nullptr) {
    std::cerr << "[CHANNELS-TRACE] >> structured_text_index->Search()..." << std::endl;
    auto fact_results = structured_text_index->Search(*request.query, request.top_k);
    std::cerr << "[CHANNELS-TRACE] << structured search returned " << fact_results.size()
              << " results" << std::endl;
    for (auto& result : fact_results) {
      result.sources = {SearchSource::kStructuredMemory};
      channels.text_results.push_back(std::move(result));
    }
  }
  return channels;
}

inline constexpr std::uint64_t kStructuredMemoryFrameIdBase = (1ULL << 63);

std::string StructuredFactPreviewText(const StructuredMemoryEntry& entry) {
  return entry.entity + " " + entry.attribute + " " + entry.value;
}

void RebuildTextIndexFromStore(WaxStore& store, FTS5SearchEngine& store_text_index) {
  store_text_index = FTS5SearchEngine{};

  const auto& metas = store.CommittedFrameMetasRef();
  std::cerr << "[STARTUP-TRACE] RebuildTextIndexFromStore: " << metas.size()
            << " frame metas loaded" << std::endl;
  const auto t0 = std::chrono::steady_clock::now();

  // ── Pass 1: collect frames that need payload reads ──
  struct FrameRead {
    std::uint64_t frame_id;
    std::uint64_t payload_offset;
    std::uint64_t payload_length;
    bool known_user;  // skip IsInternalOrchestratorPayload check
  };
  std::vector<FrameRead> candidates;
  candidates.reserve(metas.size());
  std::uint64_t skipped_status = 0;
  for (const auto& meta : metas) {
    if (meta.status != 0) { ++skipped_status; continue; }
    if (meta.payload_length == 0) continue;
    const bool known_user = IsKnownUserContentFrame(meta);
    candidates.push_back({meta.id, meta.payload_offset, meta.payload_length, known_user});
  }

  // Sort by payload_offset for sequential I/O.
  std::sort(candidates.begin(), candidates.end(),
            [](const FrameRead& a, const FrameRead& b) {
              return a.payload_offset < b.payload_offset;
            });

  const auto pass1_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cerr << "[STARTUP-TRACE] RebuildTextIndex pass1: candidates=" << candidates.size()
            << " skipped_status=" << skipped_status
            << " (" << pass1_ms << " ms)" << std::endl;

  // ── Pass 2: buffered sequential reads (4MB look-ahead) ──
  std::ifstream in(store.Path(), std::ios::binary);
  if (!in) {
    std::cerr << "[STARTUP-TRACE] FAILED to open store file for text index rebuild" << std::endl;
    return;
  }

  constexpr std::size_t kBlockSize = 4 * 1024 * 1024;  // 4 MB
  constexpr std::size_t kMaxPayload = 64 * 1024;        // 64 KB safety cap
  std::vector<char> block_buf(kBlockSize);
  std::uint64_t buf_start = 0;
  std::uint64_t buf_end = 0;
  std::uint64_t block_reads = 0;

  std::uint64_t indexed_count = 0;
  std::uint64_t skipped_internal = 0;
  auto t_last_log = std::chrono::steady_clock::now();

  for (const auto& c : candidates) {
    const auto read_len = static_cast<std::size_t>(
        std::min(c.payload_length, static_cast<std::uint64_t>(kMaxPayload)));
    const auto end_offset = c.payload_offset + read_len;

    // Refill buffer if needed
    if (c.payload_offset < buf_start || end_offset > buf_end) {
      buf_start = c.payload_offset;
      in.seekg(static_cast<std::streamoff>(buf_start), std::ios::beg);
      in.read(block_buf.data(), static_cast<std::streamsize>(kBlockSize));
      buf_end = buf_start + static_cast<std::uint64_t>(in.gcount());
      in.clear();
      ++block_reads;
      if (end_offset > buf_end) continue;
    }

    const auto buf_offset = static_cast<std::size_t>(c.payload_offset - buf_start);
    const char* data_ptr = block_buf.data() + buf_offset;

    if (!c.known_user) {
      // Could be internal — check magic bytes without full vector copy.
      std::vector<std::byte> payload(read_len);
      std::memcpy(payload.data(), data_ptr, read_len);
      if (IsInternalOrchestratorPayload(payload)) {
        ++skipped_internal;
        continue;
      }
      store_text_index.StageIndex(c.frame_id, std::string(data_ptr, read_len));
    } else {
      store_text_index.StageIndex(c.frame_id, std::string(data_ptr, read_len));
    }
    ++indexed_count;

    // Progress logging every 5 seconds
    const auto t_now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(t_now - t_last_log).count() >= 5) {
      t_last_log = t_now;
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t0).count();
      std::cerr << "[STARTUP-TRACE]   ... indexed " << indexed_count << "/" << candidates.size()
                << " blocks=" << block_reads
                << " (" << elapsed_ms << " ms)" << std::endl;
    }
  }
  const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cerr << "[STARTUP-TRACE] >> CommitStaged (" << indexed_count << " docs)..." << std::endl;
  const auto commit_t0 = std::chrono::steady_clock::now();
  store_text_index.CommitStaged();
  const auto commit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - commit_t0).count();
  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cerr << "[STARTUP-TRACE] RebuildTextIndexFromStore complete: indexed=" << indexed_count
            << " skipped_status=" << skipped_status
            << " skipped_internal=" << skipped_internal
            << " block_reads=" << block_reads
            << " read=" << read_ms << " ms"
            << " commit=" << commit_ms << " ms"
            << " total=" << total_ms << " ms"
            << " fts5_active=" << store_text_index.IsFts5Active()
            << std::endl;
}

struct PersistedEmbeddingSnapshot {
  struct PersistedEmbedding {
    std::vector<float> embedding{};
    std::optional<std::string> identity_tag{};
  };

  std::unordered_map<std::uint64_t, PersistedEmbedding> by_frame{};
  std::optional<int> dimensions{};
};

PersistedEmbeddingSnapshot LoadPersistedEmbeddingsFromStore(WaxStore& store) {
  PersistedEmbeddingSnapshot snapshot{};
  const auto& metas = store.CommittedFrameMetasRef();
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    const auto embedding_record = ParseEmbeddingRecordPayload(payload);
    if (!embedding_record.has_value()) {
      continue;
    }
    if (embedding_record->embedding.empty()) {
      // Empty embedding payloads are malformed for vector replay and must not override prior valid records.
      continue;
    }
    if (!AllFinite(embedding_record->embedding)) {
      // Keep previously loaded valid record for this frame_id if a later corrupted record appears.
      continue;
    }
    if (const auto existing_it = snapshot.by_frame.find(embedding_record->frame_id);
        existing_it != snapshot.by_frame.end() &&
        existing_it->second.embedding.size() != embedding_record->embedding.size()) {
      // Dimension-mismatched overrides for the same frame_id are treated as malformed tail noise.
      continue;
    }
    if (!snapshot.dimensions.has_value() && !embedding_record->embedding.empty()) {
      snapshot.dimensions = static_cast<int>(embedding_record->embedding.size());
    }
    PersistedEmbeddingSnapshot::PersistedEmbedding persisted{};
    persisted.embedding = std::move(embedding_record->embedding);
    persisted.identity_tag = std::move(embedding_record->identity_tag);
    snapshot.by_frame[embedding_record->frame_id] = std::move(persisted);
  }
  return snapshot;
}

std::vector<std::vector<float>> BuildEmbeddingsForTexts(std::shared_ptr<EmbeddingProvider> embedder,
                                                         const std::vector<std::string>& texts,
                                                         int ingest_batch_size,
                                                         int ingest_concurrency,
                                                         const char* error_context) {
  if (texts.empty()) {
    return {};
  }
  std::vector<std::vector<float>> out{};
  out.reserve(texts.size());

  if (auto* batch_embedder = dynamic_cast<BatchEmbeddingProvider*>(embedder.get()); batch_embedder != nullptr &&
      texts.size() > 1) {
    const std::size_t batch_size =
        ingest_batch_size > 0 ? static_cast<std::size_t>(ingest_batch_size) : texts.size();
    for (std::size_t start = 0; start < texts.size(); start += batch_size) {
      const auto end = std::min(texts.size(), start + batch_size);
      std::vector<std::string> slice{};
      slice.reserve(end - start);
      for (std::size_t i = start; i < end; ++i) {
        slice.push_back(texts[i]);
      }
      auto partial = batch_embedder->EmbedBatch(slice);
      if (partial.size() != slice.size()) {
        throw std::runtime_error(std::string(error_context) + ": mismatched embedding batch size");
      }
      out.insert(out.end(),
                 std::make_move_iterator(partial.begin()),
                 std::make_move_iterator(partial.end()));
    }
  } else {
    const std::size_t worker_count = ingest_concurrency > 1
                                         ? std::min(texts.size(), static_cast<std::size_t>(ingest_concurrency))
                                         : 1ULL;
    if (worker_count <= 1) {
      for (const auto& text : texts) {
        out.push_back(embedder->Embed(text));
      }
      return out;
    }

    out.assign(texts.size(), {});
    std::atomic<std::size_t> next_index{0};
    std::atomic<bool> stop_workers{false};
    std::exception_ptr first_error{};
    std::mutex error_mutex{};

    auto worker = [&]() {
      while (true) {
        if (stop_workers.load(std::memory_order_acquire)) {
          return;
        }
        const auto index = next_index.fetch_add(1);
        if (index >= texts.size()) {
          return;
        }
        try {
          out[index] = embedder->Embed(texts[index]);
        } catch (...) {
          std::lock_guard<std::mutex> error_lock(error_mutex);
          if (first_error == nullptr) {
            first_error = std::current_exception();
          }
          stop_workers.store(true, std::memory_order_release);
          return;
        }
      }
    };

    std::vector<std::thread> workers{};
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
      workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
      thread.join();
    }
    if (first_error != nullptr) {
      std::rethrow_exception(first_error);
    }
  }
  return out;
}

void RebuildVectorIndexFromStore(WaxStore& store,
                                 const PersistedEmbeddingSnapshot& persisted_embeddings,
                                 std::shared_ptr<EmbeddingProvider> embedder,
                                 int ingest_batch_size,
                                 int ingest_concurrency,
                                 USearchVectorEngine& vector_index) {
  const auto& metas = store.CommittedFrameMetasRef();
  std::vector<std::uint64_t> frame_ids{};
  std::vector<std::string> texts{};
  frame_ids.reserve(metas.size());
  texts.reserve(metas.size());
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    if (IsInternalOrchestratorPayload(payload)) {
      continue;
    }
    frame_ids.push_back(meta.id);
    texts.push_back(BytesToString(payload));
  }

  std::vector<std::uint64_t> missing_ids{};
  std::vector<std::string> missing_texts{};
  missing_ids.reserve(frame_ids.size());
  missing_texts.reserve(frame_ids.size());
  const auto current_identity_tag = embedder != nullptr ? BuildEmbeddingIdentityTag(embedder->identity()) : std::nullopt;

  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    const auto persisted_it = persisted_embeddings.by_frame.find(frame_ids[i]);
    if (persisted_it == persisted_embeddings.by_frame.end() ||
        persisted_it->second.embedding.size() != static_cast<std::size_t>(vector_index.dimensions()) ||
        !AllFinite(persisted_it->second.embedding)) {
      missing_ids.push_back(frame_ids[i]);
      missing_texts.push_back(texts[i]);
      continue;
    }
    if (current_identity_tag.has_value() && persisted_it->second.identity_tag.has_value() &&
        *persisted_it->second.identity_tag != *current_identity_tag) {
      missing_ids.push_back(frame_ids[i]);
      missing_texts.push_back(texts[i]);
      continue;
    }
    vector_index.StageAdd(frame_ids[i], persisted_it->second.embedding);
  }

  if (embedder != nullptr && !missing_ids.empty()) {
    auto embeddings = BuildEmbeddingsForTexts(
        embedder, missing_texts, ingest_batch_size, ingest_concurrency, "rebuild vector index");
    if (embeddings.size() != missing_ids.size()) {
      throw std::runtime_error("rebuild vector index: embedding count mismatch");
    }
    for (std::size_t i = 0; i < missing_ids.size(); ++i) {
      if (embeddings[i].size() != static_cast<std::size_t>(vector_index.dimensions())) {
        throw std::runtime_error("rebuild vector index: embedding dimension mismatch");
      }
      if (!AllFinite(embeddings[i])) {
        throw std::runtime_error("rebuild vector index: embedding contains non-finite values");
      }
      vector_index.StageAdd(missing_ids[i], embeddings[i]);
    }
  }

  vector_index.CommitStaged();
}

std::optional<int> ResolveVectorDimensions(std::shared_ptr<EmbeddingProvider> embedder,
                                           const PersistedEmbeddingSnapshot& persisted_embeddings) {
  if (embedder != nullptr) {
    return embedder->dimensions();
  }
  if (persisted_embeddings.dimensions.has_value() && *persisted_embeddings.dimensions > 0) {
    return persisted_embeddings.dimensions;
  }
  return std::nullopt;
}

void EnsureEmbedderRequiredForRemember(const OrchestratorConfig& config,
                                       std::shared_ptr<EmbeddingProvider> embedder) {
  if (config.enable_vector_search && embedder == nullptr) {
    throw std::runtime_error("remember requires embedder when vector search is enabled");
  }
}

void EnsureOnDeviceProviderPolicy(const OrchestratorConfig& config,
                                  const std::shared_ptr<EmbeddingProvider>& embedder) {
  if (!config.enable_vector_search || !config.require_on_device_providers) {
    return;
  }
  if (embedder == nullptr) {
    throw std::runtime_error("on-device policy requires embedder");
  }
  const auto identity = embedder->identity();
  if (!identity.has_value() || !identity->provider.has_value() || identity->provider->empty()) {
    throw std::runtime_error("on-device policy requires embedder identity provider");
  }
  const auto provider_lower = ToAsciiLower(*identity->provider);
  constexpr std::array<std::string_view, 5> kDisallowedProviderTokens = {
      "openai",
      "anthropic",
      "cohere",
      "azure",
      "huggingface",
  };
  for (const auto token : kDisallowedProviderTokens) {
    if (provider_lower.find(token) != std::string::npos) {
      throw std::runtime_error("on-device policy rejected remote embedder provider: " + *identity->provider);
    }
  }
}

void StagePersistedEmbeddingRecord(WaxStore& store,
                                   std::uint64_t frame_id,
                                   const std::vector<float>& embedding,
                                   int expected_dimensions,
                                   const std::optional<std::string>& embedder_identity_tag) {
  if (expected_dimensions <= 0 || embedding.size() != static_cast<std::size_t>(expected_dimensions)) {
    return;
  }
  const auto payload = BuildEmbeddingRecordPayload(frame_id, embedding, embedder_identity_tag);
  (void)store.Put(payload, {});
}

void StageVectorIndexEmbedding(USearchVectorEngine* vector_index,
                               std::uint64_t frame_id,
                               const std::vector<float>& embedding) {
  if (vector_index == nullptr) {
    return;
  }
  if (embedding.size() != static_cast<std::size_t>(vector_index->dimensions())) {
    return;
  }
  vector_index->StageAdd(frame_id, embedding);
}

void RebuildStructuredFactIndex(const StructuredMemoryStore& structured_memory, FTS5SearchEngine& structured_text_index) {
  structured_text_index = FTS5SearchEngine{};
  const auto facts = structured_memory.All(-1);
  for (const auto& fact : facts) {
    structured_text_index.StageIndex(kStructuredMemoryFrameIdBase + fact.id, StructuredFactPreviewText(fact));
  }
  structured_text_index.CommitStaged();
}

/// Build mapping from source frame ID → active surrogate frame ID.
/// Surrogate frames have the `supersedes` field set to the source frame ID.
///
/// After an overwrite the chain is source → old_surrogate → new_surrogate.
/// new_surrogate.supersedes == old_surrogate (not source), so we must walk
/// the chain backwards to discover the root source frame.
std::unordered_map<std::uint64_t, std::uint64_t> BuildSurrogateMap(
    const std::vector<WaxFrameMeta>& metas) {
  // Frame-ID → meta lookup for chain walking.
  std::unordered_map<std::uint64_t, const WaxFrameMeta*> by_id;
  by_id.reserve(metas.size());
  for (const auto& m : metas) by_id[m.id] = &m;

  std::unordered_map<std::uint64_t, std::uint64_t> result;
  for (const auto& meta : metas) {
    // Active frames that supersede another frame and are themselves not
    // superseded are the "tip" of the chain (the newest surrogate).
    if (meta.status != 0) continue;
    if (!meta.supersedes.has_value()) continue;
    if (meta.superseded_by.has_value()) continue;

    // Walk the supersedes chain to find the root source frame.
    std::uint64_t source_id = *meta.supersedes;
    constexpr int kMaxChainDepth = 64;  // Safety guard.
    for (int depth = 0; depth < kMaxChainDepth; ++depth) {
      auto it = by_id.find(source_id);
      if (it == by_id.end()) break;
      if (!it->second->supersedes.has_value()) break;
      source_id = *it->second->supersedes;
    }
    result[source_id] = meta.id;
  }
  return result;
}

void RebuildRuntimeStateFromStore(WaxStore& store,
                                  const OrchestratorConfig& config,
                                  const std::shared_ptr<EmbeddingProvider>& embedder,
                                  StructuredMemoryStore& structured_memory,
                                  FTS5SearchEngine& store_text_index,
                                  FTS5SearchEngine& structured_text_index,
                                  std::unique_ptr<USearchVectorEngine>& vector_index,
                                  EmbeddingMemoizer& embedding_cache,
                                  std::unordered_map<std::uint64_t, std::uint64_t>& surrogate_map) {
  auto phase_start = std::chrono::steady_clock::now();
  auto LogPhase = [&](const char* label) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_start).count();
    phase_start = now;
    std::cerr << "[REBUILD-TRACE] " << label << " (" << ms << " ms)" << std::endl;
  };

  std::cerr << "[REBUILD-TRACE] >> ReplayStructuredFactsFromStore..." << std::endl;
  structured_memory = StructuredMemoryStore{};
  ReplayStructuredFactsFromStore(store, structured_memory);
  LogPhase("<< ReplayStructuredFactsFromStore done");

  embedding_cache.Clear();

  std::cerr << "[REBUILD-TRACE] >> BuildSurrogateMap (FrameMetas)..." << std::endl;
  surrogate_map = BuildSurrogateMap(store.CommittedFrameMetasRef());
  LogPhase("<< BuildSurrogateMap done");

  if (config.enable_text_search) {
    std::cerr << "[REBUILD-TRACE] >> RebuildTextIndexFromStore..." << std::endl;
    RebuildTextIndexFromStore(store, store_text_index);
    LogPhase("<< RebuildTextIndexFromStore done");

    std::cerr << "[REBUILD-TRACE] >> RebuildStructuredFactIndex..." << std::endl;
    RebuildStructuredFactIndex(structured_memory, structured_text_index);
    LogPhase("<< RebuildStructuredFactIndex done");
  } else {
    store_text_index = FTS5SearchEngine{};
    structured_text_index = FTS5SearchEngine{};
    std::cerr << "[REBUILD-TRACE] text search disabled, skipped" << std::endl;
  }

  if (!config.enable_vector_search) {
    vector_index.reset();
    std::cerr << "[REBUILD-TRACE] vector search disabled, done" << std::endl;
    return;
  }

  std::cerr << "[REBUILD-TRACE] >> LoadPersistedEmbeddingsFromStore..." << std::endl;
  const auto persisted_embeddings = LoadPersistedEmbeddingsFromStore(store);
  LogPhase("<< LoadPersistedEmbeddingsFromStore done");

  const auto vector_dims = ResolveVectorDimensions(embedder, persisted_embeddings);
  if (!vector_dims.has_value() || *vector_dims <= 0) {
    vector_index.reset();
    std::cerr << "[REBUILD-TRACE] no vector dims, skipping vector index" << std::endl;
    return;
  }

  vector_index = std::make_unique<USearchVectorEngine>(*vector_dims);
  std::cerr << "[REBUILD-TRACE] >> RebuildVectorIndexFromStore..." << std::endl;
  RebuildVectorIndexFromStore(store,
                              persisted_embeddings,
                              embedder,
                              config.ingest_batch_size,
                              config.ingest_concurrency,
                              *vector_index);
  LogPhase("<< RebuildVectorIndexFromStore done");
}

/// Enrich search results with tier-appropriate surrogate text.
/// For results without preview_text, looks up surrogate content from the store,
/// selects the appropriate tier via the tier selector, and sets the preview_text.
void EnrichResultsWithSurrogates(
    SearchResponse& response,
    const std::string& query,
    WaxStore& store,
    const SurrogateTierSelector& tier_selector,
    const AccessStatsManager& access_stats,
    const std::unordered_map<std::uint64_t, std::uint64_t>& surrogate_map,
    bool enable_query_aware_tier,
    std::optional<std::int64_t> deterministic_now_ms) {
  if (surrogate_map.empty()) return;

  // Collect frame IDs that need surrogate enrichment.
  std::vector<std::uint64_t> surrogate_frame_ids;
  std::vector<std::size_t> result_indices;
  for (std::size_t i = 0; i < response.results.size(); ++i) {
    const auto& r = response.results[i];
    if (r.preview_text.has_value() && !r.preview_text->empty()) continue;
    auto it = surrogate_map.find(r.frame_id);
    if (it == surrogate_map.end()) continue;
    surrogate_frame_ids.push_back(it->second);
    result_indices.push_back(i);
  }
  if (surrogate_frame_ids.empty()) return;

  // Batch-load surrogate frame contents.
  const auto contents = store.FrameContents(surrogate_frame_ids);

  // Pre-compute query signals for tier selection.
  QueryAnalyzer analyzer;
  const std::optional<QuerySignals> signals =
      enable_query_aware_tier ? std::make_optional(analyzer.Analyze(query))
                              : std::nullopt;
  const auto now_ms = deterministic_now_ms.value_or(NowMs());

  // Enrich each result.
  for (std::size_t idx = 0; idx < result_indices.size(); ++idx) {
    auto& result = response.results[result_indices[idx]];
    const auto surr_frame_id = surrogate_frame_ids[idx];
    auto content_it = contents.find(surr_frame_id);
    if (content_it == contents.end()) continue;

    const auto data = BytesToString(content_it->second);
    if (data.empty()) continue;

    // Build tier selection context.
    const auto frame_stats = access_stats.GetStats(result.frame_id);
    TierSelectionContext ctx;
    ctx.frame_timestamp_ms =
        frame_stats.has_value() ? frame_stats->first_access_ms : now_ms;
    ctx.access_stats =
        frame_stats.has_value() ? &frame_stats.value() : nullptr;
    ctx.query_signals = signals.has_value() ? &signals.value() : nullptr;
    ctx.now_ms = now_ms;

    const auto tier = tier_selector.SelectTier(ctx);
    auto text = SurrogateTierSelector::ExtractTier(data, tier);
    if (text.has_value() && !text->empty()) {
      result.preview_text = std::move(*text);
    }
  }
}

}  // namespace

MemoryOrchestrator::MemoryOrchestrator(const std::filesystem::path& path,
                                       const OrchestratorConfig& config,
                                       std::shared_ptr<EmbeddingProvider> embedder,
                                       const TokenCounter* token_counter)
    : config_(config),
      store_([&]() -> WaxStore {
        const bool exists = std::filesystem::exists(path);
        std::cerr << "[INIT-TRACE] >> WaxStore::" << (exists ? "Open" : "Create")
                  << "(" << path << ")..." << std::endl;
        const auto t0 = std::chrono::steady_clock::now();
        auto s = exists ? WaxStore::Open(path) : WaxStore::Create(path);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        const auto stats = s.Stats();
        std::cerr << "[INIT-TRACE] << WaxStore opened in " << ms << " ms"
                  << " frames=" << stats.frame_count
                  << " generation=" << stats.generation
                  << " pending=" << stats.pending_frames << std::endl;
        return s;
      }()),
      embedder_(std::move(embedder)),
      token_counter_(token_counter),
      embedding_cache_(config.embedding_cache_capacity),
      tier_selector_(config.rag.enable_query_aware_tier_selection
                         ? config.rag.tier_selection_policy
                         : TierSelectionPolicy{TierPolicyDisabled{}}) {
  if (config_.rag.search_mode.kind == SearchModeKind::kTextOnly && !config_.enable_text_search) {
    throw std::runtime_error("text-only search mode requires text search to be enabled");
  }
  if (config_.rag.search_mode.kind == SearchModeKind::kVectorOnly && !config_.enable_vector_search) {
    throw std::runtime_error("vector-only search mode requires vector search to be enabled");
  }
  if (config_.rag.search_mode.kind == SearchModeKind::kHybrid &&
      !config_.enable_text_search &&
      !config_.enable_vector_search) {
    throw std::runtime_error("hybrid search mode requires at least one enabled search channel");
  }
  if (config_.enable_vector_search && embedder_ == nullptr) {
    throw std::runtime_error("vector-enabled config requires embedder");
  }
  EnsureOnDeviceProviderPolicy(config_, embedder_);
  if (config_.enable_vector_search && embedder_->dimensions() <= 0) {
    throw std::runtime_error("vector-enabled config requires positive embedder dimensions");
  }
  if (config_.enable_vector_search &&
      embedder_->dimensions() > static_cast<int>(kMaxEmbeddingRecordValues)) {
    throw std::runtime_error("vector-enabled config embedder dimensions exceed replay safety limit");
  }
  std::cerr << "[INIT-TRACE] >> RebuildRuntimeStateFromStore..." << std::endl;
  const auto rebuild_t0 = std::chrono::steady_clock::now();
  RebuildRuntimeStateFromStore(store_,
                               config_,
                               embedder_,
                               structured_memory_,
                               store_text_index_,
                               structured_text_index_,
                               vector_index_,
                               embedding_cache_,
                               surrogate_map_);
  const auto rebuild_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - rebuild_t0).count();
  std::cerr << "[INIT-TRACE] << RebuildRuntimeStateFromStore done in " << rebuild_ms << " ms"
            << std::endl;
  if (config_.enable_vector_search && vector_index_ == nullptr) {
    throw std::runtime_error("vector-enabled config requires initialized vector index");
  }
}

void MemoryOrchestrator::Remember(const std::string& content, const Metadata& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  last_write_activity_ms_ = NowMs();

  // Stamp session ID if active.
  Metadata effective_meta = metadata;
  if (!current_session_id_.empty()) {
    effective_meta["session_id"] = current_session_id_;
  }

  EnsureEmbedderRequiredForRemember(config_, embedder_);
  // Use TextChunker for BPE-aware chunking (falls back to whitespace when
  // token counter is unavailable).
  const auto chunks = TextChunker::Chunk(content, config_.chunking, token_counter_);

  std::optional<std::vector<std::vector<float>>> chunk_embeddings{};
  const auto embedder_identity_tag =
      (config_.enable_vector_search && embedder_ != nullptr) ? BuildEmbeddingIdentityTag(embedder_->identity())
                                                              : std::nullopt;
  if (config_.enable_vector_search && embedder_ != nullptr) {
    chunk_embeddings = BuildEmbeddingsForTexts(
        embedder_, chunks, config_.ingest_batch_size, config_.ingest_concurrency, "remember");
    if (chunk_embeddings->size() != chunks.size()) {
      throw std::runtime_error("remember: embedding count mismatch");
    }
    if (vector_index_ == nullptr) {
      throw std::runtime_error("remember: vector index is not initialized");
    }
    const auto expected_dims = static_cast<std::size_t>(vector_index_->dimensions());
    for (const auto& embedding : *chunk_embeddings) {
      if (embedding.size() != expected_dims) {
        throw std::runtime_error("remember: embedding dimension mismatch");
      }
      if (!AllFinite(embedding)) {
        throw std::runtime_error("remember: embedding contains non-finite values");
      }
    }
  }

  for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
    const auto& chunk = chunks[chunk_index];
    std::vector<std::byte> payload{};
    payload.reserve(chunk.size());
    for (const char ch : chunk) {
      payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    const auto frame_id = store_.Put(payload, effective_meta);
    if (config_.enable_text_search) {
      store_text_index_.StageIndex(frame_id, chunk);
    }

    if (config_.enable_vector_search && embedder_ != nullptr) {
      std::vector<float> embedding{};
      if (chunk_embeddings.has_value()) {
        embedding = std::move((*chunk_embeddings)[chunk_index]);
      } else {
        embedding = embedder_->Embed(chunk);
      }
      StageVectorIndexEmbedding(vector_index_.get(), frame_id, embedding);
      const int vector_dims = vector_index_ != nullptr ? vector_index_->dimensions() : 0;
      StagePersistedEmbeddingRecord(store_, frame_id, embedding, vector_dims, embedder_identity_tag);
      if (!embedding.empty()) {
        embedding_cache_.Set(frame_id, std::move(embedding));
      }
    }
  }
}

void MemoryOrchestrator::RememberFile(const std::filesystem::path& file_path,
                                       const Metadata& metadata) {
  if (!std::filesystem::exists(file_path)) {
    throw std::runtime_error("RememberFile: file not found: " + file_path.string());
  }
  // Read file content.
  const auto file_size = std::filesystem::file_size(file_path);
  if (file_size == 0) {
    throw std::runtime_error("RememberFile: empty file: " + file_path.string());
  }
  std::ifstream ifs(file_path, std::ios::binary);
  if (!ifs) {
    throw std::runtime_error("RememberFile: failed to open file: " + file_path.string());
  }
  std::string content(static_cast<std::size_t>(file_size), '\0');
  ifs.read(content.data(), static_cast<std::streamsize>(file_size));
  if (!ifs) {
    throw std::runtime_error("RememberFile: failed to read file: " + file_path.string());
  }

  // Merge file metadata.
  Metadata merged = metadata;
  merged["source_kind"] = "file";
  merged["source_uri"] = file_path.string();
  merged["source_filename"] = file_path.filename().string();
  if (file_path.has_extension()) {
    auto ext = file_path.extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    // ASCII lowercase.
    for (auto& ch : ext) {
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    merged["source_extension"] = ext;
  }

  Remember(content, merged);
}

// ── Shared Recall implementation ─────────────────────────────

namespace {

/// Check if a frame's metadata matches the MetadataFilter predicate.
bool MatchesMetadataFilter(const MetadataFilter& mf, const WaxFrameMeta& meta) {
  // Check required_entries against per-frame metadata.
  for (const auto& [key, value] : mf.required_entries) {
    auto it = meta.metadata.find(key);
    if (it == meta.metadata.end() || it->second != value) return false;
  }
  // Check required_tags against per-frame tags.
  for (const auto& [required_key, required_value] : mf.required_tags) {
    bool found = false;
    for (const auto& [tag_key, tag_value] : meta.tags) {
      if (tag_key == required_key && tag_value == required_value) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  // Check required_labels against per-frame labels.
  for (const auto& required_label : mf.required_labels) {
    bool found = false;
    for (const auto& l : meta.labels) {
      if (l == required_label) { found = true; break; }
    }
    if (!found) return false;
  }
  return true;
}

/// Apply FrameFilter to search results using WaxStore metadata.
/// Removes results that don't pass the filter predicates.
void ApplyFrameFilter(SearchResponse& response,
                      const FrameFilter& filter,
                      const WaxStore& store,
                      const std::unordered_map<std::uint64_t, std::uint64_t>& surrogate_map,
                      const std::optional<TimeRange>& time_range = std::nullopt) {
  // Build a set of surrogate frame IDs for fast lookup.
  std::unordered_set<std::uint64_t> surrogate_ids;
  if (!filter.include_surrogates) {
    for (const auto& [source_id, surr_id] : surrogate_map) {
      surrogate_ids.insert(surr_id);
    }
  }

  // Build a frame-ID → WaxFrameMeta map for the result frame IDs.
  std::unordered_map<std::uint64_t, WaxFrameMeta> meta_by_id;
  const auto all_metas = store.FrameMetas(true);
  for (const auto& m : all_metas) {
    meta_by_id[m.id] = m;
  }

  auto& results = response.results;
  results.erase(
      std::remove_if(results.begin(), results.end(),
                     [&](const SearchResult& r) {
                       auto it = meta_by_id.find(r.frame_id);
                       if (it == meta_by_id.end()) return true;
                       const auto& meta = it->second;

                       // Deleted frame check.
                       if (!filter.include_deleted && meta.status != 0) return true;

                       // Superseded frame check.
                       if (!filter.include_superseded && meta.superseded_by.has_value()) return true;

                       // Surrogate frame check.
                       if (!filter.include_surrogates && surrogate_ids.count(r.frame_id)) return true;

                       // Frame ID allowlist check.
                       if (filter.frame_ids.has_value() && !filter.frame_ids->count(r.frame_id)) return true;

                       // TimeRange check (uses persisted per-frame timestamp).
                       if (time_range.has_value() && !time_range->Contains(meta.timestamp_ms)) return true;

                       // MetadataFilter check (uses persisted per-frame metadata, tags, and labels).
                       if (filter.metadata_filter.has_value() &&
                           !MatchesMetadataFilter(*filter.metadata_filter, meta)) return true;

                       return false;
                     }),
      results.end());
}

}  // namespace

RAGContext MemoryOrchestrator::RecallImpl(
    const std::string& query,
    const std::optional<std::vector<float>>& explicit_embedding,
    const std::optional<FrameFilter>& frame_filter,
    std::optional<QueryEmbeddingPolicy> policy_override) {
  // Caller holds mutex_.
  ThrowIfClosed(closed_);
  auto recall_t0 = std::chrono::steady_clock::now();
  auto phase_start = recall_t0;
  auto ElapsedMs = [&]() {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - phase_start).count();
    phase_start = now;
    return ms;
  };

  std::cerr << "[RECALL-TRACE] RecallImpl start, query=\"" << query << "\""
            << " text_search=" << config_.enable_text_search
            << " vector_search=" << config_.enable_vector_search
            << " store_frames=" << store_.Stats().frame_count
            << " fts5_docs=" << store_text_index_.DocCount()
            << std::endl;

  // Resolve effective policy.
  const auto policy = policy_override.value_or(config_.query_embedding_policy);
  if (policy == QueryEmbeddingPolicy::kAlways && embedder_ == nullptr) {
    throw std::runtime_error("Recall: QueryEmbeddingPolicy::kAlways requires an embedder");
  }
  const bool use_vector =
      config_.enable_vector_search && policy != QueryEmbeddingPolicy::kNever;
  const std::shared_ptr<EmbeddingProvider> effective_embedder =
      (policy == QueryEmbeddingPolicy::kNever) ? nullptr : embedder_;

  // Validate explicit embedding if provided.
  if (explicit_embedding.has_value()) {
    if (!config_.enable_vector_search) {
      throw std::runtime_error("Recall(query, embedding) requires vector search to be enabled");
    }
    if (vector_index_ != nullptr &&
        explicit_embedding->size() != static_cast<std::size_t>(vector_index_->dimensions())) {
      throw std::runtime_error("Recall(query, embedding) dimension mismatch with vector index");
    }
    if (!AllFinite(*explicit_embedding)) {
      throw std::runtime_error("Recall(query, embedding) requires finite embedding values");
    }
  }

  SearchRequest req;
  req.query = query;
  if (explicit_embedding.has_value()) req.embedding = *explicit_embedding;
  req.mode = config_.rag.search_mode;
  if (frame_filter.has_value()) req.frame_filter = *frame_filter;
  const int clamped_search_top_k = std::max(0, config_.rag.search_top_k);
  const int clamped_max_snippets = std::max(0, config_.rag.max_snippets);
  req.top_k = clamped_search_top_k;
  req.max_snippets = clamped_max_snippets;
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  req.expansion_max_tokens = config_.rag.expansion_max_tokens;
  req.max_context_tokens = config_.rag.max_context_tokens;
  req.snippet_max_tokens = config_.rag.snippet_max_tokens;

  std::cerr << "[RECALL-TRACE] search_mode=" << static_cast<int>(req.mode.kind)
            << " top_k=" << clamped_search_top_k
            << " max_snippets=" << clamped_max_snippets
            << " use_vector=" << use_vector
            << std::endl;

  const bool vectors_for_channels = explicit_embedding.has_value() ? true : use_vector;
  const auto& embedder_for_channels = explicit_embedding.has_value() ? embedder_ : effective_embedder;

  (void)ElapsedMs();
  std::cerr << "[RECALL-TRACE] >> BuildStoreChannels..." << std::endl;
  const auto channels = BuildStoreChannels(
      store_,
      config_.enable_text_search ? &store_text_index_ : nullptr,
      config_.enable_text_search ? &structured_text_index_ : nullptr,
      vectors_for_channels ? vector_index_.get() : nullptr,
      embedder_for_channels,
      config_.enable_text_search,
      vectors_for_channels,
      req,
      false);
  std::cerr << "[RECALL-TRACE] << BuildStoreChannels done in " << ElapsedMs() << " ms"
            << " text_results=" << channels.text_results.size()
            << " vector_results=" << channels.vector_results.size()
            << std::endl;

  std::cerr << "[RECALL-TRACE] >> UnifiedSearchAdaptive..." << std::endl;
  auto response = UnifiedSearchAdaptive(req, channels.text_results, channels.vector_results);
  std::cerr << "[RECALL-TRACE] << UnifiedSearchAdaptive done in " << ElapsedMs() << " ms"
            << " results=" << response.results.size() << std::endl;

  // Apply frame filter as post-filter on search results.
  if (frame_filter.has_value()) {
    ApplyFrameFilter(response, *frame_filter, store_, surrogate_map_);
  }

  // Intent-aware reranking.
  if (config_.rag.enable_answer_focused_ranking && !response.results.empty()) {
    const int rerank_window = std::min(
        std::max(clamped_search_top_k * 2, 10), config_.rag.answer_rerank_window);
    response.results = IntentAwareRerank(response.results, query, rerank_window);
  }
  std::cerr << "[RECALL-TRACE] << reranking done in " << ElapsedMs() << " ms" << std::endl;

  // Record frame accesses.
  if (!response.results.empty()) {
    std::vector<std::uint64_t> accessed_ids;
    accessed_ids.reserve(response.results.size());
    for (const auto& r : response.results) accessed_ids.push_back(r.frame_id);
    access_stats_.RecordAccesses(accessed_ids);
  }

  std::cerr << "[RECALL-TRACE] >> EnrichResultsWithSurrogates..." << std::endl;
  EnrichResultsWithSurrogates(response, query, store_, tier_selector_,
                              access_stats_, surrogate_map_,
                              config_.rag.enable_query_aware_tier_selection,
                              config_.rag.deterministic_now_ms);
  std::cerr << "[RECALL-TRACE] << EnrichResultsWithSurrogates done in " << ElapsedMs() << " ms"
            << std::endl;

  std::cerr << "[RECALL-TRACE] >> BuildFastRAGContext..." << std::endl;
  auto context = BuildFastRAGContext(req, response);
  std::cerr << "[RECALL-TRACE] << BuildFastRAGContext done in " << ElapsedMs() << " ms"
            << " items=" << context.items.size()
            << " tokens=" << context.total_tokens << std::endl;

  if (config_.rag.enable_answer_extraction && !context.items.empty()) {
    std::cerr << "[RECALL-TRACE] >> ExtractAnswer..." << std::endl;
    context.extracted_answer = answer_extractor_.ExtractAnswer(query, context.items);
    std::cerr << "[RECALL-TRACE] << ExtractAnswer done in " << ElapsedMs() << " ms" << std::endl;
  }

  const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - recall_t0).count();
  std::cerr << "[RECALL-TRACE] RecallImpl total: " << total_ms << " ms" << std::endl;
  return context;
}

RAGContext MemoryOrchestrator::Recall(const std::string& query) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RecallImpl(query, std::nullopt, std::nullopt, std::nullopt);
}

RAGContext MemoryOrchestrator::Recall(const std::string& query, const std::vector<float>& embedding) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RecallImpl(query, embedding, std::nullopt, std::nullopt);
}

RAGContext MemoryOrchestrator::Recall(const std::string& query, const FrameFilter& frame_filter) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RecallImpl(query, std::nullopt, frame_filter, std::nullopt);
}

RAGContext MemoryOrchestrator::Recall(const std::string& query, QueryEmbeddingPolicy policy) {
  std::lock_guard<std::mutex> lock(mutex_);
  return RecallImpl(query, std::nullopt, std::nullopt, policy);
}

std::optional<WaxFrameMeta> MemoryOrchestrator::FrameMeta(std::uint64_t frame_id, bool include_pending) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return store_.FrameMeta(frame_id, include_pending);
}

std::vector<WaxFrameMeta> MemoryOrchestrator::FrameMetas(const std::vector<std::uint64_t>& frame_ids,
                                                         bool include_pending) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<WaxFrameMeta> out{};
  out.reserve(frame_ids.size());
  for (const auto frame_id : frame_ids) {
    if (const auto meta = store_.FrameMeta(frame_id, include_pending); meta.has_value()) {
      out.push_back(*meta);
    }
  }
  return out;
}

void MemoryOrchestrator::RememberFact(const std::string& entity,
                                      const std::string& attribute,
                                      const std::string& value,
                                      const Metadata& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  last_write_activity_ms_ = NowMs();
  if (entity.empty()) {
    throw std::runtime_error("RememberFact entity must be non-empty");
  }
  if (attribute.empty()) {
    throw std::runtime_error("RememberFact attribute must be non-empty");
  }

  // Stamp session ID if active.
  Metadata effective_meta = metadata;
  if (!current_session_id_.empty()) {
    effective_meta["session_id"] = current_session_id_;
  }

  const auto payload = BuildStructuredFactUpsertPayload(entity, attribute, value, effective_meta);
  const auto fact_id = structured_memory_.StageUpsert(entity, attribute, value, effective_meta);
  if (config_.enable_text_search) {
    StructuredMemoryEntry preview_entry{};
    preview_entry.id = fact_id;
    preview_entry.entity = entity;
    preview_entry.attribute = attribute;
    preview_entry.value = value;
    structured_text_index_.StageIndex(kStructuredMemoryFrameIdBase + fact_id, StructuredFactPreviewText(preview_entry));
  }
  (void)store_.Put(payload, effective_meta);
}

bool MemoryOrchestrator::ForgetFact(const std::string& entity, const std::string& attribute) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  if (entity.empty()) {
    throw std::runtime_error("ForgetFact entity must be non-empty");
  }
  if (attribute.empty()) {
    throw std::runtime_error("ForgetFact attribute must be non-empty");
  }
  if (entity.size() > static_cast<std::size_t>(kMaxStructuredFactFieldBytes)) {
    throw std::runtime_error("ForgetFact entity exceeds replay safety limit");
  }
  if (attribute.size() > static_cast<std::size_t>(kMaxStructuredFactFieldBytes)) {
    throw std::runtime_error("ForgetFact attribute exceeds replay safety limit");
  }
  const auto estimated_payload_size =
      kStructuredFactMagic.size() + 1U + 4U + entity.size() + 4U + attribute.size();
  if (estimated_payload_size > static_cast<std::size_t>(kMaxStructuredFactPayloadBytes)) {
    throw std::runtime_error("ForgetFact payload exceeds replay safety limit");
  }
  const auto removed_id = structured_memory_.StageRemove(entity, attribute);
  if (!removed_id.has_value()) {
    return false;
  }
  if (config_.enable_text_search) {
    structured_text_index_.StageRemove(kStructuredMemoryFrameIdBase + *removed_id);
  }
  const auto payload = BuildStructuredFactRemovePayload(entity, attribute);
  (void)store_.Put(payload, {});
  return true;
}

std::vector<StructuredMemoryEntry> MemoryOrchestrator::RecallFactsByEntityPrefix(const std::string& entity_prefix,
                                                                                  int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  return structured_memory_.QueryByEntityPrefix(entity_prefix, limit);
}

std::int64_t MemoryOrchestrator::last_write_activity_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_write_activity_ms_;
}

void MemoryOrchestrator::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  bool store_commit_completed = false;
  try {
    store_.Commit();
    store_commit_completed = true;
    if (structured_memory_.PendingMutationCount() > 0) {
      structured_memory_.CommitStaged();
    }
    if (config_.enable_text_search && store_text_index_.PendingMutationCount() > 0) {
      store_text_index_.CommitStaged();
    }
    if (config_.enable_text_search && structured_text_index_.PendingMutationCount() > 0) {
      structured_text_index_.CommitStaged();
    }
    if (vector_index_ != nullptr && vector_index_->PendingMutationCount() > 0) {
      vector_index_->CommitStaged();
    }
    ++flush_count_;

    // Trigger scheduled live-set maintenance if configured.
    if (config_.live_set_rewrite_schedule.enabled) {
      try {
        auto maint_report = RunScheduledLiveSetMaintenance(
            store_,
            config_.live_set_rewrite_schedule,
            flush_count_,
            /*force=*/false,
            NowMs(),
            last_maintenance_completed_ms_,
            last_write_activity_ms_);
        last_maintenance_report_ = maint_report;
        if (maint_report.outcome == MaintenanceOutcome::kRewriteSucceeded ||
            maint_report.outcome == MaintenanceOutcome::kRewriteFailed ||
            maint_report.outcome == MaintenanceOutcome::kValidationFailedRolledBack) {
          last_maintenance_completed_ms_ = NowMs();
        }
      } catch (...) {
        // Swallow maintenance errors — store is already committed successfully.
        last_maintenance_completed_ms_ = NowMs();
      }
    }
  } catch (const std::exception& ex) {
    (void)ex;
    bool refreshed_visible_state = false;
    bool has_pending_frames = true;
    if (!store_commit_completed) {
      try {
        refreshed_visible_state = store_.TryRefreshIfPublishedCommitVisible();
        has_pending_frames = store_.Stats().pending_frames > 0;
      } catch (...) {
        // Best effort only; if probing fails we keep current staged runtime state.
      }
    }
    const bool should_rebuild =
        store_commit_completed || (refreshed_visible_state && !has_pending_frames);
    if (should_rebuild) {
      RebuildRuntimeStateFromStore(store_,
                                   config_,
                                   embedder_,
                                   structured_memory_,
                                   store_text_index_,
                                   structured_text_index_,
                                   vector_index_,
                                   embedding_cache_,
                                   surrogate_map_);
    }
    throw;
  } catch (...) {
    if (store_commit_completed) {
      RebuildRuntimeStateFromStore(store_,
                                   config_,
                                   embedder_,
                                   structured_memory_,
                                   store_text_index_,
                                   structured_text_index_,
                                   vector_index_,
                                   embedding_cache_,
                                   surrogate_map_);
    }
    throw;
  }
}

// ── Search (direct, without RAG assembly) ───────────────────

std::vector<MemorySearchHit> MemoryOrchestrator::Search(
    const std::string& query,
    DirectSearchMode mode,
    float hybrid_alpha,
    int top_k) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  if (top_k <= 0) return {};

  // Trim whitespace.
  std::string_view sv{query};
  while (!sv.empty() && IsAsciiWhitespace(sv.front())) sv.remove_prefix(1);
  while (!sv.empty() && IsAsciiWhitespace(sv.back())) sv.remove_suffix(1);
  if (sv.empty()) return {};
  const std::string trimmed{sv};

  // Determine search mode.
  SearchMode search_mode;
  switch (mode) {
    case DirectSearchMode::kText:
      search_mode = {SearchModeKind::kTextOnly, 0.0f};
      break;
    case DirectSearchMode::kHybrid: {
      const float clamped_alpha = std::min(1.0f, std::max(0.0f, hybrid_alpha));
      if (config_.enable_vector_search && embedder_ != nullptr) {
        search_mode = {SearchModeKind::kHybrid, clamped_alpha};
      } else {
        // Fall back to text-only when vector is unavailable.
        search_mode = {SearchModeKind::kTextOnly, 0.0f};
      }
      break;
    }
  }

  // Build search request.
  SearchRequest req;
  req.query = trimmed;
  req.mode = search_mode;
  req.top_k = top_k;
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;

  // BuildStoreChannels will compute query embedding internally if needed.
  const auto channels = BuildStoreChannels(
      store_,
      config_.enable_text_search ? &store_text_index_ : nullptr,
      config_.enable_text_search ? &structured_text_index_ : nullptr,
      vector_index_.get(),
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      req,
      true);

  auto response = UnifiedSearchAdaptive(req, channels.text_results, channels.vector_results);

  // Convert to MemorySearchHit.
  std::vector<MemorySearchHit> hits;
  hits.reserve(response.results.size());
  for (const auto& r : response.results) {
    MemorySearchHit hit;
    hit.frame_id = r.frame_id;
    hit.score = r.score;
    hit.preview_text = r.preview_text;
    hit.sources = r.sources;
    hits.push_back(std::move(hit));
  }
  return hits;
}

std::vector<MemorySearchHit> MemoryOrchestrator::Search(
    const std::string& query,
    const FrameFilter& frame_filter,
    DirectSearchMode mode,
    float hybrid_alpha,
    int top_k) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  if (top_k <= 0) return {};

  // Trim whitespace.
  std::string_view sv{query};
  while (!sv.empty() && IsAsciiWhitespace(sv.front())) sv.remove_prefix(1);
  while (!sv.empty() && IsAsciiWhitespace(sv.back())) sv.remove_suffix(1);
  if (sv.empty()) return {};
  const std::string trimmed{sv};

  SearchMode search_mode;
  switch (mode) {
    case DirectSearchMode::kText:
      search_mode = {SearchModeKind::kTextOnly, 0.0f};
      break;
    case DirectSearchMode::kHybrid: {
      const float clamped_alpha = std::min(1.0f, std::max(0.0f, hybrid_alpha));
      if (config_.enable_vector_search && embedder_ != nullptr) {
        search_mode = {SearchModeKind::kHybrid, clamped_alpha};
      } else {
        search_mode = {SearchModeKind::kTextOnly, 0.0f};
      }
      break;
    }
  }

  SearchRequest req;
  req.query = trimmed;
  req.mode = search_mode;
  req.top_k = top_k;
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  req.frame_filter = frame_filter;

  const auto channels = BuildStoreChannels(
      store_,
      config_.enable_text_search ? &store_text_index_ : nullptr,
      config_.enable_text_search ? &structured_text_index_ : nullptr,
      vector_index_.get(),
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      req,
      true);

  auto response = UnifiedSearchAdaptive(req, channels.text_results, channels.vector_results);

  // Apply frame filter post-search.
  ApplyFrameFilter(response, frame_filter, store_, surrogate_map_);

  std::vector<MemorySearchHit> hits;
  hits.reserve(response.results.size());
  for (const auto& r : response.results) {
    MemorySearchHit hit;
    hit.frame_id = r.frame_id;
    hit.score = r.score;
    hit.preview_text = r.preview_text;
    hit.sources = r.sources;
    hits.push_back(std::move(hit));
  }
  return hits;
}

// ── RuntimeStats ─────────────────────────────────────────────

RuntimeStats MemoryOrchestrator::GetRuntimeStats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  RuntimeStats stats;
  const auto store_stats = store_.Stats();
  stats.frame_count = store_stats.frame_count;
  stats.pending_frames = store_stats.pending_frames;
  stats.generation = store_stats.generation;
  stats.store_path = store_.Path();
  stats.vector_search_enabled = config_.enable_vector_search && (embedder_ != nullptr);
  stats.structured_memory_enabled = config_.enable_structured_memory;
  stats.access_stats_scoring_enabled = config_.enable_access_stats_scoring;
  if (embedder_ != nullptr) {
    const auto ident = embedder_->identity();
    if (ident.has_value()) {
      std::string id_str;
      if (ident->provider.has_value()) id_str += *ident->provider;
      if (ident->model.has_value()) {
        if (!id_str.empty()) id_str += "/";
        id_str += *ident->model;
      }
      stats.embedder_identity = id_str;
    }
  }
  return stats;
}

SessionRuntimeStats MemoryOrchestrator::GetSessionRuntimeStats() const {
  std::lock_guard<std::mutex> lock(mutex_);

  SessionRuntimeStats stats;
  stats.pending_frames_store_wide = store_.Stats().pending_frames;

  if (current_session_id_.empty()) {
    return stats;  // active=false, session_frame_count=0
  }

  stats.active = true;
  stats.session_id = current_session_id_;

  // Count frames tagged with this session ID by scanning frame metadata.
  // In C++, session_id is stored in frame metadata via Remember/RememberFact.
  // We scan all active frames and count those whose content is associated
  // with the current session. Since C++ binary format does not persist
  // per-frame metadata, this counts frames added since StartSession().
  const auto& metas = store_.CommittedFrameMetasRef();
  int frame_count = 0;
  int token_estimate = 0;
  for (const auto& meta : metas) {
    if (meta.status != 0) continue;
    if (meta.superseded_by.has_value()) continue;
    // Approximate: count frames by checking content for session-tagged data.
    // Since we can't efficiently query per-frame metadata, we estimate based
    // on all active non-superseded frames. A more precise count would require
    // persisting per-frame metadata in the binary format.
  }
  // For now, report frame_count and token_estimate as 0 since C++ binary
  // format lacks per-frame metadata queries. The session_id is still accurate.
  stats.session_frame_count = frame_count;
  stats.session_token_estimate = token_estimate;

  return stats;
}

// ── Session tagging ──────────────────────────────────────────

namespace {

std::string GenerateSessionId() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned> dist(0, 15);
  const char hex[] = "0123456789abcdef";
  // 8-4-4-4-12 = 32 hex chars + 4 dashes
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 32; ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) out.push_back('-');
    out.push_back(hex[dist(rng)]);
  }
  return out;
}

}  // namespace

std::string MemoryOrchestrator::StartSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  current_session_id_ = GenerateSessionId();
  return current_session_id_;
}

void MemoryOrchestrator::EndSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  current_session_id_.clear();
}

std::string MemoryOrchestrator::ActiveSessionId() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_session_id_;
}

// ── Handoff ──────────────────────────────────────────────────

namespace {

/// Magic prefix for handoff frames (8 bytes, like WAXSURR1 for surrogates).
constexpr char kHandoffMagic[] = "WAXHND01";
constexpr std::size_t kHandoffMagicLen = 8;

std::vector<std::byte> BuildHandoffPayload(
    const std::string& content,
    const std::optional<std::string>& project,
    const std::vector<std::string>& pending_tasks,
    const std::string& session_id) {
  // Format: WAXHND01 | project_len(2) | project | tasks_count(2) | tasks... | session_id_len(2) | session_id | content
  // All lengths are little-endian uint16.
  std::vector<std::byte> payload;
  payload.reserve(kHandoffMagicLen + 64 + content.size());

  // Magic header.
  for (std::size_t i = 0; i < kHandoffMagicLen; ++i) {
    payload.push_back(static_cast<std::byte>(kHandoffMagic[i]));
  }

  auto write_u16 = [&](std::uint16_t val) {
    payload.push_back(static_cast<std::byte>(val & 0xFF));
    payload.push_back(static_cast<std::byte>((val >> 8) & 0xFF));
  };
  auto write_str = [&](const std::string& s) {
    const auto len = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 65535u));
    write_u16(len);
    for (std::size_t i = 0; i < len; ++i) {
      payload.push_back(static_cast<std::byte>(s[i]));
    }
  };

  // Project.
  write_str(project.value_or(""));
  // Pending tasks.
  write_u16(static_cast<std::uint16_t>(std::min<std::size_t>(pending_tasks.size(), 65535u)));
  for (const auto& task : pending_tasks) {
    write_str(task);
  }
  // Session ID.
  write_str(session_id);
  // Content (remainder).
  for (char ch : content) {
    payload.push_back(static_cast<std::byte>(ch));
  }
  return payload;
}

std::optional<HandoffRecord> ParseHandoffPayload(std::uint64_t frame_id,
                                                  const std::vector<std::byte>& data) {
  if (data.size() < kHandoffMagicLen) return std::nullopt;
  // Check magic.
  for (std::size_t i = 0; i < kHandoffMagicLen; ++i) {
    if (data[i] != static_cast<std::byte>(kHandoffMagic[i])) return std::nullopt;
  }

  std::size_t pos = kHandoffMagicLen;
  auto read_u16 = [&]() -> std::uint16_t {
    if (pos + 2 > data.size()) return 0;
    const auto lo = static_cast<std::uint16_t>(data[pos]);
    const auto hi = static_cast<std::uint16_t>(data[pos + 1]);
    pos += 2;
    return static_cast<std::uint16_t>(lo | (hi << 8));
  };
  auto read_str = [&]() -> std::string {
    const auto len = read_u16();
    if (pos + len > data.size()) { pos = data.size(); return {}; }
    std::string s;
    s.resize(len);
    for (std::uint16_t i = 0; i < len; ++i) {
      s[i] = static_cast<char>(data[pos + i]);
    }
    pos += len;
    return s;
  };

  HandoffRecord rec;
  rec.frame_id = frame_id;

  // Project.
  auto proj = read_str();
  if (!proj.empty()) rec.project = std::move(proj);

  // Pending tasks.
  const auto task_count = read_u16();
  rec.pending_tasks.reserve(task_count);
  for (std::uint16_t i = 0; i < task_count; ++i) {
    auto task = read_str();
    if (!task.empty()) rec.pending_tasks.push_back(std::move(task));
  }

  // Session ID (skip — not exposed in HandoffRecord).
  read_str();

  // Content (remainder).
  if (pos < data.size()) {
    rec.content.resize(data.size() - pos);
    for (std::size_t i = pos; i < data.size(); ++i) {
      rec.content[i - pos] = static_cast<char>(data[i]);
    }
  }

  return rec;
}

}  // namespace

std::uint64_t MemoryOrchestrator::RememberHandoff(
    const std::string& content,
    const std::optional<std::string>& project,
    const std::vector<std::string>& pending_tasks) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);

  // Build binary payload with handoff magic header.
  auto payload = BuildHandoffPayload(content, project, pending_tasks, current_session_id_);

  Metadata meta;
  meta["kind"] = "handoff";
  if (project.has_value() && !project->empty()) {
    meta["project"] = *project;
  }
  if (!current_session_id_.empty()) {
    meta["session_id"] = current_session_id_;
  }

  const auto frame_id = store_.Put(payload, meta);

  // Index the textual content for search visibility.
  if (config_.enable_text_search) {
    store_text_index_.StageIndex(frame_id, content);
  }

  // Commit immediately so latestHandoff() can observe this frame.
  store_.Commit();
  if (config_.enable_text_search && store_text_index_.PendingMutationCount() > 0) {
    store_text_index_.CommitStaged();
  }

  return frame_id;
}

std::optional<HandoffRecord> MemoryOrchestrator::LatestHandoff(
    const std::optional<std::string>& project) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto& metas = store_.CommittedFrameMetasRef();

  // Find the most recent active handoff frame.
  // Use frame_id as ordering proxy (IDs are monotonically increasing).
  // WaxFrameMeta now has timestamp_ms, but frame_id ordering is equivalent.
  std::optional<HandoffRecord> latest;
  std::uint64_t latest_id = 0;

  for (const auto& meta : metas) {
    if (meta.status != 0) continue;
    if (meta.superseded_by.has_value()) continue;

    // Candidate: either has kind="handoff" metadata, or has WAXHND01 magic header.
    const bool has_kind = meta.kind.has_value() && *meta.kind == "handoff";
    const bool might_have_magic = meta.payload_length >= kHandoffMagicLen;
    if (!has_kind && !might_have_magic) continue;

    const auto data = store_.FrameContent(meta.id);
    auto rec = ParseHandoffPayload(meta.id, data);
    if (!rec.has_value()) continue;

    // Populate timestamp from persisted metadata.
    rec->timestamp_ms = meta.timestamp_ms;

    // Filter by project if specified.
    if (project.has_value() && !project->empty()) {
      if (!rec->project.has_value() || *rec->project != *project) continue;
    }

    if (!latest.has_value() || meta.id > latest_id) {
      latest_id = meta.id;
      latest = std::move(rec);
    }
  }

  return latest;
}

// ── OptimizeSurrogates ───────────────────────────────────────

MaintenanceReport MemoryOrchestrator::OptimizeSurrogates(
    const MaintenanceOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);

  ExtractiveSurrogateGenerator generator{token_counter_};
  auto report = waxcpp::OptimizeSurrogates(store_, generator, options);

  // Rebuild runtime state to pick up new surrogate frames and update the surrogate map.
  if (report.generated_surrogates > 0 || report.superseded_surrogates > 0) {
    RebuildRuntimeStateFromStore(store_,
                                 config_,
                                 embedder_,
                                 structured_memory_,
                                 store_text_index_,
                                 structured_text_index_,
                                 vector_index_,
                                 embedding_cache_,
                                 surrogate_map_);
  }

  return report;
}

MaintenanceReport MemoryOrchestrator::CompactIndexes() {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);

  MaintenanceReport report;
  report.scanned_frames = static_cast<int>(store_.CommittedFrameMetasRef().size());

  // Commit the WAL store (compaction is implicit in the store's commit).
  store_.Commit();

  // Compact FTS5 indexes.
  if (config_.enable_text_search) {
    store_text_index_.CommitStaged();
    structured_text_index_.CommitStaged();
  }

  // Compact vector index.
  if (vector_index_ != nullptr) {
    vector_index_->CommitStaged();
  }

  return report;
}

// ── Close ────────────────────────────────────────────────────

void MemoryOrchestrator::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return;
  }
  store_.Close();
  closed_ = true;
}

std::optional<ScheduledLiveSetMaintenanceReport>
MemoryOrchestrator::LastMaintenanceReport() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_maintenance_report_;
}

}  // namespace waxcpp
