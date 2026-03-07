#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace waxcpp::server {

struct Ue5ScanEntry {
  std::string relative_path{};
  std::uint64_t size_bytes = 0;
};

struct Ue5ScannerConfig {
  std::vector<std::string> include_extensions{
      ".h",
      ".hpp",
      ".cpp",
      ".inl",
      ".inc",
  };
  std::vector<std::string> exclude_directory_names{
      ".git",
      "binaries",
      "intermediate",
      "deriveddatacache",
      "saved",
  };
};

class Ue5FilesystemScanner {
 public:
  explicit Ue5FilesystemScanner(Ue5ScannerConfig config = {});

  using CancelRequestedFn = std::function<bool()>;

  [[nodiscard]] std::vector<Ue5ScanEntry> Scan(const std::filesystem::path& repo_root,
                                               const CancelRequestedFn& cancel_requested = {}) const;
  [[nodiscard]] static std::string SerializeManifest(const std::vector<Ue5ScanEntry>& entries);

 private:
  [[nodiscard]] bool ShouldIncludeExtension(const std::string& extension) const;
  [[nodiscard]] bool ShouldExcludeDirectory(const std::filesystem::path& dir_path) const;

  Ue5ScannerConfig config_{};
  std::unordered_set<std::string> include_extensions_set_{};
  std::unordered_set<std::string> exclude_directory_names_set_{};
};

}  // namespace waxcpp::server
