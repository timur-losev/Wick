#include "waxcpp/wax_store.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

enum class FixtureMode {
  kPass,
  kOpenFail,
  kVerifyFail,
};

std::string_view ModeToString(FixtureMode mode) {
  switch (mode) {
    case FixtureMode::kPass:
      return "pass";
    case FixtureMode::kOpenFail:
      return "open_fail";
    case FixtureMode::kVerifyFail:
      return "verify_fail";
  }
  return "unknown";
}

struct FixtureExpectation {
  FixtureMode mode = FixtureMode::kPass;
  bool verify_deep = true;
  std::optional<std::uint64_t> frame_count;
  std::optional<std::uint64_t> generation;
  std::optional<std::string> error_contains;

  std::optional<std::uint64_t> wal_write_pos;
  std::optional<std::uint64_t> wal_checkpoint_pos;
  std::optional<std::uint64_t> wal_pending_bytes;
  std::optional<std::uint64_t> wal_last_seq;
  std::optional<std::uint64_t> wal_committed_seq;
  std::optional<std::uint64_t> wal_pending_embedding_mutations;
  std::optional<std::uint64_t> wal_pending_delete_mutations;
  std::optional<std::uint64_t> wal_pending_supersede_mutations;

  std::unordered_map<std::uint64_t, std::uint64_t> frame_payload_len;
  std::unordered_map<std::uint64_t, std::uint64_t> frame_status;
  std::unordered_map<std::uint64_t, std::string> frame_payload_utf8;
};

std::string Trim(std::string value) {
  const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

bool ParseBool(std::string value, const std::string& key) {
  value = ToLower(Trim(std::move(value)));
  if (value == "true" || value == "1" || value == "yes") {
    return true;
  }
  if (value == "false" || value == "0" || value == "no") {
    return false;
  }
  throw std::runtime_error("invalid boolean value for key '" + key + "'");
}

FixtureMode ParseMode(std::string value) {
  value = ToLower(Trim(std::move(value)));
  if (value == "pass") {
    return FixtureMode::kPass;
  }
  if (value == "open_fail") {
    return FixtureMode::kOpenFail;
  }
  if (value == "verify_fail") {
    return FixtureMode::kVerifyFail;
  }
  throw std::runtime_error("invalid mode value: " + value);
}

std::uint64_t ParseUInt64(const std::string& value, const std::string& key) {
  std::size_t parsed = 0;
  const auto out = std::stoull(value, &parsed, 10);
  if (parsed != value.size()) {
    throw std::runtime_error("invalid uint64 value for key '" + key + "'");
  }
  return static_cast<std::uint64_t>(out);
}

std::optional<std::uint64_t> ParseFrameIdKey(std::string_view key, std::string_view prefix) {
  if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  const auto suffix = key.substr(prefix.size());
  if (suffix.empty()) {
    throw std::runtime_error("missing frame id in sidecar key: " + std::string(key));
  }
  std::size_t parsed = 0;
  const auto id = std::stoull(std::string(suffix), &parsed, 10);
  if (parsed != suffix.size()) {
    throw std::runtime_error("invalid frame id in sidecar key: " + std::string(key));
  }
  return static_cast<std::uint64_t>(id);
}

FixtureExpectation LoadExpectation(const std::filesystem::path& mv2s_path) {
  FixtureExpectation expected{};
  const auto expected_path = std::filesystem::path(mv2s_path.string() + ".expected");
  if (!std::filesystem::exists(expected_path)) {
    return expected;
  }

  std::ifstream in(expected_path);
  if (!in) {
    throw std::runtime_error("failed to open expected sidecar: " + expected_path.string());
  }

  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    const auto pos = trimmed.find('=');
    if (pos == std::string::npos) {
      throw std::runtime_error("invalid expected sidecar line " + std::to_string(line_no));
    }
    const auto key = Trim(trimmed.substr(0, pos));
    const auto value = Trim(trimmed.substr(pos + 1));
    if (key == "mode") {
      expected.mode = ParseMode(value);
    } else if (key == "verify_deep") {
      expected.verify_deep = ParseBool(value, key);
    } else if (key == "frame_count") {
      expected.frame_count = ParseUInt64(value, key);
    } else if (key == "generation") {
      expected.generation = ParseUInt64(value, key);
    } else if (key == "error_contains") {
      expected.error_contains = value;
    } else if (key == "wal_write_pos") {
      expected.wal_write_pos = ParseUInt64(value, key);
    } else if (key == "wal_checkpoint_pos") {
      expected.wal_checkpoint_pos = ParseUInt64(value, key);
    } else if (key == "wal_pending_bytes") {
      expected.wal_pending_bytes = ParseUInt64(value, key);
    } else if (key == "wal_last_seq") {
      expected.wal_last_seq = ParseUInt64(value, key);
    } else if (key == "wal_committed_seq") {
      expected.wal_committed_seq = ParseUInt64(value, key);
    } else if (key == "wal_pending_embedding_mutations") {
      expected.wal_pending_embedding_mutations = ParseUInt64(value, key);
    } else if (key == "wal_pending_delete_mutations") {
      expected.wal_pending_delete_mutations = ParseUInt64(value, key);
    } else if (key == "wal_pending_supersede_mutations") {
      expected.wal_pending_supersede_mutations = ParseUInt64(value, key);
    } else {
      const auto frame_payload_len_id = ParseFrameIdKey(key, "frame_payload_len.");
      if (frame_payload_len_id.has_value()) {
        expected.frame_payload_len[*frame_payload_len_id] = ParseUInt64(value, key);
        continue;
      }
      const auto frame_status_id = ParseFrameIdKey(key, "frame_status.");
      if (frame_status_id.has_value()) {
        expected.frame_status[*frame_status_id] = ParseUInt64(value, key);
        continue;
      }
      const auto frame_payload_utf8_id = ParseFrameIdKey(key, "frame_payload_utf8.");
      if (frame_payload_utf8_id.has_value()) {
        expected.frame_payload_utf8[*frame_payload_utf8_id] = value;
        continue;
      }
      throw std::runtime_error("unknown key in expected sidecar: " + key);
    }
  }

  if (expected.mode != FixtureMode::kPass &&
      (expected.frame_count.has_value() || expected.generation.has_value() ||
       expected.wal_write_pos.has_value() || expected.wal_checkpoint_pos.has_value() ||
       expected.wal_pending_bytes.has_value() || expected.wal_last_seq.has_value() ||
       expected.wal_committed_seq.has_value() || expected.wal_pending_embedding_mutations.has_value() ||
       expected.wal_pending_delete_mutations.has_value() ||
       expected.wal_pending_supersede_mutations.has_value() || !expected.frame_payload_len.empty() ||
       !expected.frame_status.empty() || !expected.frame_payload_utf8.empty())) {
    throw std::runtime_error("state expectations are only valid for mode=pass");
  }
  return expected;
}

bool HasMv2sExtension(const std::filesystem::path& path) {
  auto ext = path.extension().string();
  if (ext.size() != 5) {
    return false;
  }
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return ext == ".mv2s";
}

bool IsSyntheticFixture(const std::filesystem::path& path) {
  const auto normalized = path.generic_string();
  return normalized.find("/synthetic/") != std::string::npos;
}

std::vector<std::filesystem::path> DiscoverFixtures(const std::filesystem::path& fixtures_root) {
  std::vector<std::filesystem::path> fixtures;
  if (!std::filesystem::exists(fixtures_root)) {
    return fixtures;
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(fixtures_root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (!HasMv2sExtension(entry.path())) {
      continue;
    }
    fixtures.push_back(entry.path());
  }

  std::sort(fixtures.begin(), fixtures.end());
  return fixtures;
}

std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string out{};
  out.reserve(bytes.size());
  for (const auto value : bytes) {
    out.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
  }
  return out;
}

bool MetaEqual(const waxcpp::WaxFrameMeta& lhs, const waxcpp::WaxFrameMeta& rhs) {
  return lhs.id == rhs.id &&
         lhs.payload_offset == rhs.payload_offset &&
         lhs.payload_length == rhs.payload_length &&
         lhs.canonical_encoding == rhs.canonical_encoding &&
         lhs.status == rhs.status &&
         lhs.supersedes == rhs.supersedes &&
         lhs.superseded_by == rhs.superseded_by;
}

void AssertExpected(const std::filesystem::path& fixture_path,
                    const waxcpp::WaxStats& stats,
                    const waxcpp::WaxWALStats& wal_stats,
                    const FixtureExpectation& expected) {
  if (expected.frame_count.has_value() && stats.frame_count != *expected.frame_count) {
    throw std::runtime_error("frame_count mismatch for " + fixture_path.string());
  }
  if (expected.generation.has_value() && stats.generation != *expected.generation) {
    throw std::runtime_error("generation mismatch for " + fixture_path.string());
  }
  if (expected.wal_write_pos.has_value() && wal_stats.write_pos != *expected.wal_write_pos) {
    throw std::runtime_error("wal_write_pos mismatch for " + fixture_path.string());
  }
  if (expected.wal_checkpoint_pos.has_value() && wal_stats.checkpoint_pos != *expected.wal_checkpoint_pos) {
    throw std::runtime_error("wal_checkpoint_pos mismatch for " + fixture_path.string());
  }
  if (expected.wal_pending_bytes.has_value() && wal_stats.pending_bytes != *expected.wal_pending_bytes) {
    throw std::runtime_error("wal_pending_bytes mismatch for " + fixture_path.string());
  }
  if (expected.wal_last_seq.has_value() && wal_stats.last_seq != *expected.wal_last_seq) {
    throw std::runtime_error("wal_last_seq mismatch for " + fixture_path.string());
  }
  if (expected.wal_committed_seq.has_value() && wal_stats.committed_seq != *expected.wal_committed_seq) {
    throw std::runtime_error("wal_committed_seq mismatch for " + fixture_path.string());
  }
  if (expected.wal_pending_embedding_mutations.has_value() &&
      wal_stats.pending_embedding_mutations != *expected.wal_pending_embedding_mutations) {
    throw std::runtime_error("wal_pending_embedding_mutations mismatch for " + fixture_path.string());
  }
  if (expected.wal_pending_delete_mutations.has_value() &&
      wal_stats.pending_delete_mutations != *expected.wal_pending_delete_mutations) {
    throw std::runtime_error("wal_pending_delete_mutations mismatch for " + fixture_path.string());
  }
  if (expected.wal_pending_supersede_mutations.has_value() &&
      wal_stats.pending_supersede_mutations != *expected.wal_pending_supersede_mutations) {
    throw std::runtime_error("wal_pending_supersede_mutations mismatch for " + fixture_path.string());
  }
}

void AssertPassInvariants(const std::filesystem::path& fixture_path,
                          waxcpp::WaxStore& store,
                          const waxcpp::WaxStats& stats,
                          const waxcpp::WaxWALStats& wal_stats,
                          const FixtureExpectation& expected) {
  if (wal_stats.wal_size > 0) {
    if (wal_stats.write_pos >= wal_stats.wal_size) {
      throw std::runtime_error("write_pos out of wal_size range for " + fixture_path.string());
    }
    if (wal_stats.checkpoint_pos >= wal_stats.wal_size) {
      throw std::runtime_error("checkpoint_pos out of wal_size range for " + fixture_path.string());
    }
    if (wal_stats.pending_bytes > wal_stats.wal_size) {
      throw std::runtime_error("pending_bytes out of wal_size range for " + fixture_path.string());
    }
  }
  if (wal_stats.pending_bytes == 0 && wal_stats.wal_size > 0 &&
      wal_stats.write_pos != wal_stats.checkpoint_pos) {
    throw std::runtime_error("clean wal must have write_pos == checkpoint_pos for " + fixture_path.string());
  }
  if (wal_stats.committed_seq > wal_stats.last_seq) {
    throw std::runtime_error("committed_seq must be <= last_seq for " + fixture_path.string());
  }

  const auto metas = store.FrameMetas();
  if (metas.size() != static_cast<std::size_t>(stats.frame_count)) {
    throw std::runtime_error("FrameMetas count mismatch for " + fixture_path.string());
  }

  std::vector<std::uint64_t> frame_ids{};
  frame_ids.reserve(metas.size());
  std::unordered_set<std::uint64_t> seen_ids{};
  seen_ids.reserve(metas.size());

  bool first = true;
  std::uint64_t prev_id = 0;
  for (const auto& meta : metas) {
    if (!seen_ids.insert(meta.id).second) {
      throw std::runtime_error("duplicate frame id in FrameMetas for " + fixture_path.string());
    }
    if (!first && meta.id <= prev_id) {
      throw std::runtime_error("FrameMetas must be strictly ordered by id for " + fixture_path.string());
    }
    first = false;
    prev_id = meta.id;
    frame_ids.push_back(meta.id);

    const auto maybe_meta = store.FrameMeta(meta.id);
    if (!maybe_meta.has_value()) {
      throw std::runtime_error("FrameMeta(id) missing for id=" + std::to_string(meta.id));
    }
    if (!MetaEqual(meta, *maybe_meta)) {
      throw std::runtime_error("FrameMeta(id) mismatch for id=" + std::to_string(meta.id));
    }

    const auto content = store.FrameContent(meta.id);
    if (content.size() != meta.payload_length) {
      throw std::runtime_error("FrameContent length mismatch for id=" + std::to_string(meta.id));
    }

    if (const auto it = expected.frame_payload_len.find(meta.id); it != expected.frame_payload_len.end()) {
      if (meta.payload_length != it->second) {
        throw std::runtime_error("frame_payload_len mismatch for id=" + std::to_string(meta.id));
      }
    }
    if (const auto it = expected.frame_status.find(meta.id); it != expected.frame_status.end()) {
      if (meta.status != static_cast<std::uint8_t>(it->second)) {
        throw std::runtime_error("frame_status mismatch for id=" + std::to_string(meta.id));
      }
    }
    if (const auto it = expected.frame_payload_utf8.find(meta.id); it != expected.frame_payload_utf8.end()) {
      if (BytesToString(content) != it->second) {
        throw std::runtime_error("frame_payload_utf8 mismatch for id=" + std::to_string(meta.id));
      }
    }
  }

  for (const auto& [frame_id, _] : expected.frame_payload_len) {
    if (seen_ids.find(frame_id) == seen_ids.end()) {
      throw std::runtime_error("frame_payload_len expectation references missing frame id=" + std::to_string(frame_id));
    }
  }
  for (const auto& [frame_id, _] : expected.frame_status) {
    if (seen_ids.find(frame_id) == seen_ids.end()) {
      throw std::runtime_error("frame_status expectation references missing frame id=" + std::to_string(frame_id));
    }
  }
  for (const auto& [frame_id, _] : expected.frame_payload_utf8) {
    if (seen_ids.find(frame_id) == seen_ids.end()) {
      throw std::runtime_error("frame_payload_utf8 expectation references missing frame id=" + std::to_string(frame_id));
    }
  }

  const auto bulk_contents = store.FrameContents(frame_ids);
  if (bulk_contents.size() != frame_ids.size()) {
    throw std::runtime_error("FrameContents bulk size mismatch for " + fixture_path.string());
  }
  for (const auto frame_id : frame_ids) {
    const auto direct_content = store.FrameContent(frame_id);
    const auto it = bulk_contents.find(frame_id);
    if (it == bulk_contents.end()) {
      throw std::runtime_error("FrameContents bulk missing id=" + std::to_string(frame_id));
    }
    if (it->second != direct_content) {
      throw std::runtime_error("FrameContents bulk mismatch for id=" + std::to_string(frame_id));
    }
  }
}

void AssertErrorMatch(const std::filesystem::path& fixture_path,
                      const std::exception& ex,
                      const FixtureExpectation& expected) {
  if (!expected.error_contains.has_value()) {
    return;
  }
  const std::string message = ex.what();
  if (message.find(*expected.error_contains) == std::string::npos) {
    throw std::runtime_error("error mismatch for " + fixture_path.string());
  }
}

}  // namespace

int main() {
  const std::filesystem::path fixtures_root = WAXCPP_PARITY_FIXTURES_DIR;
  constexpr bool require_fixtures = WAXCPP_REQUIRE_PARITY_FIXTURES != 0;
  constexpr bool require_swift_fixtures = WAXCPP_REQUIRE_SWIFT_FIXTURES != 0;

  try {
    waxcpp::tests::Log("mv2s_fixture_parity_test: start");
    waxcpp::tests::LogKV("fixtures_root", fixtures_root.string());
    waxcpp::tests::LogKV("require_fixtures", require_fixtures);
    waxcpp::tests::LogKV("require_swift_fixtures", require_swift_fixtures);
    const auto fixtures = DiscoverFixtures(fixtures_root);
    waxcpp::tests::LogKV("discovered_fixtures", static_cast<std::uint64_t>(fixtures.size()));
    if (fixtures.empty()) {
#if WAXCPP_REQUIRE_PARITY_FIXTURES
      std::cerr << "mv2s_fixture_parity_test failed: no .mv2s fixtures in "
                << fixtures_root.string() << "\n";
      return EXIT_FAILURE;
#else
      std::cout << "mv2s_fixture_parity_test skipped: no .mv2s fixtures in "
                << fixtures_root.string() << "\n";
      return EXIT_SUCCESS;
#endif
    }

    std::size_t non_synthetic_count = 0;
    for (const auto& fixture : fixtures) {
      if (!IsSyntheticFixture(fixture)) {
        ++non_synthetic_count;
      }
    }
    waxcpp::tests::LogKV("non_synthetic_fixtures", static_cast<std::uint64_t>(non_synthetic_count));
#if WAXCPP_REQUIRE_SWIFT_FIXTURES
    if (non_synthetic_count == 0) {
      std::cerr << "mv2s_fixture_parity_test failed: no non-synthetic fixtures in "
                << fixtures_root.string() << "\n";
      std::cerr << "expected at least one Swift-generated fixture (e.g. fixtures/parity/swift/*.mv2s)\n";
      return EXIT_FAILURE;
    }
#endif

    for (const auto& fixture : fixtures) {
      waxcpp::tests::Log("fixture: begin");
      waxcpp::tests::LogKV("fixture_path", fixture.string());
      waxcpp::tests::LogKV("fixture_source", IsSyntheticFixture(fixture) ? std::string("synthetic")
                                                                         : std::string("non_synthetic"));
      const auto expected = LoadExpectation(fixture);
      waxcpp::tests::LogKV("fixture_mode", std::string(ModeToString(expected.mode)));
      waxcpp::tests::LogKV("fixture_verify_deep", expected.verify_deep);
      if (expected.error_contains.has_value()) {
        waxcpp::tests::LogKV("fixture_error_contains", *expected.error_contains);
      }
      if (expected.mode == FixtureMode::kOpenFail) {
        try {
          auto store = waxcpp::WaxStore::Open(fixture);
          store.Close();
        } catch (const std::exception& ex) {
          waxcpp::tests::LogKV("fixture_open_error", std::string(ex.what()));
          AssertErrorMatch(fixture, ex, expected);
          std::cout << "fixture OK (open_fail): " << fixture.string() << "\n";
          waxcpp::tests::Log("fixture: open_fail passed");
          continue;
        }
        throw std::runtime_error("expected open failure for " + fixture.string());
      }

      auto store = waxcpp::WaxStore::Open(fixture);
      if (expected.mode == FixtureMode::kVerifyFail) {
        try {
          store.Verify(expected.verify_deep);
        } catch (const std::exception& ex) {
          waxcpp::tests::LogKV("fixture_verify_error", std::string(ex.what()));
          AssertErrorMatch(fixture, ex, expected);
          store.Close();
          std::cout << "fixture OK (verify_fail): " << fixture.string() << "\n";
          waxcpp::tests::Log("fixture: verify_fail passed");
          continue;
        }
        throw std::runtime_error("expected verify failure for " + fixture.string());
      }

      store.Verify(expected.verify_deep);
      const auto stats = store.Stats();
      const auto wal_stats = store.WalStats();
      waxcpp::tests::LogKV("fixture_stats_frame_count", stats.frame_count);
      waxcpp::tests::LogKV("fixture_stats_generation", stats.generation);
      waxcpp::tests::LogKV("fixture_wal_write_pos", wal_stats.write_pos);
      waxcpp::tests::LogKV("fixture_wal_checkpoint_pos", wal_stats.checkpoint_pos);
      waxcpp::tests::LogKV("fixture_wal_pending_bytes", wal_stats.pending_bytes);
      waxcpp::tests::LogKV("fixture_wal_last_seq", wal_stats.last_seq);
      waxcpp::tests::LogKV("fixture_wal_committed_seq", wal_stats.committed_seq);
      waxcpp::tests::LogKV("fixture_wal_pending_embedding_mutations", wal_stats.pending_embedding_mutations);
      waxcpp::tests::LogKV("fixture_wal_pending_delete_mutations", wal_stats.pending_delete_mutations);
      waxcpp::tests::LogKV("fixture_wal_pending_supersede_mutations", wal_stats.pending_supersede_mutations);

      AssertExpected(fixture, stats, wal_stats, expected);
      AssertPassInvariants(fixture, store, stats, wal_stats, expected);
      store.Close();
      std::cout << "fixture OK: " << fixture.string() << "\n";
      waxcpp::tests::Log("fixture: pass mode passed");
    }

    std::cout << "mv2s_fixture_parity_test passed (" << fixtures.size() << " fixtures)\n";
    waxcpp::tests::Log("mv2s_fixture_parity_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::cerr << "mv2s_fixture_parity_test failed: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
