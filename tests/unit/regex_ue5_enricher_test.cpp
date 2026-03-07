// tests/unit/regex_ue5_enricher_test.cpp
#include "../../server/regex_ue5_enricher.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

waxcpp::server::Ue5ChunkRecord MakeRecord(
    const std::string& path = "Source/MyModule/MyActor.h",
    const std::string& symbol = "AMyActor") {
    return {
        .chunk_id = "test_chunk_001",
        .relative_path = path,
        .language = "cpp",
        .symbol = symbol,
        .line_start = 1,
        .line_end = 30,
        .token_estimate = 200,
        .content_hash = "abc123",
        .size_bytes = 800,
    };
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

std::string GetFactValue(const waxcpp::server::FactBatch& facts,
                         const std::string& entity,
                         const std::string& attribute) {
    for (const auto& f : facts) {
        if (f.entity == entity && f.attribute == attribute) return f.value;
    }
    return {};
}

// ── Scenario 1: UCLASS with inheritance ──────────────────────

void ScenarioUclassExtraction() {
    std::cerr << "  ScenarioUclassExtraction..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
UCLASS(Blueprintable, BlueprintType)
class MYMODULE_API AMyActor : public AActor
{
    GENERATED_BODY()
};
)");

    Require(HasFact(facts, "cpp:AMyActor", "kind", "uclass"), "missing kind=uclass");
    Require(HasFact(facts, "cpp:AMyActor", "inherits", "AActor"), "missing inherits=AActor");
    Require(HasFact(facts, "cpp:AMyActor", "api_macro", "MYMODULE_API"), "missing api_macro");
    Require(HasFact(facts, "cpp:AMyActor", "specifiers"), "missing specifiers");

    const auto specs = GetFactValue(facts, "cpp:AMyActor", "specifiers");
    Require(specs.find("Blueprintable") != std::string::npos, "specifiers missing Blueprintable");
    Require(specs.find("BlueprintType") != std::string::npos, "specifiers missing BlueprintType");

    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 2: USTRUCT ──────────────────────────────────────

void ScenarioUstructExtraction() {
    std::cerr << "  ScenarioUstructExtraction..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord("Source/Types.h", "FMyStruct");

    const auto facts = enricher.Enrich(record, R"(
USTRUCT(BlueprintType)
struct FMyStruct
{
    GENERATED_BODY()
};
)");

    Require(HasFact(facts, "cpp:FMyStruct", "kind", "ustruct"), "missing kind=ustruct");
    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 3: UENUM ───────────────────────────────────────

void ScenarioUenumExtraction() {
    std::cerr << "  ScenarioUenumExtraction..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord("Source/Types.h", "EGameState");

    const auto facts = enricher.Enrich(record, R"(
UENUM(BlueprintType)
enum class EGameState : uint8
{
    Idle,
    Running,
    Paused
};
)");

    Require(HasFact(facts, "cpp:EGameState", "kind", "uenum"), "missing kind=uenum");
    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 4: UPROPERTY ────────────────────────────────────

void ScenarioUpropertyExtraction() {
    std::cerr << "  ScenarioUpropertyExtraction..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
UCLASS()
class AMyActor : public AActor
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Combat")
    float Health;

    UPROPERTY(VisibleAnywhere)
    USceneComponent* RootComp;
};
)");

    // Health property
    Require(HasFact(facts, "cpp:AMyActor.Health", "kind", "uproperty"), "missing Health kind");
    Require(HasFact(facts, "cpp:AMyActor.Health", "type", "float"), "missing Health type");
    Require(HasFact(facts, "cpp:AMyActor.Health", "category", "Combat"), "missing Health category");

    const auto health_specs = GetFactValue(facts, "cpp:AMyActor.Health", "specifiers");
    Require(health_specs.find("EditAnywhere") != std::string::npos, "Health missing EditAnywhere");
    Require(health_specs.find("BlueprintReadWrite") != std::string::npos, "Health missing BlueprintReadWrite");

    // RootComp property
    Require(HasFact(facts, "cpp:AMyActor.RootComp", "kind", "uproperty"), "missing RootComp kind");
    Require(HasFact(facts, "cpp:AMyActor.RootComp", "type"), "missing RootComp type");

    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 5: UFUNCTION ────────────────────────────────────

void ScenarioUfunctionExtraction() {
    std::cerr << "  ScenarioUfunctionExtraction..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
UCLASS()
class AMyActor : public AActor
{
    UFUNCTION(BlueprintCallable, Category = "Combat")
    void TakeDamage(float Amount);

    UFUNCTION(BlueprintPure)
    float GetHealth() const;
};
)");

    Require(HasFact(facts, "cpp:AMyActor::TakeDamage", "kind", "ufunction"), "missing TakeDamage kind");
    Require(HasFact(facts, "cpp:AMyActor::TakeDamage", "returns", "void"), "missing TakeDamage returns");
    Require(HasFact(facts, "cpp:AMyActor::TakeDamage", "category", "Combat"), "missing TakeDamage category");

    Require(HasFact(facts, "cpp:AMyActor::GetHealth", "kind", "ufunction"), "missing GetHealth kind");

    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 6: #include directives ──────────────────────────

void ScenarioIncludeDirectives() {
    std::cerr << "  ScenarioIncludeDirectives..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
#include "GameFramework/Actor.h"
#include <CoreMinimal.h>
#include "MyModule/MyComponent.h"
)");

    Require(HasFact(facts, "file:Source/MyModule/MyActor.h", "includes", "GameFramework/Actor.h"),
            "missing Actor.h include");
    Require(HasFact(facts, "file:Source/MyModule/MyActor.h", "includes", "CoreMinimal.h"),
            "missing CoreMinimal.h include");
    Require(HasFact(facts, "file:Source/MyModule/MyActor.h", "includes", "MyModule/MyComponent.h"),
            "missing MyComponent.h include");

    Require(facts.size() == 3, "expected exactly 3 include facts, got " + std::to_string(facts.size()));
    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 7: Multiple inheritance ─────────────────────────

void ScenarioMultipleInheritance() {
    std::cerr << "  ScenarioMultipleInheritance..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
UCLASS()
class AMyActor : public AActor, public IInteractable, public ISerializable
{
};
)");

    Require(HasFact(facts, "cpp:AMyActor", "inherits", "AActor"), "missing AActor");
    Require(HasFact(facts, "cpp:AMyActor", "inherits", "IInteractable"), "missing IInteractable");
    Require(HasFact(facts, "cpp:AMyActor", "inherits", "ISerializable"), "missing ISerializable");

    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 8: No UE5 macros — plain C++ ────────────────────

void ScenarioPlainCpp() {
    std::cerr << "  ScenarioPlainCpp..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord("Source/Math.cpp", "AddNumbers");

    const auto facts = enricher.Enrich(record, R"(
int FMathHelper::AddNumbers(int a, int b) {
    return a + b;
}
)");

    // No UE5 macros → no facts (no includes either)
    Require(facts.empty(), "expected no facts for plain C++, got " + std::to_string(facts.size()));
    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 9: Multi-line UCLASS specifiers ─────────────────

void ScenarioMultiLineUclass() {
    std::cerr << "  ScenarioMultiLineUclass..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
UCLASS(
    Blueprintable,
    BlueprintType,
    meta = (BlueprintSpawnableComponent)
)
class AMyActor : public AActor
{
};
)");

    Require(HasFact(facts, "cpp:AMyActor", "kind", "uclass"), "missing kind=uclass");
    Require(HasFact(facts, "cpp:AMyActor", "inherits", "AActor"), "missing inherits");

    const auto specs = GetFactValue(facts, "cpp:AMyActor", "specifiers");
    Require(specs.find("Blueprintable") != std::string::npos, "missing Blueprintable in multi-line");
    Require(specs.find("BlueprintType") != std::string::npos, "missing BlueprintType in multi-line");

    std::cerr << "    PASS (" << facts.size() << " facts)" << std::endl;
}

// ── Scenario 10: Empty chunk ─────────────────────────────────

void ScenarioEmptyChunk() {
    std::cerr << "  ScenarioEmptyChunk..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, "");
    Require(facts.empty(), "expected empty facts for empty chunk");

    const auto facts2 = enricher.Enrich(record, "   \n\n   \n");
    Require(facts2.empty(), "expected empty facts for whitespace chunk");

    std::cerr << "    PASS" << std::endl;
}

// ── Scenario 11: Fact provenance metadata ────────────────────

void ScenarioFactMetadata() {
    std::cerr << "  ScenarioFactMetadata..." << std::endl;
    waxcpp::server::RegexUe5Enricher enricher;
    const auto record = MakeRecord();

    const auto facts = enricher.Enrich(record, R"(
UCLASS()
class AMyActor : public AActor {};
)");

    Require(!facts.empty(), "expected at least one fact");
    const auto& meta = facts[0].metadata;
    Require(meta.count("enricher_kind") && meta.at("enricher_kind") == "regex_ue5",
            "missing enricher_kind=regex_ue5");
    Require(meta.count("source_path") && meta.at("source_path") == record.relative_path,
            "missing source_path");
    Require(meta.count("source_chunk_id") && meta.at("source_chunk_id") == record.chunk_id,
            "missing source_chunk_id");

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

    std::cerr << "regex_ue5_enricher_test:" << std::endl;

    run("ScenarioUclassExtraction", ScenarioUclassExtraction);
    run("ScenarioUstructExtraction", ScenarioUstructExtraction);
    run("ScenarioUenumExtraction", ScenarioUenumExtraction);
    run("ScenarioUpropertyExtraction", ScenarioUpropertyExtraction);
    run("ScenarioUfunctionExtraction", ScenarioUfunctionExtraction);
    run("ScenarioIncludeDirectives", ScenarioIncludeDirectives);
    run("ScenarioMultipleInheritance", ScenarioMultipleInheritance);
    run("ScenarioPlainCpp", ScenarioPlainCpp);
    run("ScenarioMultiLineUclass", ScenarioMultiLineUclass);
    run("ScenarioEmptyChunk", ScenarioEmptyChunk);
    run("ScenarioFactMetadata", ScenarioFactMetadata);

    std::cerr << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
