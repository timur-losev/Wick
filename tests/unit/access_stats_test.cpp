#include "waxcpp/access_stats.hpp"
#include "../test_logger.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
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

// ---------- FrameAccessStats tests ----------

void TestFrameAccessStatsInit() {
  Log("=== TestFrameAccessStatsInit ===");
  FrameAccessStats stats(42, 1000);
  Check(stats.frame_id == 42, "frame_id");
  Check(stats.access_count == 1, "initial access_count");
  Check(stats.first_access_ms == 1000, "first_access_ms");
  Check(stats.last_access_ms == 1000, "last_access_ms");
}

void TestFrameAccessStatsRecordAccess() {
  Log("=== TestFrameAccessStatsRecordAccess ===");
  FrameAccessStats stats(1, 1000);
  stats.RecordAccess(2000);
  Check(stats.access_count == 2, "access_count incremented");
  Check(stats.last_access_ms == 2000, "last_access_ms updated");
  Check(stats.first_access_ms == 1000, "first_access_ms unchanged");

  stats.RecordAccess(3000);
  Check(stats.access_count == 3, "access_count incremented again");
  Check(stats.last_access_ms == 3000, "last_access_ms updated again");
}

void TestFrameAccessStatsSaturatingOverflow() {
  Log("=== TestFrameAccessStatsSaturatingOverflow ===");
  FrameAccessStats stats(1, 1000);
  stats.access_count = UINT32_MAX;
  stats.RecordAccess(2000);
  Check(stats.access_count == UINT32_MAX, "saturated at max");
  Check(stats.last_access_ms == 2000, "last_access_ms still updated");
}

// ---------- AccessStatsManager tests ----------

void TestManagerRecordAndGet() {
  Log("=== TestManagerRecordAndGet ===");
  AccessStatsManager mgr;
  Check(mgr.Count() == 0, "initially empty");

  mgr.RecordAccess(10, 1000);
  Check(mgr.Count() == 1, "count after first record");

  auto stats = mgr.GetStats(10);
  Check(stats.has_value(), "has stats for frame 10");
  Check(stats->access_count == 1, "access_count is 1");
  Check(stats->first_access_ms == 1000, "first_access_ms");
  Check(stats->last_access_ms == 1000, "last_access_ms");
}

void TestManagerRecordMultiple() {
  Log("=== TestManagerRecordMultiple ===");
  AccessStatsManager mgr;
  mgr.RecordAccess(10, 1000);
  mgr.RecordAccess(10, 2000);
  mgr.RecordAccess(10, 3000);

  auto stats = mgr.GetStats(10);
  Check(stats.has_value(), "has stats");
  Check(stats->access_count == 3, "access_count is 3");
  Check(stats->first_access_ms == 1000, "first_access_ms preserved");
  Check(stats->last_access_ms == 3000, "last_access_ms is latest");
}

void TestManagerRecordAccessesBatch() {
  Log("=== TestManagerRecordAccessesBatch ===");
  AccessStatsManager mgr;
  std::vector<std::uint64_t> ids = {1, 2, 3, 1};
  mgr.RecordAccesses(ids, 5000);

  Check(mgr.Count() == 3, "3 distinct frames");

  auto s1 = mgr.GetStats(1);
  Check(s1.has_value(), "has stats for 1");
  Check(s1->access_count == 2, "frame 1 accessed twice in batch");

  auto s2 = mgr.GetStats(2);
  Check(s2.has_value(), "has stats for 2");
  Check(s2->access_count == 1, "frame 2 accessed once");
}

void TestManagerGetMultiple() {
  Log("=== TestManagerGetMultiple ===");
  AccessStatsManager mgr;
  mgr.RecordAccess(1, 1000);
  mgr.RecordAccess(2, 2000);
  mgr.RecordAccess(3, 3000);

  auto result = mgr.GetStats({1, 3, 99});
  Check(result.size() == 2, "only existing frames returned");
  Check(result.count(1) == 1, "frame 1 present");
  Check(result.count(3) == 1, "frame 3 present");
  Check(result.count(99) == 0, "frame 99 absent");
}

void TestManagerGetMissing() {
  Log("=== TestManagerGetMissing ===");
  AccessStatsManager mgr;
  auto stats = mgr.GetStats(999);
  Check(!stats.has_value(), "no stats for untracked frame");
}

void TestManagerPruneStats() {
  Log("=== TestManagerPruneStats ===");
  AccessStatsManager mgr;
  mgr.RecordAccess(1, 1000);
  mgr.RecordAccess(2, 2000);
  mgr.RecordAccess(3, 3000);
  mgr.RecordAccess(4, 4000);

  std::set<std::uint64_t> active = {2, 4};
  mgr.PruneStats(active);
  Check(mgr.Count() == 2, "pruned to active set");
  Check(!mgr.GetStats(1).has_value(), "frame 1 removed");
  Check(mgr.GetStats(2).has_value(), "frame 2 kept");
  Check(!mgr.GetStats(3).has_value(), "frame 3 removed");
  Check(mgr.GetStats(4).has_value(), "frame 4 kept");
}

void TestManagerExportImport() {
  Log("=== TestManagerExportImport ===");
  AccessStatsManager mgr;
  mgr.RecordAccess(10, 1000);
  mgr.RecordAccess(10, 2000);
  mgr.RecordAccess(20, 3000);

  auto exported = mgr.ExportStats();
  Check(exported.size() == 2, "exported 2 stats");

  AccessStatsManager mgr2;
  mgr2.ImportStats(exported);
  Check(mgr2.Count() == 2, "imported 2 stats");

  auto s10 = mgr2.GetStats(10);
  Check(s10.has_value(), "frame 10 imported");
  Check(s10->access_count == 2, "access_count preserved");

  auto s20 = mgr2.GetStats(20);
  Check(s20.has_value(), "frame 20 imported");
  Check(s20->access_count == 1, "access_count preserved");
}

void TestManagerImportReplaces() {
  Log("=== TestManagerImportReplaces ===");
  AccessStatsManager mgr;
  mgr.RecordAccess(1, 1000);
  mgr.RecordAccess(2, 2000);
  Check(mgr.Count() == 2, "initially 2");

  std::vector<FrameAccessStats> replacement;
  replacement.emplace_back(FrameAccessStats(99, 9999));
  mgr.ImportStats(replacement);
  Check(mgr.Count() == 1, "replaced with 1");
  Check(!mgr.GetStats(1).has_value(), "old frame 1 gone");
  Check(mgr.GetStats(99).has_value(), "new frame 99 present");
}

void TestManagerPruneEmptySet() {
  Log("=== TestManagerPruneEmptySet ===");
  AccessStatsManager mgr;
  mgr.RecordAccess(1, 1000);
  mgr.RecordAccess(2, 2000);

  std::set<std::uint64_t> empty;
  mgr.PruneStats(empty);
  Check(mgr.Count() == 0, "all pruned with empty active set");
}

void TestManagerWallClockAccess() {
  Log("=== TestManagerWallClockAccess ===");
  // RecordAccess without explicit timestamp should use wall clock.
  AccessStatsManager mgr;
  mgr.RecordAccess(42);
  auto stats = mgr.GetStats(42);
  Check(stats.has_value(), "recorded via wall clock");
  Check(stats->access_count == 1, "count is 1");
  // We can't test exact timestamp but it should be > 0.
  Check(stats->last_access_ms > 0, "timestamp is positive");
}

}  // namespace

int main() {
  TestFrameAccessStatsInit();
  TestFrameAccessStatsRecordAccess();
  TestFrameAccessStatsSaturatingOverflow();
  TestManagerRecordAndGet();
  TestManagerRecordMultiple();
  TestManagerRecordAccessesBatch();
  TestManagerGetMultiple();
  TestManagerGetMissing();
  TestManagerPruneStats();
  TestManagerExportImport();
  TestManagerImportReplaces();
  TestManagerPruneEmptySet();
  TestManagerWallClockAccess();

  std::cout << "\naccess_stats_test: " << g_pass << " passed, " << g_fail
            << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
