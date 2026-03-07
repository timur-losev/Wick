// cpp/server/wax_rag_handler.cpp
#include "wax_rag_handler.hpp"
#include "chunk_enricher.hpp"
#include "regex_ue5_enricher.hpp"
#include "llm_fact_enricher.hpp"
#include "runtime_config.hpp"
#include "server_utils.hpp"

#include <Poco/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Process.h>
#include <Poco/Pipe.h>
#include <Poco/PipeStream.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>
#include <chrono>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace waxcpp::server {

namespace {
constexpr std::uint64_t kIndexFlushEveryChunks = 4096;
constexpr std::uint64_t kMaxIndexControlValue = 1'000'000;
constexpr const char* kDefaultLlamaEmbedEndpoint = "http://127.0.0.1:8004/embedding";
constexpr const char* kDefaultLlamaGenEndpoint = "http://127.0.0.1:8004/completion";
constexpr const char* kServerLogEnv = "WAXCPP_SERVER_LOG";
constexpr const char* kOrchIngestConcurrencyEnv = "WAXCPP_ORCH_INGEST_CONCURRENCY";
constexpr const char* kOrchIngestBatchSizeEnv = "WAXCPP_ORCH_INGEST_BATCH_SIZE";
constexpr const char* kAutoFlushIntervalEnv = "WAXCPP_AUTO_FLUSH_INTERVAL_MS";
constexpr std::uint64_t kDefaultAutoFlushIntervalMs = 30000;  // 30 seconds

int ParsePositiveIntEnv(const char* name, int fallback) {
    const auto raw = EnvString(name);
    if (!raw.has_value()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(*raw, &consumed, 10);
        if (consumed != raw->size() || parsed <= 0) {
            throw std::runtime_error("");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error(std::string("invalid positive integer env value for ") + name + ": " + *raw);
    }
}

int ParseNonNegativeIntEnv(const char* name, int fallback) {
    const auto raw = EnvString(name);
    if (!raw.has_value()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(*raw, &consumed, 10);
        if (consumed != raw->size() || parsed < 0) {
            throw std::runtime_error("");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error(std::string("invalid non-negative integer env value for ") + name + ": " + *raw);
    }
}

float ParseFloatParam(const Poco::JSON::Object::Ptr& params,
                      const std::string& key,
                      float fallback) {
    if (params.isNull() || !params->has(key)) {
        return fallback;
    }
    try {
        return params->getValue<float>(key);
    } catch (const Poco::Exception&) {
        return fallback;
    }
}

int ParsePositiveIntParam(const Poco::JSON::Object::Ptr& params,
                          const std::string& key,
                          int fallback) {
    if (params.isNull() || !params->has(key)) {
        return fallback;
    }
    try {
        const auto value = params->getValue<int>(key);
        return value > 0 ? value : fallback;
    } catch (const Poco::Exception&) {
        return fallback;
    }
}

struct IntParamParseResult {
    bool present = false;
    bool valid = false;
    int value = 0;
};

IntParamParseResult ParseIntParamStrict(const Poco::JSON::Object::Ptr& params,
                                        const std::string& key) {
    if (params.isNull() || !params->has(key)) {
        return IntParamParseResult{};
    }
    try {
        return IntParamParseResult{
            .present = true,
            .valid = true,
            .value = params->getValue<int>(key),
        };
    } catch (const Poco::Exception&) {
        return IntParamParseResult{
            .present = true,
            .valid = false,
            .value = 0,
        };
    }
}

bool ServerLogEnabled() {
    static const bool enabled = []() {
        const auto raw = EnvString(kServerLogEnv);
        if (!raw.has_value()) return false;
        const auto& v = *raw;
        return v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON";
    }();
    return enabled;
}

void ServerLog(std::string_view message) {
    if (!ServerLogEnabled()) {
        return;
    }
    std::cerr << "[waxcpp-server] " << message << "\n";
}

std::uint64_t NowMs() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
}

std::optional<std::uint64_t> CurrentProcessRSSBytes() {
#if defined(_WIN32)
    // Use PrivateUsage (committed private bytes) instead of WorkingSetSize,
    // because WorkingSetSize on Windows includes OS file-cache pages mapped
    // into the process address space — inflating the reported value by the
    // size of any memory-mapped files (e.g. the 15+ GB mv2s store).
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counters.PrivateUsage);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm", std::ios::in);
    if (!statm) {
        return std::nullopt;
    }
    std::uint64_t resident_pages = 0;
    std::uint64_t ignored_pages = 0;
    statm >> ignored_pages >> resident_pages;
    (void)ignored_pages;
    if (!statm) {
        return std::nullopt;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return std::nullopt;
    }
    return resident_pages * static_cast<std::uint64_t>(page_size);
#else
    return std::nullopt;
#endif
}

struct CitationInfo {
    std::uint64_t frame_id = 0;
    std::string relative_path{};
    std::optional<int> line_start{};
    std::optional<int> line_end{};
    std::string symbol{};
    float score = 0.0f;
    waxcpp::RAGItemKind kind = waxcpp::RAGItemKind::kSnippet;
};

struct PromptBuildResult {
    std::string system_prompt{};   // System role message (instructions).
    std::string prompt{};          // User role message (query + context).
    int context_items_used = 0;
    int context_tokens_used = 0;
};

std::optional<int> ParseOptionalInt(const std::unordered_map<std::string, std::string>& metadata,
                                    const char* key) {
    const auto it = metadata.find(key);
    if (it == metadata.end() || it->second.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(it->second, &consumed, 10);
        if (consumed != it->second.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<CitationInfo> BuildCitations(waxcpp::MemoryOrchestrator& orchestrator,
                                         const waxcpp::RAGContext& context,
                                         int max_context_items) {
    std::unordered_map<std::uint64_t, CitationInfo> by_frame{};
    int seen_items = 0;
    for (const auto& item : context.items) {
        if (seen_items >= max_context_items) {
            break;
        }
        ++seen_items;
        auto [it, inserted] = by_frame.emplace(item.frame_id,
                                               CitationInfo{
                                                   .frame_id = item.frame_id,
                                                   .score = item.score,
                                                   .kind = item.kind,
                                               });
        if (!inserted) {
            continue;
        }
        const auto meta = orchestrator.FrameMeta(item.frame_id);
        if (!meta.has_value()) {
            continue;
        }
        const auto path_it = meta->metadata.find("relative_path");
        if (path_it != meta->metadata.end()) {
            it->second.relative_path = path_it->second;
        }
        it->second.line_start = ParseOptionalInt(meta->metadata, "line_start");
        it->second.line_end = ParseOptionalInt(meta->metadata, "line_end");
        const auto symbol_it = meta->metadata.find("symbol");
        if (symbol_it != meta->metadata.end()) {
            it->second.symbol = symbol_it->second;
        }
    }

    std::vector<CitationInfo> citations{};
    citations.reserve(by_frame.size());
    for (const auto& [_, citation] : by_frame) {
        citations.push_back(citation);
    }
    constexpr float kScoreEpsilon = 1e-6f;
    std::sort(citations.begin(), citations.end(), [](const CitationInfo& lhs, const CitationInfo& rhs) {
        const float diff = lhs.score - rhs.score;
        if (diff > kScoreEpsilon) return true;
        if (diff < -kScoreEpsilon) return false;
        return lhs.frame_id < rhs.frame_id;
    });
    return citations;
}

bool IsPunctOrOperator(char ch) {
    switch (ch) {
        case '(': case ')': case '[': case ']': case '{': case '}':
        case '<': case '>': case '.': case ',': case ';': case ':':
        case '!': case '?': case '+': case '-': case '*': case '/':
        case '=': case '&': case '|': case '^': case '~': case '%':
        case '#': case '@':
            return true;
        default:
            return false;
    }
}

int CountApproxTokens(std::string_view text) {
    int tokens = 0;
    bool in_token = false;
    for (const char ch : text) {
        const bool ws = (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == '\v');
        if (ws) {
            in_token = false;
            continue;
        }
        if (IsPunctOrOperator(ch)) {
            ++tokens;
            in_token = false;
            continue;
        }
        if (!in_token) {
            ++tokens;
            in_token = true;
        }
    }
    return tokens > 0 ? tokens : (text.empty() ? 0 : 1);
}

PromptBuildResult BuildAnswerPrompt(const std::string& query,
                                    const waxcpp::RAGContext& context,
                                    const std::vector<CitationInfo>& citations,
                                    int max_context_items,
                                    int max_context_tokens) {
    PromptBuildResult result{};

    // System prompt — role instructions (separate from user content).
    result.system_prompt =
        "You are a code assistant for Unreal Engine 5 C++ source code.\n"
        "Answer the query using only the provided context.\n"
        "Cite sources with [frame:<id>] tags.\n"
        "Be concise and technical.";

    // User prompt — query + context + citation map.
    std::ostringstream prompt;
    prompt << query << "\n\n"
           << "Context:\n";

    const int safe_max_tokens = std::max(1, max_context_tokens);
    for (const auto& item : context.items) {
        if (result.context_items_used >= max_context_items) {
            break;
        }
        const int item_tokens = std::max(1, CountApproxTokens(item.text));
        if (result.context_items_used > 0 && (result.context_tokens_used + item_tokens) > safe_max_tokens) {
            break;
        }
        ++result.context_items_used;
        result.context_tokens_used += item_tokens;
        prompt << "- [frame:" << item.frame_id << "] " << item.text << "\n";
    }

    prompt << "\nCitation Map:\n";
    for (const auto& citation : citations) {
        prompt << "- [frame:" << citation.frame_id << "] ";
        if (!citation.relative_path.empty()) {
            prompt << citation.relative_path;
        } else {
            prompt << "(path unavailable)";
        }
        if (citation.line_start.has_value()) {
            prompt << ":" << *citation.line_start;
            if (citation.line_end.has_value()) {
                prompt << "-" << *citation.line_end;
            }
        }
        if (!citation.symbol.empty()) {
            prompt << " symbol=" << citation.symbol;
        }
        prompt << "\n";
    }
    // Qwen3 /no_think suppresses chain-of-thought in output.
    prompt << "\n/no_think";
    result.prompt = prompt.str();
    return result;
}

}  // namespace

WaxRAGHandler::WaxRAGHandler(const std::filesystem::path& store_path,
                             waxcpp::RuntimeModelsConfig runtime_models,
                             std::unique_ptr<LlamaCppGenerationClient> generation_client_override)
    : index_job_manager_(store_path.string() + ".index.checkpoint"),
      runtime_models_(std::move(runtime_models)) {
    if (runtime_models_.generation_model.runtime.empty() &&
        runtime_models_.generation_model.model_path.empty() &&
        runtime_models_.embedding_model.runtime.empty() &&
        runtime_models_.embedding_model.model_path.empty() &&
        runtime_models_.llama_cpp_root.empty() &&
        !runtime_models_.enable_vector_search) {
        runtime_models_ = DefaultServerRuntimeConfig().models;
    }
    waxcpp::ValidateRuntimeModelsConfig(runtime_models_);
    waxcpp::OrchestratorConfig config{};
    config.enable_vector_search = runtime_models_.enable_vector_search;
    config.require_on_device_providers = false;
    config.ingest_concurrency = ParsePositiveIntEnv(kOrchIngestConcurrencyEnv, config.ingest_concurrency);
    config.ingest_batch_size = ParsePositiveIntEnv(kOrchIngestBatchSizeEnv, config.ingest_batch_size);
    // RAG recall tuning (defaults raised for code-heavy repos).
    config.rag.preview_max_bytes = ParsePositiveIntEnv("WAXCPP_RAG_PREVIEW_MAX_BYTES", 4096);
    config.rag.max_context_tokens = ParsePositiveIntEnv("WAXCPP_RAG_MAX_CONTEXT_TOKENS", 8000);
    config.rag.snippet_max_tokens = ParsePositiveIntEnv("WAXCPP_RAG_SNIPPET_MAX_TOKENS", 600);
    config.rag.expansion_max_tokens = ParsePositiveIntEnv("WAXCPP_RAG_EXPANSION_MAX_TOKENS", 1200);
    config.rag.max_snippets = ParsePositiveIntEnv("WAXCPP_RAG_MAX_SNIPPETS", 24);
    config.rag.search_top_k = ParsePositiveIntEnv("WAXCPP_RAG_SEARCH_TOP_K", 24);
    std::shared_ptr<waxcpp::EmbeddingProvider> embedder{};
    if (runtime_models_.enable_vector_search) {
        LlamaCppEmbeddingProviderConfig embedder_config{};
        embedder_config.endpoint = EnvString("WAXCPP_LLAMA_EMBED_ENDPOINT").value_or(kDefaultLlamaEmbedEndpoint);
        const auto shared_api_key = EnvString("WAXCPP_LLAMA_API_KEY");
        embedder_config.api_key =
            EnvString("WAXCPP_LLAMA_EMBED_API_KEY").value_or(shared_api_key.value_or(std::string{}));
        embedder_config.model_path = runtime_models_.embedding_model.model_path;
        embedder_config.dimensions = ParsePositiveIntEnv("WAXCPP_LLAMA_EMBED_DIMS", 1024);
        embedder_config.timeout_ms = ParsePositiveIntEnv("WAXCPP_LLAMA_EMBED_TIMEOUT_MS", 30000);
        embedder_config.max_retries = ParseNonNegativeIntEnv("WAXCPP_LLAMA_EMBED_MAX_RETRIES", 2);
        embedder_config.retry_backoff_ms = ParseNonNegativeIntEnv("WAXCPP_LLAMA_EMBED_RETRY_BACKOFF_MS", 100);
        embedder_config.max_batch_concurrency =
            ParsePositiveIntEnv("WAXCPP_LLAMA_EMBED_MAX_BATCH_CONCURRENCY", 4);
        embedder = std::make_shared<LlamaCppEmbeddingProvider>(std::move(embedder_config));
    }
    LlamaCppGenerationConfig generation_config{};
    generation_config.endpoint = EnvString("WAXCPP_LLAMA_GEN_ENDPOINT").value_or(kDefaultLlamaGenEndpoint);
    const auto shared_api_key = EnvString("WAXCPP_LLAMA_API_KEY");
    generation_config.api_key =
        EnvString("WAXCPP_LLAMA_GEN_API_KEY").value_or(shared_api_key.value_or(std::string{}));
    generation_config.model_path = runtime_models_.generation_model.model_path;
    generation_config.timeout_ms = ParsePositiveIntEnv("WAXCPP_LLAMA_GEN_TIMEOUT_MS", 60000);
    generation_config.max_retries = ParseNonNegativeIntEnv("WAXCPP_LLAMA_GEN_MAX_RETRIES", 2);
    generation_config.retry_backoff_ms = ParseNonNegativeIntEnv("WAXCPP_LLAMA_GEN_RETRY_BACKOFF_MS", 100);
    if (generation_client_override) {
        generation_client_ = std::move(generation_client_override);
    } else {
        generation_client_ = std::make_unique<LlamaCppGenerationClient>(std::move(generation_config));
    }

    std::cerr << "[INIT-TRACE] >> MemoryOrchestrator ctor (store=" << store_path << ")..." << std::endl;
    orchestrator_ = std::make_unique<waxcpp::MemoryOrchestrator>(store_path, config, std::move(embedder));
    std::cerr << "[INIT-TRACE] << MemoryOrchestrator ctor done" << std::endl;

    // Parse auto-flush interval from env (0 disables auto-flush).
    auto_flush_interval_ms_ = static_cast<std::uint64_t>(
        ParseNonNegativeIntEnv(kAutoFlushIntervalEnv,
                               static_cast<int>(kDefaultAutoFlushIntervalMs)));
    StartAutoFlushThread();
}

WaxRAGHandler::~WaxRAGHandler() {
    StopAutoFlushThread();
    std::thread worker_to_join{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index_cancel_flag_) {
            index_cancel_flag_->store(true, std::memory_order_relaxed);
        }
        if (index_job_manager_.status().state == IndexJobState::kRunning) {
            (void)index_job_manager_.Stop();
        }
        if (index_worker_.joinable()) {
            worker_to_join = std::move(index_worker_);
        }
        index_cancel_flag_.reset();
    }
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }
    // Graceful shutdown: commit any uncommitted data (staged facts, WAL)
    // so that Ctrl+C doesn't lose work.
    try {
        orchestrator_->Flush();
        std::cerr << "[shutdown] final flush completed" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[shutdown] final flush failed: " << e.what() << std::endl;
    }
}

// ── Auto-flush background thread ────────────────────────────────

void WaxRAGHandler::StartAutoFlushThread() {
    if (auto_flush_interval_ms_ == 0) {
        std::cerr << "[auto-flush] disabled (interval=0)" << std::endl;
        return;
    }
    auto_flush_stop_.store(false, std::memory_order_relaxed);
    auto_flush_thread_ = std::thread([this]() { AutoFlushLoop(); });
    std::cerr << "[auto-flush] started (interval=" << auto_flush_interval_ms_ << "ms)" << std::endl;
}

void WaxRAGHandler::StopAutoFlushThread() {
    auto_flush_stop_.store(true, std::memory_order_relaxed);
    if (auto_flush_thread_.joinable()) {
        auto_flush_thread_.join();
    }
}

void WaxRAGHandler::AutoFlushLoop() {
    // Sleep in small increments so we can exit quickly on shutdown.
    constexpr std::uint64_t kSleepGranularityMs = 500;

    while (!auto_flush_stop_.load(std::memory_order_relaxed)) {
        // Sleep for the configured interval, checking for stop every 500ms.
        std::uint64_t slept_ms = 0;
        while (slept_ms < auto_flush_interval_ms_ &&
               !auto_flush_stop_.load(std::memory_order_relaxed)) {
            const auto chunk = std::min(kSleepGranularityMs,
                                        auto_flush_interval_ms_ - slept_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            slept_ms += chunk;
        }

        if (auto_flush_stop_.load(std::memory_order_relaxed)) {
            break;
        }

        // Skip auto-flush while index job is running — it controls flush itself
        // via flush_every_chunks to avoid dead-TOC bloat.
        if (index_active_.load(std::memory_order_relaxed)) {
            continue;
        }

        // Check if there has been write activity since our last flush.
        const auto last_write_ms = orchestrator_->last_write_activity_ms();
        if (last_write_ms <= 0 || last_write_ms <= auto_flush_last_flushed_ms_) {
            continue;  // nothing new to flush
        }

        // Acquire handler-level mutex and flush.
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            // Re-check after acquiring lock.
            if (index_active_.load(std::memory_order_relaxed)) {
                continue;  // index job started while we waited for lock
            }
            const auto recheck_ms = orchestrator_->last_write_activity_ms();
            if (recheck_ms > 0 && recheck_ms > auto_flush_last_flushed_ms_) {
                orchestrator_->Flush();
                auto_flush_last_flushed_ms_ = recheck_ms;
                ServerLog("auto-flush completed");
            }
        } catch (const std::exception& e) {
            std::cerr << "[auto-flush] ERROR: " << e.what() << std::endl;
        }
    }
}

std::string WaxRAGHandler::handle_remember(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string content = (params.isNull() ? "" : params->optValue<std::string>("content", ""));
    if (content.empty()) {
        return "Missing required parameter 'content'";
    }

    waxcpp::Metadata metadata_map{};
    if (!params.isNull() && params->has("metadata")) {
        Poco::JSON::Object::Ptr metadata;
        try {
            metadata = params->getObject("metadata");
        } catch (const Poco::Exception&) {
            metadata = nullptr;
        }

        if (!metadata.isNull()) {
            for (const auto& [key, value] : *metadata) {
                try {
                    metadata_map[key] = value.convert<std::string>();
                } catch (const Poco::Exception&) {
                    // Ignore non-string metadata values for deterministic behavior.
                }
            }
        }
    }

    try {
        orchestrator_->Remember(content, metadata_map);
        return "OK";
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_recall(const Poco::JSON::Object::Ptr& params) {
    std::cerr << "[RECALL] waiting for mutex..." << std::endl;
    std::lock_guard<std::mutex> lock(mutex_);
    std::cerr << "[RECALL] mutex acquired" << std::endl;

    const std::string query = (params.isNull() ? "" : params->optValue<std::string>("query", ""));
    if (query.empty()) {
        return "Missing required parameter 'query'";
    }

    try {
        std::cerr << "[RECALL] query=\"" << query << "\"" << std::endl;
        const auto t0 = std::chrono::steady_clock::now();
        const auto context = orchestrator_->Recall(query);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::cerr << "[RECALL] Recall() returned " << context.items.size()
                  << " items, " << context.total_tokens << " tokens in "
                  << elapsed_ms << " ms" << std::endl;

        Poco::JSON::Array items_array;
        for (const auto& item : context.items) {
            Poco::JSON::Object::Ptr row = new Poco::JSON::Object();
            row->set("kind", static_cast<int>(item.kind));
            row->set("text", item.text);
            row->set("score", item.score);
            items_array.add(row);
        }

        Poco::JSON::Object response;
        response.set("items", items_array);
        response.set("count", static_cast<int>(context.items.size()));
        response.set("total_tokens", context.total_tokens);

        std::ostringstream out;
        response.stringify(out);
        const auto result_json = out.str();

        // ── File logging for recall analysis ──
        static const auto recall_log_path = EnvString("WAXCPP_RECALL_LOG");
        if (recall_log_path.has_value() && !recall_log_path->empty()) {
            try {
                std::ofstream log_file(*recall_log_path, std::ios::app);
                if (log_file) {
                    const auto now = std::chrono::system_clock::now();
                    const auto now_t = std::chrono::system_clock::to_time_t(now);
                    char time_buf[64]{};
                    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now_t));

                    log_file << "════════════════════════════════════════\n"
                             << "TIME: " << time_buf << "  (" << elapsed_ms << "ms)\n"
                             << "QUERY: " << query << "\n"
                             << "ITEMS: " << context.items.size()
                             << "  TOKENS: " << context.total_tokens << "\n"
                             << "────────────────────────────────────────\n";
                    for (std::size_t i = 0; i < context.items.size(); ++i) {
                        const auto& item = context.items[i];
                        log_file << "[" << i << "] score=" << item.score
                                 << " kind=" << static_cast<int>(item.kind) << "\n"
                                 << item.text << "\n"
                                 << "────────────────────────────────────────\n";
                    }
                    log_file << "\n";
                }
            } catch (...) {
                // never fail the request due to logging
            }
        }

        return result_json;
    } catch (const std::exception& e) {
        std::cerr << "[RECALL] exception: " << e.what() << std::endl;
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_answer_generate(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string query = (params.isNull() ? "" : params->optValue<std::string>("query", ""));
    if (query.empty()) {
        return "Missing required parameter 'query'";
    }
    const int max_context_items = ParsePositiveIntParam(params, "max_context_items", 10);
    const int max_context_tokens = ParsePositiveIntParam(params, "max_context_tokens", 4000);
    const int max_output_tokens = ParsePositiveIntParam(params, "max_output_tokens", 768);
    const float temperature = ParseFloatParam(params, "temperature", 0.1F);
    const float top_p = ParseFloatParam(params, "top_p", 0.95F);

    try {
        const auto context = orchestrator_->Recall(query);
        const auto citations = BuildCitations(*orchestrator_, context, max_context_items);
        const auto prompt = BuildAnswerPrompt(query, context, citations, max_context_items, max_context_tokens);
        const auto answer = generation_client_->Generate(
            LlamaCppGenerationRequest{
                .prompt = prompt.prompt,
                .system_prompt = prompt.system_prompt,
                .max_tokens = max_output_tokens,
                .temperature = temperature,
                .top_p = top_p,
            });

        Poco::JSON::Object response{};
        response.set("query", query);
        response.set("answer", answer);
        response.set("model", runtime_models_.generation_model.model_path);
        response.set("total_context_tokens", context.total_tokens);
        response.set("context_items_used", prompt.context_items_used);
        response.set("context_tokens_used", prompt.context_tokens_used);

        Poco::JSON::Array citations_json{};
        for (const auto& citation : citations) {
            Poco::JSON::Object citation_json{};
            citation_json.set("frame_id", citation.frame_id);
            citation_json.set("relative_path", citation.relative_path);
            citation_json.set("line_start", citation.line_start.value_or(0));
            citation_json.set("line_end", citation.line_end.value_or(0));
            citation_json.set("symbol", citation.symbol);
            citation_json.set("score", citation.score);
            citations_json.add(citation_json);
        }
        response.set("citations", citations_json);

        std::ostringstream out;
        response.stringify(out);
        return out.str();
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_flush(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)params;

    try {
        orchestrator_->Flush();
        return "OK";
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

void WaxRAGHandler::run_index_job(std::string repo_root,
                                  bool resume_requested,
                                  IndexRunOptions options,
                                  std::shared_ptr<std::atomic<bool>> cancel_flag) {
    auto is_cancelled = [&cancel_flag]() noexcept {
        return cancel_flag && cancel_flag->load(std::memory_order_relaxed);
    };

    // Suppress auto-flush while index job runs (it controls flush itself).
    index_active_.store(true, std::memory_order_relaxed);
    // RAII guard: always clear the flag when index job exits.
    struct IndexActiveGuard {
        std::atomic<bool>& flag;
        ~IndexActiveGuard() { flag.store(false, std::memory_order_relaxed); }
    } index_guard{index_active_};

    try {
        {
            std::ostringstream msg;
            msg << "index job started repo_root=" << repo_root
                << " resume=" << (resume_requested ? "true" : "false")
                << " flush_every_chunks=" << options.flush_every_chunks
                << " ingest_batch_size=" << options.ingest_batch_size
                << " max_files=" << options.max_files
                << " max_chunks=" << options.max_chunks
                << " max_ram_mb=" << options.max_ram_mb
                << " target_tokens=" << options.target_tokens
                << " overlap_tokens=" << options.overlap_tokens;
            ServerLog(msg.str());
        }
        const std::uint64_t max_ram_bytes = options.max_ram_mb * 1024ULL * 1024ULL;
        bool logged_missing_rss_probe = false;
        auto enforce_memory_cap = [&]() {
            if (max_ram_bytes == 0) {
                return;
            }
            const auto rss_bytes = CurrentProcessRSSBytes();
            if (!rss_bytes.has_value()) {
                if (!logged_missing_rss_probe) {
                    ServerLog("process RSS probe unavailable; max_ram_mb cap ignored");
                    logged_missing_rss_probe = true;
                }
                return;
            }
            if (*rss_bytes > max_ram_bytes) {
                std::ostringstream error;
                error << "index max_ram_mb exceeded: rss_bytes=" << *rss_bytes
                      << " limit_bytes=" << max_ram_bytes;
                throw std::runtime_error(error.str());
            }
        };

        enforce_memory_cap();
        (void)index_job_manager_.SetPhase("scanning");
        const auto repo_root_path = std::filesystem::path(repo_root);

        // Build per-job scanner with optional extension/exclude overrides.
        Ue5ScannerConfig scan_config{};
        if (!options.include_extensions.empty()) {
            scan_config.include_extensions = options.include_extensions;
        }
        if (!options.exclude_dirs.empty()) {
            scan_config.exclude_directory_names = options.exclude_dirs;
        }
        Ue5FilesystemScanner scanner{scan_config};
        auto entries = scanner.Scan(repo_root_path, is_cancelled);
        if (options.max_files > 0 && entries.size() > options.max_files) {
            entries.resize(static_cast<std::size_t>(options.max_files));
        }
        if (is_cancelled()) {
            ServerLog("index job cancelled during scan");
            return;
        }
        {
            std::ostringstream msg;
            msg << "scan completed files=" << entries.size();
            ServerLog(msg.str());
        }

        const auto running_status = index_job_manager_.status();
        auto manifest_path = running_status.checkpoint_path;
        manifest_path += ".scan_manifest";
        auto chunk_manifest_path = running_status.checkpoint_path;
        chunk_manifest_path += ".chunk_manifest";
        auto file_manifest_path = running_status.checkpoint_path;
        file_manifest_path += ".file_manifest";

        std::vector<Ue5FileDigest> previous_file_digests{};
        bool loaded_previous_file_manifest = false;
        if (resume_requested) {
            std::error_code ec;
            if (std::filesystem::exists(file_manifest_path, ec) && !ec) {
                loaded_previous_file_manifest = true;
                previous_file_digests = Ue5ChunkManifestBuilder::ParseFileManifest(ReadFileText(file_manifest_path));
            }
        }

        // Apply per-job chunking overrides (target_tokens / overlap_tokens).
        Ue5ChunkingConfig chunking_config{};
        if (options.target_tokens > 0) {
            chunking_config.strategy.target_tokens = options.target_tokens;
            // Default overlap = 10% of target unless explicitly overridden.
            chunking_config.strategy.overlap_tokens = (options.overlap_tokens >= 0)
                ? options.overlap_tokens
                : options.target_tokens / 10;
        } else if (options.overlap_tokens >= 0) {
            chunking_config.strategy.overlap_tokens = options.overlap_tokens;
        }
        Ue5ChunkManifestBuilder chunk_builder{chunking_config};

        std::vector<Ue5FileDigest> current_file_digests{};
        const auto chunk_records = chunk_builder.Build(repo_root_path, entries, {}, &current_file_digests);
        const auto unchanged_paths =
            Ue5ChunkManifestBuilder::ComputeUnchangedPaths(previous_file_digests, current_file_digests);
        // Count chunks that belong to changed files only.
        std::uint64_t chunks_to_process = 0;
        if (resume_requested && loaded_previous_file_manifest && !unchanged_paths.empty()) {
            for (const auto& rec : chunk_records) {
                if (!unchanged_paths.contains(rec.relative_path)) {
                    ++chunks_to_process;
                }
            }
        } else {
            chunks_to_process = static_cast<std::uint64_t>(chunk_records.size());
        }
        // Count changed files for logging.
        std::uint64_t changed_file_count = current_file_digests.size() - unchanged_paths.size();
        {
            std::ostringstream msg;
            msg << "chunk manifest prepared total_chunks=" << chunk_records.size()
                << " chunks_to_process=" << chunks_to_process
                << " unchanged_files=" << unchanged_paths.size()
                << " changed_files=" << changed_file_count
                << " previous_digests=" << previous_file_digests.size()
                << " current_digests=" << current_file_digests.size()
                << " loaded_manifest=" << (loaded_previous_file_manifest ? "true" : "false");
            ServerLog(msg.str());
            // Log first 5 changed files for diagnostics.
            int changed_logged = 0;
            for (const auto& digest : current_file_digests) {
                if (!unchanged_paths.contains(digest.relative_path) && changed_logged < 5) {
                    // Find previous digest for comparison.
                    std::string prev_info = "(new file)";
                    for (const auto& prev : previous_file_digests) {
                        if (prev.relative_path == digest.relative_path) {
                            prev_info = "prev_size=" + std::to_string(prev.size_bytes) +
                                        " prev_hash=" + prev.content_hash;
                            break;
                        }
                    }
                    std::ostringstream dmsg;
                    dmsg << "CHANGED: " << digest.relative_path
                         << " cur_size=" << digest.size_bytes
                         << " cur_hash=" << digest.content_hash
                         << " " << prev_info;
                    ServerLog(dmsg.str());
                    ++changed_logged;
                }
            }
        }
        const std::uint64_t resume_committed_watermark =
            (resume_requested && !loaded_previous_file_manifest) ? running_status.committed_chunks : 0;
        std::uint64_t remaining_resume_skip_chunks = resume_committed_watermark;
        if (resume_committed_watermark > 0) {
            std::ostringstream msg;
            msg << "resume committed watermark active committed_chunks=" << resume_committed_watermark;
            ServerLog(msg.str());
        }
        (void)index_job_manager_.SetPhase("ingesting");
        // For resume with file manifest, report only chunks needing processing.
        index_job_manager_.SetTotalChunks(chunks_to_process);

        std::uint64_t indexed_chunks = 0;
        std::uint64_t committed_chunks = 0;
        bool reached_chunk_limit = false;

        // ── Enrichment pipeline (optional) ──
        EnricherPipeline enricher_pipeline;
        if (options.enrich_regex) {
            enricher_pipeline.AddEnricher(std::make_unique<RegexUe5Enricher>());
            ServerLog("enricher: regex_ue5 enabled");
        }
        if (options.enrich_llm && generation_client_) {
            LlmFactEnricherConfig llm_cfg{};
            llm_cfg.max_tokens = ParsePositiveIntEnv("WAXCPP_ENRICH_LLM_MAX_TOKENS", llm_cfg.max_tokens);
            llm_cfg.total_chunks = chunks_to_process;
            enricher_pipeline.AddEnricher(std::make_unique<LlmFactEnricher>(generation_client_.get(), llm_cfg));
            ServerLog(std::string("enricher: llm enabled, max_tokens=") + std::to_string(llm_cfg.max_tokens));
        }
        std::uint64_t facts_extracted = 0;

        struct PendingIngestChunk {
            std::string text{};
            waxcpp::Metadata metadata{};
        };
        std::vector<PendingIngestChunk> pending_ingest{};
        std::vector<ExtractedFact> pending_facts{};
        pending_ingest.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(options.ingest_batch_size, 1024ULL)));
        auto flush_pending_ingest = [&]() {
            if (pending_ingest.empty() && pending_facts.empty()) {
                return;
            }
            bool should_report_progress = false;
            std::uint64_t progress_indexed = 0;
            std::uint64_t progress_committed = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& pending : pending_ingest) {
                    orchestrator_->Remember(pending.text, pending.metadata);
                    ++indexed_chunks;
                    if (options.max_chunks > 0 && indexed_chunks >= options.max_chunks) {
                        reached_chunk_limit = true;
                    }
                    if (indexed_chunks % options.flush_every_chunks == 0) {
                        orchestrator_->Flush();
                        committed_chunks = indexed_chunks;
                        should_report_progress = true;
                        progress_indexed = indexed_chunks;
                        progress_committed = committed_chunks;
                    }
                }
                // Emit enriched facts
                for (const auto& fact : pending_facts) {
                    orchestrator_->RememberFact(fact.entity, fact.attribute, fact.value, fact.metadata);
                    ++facts_extracted;
                }
                // For facts-only indexing (e.g. bpl_json with skip_text_index),
                // indexed_chunks stays 0 and the chunk-based flush never fires.
                // Use facts_extracted as a secondary flush trigger.
                if (indexed_chunks == 0 && facts_extracted > 0 &&
                    facts_extracted % options.flush_every_chunks == 0) {
                    orchestrator_->Flush();
                    should_report_progress = true;
                }
            }
            // Always update progress so index.status reflects reality.
            (void)index_job_manager_.UpdateProgress(static_cast<std::uint64_t>(entries.size()),
                                                    indexed_chunks,
                                                    committed_chunks);
            if (should_report_progress) {
                std::ostringstream msg;
                msg << "index progress indexed_chunks=" << progress_indexed
                    << " committed_chunks=" << progress_committed;
                if (facts_extracted > 0) {
                    msg << " facts_extracted=" << facts_extracted;
                }
                ServerLog(msg.str());
            }
            pending_ingest.clear();
            pending_facts.clear();
        };
        {
            const bool using_skip = resume_requested && !unchanged_paths.empty();
            std::ostringstream msg;
            msg << "ingest Build() starting skip_unchanged=" << (using_skip ? "true" : "false")
                << " skip_file_count=" << unchanged_paths.size();
            ServerLog(msg.str());
        }
        (void)chunk_builder.Build(
            repo_root_path,
            entries,
            [&](const Ue5ChunkRecord& chunk, std::string_view chunk_text) {
                if (reached_chunk_limit) {
                    return;
                }
                if (is_cancelled()) {
                    return;
                }
                enforce_memory_cap();
                if (remaining_resume_skip_chunks > 0) {
                    --remaining_resume_skip_chunks;
                    return;
                }
                if (options.max_chunks > 0) {
                    const std::uint64_t scheduled_chunks =
                        indexed_chunks + static_cast<std::uint64_t>(pending_ingest.size());
                    if (scheduled_chunks >= options.max_chunks) {
                        reached_chunk_limit = true;
                        return;
                    }
                }

                // bpl_json: facts-only indexing — skip raw text, only enrich.
                const bool skip_text_index = (chunk.language == "bpl_json");

                if (!skip_text_index) {
                    waxcpp::Metadata metadata{};
                    metadata["source_kind"] = "ue5_chunk";
                    metadata["repo_root"] = repo_root;
                    metadata["relative_path"] = chunk.relative_path;
                    metadata["language"] = chunk.language;
                    metadata["symbol"] = chunk.symbol;
                    metadata["line_start"] = std::to_string(chunk.line_start);
                    metadata["line_end"] = std::to_string(chunk.line_end);
                    metadata["chunk_id"] = chunk.chunk_id;
                    metadata["chunk_hash"] = chunk.content_hash;
                    metadata["token_estimate"] = std::to_string(chunk.token_estimate);
                    pending_ingest.push_back(PendingIngestChunk{
                        .text = std::string(chunk_text),
                        .metadata = std::move(metadata),
                    });
                }

                // ── Enrichment ──
                if (!enricher_pipeline.Empty()) {
                    auto facts = enricher_pipeline.EnrichAll(chunk, chunk_text);
                    for (auto& f : facts) {
                        pending_facts.push_back(std::move(f));
                    }
                }

                if (options.max_chunks > 0 &&
                    indexed_chunks + static_cast<std::uint64_t>(pending_ingest.size()) >= options.max_chunks) {
                    reached_chunk_limit = true;
                }
                if (pending_ingest.size() >= options.ingest_batch_size) {
                    flush_pending_ingest();
                }
            },
            nullptr,
            (resume_requested && !unchanged_paths.empty()) ? &unchanged_paths : nullptr);
        flush_pending_ingest();
        if (reached_chunk_limit) {
            ServerLog("index job reached max_chunks cap");
        }
        if (is_cancelled()) {
            ServerLog("index job cancelled during ingest");
            return;
        }

        // Final flush: covers both uncommitted text chunks and facts-only
        // indexing where indexed_chunks stays 0 but facts were emitted.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (indexed_chunks > committed_chunks || facts_extracted > 0) {
                orchestrator_->Flush();
                committed_chunks = indexed_chunks;
            }
        }
        (void)index_job_manager_.UpdateProgress(static_cast<std::uint64_t>(entries.size()),
                                                indexed_chunks,
                                                committed_chunks);
        if (is_cancelled()) {
            ServerLog("index job cancelled before manifest write");
            return;
        }

        (void)index_job_manager_.SetPhase("persisting_manifests");
        WriteFileText(manifest_path,
                      Ue5FilesystemScanner::SerializeManifest(entries));
        WriteFileText(chunk_manifest_path,
                      Ue5ChunkManifestBuilder::SerializeManifest(chunk_records));
        WriteFileText(file_manifest_path,
                      Ue5ChunkManifestBuilder::SerializeFileManifest(current_file_digests));
        if (is_cancelled()) {
            ServerLog("index job cancelled after manifest write");
            return;
        }

        (void)index_job_manager_.Complete(static_cast<std::uint64_t>(entries.size()), indexed_chunks, committed_chunks);
        {
            std::ostringstream msg;
            msg << "index job completed scanned_files=" << entries.size()
                << " indexed_chunks=" << indexed_chunks
                << " committed_chunks=" << committed_chunks;
            ServerLog(msg.str());
        }
    } catch (const std::exception& e) {
        if (!is_cancelled()) {
            (void)index_job_manager_.Fail(e.what());
            std::ostringstream msg;
            msg << "index job failed: " << e.what();
            ServerLog(msg.str());
        }
    }
}

void WaxRAGHandler::TryReapFinishedIndexWorker() {
    std::thread worker_to_join{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!index_worker_.joinable()) {
            if (index_cancel_flag_ && index_job_manager_.status().state != IndexJobState::kRunning) {
                index_cancel_flag_.reset();
            }
            return;
        }
        if (index_job_manager_.status().state == IndexJobState::kRunning) {
            return;
        }
        worker_to_join = std::move(index_worker_);
        index_cancel_flag_.reset();
    }
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }
}

std::string WaxRAGHandler::handle_index_start(const Poco::JSON::Object::Ptr& params) {
    const std::string repo_root = (params.isNull() ? "" : params->optValue<std::string>("repo_root", ""));
    if (repo_root.empty()) {
        return "Missing required parameter 'repo_root'";
    }
    const bool resume_requested = (params.isNull() ? false : params->optValue<bool>("resume", false));
    int flush_every_chunks_param = static_cast<int>(kIndexFlushEveryChunks);
    int ingest_batch_size_param = 1;
    int max_files_param = 0;
    int max_chunks_param = 0;
    int max_ram_mb_param = 0;

    const auto flush_raw = ParseIntParamStrict(params, "flush_every_chunks");
    if (flush_raw.present) {
        if (!flush_raw.valid) {
            return "Error: flush_every_chunks must be an integer";
        }
        flush_every_chunks_param = flush_raw.value;
    }
    const auto ingest_batch_size_raw = ParseIntParamStrict(params, "ingest_batch_size");
    if (ingest_batch_size_raw.present) {
        if (!ingest_batch_size_raw.valid) {
            return "Error: ingest_batch_size must be an integer";
        }
        ingest_batch_size_param = ingest_batch_size_raw.value;
    }
    const auto max_files_raw = ParseIntParamStrict(params, "max_files");
    if (max_files_raw.present) {
        if (!max_files_raw.valid) {
            return "Error: max_files must be an integer";
        }
        max_files_param = max_files_raw.value;
    }
    const auto max_chunks_raw = ParseIntParamStrict(params, "max_chunks");
    if (max_chunks_raw.present) {
        if (!max_chunks_raw.valid) {
            return "Error: max_chunks must be an integer";
        }
        max_chunks_param = max_chunks_raw.value;
    }
    const auto max_ram_mb_raw = ParseIntParamStrict(params, "max_ram_mb");
    if (max_ram_mb_raw.present) {
        if (!max_ram_mb_raw.valid) {
            return "Error: max_ram_mb must be an integer";
        }
        max_ram_mb_param = max_ram_mb_raw.value;
    }

    if (flush_every_chunks_param <= 0 ||
        static_cast<std::uint64_t>(flush_every_chunks_param) > kMaxIndexControlValue) {
        return "Error: flush_every_chunks must be within [1, 1000000]";
    }
    if (ingest_batch_size_param <= 0 ||
        static_cast<std::uint64_t>(ingest_batch_size_param) > kMaxIndexControlValue) {
        return "Error: ingest_batch_size must be within [1, 1000000]";
    }
    if (max_files_param < 0 || static_cast<std::uint64_t>(max_files_param) > kMaxIndexControlValue) {
        return "Error: max_files must be within [0, 1000000]";
    }
    if (max_chunks_param < 0 || static_cast<std::uint64_t>(max_chunks_param) > kMaxIndexControlValue) {
        return "Error: max_chunks must be within [0, 1000000]";
    }
    if (max_ram_mb_param < 0 || static_cast<std::uint64_t>(max_ram_mb_param) > kMaxIndexControlValue) {
        return "Error: max_ram_mb must be within [0, 1000000]";
    }

    // Chunking strategy overrides (0 / -1 = use defaults).
    int target_tokens_param = 0;
    int overlap_tokens_param = -1;
    const auto target_tokens_raw = ParseIntParamStrict(params, "target_tokens");
    if (target_tokens_raw.present) {
        if (!target_tokens_raw.valid) {
            return "Error: target_tokens must be an integer";
        }
        target_tokens_param = target_tokens_raw.value;
    }
    const auto overlap_tokens_raw = ParseIntParamStrict(params, "overlap_tokens");
    if (overlap_tokens_raw.present) {
        if (!overlap_tokens_raw.valid) {
            return "Error: overlap_tokens must be an integer";
        }
        overlap_tokens_param = overlap_tokens_raw.value;
    }
    if (target_tokens_param < 0 || target_tokens_param > 100000) {
        return "Error: target_tokens must be within [0, 100000]";
    }
    if (overlap_tokens_param < -1 || overlap_tokens_param > 50000) {
        return "Error: overlap_tokens must be within [-1, 50000]";
    }

    // Scanner overrides: include_extensions and exclude_dirs (JSON arrays of strings).
    auto parse_string_array = [&](const std::string& key) -> std::vector<std::string> {
        std::vector<std::string> result;
        if (params.isNull() || !params->has(key)) return result;
        const auto arr = params->getArray(key);
        if (!arr) return result;
        for (unsigned i = 0; i < arr->size(); ++i) {
            result.push_back(arr->getElement<std::string>(i));
        }
        return result;
    };
    auto include_extensions_param = parse_string_array("include_extensions");
    auto exclude_dirs_param = parse_string_array("exclude_dirs");

    // Enrichment flags: JSON-RPC param takes precedence, env var as fallback.
    auto env_bool = [](const char* name, bool fallback) -> bool {
        const char* val = std::getenv(name);
        if (val && (std::string(val) == "1" || std::string(val) == "true")) return true;
        if (val && (std::string(val) == "0" || std::string(val) == "false")) return false;
        return fallback;
    };
    const bool enrich_regex = params.isNull() ? env_bool("WAXCPP_ENRICH_REGEX", false)
                                              : params->optValue<bool>("enrich_regex", env_bool("WAXCPP_ENRICH_REGEX", false));
    const bool enrich_llm = params.isNull() ? env_bool("WAXCPP_ENRICH_LLM", false)
                                            : params->optValue<bool>("enrich_llm", env_bool("WAXCPP_ENRICH_LLM", false));

    const IndexRunOptions options{
        .flush_every_chunks = static_cast<std::uint64_t>(flush_every_chunks_param),
        .ingest_batch_size = static_cast<std::uint64_t>(ingest_batch_size_param),
        .max_files = static_cast<std::uint64_t>(max_files_param),
        .max_chunks = static_cast<std::uint64_t>(max_chunks_param),
        .max_ram_mb = static_cast<std::uint64_t>(max_ram_mb_param),
        .target_tokens = target_tokens_param,
        .overlap_tokens = overlap_tokens_param,
        .include_extensions = std::move(include_extensions_param),
        .exclude_dirs = std::move(exclude_dirs_param),
        .enrich_regex = enrich_regex,
        .enrich_llm = enrich_llm,
    };

    try {
        TryReapFinishedIndexWorker();

        std::shared_ptr<std::atomic<bool>> cancel_flag = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool started = index_job_manager_.Start(std::filesystem::path(repo_root), resume_requested);
            if (!started) {
                return "Error: index job is already running";
            }
            index_cancel_flag_ = cancel_flag;
            try {
                index_worker_ =
                    std::thread(&WaxRAGHandler::run_index_job, this, repo_root, resume_requested, options, cancel_flag);
            } catch (const std::exception& e) {
                index_cancel_flag_.reset();
                (void)index_job_manager_.Fail(e.what());
                return std::string("Error: failed to start index worker: ") + e.what();
            }
        }
        ServerLog("index.start accepted");
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_index_status(const Poco::JSON::Object::Ptr& params) {
    (void)params;
    try {
        TryReapFinishedIndexWorker();
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_index_stop(const Poco::JSON::Object::Ptr& params) {
    (void)params;
    try {
        std::thread worker_to_join{};
        bool had_running_worker = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            had_running_worker = index_worker_.joinable();
            if (index_cancel_flag_) {
                index_cancel_flag_->store(true, std::memory_order_relaxed);
            }
            const bool stopped = index_job_manager_.Stop();
            if (!stopped && !had_running_worker) {
                return "Error: index job is not running";
            }
            if (index_worker_.joinable()) {
                worker_to_join = std::move(index_worker_);
            }
            index_cancel_flag_.reset();
        }
        if (worker_to_join.joinable()) {
            worker_to_join.join();
        }
        ServerLog("index.stop completed");
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::make_index_status_json(const IndexJobStatus& status) const {
    Poco::JSON::Object response{};
    response.set("state", ToString(status.state));
    response.set("phase", status.phase);
    response.set("generation", status.generation);
    response.set("job_id", status.job_id.value_or(""));
    response.set("repo_root", status.repo_root.value_or(""));
    response.set("checkpoint_path", status.checkpoint_path.string());
    response.set("started_at_ms", status.started_at_ms);
    response.set("updated_at_ms", status.updated_at_ms);
    response.set("scanned_files", status.scanned_files);
    response.set("total_chunks", status.total_chunks);
    response.set("indexed_chunks", status.indexed_chunks);
    response.set("committed_chunks", status.committed_chunks);
    response.set("resume_requested", status.resume_requested);
    response.set("last_error", status.last_error.value_or(""));
    const std::uint64_t now_ms = NowMs();
    const std::uint64_t elapsed_ms = (status.started_at_ms > 0 && now_ms >= status.started_at_ms)
                                         ? (now_ms - status.started_at_ms)
                                         : 0;
    response.set("elapsed_ms", elapsed_ms);
    if (elapsed_ms > 0) {
        const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
        response.set("indexed_chunks_per_sec", static_cast<double>(status.indexed_chunks) / seconds);
        response.set("committed_chunks_per_sec", static_cast<double>(status.committed_chunks) / seconds);
    } else {
        response.set("indexed_chunks_per_sec", 0.0);
        response.set("committed_chunks_per_sec", 0.0);
    }
    if (const auto rss_bytes = CurrentProcessRSSBytes(); rss_bytes.has_value()) {
        response.set("process_rss_mb", static_cast<double>(*rss_bytes) / (1024.0 * 1024.0));
    } else {
        response.set("process_rss_mb", -1.0);
    }

    std::ostringstream out;
    response.stringify(out);
    return out.str();
}

// ── Blueprint round-trip helpers ─────────────────────────────

namespace {

/// Convert a UE Blueprint asset path to the export filename.
/// /Game/Blueprints/BP_MyActor.BP_MyActor → _Game_Blueprints_BP_MyActor_BP_MyActor.bpl_json
std::string BlueprintPathToFilename(const std::string& blueprint_path) {
    std::string name = blueprint_path;
    std::replace(name.begin(), name.end(), '/', '_');
    std::replace(name.begin(), name.end(), '.', '_');
    if (!name.empty() && name[0] != '_') {
        name = "_" + name;
    }
    return name + ".bpl_json";
}

}  // namespace

// ── blueprint.read ──────────────────────────────────────────

std::string WaxRAGHandler::handle_blueprint_read(const Poco::JSON::Object::Ptr& params) {
    const std::string blueprint_path =
        (params.isNull() ? "" : params->optValue<std::string>("blueprint_path", ""));
    if (blueprint_path.empty()) {
        return "{\"error\":\"Missing required parameter 'blueprint_path'\"}";
    }
    const std::string export_dir =
        (params.isNull() ? "" : params->optValue<std::string>("export_dir", ""));
    if (export_dir.empty()) {
        return "{\"error\":\"Missing required parameter 'export_dir'\"}";
    }

    try {
        const auto filename = BlueprintPathToFilename(blueprint_path);
        const auto file_path = std::filesystem::path(export_dir) / filename;

        if (!std::filesystem::exists(file_path)) {
            return "{\"error\":\"File not found: " + JsonEscape(file_path.string()) + "\"}";
        }

        const auto content = ReadFileText(file_path);

        Poco::JSON::Object response;
        response.set("json", content);
        response.set("file_path", file_path.string());
        response.set("size_bytes", static_cast<std::int64_t>(content.size()));

        std::ostringstream out;
        response.stringify(out);
        return out.str();

    } catch (const std::exception& e) {
        return "{\"error\":\"" + JsonEscape(e.what()) + "\"}";
    }
}

// ── blueprint.write ─────────────────────────────────────────

std::string WaxRAGHandler::handle_blueprint_write(const Poco::JSON::Object::Ptr& params) {
    const std::string blueprint_path =
        (params.isNull() ? "" : params->optValue<std::string>("blueprint_path", ""));
    if (blueprint_path.empty()) {
        return "{\"error\":\"Missing required parameter 'blueprint_path'\"}";
    }
    const std::string export_dir =
        (params.isNull() ? "" : params->optValue<std::string>("export_dir", ""));
    if (export_dir.empty()) {
        return "{\"error\":\"Missing required parameter 'export_dir'\"}";
    }
    const std::string json_content =
        (params.isNull() ? "" : params->optValue<std::string>("json", ""));
    if (json_content.empty()) {
        return "{\"error\":\"Missing required parameter 'json'\"}";
    }

    try {
        // Basic validation: must be parseable JSON with a "blueprint" key.
        Poco::JSON::Parser parser;
        const auto parsed = parser.parse(json_content);
        const auto obj = parsed.extract<Poco::JSON::Object::Ptr>();
        if (obj.isNull() || !obj->has("blueprint")) {
            return "{\"error\":\"Invalid Blueprint JSON: must be an object with a 'blueprint' key\"}";
        }

        const auto filename = BlueprintPathToFilename(blueprint_path);
        const auto file_path = std::filesystem::path(export_dir) / filename;

        // Create backup if file already exists.
        if (std::filesystem::exists(file_path)) {
            const auto stem = std::filesystem::path(filename).stem().string();
            const auto backup_path = std::filesystem::path(export_dir) /
                (stem + ".backup.bpl_json");
            std::filesystem::copy_file(file_path, backup_path,
                std::filesystem::copy_options::overwrite_existing);
            std::cerr << "[BLUEPRINT] backup: " << backup_path.string() << "\n";
        }

        WriteFileText(file_path, json_content);
        std::cerr << "[BLUEPRINT] wrote " << json_content.size()
                  << " bytes to " << file_path.string() << "\n";

        Poco::JSON::Object response;
        response.set("file_path", file_path.string());
        response.set("bytes_written", static_cast<std::int64_t>(json_content.size()));

        std::ostringstream out;
        response.stringify(out);
        return out.str();

    } catch (const Poco::Exception& e) {
        return "{\"error\":\"Invalid JSON: " + JsonEscape(e.displayText()) + "\"}";
    } catch (const std::exception& e) {
        return "{\"error\":\"" + JsonEscape(e.what()) + "\"}";
    }
}

// ── blueprint.import ────────────────────────────────────────

std::string WaxRAGHandler::handle_blueprint_import(const Poco::JSON::Object::Ptr& params) {
    const std::string ue_editor =
        (params.isNull() ? "" : params->optValue<std::string>("ue_editor", ""));
    if (ue_editor.empty()) {
        return "{\"error\":\"Missing required parameter 'ue_editor'\"}";
    }
    const std::string uproject =
        (params.isNull() ? "" : params->optValue<std::string>("uproject", ""));
    if (uproject.empty()) {
        return "{\"error\":\"Missing required parameter 'uproject'\"}";
    }
    const std::string import_dir =
        (params.isNull() ? "" : params->optValue<std::string>("import_dir", ""));
    if (import_dir.empty()) {
        return "{\"error\":\"Missing required parameter 'import_dir'\"}";
    }

    // Optional flags (all default to true).
    const bool clear_graph = (params.isNull() ? true : params->optValue<bool>("clear_graph", true));
    const bool compile = (params.isNull() ? true : params->optValue<bool>("compile", true));
    const bool save = (params.isNull() ? true : params->optValue<bool>("save", true));

    // Validate paths exist.
    if (!std::filesystem::exists(ue_editor)) {
        return "{\"error\":\"ue_editor not found: " + JsonEscape(ue_editor) + "\"}";
    }
    if (!std::filesystem::exists(uproject)) {
        return "{\"error\":\"uproject not found: " + JsonEscape(uproject) + "\"}";
    }
    if (!std::filesystem::exists(import_dir)) {
        return "{\"error\":\"import_dir not found: " + JsonEscape(import_dir) + "\"}";
    }

    try {
        Poco::Process::Args args;
        args.push_back(uproject);
        args.push_back("-run=BlueprintGraphImport");
        args.push_back("-ImportDir=" + import_dir);
        args.push_back(std::string("-ClearGraph=") + (clear_graph ? "1" : "0"));
        args.push_back(std::string("-Compile=") + (compile ? "1" : "0"));
        args.push_back(std::string("-Save=") + (save ? "1" : "0"));
        args.push_back("-unattended");
        args.push_back("-nop4");

        std::cerr << "[BLUEPRINT] launching: " << ue_editor;
        for (const auto& arg : args) {
            std::cerr << " " << arg;
        }
        std::cerr << "\n" << std::flush;

        const auto t0 = std::chrono::steady_clock::now();

        Poco::Pipe out_pipe, err_pipe;
        auto handle = Poco::Process::launch(
            ue_editor, args, nullptr, &out_pipe, &err_pipe);

        // Read stdout and stderr.
        Poco::PipeInputStream out_stream(out_pipe);
        Poco::PipeInputStream err_stream(err_pipe);

        std::string stdout_str(
            (std::istreambuf_iterator<char>(out_stream)),
            std::istreambuf_iterator<char>());
        std::string stderr_str(
            (std::istreambuf_iterator<char>(err_stream)),
            std::istreambuf_iterator<char>());

        const int exit_code = handle.wait();

        const auto t1 = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::cerr << "[BLUEPRINT] import finished: exit_code=" << exit_code
                  << " elapsed=" << elapsed_ms << "ms"
                  << " stdout=" << stdout_str.size() << " bytes"
                  << " stderr=" << stderr_str.size() << " bytes\n";

        // Truncate large output to avoid oversized JSON-RPC responses.
        constexpr std::size_t kMaxOutputChars = 32000;
        if (stdout_str.size() > kMaxOutputChars) {
            stdout_str = stdout_str.substr(0, kMaxOutputChars) +
                "\n... (truncated, " + std::to_string(stdout_str.size() - kMaxOutputChars) + " more chars)";
        }
        if (stderr_str.size() > kMaxOutputChars) {
            stderr_str = stderr_str.substr(0, kMaxOutputChars) +
                "\n... (truncated, " + std::to_string(stderr_str.size() - kMaxOutputChars) + " more chars)";
        }

        Poco::JSON::Object response;
        response.set("exit_code", exit_code);
        response.set("stdout", stdout_str);
        response.set("stderr", stderr_str);
        response.set("elapsed_ms", static_cast<std::int64_t>(elapsed_ms));

        std::ostringstream out;
        response.stringify(out);
        return out.str();

    } catch (const std::exception& e) {
        std::cerr << "[BLUEPRINT] import error: " << e.what() << "\n";
        return "{\"error\":\"" + JsonEscape(e.what()) + "\"}";
    }
}

// ── fact.search ─────────────────────────────────────────────

std::string WaxRAGHandler::handle_fact_search(const Poco::JSON::Object::Ptr& params) {
    const std::string entity_prefix =
        (params.isNull() ? "" : params->optValue<std::string>("entity_prefix", ""));
    if (entity_prefix.empty()) {
        return "{\"error\":\"Missing required parameter 'entity_prefix'\"}";
    }
    const int limit = (params.isNull() ? 100 : params->optValue<int>("limit", 100));

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto facts = orchestrator_->RecallFactsByEntityPrefix(entity_prefix, limit);

        Poco::JSON::Array facts_array;
        for (const auto& fact : facts) {
            Poco::JSON::Object::Ptr row = new Poco::JSON::Object();
            row->set("entity", fact.entity);
            row->set("attribute", fact.attribute);
            row->set("value", fact.value);
            row->set("id", static_cast<std::int64_t>(fact.id));
            facts_array.add(row);
        }

        Poco::JSON::Object response;
        response.set("facts", facts_array);
        response.set("count", static_cast<int>(facts.size()));
        response.set("entity_prefix", entity_prefix);

        std::ostringstream out;
        response.stringify(out);
        return out.str();

    } catch (const std::exception& e) {
        return "{\"error\":\"" + JsonEscape(e.what()) + "\"}";
    }
}

}  // namespace waxcpp::server
