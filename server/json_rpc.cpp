// cpp/server/json_rpc.cpp
#include "json_rpc.hpp"

#include <Poco/Exception.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

#include <sstream>
#include <stdexcept>

namespace waxcpp::server {

JsonRequest parse_json_rpc(const std::string& body) {
    Poco::JSON::Parser parser;
    const Poco::Dynamic::Var parsed = parser.parse(body);
    Poco::JSON::Object::Ptr json = parsed.extract<Poco::JSON::Object::Ptr>();
    if (json.isNull()) {
        throw std::runtime_error("Invalid JSON-RPC payload: expected object");
    }

    Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
    if (json->has("params")) {
        try {
            params = json->getObject("params");
            if (params.isNull()) {
                params = new Poco::JSON::Object();
            }
        } catch (const Poco::Exception&) {
            params = new Poco::JSON::Object();
        }
    }

    return JsonRequest{
        .jsonrpc = json->optValue<std::string>("jsonrpc", ""),
        .method = json->optValue<std::string>("method", ""),
        .params = params,
        .id = json->optValue<int>("id", 0)
    };
}

std::string make_json_response(const JsonRequest& request,
                               const std::string& result,
                               const std::string& error) {
    Poco::JSON::Object response;
    response.set("jsonrpc", "2.0");
    response.set("id", request.id);

    if (!error.empty()) {
        Poco::JSON::Object::Ptr error_obj = new Poco::JSON::Object();
        error_obj->set("code", -32000);
        error_obj->set("message", error);
        response.set("error", error_obj);
    } else if (!result.empty()) {
        response.set("result", result);
    }

    std::ostringstream out;
    response.stringify(out);
    return out.str();
}

} // namespace waxcpp::server
