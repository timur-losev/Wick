// cpp/server/json_rpc.hpp
#pragma once

#include <string>
#include <variant>

#include <Poco/JSON/Object.h>

namespace waxcpp::server {

// JSON-RPC 2.0 Request
struct JsonRequest {
    std::string jsonrpc;
    std::string method;
    Poco::JSON::Object::Ptr params;
    int id = 0;
};

// JSON-RPC 2.0 Response
struct JsonResponse {
    std::string jsonrpc;
    std::variant<Poco::JSON::Object::Ptr, std::string> result;
    std::string error;
    int id = 0;
};

// Парсинг JSON-RPC запроса
JsonRequest parse_json_rpc(const std::string& body);

// Генерация JSON-RPC ответа
std::string make_json_response(const JsonRequest& request, 
                               const std::string& result = "",
                               const std::string& error = "");

} // namespace waxcpp::server
