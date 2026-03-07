#pragma once

#include "waxcpp/access_stats.hpp"
#include "waxcpp/answer_extractor.hpp"
#include "waxcpp/embedding_memoizer.hpp"
#include "waxcpp/embeddings.hpp"
#include "waxcpp/fts5_search_engine.hpp"
#include "waxcpp/maintenance.hpp"
#include "waxcpp/tier_selector.hpp"
#include "waxcpp/token_counter.hpp"
#include "waxcpp/types.hpp"
#include "waxcpp/structured_memory.hpp"
#include "waxcpp/vector_engine.hpp"
#include "waxcpp/wax_store.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

class MemoryOrchestrator {
 public:
  MemoryOrchestrator(const std::filesystem::path& path,
                     const OrchestratorConfig& config,
                     std::shared_ptr<EmbeddingProvider> embedder = nullptr,
                     const TokenCounter* token_counter = nullptr);

  // ── Ingest ─────────────────────────────────────────────────
  void Remember(const std::string& content, const Metadata& metadata = {});
  void RememberFile(const std::filesystem::path& file_path, const Metadata& metadata = {});

  // ── Recall (RAG context assembly) ──────────────────────────
  RAGContext Recall(const std::string& query);
  RAGContext Recall(const std::string& query, const std::vector<float>& embedding);
  RAGContext Recall(const std::string& query, const FrameFilter& frame_filter);
  RAGContext Recall(const std::string& query, QueryEmbeddingPolicy policy);
  std::optional<WaxFrameMeta> FrameMeta(std::uint64_t frame_id, bool include_pending = false) const;
  std::vector<WaxFrameMeta> FrameMetas(const std::vector<std::uint64_t>& frame_ids,
                                       bool include_pending = false) const;

  // ── Structured memory (v1 entity/attribute/value) ──────────
  void RememberFact(const std::string& entity,
                    const std::string& attribute,
                    const std::string& value,
                    const Metadata& metadata = {});
  bool ForgetFact(const std::string& entity, const std::string& attribute);
  std::vector<StructuredMemoryEntry> RecallFactsByEntityPrefix(const std::string& entity_prefix, int limit = 32);

  // ── Direct search ──────────────────────────────────────────
  std::vector<MemorySearchHit> Search(
      const std::string& query,
      DirectSearchMode mode = DirectSearchMode::kHybrid,
      float hybrid_alpha = 0.5f,
      int top_k = 10);

  std::vector<MemorySearchHit> Search(
      const std::string& query,
      const FrameFilter& frame_filter,
      DirectSearchMode mode = DirectSearchMode::kHybrid,
      float hybrid_alpha = 0.5f,
      int top_k = 10);

  // ── Runtime stats ──────────────────────────────────────────
  RuntimeStats GetRuntimeStats() const;
  SessionRuntimeStats GetSessionRuntimeStats() const;

  // ── Session management ─────────────────────────────────────
  std::string StartSession();
  void EndSession();
  std::string ActiveSessionId() const;

  // ── Handoff ────────────────────────────────────────────────
  /// Store a handoff record (session summary with pending tasks).
  /// Returns the frame ID of the stored handoff.
  std::uint64_t RememberHandoff(
      const std::string& content,
      const std::optional<std::string>& project = std::nullopt,
      const std::vector<std::string>& pending_tasks = {});

  /// Retrieve the most recent handoff record, optionally filtered by project.
  std::optional<HandoffRecord> LatestHandoff(
      const std::optional<std::string>& project = std::nullopt) const;

  // ── Maintenance ────────────────────────────────────────────
  MaintenanceReport OptimizeSurrogates(const MaintenanceOptions& options = {});
  MaintenanceReport CompactIndexes();

  void Flush();
  void Close();

  /// Access the frame access stats manager (thread-safe).
  AccessStatsManager& GetAccessStats() { return access_stats_; }
  const AccessStatsManager& GetAccessStats() const { return access_stats_; }

  /// Returns the last scheduled live-set maintenance report (if any).
  std::optional<ScheduledLiveSetMaintenanceReport> LastMaintenanceReport() const;

  /// Returns true if the FTS5 SQLite full-text search backend is active.
  [[nodiscard]] bool IsFts5Active() const { return store_text_index_.IsFts5Active(); }

  /// Returns the epoch-ms timestamp of the last Remember/RememberFact call (0 if none).
  /// Thread-safe (acquires internal mutex).
  [[nodiscard]] std::int64_t last_write_activity_ms() const;

 private:
  OrchestratorConfig config_;
  WaxStore store_;
  std::shared_ptr<EmbeddingProvider> embedder_;
  const TokenCounter* token_counter_ = nullptr;
  EmbeddingMemoizer embedding_cache_;
  StructuredMemoryStore structured_memory_;
  FTS5SearchEngine store_text_index_;
  FTS5SearchEngine structured_text_index_;
  std::unique_ptr<USearchVectorEngine> vector_index_;
  AccessStatsManager access_stats_;
  SurrogateTierSelector tier_selector_;
  DeterministicAnswerExtractor answer_extractor_;
  /// Maps source frame ID → active surrogate frame ID.
  std::unordered_map<std::uint64_t, std::uint64_t> surrogate_map_;

  /// Session tagging.
  std::string current_session_id_;

  /// Maintenance bookkeeping.
  std::uint64_t flush_count_ = 0;
  std::int64_t last_write_activity_ms_ = 0;
  std::int64_t last_maintenance_completed_ms_ = 0;
  std::optional<ScheduledLiveSetMaintenanceReport> last_maintenance_report_;

  bool closed_ = false;
  mutable std::mutex mutex_{};

  /// Internal: shared Recall implementation with optional frame filter and policy override.
  RAGContext RecallImpl(const std::string& query,
                        const std::optional<std::vector<float>>& explicit_embedding,
                        const std::optional<FrameFilter>& frame_filter,
                        std::optional<QueryEmbeddingPolicy> policy_override);
};

}  // namespace waxcpp
