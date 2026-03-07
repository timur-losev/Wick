#pragma once

#include "waxcpp/live_set_rewrite.hpp"
#include "waxcpp/tier_selector.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace waxcpp {

using Metadata = std::unordered_map<std::string, std::string>;

enum class SearchSource {
  kText,
  kVector,
  kTimeline,
  kStructuredMemory,
};

enum class SearchModeKind {
  kTextOnly,
  kVectorOnly,
  kHybrid,
};

struct SearchMode {
  SearchModeKind kind = SearchModeKind::kTextOnly;
  float alpha = 0.5f;
};

enum class VectorEnginePreference {
  kAuto,
  kMetalPreferred,
  kCpuOnly,
};

// ── Time range ───────────────────────────────────────────────

/// Temporal filter for search results.
struct TimeRange {
  std::optional<std::int64_t> after_ms;
  std::optional<std::int64_t> before_ms;

  bool Contains(std::int64_t timestamp_ms) const {
    if (after_ms && timestamp_ms < *after_ms) return false;
    if (before_ms && timestamp_ms >= *before_ms) return false;
    return true;
  }
};

// ── Frame filter ─────────────────────────────────────────────

/// Metadata-based predicate for search candidate filtering.
/// Per-frame metadata and labels are persisted in the MV2S binary format
/// and available via WaxFrameMeta after read-back.
struct MetadataFilter {
  std::unordered_map<std::string, std::string> required_entries;
  std::vector<std::pair<std::string, std::string>> required_tags;
  std::vector<std::string> required_labels;
};

/// Frame filter predicate applied to search/recall results.
struct FrameFilter {
  bool include_deleted = false;
  bool include_superseded = false;
  bool include_surrogates = false;
  std::optional<std::unordered_set<std::uint64_t>> frame_ids;
  std::optional<MetadataFilter> metadata_filter;
};

// ── Structured memory search options ─────────────────────────

/// Options for the structured-memory lane in unified search.
struct StructuredMemorySearchOptions {
  float weight = 0.2f;
  int max_entity_candidates = 16;
  int max_facts = 64;
  int max_evidence_frames = 32;
  bool require_evidence_span = false;
};

// ── Search request ───────────────────────────────────────────

struct SearchRequest {
  std::optional<std::string> query;
  std::optional<std::vector<float>> embedding;
  VectorEnginePreference vector_preference = VectorEnginePreference::kAuto;
  SearchMode mode{};
  int top_k = 10;
  std::optional<float> min_score;
  std::optional<TimeRange> time_range;
  std::optional<FrameFilter> frame_filter;
  std::int64_t as_of_ms = INT64_MAX;
  StructuredMemorySearchOptions structured_memory{};

  int max_snippets = 24;
  int rrf_k = 60;
  int preview_max_bytes = 512;
  int expansion_max_tokens = 600;
  int max_context_tokens = 1500;
  int snippet_max_tokens = 200;

  /// Threshold for switching between lazy and batch metadata loading.
  int metadata_loading_threshold = 50;
  bool allow_timeline_fallback = false;
  int timeline_fallback_limit = 10;
  bool enable_ranking_diagnostics = false;
  int ranking_diagnostics_top_k = 10;
};

// ── Search response ──────────────────────────────────────────

enum class RankingTieBreakReason {
  kTopResult,
  kRerankComposite,
  kFusedScore,
  kBestLaneRank,
  kFrameID,
};

struct RankingLaneContribution {
  SearchSource source = SearchSource::kText;
  float weight = 0.0f;
  int rank = 0;
  float rrf_score = 0.0f;
};

struct RankingDiagnostics {
  std::optional<int> best_lane_rank;
  std::vector<RankingLaneContribution> lane_contributions;
  RankingTieBreakReason tie_break_reason = RankingTieBreakReason::kTopResult;
};

struct SearchResult {
  std::uint64_t frame_id = 0;
  float score = 0.0f;
  std::optional<std::string> preview_text;
  std::vector<SearchSource> sources;
  std::optional<RankingDiagnostics> ranking_diagnostics;
};

struct SearchResponse {
  std::vector<SearchResult> results;
};

// ── RAG types ────────────────────────────────────────────────

enum class RAGItemKind {
  kSnippet,
  kExpanded,
  kSurrogate,
};

struct RAGItem {
  RAGItemKind kind = RAGItemKind::kSnippet;
  std::uint64_t frame_id = 0;
  float score = 0.0f;
  std::vector<SearchSource> sources;
  std::string text;
};

struct RAGContext {
  std::string query;
  std::vector<RAGItem> items;
  int total_tokens = 0;

  /// Optional extracted answer span (populated by DeterministicAnswerExtractor).
  std::string extracted_answer;
};

struct ChunkingStrategy {
  int target_tokens = 400;
  int overlap_tokens = 40;
};

/// Assembly mode for FastRAG context builder.
enum class FastRAGMode {
  kFast,          // Expansion + snippets.
  kDenseCached,   // Expansion + surrogates + snippets.
};

struct FastRAGConfig {
  FastRAGMode mode = FastRAGMode::kFast;

  int max_context_tokens = 1500;
  int expansion_max_tokens = 600;
  int expansion_max_bytes = 2 * 1024 * 1024;
  int snippet_max_tokens = 200;
  int max_snippets = 24;
  int max_surrogates = 8;
  int surrogate_max_tokens = 60;
  int search_top_k = 24;
  SearchMode search_mode{SearchModeKind::kHybrid, 0.5f};
  int rrf_k = 60;
  int preview_max_bytes = 512;

  /// Enable deterministic query-aware reranking for context item ordering.
  bool enable_answer_focused_ranking = true;
  int answer_rerank_window = 12;
  float answer_distractor_penalty = 0.30f;

  /// Enable deterministic answer extraction as post-processing on RAGContext.
  bool enable_answer_extraction = true;

  /// Policy for selecting which surrogate tier to use at retrieval time.
  TierSelectionPolicy tier_selection_policy = TierPolicyImportanceBalanced();

  /// Enable query-aware tier selection (boosts tier for specific queries).
  bool enable_query_aware_tier_selection = true;

  /// Optional fixed "now" timestamp for deterministic tier selection.
  /// When nullopt, uses wall clock time.
  std::optional<std::int64_t> deterministic_now_ms;
};

// ── Query embedding policy ───────────────────────────────────

/// Controls whether the orchestrator computes a query embedding during Recall.
enum class QueryEmbeddingPolicy {
  kNever,        // Never compute a query embedding; use text search only.
  kIfAvailable,  // Use embedding if an embedder is available (default).
  kAlways,       // Require an embedder; throw if unavailable.
};

// ── Direct search types ──────────────────────────────────────

/// Direct search mode for raw candidate retrieval without RAG context assembly.
enum class DirectSearchMode {
  kText,     // Text-only search (FTS5).
  kHybrid,   // Text + vector fusion.
};

/// A single raw search hit returned by Search().
struct MemorySearchHit {
  std::uint64_t frame_id = 0;
  float score = 0.0f;
  std::optional<std::string> preview_text;
  std::vector<SearchSource> sources;
};

// ── Runtime stats ────────────────────────────────────────────

/// Forward-declare WaxWALStats (defined in wax_store.hpp) to avoid circular include.
struct WaxWALStats;

/// Lightweight runtime statistics DTO for operators and diagnostics.
struct RuntimeStats {
  std::uint64_t frame_count = 0;
  std::uint64_t pending_frames = 0;
  std::uint64_t generation = 0;
  std::filesystem::path store_path;
  bool vector_search_enabled = false;
  bool structured_memory_enabled = false;
  bool access_stats_scoring_enabled = false;
  std::string embedder_identity;
};

// ── Session runtime stats ────────────────────────────────────

/// Session-scoped runtime statistics.
struct SessionRuntimeStats {
  bool active = false;
  std::string session_id;
  int session_frame_count = 0;
  int session_token_estimate = 0;
  std::uint64_t pending_frames_store_wide = 0;
};

// ── Handoff ──────────────────────────────────────────────────

/// Record of a session handoff stored via rememberHandoff().
struct HandoffRecord {
  std::uint64_t frame_id = 0;
  std::int64_t timestamp_ms = 0;
  std::string content;
  std::optional<std::string> project;
  std::vector<std::string> pending_tasks;
};

// ── Orchestrator config ──────────────────────────────────────

struct OrchestratorConfig {
  bool enable_text_search = true;
  bool enable_vector_search = true;
  bool enable_structured_memory = true;
  bool enable_access_stats_scoring = false;
  QueryEmbeddingPolicy query_embedding_policy = QueryEmbeddingPolicy::kIfAvailable;
  FastRAGConfig rag{};
  ChunkingStrategy chunking{};
  int ingest_concurrency = 1;
  int ingest_batch_size = 32;
  int embedding_cache_capacity = 2048;
  bool use_metal_vector_search = true;
  bool require_on_device_providers = true;

  /// Scheduled live-set file compaction configuration.
  /// By default, disabled. Set `live_set_rewrite_schedule.enabled = true`
  /// and configure cadence/threshold/cooldown to enable automatic compaction.
  LiveSetRewriteSchedule live_set_rewrite_schedule{};
};

}  // namespace waxcpp
