#include "../../server/wax_rag_handler.hpp"

#include "../temp_artifacts.hpp"
#include "../test_logger.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string TempName(const std::string& prefix, const std::string& suffix) {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::ostringstream out;
  out << prefix << static_cast<long long>(now) << suffix;
  return out.str();
}

void SetEnvVar(const char* key, const std::string& value) {
#if defined(_MSC_VER)
  if (_putenv_s(key, value.c_str()) != 0) {
    throw std::runtime_error(std::string("failed to set env var: ") + key);
  }
#else
  if (setenv(key, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("failed to set env var: ") + key);
  }
#endif
}

std::optional<std::string> GetEnvVar(const char* key) {
#if defined(_MSC_VER)
  char* value = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&value, &len, key) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out(value);
  std::free(value);
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
#else
  const char* value = std::getenv(key);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

void UnsetEnvVar(const char* key) {
#if defined(_MSC_VER)
  if (_putenv_s(key, "") != 0) {
    throw std::runtime_error(std::string("failed to unset env var: ") + key);
  }
#else
  if (unsetenv(key) != 0) {
    throw std::runtime_error(std::string("failed to unset env var: ") + key);
  }
#endif
}

class EnvVarGuard {
 public:
  explicit EnvVarGuard(const char* key) : key_(key), previous_(GetEnvVar(key)) {}
  ~EnvVarGuard() noexcept {
    try {
      if (previous_.has_value()) {
        SetEnvVar(key_.c_str(), *previous_);
      } else {
        UnsetEnvVar(key_.c_str());
      }
    } catch (...) {
    }
  }

 private:
  std::string key_{};
  std::optional<std::string> previous_{};
};

Poco::JSON::Object::Ptr ParseObject(const std::string& json) {
  Poco::JSON::Parser parser;
  const Poco::Dynamic::Var parsed = parser.parse(json);
  auto object = parsed.extract<Poco::JSON::Object::Ptr>();
  if (object.isNull()) {
    throw std::runtime_error("expected JSON object");
  }
  return object;
}

std::vector<std::string> CitationSignature(const Poco::JSON::Object::Ptr& response) {
  auto citations = response->getArray("citations");
  if (citations.isNull()) {
    throw std::runtime_error("citations array must be present");
  }
  const auto citation_count = citations->size();
  std::vector<std::string> signature{};
  signature.reserve(static_cast<std::size_t>(citation_count));
  for (unsigned int i = 0; i < citation_count; ++i) {
    auto entry = citations->getObject(i);
    if (entry.isNull()) {
      throw std::runtime_error("citation entry must be object");
    }
    std::ostringstream key;
    key << entry->optValue<std::string>("relative_path", "") << ":"
        << entry->optValue<int>("line_start", 0) << ":"
        << entry->optValue<int>("line_end", 0) << ":"
        << entry->optValue<std::string>("symbol", "");
    signature.push_back(key.str());
  }
  return signature;
}

void RememberWithMetadata(waxcpp::server::WaxRAGHandler& handler,
                          const std::string& content,
                          const std::string& path,
                          int line_start,
                          int line_end,
                          const std::string& symbol) {
  Poco::JSON::Object::Ptr metadata = new Poco::JSON::Object();
  metadata->set("relative_path", path);
  metadata->set("line_start", std::to_string(line_start));
  metadata->set("line_end", std::to_string(line_end));
  metadata->set("symbol", symbol);

  Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
  params->set("content", content);
  params->set("metadata", metadata);
  const auto reply = handler.handle_remember(params);
  Require(reply == "OK", "remember call failed: " + reply);
}

std::unique_ptr<waxcpp::server::LlamaCppGenerationClient> MakeStubGenerationClient(
    const waxcpp::RuntimeModelsConfig& models,
    std::string* captured_request = nullptr) {
  return std::make_unique<waxcpp::server::LlamaCppGenerationClient>(
      waxcpp::server::LlamaCppGenerationConfig{
          .endpoint = "",
          .model_path = models.generation_model.model_path,
          .timeout_ms = 1000,
          .max_retries = 0,
          .retry_backoff_ms = 0,
          .request_fn =
              [captured_request](const std::string& body) {
            if (captured_request != nullptr) {
              *captured_request = body;
            }
            return std::string(R"({"choices":[{"message":{"content":"stubbed-answer"}}]})");
          },
      });
}

void ScenarioAnswerGenerateUsesContextBudgetAndCitations() {
  waxcpp::tests::Log("scenario: answer.generate applies context budget and returns citations");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_store_", ".mv2s");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp runtime root");
  }
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());

  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = temp_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;

  std::string captured_generation_request{};
  waxcpp::server::WaxRAGHandler handler(
      store_path,
      models,
      MakeStubGenerationClient(models, &captured_generation_request));

  RememberWithMetadata(handler,
                       "AlphaFn shared tokenA tokenB tokenC tokenD tokenE tokenF",
                       "Engine/Source/Alpha.cpp",
                       10,
                       20,
                       "AlphaFn");
  RememberWithMetadata(handler,
                       "BetaFn shared token1 token2 token3 token4 token5 token6",
                       "Engine/Source/Beta.cpp",
                       30,
                       40,
                       "BetaFn");
  Require(handler.handle_flush(Poco::JSON::Object::Ptr{}) == "OK", "flush must succeed");

  Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
  params->set("query", "shared");
  params->set("max_context_items", 10);
  params->set("max_context_tokens", 4);
  params->set("max_output_tokens", 128);
  const auto response_raw = handler.handle_answer_generate(params);
  Require(response_raw.rfind("Error:", 0) != 0, "answer.generate returned error: " + response_raw);
  const auto response = ParseObject(response_raw);

  Require(response->optValue<std::string>("answer", "") == "stubbed-answer", "answer payload mismatch");
  const int context_items_used = response->optValue<int>("context_items_used", -1);
  Require(context_items_used == 1,
          "budget clamp must keep exactly one context item, got " + std::to_string(context_items_used) +
              ", response=" + response_raw);
  Require(response->optValue<int>("context_tokens_used", 0) > 0, "context token count must be positive");
  Require(response->optValue<std::string>("model", "") == models.generation_model.model_path,
          "response model path mismatch");

  auto citations = response->getArray("citations");
  Require(!citations.isNull(), "citations array must be present");
  Require(citations->size() >= 1, "at least one citation expected");
  auto first_citation = citations->getObject(0);
  Require(!first_citation.isNull(), "citation entry must be an object");
  Require(!first_citation->optValue<std::string>("relative_path", "").empty(),
          "citation relative_path must be populated");

  Require(captured_generation_request.find("Citation Map") != std::string::npos,
          "generation request must include citation map text");
  Require(captured_generation_request.find("[frame:") != std::string::npos,
          "generation request must include frame tags");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
}

void ScenarioFactLifecycleApisWorkAndPersist() {
  waxcpp::tests::Log("scenario: fact lifecycle APIs work and persist across reopen");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_fact_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_fact_store_", ".mv2s");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp runtime root");
  }
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());

  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = temp_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;

  auto build_fact_add_params = [](const std::string& entity,
                                  const std::string& attribute,
                                  const std::string& value,
                                  const std::string& source) {
    Poco::JSON::Object::Ptr metadata = new Poco::JSON::Object();
    metadata->set("source", source);
    metadata->set("kind", "runtime_eav");

    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("entity", entity);
    params->set("attribute", attribute);
    params->set("value", value);
    params->set("metadata", metadata);
    return params;
  };

  auto search_fact = [](waxcpp::server::WaxRAGHandler& handler, const std::string& prefix) {
    Poco::JSON::Object::Ptr search_params = new Poco::JSON::Object();
    search_params->set("entity_prefix", prefix);
    search_params->set("limit", 10);
    return ParseObject(handler.handle_fact_search(search_params));
  };

  auto get_fact = [](waxcpp::server::WaxRAGHandler& handler, std::int64_t id) {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("id", id);
    return ParseObject(handler.handle_fact_get(params));
  };

  auto history_fact = [](waxcpp::server::WaxRAGHandler& handler, std::int64_t id) {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("id", id);
    return ParseObject(handler.handle_fact_history(params));
  };

  auto require_single_search_result = [](const Poco::JSON::Object::Ptr& response,
                                         const std::string& entity,
                                         const std::string& attribute,
                                         const std::string& value) {
    Require(response->optValue<int>("count", 0) == 1, "fact.search must return exactly one fact");
    auto facts = response->getArray("facts");
    Require(!facts.isNull() && facts->size() == 1, "facts array must contain one item");
    auto fact = facts->getObject(0);
    Require(!fact.isNull(), "fact row must be an object");
    Require(fact->optValue<std::string>("entity", "") == entity, "fact entity mismatch");
    Require(fact->optValue<std::string>("attribute", "") == attribute, "fact attribute mismatch");
    Require(fact->optValue<std::string>("value", "") == value, "fact value mismatch");
    Require(fact->optValue<std::int64_t>("id", -1) >= 0, "fact id must be non-negative");
    return fact;
  };

  auto require_empty_search_result = [](const Poco::JSON::Object::Ptr& response) {
    Require(response->optValue<int>("count", -1) == 0, "fact.search must return zero facts");
    auto facts = response->getArray("facts");
    Require(!facts.isNull() && facts->size() == 0, "facts array must be empty");
  };

  std::int64_t pinned_fact_id = -1;
  std::int64_t deleted_history_fact_id = -1;
  std::int64_t deleted_active_fact_id = -1;

  {
    waxcpp::server::WaxRAGHandler handler(store_path, models, MakeStubGenerationClient(models));

    const auto add_reply = handler.handle_fact_add(
        build_fact_add_params("cpp:AMyActor", "inherits", "AActor", "hive"));
    const auto add0 = ParseObject(add_reply);
    Require(add0->optValue<bool>("added", false), "fact.add must report added=true");
    const auto fact0_id = add0->optValue<Poco::Int64>("id", -1);
    Require(fact0_id >= 0, "fact.add must return a valid id");
    const auto add0_fact = add0->getObject("fact");
    Require(!add0_fact.isNull(), "fact.add must include fact object");
    Require(add0_fact->optValue<std::string>("entity", "") == "cpp:AMyActor", "fact.add entity mismatch");
    Require(add0_fact->optValue<std::string>("attribute", "") == "inherits", "fact.add attribute mismatch");
    Require(add0_fact->optValue<std::string>("value", "") == "AActor", "fact.add value mismatch");
    const auto first_fact = require_single_search_result(
        search_fact(handler, "cpp:AMyActor"), "cpp:AMyActor", "inherits", "AActor");
    Require(first_fact->optValue<Poco::Int64>("id", -1) == fact0_id, "fact.add id must match search result");
    Require(fact0_id >= 0, "initial fact id must be valid");

    const auto get0 = get_fact(handler, fact0_id);
    auto get0_fact = get0->getObject("fact");
    Require(!get0_fact.isNull(), "fact.get must return a fact object");
    Require(!get0_fact->optValue<bool>("pinned", true), "new fact must start unpinned");
    Require(!get0_fact->optValue<bool>("deleted", true), "new fact must start undeleted");

    Poco::JSON::Object::Ptr update_metadata = new Poco::JSON::Object();
    update_metadata->set("source", "correction");
    Poco::JSON::Object::Ptr update_params = new Poco::JSON::Object();
    update_params->set("id", fact0_id);
    update_params->set("value", "APawn");
    update_params->set("metadata", update_metadata);
    const auto update0 = ParseObject(handler.handle_fact_update(update_params));
    const auto fact1_id = update0->optValue<Poco::Int64>("id", -1);
    Require(update0->optValue<bool>("updated", false), "fact.update must report updated=true");
    Require(fact1_id >= 0 && fact1_id != fact0_id, "fact.update must create a new revision id");

    const auto second_fact = require_single_search_result(
        search_fact(handler, "cpp:AMyActor"), "cpp:AMyActor", "inherits", "APawn");
    Require(second_fact->optValue<Poco::Int64>("supersedes", -1) == fact0_id,
            "updated fact must supersede previous revision");

    const auto history1 = history_fact(handler, fact1_id);
    Require(history1->optValue<int>("count", 0) == 2, "updated fact must have two-step history");
    auto history1_array = history1->getArray("history");
    Require(!history1_array.isNull() && history1_array->size() == 2, "history array size mismatch");

    Poco::JSON::Object::Ptr pin_params = new Poco::JSON::Object();
    pin_params->set("id", fact1_id);
    const auto pin1 = ParseObject(handler.handle_fact_pin(pin_params));
    pinned_fact_id = pin1->optValue<Poco::Int64>("id", -1);
    Require(pinned_fact_id >= 0 && pinned_fact_id != fact1_id, "fact.pin must create pinned revision");

    const auto pinned_fact = get_fact(handler, pinned_fact_id)->getObject("fact");
    Require(!pinned_fact.isNull(), "pinned fact must be readable");
    Require(pinned_fact->optValue<bool>("pinned", false), "pinned revision must expose pinned=true");
    Require(pinned_fact->optValue<Poco::Int64>("supersedes", -1) == fact1_id,
            "pinned revision must supersede previous revision");

    update_params->set("id", pinned_fact_id);
    const auto pinned_update = ParseObject(handler.handle_fact_update(update_params));
    Require(!pinned_update->has("updated"), "pinned fact update must fail");
    Require(pinned_update->optValue<std::string>("error", "").find("pinned") != std::string::npos,
            "pinned fact update must mention pinned guard");

    Poco::JSON::Object::Ptr delete_pinned_params = new Poco::JSON::Object();
    delete_pinned_params->set("id", pinned_fact_id);
    const auto pinned_delete = ParseObject(handler.handle_fact_delete(delete_pinned_params));
    Require(!pinned_delete->optValue<bool>("deleted", false), "pinned fact delete must not report deleted");
    Require(pinned_delete->optValue<std::string>("error", "").find("pinned") != std::string::npos,
            "pinned fact delete must mention pinned guard");

    Poco::JSON::Object::Ptr delete_history_params = new Poco::JSON::Object();
    delete_history_params->set("id", fact0_id);
    const auto deleted_history = ParseObject(handler.handle_fact_delete(delete_history_params));
    Require(deleted_history->optValue<bool>("deleted", false), "historical fact delete must succeed");
    deleted_history_fact_id = fact0_id;

    const auto deleted_history_fact = get_fact(handler, fact0_id)->getObject("fact");
    Require(!deleted_history_fact.isNull(), "deleted historical fact must remain addressable");
    Require(deleted_history_fact->optValue<bool>("deleted", false),
            "deleted historical fact must expose deleted=true");

    const auto second_add_reply = handler.handle_fact_add(
        build_fact_add_params("cfg:rate_limit", "value", "10/s", "hive"));
    const auto add1 = ParseObject(second_add_reply);
    Require(add1->optValue<bool>("added", false), "second fact.add must report added=true");
    const auto rate_fact = require_single_search_result(
        search_fact(handler, "cfg:rate_limit"), "cfg:rate_limit", "value", "10/s");
    deleted_active_fact_id = rate_fact->optValue<Poco::Int64>("id", -1);
    Require(deleted_active_fact_id >= 0, "active fact id must be valid");

    Poco::JSON::Object::Ptr delete_active_params = new Poco::JSON::Object();
    delete_active_params->set("id", deleted_active_fact_id);
    const auto delete_active = ParseObject(handler.handle_fact_delete(delete_active_params));
    Require(delete_active->optValue<bool>("deleted", false), "active fact delete must succeed");
    require_empty_search_result(search_fact(handler, "cfg:rate_limit"));

    const auto deleted_active_fact = get_fact(handler, deleted_active_fact_id)->getObject("fact");
    Require(!deleted_active_fact.isNull(), "deleted active fact must remain addressable");
    Require(deleted_active_fact->optValue<bool>("deleted", false),
            "deleted active fact must expose deleted=true");

    const auto final_search = require_single_search_result(
        search_fact(handler, "cpp:AMyActor"), "cpp:AMyActor", "inherits", "APawn");
    Require(final_search->optValue<Poco::Int64>("id", -1) == pinned_fact_id,
            "search must return pinned active revision");
    Require(final_search->optValue<bool>("pinned", false), "search result must expose pinned=true");

    const auto flush_reply = handler.handle_flush(Poco::JSON::Object::Ptr{});
    Require(flush_reply == "OK", "flush must succeed after fact lifecycle operations");
  }

  {
    waxcpp::server::WaxRAGHandler reopened(store_path, models, MakeStubGenerationClient(models));
    const auto reopened_fact = require_single_search_result(
        search_fact(reopened, "cpp:AMyActor"), "cpp:AMyActor", "inherits", "APawn");
    Require(reopened_fact->optValue<Poco::Int64>("id", -1) == pinned_fact_id,
            "reopen must preserve active pinned revision id");
    Require(reopened_fact->optValue<bool>("pinned", false), "reopen must preserve pinned flag");

    require_empty_search_result(search_fact(reopened, "cfg:rate_limit"));

    const auto reopened_history = history_fact(reopened, pinned_fact_id);
    Require(reopened_history->optValue<int>("count", 0) == 3, "reopen must preserve full history chain");
    auto reopened_history_array = reopened_history->getArray("history");
    Require(!reopened_history_array.isNull() && reopened_history_array->size() == 3,
            "reopened history array size mismatch");
    auto oldest = reopened_history_array->getObject(2);
    Require(!oldest.isNull() && oldest->optValue<Poco::Int64>("id", -1) == deleted_history_fact_id,
            "history tail must include deleted original revision");
    Require(oldest->optValue<bool>("deleted", false), "history tail must preserve deleted status");

    const auto reopened_deleted_active = get_fact(reopened, deleted_active_fact_id)->getObject("fact");
    Require(!reopened_deleted_active.isNull(), "reopen must keep deleted active fact addressable");
    Require(reopened_deleted_active->optValue<bool>("deleted", false),
            "reopen must preserve deleted active fact state");
  }

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
}

void ScenarioRejectsInvalidOrchestratorIngestEnvValues() {
  waxcpp::tests::Log("scenario: constructor rejects invalid orchestrator ingest env values");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_store_", ".mv2s");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp runtime root");
  }

  EnvVarGuard guard_cpp_root("WAXCPP_LLAMA_CPP_ROOT");
  EnvVarGuard guard_ingest_concurrency("WAXCPP_ORCH_INGEST_CONCURRENCY");
  EnvVarGuard guard_ingest_batch_size("WAXCPP_ORCH_INGEST_BATCH_SIZE");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  SetEnvVar("WAXCPP_ORCH_INGEST_CONCURRENCY", "0");
  SetEnvVar("WAXCPP_ORCH_INGEST_BATCH_SIZE", "not-a-number");

  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = temp_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;

  bool threw = false;
  try {
    waxcpp::server::WaxRAGHandler handler(store_path, models);
    (void)handler;
  } catch (const std::exception& ex) {
    threw = true;
    const std::string message = ex.what();
    Require(message.find("WAXCPP_ORCH_INGEST_CONCURRENCY") != std::string::npos,
            "invalid ingest env error must mention variable name");
  }
  Require(threw, "constructor must reject invalid ingest env values");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
}

void ScenarioAnswerGenerateIsDeterministicAcrossCallsAndReopen() {
  waxcpp::tests::Log("scenario: answer.generate deterministic citations across repeated calls and reopen");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_store_", ".mv2s");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp runtime root");
  }
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());

  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = temp_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;

  auto run_answer = [&](waxcpp::server::WaxRAGHandler& handler) {
    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    params->set("query", "shared deterministic token");
    params->set("max_context_items", 6);
    params->set("max_context_tokens", 256);
    params->set("max_output_tokens", 64);
    const auto raw = handler.handle_answer_generate(params);
    Require(raw.rfind("Error:", 0) != 0, "answer.generate returned error: " + raw);
    return ParseObject(raw);
  };

  {
    waxcpp::server::WaxRAGHandler handler(store_path, models, MakeStubGenerationClient(models));
    RememberWithMetadata(handler,
                         "Alpha shared deterministic token a1 a2 a3",
                         "Engine/Source/Alpha.cpp",
                         10,
                         20,
                         "Alpha");
    RememberWithMetadata(handler,
                         "Beta shared deterministic token b1 b2 b3",
                         "Engine/Source/Beta.cpp",
                         30,
                         40,
                         "Beta");
    RememberWithMetadata(handler,
                         "Gamma shared deterministic token c1 c2 c3",
                         "Engine/Source/Gamma.cpp",
                         50,
                         60,
                         "Gamma");
    Require(handler.handle_flush(Poco::JSON::Object::Ptr{}) == "OK", "flush must succeed");

    const auto first = run_answer(handler);
    const auto second = run_answer(handler);
    const auto first_sig = CitationSignature(first);
    const auto second_sig = CitationSignature(second);
    Require(first_sig == second_sig, "repeated calls must keep identical citation order");
    Require(first->optValue<int>("context_items_used", -1) == second->optValue<int>("context_items_used", -2),
            "repeated calls must keep same context_items_used");
    Require(first->optValue<int>("context_tokens_used", -1) == second->optValue<int>("context_tokens_used", -2),
            "repeated calls must keep same context_tokens_used");
  }

  {
    waxcpp::server::WaxRAGHandler reopened(store_path, models, MakeStubGenerationClient(models));
    const auto third = run_answer(reopened);
    const auto fourth = run_answer(reopened);
    const auto third_sig = CitationSignature(third);
    const auto fourth_sig = CitationSignature(fourth);
    Require(third_sig == fourth_sig, "reopened repeated calls must keep identical citation order");
  }

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("wax_rag_handler_answer_test: start");
    ScenarioAnswerGenerateUsesContextBudgetAndCitations();
    ScenarioRejectsInvalidOrchestratorIngestEnvValues();
    ScenarioFactLifecycleApisWorkAndPersist();
    ScenarioAnswerGenerateIsDeterministicAcrossCallsAndReopen();
    waxcpp::tests::Log("wax_rag_handler_answer_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_answer_repo_");
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_answer_store_");
    return EXIT_FAILURE;
  }
}
