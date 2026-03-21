#pragma once

#include "../include/waxcpp/memory_orchestrator.hpp"

#include <Poco/JSON/Object.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace waxcpp::server {

struct FactMutationResult {
    std::uint64_t id = 0;
    std::optional<std::uint64_t> previous_id{};
};

class FactService {
public:
    explicit FactService(waxcpp::MemoryOrchestrator& orchestrator)
        : orchestrator_(orchestrator) {}

    [[nodiscard]] std::uint64_t AddFact(const std::string& entity,
                                        const std::string& attribute,
                                        const std::string& value,
                                        const waxcpp::Metadata& metadata = {});

    [[nodiscard]] std::optional<waxcpp::StructuredMemoryEntry> GetFact(std::uint64_t fact_id);
    [[nodiscard]] FactMutationResult UpdateFact(std::uint64_t fact_id,
                                                const std::string& value,
                                                const waxcpp::Metadata& metadata = {});
    [[nodiscard]] bool DeleteFact(std::uint64_t fact_id);
    [[nodiscard]] FactMutationResult PinFact(std::uint64_t fact_id, bool pinned = true);
    [[nodiscard]] std::vector<waxcpp::StructuredMemoryEntry> History(std::uint64_t fact_id);
    [[nodiscard]] std::vector<waxcpp::StructuredMemoryEntry> SearchByEntityPrefix(const std::string& entity_prefix,
                                                                                  int limit);

    [[nodiscard]] static Poco::JSON::Object::Ptr ToJson(const waxcpp::StructuredMemoryEntry& fact);

private:
    waxcpp::MemoryOrchestrator& orchestrator_;
};

}  // namespace waxcpp::server
