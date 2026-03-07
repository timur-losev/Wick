// cpp/server/llm_fact_enricher.cpp
#include "llm_fact_enricher.hpp"
#include "server_utils.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <cctype>
#include <chrono>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace waxcpp::server {

namespace {

bool EnrichLlmLogEnabled() {
    static const bool enabled = []() {
        const auto raw = EnvString("WAXCPP_ENRICH_LLM_LOG");
        if (!raw.has_value()) return false;
        const auto& v = *raw;
        return v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON";
    }();
    return enabled;
}

/// For Blueprint JSON: skip chunks that contain only pins/links/GUIDs with no node declarations.
/// A useful chunk has at least one "class_path" or "member_name" or "blueprint" key.
bool IsBlueprintChunkUseful(std::string_view text) {
    return text.find("\"class_path\"") != std::string_view::npos
        || text.find("\"member_name\"") != std::string_view::npos
        || text.find("\"blueprint\"") != std::string_view::npos
        || text.find("\"custom_function_name\"") != std::string_view::npos
        || text.find("\"var_name\"") != std::string_view::npos
        || text.find("\"cast_target\"") != std::string_view::npos
        || text.find("\"macro_ref\"") != std::string_view::npos;
}

/// Check if a string looks like a hex GUID (32 hex chars, possibly with hyphens).
bool LooksLikeGuid(std::string_view s) {
    if (s.size() < 16) return false;
    int hex_count = 0;
    for (char ch : s) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) ++hex_count;
        else if (ch != '-') return false;
    }
    return hex_count >= 16;
}

}  // namespace

LlmFactEnricher::LlmFactEnricher(
    LlamaCppGenerationClient* client,
    LlmFactEnricherConfig config)
    : client_(client), config_(config) {}

// ── Prompts ──────────────────────────────────────────────────

std::string LlmFactEnricher::BuildSystemPrompt(const std::string& language) {
    if (language == "json" || language == "blueprint_json" || language == "bpl_json") {
        return
            "You are a UE Blueprint analysis assistant. Extract structured facts from Blueprint graph JSON.\n"
            "Return a JSON array of objects: {\"entity\", \"attribute\", \"value\"}.\n"
            "\n"
            "RULES:\n"
            "- entity = human-readable name (Blueprint name, function name, variable name, event name)\n"
            "- NEVER use GUIDs (hex strings like \"AA1AC0D5...\") as entity or value\n"
            "- NEVER use pin names (\"then\", \"execute\", \"self\", \"ReturnValue\", \"A\", \"B\") as values for \"calls\"\n"
            "- For \"calls\" attribute, use the \"member_name\" field from CallFunction nodes\n"
            "- For \"has_variable\" attribute, use the \"member_name\" from VariableGet/Set nodes\n"
            "- For \"has_event\" attribute, use \"custom_function_name\" or \"title\" from Event nodes\n"
            "- If chunk contains only pins, links, or GUIDs without node class_path/title — return []\n"
            "\n"
            "GOOD attributes: calls, has_variable, has_event, type, cast_target, macro_ref, depends_on, purpose\n"
            "- For \"depends_on\", use cast_target class, macro blueprint, or parent class references\n"
            "- For \"purpose\", describe what the Blueprint or function DOES based on its name, events, and called functions (e.g. \"handles player movement\", \"enemy AI behavior\")\n"
            "\n"
            "Example input node:\n"
            "{\"class_path\": \"K2Node_CallFunction\", \"title\": \"Set Actor Location\", "
            "\"function\": {\"member_name\": \"K2_SetActorLocation\", \"member_parent\": \"/Script/Engine.Actor\"}}\n"
            "Example output:\n"
            "[{\"entity\": \"BP_MyActor\", \"attribute\": \"calls\", \"value\": \"K2_SetActorLocation\"}]\n"
            "\n"
            "Respond ONLY with the JSON array, no other text.";
    }
    return
        "You are a code analysis assistant. Extract structured facts from the given C++ code chunk.\n"
        "Return a JSON array of objects with exactly these fields:\n"
        "- \"entity\": The class, function, or component name (e.g. \"AMyActor\", \"AMyActor::TakeDamage\")\n"
        "- \"attribute\": The relationship type (e.g. \"inherits\", \"returns\", \"calls\", \"depends_on\", \"purpose\")\n"
        "- \"value\": The value of the relationship\n"
        "\n"
        "Focus on: inheritance, function signatures, dependencies, design patterns, semantic purpose.\n"
        "Only extract facts you are confident about. Return [] for trivial or boilerplate code.\n"
        "Respond ONLY with the JSON array, no other text.";
}

std::string LlmFactEnricher::EntityPrefix(const std::string& language) {
    if (language == "json" || language == "blueprint_json" || language == "bpl_json") return "bp:";
    return "cpp:";
}

std::string LlmFactEnricher::BuildUserPrompt(
    const Ue5ChunkRecord& record,
    std::string_view chunk_text) {
    std::ostringstream out;
    out << "File: " << record.relative_path
        << " (lines " << record.line_start << "-" << record.line_end
        << ", language: " << record.language << ")\n";
    if (!record.symbol.empty()) {
        out << "Symbol context: " << record.symbol << "\n";
    }
    const auto lang_tag = (record.language == "json" || record.language == "blueprint_json" || record.language == "bpl_json") ? "json" : "cpp";
    out << "\n```" << lang_tag << "\n" << chunk_text << "\n```\n\n"
        << "Extract facts as JSON:\n/no_think";
    return out.str();
}

// ── JSON response parsing ────────────────────────────────────

std::string LlmFactEnricher::ExtractJsonArray(const std::string& text) {
    // Strip markdown code fences if present
    auto start = text.find('[');
    if (start == std::string::npos) return "[]";

    // Find matching closing bracket
    int depth = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        if (text[i] == '[') ++depth;
        else if (text[i] == ']') {
            --depth;
            if (depth == 0) {
                return text.substr(start, i - start + 1);
            }
        }
    }
    return "[]";
}

FactBatch LlmFactEnricher::ParseJsonResponse(
    const std::string& response,
    const Ue5ChunkRecord& record) {

    FactBatch out;
    const auto json_str = ExtractJsonArray(response);

    Poco::JSON::Parser parser;
    const auto parsed = parser.parse(json_str);
    const auto arr = parsed.extract<Poco::JSON::Array::Ptr>();
    if (arr.isNull()) return out;

    const waxcpp::Metadata meta = {
        {"enricher_kind", "llm"},
        {"source_path", record.relative_path},
        {"source_lines", std::to_string(record.line_start) + "-" + std::to_string(record.line_end)},
        {"source_chunk_id", record.chunk_id},
    };

    for (std::size_t i = 0; i < arr->size(); ++i) {
        const auto obj = arr->getObject(static_cast<unsigned int>(i));
        if (obj.isNull()) continue;

        const auto entity = obj->optValue<std::string>("entity", "");
        const auto attribute = obj->optValue<std::string>("attribute", "");
        const auto value = obj->optValue<std::string>("value", "");

        if (entity.empty() || attribute.empty()) continue;

        // Post-filter for Blueprint JSON: reject GUID entities and pin-name-as-value garbage.
        const bool is_bp = (record.language == "json" || record.language == "blueprint_json" || record.language == "bpl_json");
        if (is_bp) {
            if (LooksLikeGuid(entity)) continue;
            if (LooksLikeGuid(value)) continue;
            // Reject known pin-name values used as "calls" targets
            if (attribute == "calls" || attribute == "has_event") {
                static const std::unordered_set<std::string> kPinNames{
                    "then", "execute", "self", "ReturnValue", "A", "B", "C", "D",
                    "then_0", "then_1", "then_2", "Condition", "Result",
                    "Output", "Input", "Value", "Target", "Object",
                };
                if (kPinNames.contains(value)) continue;
            }
        }

        // Add namespace prefix based on language (cpp: or bp:)
        std::string prefixed_entity = entity;
        const auto prefix = EntityPrefix(record.language);
        if (!entity.starts_with("cpp:") && !entity.starts_with("bp:") && !entity.starts_with("file:")) {
            prefixed_entity = prefix + entity;
        }

        out.push_back({std::move(prefixed_entity), attribute, value, meta});
    }

    return out;
}

// ── Main Enrich method ───────────────────────────────────────

FactBatch LlmFactEnricher::Enrich(
    const Ue5ChunkRecord& record,
    std::string_view chunk_text) {

    if (!client_) return {};
    if (chunk_text.empty()) return {};

    // Pre-filter: skip Blueprint JSON chunks that contain only pins/links/GUIDs.
    const bool is_json = (record.language == "json" || record.language == "blueprint_json" || record.language == "bpl_json");
    if (is_json && !IsBlueprintChunkUseful(chunk_text)) {
        ++chunk_counter_;
        if (EnrichLlmLogEnabled()) {
            const auto tag = config_.total_chunks > 0
                ? "[" + std::to_string(chunk_counter_) + "/" + std::to_string(config_.total_chunks) + "] "
                : "[" + std::to_string(chunk_counter_) + "] ";
            std::cerr << "[ENRICH-LLM] " << tag << "SKIP (no node declarations) "
                      << record.relative_path << ":" << record.line_start << "-" << record.line_end << "\n";
        }
        return {};
    }

    ++chunk_counter_;
    const bool verbose = EnrichLlmLogEnabled();
    const auto progress_tag = [&]() -> std::string {
        if (config_.total_chunks > 0) {
            return "[" + std::to_string(chunk_counter_) + "/" + std::to_string(config_.total_chunks) + "] ";
        }
        return "[" + std::to_string(chunk_counter_) + "] ";
    }();

    try {
        const auto user_prompt = BuildUserPrompt(record, chunk_text);
        const auto system_prompt = BuildSystemPrompt(record.language);

        if (verbose) {
            std::cerr << "\n[ENRICH-LLM] " << progress_tag << "── REQUEST ──────────────────────\n"
                      << "[ENRICH-LLM] file: " << record.relative_path
                      << " lines " << record.line_start << "-" << record.line_end
                      << " symbol: " << record.symbol << "\n"
                      << "[ENRICH-LLM] chunk (" << chunk_text.size() << " chars):\n"
                      << chunk_text.substr(0, 500);
            if (chunk_text.size() > 500) {
                std::cerr << "\n... (" << (chunk_text.size() - 500) << " more chars)";
            }
            std::cerr << "\n[ENRICH-LLM] ─────────────────────────────────\n"
                      << std::flush;
        }

        const auto t0 = std::chrono::steady_clock::now();

        const auto response = client_->Generate(LlamaCppGenerationRequest{
            .prompt = user_prompt,
            .system_prompt = system_prompt,
            .max_tokens = config_.max_tokens,
            .temperature = config_.temperature,
        });

        const auto t1 = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        auto facts = ParseJsonResponse(response, record);

        if (verbose) {
            std::cerr << "[ENRICH-LLM] ── RESPONSE (" << elapsed_ms << "ms) ────────────\n"
                      << "[ENRICH-LLM] raw (" << response.size() << " chars): "
                      << response.substr(0, 800);
            if (response.size() > 800) {
                std::cerr << "\n... (" << (response.size() - 800) << " more chars)";
            }
            std::cerr << "\n[ENRICH-LLM] facts extracted: " << facts.size() << "\n";
            for (std::size_t i = 0; i < facts.size(); ++i) {
                std::cerr << "[ENRICH-LLM]   [" << i << "] "
                          << facts[i].entity << " | "
                          << facts[i].attribute << " | "
                          << facts[i].value << "\n";
            }
            std::cerr << "[ENRICH-LLM] ─────────────────────────────────\n"
                      << std::flush;
        } else {
            // Compact one-liner even without verbose
            std::cerr << "[ENRICH-LLM] " << progress_tag << record.relative_path
                      << ":" << record.line_start << "-" << record.line_end
                      << " -> " << facts.size() << " facts (" << elapsed_ms << "ms)\n";
        }

        return facts;

    } catch (const std::exception& e) {
        std::cerr << "[ENRICH-LLM] error: " << e.what()
                  << " file=" << record.relative_path << "\n";
        if (config_.skip_on_error) return {};
        throw;
    }
}

}  // namespace waxcpp::server
