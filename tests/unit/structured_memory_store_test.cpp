#include "waxcpp/structured_memory.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ScenarioUpsertGetAndVersioning() {
  waxcpp::tests::Log("scenario: upsert/get/versioning");
  waxcpp::StructuredMemoryStore store;

  const auto id0 = store.Upsert("user:1", "name", "Alice", {{"src", "ingest"}});
  const auto first = store.Get("user:1", "name");
  Require(first.has_value(), "entry should exist after upsert");
  Require(first->id == id0, "id mismatch on first insert");
  Require(first->version == 1, "version should start at 1");
  Require(first->value == "Alice", "value mismatch after insert");
  Require(first->metadata.at("src") == "ingest", "metadata mismatch");

  const auto id1 = store.Upsert("user:1", "name", "Alice B", {{"src", "edit"}});
  Require(id1 == id0, "upsert should preserve id for same (entity,attribute)");
  const auto second = store.Get("user:1", "name");
  Require(second.has_value(), "entry should still exist after update");
  Require(second->version == 2, "version should increment on update");
  Require(second->value == "Alice B", "value mismatch after update");
  Require(second->metadata.at("src") == "edit", "metadata should be replaced on update");
}

void ScenarioRemove() {
  waxcpp::tests::Log("scenario: remove");
  waxcpp::StructuredMemoryStore store;
  (void)store.Upsert("user:1", "city", "Paris");
  Require(store.Remove("user:1", "city"), "remove should return true for existing entry");
  Require(!store.Remove("user:1", "city"), "remove should return false for missing entry");
  Require(!store.Get("user:1", "city").has_value(), "removed entry should not be found");
}

void ScenarioQueryDeterminismAndLimit() {
  waxcpp::tests::Log("scenario: query determinism and limit");
  waxcpp::StructuredMemoryStore store;
  (void)store.Upsert("user:2", "name", "Bob");
  (void)store.Upsert("user:1", "city", "Paris");
  (void)store.Upsert("user:1", "name", "Alice");
  (void)store.Upsert("order:1", "state", "paid");

  const auto user_entries = store.QueryByEntityPrefix("user:", -1);
  Require(user_entries.size() == 3, "prefix query count mismatch");
  Require(user_entries[0].entity == "user:1" && user_entries[0].attribute == "city",
          "deterministic ordering mismatch [0]");
  Require(user_entries[1].entity == "user:1" && user_entries[1].attribute == "name",
          "deterministic ordering mismatch [1]");
  Require(user_entries[2].entity == "user:2" && user_entries[2].attribute == "name",
          "deterministic ordering mismatch [2]");

  const auto limited = store.QueryByEntityPrefix("user:", 2);
  Require(limited.size() == 2, "limit should clamp query results");

  const auto none = store.QueryByEntityPrefix("missing:", -1);
  Require(none.empty(), "missing prefix should return empty result");
}

void ScenarioValidation() {
  waxcpp::tests::Log("scenario: validation");
  waxcpp::StructuredMemoryStore store;
  bool entity_throw = false;
  try {
    (void)store.Upsert("", "name", "x");
  } catch (const std::exception&) {
    entity_throw = true;
  }
  Require(entity_throw, "empty entity should throw");

  bool attribute_throw = false;
  try {
    (void)store.Upsert("user:1", "", "x");
  } catch (const std::exception&) {
    attribute_throw = true;
  }
  Require(attribute_throw, "empty attribute should throw");

  bool remove_entity_throw = false;
  try {
    (void)store.Remove("", "name");
  } catch (const std::exception&) {
    remove_entity_throw = true;
  }
  Require(remove_entity_throw, "remove with empty entity should throw");

  bool remove_attribute_throw = false;
  try {
    (void)store.Remove("user:1", "");
  } catch (const std::exception&) {
    remove_attribute_throw = true;
  }
  Require(remove_attribute_throw, "remove with empty attribute should throw");
}

void ScenarioStagedMutationVisibilityAndRollback() {
  waxcpp::tests::Log("scenario: staged mutation visibility and rollback");
  waxcpp::StructuredMemoryStore store;

  const auto staged_id = store.StageUpsert("user:1", "city", "Rome", {{"src", "stage"}});
  Require(store.PendingMutationCount() == 1, "stage upsert must increase pending mutation count");
  Require(!store.Get("user:1", "city").has_value(), "staged upsert must stay invisible before commit");
  Require(store.QueryByEntityPrefix("user:", -1).empty(),
          "staged upsert must not appear in committed query view");

  store.RollbackStaged();
  Require(store.PendingMutationCount() == 0, "rollback must clear pending mutation count");
  Require(!store.Get("user:1", "city").has_value(), "rollback must discard staged upsert");

  const auto committed_id = store.StageUpsert("user:1", "city", "Rome", {});
  Require(committed_id == staged_id, "id allocation should be deterministic across rollback retries");
  store.CommitStaged();
  Require(store.PendingMutationCount() == 0, "commit must clear pending mutation count");
  const auto committed = store.Get("user:1", "city");
  Require(committed.has_value(), "commit must publish staged upsert");
  Require(committed->id == committed_id, "committed staged id mismatch");
  Require(committed->value == "Rome", "committed staged value mismatch");

  (void)store.StageUpsert("user:1", "city", "Milan", {});
  Require(store.Get("user:1", "city")->value == "Rome",
          "staged update must stay invisible before commit");
  store.RollbackStaged();
  Require(store.Get("user:1", "city")->value == "Rome",
          "rollback must preserve last committed value");

  const auto removed_id = store.StageRemove("user:1", "city");
  Require(removed_id.has_value() && *removed_id == committed_id, "staged remove should expose removed id");
  Require(store.Get("user:1", "city").has_value(), "staged remove must stay invisible before commit");
  store.CommitStaged();
  Require(!store.Get("user:1", "city").has_value(), "commit should apply staged remove");

  const auto missing_removed_id = store.StageRemove("user:1", "city");
  Require(!missing_removed_id.has_value(), "StageRemove should report missing key for absent entry");
  Require(store.PendingMutationCount() == 0,
          "StageRemove on missing key must not create synthetic staged mutation");
}

void ScenarioSeededStagedMutationModelParity() {
  waxcpp::tests::Log("scenario: seeded staged mutation model parity");
  waxcpp::StructuredMemoryStore store;

  struct ModelEntry {
    std::uint64_t id = 0;
    std::string entity{};
    std::string attribute{};
    std::string value{};
    waxcpp::Metadata metadata{};
    std::uint64_t version = 0;
  };

  auto key_for = [](const std::string& entity, const std::string& attribute) {
    return entity + '\x1F' + attribute;
  };

  auto entry_less = [](const waxcpp::StructuredMemoryEntry& lhs, const waxcpp::StructuredMemoryEntry& rhs) {
    if (lhs.entity != rhs.entity) {
      return lhs.entity < rhs.entity;
    }
    if (lhs.attribute != rhs.attribute) {
      return lhs.attribute < rhs.attribute;
    }
    return lhs.id < rhs.id;
  };

  std::unordered_map<std::string, ModelEntry> committed{};
  std::unordered_map<std::string, ModelEntry> staged{};
  std::uint64_t next_id = 0;
  std::uint64_t staged_next_id = 0;
  std::size_t pending_mutations = 0;

  auto ensure_staging = [&]() {
    if (pending_mutations != 0) {
      return;
    }
    staged = committed;
    staged_next_id = next_id;
  };

  auto model_stage_upsert = [&](const std::string& entity,
                                const std::string& attribute,
                                const std::string& value,
                                const waxcpp::Metadata& metadata) {
    ensure_staging();
    const auto key = key_for(entity, attribute);
    auto it = staged.find(key);
    std::uint64_t id = 0;
    if (it == staged.end()) {
      ModelEntry entry{};
      entry.id = staged_next_id++;
      entry.entity = entity;
      entry.attribute = attribute;
      entry.value = value;
      entry.metadata = metadata;
      entry.version = 1;
      id = entry.id;
      staged.emplace(key, std::move(entry));
    } else {
      it->second.value = value;
      it->second.metadata = metadata;
      it->second.version += 1;
      id = it->second.id;
    }
    ++pending_mutations;
    return id;
  };

  auto model_stage_remove = [&](const std::string& entity, const std::string& attribute) -> std::optional<std::uint64_t> {
    const auto key = key_for(entity, attribute);
    if (pending_mutations == 0) {
      const auto committed_it = committed.find(key);
      if (committed_it == committed.end()) {
        return std::nullopt;
      }
      staged = committed;
      staged_next_id = next_id;
    }
    auto it = staged.find(key);
    if (it == staged.end()) {
      return std::nullopt;
    }
    const std::uint64_t removed_id = it->second.id;
    staged.erase(it);
    ++pending_mutations;
    return removed_id;
  };

  auto model_commit = [&]() {
    if (pending_mutations == 0) {
      return;
    }
    committed = staged;
    staged.clear();
    next_id = staged_next_id;
    pending_mutations = 0;
  };

  auto model_rollback = [&]() {
    staged.clear();
    staged_next_id = next_id;
    pending_mutations = 0;
  };

  auto model_all = [&]() {
    std::vector<waxcpp::StructuredMemoryEntry> out{};
    out.reserve(committed.size());
    for (const auto& [_, entry] : committed) {
      out.push_back(waxcpp::StructuredMemoryEntry{
          .id = entry.id,
          .entity = entry.entity,
          .attribute = entry.attribute,
          .value = entry.value,
          .metadata = entry.metadata,
          .version = entry.version,
      });
    }
    std::sort(out.begin(), out.end(), entry_less);
    return out;
  };

  auto model_query = [&](const std::string& prefix, int limit) {
    if (limit == 0) {
      return std::vector<waxcpp::StructuredMemoryEntry>{};
    }
    auto out = model_all();
    out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& entry) {
                if (prefix.empty()) {
                  return false;
                }
                return entry.entity.rfind(prefix, 0) != 0;
              }),
              out.end());
    if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
      out.resize(static_cast<std::size_t>(limit));
    }
    return out;
  };

  auto require_entries_equal = [&](const std::vector<waxcpp::StructuredMemoryEntry>& expected,
                                   const std::vector<waxcpp::StructuredMemoryEntry>& actual,
                                   const std::string& where) {
    Require(expected.size() == actual.size(), where + ": size mismatch");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto& lhs = expected[i];
      const auto& rhs = actual[i];
      Require(lhs.id == rhs.id, where + ": id mismatch");
      Require(lhs.entity == rhs.entity, where + ": entity mismatch");
      Require(lhs.attribute == rhs.attribute, where + ": attribute mismatch");
      Require(lhs.value == rhs.value, where + ": value mismatch");
      Require(lhs.version == rhs.version, where + ": version mismatch");
      Require(lhs.metadata == rhs.metadata, where + ": metadata mismatch");
    }
  };

  std::mt19937 rng(0x5EED1234U);
  std::uniform_int_distribution<int> op_dist(0, 5);
  std::uniform_int_distribution<int> idx_dist(0, 4);
  std::uniform_int_distribution<int> limit_dist(-1, 4);
  std::uniform_int_distribution<int> value_dist(0, 9999);
  std::uniform_int_distribution<int> prefix_dist(0, 2);

  const std::vector<std::string> entities = {
      "user:1",
      "user:2",
      "order:1",
      "project:1",
      "topic:1",
  };
  const std::vector<std::string> attributes = {
      "name",
      "city",
      "status",
      "note",
      "owner",
  };

  constexpr std::size_t kIterations = 512;
  for (std::size_t i = 0; i < kIterations; ++i) {
    const std::string entity = entities[static_cast<std::size_t>(idx_dist(rng))];
    const std::string attribute = attributes[static_cast<std::size_t>(idx_dist(rng))];
    const std::string value = "v" + std::to_string(value_dist(rng));
    waxcpp::Metadata metadata = {
        {"iter", std::to_string(i)},
        {"tag", std::to_string(value_dist(rng) % 7)},
    };

    switch (op_dist(rng)) {
      case 0: {
        const auto expected_id = model_stage_upsert(entity, attribute, value, metadata);
        model_commit();
        const auto actual_id = store.Upsert(entity, attribute, value, metadata);
        Require(actual_id == expected_id, "direct upsert id mismatch with model");
        break;
      }
      case 1: {
        const auto expected_removed_id = model_stage_remove(entity, attribute);
        model_commit();
        const bool removed = store.Remove(entity, attribute);
        Require(removed == expected_removed_id.has_value(), "direct remove result mismatch with model");
        break;
      }
      case 2: {
        const auto expected_id = model_stage_upsert(entity, attribute, value, metadata);
        const auto actual_id = store.StageUpsert(entity, attribute, value, metadata);
        Require(actual_id == expected_id, "staged upsert id mismatch with model");
        break;
      }
      case 3: {
        const auto expected_removed_id = model_stage_remove(entity, attribute);
        const auto actual_removed_id = store.StageRemove(entity, attribute);
        Require(actual_removed_id.has_value() == expected_removed_id.has_value(),
                "staged remove presence mismatch with model");
        if (actual_removed_id.has_value()) {
          Require(*actual_removed_id == *expected_removed_id, "staged remove id mismatch with model");
        }
        break;
      }
      case 4:
        model_commit();
        store.CommitStaged();
        break;
      case 5:
        model_rollback();
        store.RollbackStaged();
        break;
      default:
        break;
    }

    Require(store.PendingMutationCount() == pending_mutations,
            "pending mutation count mismatch with model");

    const auto expected_all = model_all();
    const auto actual_all = store.All(-1);
    require_entries_equal(expected_all, actual_all, "All() parity");

    const int query_limit = limit_dist(rng);
    std::string prefix{};
    switch (prefix_dist(rng)) {
      case 0:
        prefix = "";
        break;
      case 1:
        prefix = "user:";
        break;
      case 2:
        prefix = "order:";
        break;
      default:
        break;
    }
    const auto expected_query = model_query(prefix, query_limit);
    const auto actual_query = store.QueryByEntityPrefix(prefix, query_limit);
    require_entries_equal(expected_query, actual_query, "QueryByEntityPrefix() parity");

    const auto maybe_store_entry = store.Get(entity, attribute);
    const auto committed_it = committed.find(key_for(entity, attribute));
    const bool expected_present = committed_it != committed.end();
    Require(maybe_store_entry.has_value() == expected_present, "Get() presence mismatch with model");
    if (expected_present) {
      Require(maybe_store_entry->id == committed_it->second.id, "Get() id mismatch with model");
      Require(maybe_store_entry->version == committed_it->second.version, "Get() version mismatch with model");
      Require(maybe_store_entry->value == committed_it->second.value, "Get() value mismatch with model");
      Require(maybe_store_entry->metadata == committed_it->second.metadata, "Get() metadata mismatch with model");
    }
  }
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("structured_memory_store_test: start");
    ScenarioUpsertGetAndVersioning();
    ScenarioRemove();
    ScenarioQueryDeterminismAndLimit();
    ScenarioValidation();
    ScenarioStagedMutationVisibilityAndRollback();
    ScenarioSeededStagedMutationModelParity();
    waxcpp::tests::Log("structured_memory_store_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
