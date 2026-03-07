#include "waxcpp/live_set_rewrite.hpp"
#include "waxcpp/wax_store.hpp"
#include "../test_logger.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
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

// ── Helper: create a default enabled schedule. ──────────────

LiveSetRewriteSchedule DefaultSchedule() {
  LiveSetRewriteSchedule s;
  s.enabled = true;
  s.check_every_flushes = 4;
  s.min_dead_payload_bytes = 1024;
  s.min_dead_payload_fraction = 0.20;
  s.minimum_idle_ms = 5000;
  s.min_interval_ms = 60000;
  return s;
}

MaintenanceGateInput DefaultInput(const LiveSetRewriteSchedule& sched) {
  MaintenanceGateInput in;
  in.schedule = &sched;
  in.flush_count = 4;  // passes cadence gate (4 % 4 == 0)
  in.force = false;
  in.now_ms = 200'000;
  in.last_completed_ms = 100'000;          // 100s ago > 60s cooldown
  in.last_write_activity_ms = 190'000;     // 10s ago > 5s idle
  in.dead_payload_bytes = 2048;            // > 1024 threshold
  in.total_payload_bytes = 8192;           // fraction = 0.25 >= 0.20
  return in;
}

// ============================================================
// 1. Disabled schedule
// ============================================================

void TestDisabled() {
  Log("=== TestDisabled ===");

  // nullptr schedule
  {
    MaintenanceGateInput in{};
    ScheduledLiveSetMaintenanceReport report{};
    bool ok = EvaluateMaintenanceGate(in, report);
    Check(!ok, "nullptr schedule returns false");
    Check(report.outcome == MaintenanceOutcome::kDisabled, "outcome is kDisabled (nullptr)");
  }

  // schedule.enabled == false
  {
    LiveSetRewriteSchedule sched;
    sched.enabled = false;
    MaintenanceGateInput in{};
    in.schedule = &sched;
    ScheduledLiveSetMaintenanceReport report{};
    bool ok = EvaluateMaintenanceGate(in, report);
    Check(!ok, "disabled schedule returns false");
    Check(report.outcome == MaintenanceOutcome::kDisabled, "outcome is kDisabled (disabled)");
  }

  // force + disabled still returns false
  {
    LiveSetRewriteSchedule sched;
    sched.enabled = false;
    MaintenanceGateInput in{};
    in.schedule = &sched;
    in.force = true;
    ScheduledLiveSetMaintenanceReport report{};
    bool ok = EvaluateMaintenanceGate(in, report);
    Check(!ok, "force + disabled still returns false");
  }
}

// ============================================================
// 2. All gates pass
// ============================================================

void TestAllGatesPass() {
  Log("=== TestAllGatesPass ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "all gates pass → eligible");
  Check(report.dead_payload_fraction > 0.0, "fraction computed");
  Check(report.flush_count == in.flush_count, "flush_count copied");
  Check(report.dead_payload_bytes == in.dead_payload_bytes, "dead_payload_bytes copied");
  Check(report.total_payload_bytes == in.total_payload_bytes, "total_payload_bytes copied");
}

// ============================================================
// 3. Cadence gate
// ============================================================

void TestCadenceGate() {
  Log("=== TestCadenceGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // flush_count not a multiple of cadence → skipped
  in.flush_count = 5;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "cadence gate rejects non-multiple");
  Check(report.outcome == MaintenanceOutcome::kCadenceSkipped, "outcome kCadenceSkipped");

  // force bypasses cadence gate
  in.force = true;
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "force bypasses cadence gate");
}

// ============================================================
// 4. Cooldown gate
// ============================================================

void TestCooldownGate() {
  Log("=== TestCooldownGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // now_ms too close to last_completed_ms
  in.last_completed_ms = 150'000;  // 50s ago, cooldown is 60s
  in.now_ms = 200'000;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "cooldown gate rejects too-soon");
  Check(report.outcome == MaintenanceOutcome::kCooldownSkipped, "outcome kCooldownSkipped");

  // force bypasses cooldown gate
  in.force = true;
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "force bypasses cooldown gate");
}

// ============================================================
// 5. Idle gate
// ============================================================

void TestIdleGate() {
  Log("=== TestIdleGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // write activity 2s ago < minimum_idle_ms (5s)
  in.last_write_activity_ms = 198'000;
  in.now_ms = 200'000;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "idle gate rejects recent writes");
  Check(report.outcome == MaintenanceOutcome::kIdleSkipped, "outcome kIdleSkipped");

  // force bypasses idle gate
  in.force = true;
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "force bypasses idle gate");
}

// ============================================================
// 6. Threshold gate (bytes)
// ============================================================

void TestThresholdBytesGate() {
  Log("=== TestThresholdBytesGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // Below both thresholds
  in.dead_payload_bytes = 100;                   // < 1024
  in.total_payload_bytes = 10'000;               // fraction = 0.01 < 0.20
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(!ok, "below both thresholds rejects");
  Check(report.outcome == MaintenanceOutcome::kBelowThreshold, "outcome kBelowThreshold");

  // Meets bytes threshold but not fraction
  in.dead_payload_bytes = 2000;                  // > 1024
  in.total_payload_bytes = 100'000;              // fraction = 0.02 < 0.20
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "meets bytes threshold alone → eligible");
}

// ============================================================
// 7. Threshold gate (fraction)
// ============================================================

void TestThresholdFractionGate() {
  Log("=== TestThresholdFractionGate ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);

  // Meets fraction but not bytes
  in.dead_payload_bytes = 500;                   // < 1024
  in.total_payload_bytes = 1000;                 // fraction = 0.50 >= 0.20
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "meets fraction threshold alone → eligible");
}

// ============================================================
// 8. Edge: cadence == 1 (every flush)
// ============================================================

void TestCadenceEveryFlush() {
  Log("=== TestCadenceEveryFlush ===");
  auto sched = DefaultSchedule();
  sched.check_every_flushes = 1;
  auto in = DefaultInput(sched);
  in.flush_count = 7;  // any non-zero flush is a multiple of 1
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "cadence=1 allows every flush");
}

// ============================================================
// 9. Edge: zero total_payload_bytes (avoid div-by-zero)
// ============================================================

void TestZeroTotalPayload() {
  Log("=== TestZeroTotalPayload ===");
  auto sched = DefaultSchedule();
  sched.min_dead_payload_bytes = 0;     // allow zero
  sched.min_dead_payload_fraction = 0.0; // allow zero fraction
  auto in = DefaultInput(sched);
  in.dead_payload_bytes = 0;
  in.total_payload_bytes = 0;
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  // fraction is 0.0, meets_fraction is 0.0 >= 0.0 = true
  // meets_bytes is 0 >= 0 = true
  Check(ok, "zero totals with zero thresholds → eligible");
  Check(report.dead_payload_fraction == 0.0, "fraction is 0 with zero totals");
}

// ============================================================
// 10. Edge: last_completed_ms == 0 (never run before)
// ============================================================

void TestNeverRunBefore() {
  Log("=== TestNeverRunBefore ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  in.last_completed_ms = 0;  // never completed
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "never run before → cooldown is skipped → eligible");
}

// ============================================================
// 11. Edge: last_write_activity_ms == 0 (never written)
// ============================================================

void TestNeverWritten() {
  Log("=== TestNeverWritten ===");
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  in.last_write_activity_ms = 0;  // never written
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "never written → idle gate skipped → eligible");
}

// ============================================================
// 12. Report notes contain useful info
// ============================================================

void TestReportNotes() {
  Log("=== TestReportNotes ===");

  // Cadence skip: notes should mention flush count and cadence
  auto sched = DefaultSchedule();
  auto in = DefaultInput(sched);
  in.flush_count = 5;
  ScheduledLiveSetMaintenanceReport report{};
  EvaluateMaintenanceGate(in, report);
  Check(!report.notes.empty(), "cadence skip notes non-empty");
  Check(report.notes.find("cadence") != std::string::npos, "cadence note mentions cadence");

  // Below threshold: notes should mention bytes/fraction
  in.flush_count = 4;
  in.dead_payload_bytes = 100;
  in.total_payload_bytes = 100'000;
  report = {};
  EvaluateMaintenanceGate(in, report);
  Check(report.notes.find("threshold") != std::string::npos ||
        report.notes.find("bytes") != std::string::npos,
        "threshold note mentions bytes or threshold");
}

// ============================================================
// 13. Fraction threshold clamped to [0, 1]
// ============================================================

void TestFractionClamping() {
  Log("=== TestFractionClamping ===");
  auto sched = DefaultSchedule();
  sched.min_dead_payload_fraction = 2.0;  // > 1.0, should be clamped to 1.0
  auto in = DefaultInput(sched);
  in.dead_payload_bytes = 8192;
  in.total_payload_bytes = 8192;  // fraction = 1.0
  // After clamping, meets_fraction = 1.0 >= 1.0 = true
  ScheduledLiveSetMaintenanceReport report{};
  bool ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "fraction > 1.0 clamped: 100% dead still passes");

  // Negative fraction clamped to 0.0
  sched.min_dead_payload_fraction = -1.0;
  in.dead_payload_bytes = 0;
  in.total_payload_bytes = 1000;  // fraction = 0.0
  // After clamping, meets_fraction = 0.0 >= 0.0 = true
  report = {};
  ok = EvaluateMaintenanceGate(in, report);
  Check(ok, "negative fraction clamped to 0 → zero dead is still ok");
}

// ── Helpers for store-based integration tests ──────────────

std::filesystem::path UniquePath(const std::string& label) {
  const auto now =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_lsr_" + label + "_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

std::vector<std::byte> StringToBytes(const std::string& text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

struct AutoClean {
  std::vector<std::filesystem::path> paths;
  ~AutoClean() {
    for (const auto& p : paths) {
      std::error_code ec;
      std::filesystem::remove(p, ec);
    }
  }
};

// ============================================================
// 14. RewriteLiveSet: basic round-trip
// ============================================================

void TestRewriteBasic() {
  Log("=== TestRewriteBasic ===");
  auto src_path = UniquePath("rw_basic_src");
  auto dst_path = UniquePath("rw_basic_dst");
  AutoClean cleaner{{src_path, dst_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("frame zero"), {});
  store.Put(StringToBytes("frame one"), {});
  store.Commit();

  auto report = RewriteLiveSet(store, dst_path);
  Check(report.frame_count == 2, "frame_count == 2");
  Check(report.active_frame_count == 2, "all frames are active/live");
  Check(report.dropped_payload_frames == 0, "no dropped payloads (all live)");
  Check(report.deleted_frame_count == 0, "no deleted frames");
  Check(report.logical_bytes_before > 0, "logical_bytes_before > 0");
  Check(report.logical_bytes_after > 0, "logical_bytes_after > 0");
  Check(report.duration_ms >= 0.0, "duration non-negative");
  Check(report.destination_path == dst_path.string(), "destination_path matches");

  // Verify destination has same frame count.
  auto dest = WaxStore::Open(dst_path);
  Check(dest.Stats().frame_count == 2, "dest frame_count == 2");
  dest.Close();
  store.Close();
}

// ============================================================
// 15. RewriteLiveSet: drops deleted frame payloads
// ============================================================

void TestRewriteDropsDeleted() {
  Log("=== TestRewriteDropsDeleted ===");
  auto src_path = UniquePath("rw_del_src");
  auto dst_path = UniquePath("rw_del_dst");
  AutoClean cleaner{{src_path, dst_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("keep this"), {});
  store.Put(StringToBytes("delete this large payload that uses space"), {});
  store.Delete(1);
  store.Commit();

  auto report = RewriteLiveSet(store, dst_path);
  Check(report.frame_count == 2, "total frames == 2");
  Check(report.active_frame_count == 1, "1 active live frame");
  Check(report.deleted_frame_count == 1, "1 deleted frame");
  Check(report.dropped_payload_frames >= 1, "at least 1 payload dropped");
  Check(report.logical_bytes_after < report.logical_bytes_before,
        "dest has fewer logical bytes than source");

  store.Close();
}

// ============================================================
// 16. RewriteLiveSet: drops superseded frame payloads
// ============================================================

void TestRewriteDropsSuperseded() {
  Log("=== TestRewriteDropsSuperseded ===");
  auto src_path = UniquePath("rw_sup_src");
  auto dst_path = UniquePath("rw_sup_dst");
  AutoClean cleaner{{src_path, dst_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("original version of content"), {});
  store.Put(StringToBytes("new version"), {});
  store.Supersede(0, 1);  // frame 0 superseded by frame 1
  store.Commit();

  auto report = RewriteLiveSet(store, dst_path);
  Check(report.frame_count == 2, "total frames == 2");
  Check(report.superseded_frame_count == 1, "1 superseded frame");
  Check(report.dropped_payload_frames >= 1, "at least 1 payload dropped");

  store.Close();
}

// ============================================================
// 17. RewriteLiveSet: same path throws
// ============================================================

void TestRewriteSamePathThrows() {
  Log("=== TestRewriteSamePathThrows ===");
  auto path = UniquePath("rw_same");
  AutoClean cleaner{{path}};

  auto store = WaxStore::Create(path);
  store.Put(StringToBytes("data"), {});
  store.Commit();

  bool threw = false;
  try {
    RewriteLiveSet(store, path);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Check(threw, "same path throws runtime_error");
  store.Close();
}

// ============================================================
// 18. RewriteLiveSet: destination exists without overwrite throws
// ============================================================

void TestRewriteDestExistsThrows() {
  Log("=== TestRewriteDestExistsThrows ===");
  auto src_path = UniquePath("rw_exist_src");
  auto dst_path = UniquePath("rw_exist_dst");
  AutoClean cleaner{{src_path, dst_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("data"), {});
  store.Commit();

  // Create destination file.
  auto blocker = WaxStore::Create(dst_path);
  blocker.Commit();
  blocker.Close();

  bool threw = false;
  try {
    LiveSetRewriteOptions opts;
    opts.overwrite_destination = false;
    RewriteLiveSet(store, dst_path, opts);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Check(threw, "existing dest without overwrite throws");
  store.Close();
}

// ============================================================
// 19. RewriteLiveSet: deep verify
// ============================================================

void TestRewriteDeepVerify() {
  Log("=== TestRewriteDeepVerify ===");
  auto src_path = UniquePath("rw_verify_src");
  auto dst_path = UniquePath("rw_verify_dst");
  AutoClean cleaner{{src_path, dst_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("hello"), {});
  store.Commit();

  LiveSetRewriteOptions opts;
  opts.verify_deep = true;
  auto report = RewriteLiveSet(store, dst_path, opts);
  Check(report.frame_count == 1, "verify: frame_count == 1");
  store.Close();
}

// ============================================================
// 20. RunScheduledLiveSetMaintenance: full flow success
// ============================================================

void TestScheduledMaintenanceSuccess() {
  Log("=== TestScheduledMaintenanceSuccess ===");
  auto src_path = UniquePath("sched_ok_src");
  AutoClean cleaner{{src_path}};

  auto store = WaxStore::Create(src_path);
  // Write some frames, delete one to create dead payload.
  store.Put(StringToBytes("live content that should remain"), {});
  store.Put(StringToBytes("dead content that should be dropped after delete"), {});
  store.Delete(1);
  store.Commit();

  LiveSetRewriteSchedule sched;
  sched.enabled = true;
  sched.check_every_flushes = 1;
  sched.min_dead_payload_bytes = 0;
  sched.min_dead_payload_fraction = 0.0;
  sched.minimum_idle_ms = 0;
  sched.min_interval_ms = 0;
  sched.minimum_compaction_gain_bytes = 0;
  sched.destination_directory = std::filesystem::temp_directory_path().string();

  auto report = RunScheduledLiveSetMaintenance(
      store, sched,
      /*flush_count=*/1,
      /*force=*/false,
      /*now_ms=*/100'000,
      /*last_completed_ms=*/0,
      /*last_write_activity_ms=*/0);

  Check(report.outcome == MaintenanceOutcome::kRewriteSucceeded,
        "outcome kRewriteSucceeded");
  Check(report.rewrite_report.has_value(), "rewrite_report present");
  Check(!report.candidate_path.empty(), "candidate_path non-empty");
  Check(!report.rollback_performed, "no rollback performed");

  // Clean up candidate.
  if (!report.candidate_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(report.candidate_path, ec);
  }

  store.Close();
}

// ============================================================
// 21. RunScheduledLiveSetMaintenance: gate rejects
// ============================================================

void TestScheduledMaintenanceGateRejects() {
  Log("=== TestScheduledMaintenanceGateRejects ===");
  auto src_path = UniquePath("sched_rej_src");
  AutoClean cleaner{{src_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("data"), {});
  store.Commit();

  LiveSetRewriteSchedule sched;
  sched.enabled = true;
  sched.check_every_flushes = 10;  // cadence gate will reject flush_count=1

  auto report = RunScheduledLiveSetMaintenance(
      store, sched,
      /*flush_count=*/1,
      /*force=*/false,
      /*now_ms=*/100'000,
      /*last_completed_ms=*/0,
      /*last_write_activity_ms=*/0);

  Check(report.outcome == MaintenanceOutcome::kCadenceSkipped,
        "cadence gate rejects");
  Check(!report.rewrite_report.has_value(), "no rewrite performed");

  store.Close();
}

// ============================================================
// 22. RunScheduledLiveSetMaintenance: disabled schedule
// ============================================================

void TestScheduledMaintenanceDisabled() {
  Log("=== TestScheduledMaintenanceDisabled ===");
  auto src_path = UniquePath("sched_dis_src");
  AutoClean cleaner{{src_path}};

  auto store = WaxStore::Create(src_path);
  store.Put(StringToBytes("data"), {});
  store.Commit();

  LiveSetRewriteSchedule sched;
  sched.enabled = false;

  auto report = RunScheduledLiveSetMaintenance(
      store, sched, 1, false, 100'000, 0, 0);

  Check(report.outcome == MaintenanceOutcome::kDisabled,
        "disabled schedule → kDisabled");
  store.Close();
}

// ============================================================
// 23. RunScheduledLiveSetMaintenance: validation rejects low gain
// ============================================================

void TestScheduledMaintenanceValidationRejects() {
  Log("=== TestScheduledMaintenanceValidationRejects ===");
  auto src_path = UniquePath("sched_val_src");
  AutoClean cleaner{{src_path}};

  auto store = WaxStore::Create(src_path);
  // Only live frames → zero compaction gain.
  store.Put(StringToBytes("live content"), {});
  store.Commit();

  LiveSetRewriteSchedule sched;
  sched.enabled = true;
  sched.check_every_flushes = 1;
  sched.min_dead_payload_bytes = 0;
  sched.min_dead_payload_fraction = 0.0;
  sched.minimum_idle_ms = 0;
  sched.min_interval_ms = 0;
  sched.minimum_compaction_gain_bytes = 999'999'999;  // impossible to meet
  sched.destination_directory = std::filesystem::temp_directory_path().string();

  auto report = RunScheduledLiveSetMaintenance(
      store, sched, 1, false, 100'000, 0, 0);

  Check(report.outcome == MaintenanceOutcome::kValidationFailedRolledBack,
        "validation rejects low gain");
  Check(report.rollback_performed, "rollback performed");
  Check(report.rewrite_report.has_value(), "rewrite was attempted");

  store.Close();
}

}  // namespace

int main() {
  Log("== live_set_rewrite_test ==");

  // Gate-only unit tests
  TestDisabled();
  TestAllGatesPass();
  TestCadenceGate();
  TestCooldownGate();
  TestIdleGate();
  TestThresholdBytesGate();
  TestThresholdFractionGate();
  TestCadenceEveryFlush();
  TestZeroTotalPayload();
  TestNeverRunBefore();
  TestNeverWritten();
  TestReportNotes();
  TestFractionClamping();

  // Store-based integration tests
  TestRewriteBasic();
  TestRewriteDropsDeleted();
  TestRewriteDropsSuperseded();
  TestRewriteSamePathThrows();
  TestRewriteDestExistsThrows();
  TestRewriteDeepVerify();
  TestScheduledMaintenanceSuccess();
  TestScheduledMaintenanceGateRejects();
  TestScheduledMaintenanceDisabled();
  TestScheduledMaintenanceValidationRejects();

  std::cout << "\n[live_set_rewrite_test] " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail > 0 ? 1 : 0;
}
