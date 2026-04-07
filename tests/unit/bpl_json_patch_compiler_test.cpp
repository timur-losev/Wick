// tests/unit/bpl_json_patch_compiler_test.cpp
#include "../../server/bpl_json_patch_compiler.hpp"

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Assert(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ── Test: Create mode ──────────────────────────────────────────

void TestCreateMode() {
    std::cerr << "  TestCreateMode..." << std::endl;

    auto result = waxcpp::server::CompileBlueprintPatch("",
        R"({
            "create": true,
            "target": "/Game/Test/BP_Test.BP_Test",
            "parent_class": "/Script/Engine.Actor",
            "graphs": [{
                "name": "EventGraph",
                "add_nodes": [
                    {"ref": "tick", "type": "Event", "event": "ReceiveTick"},
                    {"ref": "call", "type": "CallFunction", "function": "MyFunc", "function_owner": "MyClass"}
                ],
                "add_links": [
                    {"from": "tick", "from_pin": "then", "to": "call", "to_pin": "execute"}
                ]
            }]
        })");

    Assert(result.error.empty(), "create mode should not error: " + result.error);
    Assert(Contains(result.json, "BP_Test"), "must contain asset_name");
    Assert(Contains(result.json, "ReceiveTick"), "must contain event");
    Assert(Contains(result.json, "MyFunc"), "must contain function");
    Assert(Contains(result.json, "K2Node_Event"), "must contain class_path for Event");
    Assert(Contains(result.json, "K2Node_CallFunction"), "must contain class_path for CallFunction");
    Assert(Contains(result.summary, "2 nodes added"), "summary must mention 2 nodes: " + result.summary);
    Assert(Contains(result.summary, "1 links added"), "summary must mention 1 link: " + result.summary);

    std::cerr << "    summary: " << result.summary << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Patch mode — add nodes and links ─────────────────────

void TestPatchAddNodes() {
    std::cerr << "  TestPatchAddNodes..." << std::endl;

    const std::string existing = R"({
        "blueprint": "/Game/Test.Test",
        "asset_name": "Test",
        "asset_kind": "blueprint",
        "graphs": [{
            "name": "EventGraph",
            "graph_guid": "AAAA",
            "nodes": [
                {"node_guid": "N1", "title": "Get Owner", "class_path": "/Script/BlueprintGraph.K2Node_CallFunction", "pos_x": 0, "pos_y": 0, "pins": []},
                {"node_guid": "N2", "title": "Do Thing", "class_path": "/Script/BlueprintGraph.K2Node_CallFunction", "pos_x": 200, "pos_y": 0, "pins": []}
            ],
            "links": [
                {"from_node_guid": "N1", "from_pin_name": "then", "to_node_guid": "N2", "to_pin_name": "execute"}
            ]
        }]
    })";

    const std::string intent = R"({
        "graphs": [{
            "name": "EventGraph",
            "remove_links": [
                {"from": "existing:Get Owner", "from_pin": "then", "to": "existing:Do Thing", "to_pin": "execute"}
            ],
            "add_nodes": [
                {"ref": "check", "type": "Branch"}
            ],
            "add_links": [
                {"from": "existing:Get Owner", "from_pin": "then", "to": "check", "to_pin": "execute"},
                {"from": "check", "from_pin": "True", "to": "existing:Do Thing", "to_pin": "execute"}
            ]
        }]
    })";

    auto result = waxcpp::server::CompileBlueprintPatch(existing, intent);
    Assert(result.error.empty(), "patch mode should not error: " + result.error);
    Assert(Contains(result.json, "Branch"), "must contain Branch node");
    Assert(Contains(result.summary, "1 nodes added"), "summary: " + result.summary);
    Assert(Contains(result.summary, "2 links added"), "summary: " + result.summary);
    Assert(Contains(result.summary, "1 links removed"), "summary: " + result.summary);

    // Verify node count: 2 existing + 1 new = 3
    Poco::JSON::Parser parser;
    auto parsed = parser.parse(result.json);
    auto root = parsed.extract<Poco::JSON::Object::Ptr>();
    auto graphs = root->getArray("graphs");
    auto nodes = graphs->getObject(0)->getArray("nodes");
    auto links = graphs->getObject(0)->getArray("links");
    Assert(nodes->size() == 3, "expected 3 nodes, got " + std::to_string(nodes->size()));
    Assert(links->size() == 2, "expected 2 links, got " + std::to_string(links->size()));

    std::cerr << "    summary: " << result.summary << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Error — existing node not found ──────────────────────

void TestExistingNodeNotFound() {
    std::cerr << "  TestExistingNodeNotFound..." << std::endl;

    const std::string existing = R"({
        "graphs": [{
            "name": "G",
            "nodes": [{"node_guid": "X", "title": "Foo", "pins": []}],
            "links": []
        }]
    })";

    const std::string intent = R"({
        "graphs": [{
            "name": "G",
            "add_links": [
                {"from": "existing:Bar", "from_pin": "then", "to": "existing:Foo", "to_pin": "execute"}
            ]
        }]
    })";

    auto result = waxcpp::server::CompileBlueprintPatch(existing, intent);
    Assert(!result.error.empty(), "should error on missing node");
    Assert(Contains(result.error, "existing:Bar"), "error must mention missing ref: " + result.error);
    Assert(Contains(result.error, "Foo"), "error must list available nodes: " + result.error);

    std::cerr << "    error: " << result.error << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Error — duplicate ref ────────────────────────────────

void TestDuplicateRef() {
    std::cerr << "  TestDuplicateRef..." << std::endl;

    auto result = waxcpp::server::CompileBlueprintPatch("",
        R"({
            "create": true, "target": "/Game/T.T",
            "graphs": [{"name": "G", "add_nodes": [
                {"ref": "a", "type": "Branch"},
                {"ref": "a", "type": "Branch"}
            ]}]
        })");

    Assert(!result.error.empty(), "should error on duplicate ref");
    Assert(Contains(result.error, "Duplicate"), "error must mention duplicate: " + result.error);

    std::cerr << "    error: " << result.error << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Error — unknown node type ────────────────────────────

void TestUnknownNodeType() {
    std::cerr << "  TestUnknownNodeType..." << std::endl;

    auto result = waxcpp::server::CompileBlueprintPatch("",
        R"({
            "create": true, "target": "/Game/T.T",
            "graphs": [{"name": "G", "add_nodes": [
                {"ref": "x", "type": "FooBar"}
            ]}]
        })");

    Assert(!result.error.empty(), "should error on unknown type");
    Assert(Contains(result.error, "FooBar"), "error must mention bad type: " + result.error);

    std::cerr << "    error: " << result.error << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: set_defaults on new node creates pin stub ─────────────

void TestSetDefaultsNewNode() {
    std::cerr << "  TestSetDefaultsNewNode..." << std::endl;

    auto result = waxcpp::server::CompileBlueprintPatch("",
        R"({
            "create": true, "target": "/Game/T.T",
            "graphs": [{"name": "G",
                "add_nodes": [{"ref": "call", "type": "CallFunction", "function": "Foo", "function_owner": "Bar"}],
                "set_defaults": [{"node": "call", "pin": "Damage", "value": "50.0"}]
            }]
        })");

    Assert(result.error.empty(), "should not error: " + result.error);
    Assert(Contains(result.json, "Damage"), "must contain pin name");
    Assert(Contains(result.json, "50.0"), "must contain default value");

    std::cerr << "    PASS" << std::endl;
}

// ── Test: remove_nodes removes associated links ─────────────────

void TestRemoveNodeCascadesLinks() {
    std::cerr << "  TestRemoveNodeCascadesLinks..." << std::endl;

    const std::string existing = R"({
        "graphs": [{
            "name": "G",
            "nodes": [
                {"node_guid": "A", "title": "Alpha", "pins": []},
                {"node_guid": "B", "title": "Beta", "pins": []},
                {"node_guid": "C", "title": "Gamma", "pins": []}
            ],
            "links": [
                {"from_node_guid": "A", "from_pin_name": "then", "to_node_guid": "B", "to_pin_name": "execute"},
                {"from_node_guid": "B", "from_pin_name": "then", "to_node_guid": "C", "to_pin_name": "execute"}
            ]
        }]
    })";

    auto result = waxcpp::server::CompileBlueprintPatch(existing,
        R"({"graphs": [{"name": "G", "remove_nodes": ["Beta"]}]})");

    Assert(result.error.empty(), "should not error: " + result.error);

    Poco::JSON::Parser parser;
    auto parsed = parser.parse(result.json);
    auto root = parsed.extract<Poco::JSON::Object::Ptr>();
    auto nodes = root->getArray("graphs")->getObject(0)->getArray("nodes");
    auto links = root->getArray("graphs")->getObject(0)->getArray("links");

    Assert(nodes->size() == 2, "expected 2 nodes after removal, got " + std::to_string(nodes->size()));
    Assert(links->size() == 0, "expected 0 links after cascade, got " + std::to_string(links->size()));

    std::cerr << "    summary: " << result.summary << std::endl;
    std::cerr << "    PASS" << std::endl;
}

// ── Test: Variables merge ───────────────────────────────────────

void TestVariablesMerge() {
    std::cerr << "  TestVariablesMerge..." << std::endl;

    const std::string existing = R"({
        "graphs": [],
        "variables": [
            {"var_name": "Health", "var_guid": "V1", "default_value": "100.0"}
        ]
    })";

    auto result = waxcpp::server::CompileBlueprintPatch(existing,
        R"({
            "variables": [
                {"var_name": "Health", "default_value": "200.0"},
                {"var_name": "Speed", "default_value": "600.0"}
            ],
            "graphs": []
        })");

    Assert(result.error.empty(), "should not error: " + result.error);
    Assert(Contains(result.json, "200.0"), "Health should be updated to 200");
    Assert(Contains(result.json, "Speed"), "Speed should be added");
    Assert(Contains(result.json, "600.0"), "Speed default should be 600");

    std::cerr << "    PASS" << std::endl;
}

// ── Test: BOM handling in existing JSON ─────────────────────────

void TestBomHandling() {
    std::cerr << "  TestBomHandling..." << std::endl;

    // UTF-8 BOM + valid JSON
    std::string bom = "\xEF\xBB\xBF";
    std::string existing = bom + R"({"graphs":[],"variables":[]})";

    auto result = waxcpp::server::CompileBlueprintPatch(existing,
        R"({"graphs":[], "variables":[{"var_name":"X","default_value":"1"}]})");

    Assert(result.error.empty(), "BOM should be handled: " + result.error);
    Assert(Contains(result.json, "X"), "variable should be added");

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

    run("CreateMode", TestCreateMode);
    run("PatchAddNodes", TestPatchAddNodes);
    run("ExistingNodeNotFound", TestExistingNodeNotFound);
    run("DuplicateRef", TestDuplicateRef);
    run("UnknownNodeType", TestUnknownNodeType);
    run("SetDefaultsNewNode", TestSetDefaultsNewNode);
    run("RemoveNodeCascadesLinks", TestRemoveNodeCascadesLinks);
    run("VariablesMerge", TestVariablesMerge);
    run("BomHandling", TestBomHandling);

    std::cerr << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}
