// cpp/server/chunk_enricher.hpp
// Chunk enrichment pipeline — extracts structured facts from code chunks
// during indexing. Facts are stored via RememberFact() (EAV triples).
#pragma once

#include "ue5_chunk_manifest.hpp"
#include "waxcpp/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace waxcpp::server {

/// A single extracted fact (entity-attribute-value triple with provenance).
struct ExtractedFact {
    std::string entity;      // e.g. "cpp:AMyActor"
    std::string attribute;   // e.g. "inherits"
    std::string value;       // e.g. "AActor"
    waxcpp::Metadata metadata;  // enricher_kind, source_path, chunk_id, etc.
};

/// Batch of facts produced by an enricher for a single chunk.
using FactBatch = std::vector<ExtractedFact>;

/// Abstract interface for chunk enrichers.
class ChunkEnricher {
 public:
    virtual ~ChunkEnricher() = default;

    /// Short identifier for logging (e.g. "regex_ue5", "llm").
    [[nodiscard]] virtual std::string Name() const = 0;

    /// Extract facts from a single chunk.
    [[nodiscard]] virtual FactBatch Enrich(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text) = 0;
};

/// Composes zero or more enrichers and runs them in sequence.
class EnricherPipeline {
 public:
    void AddEnricher(std::unique_ptr<ChunkEnricher> enricher) {
        enrichers_.push_back(std::move(enricher));
    }

    /// Run all enrichers, concatenating their outputs.
    [[nodiscard]] FactBatch EnrichAll(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text) {
        FactBatch combined{};
        for (auto& e : enrichers_) {
            auto batch = e->Enrich(record, chunk_text);
            for (auto& fact : batch) {
                combined.push_back(std::move(fact));
            }
        }
        return combined;
    }

    [[nodiscard]] bool Empty() const { return enrichers_.empty(); }

 private:
    std::vector<std::unique_ptr<ChunkEnricher>> enrichers_;
};

}  // namespace waxcpp::server
