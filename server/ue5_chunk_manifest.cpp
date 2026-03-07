#include "ue5_chunk_manifest.hpp"
#include "server_utils.hpp"

#include "waxcpp/token_counter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace waxcpp::server {

namespace {

struct ChunkWindow {
  std::size_t start_line = 0;
  std::size_t end_line = 0;  // inclusive
  int token_estimate = 0;
};

std::string TrimAscii(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines{};
  std::string current{};
  current.reserve(128);
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
      lines.push_back(current);
      current.clear();
      continue;
    }
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}

std::string JoinLines(const std::vector<std::string>& lines, std::size_t start, std::size_t end_inclusive) {
  if (start >= lines.size() || end_inclusive >= lines.size() || start > end_inclusive) {
    return {};
  }
  std::ostringstream out;
  for (std::size_t i = start; i <= end_inclusive; ++i) {
    if (i != start) {
      out << '\n';
    }
    out << lines[i];
  }
  return out.str();
}

std::uint64_t Fnv1a64(std::string_view text) {
  static constexpr std::uint64_t kOffset = 14695981039346656037ull;
  static constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t hash = kOffset;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kPrime;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

std::optional<std::uint64_t> ParseU64(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto value = std::stoull(std::string(text), &consumed, 10);
    if (consumed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

std::string ExtractIdentifierAfterKeyword(std::string_view trimmed, std::string_view keyword) {
  if (!trimmed.starts_with(keyword)) {
    return {};
  }
  std::size_t pos = keyword.size();
  while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
    ++pos;
  }
  const std::size_t start = pos;
  while (pos < trimmed.size()) {
    const unsigned char ch = static_cast<unsigned char>(trimmed[pos]);
    if (!(std::isalnum(ch) || ch == '_' || ch == ':')) {
      break;
    }
    ++pos;
  }
  if (start == pos) {
    return {};
  }
  return std::string(trimmed.substr(start, pos - start));
}

bool LooksLikeControlFlow(std::string_view trimmed) {
  return trimmed.starts_with("if ") || trimmed.starts_with("if(") || trimmed.starts_with("for ") ||
         trimmed.starts_with("for(") || trimmed.starts_with("while ") || trimmed.starts_with("while(") ||
         trimmed.starts_with("switch ") || trimmed.starts_with("switch(") || trimmed.starts_with("return ");
}

std::string ExtractFunctionLikeSymbol(std::string_view trimmed) {
  if (LooksLikeControlFlow(trimmed)) {
    return {};
  }
  const std::size_t open = trimmed.find('(');
  const std::size_t close = trimmed.find(')');
  if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
    return {};
  }
  if (open == 0) {
    return {};
  }
  std::size_t end = open;
  while (end > 0 && std::isspace(static_cast<unsigned char>(trimmed[end - 1]))) {
    --end;
  }
  std::size_t start = end;
  while (start > 0) {
    const unsigned char ch = static_cast<unsigned char>(trimmed[start - 1]);
    if (!(std::isalnum(ch) || ch == '_' || ch == ':' || ch == '~')) {
      break;
    }
    --start;
  }
  if (start == end) {
    return {};
  }
  return std::string(trimmed.substr(start, end - start));
}

std::string ExtractBestEffortSymbol(const std::vector<std::string>& lines,
                                    std::size_t start_line,
                                    std::size_t end_line) {
  for (std::size_t i = start_line; i <= end_line && i < lines.size(); ++i) {
    const auto trimmed = TrimAscii(lines[i]);
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed.starts_with("//") || trimmed.starts_with("/*") || trimmed.starts_with("*")) {
      continue;
    }
    if (const auto symbol = ExtractIdentifierAfterKeyword(trimmed, "class "); !symbol.empty()) {
      return symbol;
    }
    if (const auto symbol = ExtractIdentifierAfterKeyword(trimmed, "struct "); !symbol.empty()) {
      return symbol;
    }
    if (const auto symbol = ExtractIdentifierAfterKeyword(trimmed, "enum "); !symbol.empty()) {
      return symbol;
    }
    if (const auto symbol = ExtractIdentifierAfterKeyword(trimmed, "namespace "); !symbol.empty()) {
      return symbol;
    }
    if (const auto symbol = ExtractFunctionLikeSymbol(trimmed); !symbol.empty()) {
      return symbol;
    }
  }
  return {};
}

std::vector<ChunkWindow> BuildChunkWindows(const std::vector<int>& line_tokens,
                                           const waxcpp::ChunkingStrategy& strategy) {
  std::vector<ChunkWindow> windows{};
  if (line_tokens.empty()) {
    return windows;
  }

  const int target = std::max(1, strategy.target_tokens);
  const int overlap = std::max(0, strategy.overlap_tokens);
  std::size_t start = 0;
  while (start < line_tokens.size()) {
    std::size_t end = start;
    int token_sum = 0;
    while (end < line_tokens.size()) {
      const int next_tokens = std::max(1, line_tokens[end]);
      if (end > start && token_sum + next_tokens > target) {
        break;
      }
      token_sum += next_tokens;
      ++end;
      if (token_sum >= target) {
        break;
      }
    }
    if (end == start) {
      end = start + 1;
      token_sum = std::max(1, line_tokens[start]);
    }
    windows.push_back(ChunkWindow{
        .start_line = start,
        .end_line = end - 1,
        .token_estimate = token_sum,
    });
    if (end >= line_tokens.size()) {
      break;
    }

    std::size_t next_start = end;
    int overlap_sum = 0;
    while (next_start > start) {
      const std::size_t prev = next_start - 1;
      const int prev_tokens = std::max(1, line_tokens[prev]);
      if (overlap_sum + prev_tokens > overlap) {
        break;
      }
      overlap_sum += prev_tokens;
      next_start = prev;
    }
    if (next_start <= start) {
      next_start = start + 1;
    }
    start = next_start;
  }
  return windows;
}

}  // namespace

Ue5ChunkManifestBuilder::Ue5ChunkManifestBuilder(Ue5ChunkingConfig config) : config_(std::move(config)) {}

std::vector<Ue5ChunkRecord> Ue5ChunkManifestBuilder::Build(
    const std::filesystem::path& repo_root,
    const std::vector<Ue5ScanEntry>& entries,
    const ChunkVisitor& on_chunk,
    std::vector<Ue5FileDigest>* file_digests_out,
    const std::unordered_set<std::string>* skip_paths) const {
  waxcpp::TokenCounter token_counter{};
  std::vector<Ue5ChunkRecord> records{};
  records.reserve(entries.size() * 2);
  if (file_digests_out != nullptr) {
    file_digests_out->clear();
    file_digests_out->reserve(entries.size());
  }

  for (const auto& entry : entries) {
    if (skip_paths != nullptr && skip_paths->contains(entry.relative_path)) {
      continue;
    }
    const auto full_path = repo_root / entry.relative_path;
    const auto content = ReadFileText(full_path);
    const auto file_hash = Hex64(Fnv1a64(content));
    if (file_digests_out != nullptr) {
      file_digests_out->push_back(Ue5FileDigest{
          .relative_path = entry.relative_path,
          .size_bytes = entry.size_bytes,
          .content_hash = file_hash,
      });
    }
    const auto lines = SplitLines(content);
    if (lines.empty()) {
      continue;
    }

    std::vector<int> line_tokens{};
    line_tokens.reserve(lines.size());
    for (const auto& line : lines) {
      line_tokens.push_back(std::max(1, token_counter.Count(line)));
    }

    const auto windows = BuildChunkWindows(line_tokens, config_.strategy);
    const auto language = DetectLanguage(entry.relative_path);
    for (const auto& window : windows) {
      const auto chunk_text = JoinLines(lines, window.start_line, window.end_line);
      if (TrimAscii(chunk_text).empty()) {
        continue;
      }
      const auto content_hash = Hex64(Fnv1a64(chunk_text));
      const auto symbol = config_.include_symbol_metadata
                              ? ExtractBestEffortSymbol(lines, window.start_line, window.end_line)
                              : std::string{};
      std::ostringstream id_material;
      id_material << entry.relative_path << '\n' << symbol << '\n' << (window.start_line + 1) << ':'
                  << (window.end_line + 1) << '\n' << content_hash;

      Ue5ChunkRecord record{
          .chunk_id = Hex64(Fnv1a64(id_material.str())),
          .relative_path = entry.relative_path,
          .language = language,
          .symbol = symbol,
          .line_start = static_cast<std::uint32_t>(window.start_line + 1),
          .line_end = static_cast<std::uint32_t>(window.end_line + 1),
          .token_estimate = static_cast<std::uint32_t>(std::max(1, window.token_estimate)),
          .content_hash = content_hash,
          .size_bytes = static_cast<std::uint64_t>(chunk_text.size()),
      };
      if (on_chunk) {
        on_chunk(record, chunk_text);
      }
      records.push_back(std::move(record));
    }
  }

  // Precompute lowercase sort keys to avoid O(n log n) redundant lowercasing.
  {
    std::unordered_map<std::string, std::string> lower_cache{};
    lower_cache.reserve(records.size());
    auto get_lower = [&](const std::string& path) -> const std::string& {
      auto [it, inserted] = lower_cache.emplace(path, std::string{});
      if (inserted) {
        it->second = ToAsciiLower(path);
      }
      return it->second;
    };
    std::sort(records.begin(), records.end(),
              [&](const Ue5ChunkRecord& lhs, const Ue5ChunkRecord& rhs) {
                const auto& lhs_path = get_lower(lhs.relative_path);
                const auto& rhs_path = get_lower(rhs.relative_path);
                if (lhs_path != rhs_path) {
                  return lhs_path < rhs_path;
                }
                if (lhs.line_start != rhs.line_start) {
                  return lhs.line_start < rhs.line_start;
                }
                if (lhs.line_end != rhs.line_end) {
                  return lhs.line_end < rhs.line_end;
                }
                return lhs.chunk_id < rhs.chunk_id;
              });
  }
  if (file_digests_out != nullptr) {
    std::unordered_map<std::string, std::string> lower_cache{};
    lower_cache.reserve(file_digests_out->size());
    auto get_lower = [&](const std::string& path) -> const std::string& {
      auto [it, inserted] = lower_cache.emplace(path, std::string{});
      if (inserted) {
        it->second = ToAsciiLower(path);
      }
      return it->second;
    };
    std::sort(file_digests_out->begin(),
              file_digests_out->end(),
              [&](const Ue5FileDigest& lhs, const Ue5FileDigest& rhs) {
                const auto& lhs_key = get_lower(lhs.relative_path);
                const auto& rhs_key = get_lower(rhs.relative_path);
                if (lhs_key == rhs_key) {
                  return lhs.relative_path < rhs.relative_path;
                }
                return lhs_key < rhs_key;
              });
  }
  return records;
}

std::string Ue5ChunkManifestBuilder::SerializeManifest(const std::vector<Ue5ChunkRecord>& records) {
  std::ostringstream out;
  for (const auto& record : records) {
    out << record.chunk_id << '\t' << record.relative_path << '\t' << record.language << '\t'
        << record.symbol << '\t' << record.line_start << '\t' << record.line_end << '\t'
        << record.token_estimate << '\t' << record.content_hash << '\t' << record.size_bytes << '\n';
  }
  return out.str();
}

std::string Ue5ChunkManifestBuilder::SerializeFileManifest(const std::vector<Ue5FileDigest>& digests) {
  std::ostringstream out;
  for (const auto& digest : digests) {
    out << digest.relative_path << '\t' << digest.size_bytes << '\t' << digest.content_hash << '\n';
  }
  return out.str();
}

std::vector<Ue5FileDigest> Ue5ChunkManifestBuilder::ParseFileManifest(std::string_view manifest) {
  std::vector<Ue5FileDigest> digests{};
  std::size_t cursor = 0;
  while (cursor < manifest.size()) {
    const auto line_end = manifest.find('\n', cursor);
    const auto line = manifest.substr(cursor,
                                      line_end == std::string_view::npos ? std::string_view::npos : line_end - cursor);
    cursor = line_end == std::string_view::npos ? manifest.size() : line_end + 1;
    if (line.empty()) {
      continue;
    }
    const auto first_tab = line.find('\t');
    if (first_tab == std::string_view::npos) {
      continue;
    }
    const auto second_tab = line.find('\t', first_tab + 1);
    if (second_tab == std::string_view::npos) {
      continue;
    }
    const auto path = std::string(line.substr(0, first_tab));
    const auto size_text = line.substr(first_tab + 1, second_tab - first_tab - 1);
    const auto hash_text = line.substr(second_tab + 1);
    if (path.empty() || hash_text.empty()) {
      continue;
    }
    const auto parsed_size = ParseU64(size_text);
    if (!parsed_size.has_value()) {
      continue;
    }
    digests.push_back(Ue5FileDigest{
        .relative_path = path,
        .size_bytes = *parsed_size,
        .content_hash = std::string(hash_text),
    });
  }
  {
    std::unordered_map<std::string, std::string> lower_cache{};
    lower_cache.reserve(digests.size());
    auto get_lower = [&](const std::string& path) -> const std::string& {
      auto [it, inserted] = lower_cache.emplace(path, std::string{});
      if (inserted) {
        it->second = ToAsciiLower(path);
      }
      return it->second;
    };
    std::sort(digests.begin(), digests.end(),
              [&](const Ue5FileDigest& lhs, const Ue5FileDigest& rhs) {
                const auto& lhs_key = get_lower(lhs.relative_path);
                const auto& rhs_key = get_lower(rhs.relative_path);
                if (lhs_key == rhs_key) {
                  return lhs.relative_path < rhs.relative_path;
                }
                return lhs_key < rhs_key;
              });
  }
  return digests;
}

std::unordered_set<std::string> Ue5ChunkManifestBuilder::ComputeUnchangedPaths(
    const std::vector<Ue5FileDigest>& previous,
    const std::vector<Ue5FileDigest>& current) {
  std::unordered_map<std::string, Ue5FileDigest> previous_by_path{};
  previous_by_path.reserve(previous.size());
  for (const auto& digest : previous) {
    previous_by_path[digest.relative_path] = digest;
  }

  std::unordered_set<std::string> unchanged{};
  unchanged.reserve(current.size());
  for (const auto& digest : current) {
    const auto it = previous_by_path.find(digest.relative_path);
    if (it == previous_by_path.end()) {
      continue;
    }
    if (it->second.size_bytes == digest.size_bytes && it->second.content_hash == digest.content_hash) {
      unchanged.insert(digest.relative_path);
    }
  }
  return unchanged;
}

std::string Ue5ChunkManifestBuilder::DetectLanguage(const std::string& relative_path) {
  const auto extension = ToAsciiLower(std::filesystem::path(relative_path).extension().string());
  if (extension == ".h" || extension == ".hpp" || extension == ".cpp" || extension == ".inl" ||
      extension == ".inc") {
    return "cpp";
  }
  if (extension == ".json") {
    return "json";
  }
  if (extension == ".bpl_json") {
    return "bpl_json";
  }
  return "unknown";
}

}  // namespace waxcpp::server
