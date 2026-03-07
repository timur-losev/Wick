#include "../../server/ue5_filesystem_scanner.hpp"
#include "../temp_artifacts.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path UniqueRepoRoot() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_ue5_scanner_test_" + std::to_string(static_cast<long long>(now)));
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    throw std::runtime_error("failed to create parent dir for file: " + path.string());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open file for write: " + path.string());
  }
  out << content;
  if (!out) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
}

void ExpectThrows(const std::string& scenario, const std::function<void()>& fn) {
  bool threw = false;
  try {
    fn();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "expected throw: " + scenario);
}

void ScenarioDeterministicScanAndFiltering(const std::filesystem::path& root) {
  WriteFile(root / "Engine/Source/Public/Zeta.h", "class Zeta {};");
  WriteFile(root / "Engine/Source/Public/Alpha.cpp", "int alpha = 1;");
  WriteFile(root / "Engine/Source/Public/Sub/Gamma.INL", "inline void gamma() {}");
  WriteFile(root / "Engine/Source/Public/Sub/Beta.hpp", "struct Beta {};");
  WriteFile(root / "Engine/Source/Public/Sub/Ignore.txt", "not code");
  WriteFile(root / "Engine/Binaries/Win64/ShouldSkip.cpp", "int skip = 0;");
  WriteFile(root / "Engine/Intermediate/Build/ShouldSkip.h", "int skip2 = 0;");
  WriteFile(root / "Engine/DerivedDataCache/ShouldSkip.inc", "int skip3 = 0;");
  WriteFile(root / "Engine/Saved/ShouldSkip.hpp", "int skip4 = 0;");
  WriteFile(root / ".git/ShouldSkip.inl", "int skip5 = 0;");

  waxcpp::server::Ue5FilesystemScanner scanner{};
  const auto entries_first = scanner.Scan(root);
  const auto entries_second = scanner.Scan(root);

  Require(entries_first.size() == 4, "scanner must include exactly four code files after exclusions");
  Require(entries_second.size() == entries_first.size(), "repeated scan size mismatch");

  const std::vector<std::string> expected_paths = {
      "Engine/Source/Public/Alpha.cpp",
      "Engine/Source/Public/Sub/Beta.hpp",
      "Engine/Source/Public/Sub/Gamma.INL",
      "Engine/Source/Public/Zeta.h",
  };
  for (std::size_t i = 0; i < expected_paths.size(); ++i) {
    Require(entries_first[i].relative_path == expected_paths[i], "scan order mismatch at index " + std::to_string(i));
    Require(entries_second[i].relative_path == expected_paths[i],
            "repeated scan order mismatch at index " + std::to_string(i));
  }

  const auto manifest_first = waxcpp::server::Ue5FilesystemScanner::SerializeManifest(entries_first);
  const auto manifest_second = waxcpp::server::Ue5FilesystemScanner::SerializeManifest(entries_second);
  Require(manifest_first == manifest_second, "serialized manifest must be deterministic across repeated scans");
}

void ScenarioMissingRootThrows(const std::filesystem::path& root) {
  waxcpp::server::Ue5FilesystemScanner scanner{};
  ExpectThrows("scan missing root", [&]() {
    (void)scanner.Scan(root / "missing");
  });
}

void ScenarioCancelStopsScanEarly(const std::filesystem::path& root) {
  const auto cancel_root = root / "CancelOnly";
  WriteFile(cancel_root / "A.cpp", "int a = 1;");
  WriteFile(cancel_root / "B.cpp", "int b = 2;");
  WriteFile(cancel_root / "C.cpp", "int c = 3;");

  waxcpp::server::Ue5FilesystemScanner scanner{};
  const auto full_entries = scanner.Scan(cancel_root);
  Require(full_entries.size() == 3, "cancel scenario baseline must have three files");

  const auto cancelled_entries = scanner.Scan(cancel_root, []() { return true; });
  Require(cancelled_entries.empty(), "cancelled scan should return no entries when cancelled immediately");
}

}  // namespace

int main() {
  const auto root = UniqueRepoRoot();
  try {
    ScenarioDeterministicScanAndFiltering(root);
    ScenarioMissingRootThrows(root);
    ScenarioCancelStopsScanEarly(root);
    waxcpp::tests::CleanupStoreArtifacts(root);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_ue5_scanner_test_");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::CleanupStoreArtifacts(root);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_ue5_scanner_test_");
    std::cerr << "ue5_filesystem_scanner_test failed: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
