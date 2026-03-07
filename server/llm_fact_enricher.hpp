// cpp/server/llm_fact_enricher.hpp
// LLM-based enricher: sends chunk text to llama-server (Qwen3)
// and parses JSON array of {entity, attribute, value} facts.
#pragma once

#include "chunk_enricher.hpp"
#include "llama_cpp_generation_client.hpp"

#include <cstdint>

namespace waxcpp::server {

struct LlmFactEnricherConfig {
    int max_tokens = 1024;
    float temperature = 0.1f;
    bool skip_on_error = true;  // continue indexing if LLM fails
    std::uint64_t total_chunks = 0;  // for progress logging (0 = unknown)
};

class LlmFactEnricher : public ChunkEnricher {
 public:
    explicit LlmFactEnricher(
        LlamaCppGenerationClient* client,  // non-owning
        LlmFactEnricherConfig config = {});

    [[nodiscard]] std::string Name() const override { return "llm"; }

    [[nodiscard]] FactBatch Enrich(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text) override;

 private:
    [[nodiscard]] static std::string BuildSystemPrompt(const std::string& language);
    [[nodiscard]] static std::string BuildUserPrompt(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text);
    [[nodiscard]] static FactBatch ParseJsonResponse(
        const std::string& response,
        const Ue5ChunkRecord& record);
    [[nodiscard]] static std::string EntityPrefix(const std::string& language);
    [[nodiscard]] static std::string ExtractJsonArray(const std::string& text);

    LlamaCppGenerationClient* client_;
    LlmFactEnricherConfig config_;
    std::uint64_t chunk_counter_{0};
};

}  // namespace waxcpp::server
