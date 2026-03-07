#include "waxcpp/embedding_memoizer.hpp"
#include "waxcpp/embeddings.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

namespace waxcpp {

// ──────────────────────────────────────────────
// EmbeddingKey
// ──────────────────────────────────────────────

std::uint64_t EmbeddingKey::Make(std::string_view text,
                                  const EmbeddingIdentity* identity,
                                  int dimensions,
                                  bool normalized) {
  FNV1a64 hasher;
  if (identity != nullptr) {
    hasher.Append(identity->provider.value_or(""));
    hasher.Append(identity->model.value_or(""));
    hasher.Append(std::to_string(identity->dimensions.value_or(dimensions)));
    hasher.Append(identity->normalized.value_or(normalized) ? "true" : "false");
  } else {
    hasher.Append("nil_identity");
    hasher.Append(std::to_string(dimensions));
    hasher.Append(normalized ? "true" : "false");
  }
  hasher.Append(text);
  return hasher.Finalize();
}

// ──────────────────────────────────────────────
// EmbeddingMemoizer
// ──────────────────────────────────────────────

EmbeddingMemoizer::EmbeddingMemoizer(int capacity)
    : capacity_(std::max(0, capacity)) {
  entries_.reserve(static_cast<std::size_t>(capacity_));
}

std::optional<std::vector<float>> EmbeddingMemoizer::Get(std::uint64_t key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ <= 0) return std::nullopt;

  auto it = entries_.find(key);
  if (it == entries_.end()) {
    ++misses_;
    return std::nullopt;
  }
  ++hits_;
  MoveToFront(it->second);
  return it->second.value;
}

std::unordered_map<std::uint64_t, std::vector<float>> EmbeddingMemoizer::GetBatch(
    const std::vector<std::uint64_t>& keys) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::unordered_map<std::uint64_t, std::vector<float>> results;
  if (capacity_ <= 0) return results;

  results.reserve(keys.size());
  for (const auto k : keys) {
    auto it = entries_.find(k);
    if (it != entries_.end()) {
      ++hits_;
      MoveToFront(it->second);
      results[k] = it->second.value;
    } else {
      ++misses_;
    }
  }
  return results;
}

void EmbeddingMemoizer::Set(std::uint64_t key, std::vector<float> value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ <= 0) return;

  // Update existing entry.
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    it->second.value = std::move(value);
    MoveToFront(it->second);
    return;
  }

  // Insert new entry at head.
  Entry entry;
  entry.key = key;
  entry.value = std::move(value);
  entry.prev = std::nullopt;
  entry.next = head_;

  if (head_.has_value()) {
    auto head_it = entries_.find(*head_);
    if (head_it != entries_.end()) {
      head_it->second.prev = key;
    }
  } else {
    tail_ = key;
  }
  head_ = key;
  entries_[key] = std::move(entry);

  // Evict LRU if over capacity.
  if (static_cast<int>(entries_.size()) > capacity_ && tail_.has_value()) {
    Remove(*tail_);
  }
}

void EmbeddingMemoizer::SetBatch(
    const std::vector<std::pair<std::uint64_t, std::vector<float>>>& items) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ <= 0) return;

  for (const auto& [k, v] : items) {
    // Inline set without re-acquiring mutex (already held).
    auto it = entries_.find(k);
    if (it != entries_.end()) {
      it->second.value = v;
      MoveToFront(it->second);
      continue;
    }
    Entry entry;
    entry.key = k;
    entry.value = v;
    entry.prev = std::nullopt;
    entry.next = head_;
    if (head_.has_value()) {
      auto head_it = entries_.find(*head_);
      if (head_it != entries_.end()) {
        head_it->second.prev = k;
      }
    } else {
      tail_ = k;
    }
    head_ = k;
    entries_[k] = std::move(entry);
    if (static_cast<int>(entries_.size()) > capacity_ && tail_.has_value()) {
      Remove(*tail_);
    }
  }
}

void EmbeddingMemoizer::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  head_ = std::nullopt;
  tail_ = std::nullopt;
  hits_ = 0;
  misses_ = 0;
}

double EmbeddingMemoizer::HitRate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto total = hits_ + misses_;
  if (total == 0) return 0.0;
  return static_cast<double>(hits_) / static_cast<double>(total);
}

void EmbeddingMemoizer::ResetStats() {
  std::lock_guard<std::mutex> lock(mutex_);
  hits_ = 0;
  misses_ = 0;
}

std::size_t EmbeddingMemoizer::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

std::unique_ptr<EmbeddingMemoizer> EmbeddingMemoizer::FromConfig(int capacity, bool enabled) {
  if (!enabled || capacity <= 0) return nullptr;
  return std::make_unique<EmbeddingMemoizer>(capacity);
}

// ──────────────────────────────────────────────
// Private helpers
// ──────────────────────────────────────────────

void EmbeddingMemoizer::MoveToFront(Entry& entry) {
  const auto key = entry.key;
  if (head_.has_value() && *head_ == key) {
    // Already at front.
    return;
  }

  const auto prev_key = entry.prev;
  const auto next_key = entry.next;

  // Unlink from current position.
  if (prev_key.has_value()) {
    auto prev_it = entries_.find(*prev_key);
    if (prev_it != entries_.end()) {
      prev_it->second.next = next_key;
    }
  }
  if (next_key.has_value()) {
    auto next_it = entries_.find(*next_key);
    if (next_it != entries_.end()) {
      next_it->second.prev = prev_key;
    }
  }
  if (tail_.has_value() && *tail_ == key) {
    tail_ = prev_key;
  }

  // Link at front.
  entry.prev = std::nullopt;
  entry.next = head_;
  if (head_.has_value()) {
    auto head_it = entries_.find(*head_);
    if (head_it != entries_.end()) {
      head_it->second.prev = key;
    }
  }
  head_ = key;
}

void EmbeddingMemoizer::Remove(std::uint64_t key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;

  const auto prev_key = it->second.prev;
  const auto next_key = it->second.next;

  if (prev_key.has_value()) {
    auto prev_it = entries_.find(*prev_key);
    if (prev_it != entries_.end()) {
      prev_it->second.next = next_key;
    }
  } else {
    head_ = next_key;
  }
  if (next_key.has_value()) {
    auto next_it = entries_.find(*next_key);
    if (next_it != entries_.end()) {
      next_it->second.prev = prev_key;
    }
  } else {
    tail_ = prev_key;
  }
  entries_.erase(it);
}

}  // namespace waxcpp
