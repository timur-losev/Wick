#include "waxcpp/structured_memory.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waxcpp {
namespace {

bool EntryLess(const StructuredMemoryEntry& lhs, const StructuredMemoryEntry& rhs) {
  if (lhs.entity != rhs.entity) {
    return lhs.entity < rhs.entity;
  }
  if (lhs.attribute != rhs.attribute) {
    return lhs.attribute < rhs.attribute;
  }
  return lhs.id < rhs.id;
}

}  // namespace

std::string StructuredMemoryStore::CompositeKey(const std::string& entity, const std::string& attribute) {
  return entity + '\x1F' + attribute;
}

void StructuredMemoryStore::EnsureStagingState() {
  if (!pending_mutations_.empty()) {
    return;
  }
  staged_entries_ = entries_;
  staged_next_id_ = next_id_;
}

std::uint64_t StructuredMemoryStore::StageUpsert(const std::string& entity,
                                                 const std::string& attribute,
                                                 const std::string& value,
                                                 const Metadata& metadata) {
  if (entity.empty()) {
    throw std::runtime_error("StructuredMemoryStore::Upsert entity must be non-empty");
  }
  if (attribute.empty()) {
    throw std::runtime_error("StructuredMemoryStore::Upsert attribute must be non-empty");
  }

  EnsureStagingState();

  const auto key = CompositeKey(entity, attribute);
  auto it = staged_entries_.find(key);
  std::uint64_t id = 0;
  if (it == staged_entries_.end()) {
    StructuredMemoryEntry entry{};
    entry.id = staged_next_id_++;
    entry.entity = entity;
    entry.attribute = attribute;
    entry.value = value;
    entry.metadata = metadata;
    entry.version = 1;
    id = entry.id;
    staged_entries_.emplace(key, std::move(entry));
  } else {
    it->second.value = value;
    it->second.metadata = metadata;
    it->second.version += 1;
    id = it->second.id;
  }

  pending_mutations_.push_back(PendingMutation{
      PendingMutationType::kUpsert,
      key,
      id,
  });
  return id;
}

std::optional<std::uint64_t> StructuredMemoryStore::StageRemove(const std::string& entity,
                                                                const std::string& attribute) {
  if (entity.empty()) {
    throw std::runtime_error("StructuredMemoryStore::Remove entity must be non-empty");
  }
  if (attribute.empty()) {
    throw std::runtime_error("StructuredMemoryStore::Remove attribute must be non-empty");
  }
  const auto key = CompositeKey(entity, attribute);
  if (pending_mutations_.empty()) {
    const auto committed_it = entries_.find(key);
    if (committed_it == entries_.end()) {
      return std::nullopt;
    }
    staged_entries_ = entries_;
    staged_next_id_ = next_id_;
  } else if (staged_entries_.empty()) {
    // Defensive fallback for externally manipulated state.
    staged_entries_ = entries_;
    staged_next_id_ = next_id_;
  }

  std::optional<std::uint64_t> removed_id{};
  const auto it = staged_entries_.find(key);
  if (it != staged_entries_.end()) {
    removed_id = it->second.id;
    staged_entries_.erase(it);
  } else {
    return std::nullopt;
  }
  pending_mutations_.push_back(PendingMutation{
      PendingMutationType::kRemove,
      key,
      removed_id,
  });
  return removed_id;
}

void StructuredMemoryStore::CommitStaged() {
  if (pending_mutations_.empty()) {
    return;
  }
  entries_ = std::move(staged_entries_);
  staged_entries_.clear();
  next_id_ = staged_next_id_;
  pending_mutations_.clear();
}

void StructuredMemoryStore::RollbackStaged() {
  pending_mutations_.clear();
  staged_entries_.clear();
  staged_next_id_ = next_id_;
}

std::size_t StructuredMemoryStore::PendingMutationCount() const {
  return pending_mutations_.size();
}

std::uint64_t StructuredMemoryStore::Upsert(const std::string& entity,
                                            const std::string& attribute,
                                            const std::string& value,
                                            const Metadata& metadata) {
  const auto id = StageUpsert(entity, attribute, value, metadata);
  CommitStaged();
  return id;
}

bool StructuredMemoryStore::Remove(const std::string& entity, const std::string& attribute) {
  const auto removed_id = StageRemove(entity, attribute);
  CommitStaged();
  return removed_id.has_value();
}

std::optional<StructuredMemoryEntry> StructuredMemoryStore::Get(const std::string& entity,
                                                                const std::string& attribute) const {
  const auto key = CompositeKey(entity, attribute);
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<StructuredMemoryEntry> StructuredMemoryStore::QueryByEntityPrefix(const std::string& entity_prefix,
                                                                               int limit) const {
  if (limit == 0) {
    return {};
  }
  std::vector<StructuredMemoryEntry> out{};
  out.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) {
    if (!entity_prefix.empty() && entry.entity.rfind(entity_prefix, 0) != 0) {
      continue;
    }
    out.push_back(entry);
  }
  std::sort(out.begin(), out.end(), EntryLess);
  if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
    out.resize(static_cast<std::size_t>(limit));
  }
  return out;
}

std::vector<StructuredMemoryEntry> StructuredMemoryStore::All(int limit) const {
  return QueryByEntityPrefix("", limit);
}

}  // namespace waxcpp
