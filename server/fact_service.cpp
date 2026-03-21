#include "fact_service.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>

namespace waxcpp::server {

std::uint64_t FactService::AddFact(const std::string& entity,
                                   const std::string& attribute,
                                   const std::string& value,
                                   const waxcpp::Metadata& metadata) {
    return orchestrator_.RememberFact(entity, attribute, value, metadata);
}

std::optional<waxcpp::StructuredMemoryEntry> FactService::GetFact(std::uint64_t fact_id) {
    return orchestrator_.GetFactById(fact_id);
}

FactMutationResult FactService::UpdateFact(std::uint64_t fact_id,
                                           const std::string& value,
                                           const waxcpp::Metadata& metadata) {
    return FactMutationResult{
        .id = orchestrator_.UpdateFactById(fact_id, value, metadata),
        .previous_id = fact_id,
    };
}

bool FactService::DeleteFact(std::uint64_t fact_id) {
    return orchestrator_.DeleteFactById(fact_id);
}

FactMutationResult FactService::PinFact(std::uint64_t fact_id, bool pinned) {
    return FactMutationResult{
        .id = orchestrator_.SetFactPinned(fact_id, pinned),
        .previous_id = fact_id,
    };
}

std::vector<waxcpp::StructuredMemoryEntry> FactService::History(std::uint64_t fact_id) {
    return orchestrator_.FactHistoryById(fact_id);
}

std::vector<waxcpp::StructuredMemoryEntry> FactService::SearchByEntityPrefix(const std::string& entity_prefix,
                                                                             int limit) {
    return orchestrator_.RecallFactsByEntityPrefix(entity_prefix, limit);
}

Poco::JSON::Object::Ptr FactService::ToJson(const waxcpp::StructuredMemoryEntry& fact) {
    Poco::JSON::Object::Ptr row = new Poco::JSON::Object();
    row->set("entity", fact.entity);
    row->set("attribute", fact.attribute);
    row->set("value", fact.value);
    row->set("id", static_cast<Poco::Int64>(fact.id));
    row->set("version", static_cast<Poco::Int64>(fact.version));
    row->set("pinned", fact.pinned);
    row->set("deleted", fact.deleted);
    row->set("timestamp_ms", static_cast<Poco::Int64>(fact.timestamp_ms));
    if (fact.supersedes.has_value()) {
        row->set("supersedes", static_cast<Poco::Int64>(*fact.supersedes));
    } else {
        row->set("supersedes", Poco::Dynamic::Var());
    }

    Poco::JSON::Object::Ptr metadata = new Poco::JSON::Object();
    for (const auto& [key, value] : fact.metadata) {
        metadata->set(key, value);
    }
    row->set("metadata", metadata);
    return row;
}

}  // namespace waxcpp::server
