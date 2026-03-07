// cpp/server/regex_ue5_enricher.hpp
// Heuristic regex-based enricher for UE5 C++ code.
// Parses UCLASS/USTRUCT/UENUM macros, UPROPERTY, UFUNCTION,
// inheritance, and #include directives.
#pragma once

#include "chunk_enricher.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace waxcpp::server {

class RegexUe5Enricher : public ChunkEnricher {
 public:
    [[nodiscard]] std::string Name() const override { return "regex_ue5"; }

    [[nodiscard]] FactBatch Enrich(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text) override;

 private:
    // ── Helpers ──

    /// Extract content inside balanced parentheses, starting from lines[start_idx]
    /// at position paren_offset. May span multiple lines. Returns empty if unbalanced.
    static std::string ExtractBalancedParens(
        const std::vector<std::string>& lines,
        std::size_t start_idx,
        std::size_t paren_offset,
        std::size_t* end_line_out = nullptr);

    /// Split comma-separated specifiers, trim each. Respects nested parens.
    static std::vector<std::string> SplitSpecifiers(std::string_view content);

    /// Extract the Category value from specifiers (e.g. Category = "Combat").
    static std::string ExtractCategory(const std::vector<std::string>& specifiers);

    /// Join specifiers into comma-separated string.
    static std::string JoinSpecifiers(const std::vector<std::string>& specifiers);

    /// Build provenance metadata for a fact.
    waxcpp::Metadata MakeFactMeta(const Ue5ChunkRecord& record) const;

    // ── Per-construct parsers (all write into `out`) ──

    void ParseClassDecl(
        const std::vector<std::string>& lines,
        std::size_t class_line_idx,
        const std::string& macro_kind,          // "uclass", "ustruct", "uenum"
        const std::vector<std::string>& macro_specifiers,
        const Ue5ChunkRecord& record,
        FactBatch& out,
        std::string& current_class_out);

    void ParsePropertyDecl(
        const std::vector<std::string>& lines,
        std::size_t decl_line_idx,
        const std::vector<std::string>& specifiers,
        const std::string& owning_class,
        const Ue5ChunkRecord& record,
        FactBatch& out);

    void ParseFunctionDecl(
        const std::vector<std::string>& lines,
        std::size_t decl_line_idx,
        const std::vector<std::string>& specifiers,
        const std::string& owning_class,
        const Ue5ChunkRecord& record,
        FactBatch& out);

    void ParseInclude(
        std::string_view line,
        const Ue5ChunkRecord& record,
        FactBatch& out);
};

}  // namespace waxcpp::server
