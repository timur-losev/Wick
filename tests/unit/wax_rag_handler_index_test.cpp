#include "../../server/wax_rag_handler.hpp"

#include "../temp_artifacts.hpp"
#include "../test_env_guard.hpp"
#include "../test_logger.hpp"
#include "../test_temp_dir.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Poco::JSON::Object::Ptr ParseObject(const std::string& json) {
  Poco::JSON::Parser parser;
  const Poco::Dynamic::Var parsed = parser.parse(json);
  auto object = parsed.extract<Poco::JSON::Object::Ptr>();
  if (object.isNull()) {
    throw std::runtime_error("expected JSON object");
  }
  return object;
}

std::string TempName(const std::string& prefix, const std::string& suffix) {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::ostringstream out;
  out << prefix << static_cast<long long>(now) << suffix;
  return out.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& body) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    throw std::runtime_error("failed to create parent path for test file: " + path.string());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open test file for write: " + path.string());
  }
  out << body;
  if (!out) {
    throw std::runtime_error("failed to write test file: " + path.string());
  }
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for read: " + path.string());
  }
  std::ostringstream out;
  out << in.rdbuf();
  if (!in.good() && !in.eof()) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  return out.str();
}

std::string MakeLargeCppBody(int lines) {
  std::ostringstream out;
  out << "#include <cstdint>\n\n";
  for (int i = 0; i < lines; ++i) {
    out << "int f_" << i << "() { return " << i << "; }\n";
  }
  return out.str();
}

waxcpp::RuntimeModelsConfig MakeRuntimeConfigForTests(const std::filesystem::path& runtime_root) {
  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = runtime_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;
  return models;
}

using waxcpp::tests::EnvVarGuard;
using waxcpp::tests::SetEnvVar;

struct IndexStatusView {
  std::string state{};
  std::uint64_t indexed_chunks = 0;
  std::uint64_t committed_chunks = 0;
  std::uint64_t scanned_files = 0;
};

IndexStatusView WaitForTerminalState(waxcpp::server::WaxRAGHandler& handler, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  IndexStatusView view{};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status_raw = handler.handle_index_status(Poco::JSON::Object::Ptr{});
    Require(status_raw.rfind("Error:", 0) != 0, "index.status must not fail");
    const auto status_json = ParseObject(status_raw);
    view.state = status_json->optValue<std::string>("state", "");
    view.indexed_chunks = status_json->optValue<std::uint64_t>("indexed_chunks", 0);
    view.committed_chunks = status_json->optValue<std::uint64_t>("committed_chunks", 0);
    view.scanned_files = status_json->optValue<std::uint64_t>("scanned_files", 0);
    if (view.state != "running") {
      return view;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  throw std::runtime_error("index job did not reach terminal state before timeout");
}

void WaitForRunningState(waxcpp::server::WaxRAGHandler& handler, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status_raw = handler.handle_index_status(Poco::JSON::Object::Ptr{});
    Require(status_raw.rfind("Error:", 0) != 0, "index.status must not fail");
    const auto status_json = ParseObject(status_raw);
    if (status_json->optValue<std::string>("state", "") == "running") {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("index job did not enter running state before timeout");
}

IndexStatusView WaitForCommittedChunksAtLeast(waxcpp::server::WaxRAGHandler& handler,
                                              std::uint64_t committed_target,
                                              int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  IndexStatusView view{};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto status_raw = handler.handle_index_status(Poco::JSON::Object::Ptr{});
    Require(status_raw.rfind("Error:", 0) != 0, "index.status must not fail");
    const auto status_json = ParseObject(status_raw);
    view.state = status_json->optValue<std::string>("state", "");
    view.indexed_chunks = status_json->optValue<std::uint64_t>("indexed_chunks", 0);
    view.committed_chunks = status_json->optValue<std::uint64_t>("committed_chunks", 0);
    view.scanned_files = status_json->optValue<std::uint64_t>("scanned_files", 0);
    if (view.committed_chunks >= committed_target) {
      return view;
    }
    if (view.state != "running") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  throw std::runtime_error("index job did not reach committed chunk target before timeout");
}

void ScenarioIndexStartIsAsyncAndStopWorks() {
  waxcpp::tests::Log("scenario: index.start returns promptly and stop cancels running job");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  for (int i = 0; i < 40; ++i) {
    WriteTextFile(temp_root / ("File" + std::to_string(i) + ".cpp"), MakeLargeCppBody(1200));
  }

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);

  const auto start_ts = std::chrono::steady_clock::now();
  const auto start_raw = handler.handle_index_start(start_params);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_ts);

  Require(start_raw.rfind("Error:", 0) != 0, "index.start must not fail");
  Require(elapsed_ms.count() < 1500, "index.start must be asynchronous and return quickly");

  const auto start_json = ParseObject(start_raw);
  Require(start_json->optValue<std::string>("state", "") == "running",
          "index.start must report running state");

  const auto stop_raw = handler.handle_index_stop(Poco::JSON::Object::Ptr{});
  Require(stop_raw.rfind("Error:", 0) != 0, "index.stop must not fail when worker is running");
  const auto stop_json = ParseObject(stop_raw);
  Require(stop_json->optValue<std::string>("state", "") == "stopped", "index.stop must report stopped state");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
}

void ScenarioIndexCompleteWritesManifests() {
  waxcpp::tests::Log("scenario: finished index job persists manifests");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "A.cpp", MakeLargeCppBody(60));
  WriteTextFile(temp_root / "B.h", "struct B { int v = 7; };");

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);
  const auto start_raw = handler.handle_index_start(start_params);
  Require(start_raw.rfind("Error:", 0) != 0, "index.start must not fail");

  const auto view = WaitForTerminalState(handler, 20000);
  Require(view.state == "stopped", "index job must eventually complete");
  Require(view.indexed_chunks > 0, "index job must ingest at least one chunk");
  const auto status_raw = handler.handle_index_status(Poco::JSON::Object::Ptr{});
  Require(status_raw.rfind("Error:", 0) != 0, "index.status must not fail");
  const auto status_json = ParseObject(status_raw);
  Require(status_json->has("elapsed_ms"), "index.status must expose elapsed_ms");
  Require(status_json->has("indexed_chunks_per_sec"), "index.status must expose indexed_chunks_per_sec");
  Require(status_json->has("committed_chunks_per_sec"), "index.status must expose committed_chunks_per_sec");
  Require(status_json->has("process_rss_mb"), "index.status must expose process_rss_mb");
  Require(status_json->optValue<double>("indexed_chunks_per_sec", -1.0) >= 0.0,
          "indexed_chunks_per_sec must be non-negative");
  Require(status_json->optValue<double>("committed_chunks_per_sec", -1.0) >= 0.0,
          "committed_chunks_per_sec must be non-negative");
  Require(status_json->optValue<double>("process_rss_mb", -2.0) >= -1.0,
          "process_rss_mb must be either non-negative or -1 when probe unavailable");
  Require(std::filesystem::exists(scan_manifest), "scan manifest must exist");
  Require(std::filesystem::exists(chunk_manifest), "chunk manifest must exist");
  Require(std::filesystem::exists(file_manifest), "file manifest must exist");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioResumeSkipsUnchangedFilesThenIndexesChangedFile() {
  waxcpp::tests::Log("scenario: resume skips unchanged files and indexes changed file");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "One.cpp", MakeLargeCppBody(120));
  WriteTextFile(temp_root / "Two.cpp", MakeLargeCppBody(100));

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  auto start_with_resume = [&](bool resume) {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("resume", resume);
    const auto raw = handler.handle_index_start(params);
    Require(raw.rfind("Error:", 0) != 0, "index.start must not fail");
  };

  start_with_resume(false);
  const auto first = WaitForTerminalState(handler, 20000);
  Require(first.state == "stopped", "initial index run must stop");
  Require(first.indexed_chunks > 0, "initial index run must ingest chunks");

  start_with_resume(true);
  const auto second = WaitForTerminalState(handler, 20000);
  Require(second.state == "stopped", "resume run must stop");
  Require(second.indexed_chunks == 0, "resume run over unchanged files must ingest zero chunks");

  WriteTextFile(temp_root / "Two.cpp", MakeLargeCppBody(140));
  start_with_resume(true);
  const auto third = WaitForTerminalState(handler, 20000);
  Require(third.state == "stopped", "resume run with changed file must stop");
  Require(third.indexed_chunks > 0, "resume run with changed file must ingest chunks");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioInterruptedIndexResumesAfterHandlerRecreate() {
  waxcpp::tests::Log("scenario: interrupted index resumes after handler recreate");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  for (int i = 0; i < 24; ++i) {
    WriteTextFile(temp_root / ("R" + std::to_string(i) + ".cpp"), MakeLargeCppBody(450));
  }

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);

  {
    waxcpp::server::WaxRAGHandler first_handler(store_path, models);
    Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
    start_params->set("repo_root", temp_root.string());
    start_params->set("resume", false);
    const auto start_raw = first_handler.handle_index_start(start_params);
    Require(start_raw.rfind("Error:", 0) != 0, "first index.start must not fail");

    WaitForRunningState(first_handler, 2000);
    const auto stop_raw = first_handler.handle_index_stop(Poco::JSON::Object::Ptr{});
    Require(stop_raw.rfind("Error:", 0) != 0, "first index.stop must not fail");
    const auto stop_json = ParseObject(stop_raw);
    Require(stop_json->optValue<std::string>("state", "") == "stopped",
            "first index.stop must transition to stopped");
  }

  {
    waxcpp::server::WaxRAGHandler resumed_handler(store_path, models);
    Poco::JSON::Object::Ptr resume_params = new Poco::JSON::Object();
    resume_params->set("repo_root", temp_root.string());
    resume_params->set("resume", true);
    const auto resume_raw = resumed_handler.handle_index_start(resume_params);
    Require(resume_raw.rfind("Error:", 0) != 0, "resume index.start must not fail");

    const auto final_view = WaitForTerminalState(resumed_handler, 25000);
    Require(final_view.state == "stopped", "resumed job must complete");
    Require(final_view.indexed_chunks > 0, "resumed job must ingest chunks");
  }

  Require(std::filesystem::exists(checkpoint_path), "checkpoint must exist after resumed run");
  Require(std::filesystem::exists(scan_manifest), "scan manifest must exist after resumed run");
  Require(std::filesystem::exists(chunk_manifest), "chunk manifest must exist after resumed run");
  Require(std::filesystem::exists(file_manifest), "file manifest must exist after resumed run");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioResumeWithoutFileManifestSkipsCommittedWatermark() {
  waxcpp::tests::Log("scenario: resume without file_manifest skips committed watermark to avoid duplicates");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto baseline_store_path =
      std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_baseline_", ".mv2s");
  const auto baseline_checkpoint_path = std::filesystem::path(baseline_store_path.string() + ".index.checkpoint");
  const auto interrupted_store_path =
      std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_resume_", ".mv2s");
  const auto interrupted_checkpoint_path = std::filesystem::path(interrupted_store_path.string() + ".index.checkpoint");
  const auto interrupted_scan_manifest = std::filesystem::path(interrupted_checkpoint_path.string() + ".scan_manifest");
  const auto interrupted_chunk_manifest = std::filesystem::path(interrupted_checkpoint_path.string() + ".chunk_manifest");
  const auto interrupted_file_manifest = std::filesystem::path(interrupted_checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  for (int i = 0; i < 24; ++i) {
    WriteTextFile(temp_root / ("W" + std::to_string(i) + ".cpp"), MakeLargeCppBody(500));
  }

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);

  std::uint64_t full_chunk_count = 0;
  {
    waxcpp::server::WaxRAGHandler baseline_handler(baseline_store_path, models);
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("resume", false);
    params->set("flush_every_chunks", 1);
    const auto start_raw = baseline_handler.handle_index_start(params);
    Require(start_raw.rfind("Error:", 0) != 0, "baseline index.start must not fail");
    const auto complete_view = WaitForTerminalState(baseline_handler, 35000);
    Require(complete_view.state == "stopped", "baseline run must complete");
    Require(complete_view.indexed_chunks > 0, "baseline run must produce chunks");
    full_chunk_count = complete_view.indexed_chunks;
  }
  Require(full_chunk_count > 8, "baseline chunk count too small for watermark scenario");

  std::uint64_t committed_before_stop = 0;
  {
    waxcpp::server::WaxRAGHandler first_handler(interrupted_store_path, models);
    Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
    start_params->set("repo_root", temp_root.string());
    start_params->set("resume", false);
    start_params->set("flush_every_chunks", 1);
    const auto start_raw = first_handler.handle_index_start(start_params);
    Require(start_raw.rfind("Error:", 0) != 0, "interrupted index.start must not fail");

    const auto committed_target = std::max<std::uint64_t>(2, full_chunk_count / 6);
    const auto running_view = WaitForCommittedChunksAtLeast(first_handler, committed_target, 12000);
    Require(running_view.state == "running", "interrupted run must still be running before explicit stop");
    const auto stop_raw = first_handler.handle_index_stop(Poco::JSON::Object::Ptr{});
    Require(stop_raw.rfind("Error:", 0) != 0, "interrupted index.stop must not fail");
    const auto stop_json = ParseObject(stop_raw);
    committed_before_stop = stop_json->optValue<std::uint64_t>("committed_chunks", 0);
    Require(committed_before_stop >= committed_target, "interrupted run committed watermark must be persisted");
    Require(committed_before_stop < full_chunk_count, "interrupted run must stop before full ingest completion");
  }

  {
    waxcpp::server::WaxRAGHandler resumed_handler(interrupted_store_path, models);
    Poco::JSON::Object::Ptr resume_params = new Poco::JSON::Object();
    resume_params->set("repo_root", temp_root.string());
    resume_params->set("resume", true);
    resume_params->set("flush_every_chunks", 1);
    const auto resume_raw = resumed_handler.handle_index_start(resume_params);
    Require(resume_raw.rfind("Error:", 0) != 0, "resume index.start must not fail");

    const auto final_view = WaitForTerminalState(resumed_handler, 35000);
    Require(final_view.state == "stopped", "resume run must complete");
    Require(final_view.indexed_chunks + committed_before_stop == full_chunk_count,
            "resume run must only ingest remainder after committed watermark");
  }

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(baseline_store_path);
  waxcpp::tests::CleanupStoreArtifacts(interrupted_store_path);
  std::filesystem::remove(baseline_checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(interrupted_checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(interrupted_scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(interrupted_chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(interrupted_file_manifest, ec);
  ec.clear();
}

void ScenarioRepeatedRunsProduceIdenticalChunkManifest() {
  waxcpp::tests::Log("scenario: repeated runs produce byte-identical chunk manifests");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto first_store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_first_", ".mv2s");
  const auto second_store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_second_", ".mv2s");
  const auto first_checkpoint_path = std::filesystem::path(first_store_path.string() + ".index.checkpoint");
  const auto second_checkpoint_path = std::filesystem::path(second_store_path.string() + ".index.checkpoint");
  const auto first_scan_manifest = std::filesystem::path(first_checkpoint_path.string() + ".scan_manifest");
  const auto first_chunk_manifest = std::filesystem::path(first_checkpoint_path.string() + ".chunk_manifest");
  const auto first_file_manifest = std::filesystem::path(first_checkpoint_path.string() + ".file_manifest");
  const auto second_scan_manifest = std::filesystem::path(second_checkpoint_path.string() + ".scan_manifest");
  const auto second_chunk_manifest = std::filesystem::path(second_checkpoint_path.string() + ".chunk_manifest");
  const auto second_file_manifest = std::filesystem::path(second_checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "A.h", "struct A { int x; };");
  WriteTextFile(temp_root / "B.cpp", MakeLargeCppBody(120));
  WriteTextFile(temp_root / "C.inl", "inline int CValue(int v) { return v * 2; }\n");

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);

  auto run_and_read_manifest = [&](const std::filesystem::path& store_path,
                                   const std::filesystem::path& chunk_manifest_path) {
    waxcpp::server::WaxRAGHandler handler(store_path, models);
    Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
    start_params->set("repo_root", temp_root.string());
    start_params->set("resume", false);
    start_params->set("flush_every_chunks", 1);
    const auto start_raw = handler.handle_index_start(start_params);
    Require(start_raw.rfind("Error:", 0) != 0, "index.start must not fail");
    const auto done = WaitForTerminalState(handler, 25000);
    Require(done.state == "stopped", "index run must stop");
    Require(done.indexed_chunks > 0, "index run must ingest chunks");
    Require(std::filesystem::exists(chunk_manifest_path), "chunk manifest must be created");
    return std::make_pair(done.indexed_chunks, ReadTextFile(chunk_manifest_path));
  };

  const auto first = run_and_read_manifest(first_store_path, first_chunk_manifest);
  const auto second = run_and_read_manifest(second_store_path, second_chunk_manifest);

  Require(first.first == second.first, "repeated runs must produce same indexed chunk count");
  Require(first.second == second.second, "repeated runs must produce byte-identical chunk manifest");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(first_store_path);
  waxcpp::tests::CleanupStoreArtifacts(second_store_path);
  std::filesystem::remove(first_checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(first_scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(first_chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(first_file_manifest, ec);
  ec.clear();
  std::filesystem::remove(second_checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(second_scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(second_chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(second_file_manifest, ec);
  ec.clear();
}

void ScenarioMaxFilesCapsScanDeterministically() {
  waxcpp::tests::Log("scenario: index.start max_files caps deterministic scan set");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  for (int i = 0; i < 6; ++i) {
    WriteTextFile(temp_root / ("Cap" + std::to_string(i) + ".cpp"), MakeLargeCppBody(50 + i));
  }

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);
  start_params->set("max_files", 2);
  start_params->set("flush_every_chunks", 1);
  const auto start_raw = handler.handle_index_start(start_params);
  Require(start_raw.rfind("Error:", 0) != 0, "max_files index.start must not fail");

  const auto view = WaitForTerminalState(handler, 20000);
  Require(view.state == "stopped", "max_files run must complete");
  Require(view.scanned_files == 2, "max_files must cap scanned_files to requested limit");
  Require(view.indexed_chunks > 0, "max_files run must still ingest chunks");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioMaxChunksCapsIngestDeterministically() {
  waxcpp::tests::Log("scenario: index.start max_chunks caps ingest deterministically");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "Big.cpp", MakeLargeCppBody(2000));

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);
  start_params->set("max_chunks", 3);
  start_params->set("flush_every_chunks", 1);
  const auto start_raw = handler.handle_index_start(start_params);
  Require(start_raw.rfind("Error:", 0) != 0, "max_chunks index.start must not fail");

  const auto view = WaitForTerminalState(handler, 20000);
  Require(view.state == "stopped", "max_chunks run must complete");
  Require(view.indexed_chunks == 3, "max_chunks must cap indexed_chunks exactly");
  Require(view.committed_chunks == 3, "max_chunks must cap committed_chunks exactly");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioIngestBatchSizeControlWorks() {
  waxcpp::tests::Log("scenario: index.start ingest_batch_size batches deterministic ingest path");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "Batch.cpp", MakeLargeCppBody(2500));

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);
  start_params->set("ingest_batch_size", 7);
  start_params->set("max_chunks", 11);
  start_params->set("flush_every_chunks", 5);
  const auto start_raw = handler.handle_index_start(start_params);
  Require(start_raw.rfind("Error:", 0) != 0, "ingest_batch_size index.start must not fail");

  const auto view = WaitForTerminalState(handler, 20000);
  Require(view.state == "stopped", "ingest_batch_size run must complete");
  Require(view.indexed_chunks == 11, "ingest_batch_size run must honor max_chunks cap");
  Require(view.committed_chunks == 11, "ingest_batch_size run must commit all indexed chunks");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioMaxRamCapFailsWhenTooLow() {
  waxcpp::tests::Log("scenario: index.start max_ram_mb fails when process RSS exceeds cap");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "Ram.cpp", MakeLargeCppBody(300));

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  Poco::JSON::Object::Ptr start_params = new Poco::JSON::Object();
  start_params->set("repo_root", temp_root.string());
  start_params->set("resume", false);
  start_params->set("max_ram_mb", 1);
  start_params->set("flush_every_chunks", 1);
  const auto start_raw = handler.handle_index_start(start_params);
  Require(start_raw.rfind("Error:", 0) != 0, "max_ram_mb index.start must not fail at parse/start stage");

  const auto view = WaitForTerminalState(handler, 20000);
#if defined(_WIN32) || defined(__linux__)
  Require(view.state == "failed", "max_ram_mb with tiny cap must fail on supported RSS-probe platforms");
  const auto status_raw = handler.handle_index_status(Poco::JSON::Object::Ptr{});
  Require(status_raw.rfind("Error:", 0) != 0, "index.status must not fail");
  const auto status_json = ParseObject(status_raw);
  const auto last_error = status_json->optValue<std::string>("last_error", "");
  Require(last_error.find("max_ram_mb exceeded") != std::string::npos,
          "failure reason must mention max_ram_mb exceed");
#else
  Require(view.state == "failed" || view.state == "stopped",
          "max_ram_mb run must terminate on unsupported RSS-probe platforms");
#endif

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

void ScenarioIndexStartRejectsInvalidControls() {
  waxcpp::tests::Log("scenario: index.start rejects invalid control values");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_handler_index_store_", ".mv2s");
  const auto checkpoint_path = std::filesystem::path(store_path.string() + ".index.checkpoint");
  const auto scan_manifest = std::filesystem::path(checkpoint_path.string() + ".scan_manifest");
  const auto chunk_manifest = std::filesystem::path(checkpoint_path.string() + ".chunk_manifest");
  const auto file_manifest = std::filesystem::path(checkpoint_path.string() + ".file_manifest");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create test repo directory: " + temp_root.string());
  }
  WriteTextFile(temp_root / "Ctl.cpp", "int ctl() { return 42; }\n");

  EnvVarGuard llama_root_guard("WAXCPP_LLAMA_CPP_ROOT");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  const auto models = MakeRuntimeConfigForTests(temp_root);
  waxcpp::server::WaxRAGHandler handler(store_path, models);

  {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("flush_every_chunks", 0);
    const auto raw = handler.handle_index_start(params);
    Require(raw == "Error: flush_every_chunks must be within [1, 1000000]",
            "flush_every_chunks=0 must be rejected");
  }
  {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("ingest_batch_size", std::string("oops"));
    const auto raw = handler.handle_index_start(params);
    Require(raw == "Error: ingest_batch_size must be an integer",
            "string ingest_batch_size must be rejected");
  }
  {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("ingest_batch_size", 0);
    const auto raw = handler.handle_index_start(params);
    Require(raw == "Error: ingest_batch_size must be within [1, 1000000]",
            "ingest_batch_size=0 must be rejected");
  }
  {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("max_files", std::string("oops"));
    const auto raw = handler.handle_index_start(params);
    Require(raw == "Error: max_files must be an integer", "string max_files must be rejected");
  }
  {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("max_chunks", -1);
    const auto raw = handler.handle_index_start(params);
    Require(raw == "Error: max_chunks must be within [0, 1000000]",
            "negative max_chunks must be rejected");
  }
  {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("repo_root", temp_root.string());
    params->set("max_ram_mb", std::string("oops"));
    const auto raw = handler.handle_index_start(params);
    Require(raw == "Error: max_ram_mb must be an integer", "string max_ram_mb must be rejected");
  }

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
  std::filesystem::remove(checkpoint_path, ec);
  ec.clear();
  std::filesystem::remove(scan_manifest, ec);
  ec.clear();
  std::filesystem::remove(chunk_manifest, ec);
  ec.clear();
  std::filesystem::remove(file_manifest, ec);
  ec.clear();
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("wax_rag_handler_index_test: start");
    ScenarioIndexStartIsAsyncAndStopWorks();
    ScenarioIndexCompleteWritesManifests();
    ScenarioResumeSkipsUnchangedFilesThenIndexesChangedFile();
    ScenarioInterruptedIndexResumesAfterHandlerRecreate();
    ScenarioResumeWithoutFileManifestSkipsCommittedWatermark();
    ScenarioRepeatedRunsProduceIdenticalChunkManifest();
    ScenarioMaxFilesCapsScanDeterministically();
    ScenarioMaxChunksCapsIngestDeterministically();
    ScenarioIngestBatchSizeControlWorks();
    ScenarioMaxRamCapFailsWhenTooLow();
    ScenarioIndexStartRejectsInvalidControls();
    waxcpp::tests::Log("wax_rag_handler_index_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_handler_index_repo_");
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_handler_index_store_");
    return EXIT_FAILURE;
  }
}
