#include "waxcpp/embeddings.hpp"

#include "../core/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef WAXCPP_HAS_TORCH_RUNTIME
#define WAXCPP_HAS_TORCH_RUNTIME 0
#endif

#if WAXCPP_HAS_TORCH_RUNTIME
#include <torch/torch.h>
#include <torch/script.h>
#endif

namespace waxcpp {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::string ToAsciiLowerString(std::string_view text) {
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

std::optional<std::string> GetEnvValue(const char* name) {
#ifdef _WIN32
  char* raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
    return std::nullopt;
  }
  std::string value(raw);
  std::free(raw);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
#else
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  return std::string(raw);
#endif
}

bool EnvIsTruthy(const char* name) {
  const auto raw = GetEnvValue(name);
  if (!raw.has_value()) {
    return false;
  }
  const auto value = ToAsciiLowerString(*raw);
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::optional<bool> ParseOptionalBoolEnv(const char* name) {
  const auto raw = GetEnvValue(name);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  const auto value = ToAsciiLowerString(*raw);
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  throw std::runtime_error(std::string("invalid boolean env value for ") + name + ": " + *raw);
}

std::string ResolveTorchRuntimePolicy() {
  const auto raw = GetEnvValue("WAXCPP_TORCH_RUNTIME");
  if (!raw.has_value()) {
    return "cpu_only";
  }
  const auto value = ToAsciiLowerString(*raw);
  if (value == "cpu_only" || value == "cuda_preferred") {
    return value;
  }
  throw std::runtime_error("invalid WAXCPP_TORCH_RUNTIME; expected cpu_only or cuda_preferred");
}

bool DetectCudaRuntimeAvailable() {
  if (const auto assumed = ParseOptionalBoolEnv("WAXCPP_TORCH_ASSUME_CUDA_AVAILABLE"); assumed.has_value()) {
    return *assumed;
  }

#if WAXCPP_HAS_TORCH_RUNTIME
  try {
    return torch::cuda::is_available();
  } catch (...) {
    return false;
  }
#else
  return false;
#endif
}

std::optional<std::filesystem::path> ResolveTorchScriptModulePath() {
  const auto raw = GetEnvValue("WAXCPP_TORCH_SCRIPT_MODULE");
  if (!raw.has_value()) {
    return std::nullopt;
  }

  const std::filesystem::path candidate(*raw);
  if (!std::filesystem::exists(candidate) || !std::filesystem::is_regular_file(candidate)) {
    throw std::runtime_error("WAXCPP_TORCH_SCRIPT_MODULE does not exist or is not a regular file");
  }
  return std::filesystem::absolute(candidate);
}

bool ResolveRealTorchRuntimeEnabled() {
#if WAXCPP_HAS_TORCH_RUNTIME
  if (const auto enabled = ParseOptionalBoolEnv("WAXCPP_ENABLE_REAL_TORCH_RUNTIME"); enabled.has_value()) {
    return *enabled;
  }
  return true;
#else
  if (const auto enabled = ParseOptionalBoolEnv("WAXCPP_ENABLE_REAL_TORCH_RUNTIME"); enabled.has_value() && *enabled) {
    return true;
  }
  return false;
#endif
}

bool IsAsciiHex(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

std::optional<std::filesystem::path> ResolveLibTorchManifestPath(bool* override_was_set = nullptr) {
  if (override_was_set != nullptr) {
    *override_was_set = false;
  }

  if (const auto raw_override = GetEnvValue("WAXCPP_LIBTORCH_MANIFEST"); raw_override.has_value()) {
    if (override_was_set != nullptr) {
      *override_was_set = true;
    }
    const std::filesystem::path candidate(*raw_override);
    if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::absolute(candidate);
    }
    return std::nullopt;
  }

  const auto cwd = std::filesystem::current_path();
  const std::vector<std::filesystem::path> candidates = {
      cwd / "manifest" / "libtorch-manifest.json",
      cwd / ".." / "manifest" / "libtorch-manifest.json",
      cwd / ".." / ".." / "manifest" / "libtorch-manifest.json",
      cwd / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / ".." / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / ".." / ".." / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / "cpp" / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / "cpp" / "manifest" / "libtorch-manifest.json",
  };
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::absolute(candidate);
    }
  }
  return std::nullopt;
}

std::string_view TrimAsciiWhitespace(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    const char ch = text[begin];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      ++begin;
      continue;
    }
    break;
  }

  std::size_t end = text.size();
  while (end > begin) {
    const char ch = text[end - 1];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      --end;
      continue;
    }
    break;
  }

  return text.substr(begin, end - begin);
}

std::optional<std::pair<std::size_t, std::size_t>> FindDelimitedRange(
    std::string_view text,
    std::size_t open_index,
    char open_char,
    char close_char) {
  if (open_index >= text.size() || text[open_index] != open_char) {
    return std::nullopt;
  }

  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t i = open_index; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == open_char) {
      ++depth;
      continue;
    }
    if (ch == close_char) {
      --depth;
      if (depth == 0) {
        return std::make_pair(open_index, i);
      }
      if (depth < 0) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> FindArtifactArrayRange(std::string_view json) {
  if (json.empty()) {
    return std::nullopt;
  }
  if (json.front() == '[') {
    return FindDelimitedRange(json, 0, '[', ']');
  }

  constexpr std::array<std::string_view, 3> kArtifactKeys = {
      "\"artifacts\"",
      "\"files\"",
      "\"entries\"",
  };

  for (const auto key : kArtifactKeys) {
    std::size_t cursor = 0;
    while (cursor < json.size()) {
      const auto key_pos = json.find(key, cursor);
      if (key_pos == std::string_view::npos) {
        break;
      }
      const auto colon = json.find(':', key_pos + key.size());
      if (colon == std::string_view::npos) {
        break;
      }
      const auto array_open = json.find('[', colon + 1);
      if (array_open == std::string_view::npos) {
        break;
      }
      if (const auto range = FindDelimitedRange(json, array_open, '[', ']'); range.has_value()) {
        return range;
      }
      cursor = key_pos + key.size();
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> ExtractJsonStringField(std::string_view object,
                                                       std::string_view key_a,
                                                       std::string_view key_b) {
  if (object.size() < 2 || object.front() != '{' || object.back() != '}') {
    return std::nullopt;
  }

  auto parse_string = [&](std::size_t open_quote_index) -> std::optional<std::pair<std::string_view, std::size_t>> {
    if (open_quote_index >= object.size() || object[open_quote_index] != '"') {
      return std::nullopt;
    }
    const auto open_quote = open_quote_index;
    bool escaped_inner = false;
    for (std::size_t i = open_quote + 1; i < object.size(); ++i) {
      const char ch = object[i];
      if (escaped_inner) {
        escaped_inner = false;
        continue;
      }
      if (ch == '\\') {
        escaped_inner = true;
        continue;
      }
      if (ch == '"') {
        if (i == open_quote + 1) {
          return std::nullopt;
        }
        return std::make_pair(object.substr(open_quote + 1, i - open_quote - 1), i);
      }
    }
    return std::nullopt;
  };

  auto key_matches = [&](std::string_view parsed_key) -> bool {
    const auto quoted = std::string("\"") + std::string(parsed_key) + std::string("\"");
    return quoted == key_a || quoted == key_b;
  };

  int nested_depth = 0;
  std::size_t i = 1;
  bool escaped = false;
  bool in_string = false;
  while (i + 1 < object.size()) {
    const char ch = object[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
        ++i;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        ++i;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      ++i;
      continue;
    }

    if (ch == '"') {
      if (nested_depth != 0) {
        in_string = true;
        ++i;
        continue;
      }

      const auto key = parse_string(i);
      if (!key.has_value()) {
        ++i;
        continue;
      }
      const auto [parsed_key, key_end] = *key;
      i = key_end + 1;
      while (i < object.size()) {
        const char ws = object[i];
        if (ws == ' ' || ws == '\t' || ws == '\r' || ws == '\n') {
          ++i;
          continue;
        }
        break;
      }
      if (i >= object.size() || object[i] != ':') {
        continue;
      }
      ++i;
      while (i < object.size()) {
        const char ws = object[i];
        if (ws == ' ' || ws == '\t' || ws == '\r' || ws == '\n') {
          ++i;
          continue;
        }
        break;
      }
      if (!key_matches(parsed_key)) {
        continue;
      }
      if (i >= object.size() || object[i] != '"') {
        return std::nullopt;
      }
      const auto value = parse_string(i);
      if (!value.has_value() || value->first.empty()) {
        return std::nullopt;
      }
      return value->first;
    }

    if (ch == '{' || ch == '[') {
      ++nested_depth;
      ++i;
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (nested_depth > 0) {
        --nested_depth;
      }
      ++i;
      continue;
    }
    ++i;
  }

  return std::nullopt;
}

std::optional<std::string> DecodeJsonEscapes(std::string_view encoded) {
  auto hex_nibble = [](char ch) -> std::optional<std::uint32_t> {
    if (ch >= '0' && ch <= '9') {
      return static_cast<std::uint32_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
      return static_cast<std::uint32_t>(ch - 'a' + 10);
    }
    if (ch >= 'A' && ch <= 'F') {
      return static_cast<std::uint32_t>(ch - 'A' + 10);
    }
    return std::nullopt;
  };

  std::string decoded{};
  decoded.reserve(encoded.size());
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    const char ch = encoded[i];
    if (ch != '\\') {
      decoded.push_back(ch);
      continue;
    }
    if (i + 1 >= encoded.size()) {
      return std::nullopt;
    }
    const char esc = encoded[++i];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        decoded.push_back(esc);
        break;
      case 'b':
        decoded.push_back('\b');
        break;
      case 'f':
        decoded.push_back('\f');
        break;
      case 'n':
        decoded.push_back('\n');
        break;
      case 'r':
        decoded.push_back('\r');
        break;
      case 't':
        decoded.push_back('\t');
        break;
      case 'u': {
        if (i + 4 >= encoded.size()) {
          return std::nullopt;
        }
        std::uint32_t codepoint = 0;
        for (std::size_t digit = 0; digit < 4; ++digit) {
          const auto nibble = hex_nibble(encoded[i + 1 + digit]);
          if (!nibble.has_value()) {
            return std::nullopt;
          }
          codepoint = (codepoint << 4U) | *nibble;
        }
        i += 4;
        if (codepoint > 0x7FU) {
          // Keep parser deterministic and ASCII-only for manifest fields.
          return std::nullopt;
        }
        decoded.push_back(static_cast<char>(codepoint));
        break;
      }
      default:
        // Keep parser deterministic and strict for manifest paths.
        return std::nullopt;
    }
  }
  return decoded;
}

std::optional<std::string> ExtractArtifactPath(std::string_view object) {
  const auto path = ExtractJsonStringField(object, "\"path\"", "\"file\"");
  if (!path.has_value() || path->empty()) {
    return std::nullopt;
  }
  const auto decoded = DecodeJsonEscapes(*path);
  if (!decoded.has_value() || decoded->empty()) {
    return std::nullopt;
  }
  for (const unsigned char ch : *decoded) {
    if (ch < 0x20U || ch == 0x7FU) {
      return std::nullopt;
    }
  }
  return decoded;
}

std::optional<std::string> ExtractArtifactSha256(std::string_view object) {
  const auto sha = ExtractJsonStringField(object, "\"sha256\"", "\"sha256sum\"");
  if (!sha.has_value() || sha->size() != 64) {
    return std::nullopt;
  }
  const auto decoded = DecodeJsonEscapes(*sha);
  if (!decoded.has_value() || decoded->size() != 64) {
    return std::nullopt;
  }
  for (const char ch : *decoded) {
    if (!IsAsciiHex(ch)) {
      return std::nullopt;
    }
  }
  return decoded;
}

bool HasValidSha256(std::string_view object) {
  return ExtractArtifactSha256(object).has_value();
}

struct ManifestArtifactSelection {
  std::string path{};
  std::string sha256{};
};

struct ManifestValidationSummary {
  std::size_t valid_artifact_count = 0;
  std::size_t cpu_artifact_count = 0;
  std::size_t cuda_artifact_count = 0;
  std::optional<ManifestArtifactSelection> any_artifact{};
  std::optional<ManifestArtifactSelection> cpu_artifact{};
  std::optional<ManifestArtifactSelection> cuda_artifact{};
};

std::string HexLower(std::span<const std::byte> bytes) {
  constexpr std::array<char, 16> kHexDigits = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string out{};
  out.reserve(bytes.size() * 2U);
  for (const auto value : bytes) {
    const auto v = std::to_integer<unsigned char>(value);
    out.push_back(kHexDigits[(v >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[v & 0x0FU]);
  }
  return out;
}

std::string ComputeFileSha256Hex(const std::filesystem::path& path) {
  std::error_code size_ec{};
  const auto file_size = std::filesystem::file_size(path, size_ec);
  if (size_ec) {
    throw std::runtime_error("failed to read selected libtorch artifact file size for checksum verification");
  }
  if (file_size == 0) {
    throw std::runtime_error("selected libtorch artifact is empty");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open selected libtorch artifact for checksum verification");
  }

  waxcpp::core::Sha256 hasher;
  std::array<char, 64 * 1024> buffer{};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = input.gcount();
    if (got > 0) {
      hasher.Update(std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(buffer.data()),
          static_cast<std::size_t>(got)));
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("failed to read selected libtorch artifact for checksum verification");
  }

  const auto digest = hasher.Finalize();
  return HexLower(std::span<const std::byte>(digest.data(), digest.size()));
}

bool PathElementEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
#ifdef _WIN32
  return ToAsciiLowerString(lhs.generic_string()) == ToAsciiLowerString(rhs.generic_string());
#else
  return lhs == rhs;
#endif
}

std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& path) {
  std::error_code ec{};
  auto normalized = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    normalized = std::filesystem::absolute(path, ec);
    if (ec) {
      return path.lexically_normal();
    }
  }
  return normalized.lexically_normal();
}

bool IsPathWithinRoot(const std::filesystem::path& candidate, const std::filesystem::path& root) {
  const auto normalized_candidate = NormalizeAbsolutePath(candidate);
  const auto normalized_root = NormalizeAbsolutePath(root);

  if (normalized_root.empty()) {
    return false;
  }

  auto candidate_it = normalized_candidate.begin();
  auto root_it = normalized_root.begin();
  for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == normalized_candidate.end()) {
      return false;
    }
    if (!PathElementEqual(*candidate_it, *root_it)) {
      return false;
    }
  }
  return true;
}

std::optional<std::filesystem::path> ResolveSelectedArtifactPath(
    const std::filesystem::path& manifest_path,
    std::string_view relative_or_absolute_artifact_path) {
  const std::filesystem::path artifact_path(relative_or_absolute_artifact_path);
  const auto dist_root = GetEnvValue("WAXCPP_LIBTORCH_DIST_ROOT");
  if (dist_root.has_value()) {
    const auto root = std::filesystem::path(*dist_root);
    if (artifact_path.is_absolute()) {
      if (!IsPathWithinRoot(artifact_path, root)) {
        return std::nullopt;
      }
      if (std::filesystem::exists(artifact_path) && std::filesystem::is_regular_file(artifact_path)) {
        return NormalizeAbsolutePath(artifact_path);
      }
      return std::nullopt;
    }

    const auto candidate = root / artifact_path;
    if (!IsPathWithinRoot(candidate, root)) {
      return std::nullopt;
    }
    if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
      return NormalizeAbsolutePath(candidate);
    }
    return std::nullopt;
  }

  if (artifact_path.is_absolute()) {
    if (std::filesystem::exists(artifact_path) && std::filesystem::is_regular_file(artifact_path)) {
      return NormalizeAbsolutePath(artifact_path);
    }
    return std::nullopt;
  }

  struct RootedCandidate {
    std::filesystem::path candidate;
    std::filesystem::path root;
  };

  std::vector<RootedCandidate> candidates{};
  const auto manifest_dir = manifest_path.parent_path();
  candidates.push_back(RootedCandidate{
      .candidate = manifest_dir / artifact_path,
      .root = manifest_dir,
  });
  if (manifest_dir.has_parent_path()) {
    candidates.push_back(RootedCandidate{
        .candidate = manifest_dir.parent_path() / artifact_path,
        .root = manifest_dir.parent_path(),
    });
  }

  for (const auto& candidate : candidates) {
    if (!IsPathWithinRoot(candidate.candidate, candidate.root)) {
      continue;
    }
    if (std::filesystem::exists(candidate.candidate) && std::filesystem::is_regular_file(candidate.candidate)) {
      return NormalizeAbsolutePath(candidate.candidate);
    }
  }
  return std::nullopt;
}

bool ArtifactPathLooksCuda(std::string_view path) {
  const auto lower = ToAsciiLowerString(path);
  if (lower.find("cuda") != std::string::npos) {
    return true;
  }
  for (std::size_t i = 0; i + 2 < lower.size(); ++i) {
    if (lower[i] != 'c' || lower[i + 1] != 'u') {
      continue;
    }
    std::size_t j = i + 2;
    std::size_t digit_count = 0;
    while (j < lower.size() && lower[j] >= '0' && lower[j] <= '9') {
      ++digit_count;
      ++j;
    }
    // Common artifact naming includes cuXXX tags, e.g. libtorch-cu124.zip.
    if (digit_count >= 2) {
      return true;
    }
  }
  return false;
}

bool ArtifactPathLooksCpu(std::string_view path) {
  const auto lower = ToAsciiLowerString(path);
  return lower.find("cpu") != std::string::npos;
}

ManifestValidationSummary CountValidArtifactObjects(std::string_view json,
                                                    const std::pair<std::size_t, std::size_t>& array_range) {
  const std::size_t begin = array_range.first + 1;
  const std::size_t end = array_range.second;
  if (begin >= end || end > json.size()) {
    return {};
  }

  ManifestValidationSummary summary{};
  auto update_min_selection = [](std::optional<ManifestArtifactSelection>& slot,
                                 std::string_view candidate_path,
                                 std::string_view candidate_sha256) {
    if (!slot.has_value() ||
        candidate_path < slot->path ||
        (candidate_path == slot->path && candidate_sha256 < slot->sha256)) {
      slot = ManifestArtifactSelection{std::string(candidate_path), std::string(candidate_sha256)};
    }
  };
  bool in_string = false;
  bool escaped = false;
  int brace_depth = 0;
  std::size_t object_begin = std::string_view::npos;
  for (std::size_t i = begin; i < end; ++i) {
    const char ch = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      if (brace_depth == 0) {
        object_begin = i;
      }
      ++brace_depth;
      continue;
    }
    if (ch == '}') {
      if (brace_depth <= 0) {
        continue;
      }
      --brace_depth;
      if (brace_depth == 0 && object_begin != std::string_view::npos && i > object_begin) {
        const auto artifact_object = json.substr(object_begin, i - object_begin + 1);
        const auto path = ExtractArtifactPath(artifact_object);
        const auto sha256 = ExtractArtifactSha256(artifact_object);
        if (path.has_value() && sha256.has_value()) {
          ++summary.valid_artifact_count;
          update_min_selection(summary.any_artifact, *path, *sha256);

          const bool looks_cuda = ArtifactPathLooksCuda(*path);
          const bool looks_cpu = ArtifactPathLooksCpu(*path);
          if (looks_cuda) {
            ++summary.cuda_artifact_count;
            update_min_selection(summary.cuda_artifact, *path, *sha256);
          }
          if (looks_cpu) {
            ++summary.cpu_artifact_count;
            update_min_selection(summary.cpu_artifact, *path, *sha256);
          }
        }
      }
    }
  }
  return summary;
}

ManifestValidationSummary ValidateManifestFile(const std::filesystem::path& manifest_path) {
  constexpr std::uintmax_t kMaxManifestBytes = 8U * 1024U * 1024U;
  std::error_code ec{};
  const auto size = std::filesystem::file_size(manifest_path, ec);
  if (ec) {
    throw std::runtime_error("failed to read libtorch manifest size");
  }
  if (size == 0) {
    throw std::runtime_error("libtorch manifest is empty");
  }
  if (size > kMaxManifestBytes) {
    throw std::runtime_error("libtorch manifest exceeds size limit");
  }

  std::ifstream input(manifest_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open libtorch manifest");
  }
  std::string content(static_cast<std::size_t>(size), '\0');
  if (!input.read(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("failed to read libtorch manifest");
  }

  const auto trimmed = TrimAsciiWhitespace(content);
  if (trimmed.empty()) {
    throw std::runtime_error("libtorch manifest is blank");
  }
  const char first = trimmed.front();
  if (first != '{' && first != '[') {
    throw std::runtime_error("libtorch manifest does not look like JSON");
  }

  const bool has_artifact_list =
      trimmed.find("\"artifacts\"") != std::string_view::npos ||
      trimmed.find("\"files\"") != std::string_view::npos ||
      trimmed.find("\"entries\"") != std::string_view::npos ||
      first == '[';
  if (!has_artifact_list) {
    throw std::runtime_error("libtorch manifest does not define artifact list keys");
  }

  const bool has_path_key =
      trimmed.find("\"path\"") != std::string_view::npos ||
      trimmed.find("\"file\"") != std::string_view::npos;
  if (!has_path_key) {
    throw std::runtime_error("libtorch manifest does not contain artifact path keys");
  }

  const bool has_sha_key =
      trimmed.find("\"sha256\"") != std::string_view::npos ||
      trimmed.find("\"sha256sum\"") != std::string_view::npos;
  if (!has_sha_key) {
    throw std::runtime_error("libtorch manifest does not contain sha256 keys");
  }

  const auto artifacts_range = FindArtifactArrayRange(trimmed);
  if (!artifacts_range.has_value()) {
    throw std::runtime_error("libtorch manifest does not contain a parseable artifact array");
  }

  const auto summary = CountValidArtifactObjects(trimmed, *artifacts_range);
  if (summary.valid_artifact_count == 0) {
    throw std::runtime_error("libtorch manifest does not contain artifact objects with path and valid sha256");
  }

  return summary;
}

void MaybeVerifySelectedArtifactSha256(MiniLMRuntimeInfo& runtime_info,
                                       const std::filesystem::path& manifest_path) {
  if (!runtime_info.libtorch_selected_artifact_path.has_value() ||
      !runtime_info.libtorch_selected_artifact_sha256.has_value()) {
    return;
  }

  const auto resolved_path = ResolveSelectedArtifactPath(
      manifest_path,
      *runtime_info.libtorch_selected_artifact_path);
  if (resolved_path.has_value()) {
    runtime_info.libtorch_selected_artifact_resolved_path = resolved_path->string();
  }

  if (!EnvIsTruthy("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256")) {
    return;
  }
  if (!resolved_path.has_value()) {
    throw std::runtime_error(
        "MiniLMEmbedderTorch selected libtorch artifact was not found for checksum verification");
  }

  const auto actual_sha256 = ComputeFileSha256Hex(*resolved_path);
  const auto expected_sha256 = ToAsciiLowerString(*runtime_info.libtorch_selected_artifact_sha256);
  if (actual_sha256 != expected_sha256) {
    throw std::runtime_error(
        "MiniLMEmbedderTorch selected libtorch artifact checksum mismatch");
  }
  runtime_info.libtorch_selected_artifact_sha256_verified = true;
}

bool IsAsciiAlphaNum(unsigned char ch) {
  return (ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9')) ||
         (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) ||
         (ch >= static_cast<unsigned char>('a') && ch <= static_cast<unsigned char>('z'));
}

char ToAsciiLower(unsigned char ch) {
  if (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) {
    return static_cast<char>(ch - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
  }
  return static_cast<char>(ch);
}

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens{};
  std::string current{};
  current.reserve(32);

  for (const unsigned char ch : text) {
    if (IsAsciiAlphaNum(ch)) {
      current.push_back(ToAsciiLower(ch));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::uint64_t HashToken(std::string_view token) {
  std::uint64_t hash = kFnvOffset;
  for (const unsigned char ch : token) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

void NormalizeL2(std::vector<float>& v) {
  double sum_sq = 0.0;
  for (const auto x : v) {
    sum_sq += static_cast<double>(x) * static_cast<double>(x);
  }
  if (sum_sq <= 0.0) {
    return;
  }
  const auto inv_norm = 1.0 / std::sqrt(sum_sq);
  for (auto& x : v) {
    x = static_cast<float>(static_cast<double>(x) * inv_norm);
  }
}

std::vector<float> BuildFallbackEmbedding(std::string_view text, int dims, bool do_normalize) {
  std::vector<float> embedding(static_cast<std::size_t>(dims), 0.0F);
  const auto tokens = Tokenize(text);
  for (const auto& token : tokens) {
    const auto hash = HashToken(token);
    const auto index = static_cast<std::size_t>(hash % static_cast<std::uint64_t>(dims));
    const float sign = ((hash >> 63U) != 0U) ? -1.0F : 1.0F;
    embedding[index] += sign;
  }

  if (do_normalize) {
    NormalizeL2(embedding);
  }
  return embedding;
}

#if WAXCPP_HAS_TORCH_RUNTIME
std::string TorchModuleCacheKey(const std::filesystem::path& module_path, bool use_cuda) {
  return module_path.string() + "|" + (use_cuda ? "cuda" : "cpu");
}

std::shared_ptr<torch::jit::script::Module> LoadCachedTorchScriptModule(const std::filesystem::path& module_path,
                                                                         bool use_cuda) {
  static std::mutex cache_mutex{};
  static std::unordered_map<std::string, std::shared_ptr<torch::jit::script::Module>> cache{};

  const auto key = TorchModuleCacheKey(module_path, use_cuda);
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
  }

  const auto device = use_cuda ? torch::Device(torch::kCUDA, 0) : torch::Device(torch::kCPU);
  auto module = std::make_shared<torch::jit::script::Module>(torch::jit::load(module_path.string(), device));
  module->eval();

  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto [it, _inserted] = cache.emplace(key, module);
    return it->second;
  }
}

std::vector<float> BuildTorchRuntimeEmbedding(std::string_view text, int dims, bool use_cuda, bool do_normalize) {
  const auto tokens = Tokenize(text);
  if (tokens.empty()) {
    return std::vector<float>(static_cast<std::size_t>(dims), 0.0F);
  }

  std::vector<std::int64_t> indices{};
  std::vector<float> values{};
  indices.reserve(tokens.size());
  values.reserve(tokens.size());
  for (const auto& token : tokens) {
    const auto hash = HashToken(token);
    indices.push_back(static_cast<std::int64_t>(hash % static_cast<std::uint64_t>(dims)));
    const float sign = ((hash >> 63U) != 0U) ? -1.0F : 1.0F;
    values.push_back(sign);
  }

  const auto device = use_cuda ? torch::Device(torch::kCUDA, 0) : torch::Device(torch::kCPU);
  auto embedding = torch::zeros({dims}, torch::TensorOptions().dtype(torch::kFloat32).device(device));
  auto idx_tensor =
      torch::from_blob(indices.data(), {static_cast<std::int64_t>(indices.size())}, torch::TensorOptions().dtype(torch::kInt64))
          .clone()
          .to(device);
  auto val_tensor =
      torch::from_blob(values.data(), {static_cast<std::int64_t>(values.size())}, torch::TensorOptions().dtype(torch::kFloat32))
          .clone()
          .to(device);

  embedding.index_add_(0, idx_tensor, val_tensor);

  if (const auto module_path = ResolveTorchScriptModulePath(); module_path.has_value()) {
    auto module = LoadCachedTorchScriptModule(*module_path, use_cuda);
    std::vector<torch::jit::IValue> inputs{};
    inputs.emplace_back(embedding.unsqueeze(0));
    auto output_ivalue = module->forward(inputs);

    torch::Tensor output{};
    if (output_ivalue.isTensor()) {
      output = output_ivalue.toTensor();
    } else if (output_ivalue.isTuple()) {
      const auto tuple = output_ivalue.toTuple();
      if (tuple != nullptr && !tuple->elements().empty() && tuple->elements().front().isTensor()) {
        output = tuple->elements().front().toTensor();
      }
    }
    if (!output.defined()) {
      throw std::runtime_error("torchscript module forward did not return a tensor");
    }
    output = output.to(device, torch::kFloat32);
    if (output.dim() == 2 && output.size(0) == 1) {
      output = output.squeeze(0);
    }
    if (output.dim() != 1 || output.size(0) != dims) {
      throw std::runtime_error("torchscript module output shape mismatch; expected [384] or [1,384]");
    }
    embedding = output.contiguous();
  }

  if (do_normalize) {
    const auto norm = embedding.norm(2);
    const auto norm_scalar = norm.item<float>();
    if (norm_scalar > 0.0F) {
      embedding = embedding / norm;
    }
  }

  auto cpu_embedding = embedding.to(torch::kCPU).contiguous();
  std::vector<float> out(static_cast<std::size_t>(dims), 0.0F);
  std::memcpy(out.data(), cpu_embedding.data_ptr<float>(), static_cast<std::size_t>(dims) * sizeof(float));
  return out;
}
#endif

}  // namespace

MiniLMEmbedderTorch::MiniLMEmbedderTorch(std::size_t memoization_capacity)
    : memoization_capacity_(memoization_capacity) {
  runtime_info_.libtorch_runtime_compiled = WAXCPP_HAS_TORCH_RUNTIME != 0;
  runtime_info_.runtime_policy = ResolveTorchRuntimePolicy();
  runtime_info_.cuda_preferred_requested = runtime_info_.runtime_policy == "cuda_preferred";
  runtime_info_.libtorch_runtime_strict = EnvIsTruthy("WAXCPP_REQUIRE_REAL_TORCH_RUNTIME");
  runtime_info_.libtorch_runtime_enabled = ResolveRealTorchRuntimeEnabled();
  if (runtime_info_.libtorch_runtime_strict) {
    runtime_info_.libtorch_runtime_enabled = true;
  }
  if (runtime_info_.libtorch_runtime_enabled && !runtime_info_.libtorch_runtime_compiled) {
    throw std::runtime_error("real libtorch runtime is requested but this build was compiled without libtorch");
  }
  if (const auto script_module_path = ResolveTorchScriptModulePath(); script_module_path.has_value()) {
    runtime_info_.libtorch_script_module_path = script_module_path->string();
  }
  runtime_info_.cuda_runtime_available = DetectCudaRuntimeAvailable();
  runtime_info_.selected_backend = "fallback_cpu";
  runtime_info_.fallback_active = true;

  bool override_was_set = false;
  std::optional<ManifestArtifactSelection> manifest_any_artifact{};
  std::optional<ManifestArtifactSelection> manifest_cpu_artifact{};
  std::optional<ManifestArtifactSelection> manifest_cuda_artifact{};
  const auto manifest_path = ResolveLibTorchManifestPath(&override_was_set);
  if (manifest_path.has_value()) {
    runtime_info_.libtorch_manifest_detected = true;
    runtime_info_.libtorch_manifest_path = manifest_path->string();
    try {
      const auto manifest_summary = ValidateManifestFile(*manifest_path);
      runtime_info_.libtorch_manifest_artifact_count = manifest_summary.valid_artifact_count;
      runtime_info_.libtorch_manifest_cpu_artifact_count = manifest_summary.cpu_artifact_count;
      runtime_info_.libtorch_manifest_cuda_artifact_count = manifest_summary.cuda_artifact_count;
      manifest_any_artifact = manifest_summary.any_artifact;
      manifest_cpu_artifact = manifest_summary.cpu_artifact;
      manifest_cuda_artifact = manifest_summary.cuda_artifact;
      runtime_info_.libtorch_manifest_valid = true;
    } catch (const std::exception& ex) {
      throw std::runtime_error(std::string("MiniLMEmbedderTorch libtorch manifest is invalid: ") + ex.what());
    }
  }
  if (EnvIsTruthy("WAXCPP_REQUIRE_LIBTORCH_MANIFEST")) {
    if (!runtime_info_.libtorch_manifest_detected) {
      if (override_was_set) {
        throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest is missing at WAXCPP_LIBTORCH_MANIFEST");
      }
      throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest was not found");
    }
    if (!runtime_info_.libtorch_manifest_valid) {
      throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest is invalid");
    }
  }

  if (runtime_info_.cuda_preferred_requested && runtime_info_.cuda_runtime_available) {
    const bool manifest_allows_cuda =
        !runtime_info_.libtorch_manifest_detected || runtime_info_.libtorch_manifest_cuda_artifact_count > 0;
    if (manifest_allows_cuda) {
      if (runtime_info_.libtorch_runtime_enabled) {
        runtime_info_.selected_backend = "libtorch_cuda";
        runtime_info_.fallback_active = false;
      } else {
        runtime_info_.selected_backend = "fallback_cuda";
      }
    }
  } else if (runtime_info_.libtorch_runtime_enabled) {
    runtime_info_.selected_backend = "libtorch_cpu";
    runtime_info_.fallback_active = false;
  }

  if (runtime_info_.libtorch_manifest_detected && runtime_info_.libtorch_manifest_valid) {
    std::optional<ManifestArtifactSelection> selected_artifact{};
    if (runtime_info_.selected_backend == "fallback_cuda") {
      selected_artifact = manifest_cuda_artifact.has_value() ? manifest_cuda_artifact : manifest_any_artifact;
    } else {
      selected_artifact = manifest_cpu_artifact.has_value() ? manifest_cpu_artifact : manifest_any_artifact;
    }
    if (selected_artifact.has_value()) {
      runtime_info_.libtorch_selected_artifact_path = selected_artifact->path;
      runtime_info_.libtorch_selected_artifact_sha256 = selected_artifact->sha256;
      const bool looks_cuda = ArtifactPathLooksCuda(selected_artifact->path);
      const bool looks_cpu = ArtifactPathLooksCpu(selected_artifact->path);
      if (looks_cuda && !looks_cpu) {
        runtime_info_.libtorch_selected_artifact_class = "cuda";
      } else if (looks_cpu && !looks_cuda) {
        runtime_info_.libtorch_selected_artifact_class = "cpu";
      } else {
        runtime_info_.libtorch_selected_artifact_class = "any";
      }
      if (manifest_path.has_value()) {
        MaybeVerifySelectedArtifactSha256(runtime_info_, *manifest_path);
      }
    }
  }
}

int MiniLMEmbedderTorch::dimensions() const {
  return 384;
}

bool MiniLMEmbedderTorch::normalize() const {
  return true;
}

std::optional<EmbeddingIdentity> MiniLMEmbedderTorch::identity() const {
  return EmbeddingIdentity{
      .provider = std::string("WaxCpp"),
      .model = std::string("MiniLM-Torch"),
      .dimensions = 384,
      .normalized = true,
  };
}

std::vector<float> MiniLMEmbedderTorch::Embed(const std::string& text) {
  if (memoization_capacity_ > 0) {
    std::lock_guard<std::mutex> lock(memoization_mutex_);
    const auto cached = memoized_embeddings_.find(text);
    if (cached != memoized_embeddings_.end()) {
      return cached->second;
    }
  }

  constexpr int kDims = 384;
  std::vector<float> embedding{};
  if (!runtime_info_.fallback_active && runtime_info_.libtorch_runtime_enabled) {
#if WAXCPP_HAS_TORCH_RUNTIME
    const bool use_cuda_backend = runtime_info_.selected_backend == "libtorch_cuda";
    try {
      embedding = BuildTorchRuntimeEmbedding(text, kDims, use_cuda_backend, normalize());
      if (runtime_info_.libtorch_script_module_path.has_value()) {
        runtime_info_.libtorch_script_module_loaded = true;
      }
    } catch (const std::exception& ex) {
      if (runtime_info_.libtorch_runtime_strict) {
        throw std::runtime_error(std::string("real libtorch runtime embedding failed: ") + ex.what());
      }
      {
        std::lock_guard<std::mutex> lock(memoization_mutex_);
        runtime_info_.libtorch_runtime_error = ex.what();
        runtime_info_.libtorch_script_module_loaded = false;
      }
      embedding = BuildFallbackEmbedding(text, kDims, normalize());
    }
#else
    if (runtime_info_.libtorch_runtime_strict) {
      throw std::runtime_error("real libtorch runtime embedding is unavailable in this build");
    }
    embedding = BuildFallbackEmbedding(text, kDims, normalize());
#endif
  } else {
    embedding = BuildFallbackEmbedding(text, kDims, normalize());
  }

  if (memoization_capacity_ > 0) {
    std::lock_guard<std::mutex> lock(memoization_mutex_);
    const auto cached = memoized_embeddings_.find(text);
    if (cached != memoized_embeddings_.end()) {
      return cached->second;
    }
    while (memoized_embeddings_.size() >= memoization_capacity_ && !memoization_order_.empty()) {
      const auto evict_key = memoization_order_.front();
      memoization_order_.pop_front();
      (void)memoized_embeddings_.erase(evict_key);
    }
    memoization_order_.push_back(text);
    memoized_embeddings_[text] = embedding;
  }

  return embedding;
}

std::vector<std::vector<float>> MiniLMEmbedderTorch::EmbedBatch(const std::vector<std::string>& texts) {
  std::vector<std::vector<float>> out{};
  out.reserve(texts.size());
  for (const auto& text : texts) {
    out.push_back(Embed(text));
  }
  return out;
}

std::size_t MiniLMEmbedderTorch::cache_size() const {
  std::lock_guard<std::mutex> lock(memoization_mutex_);
  return memoized_embeddings_.size();
}

MiniLMRuntimeInfo MiniLMEmbedderTorch::runtime_info() const {
  return runtime_info_;
}

}  // namespace waxcpp
