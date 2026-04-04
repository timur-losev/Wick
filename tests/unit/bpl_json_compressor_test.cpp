// tests/unit/bpl_json_compressor_test.cpp
#include "../../server/bpl_json_compressor.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void TestAssert(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// ── Test: ShortenClassPath ─────────────────────────────────────

void TestShortenClassPath() {
    std::cerr << "  TestShortenClassPath..." << std::endl;

    TestAssert(waxcpp::server::ShortenClassPath("/Script/BlueprintGraph.K2Node_CallFunction") == "K2Node_CallFunction",
            "failed to shorten K2Node_CallFunction");
    TestAssert(waxcpp::server::ShortenClassPath("/Script/Engine.Blueprint") == "Blueprint",
            "failed to shorten Blueprint");
    TestAssert(waxcpp::server::ShortenClassPath("K2Node_Event") == "K2Node_Event",
            "already short class should pass through");
    TestAssert(waxcpp::server::ShortenClassPath("") == "",
            "empty should return empty");

    std::cerr << "    PASS" << std::endl;
}

// ── Test: ShortenTypeObj ───────────────────────────────────────

void TestShortenTypeObj() {
    std::cerr << "  TestShortenTypeObj..." << std::endl;

    TestAssert(waxcpp::server::ShortenTypeObj("/Script/Engine.SkeletalMeshComponent") == "SkeletalMeshComponent",
            "failed to shorten SkeletalMeshComponent");
    TestAssert(waxcpp::server::ShortenTypeObj("/Script/GameplayAbilities.AbilitySystemBlueprintLibrary") == "AbilitySystemBlueprintLibrary",
            "failed to shorten AbilitySystemBlueprintLibrary");
    TestAssert(waxcpp::server::ShortenTypeObj("/Script/Engine.Actor") == "Actor",
            "failed to shorten Actor");
    TestAssert(waxcpp::server::ShortenTypeObj("Actor") == "Actor",
            "already short type should pass through");

    std::cerr << "    PASS" << std::endl;
}

// ── Test: CompressBplJson with AN_Melee ────────────────────────

void TestCompressAnMelee() {
    std::cerr << "  TestCompressAnMelee..." << std::endl;

    const std::string raw = R"JSON({
        "blueprint": "/Game/Characters/Heroes/Abilities/AN_Melee.AN_Melee",
        "asset_path": "/Game/Characters/Heroes/Abilities/AN_Melee.AN_Melee",
        "asset_name": "AN_Melee",
        "asset_class": "/Script/Engine.Blueprint",
        "asset_kind": "blueprint",
        "graphs": [
            {
                "name": "Received_Notify",
                "graph_guid": "7169677E426C4C919D89ED97DDA39ED4",
                "nodes": [
                    {
                        "node_guid": "BCC34ED8402512AB90ADD6924976221A",
                        "class_path": "/Script/BlueprintGraph.K2Node_FunctionEntry",
                        "title": "Received Notify",
                        "pos_x": 0,
                        "pos_y": 0,
                        "pins": [
                            {"pin_id": "F789", "name": "then", "direction": "out", "type_cat": "exec", "type_obj": "", "default_value": ""},
                            {"pin_id": "B0E3", "name": "MeshComp", "direction": "out", "type_cat": "object", "type_obj": "/Script/Engine.SkeletalMeshComponent", "default_value": ""},
                            {"pin_id": "5ED4", "name": "Animation", "direction": "out", "type_cat": "object", "type_obj": "/Script/Engine.AnimSequenceBase", "default_value": ""}
                        ]
                    },
                    {
                        "node_guid": "6F01",
                        "class_path": "/Script/BlueprintGraph.K2Node_CallFunction",
                        "title": "Get Owner",
                        "pos_x": 208,
                        "pos_y": 192,
                        "function": {
                            "member_name": "GetOwner",
                            "member_parent": "/Script/Engine.ActorComponent",
                            "member_guid": "00000000000000000000000000000000"
                        },
                        "pins": [
                            {"pin_id": "FE47", "name": "self", "direction": "in", "type_cat": "object", "type_obj": "/Script/Engine.ActorComponent", "default_value": ""},
                            {"pin_id": "5D2B", "name": "ReturnValue", "direction": "out", "type_cat": "object", "type_obj": "/Script/Engine.Actor", "default_value": ""}
                        ]
                    },
                    {
                        "node_guid": "8B55",
                        "class_path": "/Script/BlueprintGraph.K2Node_CallFunction",
                        "title": "Send Gameplay Event to Actor",
                        "pos_x": 368,
                        "pos_y": 0,
                        "function": {
                            "member_name": "SendGameplayEventToActor",
                            "member_parent": "/Script/GameplayAbilities.AbilitySystemBlueprintLibrary",
                            "member_guid": "00000000000000000000000000000000"
                        },
                        "pins": [
                            {"pin_id": "B1F9", "name": "execute", "direction": "in", "type_cat": "exec", "type_obj": "", "default_value": ""},
                            {"pin_id": "BAA1", "name": "then", "direction": "out", "type_cat": "exec", "type_obj": "", "default_value": ""},
                            {"pin_id": "3F00", "name": "self", "direction": "in", "type_cat": "object", "type_obj": "/Script/GameplayAbilities.AbilitySystemBlueprintLibrary", "default_value": ""},
                            {"pin_id": "D951", "name": "Actor", "direction": "in", "type_cat": "object", "type_obj": "/Script/Engine.Actor", "default_value": ""},
                            {"pin_id": "356E", "name": "EventTag", "direction": "in", "type_cat": "struct", "type_obj": "/Script/GameplayTags.GameplayTag", "default_value": "(TagName=\"GameplayEvent.MeleeHit\")"},
                            {"pin_id": "883F", "name": "Payload", "direction": "in", "type_cat": "struct", "type_obj": "/Script/GameplayAbilities.GameplayEventData", "default_value": ""}
                        ]
                    },
                    {
                        "node_guid": "BE1E",
                        "class_path": "/Script/BlueprintGraph.K2Node_FunctionResult",
                        "title": "Return Node",
                        "pos_x": 976,
                        "pos_y": 0,
                        "pins": [
                            {"pin_id": "522D", "name": "execute", "direction": "in", "type_cat": "exec", "type_obj": "", "default_value": ""},
                            {"pin_id": "35DA", "name": "ReturnValue", "direction": "in", "type_cat": "bool", "type_obj": "", "default_value": "false"}
                        ]
                    }
                ],
                "links": [
                    {"from_node_guid": "BCC3", "from_pin_name": "then", "to_node_guid": "8B55", "to_pin_name": "execute"},
                    {"from_node_guid": "BCC3", "from_pin_name": "MeshComp", "to_node_guid": "6F01", "to_pin_name": "self"},
                    {"from_node_guid": "6F01", "from_pin_name": "ReturnValue", "to_node_guid": "8B55", "to_pin_name": "Actor"},
                    {"from_node_guid": "8B55", "from_pin_name": "then", "to_node_guid": "BE1E", "to_pin_name": "execute"}
                ]
            }
        ],
        "variables": []
    })JSON";

    const auto compressed = waxcpp::server::CompressBplJson(raw);
    TestAssert(!compressed.empty(), "compressed should not be empty");

    // Must contain semantic data
    TestAssert(compressed.find("AN_Melee") != std::string::npos, "must contain asset_name");
    TestAssert(compressed.find("Received_Notify") != std::string::npos, "must contain graph name");
    TestAssert(compressed.find("GetOwner") != std::string::npos, "must contain calls GetOwner");
    TestAssert(compressed.find("SendGameplayEventToActor") != std::string::npos, "must contain calls SendGameplayEventToActor");
    TestAssert(compressed.find("GameplayEvent.MeleeHit") != std::string::npos, "must contain EventTag default_value");
    TestAssert(compressed.find("SkeletalMeshComponent") != std::string::npos, "must contain typed pin MeshComp");
    TestAssert(compressed.find("ActorComponent") != std::string::npos, "must contain member_parent shortened");

    // Must NOT contain noise
    TestAssert(compressed.find("pin_id") == std::string::npos, "must not contain pin_id");
    TestAssert(compressed.find("node_guid") == std::string::npos, "must not contain node_guid");
    TestAssert(compressed.find("graph_guid") == std::string::npos, "must not contain graph_guid");
    TestAssert(compressed.find("member_guid") == std::string::npos, "must not contain member_guid");
    TestAssert(compressed.find("pos_x") == std::string::npos, "must not contain pos_x");
    TestAssert(compressed.find("from_node_guid") == std::string::npos, "must not contain links");
    TestAssert(compressed.find("direction") == std::string::npos, "must not contain direction");
    TestAssert(compressed.find("type_cat") == std::string::npos, "must not contain type_cat");
    TestAssert(compressed.find("type_sub") == std::string::npos, "must not contain type_sub");

    // K2Node_FunctionResult should be stripped (trivial node)
    TestAssert(compressed.find("Return Node") == std::string::npos ||
            compressed.find("FunctionResult") == std::string::npos,
            "K2Node_FunctionResult should be stripped or minimal");

    // Size reduction check
    TestAssert(compressed.size() < raw.size() / 3, "compressed must be at least 3x smaller than raw");

    std::cerr << "    compressed size: " << compressed.size() << " vs raw: " << raw.size()
              << " (ratio: " << (static_cast<double>(raw.size()) / compressed.size()) << "x)"
              << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Empty/invalid input ──────────────────────────────────

void TestCompressInvalidInput() {
    std::cerr << "  TestCompressInvalidInput..." << std::endl;

    TestAssert(waxcpp::server::CompressBplJson("").empty(), "empty input should return empty");
    TestAssert(waxcpp::server::CompressBplJson("not json").empty(), "invalid JSON should return empty");
    TestAssert(waxcpp::server::CompressBplJson("[]").empty(), "array root should return empty");

    std::cerr << "    PASS" << std::endl;
}

// ── Test: Noise-only blueprint produces minimal output ─────────

void TestCompressNoiseOnly() {
    std::cerr << "  TestCompressNoiseOnly..." << std::endl;

    const std::string noise_bp = R"({
        "asset_name": "BP_Empty",
        "asset_kind": "blueprint",
        "graphs": [{
            "name": "EventGraph",
            "nodes": [
                {
                    "node_guid": "AAAA",
                    "class_path": "/Script/BlueprintGraph.K2Node_FunctionResult",
                    "title": "Return Node",
                    "pins": [
                        {"pin_id": "1111", "name": "execute", "direction": "in", "type_cat": "exec", "type_obj": "", "default_value": ""},
                        {"pin_id": "2222", "name": "ReturnValue", "direction": "in", "type_cat": "bool", "type_obj": "", "default_value": "false"}
                    ]
                }
            ],
            "links": []
        }],
        "variables": []
    })";

    const auto compressed = waxcpp::server::CompressBplJson(noise_bp);
    // Graph with only a FunctionResult node should produce no graph output
    // because FunctionResult is stripped as trivial
    TestAssert(compressed.find("nodes") == std::string::npos || compressed.find("\"nodes\":[]") != std::string::npos ||
            compressed.find("graphs") == std::string::npos,
            "noise-only blueprint should have no meaningful nodes");

    std::cerr << "    PASS" << std::endl;
}

// ── Test: ChunkBplJsonByNodes ──────────────────────────────────

void TestChunkByNodesSmall() {
    std::cerr << "  TestChunkByNodesSmall..." << std::endl;

    const std::string small = R"({"asset_name":"BP_Test","asset_kind":"blueprint","graphs":[{"name":"EventGraph","nodes":[{"title":"Node1","calls":"Func1"},{"title":"Node2","calls":"Func2"}]}]})";

    auto chunks = waxcpp::server::ChunkBplJsonByNodes(small, 3000);
    TestAssert(chunks.size() == 1, "small blueprint should produce 1 chunk, got " + std::to_string(chunks.size()));
    TestAssert(chunks[0].text.find("BP_Test") != std::string::npos, "chunk must contain asset_name");
    TestAssert(chunks[0].text.find("Func1") != std::string::npos, "chunk must contain Func1");
    TestAssert(chunks[0].text.find("Func2") != std::string::npos, "chunk must contain Func2");

    std::cerr << "    PASS" << std::endl;
}

void TestChunkByNodesLarge() {
    std::cerr << "  TestChunkByNodesLarge..." << std::endl;

    // Build a blueprint with many nodes that exceed target_tokens
    std::string json = R"({"asset_name":"BP_Big","asset_kind":"blueprint","graphs":[{"name":"EventGraph","nodes":[)";
    for (int i = 0; i < 100; ++i) {
        if (i > 0) json += ",";
        json += R"({"title":"Call Func_)" + std::to_string(i) + R"(","calls":"Func_)" + std::to_string(i) +
                R"(","from":"ActorComponent","class":"K2Node_CallFunction"})";
    }
    json += "]}]}";

    // With small target_tokens, should split into multiple chunks
    auto chunks = waxcpp::server::ChunkBplJsonByNodes(json, 200);
    TestAssert(chunks.size() > 1, "large blueprint with small target should split, got " + std::to_string(chunks.size()));

    // Every chunk must contain ancestor breadcrumb
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        TestAssert(chunks[i].text.find("BP_Big") != std::string::npos,
                "chunk " + std::to_string(i) + " must contain asset_name breadcrumb");
        TestAssert(chunks[i].text.find("EventGraph") != std::string::npos,
                "chunk " + std::to_string(i) + " must contain graph name breadcrumb");
    }

    // All function calls must be present across all chunks
    for (int i = 0; i < 100; ++i) {
        bool found = false;
        const auto needle = "Func_" + std::to_string(i);
        for (const auto& chunk : chunks) {
            if (chunk.text.find(needle) != std::string::npos) {
                found = true;
                break;
            }
        }
        TestAssert(found, "function " + needle + " must appear in at least one chunk");
    }

    std::cerr << "    chunks: " << chunks.size() << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: DataAsset with properties ────────────────────────────

void TestCompressDataAsset() {
    std::cerr << "  TestCompressDataAsset..." << std::endl;

    const std::string data_asset = R"({
        "asset_name": "DA_WeaponData_Sword",
        "asset_kind": "data_asset",
        "asset_class": "/Script/MyGame.WeaponDataAsset",
        "properties": {
            "Damage": 25,
            "AttackSpeed": 1.5,
            "WeaponMesh": "/Game/Meshes/Sword.Sword"
        }
    })";

    const auto compressed = waxcpp::server::CompressBplJson(data_asset);
    TestAssert(!compressed.empty(), "data asset compressed should not be empty");
    TestAssert(compressed.find("DA_WeaponData_Sword") != std::string::npos, "must contain asset_name");
    TestAssert(compressed.find("properties") != std::string::npos, "must preserve properties");
    TestAssert(compressed.find("Damage") != std::string::npos, "must preserve Damage property");
    TestAssert(compressed.find("WeaponMesh") != std::string::npos, "must preserve WeaponMesh property");

    std::cerr << "    PASS" << std::endl;
}

}  // namespace

int main() {
    int passed = 0;
    int failed = 0;

    auto run = [&](const char* name, void (*fn)()) {
        try {
            std::cerr << name << std::endl;
            fn();
            ++passed;
        } catch (const std::exception& e) {
            std::cerr << "    FAIL: " << e.what() << std::endl;
            ++failed;
        }
    };

    run("ShortenClassPath", TestShortenClassPath);
    run("ShortenTypeObj", TestShortenTypeObj);
    run("CompressAnMelee", TestCompressAnMelee);
    run("CompressInvalidInput", TestCompressInvalidInput);
    run("CompressNoiseOnly", TestCompressNoiseOnly);
    run("ChunkByNodesSmall", TestChunkByNodesSmall);
    run("ChunkByNodesLarge", TestChunkByNodesLarge);
    run("CompressDataAsset", TestCompressDataAsset);

    std::cerr << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
