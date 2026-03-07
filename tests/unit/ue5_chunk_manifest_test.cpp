#include "../../server/ue5_chunk_manifest.hpp"
#include "../../server/ue5_filesystem_scanner.hpp"
#include "../temp_artifacts.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
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
         ("waxcpp_ue5_chunk_manifest_test_" + std::to_string(static_cast<long long>(now)));
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

void SeedFixtureTree(const std::filesystem::path& root) {
  WriteFile(root / "Engine/Source/Public/Math.hpp",
            "class FMathHelper {\n"
            "public:\n"
            "  static int AddNumbers(int a, int b);\n"
            "};\n");
  WriteFile(root / "Engine/Source/Private/Math.cpp",
            "int FMathHelper::AddNumbers(int a, int b) {\n"
            "  return a + b;\n"
            "}\n"
            "\n"
            "static int LocalMul(int a, int b) {\n"
            "  return a * b;\n"
            "}\n");
}

void ScenarioManifestDeterministicAndHasMetadata(const std::filesystem::path& root) {
  SeedFixtureTree(root);
  waxcpp::server::Ue5FilesystemScanner scanner{};
  waxcpp::server::Ue5ChunkManifestBuilder builder{};

  const auto entries = scanner.Scan(root);
  const auto first = builder.Build(root, entries);
  const auto second = builder.Build(root, entries);
  const auto first_manifest = waxcpp::server::Ue5ChunkManifestBuilder::SerializeManifest(first);
  const auto second_manifest = waxcpp::server::Ue5ChunkManifestBuilder::SerializeManifest(second);

  Require(!first.empty(), "chunk manifest must produce records for non-empty source tree");
  Require(first_manifest == second_manifest, "chunk manifest serialization must be deterministic");

  bool has_symbol = false;
  for (const auto& record : first) {
    Require(!record.chunk_id.empty(), "chunk_id must be populated");
    Require(!record.relative_path.empty(), "relative_path must be populated");
    Require(record.language == "cpp", "language must be cpp for UE5 source extensions");
    Require(record.line_start >= 1, "line_start must be 1-based");
    Require(record.line_end >= record.line_start, "line_end must be >= line_start");
    Require(record.token_estimate >= 1, "token_estimate must be positive");
    Require(!record.content_hash.empty(), "content hash must be populated");
    if (!record.symbol.empty()) {
      has_symbol = true;
    }
  }
  Require(has_symbol, "at least one chunk should capture best-effort symbol metadata");
}

void ScenarioManifestStableAcrossEntryOrder(const std::filesystem::path& root) {
  waxcpp::server::Ue5FilesystemScanner scanner{};
  waxcpp::server::Ue5ChunkManifestBuilder builder{};

  const auto entries = scanner.Scan(root);
  auto reversed = entries;
  std::reverse(reversed.begin(), reversed.end());

  const auto ordered_manifest =
      waxcpp::server::Ue5ChunkManifestBuilder::SerializeManifest(builder.Build(root, entries));
  const auto reversed_manifest =
      waxcpp::server::Ue5ChunkManifestBuilder::SerializeManifest(builder.Build(root, reversed));
  Require(ordered_manifest == reversed_manifest,
          "chunk manifest must be independent from scan entry input order");
}

void ScenarioManifestChangesAfterSourceMutation(const std::filesystem::path& root) {
  waxcpp::server::Ue5FilesystemScanner scanner{};
  waxcpp::server::Ue5ChunkManifestBuilder builder{};

  const auto baseline_entries = scanner.Scan(root);
  const auto baseline_manifest =
      waxcpp::server::Ue5ChunkManifestBuilder::SerializeManifest(builder.Build(root, baseline_entries));

  WriteFile(root / "Engine/Source/Private/Math.cpp",
            "int FMathHelper::AddNumbers(int a, int b) {\n"
            "  return a + b + 1;\n"
            "}\n");

  const auto changed_entries = scanner.Scan(root);
  const auto changed_manifest =
      waxcpp::server::Ue5ChunkManifestBuilder::SerializeManifest(builder.Build(root, changed_entries));

  Require(baseline_manifest != changed_manifest,
          "chunk manifest must change when source content changes");
}

void ScenarioFileManifestRoundTripAndUnchangedDetection(const std::filesystem::path& root) {
  waxcpp::server::Ue5FilesystemScanner scanner{};
  waxcpp::server::Ue5ChunkManifestBuilder builder{};

  const auto entries = scanner.Scan(root);
  std::vector<waxcpp::server::Ue5FileDigest> baseline_digests{};
  (void)builder.Build(root, entries, {}, &baseline_digests);
  Require(!baseline_digests.empty(), "baseline file digests must not be empty");

  const auto serialized = waxcpp::server::Ue5ChunkManifestBuilder::SerializeFileManifest(baseline_digests);
  const auto parsed = waxcpp::server::Ue5ChunkManifestBuilder::ParseFileManifest(serialized);
  Require(parsed.size() == baseline_digests.size(), "file manifest parse roundtrip count mismatch");
  const auto unchanged_all =
      waxcpp::server::Ue5ChunkManifestBuilder::ComputeUnchangedPaths(baseline_digests, parsed);
  Require(unchanged_all.size() == baseline_digests.size(),
          "all files must be unchanged for identical file manifests");

  WriteFile(root / "Engine/Source/Public/Math.hpp",
            "class FMathHelper {\n"
            "public:\n"
            "  static int AddNumbers(int a, int b);\n"
            "  static int SubNumbers(int a, int b);\n"
            "};\n");
  const auto changed_entries = scanner.Scan(root);
  std::vector<waxcpp::server::Ue5FileDigest> changed_digests{};
  (void)builder.Build(root, changed_entries, {}, &changed_digests);
  const auto unchanged_after_change =
      waxcpp::server::Ue5ChunkManifestBuilder::ComputeUnchangedPaths(baseline_digests, changed_digests);
  Require(unchanged_after_change.size() + 1 == baseline_digests.size(),
          "exactly one file should be detected as changed after single-file mutation");
}

void ScenarioBuildSkipsProvidedPaths(const std::filesystem::path& root) {
  waxcpp::server::Ue5FilesystemScanner scanner{};
  waxcpp::server::Ue5ChunkManifestBuilder builder{};

  const auto entries = scanner.Scan(root);
  Require(entries.size() >= 2, "fixture must contain at least two files for skip-path scenario");

  const auto full = builder.Build(root, entries);
  Require(!full.empty(), "full build must produce chunk records");

  std::unordered_set<std::string> skip_paths{};
  skip_paths.insert(entries.front().relative_path);

  std::size_t callback_count = 0;
  const auto skipped = builder.Build(
      root,
      entries,
      [&](const waxcpp::server::Ue5ChunkRecord&, std::string_view) { ++callback_count; },
      nullptr,
      &skip_paths);

  Require(callback_count == skipped.size(), "callback count must match produced records");
  Require(skipped.size() < full.size(), "skip-path build must produce fewer records than full build");
  for (const auto& record : skipped) {
    Require(!skip_paths.contains(record.relative_path), "skip-path record must not appear in result set");
  }
}

}  // namespace

int main() {
  const auto root = UniqueRepoRoot();
  try {
    ScenarioManifestDeterministicAndHasMetadata(root);
    ScenarioManifestStableAcrossEntryOrder(root);
    ScenarioManifestChangesAfterSourceMutation(root);
    ScenarioFileManifestRoundTripAndUnchangedDetection(root);
    ScenarioBuildSkipsProvidedPaths(root);
    waxcpp::tests::CleanupStoreArtifacts(root);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_ue5_chunk_manifest_test_");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::CleanupStoreArtifacts(root);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_ue5_chunk_manifest_test_");
    std::cerr << "ue5_chunk_manifest_test failed: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
