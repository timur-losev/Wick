// cpp/server/wax_rag_handler.hpp
#pragma once

#include "../include/waxcpp/memory_orchestrator.hpp"
#include "../include/waxcpp/runtime_model_config.hpp"
#include "index_job_manager.hpp"
#include "json_rpc.hpp"
#include "llama_cpp_generation_client.hpp"
#include "llama_cpp_embedding_provider.hpp"
#include "ue5_chunk_manifest.hpp"
#include "ue5_filesystem_scanner.hpp"

#include <Poco/JSON/Object.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

namespace waxcpp::server {

class WaxRAGHandler {
public:
    explicit WaxRAGHandler(
        const std::filesystem::path& store_path = "data/wax-server.mv2s",
        waxcpp::RuntimeModelsConfig runtime_models = {},
        std::unique_ptr<LlamaCppGenerationClient> generation_client_override = nullptr);
    ~WaxRAGHandler();
    
    // Обработчики JSON-RPC методов
    std::string handle_remember(const Poco::JSON::Object::Ptr& params);
    std::string handle_recall(const Poco::JSON::Object::Ptr& params);
    std::string handle_answer_generate(const Poco::JSON::Object::Ptr& params);
    std::string handle_flush(const Poco::JSON::Object::Ptr& params);
    std::string handle_index_start(const Poco::JSON::Object::Ptr& params);
    std::string handle_index_status(const Poco::JSON::Object::Ptr& params);
    std::string handle_index_stop(const Poco::JSON::Object::Ptr& params);

    // Blueprint round-trip methods
    std::string handle_blueprint_read(const Poco::JSON::Object::Ptr& params);
    std::string handle_blueprint_write(const Poco::JSON::Object::Ptr& params);
    std::string handle_blueprint_import(const Poco::JSON::Object::Ptr& params);

    // Fact inspection
    std::string handle_fact_search(const Poco::JSON::Object::Ptr& params);

    /// Returns true if the FTS5 SQLite full-text search backend is active.
    [[nodiscard]] bool IsFts5Active() const { return orchestrator_ && orchestrator_->IsFts5Active(); }

private:
    struct IndexRunOptions {
        std::uint64_t flush_every_chunks = 128;
        std::uint64_t ingest_batch_size = 1;
        std::uint64_t max_files = 0;
        std::uint64_t max_chunks = 0;
        std::uint64_t max_ram_mb = 0;
        int target_tokens = 0;       // 0 = use default (400)
        int overlap_tokens = -1;     // -1 = use default (40)
        std::vector<std::string> include_extensions{};  // empty = use scanner defaults
        std::vector<std::string> exclude_dirs{};        // empty = use scanner defaults
        bool enrich_regex = false;
        bool enrich_llm = false;
    };

    void run_index_job(std::string repo_root,
                       bool resume_requested,
                       IndexRunOptions options,
                       std::shared_ptr<std::atomic<bool>> cancel_flag);
    void TryReapFinishedIndexWorker();
    std::string make_index_status_json(const IndexJobStatus& status) const;

    // ── Auto-flush background timer ────────────────────────────
    void StartAutoFlushThread();
    void StopAutoFlushThread();
    void AutoFlushLoop();

    std::unique_ptr<waxcpp::MemoryOrchestrator> orchestrator_;
    std::unique_ptr<LlamaCppGenerationClient> generation_client_;
    IndexJobManager index_job_manager_;
    Ue5FilesystemScanner ue5_scanner_{};
    Ue5ChunkManifestBuilder ue5_chunk_builder_{};
    waxcpp::RuntimeModelsConfig runtime_models_{};
    std::thread index_worker_{};
    std::shared_ptr<std::atomic<bool>> index_cancel_flag_{};
    std::mutex mutex_;

    // ── Auto-flush state ───────────────────────────────────────
    std::thread auto_flush_thread_{};
    std::atomic<bool> auto_flush_stop_{false};
    std::atomic<bool> index_active_{false};  // suppresses auto-flush during indexing
    std::int64_t auto_flush_last_flushed_ms_{0};  // guarded by auto_flush_thread_ only
    std::uint64_t auto_flush_interval_ms_{30000};  // default 30s
};

}  // namespace waxcpp::server
