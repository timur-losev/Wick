#include "ue5_filesystem_scanner.hpp"
#include "server_utils.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace waxcpp::server {

Ue5FilesystemScanner::Ue5FilesystemScanner(Ue5ScannerConfig config) : config_(std::move(config)) {
  for (auto& extension : config_.include_extensions) {
    extension = ToAsciiLower(extension);
  }
  for (auto& dir_name : config_.exclude_directory_names) {
    dir_name = ToAsciiLower(dir_name);
  }
  include_extensions_set_.insert(config_.include_extensions.begin(), config_.include_extensions.end());
  exclude_directory_names_set_.insert(config_.exclude_directory_names.begin(), config_.exclude_directory_names.end());
}

std::vector<Ue5ScanEntry> Ue5FilesystemScanner::Scan(
    const std::filesystem::path& repo_root,
    const CancelRequestedFn& cancel_requested) const {
  std::error_code ec;
  if (!std::filesystem::exists(repo_root, ec) || ec) {
    throw std::runtime_error("scan repo_root does not exist: " + repo_root.string());
  }
  if (!std::filesystem::is_directory(repo_root, ec) || ec) {
    throw std::runtime_error("scan repo_root must be a directory: " + repo_root.string());
  }

  std::vector<Ue5ScanEntry> entries{};
  std::filesystem::recursive_directory_iterator it(
      repo_root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end{};
  for (; !ec && it != end; it.increment(ec)) {
    if (cancel_requested && cancel_requested()) {
      break;
    }
    const auto& path = it->path();
    const bool is_directory = it->is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (is_directory) {
      if (ShouldExcludeDirectory(path)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    const bool is_file = it->is_regular_file(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!is_file) {
      continue;
    }

    if (!ShouldIncludeExtension(path.extension().string())) {
      continue;
    }

    const auto relative = std::filesystem::relative(path, repo_root, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const auto relative_generic = relative.generic_string();
    if (relative_generic.empty() || relative_generic.starts_with("../")) {
      continue;
    }

    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec) {
      ec.clear();
      continue;
    }

    entries.push_back(Ue5ScanEntry{
        .relative_path = relative_generic,
        .size_bytes = static_cast<std::uint64_t>(file_size),
    });
  }

  // Precompute lowercase sort keys to avoid O(n log n) redundant lowercasing.
  struct KeyedEntry {
    std::string lower_path;
    Ue5ScanEntry entry;
  };
  std::vector<KeyedEntry> keyed{};
  keyed.reserve(entries.size());
  for (auto& entry : entries) {
    keyed.push_back({ToAsciiLower(entry.relative_path), std::move(entry)});
  }
  std::sort(keyed.begin(), keyed.end(), [](const KeyedEntry& a, const KeyedEntry& b) {
    if (a.lower_path != b.lower_path) {
      return a.lower_path < b.lower_path;
    }
    return a.entry.relative_path < b.entry.relative_path;
  });
  entries.clear();
  entries.reserve(keyed.size());
  for (auto& k : keyed) {
    entries.push_back(std::move(k.entry));
  }
  return entries;
}

std::string Ue5FilesystemScanner::SerializeManifest(const std::vector<Ue5ScanEntry>& entries) {
  std::ostringstream out;
  for (const auto& entry : entries) {
    out << entry.relative_path << "\t" << entry.size_bytes << "\n";
  }
  return out.str();
}

bool Ue5FilesystemScanner::ShouldIncludeExtension(const std::string& extension) const {
  if (extension.empty()) {
    return false;
  }
  return include_extensions_set_.contains(ToAsciiLower(extension));
}

bool Ue5FilesystemScanner::ShouldExcludeDirectory(const std::filesystem::path& dir_path) const {
  return exclude_directory_names_set_.contains(ToAsciiLower(dir_path.filename().string()));
}

}  // namespace waxcpp::server
