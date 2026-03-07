#include "waxcpp/embedding_memoizer.hpp"
#include "waxcpp/embeddings.hpp"
#include "../test_logger.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace waxcpp;
using namespace waxcpp::tests;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* label) {
  if (condition) {
    ++g_pass;
    Log(std::string("  PASS: ") + label);
  } else {
    ++g_fail;
    LogError(std::string("  FAIL: ") + label);
  }
}

std::vector<float> Vec(std::initializer_list<float> vals) {
  return std::vector<float>(vals);
}

// ============================================================
// 1. Basic get/set
// ============================================================

void TestBasicGetSet() {
  Log("=== TestBasicGetSet ===");
  EmbeddingMemoizer cache(4);
  Check(!cache.Get(1).has_value(), "miss before set");
  cache.Set(1, Vec({0.1f, 0.2f, 0.3f}));
  auto result = cache.Get(1);
  Check(result.has_value(), "hit after set");
  Check(result->size() == 3, "correct size");
  Check((*result)[0] == 0.1f, "correct value");
}

// ============================================================
// 2. Eviction at capacity
// ============================================================

void TestEvictionAtCapacity() {
  Log("=== TestEvictionAtCapacity ===");
  EmbeddingMemoizer cache(3);
  cache.Set(1, Vec({1.0f}));
  cache.Set(2, Vec({2.0f}));
  cache.Set(3, Vec({3.0f}));
  Check(cache.Size() == 3, "at capacity");

  // Insert 4th → should evict key 1 (LRU).
  cache.Set(4, Vec({4.0f}));
  Check(cache.Size() == 3, "still at capacity after eviction");
  Check(!cache.Get(1).has_value(), "LRU key evicted");
  Check(cache.Get(2).has_value(), "key 2 still present");
  Check(cache.Get(3).has_value(), "key 3 still present");
  Check(cache.Get(4).has_value(), "key 4 present");
}

// ============================================================
// 3. Access refreshes LRU order
// ============================================================

void TestAccessRefreshesOrder() {
  Log("=== TestAccessRefreshesOrder ===");
  EmbeddingMemoizer cache(3);
  cache.Set(1, Vec({1.0f}));
  cache.Set(2, Vec({2.0f}));
  cache.Set(3, Vec({3.0f}));

  // Access key 1, making it MRU → LRU is now key 2.
  (void)cache.Get(1);

  cache.Set(4, Vec({4.0f}));
  Check(!cache.Get(2).has_value(), "key 2 evicted (was LRU after key 1 accessed)");
  Check(cache.Get(1).has_value(), "key 1 still present (refreshed)");
  Check(cache.Get(3).has_value(), "key 3 still present");
  Check(cache.Get(4).has_value(), "key 4 present");
}

// ============================================================
// 4. Update existing key
// ============================================================

void TestUpdateExistingKey() {
  Log("=== TestUpdateExistingKey ===");
  EmbeddingMemoizer cache(4);
  cache.Set(1, Vec({1.0f}));
  cache.Set(1, Vec({9.0f}));
  auto result = cache.Get(1);
  Check(result.has_value(), "key still present after update");
  Check(result->size() == 1 && (*result)[0] == 9.0f, "value updated");
  Check(cache.Size() == 1, "no duplicate entries");
}

// ============================================================
// 5. Zero capacity
// ============================================================

void TestZeroCapacity() {
  Log("=== TestZeroCapacity ===");
  EmbeddingMemoizer cache(0);
  cache.Set(1, Vec({1.0f}));
  Check(!cache.Get(1).has_value(), "zero-capacity cache never stores");
  Check(cache.Size() == 0, "zero size");
}

// ============================================================
// 6. Negative capacity
// ============================================================

void TestNegativeCapacity() {
  Log("=== TestNegativeCapacity ===");
  EmbeddingMemoizer cache(-5);
  cache.Set(1, Vec({1.0f}));
  Check(!cache.Get(1).has_value(), "negative capacity clamped to zero");
  Check(cache.Size() == 0, "zero size");
}

// ============================================================
// 7. Hit rate statistics
// ============================================================

void TestHitRate() {
  Log("=== TestHitRate ===");
  EmbeddingMemoizer cache(4);
  Check(cache.HitRate() == 0.0, "initial hit rate is 0");

  cache.Set(1, Vec({1.0f}));
  (void)cache.Get(1);  // hit
  (void)cache.Get(2);  // miss
  Check(cache.HitRate() == 0.5, "50% hit rate after 1 hit + 1 miss");

  cache.ResetStats();
  Check(cache.HitRate() == 0.0, "hit rate reset to 0");
}

// ============================================================
// 8. Batch get
// ============================================================

void TestBatchGet() {
  Log("=== TestBatchGet ===");
  EmbeddingMemoizer cache(8);
  cache.Set(1, Vec({1.0f}));
  cache.Set(2, Vec({2.0f}));
  cache.Set(3, Vec({3.0f}));

  auto results = cache.GetBatch({1, 3, 5});
  Check(results.size() == 2, "batch get returns found entries only");
  Check(results.count(1) == 1, "key 1 found");
  Check(results.count(3) == 1, "key 3 found");
  Check(results.count(5) == 0, "key 5 not found");
}

// ============================================================
// 9. Batch set
// ============================================================

void TestBatchSet() {
  Log("=== TestBatchSet ===");
  EmbeddingMemoizer cache(4);
  cache.SetBatch({
      {1, Vec({1.0f})},
      {2, Vec({2.0f})},
      {3, Vec({3.0f})},
  });
  Check(cache.Size() == 3, "batch set stored all entries");
  Check(cache.Get(1).has_value(), "key 1 present");
  Check(cache.Get(2).has_value(), "key 2 present");
  Check(cache.Get(3).has_value(), "key 3 present");
}

// ============================================================
// 10. Batch set with eviction
// ============================================================

void TestBatchSetEviction() {
  Log("=== TestBatchSetEviction ===");
  EmbeddingMemoizer cache(2);
  cache.SetBatch({
      {1, Vec({1.0f})},
      {2, Vec({2.0f})},
      {3, Vec({3.0f})},
  });
  Check(cache.Size() == 2, "batch set respects capacity");
  // Key 1 was oldest → should be evicted.
  Check(!cache.Get(1).has_value(), "earliest batch entry evicted");
  Check(cache.Get(2).has_value(), "key 2 present");
  Check(cache.Get(3).has_value(), "key 3 present");
}

// ============================================================
// 11. FromConfig factory
// ============================================================

void TestFromConfig() {
  Log("=== TestFromConfig ===");
  auto cache = EmbeddingMemoizer::FromConfig(16, true);
  Check(cache != nullptr, "enabled config returns non-null");
  Check(cache->Capacity() == 16, "correct capacity");

  auto disabled = EmbeddingMemoizer::FromConfig(16, false);
  Check(disabled == nullptr, "disabled config returns null");

  auto zero = EmbeddingMemoizer::FromConfig(0, true);
  Check(zero == nullptr, "zero capacity returns null");

  auto negative = EmbeddingMemoizer::FromConfig(-3, true);
  Check(negative == nullptr, "negative capacity returns null");
}

// ============================================================
// 12. FNV1a64 hash
// ============================================================

void TestFNV1a64() {
  Log("=== TestFNV1a64 ===");
  FNV1a64 h1;
  h1.Append("hello");
  auto hash1 = h1.Finalize();

  FNV1a64 h2;
  h2.Append("hello");
  auto hash2 = h2.Finalize();
  Check(hash1 == hash2, "same input same hash");

  FNV1a64 h3;
  h3.Append("world");
  auto hash3 = h3.Finalize();
  Check(hash1 != hash3, "different input different hash");

  // Multi-field: "a" + "b" differs from "ab" + "".
  FNV1a64 h4;
  h4.Append("a");
  h4.Append("b");
  auto hash4 = h4.Finalize();

  FNV1a64 h5;
  h5.Append("ab");
  h5.Append("");
  auto hash5 = h5.Finalize();
  Check(hash4 != hash5, "field separator prevents collision");
}

// ============================================================
// 13. EmbeddingKey
// ============================================================

void TestEmbeddingKey() {
  Log("=== TestEmbeddingKey ===");
  // Same parameters should produce same key.
  auto key1 = EmbeddingKey::Make("test text", nullptr, 384, true);
  auto key2 = EmbeddingKey::Make("test text", nullptr, 384, true);
  Check(key1 == key2, "same params same key");

  // Different text should produce different key.
  auto key3 = EmbeddingKey::Make("other text", nullptr, 384, true);
  Check(key1 != key3, "different text different key");

  // Different dimensions should produce different key.
  auto key4 = EmbeddingKey::Make("test text", nullptr, 768, true);
  Check(key1 != key4, "different dimensions different key");

  // With identity.
  EmbeddingIdentity identity;
  identity.provider = "local";
  identity.model = "minilm";
  identity.dimensions = 384;
  identity.normalized = true;
  auto key5 = EmbeddingKey::Make("test text", &identity, 384, true);
  Check(key5 != key1, "identity vs nil_identity differ");

  auto key6 = EmbeddingKey::Make("test text", &identity, 384, true);
  Check(key5 == key6, "same identity same key");
}

// ============================================================
// 14. Capacity 1
// ============================================================

void TestCapacityOne() {
  Log("=== TestCapacityOne ===");
  EmbeddingMemoizer cache(1);
  cache.Set(1, Vec({1.0f}));
  Check(cache.Get(1).has_value(), "single entry present");
  cache.Set(2, Vec({2.0f}));
  Check(!cache.Get(1).has_value(), "old entry evicted");
  Check(cache.Get(2).has_value(), "new entry present");
  Check(cache.Size() == 1, "size stays at 1");
}

// ============================================================
// 15. LRU eviction order across multiple operations
// ============================================================

void TestLRUEvictionOrder() {
  Log("=== TestLRUEvictionOrder ===");
  EmbeddingMemoizer cache(3);
  // Insert 1,2,3 → MRU order: 3,2,1
  cache.Set(1, Vec({1.0f}));
  cache.Set(2, Vec({2.0f}));
  cache.Set(3, Vec({3.0f}));

  // Access 1 → MRU order: 1,3,2
  (void)cache.Get(1);
  // Access 2 → MRU order: 2,1,3
  (void)cache.Get(2);

  // Insert 4 → evicts 3 (current LRU)
  cache.Set(4, Vec({4.0f}));
  Check(!cache.Get(3).has_value(), "key 3 evicted (LRU after access pattern)");
  Check(cache.Get(1).has_value(), "key 1 present");
  Check(cache.Get(2).has_value(), "key 2 present");
  Check(cache.Get(4).has_value(), "key 4 present");
}

}  // namespace

int main() {
  Log("== EmbeddingMemoizer Tests ==");

  TestBasicGetSet();
  TestEvictionAtCapacity();
  TestAccessRefreshesOrder();
  TestUpdateExistingKey();
  TestZeroCapacity();
  TestNegativeCapacity();
  TestHitRate();
  TestBatchGet();
  TestBatchSet();
  TestBatchSetEviction();
  TestFromConfig();
  TestFNV1a64();
  TestEmbeddingKey();
  TestCapacityOne();
  TestLRUEvictionOrder();

  std::cout << "\n== Results: " << g_pass << " passed, " << g_fail << " failed ==\n";
  return g_fail > 0 ? 1 : 0;
}
