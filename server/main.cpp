// cpp/server/main.cpp
#include "wax_rag_handler.hpp"
#include "runtime_config.hpp"
#include "server_utils.hpp"
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Logger.h>
#include <Poco/AutoPtr.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <cstdlib>
#include <chrono>
#include <iterator>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace Poco::Net;
using namespace Poco::Util;

namespace {
double SteadyNowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsTruthyLogValue(std::string_view value) {
    const auto normalized = waxcpp::server::ToAsciiLower(value);
    return normalized == "1" || normalized == "true" || normalized == "on" || normalized == "verbose";
}

bool IsFalsyLogValue(std::string_view value) {
    const auto normalized = waxcpp::server::ToAsciiLower(value);
    return normalized == "0" || normalized == "false" || normalized == "off" || normalized == "normal" ||
           normalized == "info" || normalized == "information";
}

bool ResolveVerboseRpcLoggingFromEnv() {
    const auto rpc_log = waxcpp::server::EnvString("WAXCPP_RPC_LOG");
    if (rpc_log.has_value()) {
        return IsTruthyLogValue(*rpc_log);
    }
    const auto server_log = waxcpp::server::EnvString("WAXCPP_SERVER_LOG");
    if (server_log.has_value()) {
        return IsTruthyLogValue(*server_log);
    }
    return false;
}

void SetProcessEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::optional<Poco::JSON::Object::Ptr> ParseJsonObject(std::string_view text) {
    try {
        Poco::JSON::Parser parser;
        const Poco::Dynamic::Var parsed = parser.parse(std::string(text));
        Poco::JSON::Object::Ptr object = parsed.extract<Poco::JSON::Object::Ptr>();
        if (object.isNull()) {
            return std::nullopt;
        }
        return object;
    } catch (...) {
        return std::nullopt;
    }
}

std::string JsonStringValue(const Poco::JSON::Object::Ptr& object, const std::string& key) {
    if (object.isNull() || !object->has(key)) {
        return {};
    }
    try {
        return object->optValue<std::string>(key, "");
    } catch (...) {
        return {};
    }
}

std::string SummarizeRpcRequest(const waxcpp::server::JsonRequest& request) {
    const auto& params = request.params;
    if (request.method == "fact.add") {
        return "entity=" + params->optValue<std::string>("entity", "") +
               " attribute=" + params->optValue<std::string>("attribute", "") +
               " value=" + params->optValue<std::string>("value", "");
    }
    if (request.method == "fact.get" || request.method == "fact.delete" || request.method == "fact.history") {
        return "id=" + std::to_string(params->optValue<Poco::Int64>("id", -1));
    }
    if (request.method == "fact.update") {
        return "id=" + std::to_string(params->optValue<Poco::Int64>("id", -1)) +
               " value=" + params->optValue<std::string>("value", "");
    }
    if (request.method == "fact.pin") {
        return "id=" + std::to_string(params->optValue<Poco::Int64>("id", -1)) +
               " pinned=" + std::string(params->optValue<bool>("pinned", true) ? "true" : "false");
    }
    if (request.method == "fact.search") {
        return "entity_prefix=" + params->optValue<std::string>("entity_prefix", "") +
               " limit=" + std::to_string(params->optValue<int>("limit", 0));
    }
    if (request.method == "recall" || request.method == "answer.generate") {
        return "query=\"" + params->optValue<std::string>("query", "") + "\"";
    }
    if (request.method == "remember") {
        return "content_len=" + std::to_string(params->optValue<std::string>("content", "").size());
    }
    if (request.method == "index.start") {
        return "repo_root=" + params->optValue<std::string>("repo_root", "") +
               " resume=" + std::string(params->optValue<bool>("resume", false) ? "true" : "false");
    }
    return {};
}

std::string SummarizeRpcResponse(const std::string& method, const std::string& result) {
    const auto parsed = ParseJsonObject(result);
    if (!parsed.has_value()) {
        return {};
    }
    const auto& obj = *parsed;

    if (method == "fact.add") {
        if (!obj->has("fact")) {
            return {};
        }
        const auto fact = obj->getObject("fact");
        return "id=" + std::to_string(obj->optValue<Poco::Int64>("id", -1)) +
               " value=" + JsonStringValue(fact, "value");
    }
    if (method == "fact.get") {
        if (!obj->has("fact")) {
            return {};
        }
        const auto fact = obj->getObject("fact");
        return "id=" + std::to_string(obj->optValue<Poco::Int64>("id", -1)) +
               " value=" + JsonStringValue(fact, "value");
    }
    if (method == "fact.update") {
        return "id=" + std::to_string(obj->optValue<Poco::Int64>("id", -1)) +
               " previous_id=" + std::to_string(obj->optValue<Poco::Int64>("previous_id", -1));
    }
    if (method == "fact.pin") {
        return "id=" + std::to_string(obj->optValue<Poco::Int64>("id", -1)) +
               " previous_id=" + std::to_string(obj->optValue<Poco::Int64>("previous_id", -1));
    }
    if (method == "fact.delete") {
        return "id=" + std::to_string(obj->optValue<Poco::Int64>("id", -1)) +
               " deleted=" + std::string(obj->optValue<bool>("deleted", false) ? "true" : "false");
    }
    if (method == "fact.history") {
        return "id=" + std::to_string(obj->optValue<Poco::Int64>("id", -1)) +
               " count=" + std::to_string(obj->optValue<int>("count", 0));
    }
    if (method == "fact.search") {
        return "count=" + std::to_string(obj->optValue<int>("count", 0));
    }
    if (method == "recall") {
        return "count=" + std::to_string(obj->optValue<int>("count", 0)) +
               " total_tokens=" + std::to_string(obj->optValue<int>("total_tokens", 0));
    }
    if (method == "answer.generate") {
        return "context_items_used=" + std::to_string(obj->optValue<int>("context_items_used", 0)) +
               " total_context_tokens=" + std::to_string(obj->optValue<int>("total_context_tokens", 0));
    }
    if (method == "index.start") {
        return "status=" + JsonStringValue(obj, "status");
    }
    return {};
}
}  // namespace

class RAGRequestHandler : public HTTPRequestHandler {
public:
    RAGRequestHandler(waxcpp::server::WaxRAGHandler& handler, bool verbose_rpc_logging)
        : handler_(handler),
          verbose_rpc_logging_(verbose_rpc_logging) {}

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override {
        std::string body;
        try {
            std::istream& is = request.stream();
            std::copy(std::istreambuf_iterator<char>(is),
                     std::istreambuf_iterator<char>(),
                     std::back_inserter(body));

            // Парсим JSON-RPC
            auto json_request = waxcpp::server::parse_json_rpc(body);

            const auto t0 = SteadyNowMs();
            if (verbose_rpc_logging_) {
                std::cerr << "[RPC] >> id=" << json_request.id
                          << " method=" << json_request.method << std::endl;
                const auto request_summary = SummarizeRpcRequest(json_request);
                if (!request_summary.empty()) {
                    std::cerr << "[RPC] .. " << request_summary << std::endl;
                }
            }

            std::string result;
            if (json_request.method == "remember") {
                result = handler_.handle_remember(json_request.params);
            } else if (json_request.method == "recall") {
                result = handler_.handle_recall(json_request.params);
            } else if (json_request.method == "answer.generate") {
                result = handler_.handle_answer_generate(json_request.params);
            } else if (json_request.method == "flush") {
                result = handler_.handle_flush(json_request.params);
            } else if (json_request.method == "index.start") {
                result = handler_.handle_index_start(json_request.params);
            } else if (json_request.method == "index.status") {
                result = handler_.handle_index_status(json_request.params);
            } else if (json_request.method == "index.stop") {
                result = handler_.handle_index_stop(json_request.params);
            } else if (json_request.method == "blueprint.read") {
                result = handler_.handle_blueprint_read(json_request.params);
            } else if (json_request.method == "blueprint.write") {
                result = handler_.handle_blueprint_write(json_request.params);
            } else if (json_request.method == "blueprint.import") {
                result = handler_.handle_blueprint_import(json_request.params);
            } else if (json_request.method == "fact.add") {
                result = handler_.handle_fact_add(json_request.params);
            } else if (json_request.method == "fact.get") {
                result = handler_.handle_fact_get(json_request.params);
            } else if (json_request.method == "fact.update") {
                result = handler_.handle_fact_update(json_request.params);
            } else if (json_request.method == "fact.delete") {
                result = handler_.handle_fact_delete(json_request.params);
            } else if (json_request.method == "fact.pin") {
                result = handler_.handle_fact_pin(json_request.params);
            } else if (json_request.method == "fact.history") {
                result = handler_.handle_fact_history(json_request.params);
            } else if (json_request.method == "fact.search") {
                result = handler_.handle_fact_search(json_request.params);
            } else {
                result = "Unknown method: " + json_request.method;
            }

            const auto elapsed = SteadyNowMs() - t0;
            if (verbose_rpc_logging_) {
                std::cerr << "[RPC] << id=" << json_request.id
                          << " method=" << json_request.method
                          << " done in " << static_cast<int>(elapsed) << " ms"
                          << " (response " << result.size() << " bytes)" << std::endl;
                const auto response_summary = SummarizeRpcResponse(json_request.method, result);
                if (!response_summary.empty()) {
                    std::cerr << "[RPC] .. " << response_summary << std::endl;
                }
            } else {
                std::cerr << "[HTTP] << " << json_request.method
                          << " done in " << static_cast<int>(elapsed) << " ms"
                          << " (response " << result.size() << " bytes)" << std::endl;
            }

            // Отправляем JSON-RPC ответ
            response.setContentType("application/json");
            response.setStatus(HTTPResponse::HTTP_OK);
            response.send() << result;

        } catch (const std::exception& e) {
            if (verbose_rpc_logging_) {
                std::cerr << "[RPC] !! exception: " << e.what() << std::endl;
                if (!body.empty()) {
                    std::cerr << "[RPC] .. raw request bytes=" << body.size() << std::endl;
                }
            } else {
                std::cerr << "[HTTP] !! exception: " << e.what() << std::endl;
            }
            response.setStatus(HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            response.send() << "{\"error\":\"" << waxcpp::server::JsonEscape(e.what()) << "\"}";
        }
    }

private:
    waxcpp::server::WaxRAGHandler& handler_;
    bool verbose_rpc_logging_ = false;
};

class RAGRequestHandlerFactory : public HTTPRequestHandlerFactory {
public:
    RAGRequestHandlerFactory(waxcpp::server::WaxRAGHandler& handler, bool verbose_rpc_logging)
        : handler_(handler),
          verbose_rpc_logging_(verbose_rpc_logging) {}

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest&) override {
        return new RAGRequestHandler(handler_, verbose_rpc_logging_);
    }

private:
    waxcpp::server::WaxRAGHandler& handler_;
    bool verbose_rpc_logging_ = false;
};

class RAGServer : public ServerApplication {
protected:
    void defineOptions(Poco::Util::OptionSet& options) override {
        ServerApplication::defineOptions(options);
        options.addOption(Poco::Util::Option("rpc-log", "", "Set RPC log mode. Use 'verbose' to log request/response bodies.")
                              .required(false)
                              .repeatable(false)
                              .argument("level"));
        options.addOption(Poco::Util::Option("log", "", "Alias for rpc-log. Use 'verbose' to log RPC request/response bodies.")
                              .required(false)
                              .repeatable(false)
                              .argument("level"));
    }

    void handleOption(const std::string& name, const std::string& value) override {
        if (name == "rpc-log" || name == "log") {
            if (!IsTruthyLogValue(value) && !IsFalsyLogValue(value)) {
                throw std::runtime_error("invalid --rpc-log value: " + value + " (expected verbose|normal|info|off|true|false|1|0)");
            }
            verbose_rpc_logging_ = IsTruthyLogValue(value);
            SetProcessEnv("WAXCPP_SERVER_LOG", verbose_rpc_logging_ ? "1" : "0");
            return;
        }
        ServerApplication::handleOption(name, value);
    }

    int main(const std::vector<std::string>& args) override {
        (void)args;
        // Настройка логирования
        auto& logger = Poco::Logger::get("WaxRAGServer");
        Poco::AutoPtr<Poco::ConsoleChannel> channel = new Poco::ConsoleChannel;
        Poco::AutoPtr<Poco::PatternFormatter> formatter = new Poco::PatternFormatter("%Y-%m-%d %H:%M:%S [%p] %t");
        Poco::AutoPtr<Poco::FormattingChannel> logChannel = new Poco::FormattingChannel(formatter, channel);
        logger.setChannel(logChannel);
        logger.setLevel("information");
        logger.information("RPC request/response logging: " + std::string(verbose_rpc_logging_ ? "verbose" : "normal"));

        // Параметры сервера
        unsigned short port = static_cast<unsigned short>(config().getUInt("port", 8080));
        int maxQueue = config().getInt("maxQueue", 64);
        int maxThreads = config().getInt("maxThreads", 8);

        // Создание сокета и сервера
        ServerSocket socket(port);
        Poco::Net::HTTPServerParams::Ptr params = new Poco::Net::HTTPServerParams;
        params->setMaxQueued(maxQueue);
        params->setMaxThreads(maxThreads);

        auto runtime_config_path = waxcpp::server::ResolveServerRuntimeConfigPathFromEnv();
        auto runtime_config = waxcpp::server::LoadServerRuntimeConfig(runtime_config_path);
        logger.information("Generation runtime: " + runtime_config.models.generation_model.runtime);
        logger.information("Generation model: " + runtime_config.models.generation_model.model_path);
        logger.information("llama.cpp generation endpoint: " +
                           waxcpp::server::EnvString("WAXCPP_LLAMA_GEN_ENDPOINT")
                               .value_or("http://127.0.0.1:8004/completion (default)"));
        const bool gen_api_key_enabled =
            waxcpp::server::EnvString("WAXCPP_LLAMA_GEN_API_KEY").has_value() ||
            waxcpp::server::EnvString("WAXCPP_LLAMA_API_KEY").has_value();
        logger.information("llama.cpp generation api key: " + std::string(gen_api_key_enabled ? "set" : "not set"));
        logger.information("llama.cpp generation timeout ms: " +
                           waxcpp::server::EnvString("WAXCPP_LLAMA_GEN_TIMEOUT_MS")
                               .value_or("60000 (default)"));
        logger.information("llama.cpp generation max retries: " +
                           waxcpp::server::EnvString("WAXCPP_LLAMA_GEN_MAX_RETRIES")
                               .value_or("2 (default)"));
        logger.information("llama.cpp generation retry backoff ms: " +
                           waxcpp::server::EnvString("WAXCPP_LLAMA_GEN_RETRY_BACKOFF_MS")
                               .value_or("100 (default)"));
        logger.information("Embedding runtime: " + runtime_config.models.embedding_model.runtime);
        logger.information("Embedding model: " +
                           (runtime_config.models.embedding_model.model_path.empty()
                                ? std::string("(disabled)")
                                : runtime_config.models.embedding_model.model_path));
        logger.information("llama.cpp root: " +
                           (runtime_config.models.llama_cpp_root.empty()
                                ? std::string("(not set)")
                                : runtime_config.models.llama_cpp_root));
        logger.information("Vector search enabled: " +
                           std::string(runtime_config.models.enable_vector_search ? "true" : "false"));
        logger.information("Orchestrator ingest concurrency: " +
                           waxcpp::server::EnvString("WAXCPP_ORCH_INGEST_CONCURRENCY")
                               .value_or("1 (default)"));
        logger.information("Orchestrator ingest batch size: " +
                           waxcpp::server::EnvString("WAXCPP_ORCH_INGEST_BATCH_SIZE")
                               .value_or("32 (default)"));
        if (runtime_config.models.enable_vector_search) {
            logger.information("llama.cpp embedding endpoint: " +
                               waxcpp::server::EnvString("WAXCPP_LLAMA_EMBED_ENDPOINT")
                                   .value_or("http://127.0.0.1:8004/embedding (default)"));
            const bool embed_api_key_enabled =
                waxcpp::server::EnvString("WAXCPP_LLAMA_EMBED_API_KEY").has_value() ||
                waxcpp::server::EnvString("WAXCPP_LLAMA_API_KEY").has_value();
            logger.information("llama.cpp embedding api key: " +
                               std::string(embed_api_key_enabled ? "set" : "not set"));
            logger.information("llama.cpp embedding dimensions: " +
                               waxcpp::server::EnvString("WAXCPP_LLAMA_EMBED_DIMS")
                                   .value_or("1024 (default)"));
            logger.information("llama.cpp embedding max retries: " +
                               waxcpp::server::EnvString("WAXCPP_LLAMA_EMBED_MAX_RETRIES")
                                   .value_or("2 (default)"));
            logger.information("llama.cpp embedding retry backoff ms: " +
                               waxcpp::server::EnvString("WAXCPP_LLAMA_EMBED_RETRY_BACKOFF_MS")
                                   .value_or("100 (default)"));
            logger.information("llama.cpp embedding batch concurrency: " +
                               waxcpp::server::EnvString("WAXCPP_LLAMA_EMBED_MAX_BATCH_CONCURRENCY")
                                   .value_or("4 (default)"));
        }
        if (runtime_config_path.has_value()) {
            logger.information("Runtime config file: " + runtime_config_path->string());
        }

        // Инициализация WAX — store lives in data/ subdirectory to keep bin/ clean
        const std::filesystem::path store_dir = "data";
        std::filesystem::create_directories(store_dir);
        const auto store_path = store_dir / "wax-server.mv2s";
        logger.information("Store path: " + store_path.string());
        logger.information("Initializing WAX orchestrator...");
        waxcpp::server::WaxRAGHandler handler(store_path, runtime_config.models);
        logger.information("WAX orchestrator initialized");
        logger.information("FTS5 full-text search: %s",
                           std::string(handler.IsFts5Active() ? "enabled (SQLite)" : "disabled (brute-force TF-IDF)"));

        // Запуск сервера
        HTTPServer server(new RAGRequestHandlerFactory(handler, verbose_rpc_logging_), socket, params);
        server.start();

        logger.information("WAX RAG server started on port %hu", port);
        waitForTerminationRequest();

        logger.information("Shutting down WAX server...");
        server.stop();
        return Application::EXIT_OK;
    }

private:
    bool verbose_rpc_logging_ = ResolveVerboseRpcLoggingFromEnv();
};

int main(int argc, char** argv) {
    RAGServer app;
    return app.run(argc, argv);
}
