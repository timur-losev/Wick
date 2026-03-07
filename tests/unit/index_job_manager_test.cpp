#include "../../server/index_job_manager.hpp"
#include "../temp_artifacts.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path UniqueCheckpointPath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_index_job_manager_test_" + std::to_string(static_cast<long long>(now)) + ".checkpoint");
}

void ScenarioStartStatusStopRoundtrip(const std::filesystem::path& checkpoint_path) {
  waxcpp::server::IndexJobManager manager(checkpoint_path);
  const auto idle = manager.status();
  Require(idle.state == waxcpp::server::IndexJobState::kIdle, "initial state must be idle");
  Require(idle.phase == "idle", "initial phase must be idle");
  Require(idle.generation == 0, "initial generation must be zero");

  const auto started = manager.Start("g:/Proj/UnrealEngine/Engine/Source", false);
  Require(started, "start must succeed from idle");
  const auto running = manager.status();
  Require(running.state == waxcpp::server::IndexJobState::kRunning, "state must be running after start");
  Require(running.phase == "starting", "phase must be starting after start");
  Require(running.generation == 1, "generation must increment on start");
  Require(running.job_id.has_value() && !running.job_id->empty(), "running status must expose job_id");
  Require(running.repo_root.has_value() && *running.repo_root == "g:/Proj/UnrealEngine/Engine/Source",
          "repo_root must be persisted in status");
  Require(running.started_at_ms > 0 && running.updated_at_ms > 0, "timestamps must be set after start");

  const auto started_again = manager.Start("g:/Proj/UnrealEngine/Engine/Source", false);
  Require(!started_again, "second start while running must fail");

  const auto stopped = manager.Stop();
  Require(stopped, "stop must succeed while running");
  const auto stopped_status = manager.status();
  Require(stopped_status.state == waxcpp::server::IndexJobState::kStopped, "state must be stopped after stop");
  Require(stopped_status.phase == "stopped", "phase must be stopped after stop");

  const auto stopped_again = manager.Stop();
  Require(!stopped_again, "stop must fail when already stopped");
}

void ScenarioCheckpointReloadConvertsRunningToStopped(const std::filesystem::path& checkpoint_path) {
  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(manager.Start("g:/Proj/UE5/Engine/Source", false), "start must succeed");
    const auto running = manager.status();
    Require(running.state == waxcpp::server::IndexJobState::kRunning, "precondition: running state expected");
  }

  {
    waxcpp::server::IndexJobManager reloaded(checkpoint_path);
    const auto status = reloaded.status();
    Require(status.state == waxcpp::server::IndexJobState::kStopped,
            "reloaded manager must convert stale running state to stopped");
    Require(status.phase == "stopped", "reloaded stale running phase must become stopped");
    Require(status.generation > 0, "generation must survive checkpoint reload");
    Require(status.repo_root.has_value() && *status.repo_root == "g:/Proj/UE5/Engine/Source",
            "repo_root must survive checkpoint reload");
  }
}

void ScenarioResumeKeepsCountersForSameRepo(const std::filesystem::path& checkpoint_path) {
  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(manager.Start("g:/Proj/UE5/Engine/Source", false), "initial start must succeed");
    const auto stopped = manager.Stop();
    Require(stopped, "initial stop must succeed");
  }

  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(manager.Start("g:/Proj/UE5/Engine/Source", true), "resume start must succeed");
    const auto status = manager.status();
    Require(status.resume_requested, "resume flag must be reflected in status");
    Require(status.state == waxcpp::server::IndexJobState::kRunning, "state must be running on resumed start");
  }
}

void ScenarioFailTransitionsToFailed(const std::filesystem::path& checkpoint_path) {
  waxcpp::server::IndexJobManager manager(checkpoint_path);
  Require(manager.Start("g:/Proj/UE5/Engine/Source", false), "start must succeed");
  Require(manager.Fail("simulated failure"), "fail must succeed while running");
  const auto status = manager.status();
  Require(status.state == waxcpp::server::IndexJobState::kFailed, "state must be failed after Fail()");
  Require(status.phase == "failed", "phase must be failed after Fail()");
  Require(status.last_error.has_value() && *status.last_error == "simulated failure",
          "last_error must be persisted");
}

void ScenarioCompletePersistsCounters(const std::filesystem::path& checkpoint_path) {
  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(manager.Start("g:/Proj/UE5/Engine/Source", false), "start must succeed");
    Require(manager.Complete(321, 654, 600), "complete must succeed while running");
    const auto status = manager.status();
    Require(status.state == waxcpp::server::IndexJobState::kStopped, "complete must move state to stopped");
    Require(status.phase == "completed", "complete must set completed phase");
    Require(status.scanned_files == 321, "complete must persist scanned_files");
    Require(status.indexed_chunks == 654, "complete must persist indexed_chunks");
    Require(status.committed_chunks == 600, "complete must persist committed_chunks");
  }

  {
    waxcpp::server::IndexJobManager reloaded(checkpoint_path);
    const auto status = reloaded.status();
    Require(status.state == waxcpp::server::IndexJobState::kStopped,
            "reloaded state must stay stopped after complete");
    Require(status.phase == "completed", "reloaded phase must stay completed");
    Require(status.scanned_files == 321, "reloaded scanned_files mismatch");
    Require(status.indexed_chunks == 654, "reloaded indexed_chunks mismatch");
    Require(status.committed_chunks == 600, "reloaded committed_chunks mismatch");
  }
}

void ScenarioUpdateProgressPersistsWhileRunning(const std::filesystem::path& checkpoint_path) {
  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(manager.Start("g:/Proj/UE5/Engine/Source", false), "start must succeed");
    Require(manager.UpdateProgress(100, 80, 64), "update progress must succeed while running");
    const auto status = manager.status();
    Require(status.state == waxcpp::server::IndexJobState::kRunning, "update progress must keep running state");
    Require(status.scanned_files == 100, "update progress scanned_files mismatch");
    Require(status.indexed_chunks == 80, "update progress indexed_chunks mismatch");
    Require(status.committed_chunks == 64, "update progress committed_chunks mismatch");
  }

  {
    waxcpp::server::IndexJobManager reloaded(checkpoint_path);
    const auto status = reloaded.status();
    Require(status.state == waxcpp::server::IndexJobState::kStopped,
            "running checkpoint must reload as stopped");
    Require(status.scanned_files == 100, "reloaded scanned_files mismatch after update progress");
    Require(status.indexed_chunks == 80, "reloaded indexed_chunks mismatch after update progress");
    Require(status.committed_chunks == 64, "reloaded committed_chunks mismatch after update progress");
  }

  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(!manager.UpdateProgress(1, 1, 1), "update progress must fail when not running");
  }
}

void ScenarioSetPhaseRoundtrip(const std::filesystem::path& checkpoint_path) {
  {
    waxcpp::server::IndexJobManager manager(checkpoint_path);
    Require(manager.Start("g:/Proj/UE5/Engine/Source", false), "start must succeed");
    Require(manager.SetPhase("scanning"), "set phase must succeed while running");
    const auto status = manager.status();
    Require(status.phase == "scanning", "set phase must update running status");
  }
  {
    waxcpp::server::IndexJobManager reloaded(checkpoint_path);
    const auto status = reloaded.status();
    Require(status.phase == "stopped", "stale-running reload must normalize phase to stopped");
  }
}

}  // namespace

int main() {
  const auto checkpoint_path = UniqueCheckpointPath();
  try {
    ScenarioStartStatusStopRoundtrip(checkpoint_path);
    ScenarioCheckpointReloadConvertsRunningToStopped(checkpoint_path);
    ScenarioResumeKeepsCountersForSameRepo(checkpoint_path);
    ScenarioFailTransitionsToFailed(checkpoint_path);
    ScenarioCompletePersistsCounters(checkpoint_path);
    ScenarioUpdateProgressPersistsWhileRunning(checkpoint_path);
    ScenarioSetPhaseRoundtrip(checkpoint_path);
    waxcpp::tests::CleanupStoreArtifacts(checkpoint_path);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_index_job_manager_test_");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::CleanupStoreArtifacts(checkpoint_path);
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_index_job_manager_test_");
    std::cerr << "index_job_manager_test failed: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
