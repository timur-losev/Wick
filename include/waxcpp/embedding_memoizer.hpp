#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace waxcpp {

/// FNV-1a 64-bit hash with per-field separator, matching Swift FNV1a64.
struct FNV1a64 {
  std::uint64_t state = 14695981039346656037ULL;

  void Append(std::string_view text) {
    for (unsigned char byte : text) {
      state ^= static_cast<std::uint64_t>(byte);
      state *= 1099511628211ULL;
    }
    // Field separator (matching Swift sentinel 0xFF).
    state ^= 0xFFULL;
    state *= 1099511628211ULL;
  }

  [[nodiscard]] std::uint64_t Finalize() const { return state; }
};

/// Forward declaration for EmbeddingIdentity.
struct EmbeddingIdentity;

/// Build a deterministic cache key for an embedding request.
struct EmbeddingKey {
  static std::uint64_t Make(std::string_view text,
                            const EmbeddingIdentity* identity,
                            int dimensions,
                            bool normalized);
};

/// High-performance LRU cache for embeddings with O(1) access and eviction.
/// Thread-safe via internal mutex (C++ equivalent of Swift actor).
class EmbeddingMemoizer {
 public:
  explicit EmbeddingMemoizer(int capacity);

  /// Get a cached embedding. Returns empty optional if not found. O(1).
  std::optional<std::vector<float>> Get(std::uint64_t key);

  /// Batch get multiple embeddings. Returns map of found keys → embeddings.
  std::unordered_map<std::uint64_t, std::vector<float>> GetBatch(
      const std::vector<std::uint64_t>& keys);

  /// Cache an embedding. Evicts LRU entry if at capacity. O(1).
  void Set(std::uint64_t key, std::vector<float> value);

  /// Batch set multiple embeddings.
  void SetBatch(const std::vector<std::pair<std::uint64_t, std::vector<float>>>& items);

  /// Returns cache hit rate (0.0 to 1.0).
  double HitRate() const;

  /// Reset hit/miss counters.
  void ResetStats();

  /// Remove all cached entries and reset statistics.
  void Clear();

  /// Current number of entries in the cache.
  std::size_t Size() const;

  /// Configured capacity.
  int Capacity() const { return capacity_; }

  /// Factory helper matching Swift EmbeddingMemoizer.fromConfig().
  static std::unique_ptr<EmbeddingMemoizer> FromConfig(int capacity, bool enabled = true);

 private:
  struct Entry {
    std::uint64_t key = 0;
    std::vector<float> value;
    std::optional<std::uint64_t> prev;
    std::optional<std::uint64_t> next;
  };

  void MoveToFront(Entry& entry);
  void Remove(std::uint64_t key);

  int capacity_;
  std::unordered_map<std::uint64_t, Entry> entries_;
  std::optional<std::uint64_t> head_;
  std::optional<std::uint64_t> tail_;
  std::uint64_t hits_ = 0;
  std::uint64_t misses_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace waxcpp
