// tools/bpl_structural_enricher_poc.cpp
//
// CLI wrapper over server/bpl_structural_enricher. Prints the extracted facts
// for one .bpl_json file in a human-readable form. Useful for quickly
// inspecting what the deterministic enricher sees for any given Blueprint.
//
// Usage:  waxcpp_bpl_structural_enricher_poc <path.bpl_json>

#include "../server/bpl_structural_enricher.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open: " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void PrintList(std::string_view label, const std::vector<std::string>& xs) {
    if (xs.empty()) return;
    std::cout << "  " << label << " (" << xs.size() << "):\n    ";
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << xs[i];
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: waxcpp_bpl_structural_enricher_poc <path.bpl_json>\n";
        return 1;
    }
    try {
        const std::filesystem::path p = argv[1];
        const auto raw = ReadFile(p);

        const auto facts = waxcpp::server::BplStructuralEnricher::Enrich(raw);
        if (!facts.has_value()) {
            std::cerr << "ERROR: enrich returned nullopt (invalid JSON or missing asset_name)\n";
            return 2;
        }

        const auto& f = *facts;
        std::cout << "\n══════════════════════════════════════════════════════════\n"
                  << " Structural facts\n"
                  << "══════════════════════════════════════════════════════════\n\n"
                  << f.entity << '\n'
                  << std::string(f.entity.size(), '-') << '\n'
                  << "  asset_name:        " << f.asset_name << '\n'
                  << "  asset_path:        " << f.asset_path << '\n'
                  << "  kind:              " << f.kind << '\n'
                  << "  parent_class_hint: " << f.parent_class_hint << '\n'
                  << "  node_count / link_count: " << f.node_count << " / " << f.link_count << '\n'
                  << "  structural_hash:   " << f.structural_hash << '\n';

        PrintList("graphs",          f.graphs);
        PrintList("events",          f.events);
        PrintList("event_owners",    f.event_owners);
        PrintList("custom_events",   f.custom_events);
        PrintList("calls",           f.calls);
        PrintList("call_owners",     f.call_owners);
        PrintList("gets_variables",  f.gets_variables);
        PrintList("sets_variables",  f.sets_variables);
        PrintList("casts_to",        f.casts_to);
        PrintList("macros",          f.macros);

        if (!f.variables.empty()) {
            std::cout << "  variables (" << f.variables.size() << "):\n";
            for (const auto& v : f.variables) {
                std::cout << "    " << v.name << " : " << v.type << '\n';
            }
        }
        if (!f.exec_chains.empty()) {
            std::cout << "\n  exec chains:\n";
            for (const auto& [evt, chain] : f.exec_chains) {
                std::cout << "    " << f.entity << '.' << evt << ":\n      " << chain << '\n';
            }
        }
        std::cout << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 2;
    }
}
