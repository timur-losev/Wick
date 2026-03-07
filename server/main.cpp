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
#include <Poco/Util/ServerApplication.h>
#include <Poco/Logger.h>
#include <Poco/AutoPtr.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <chrono>
#include <iterator>
#include <iostream>
#include <string>

using namespace Poco::Net;
using namespace Poco::Util;

namespace {
double SteadyNowMs() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

class RAGRequestHandler : public HTTPRequestHandler {
public:
    RAGRequestHandler(waxcpp::server::WaxRAGHandler& handler)
        : handler_(handler) {}

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override {
        try {
            std::string body;
            std::istream& is = request.stream();
            std::copy(std::istreambuf_iterator<char>(is),
                     std::istreambuf_iterator<char>(),
                     std::back_inserter(body));

            // Парсим JSON-RPC
            auto json_request = waxcpp::server::parse_json_rpc(body);

            const auto t0 = SteadyNowMs();
            std::cerr << "[HTTP] >> " << json_request.method << std::endl;

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
            } else if (json_request.method == "fact.search") {
                result = handler_.handle_fact_search(json_request.params);
            } else {
                result = "Unknown method: " + json_request.method;
            }

            const auto elapsed = SteadyNowMs() - t0;
            std::cerr << "[HTTP] << " << json_request.method
                      << " done in " << static_cast<int>(elapsed) << " ms"
                      << " (response " << result.size() << " bytes)" << std::endl;

            // Отправляем JSON-RPC ответ
            response.setContentType("application/json");
            response.setStatus(HTTPResponse::HTTP_OK);
            response.send() << result;

        } catch (const std::exception& e) {
            std::cerr << "[HTTP] !! exception: " << e.what() << std::endl;
            response.setStatus(HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            response.send() << "{\"error\":\"" << waxcpp::server::JsonEscape(e.what()) << "\"}";
        }
    }

private:
    waxcpp::server::WaxRAGHandler& handler_;
};

class RAGRequestHandlerFactory : public HTTPRequestHandlerFactory {
public:
    RAGRequestHandlerFactory(waxcpp::server::WaxRAGHandler& handler)
        : handler_(handler) {}

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest&) override {
        return new RAGRequestHandler(handler_);
    }

private:
    waxcpp::server::WaxRAGHandler& handler_;
};

class RAGServer : public ServerApplication {
protected:
    int main(const std::vector<std::string>& args) override {
        (void)args;
        // Настройка логирования
        auto& logger = Poco::Logger::get("WaxRAGServer");
        Poco::AutoPtr<Poco::ConsoleChannel> channel = new Poco::ConsoleChannel;
        Poco::AutoPtr<Poco::PatternFormatter> formatter = new Poco::PatternFormatter("%Y-%m-%d %H:%M:%S [%p] %t");
        Poco::AutoPtr<Poco::FormattingChannel> logChannel = new Poco::FormattingChannel(formatter, channel);
        logger.setChannel(logChannel);
        logger.setLevel("information");

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
        HTTPServer server(new RAGRequestHandlerFactory(handler), socket, params);
        server.start();

        logger.information("WAX RAG server started on port %hu", port);
        waitForTerminationRequest();

        logger.information("Shutting down WAX server...");
        server.stop();
        return Application::EXIT_OK;
    }
};

int main(int argc, char** argv) {
    RAGServer app;
    return app.run(argc, argv);
}
