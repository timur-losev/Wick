// tests/unit/llm_fact_enricher_test.cpp
#include "../../server/llm_fact_enricher.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

waxcpp::server::Ue5ChunkRecord MakeRecord() {
    return {
        .chunk_id = "test_llm_001",
        .relative_path = "Source/MyModule/MyActor.cpp",
        .language = "cpp",
        .symbol = "AMyActor",
        .line_start = 10,
        .line_end = 40,
        .token_estimate = 300,
        .content_hash = "deadbeef",
        .size_bytes = 1200,
    };
}

waxcpp::server::Ue5ChunkRecord MakeBlueprintRecord() {
    auto record = MakeRecord();
    record.relative_path = "Content/Test/BP_Test.bpl_json";
    record.language = "bpl_json";
    record.symbol.clear();
    return record;
}

bool HasFact(const waxcpp::server::FactBatch& facts,
             const std::string& entity,
             const std::string& attribute,
             const std::string& value = "") {
    for (const auto& f : facts) {
        if (f.entity == entity && f.attribute == attribute) {
            if (value.empty() || f.value == value) return true;
        }
    }
    return false;
}

// ── Scenario 1: Parse valid JSON response ────────────────────

void ScenarioParseValidJson() {
    std::cerr << "  ScenarioParseValidJson..." << std::endl;

    // Mock LLM that returns structured JSON
    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [](const std::string&) -> std::string {
        return R"({"choices":[{"message":{"content":"[{\"entity\":\"AMyActor\",\"attribute\":\"inherits\",\"value\":\"AActor\"},{\"entity\":\"AMyActor::BeginPlay\",\"attribute\":\"returns\",\"value\":\"void\"}]"}}]})";
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    waxcpp::server::LlmFactEnricher enricher(&client);
    const auto facts = enricher.Enrich(MakeRecord(), "class AMyActor : public AActor {}");

    Require(HasFact(facts, "cpp:AMyActor", "inherits", "AActor"), "missing AMyActor inherits AActor");
    Require(HasFact(facts, "cpp:AMyActor::BeginPlay", "returns", "void"), "missing BeginPlay returns void");
    Require(facts.size() == 2, "expected 2 facts, got " + std::to_string(facts.size()));

    // Check metadata
    Require(facts[0].metadata.at("enricher_kind") == "llm", "wrong enricher_kind");

    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 2: JSON with markdown code fences ───────────────

void ScenarioJsonWithCodeFences() {
    std::cerr << "  ScenarioJsonWithCodeFences..." << std::endl;

    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [](const std::string&) -> std::string {
        return R"({"choices":[{"message":{"content":"```json\n[{\"entity\":\"FMyStruct\",\"attribute\":\"kind\",\"value\":\"struct\"}]\n```"}}]})";
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    waxcpp::server::LlmFactEnricher enricher(&client);
    const auto facts = enricher.Enrich(MakeRecord(), "struct FMyStruct {};");

    Require(HasFact(facts, "cpp:FMyStruct", "kind", "struct"), "failed to parse fenced JSON");
    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 3: Empty array ──────────────────────────────────

void ScenarioEmptyArray() {
    std::cerr << "  ScenarioEmptyArray..." << std::endl;

    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [](const std::string&) -> std::string {
        return R"({"choices":[{"message":{"content":"[]"}}]})";
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    waxcpp::server::LlmFactEnricher enricher(&client);
    const auto facts = enricher.Enrich(MakeRecord(), "int x = 42;");

    Require(facts.empty(), "expected empty facts for []");
    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 4: Malformed JSON, skip_on_error=true ───────────

void ScenarioMalformedJson() {
    std::cerr << "  ScenarioMalformedJson..." << std::endl;

    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [](const std::string&) -> std::string {
        return R"({"choices":[{"message":{"content":"This is not JSON at all."}}]})";
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    waxcpp::server::LlmFactEnricher enricher(&client, {.skip_on_error = true});
    const auto facts = enricher.Enrich(MakeRecord(), "some code");

    // Should return empty, not throw
    Require(facts.empty(), "expected empty on malformed JSON");
    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 5: Network error, skip_on_error=true ────────────

void ScenarioNetworkError() {
    std::cerr << "  ScenarioNetworkError..." << std::endl;

    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [](const std::string&) -> std::string {
        throw std::runtime_error("connection refused");
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    waxcpp::server::LlmFactEnricher enricher(&client, {.skip_on_error = true});
    const auto facts = enricher.Enrich(MakeRecord(), "some code");

    Require(facts.empty(), "expected empty on network error");
    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 6: Noise-only blueprint chunk is skipped before LLM ────────────

void ScenarioBlueprintNoiseChunkSkipped() {
    std::cerr << "  ScenarioBlueprintNoiseChunkSkipped..." << std::endl;

    int call_count = 0;
    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [&call_count](const std::string&) -> std::string {
        ++call_count;
        return R"({"choices":[{"message":{"content":"[{\"entity\":\"BP_Test\",\"attribute\":\"calls\",\"value\":\"ShouldNotHappen\"}]"}}]})";
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    const std::string noise_chunk = R"({
  "pins": [
    {
      "pin_id": "A1",
      "name": "execute",
      "direction": "in",
      "type_cat": "exec",
      "type_sub": "None",
      "default_value": "",
      "default_object": "",
      "default_text": "",
      "container_type": "None"
    }
  ],
  "links": [
    {
      "from_node_guid": "N1",
      "from_pin_name": "then",
      "to_node_guid": "N2",
      "to_pin_name": "execute"
    }
  ]
})";

    waxcpp::server::LlmFactEnricher enricher(&client);
    const auto facts = enricher.Enrich(MakeBlueprintRecord(), noise_chunk);

    Require(facts.empty(), "noise-only blueprint chunk must produce no facts");
    Require(call_count == 0, "noise-only blueprint chunk must not call LLM");
    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 7: Semantic blueprint chunk still reaches LLM ──────────────────

void ScenarioBlueprintSemanticChunkNotSkipped() {
    std::cerr << "  ScenarioBlueprintSemanticChunkNotSkipped..." << std::endl;

    int call_count = 0;
    waxcpp::server::LlamaCppGenerationConfig gen_config;
    gen_config.request_fn = [&call_count](const std::string&) -> std::string {
        ++call_count;
        return R"({"choices":[{"message":{"content":"[{\"entity\":\"BP_Test\",\"attribute\":\"calls\",\"value\":\"K2_SetActorLocation\"}]"}}]})";
    };
    waxcpp::server::LlamaCppGenerationClient client(gen_config);

    const std::string semantic_chunk = R"({
  "class_path": "/Script/BlueprintGraph.K2Node_CallFunction",
  "title": "Set Actor Location",
  "function": {
    "member_name": "K2_SetActorLocation",
    "member_parent": "/Script/Engine.Actor"
  }
})";

    waxcpp::server::LlmFactEnricher enricher(&client);
    const auto facts = enricher.Enrich(MakeBlueprintRecord(), semantic_chunk);

    Require(call_count == 1, "semantic blueprint chunk must call LLM");
    Require(HasFact(facts, "bp:BP_Test", "calls", "K2_SetActorLocation"),
            "semantic blueprint fact missing");
    std::cerr << "    PASS" << std::endl;
}

}  // namespace

int main() {
    int passed = 0;
    int failed = 0;

    auto run = [&](const char* name, void (*fn)()) {
        try {
            fn();
            ++passed;
        } catch (const std::exception& e) {
            std::cerr << "  FAIL: " << name << " — " << e.what() << std::endl;
            ++failed;
        }
    };

    std::cerr << "llm_fact_enricher_test:" << std::endl;

    run("ScenarioParseValidJson", ScenarioParseValidJson);
    run("ScenarioJsonWithCodeFences", ScenarioJsonWithCodeFences);
    run("ScenarioEmptyArray", ScenarioEmptyArray);
    run("ScenarioMalformedJson", ScenarioMalformedJson);
    run("ScenarioNetworkError", ScenarioNetworkError);
    run("ScenarioBlueprintNoiseChunkSkipped", ScenarioBlueprintNoiseChunkSkipped);
    run("ScenarioBlueprintSemanticChunkNotSkipped", ScenarioBlueprintSemanticChunkNotSkipped);

    std::cerr << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
