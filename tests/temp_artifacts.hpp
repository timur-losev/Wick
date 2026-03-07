#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace waxcpp::tests {

inline void CleanupTempArtifactsByPrefix(std::string_view prefix) {
  std::error_code ec;
  const auto temp_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return;
  }
  for (std::filesystem::directory_iterator it(temp_dir, ec);
       !ec && it != std::filesystem::directory_iterator();
       it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (!name.starts_with(prefix)) {
      continue;
    }
    std::filesystem::remove_all(it->path(), ec);
    ec.clear();
  }
}

inline void CleanupStoreArtifacts(const std::filesystem::path& store_path) {
  std::error_code ec;
  std::filesystem::remove_all(store_path, ec);
  ec.clear();
  std::filesystem::remove(store_path.string() + ".writer.lease", ec);
  ec.clear();
  std::filesystem::remove(store_path.string() + ".writer.lock", ec);
  ec.clear();
  std::filesystem::remove(store_path.string() + ".cross-process-lease.ready", ec);
}

}  // namespace waxcpp::tests
