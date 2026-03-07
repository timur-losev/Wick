#include "waxcpp/maintenance.hpp"
#include "waxcpp/wax_store.hpp"

#include "../test_logger.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path UniquePath() {
  const auto now =
      std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_maintenance_test_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

std::vector<std::byte> StringToBytes(const std::string& text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const auto b : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return text;
}

// ---- Scenarios ----

void ScenarioEmptyStore() {
  waxcpp::tests::Log("scenario: maintenance on empty store");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  Require(report.scanned_frames == 0, "empty store: scanned should be 0");
  Require(report.eligible_frames == 0, "empty store: eligible should be 0");
  Require(report.generated_surrogates == 0, "empty store: generated should be 0");
  Require(!report.did_timeout, "empty store: should not timeout");

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioSingleFrame() {
  waxcpp::tests::Log("scenario: maintenance on single frame");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  // Insert a content frame.
  std::string content =
      "The quick brown fox jumps over the lazy dog. "
      "Pack my box with five dozen liquor jugs. "
      "How vexingly quick daft zebras jump.";
  store.Put(StringToBytes(content));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  Require(report.scanned_frames >= 1, "single: scanned should be >= 1");
  Require(report.eligible_frames >= 1, "single: eligible should be >= 1, got " +
              std::to_string(report.eligible_frames));
  Require(report.generated_surrogates >= 1, "single: generated should be >= 1, got " +
              std::to_string(report.generated_surrogates));
  Require(!report.did_timeout, "single: should not timeout");

  // Verify a new frame was added (the surrogate).
  auto stats = store.Stats();
  Require(stats.frame_count >= 2, "single: store should have >= 2 frames after maintenance, got " +
              std::to_string(stats.frame_count));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioMultipleFrames() {
  waxcpp::tests::Log("scenario: maintenance on multiple frames");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  // Insert several content frames.
  std::vector<std::string> contents = {
      "Machine learning models require large datasets for training. "
      "The neural network architecture includes convolutional layers.",
      "Climate change is accelerating due to greenhouse gas emissions. "
      "Rising sea levels threaten coastal communities worldwide.",
      "The database query optimizer selects execution plans based on statistics. "
      "Index structures improve lookup performance significantly.",
  };
  for (const auto& c : contents) {
    store.Put(StringToBytes(c));
  }
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  Require(report.scanned_frames == 3, "multi: scanned should be 3, got " +
              std::to_string(report.scanned_frames));
  Require(report.eligible_frames == 3, "multi: eligible should be 3, got " +
              std::to_string(report.eligible_frames));
  Require(report.generated_surrogates == 3, "multi: generated should be 3, got " +
              std::to_string(report.generated_surrogates));

  // Store should now have 6 frames (3 source + 3 surrogate).
  auto stats = store.Stats();
  Require(stats.frame_count == 6, "multi: store should have 6 frames, got " +
              std::to_string(stats.frame_count));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioMaxFramesLimit() {
  waxcpp::tests::Log("scenario: maintenance respects max_frames limit");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  for (int i = 0; i < 5; ++i) {
    store.Put(StringToBytes(
        "Content frame number " + std::to_string(i) +
        " with enough text to generate a meaningful surrogate. "
        "The quick brown fox jumps over the lazy dog."));
  }
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.max_frames = 2;

  auto report = waxcpp::OptimizeSurrogates(store, gen, opts);

  Require(report.eligible_frames <= 2, "max_frames: eligible should be <= 2, got " +
              std::to_string(report.eligible_frames));
  Require(report.generated_surrogates <= 2, "max_frames: generated should be <= 2, got " +
              std::to_string(report.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioWallTimeDeadline() {
  waxcpp::tests::Log("scenario: maintenance respects wall time deadline");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  // Insert many frames to make the maintenance take measurable time.
  for (int i = 0; i < 20; ++i) {
    std::string content;
    for (int j = 0; j < 10; ++j) {
      content += "Sentence " + std::to_string(j) +
                 " of frame " + std::to_string(i) +
                 " contains some interesting content. ";
    }
    store.Put(StringToBytes(content));
  }
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.max_wall_time_ms = 0;  // Immediate deadline.

  auto report = waxcpp::OptimizeSurrogates(store, gen, opts);

  // With zero deadline, either nothing was processed or it timed out.
  // The timeout check happens at the top of each iteration, so the first
  // frame might still be processed before the check fires.
  Require(report.did_timeout || report.generated_surrogates <= 1,
          "zero deadline: should timeout or process at most 1, generated " +
              std::to_string(report.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioDeletedFramesSkipped() {
  waxcpp::tests::Log("scenario: maintenance skips deleted frames");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  auto id1 = store.Put(StringToBytes(
      "First frame with enough text for surrogate generation. "
      "The quick brown fox jumps over the lazy dog."));
  auto id2 = store.Put(StringToBytes(
      "Second frame with enough text for surrogate generation. "
      "Pack my box with five dozen liquor jugs."));
  store.Commit();

  // Delete the first frame.
  store.Delete(id1);
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  // Only the second frame should be eligible (first is deleted).
  Require(report.eligible_frames == 1, "deleted: eligible should be 1 (not deleted frame), got " +
              std::to_string(report.eligible_frames));
  Require(report.generated_surrogates == 1, "deleted: generated should be 1, got " +
              std::to_string(report.generated_surrogates));

  (void)id2;  // Suppress unused warning.
  store.Close();
  std::filesystem::remove(path);
}

void ScenarioSupersededFramesSkipped() {
  waxcpp::tests::Log("scenario: maintenance skips superseded frames");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  auto id_old = store.Put(StringToBytes(
      "Old version of this document with plenty of text. "
      "The quick brown fox jumps over the lazy dog."));
  auto id_new = store.Put(StringToBytes(
      "New version of this document with updated text. "
      "Pack my box with five dozen liquor jugs."));
  store.Commit();

  // Supersede old with new.
  store.Supersede(id_old, id_new);
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  // Only the new frame should be eligible (old is superseded).
  Require(report.eligible_frames == 1, "superseded: eligible should be 1, got " +
              std::to_string(report.eligible_frames));
  Require(report.generated_surrogates == 1, "superseded: generated should be 1, got " +
              std::to_string(report.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioEmptyPayloadSkipped() {
  waxcpp::tests::Log("scenario: maintenance skips empty payloads");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  // Insert a frame with actual content.
  store.Put(StringToBytes(
      "Real content with enough text for surrogate generation. "
      "The quick brown fox jumps over the lazy dog."));
  // Insert a frame with whitespace-only content.
  store.Put(StringToBytes("   \n\t  "));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  // Only the real content frame should be eligible.
  Require(report.eligible_frames == 1, "empty payload: eligible should be 1, got " +
              std::to_string(report.eligible_frames));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioHierarchicalTiers() {
  waxcpp::tests::Log("scenario: maintenance generates hierarchical tiers");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "The database optimizer analyzes query plans and selects the most "
      "efficient execution strategy based on table statistics and index "
      "availability. B-tree indexes provide logarithmic lookup time. "
      "Hash indexes are optimized for equality comparisons."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.enable_hierarchical = true;
  opts.tier_config = waxcpp::SurrogateTierConfig::Default();

  auto report = waxcpp::OptimizeSurrogates(store, gen, opts);

  Require(report.generated_surrogates == 1, "hierarchical: generated should be 1, got " +
              std::to_string(report.generated_surrogates));

  // Read the surrogate frame and verify it has the hierarchical format header.
  auto all_frames = store.FrameMetas();
  Require(all_frames.size() >= 2, "hierarchical: should have >= 2 frames");

  // The surrogate is the last frame (highest ID).
  std::uint64_t max_id = 0;
  for (const auto& f : all_frames) {
    if (f.id > max_id) max_id = f.id;
  }
  auto surrogate_bytes = store.FrameContent(max_id);
  auto surrogate_str = BytesToString(surrogate_bytes);

  Require(surrogate_str.substr(0, 8) == "WAXSURR1",
          "hierarchical: surrogate should start with WAXSURR1 magic");
  Require(surrogate_str.find("FULL:") != std::string::npos,
          "hierarchical: surrogate should contain FULL: tier");
  Require(surrogate_str.find("GIST:") != std::string::npos,
          "hierarchical: surrogate should contain GIST: tier");
  Require(surrogate_str.find("MICRO:") != std::string::npos,
          "hierarchical: surrogate should contain MICRO: tier");

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioNonHierarchicalFallback() {
  waxcpp::tests::Log("scenario: maintenance with hierarchical disabled");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "Single-tier surrogate generation produces a flat text summary. "
      "The neural network uses backpropagation for weight updates. "
      "Gradient descent optimizes the loss function iteratively."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.enable_hierarchical = false;
  opts.surrogate_max_tokens = 30;

  auto report = waxcpp::OptimizeSurrogates(store, gen, opts);

  Require(report.generated_surrogates == 1, "non-hier: generated should be 1, got " +
              std::to_string(report.generated_surrogates));

  // Verify the surrogate does NOT have the hierarchical header.
  auto all_frames = store.FrameMetas();
  std::uint64_t max_id = 0;
  for (const auto& f : all_frames) {
    if (f.id > max_id) max_id = f.id;
  }
  auto surrogate_bytes = store.FrameContent(max_id);
  auto surrogate_str = BytesToString(surrogate_bytes);

  Require(surrogate_str.substr(0, 8) != "WAXSURR1",
          "non-hier: surrogate should NOT start with WAXSURR1 magic");
  Require(!surrogate_str.empty(), "non-hier: surrogate should not be empty");

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioSkipExistingSurrogates() {
  waxcpp::tests::Log("scenario: maintenance re-run skips already generated surrogates");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "Document about machine learning fundamentals and neural networks. "
      "Deep learning architectures include CNNs and transformers."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;

  // First run: should generate surrogates.
  auto report1 = waxcpp::OptimizeSurrogates(store, gen);
  Require(report1.generated_surrogates == 1, "first run: should generate 1, got " +
              std::to_string(report1.generated_surrogates));

  // On second run, the source frame is now superseded by its surrogate via
  // the supersede chain, so it is skipped entirely. No new surrogates generated.
  auto report2 = waxcpp::OptimizeSurrogates(store, gen);

  Require(report2.generated_surrogates == 0, "second run: should generate 0 (source superseded), got " +
              std::to_string(report2.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioOverwriteExisting() {
  waxcpp::tests::Log("scenario: maintenance overwrite existing surrogates");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "Important technical document about system architecture. "
      "Microservices communicate via message queues and REST APIs."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.overwrite_existing = true;

  // First run.
  auto report1 = waxcpp::OptimizeSurrogates(store, gen, opts);
  Require(report1.generated_surrogates == 1, "overwrite first: generated 1, got " +
              std::to_string(report1.generated_surrogates));

  // Second run with overwrite = true: should still generate.
  auto report2 = waxcpp::OptimizeSurrogates(store, gen, opts);
  Require(report2.generated_surrogates >= 1, "overwrite second: should re-generate, got " +
              std::to_string(report2.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioReportCounts() {
  waxcpp::tests::Log("scenario: maintenance report counts are consistent");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  for (int i = 0; i < 3; ++i) {
    store.Put(StringToBytes(
        "Frame " + std::to_string(i) + " with multi-sentence content. "
        "The quick brown fox jumps. The lazy dog sleeps."));
  }
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  // Consistency: generated + skipped <= eligible <= scanned.
  Require(report.generated_surrogates + report.skipped_up_to_date <= report.eligible_frames,
          "report: generated + skipped should be <= eligible");
  Require(report.eligible_frames <= report.scanned_frames,
          "report: eligible should be <= scanned");

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioCustomTierConfig() {
  waxcpp::tests::Log("scenario: maintenance with custom tier config");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "Compact tier configuration produces shorter surrogates. "
      "The encoder processes input sequences through attention layers. "
      "Multi-head attention enables parallel feature extraction. "
      "Layer normalization stabilizes training dynamics."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.enable_hierarchical = true;
  opts.tier_config = waxcpp::SurrogateTierConfig::Compact();

  auto report = waxcpp::OptimizeSurrogates(store, gen, opts);

  Require(report.generated_surrogates == 1, "custom tier: generated should be 1, got " +
              std::to_string(report.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioDefaultOptions() {
  waxcpp::tests::Log("scenario: maintenance with default options");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "Testing default maintenance options with hierarchical enabled. "
      "The API provides CRUD operations for resource management."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  // Use default-constructed options.
  auto report = waxcpp::OptimizeSurrogates(store, gen);

  Require(report.generated_surrogates == 1, "defaults: generated should be 1, got " +
              std::to_string(report.generated_surrogates));
  Require(report.eligible_frames == 1, "defaults: eligible should be 1, got " +
              std::to_string(report.eligible_frames));

  store.Close();
  std::filesystem::remove(path);
}

void ScenarioZeroMaxFrames() {
  waxcpp::tests::Log("scenario: maintenance with max_frames = 0");
  auto path = UniquePath();
  auto store = waxcpp::WaxStore::Create(path);

  store.Put(StringToBytes(
      "This content should not be processed due to zero limit. "
      "The quick brown fox jumps over the lazy dog."));
  store.Commit();

  waxcpp::ExtractiveSurrogateGenerator gen;
  waxcpp::MaintenanceOptions opts;
  opts.max_frames = 0;

  auto report = waxcpp::OptimizeSurrogates(store, gen, opts);

  Require(report.eligible_frames == 0, "zero max: eligible should be 0, got " +
              std::to_string(report.eligible_frames));
  Require(report.generated_surrogates == 0, "zero max: generated should be 0, got " +
              std::to_string(report.generated_surrogates));

  store.Close();
  std::filesystem::remove(path);
}

int RunScenarios() {
  int passed = 0;
  auto run = [&](auto fn) {
    fn();
    ++passed;
  };

  run(ScenarioEmptyStore);
  run(ScenarioSingleFrame);
  run(ScenarioMultipleFrames);
  run(ScenarioMaxFramesLimit);
  run(ScenarioWallTimeDeadline);
  run(ScenarioDeletedFramesSkipped);
  run(ScenarioSupersededFramesSkipped);
  run(ScenarioEmptyPayloadSkipped);
  run(ScenarioHierarchicalTiers);
  run(ScenarioNonHierarchicalFallback);
  run(ScenarioSkipExistingSurrogates);
  run(ScenarioOverwriteExisting);
  run(ScenarioReportCounts);
  run(ScenarioCustomTierConfig);
  run(ScenarioDefaultOptions);
  run(ScenarioZeroMaxFrames);

  return passed;
}

}  // namespace

int main() {
  try {
    const int passed = RunScenarios();
    waxcpp::tests::Log("maintenance_test: all " + std::to_string(passed) +
                        " scenarios passed");
    return 0;
  } catch (const std::exception& ex) {
    waxcpp::tests::Log(std::string("FAIL: ") + ex.what());
    return 1;
  }
}
