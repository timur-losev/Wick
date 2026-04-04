#include "llama_cpp_generation_client.hpp"
#include "server_utils.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/URI.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace waxcpp::server {

namespace {

/// Strip <think>...</think> blocks from Qwen3-style thinking output.
/// Preserves the actual answer that follows after the closing tag.
/// If no think tags found, returns the original text unchanged.
std::string StripThinkingBlocks(const std::string& text) {
  std::string result = text;
  while (true) {
    const auto open_pos = result.find("<think>");
    if (open_pos == std::string::npos) break;
    const auto close_pos = result.find("</think>", open_pos);
    if (close_pos == std::string::npos) {
      // Unclosed <think> — strip everything from <think> to end
      result.erase(open_pos);
      break;
    }
    // Erase <think>...</think> including the tags
    result.erase(open_pos, close_pos + 8 - open_pos);
  }
  // Trim leading whitespace left after stripping
  const auto first_non_ws = result.find_first_not_of(" \t\n\r");
  if (first_non_ws != std::string::npos && first_non_ws > 0) {
    result.erase(0, first_non_ws);
  } else if (first_non_ws == std::string::npos) {
    result.clear();
  }
  return result;
}

std::optional<std::string> TryGetStringField(
    const Poco::JSON::Object::Ptr& root,
    const char* key) {
  if (root.isNull() || !root->has(key)) {
    return std::nullopt;
  }
  try {
    return root->getValue<std::string>(key);
  } catch (const Poco::Exception&) {
    return std::nullopt;
  }
}

std::string ExtractGenerationText(const Poco::JSON::Object::Ptr& root) {
  if (root.isNull()) {
    return {};
  }
  if (const auto output_text = TryGetStringField(root, "output_text");
      output_text.has_value()) {
    return *output_text;
  }
  if (root->has("output")) {
    try {
      const auto output = root->getArray("output");
      if (!output.isNull()) {
        for (unsigned int i = 0; i < output->size(); ++i) {
          if (!output->isObject(i)) {
            continue;
          }
          const auto item = output->getObject(i);
          if (item.isNull()) {
            continue;
          }
          if (item->has("content")) {
            const auto content = item->getArray("content");
            if (content.isNull()) {
              continue;
            }
            for (unsigned int j = 0; j < content->size(); ++j) {
              if (!content->isObject(j)) {
                continue;
              }
              const auto content_item = content->getObject(j);
              if (content_item.isNull()) {
                continue;
              }
              if (const auto text = TryGetStringField(content_item, "text");
                  text.has_value()) {
                return *text;
              }
            }
          }
        }
      }
    } catch (const Poco::Exception&) {
    }
  }
  if (root->has("choices")) {
    try {
      const auto choices = root->getArray("choices");
      if (!choices.isNull() && !choices->empty() && choices->isObject(0)) {
        const auto first = choices->getObject(0);
        if (!first.isNull()) {
          if (const auto text = TryGetStringField(first, "text"); text.has_value()) {
            return *text;
          }
          if (first->has("message")) {
            const auto message = first->getObject("message");
            if (const auto content = TryGetStringField(message, "content");
                content.has_value()) {
              return *content;
            }
          }
        }
      }
    } catch (const Poco::Exception&) {
    }
  }
  if (const auto content = TryGetStringField(root, "content"); content.has_value()) {
    return *content;
  }
  if (const auto response = TryGetStringField(root, "response"); response.has_value()) {
    return *response;
  }
  if (const auto text = TryGetStringField(root, "text"); text.has_value()) {
    return *text;
  }
  return {};
}

std::string ResolveOpenAIPath(const Poco::URI& uri, std::string_view resource) {
  auto path = uri.getPath();
  if (path.empty() || path == "/") {
    return "/v1/" + std::string(resource);
  }
  const std::string suffix = "/" + std::string(resource);
  if (path.ends_with(suffix)) {
    return path;
  }
  if (path.back() == '/') {
    path.pop_back();
  }
  return path + suffix;
}

std::string TruncateForError(std::string text, std::size_t limit = 1024) {
  if (text.size() <= limit) {
    return text;
  }
  text.resize(limit);
  text += "...";
  return text;
}

Poco::Net::Context::Ptr OpenAIClientContext() {
  static std::once_flag init_flag;
  static Poco::Net::Context::Ptr context;
  std::call_once(init_flag, []() {
    Poco::Net::initializeSSL();
#if defined(_WIN32)
    context = new Poco::Net::Context(
        Poco::Net::Context::CLIENT_USE,
        "",
        Poco::Net::Context::VERIFY_RELAXED,
        Poco::Net::Context::OPT_DEFAULTS);
    context->requireMinimumProtocol(Poco::Net::Context::PROTO_TLSV1_2);
#else
    context = new Poco::Net::Context(
        Poco::Net::Context::CLIENT_USE,
        "",
        "",
        "",
        Poco::Net::Context::VERIFY_RELAXED,
        9,
        true,
        "ALL");
#endif
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> cert_handler =
        new Poco::Net::AcceptCertificateHandler(false);
    Poco::Net::SSLManager::instance().initializeClient(nullptr, cert_handler, context);
  });
  return context;
}

}  // namespace

LlamaCppGenerationClient::LlamaCppGenerationClient(LlamaCppGenerationConfig config)
    : config_(std::move(config)) {
  if (config_.timeout_ms <= 0) {
    throw std::runtime_error("llama.cpp generation timeout must be positive");
  }
  if (config_.max_retries < 0) {
    throw std::runtime_error("llama.cpp generation max_retries must be >= 0");
  }
  if (config_.retry_backoff_ms < 0) {
    throw std::runtime_error("llama.cpp generation retry_backoff_ms must be >= 0");
  }
  if (config_.request_fn == nullptr && config_.endpoint.empty()) {
    throw std::runtime_error("generation client requires endpoint or request_fn");
  }
  if ((config_.api == GenerationApiKind::kOpenAIResponses ||
       config_.api == GenerationApiKind::kOpenAICompatibleChatCompletions) &&
      config_.model_path.empty()) {
    throw std::runtime_error("remote generation client requires model identifier");
  }
}

std::string LlamaCppGenerationClient::Generate(const LlamaCppGenerationRequest& request) const {
  if (request.prompt.empty()) {
    throw std::runtime_error("generation request prompt must not be empty");
  }
  if (request.max_tokens <= 0) {
    throw std::runtime_error("generation request max_tokens must be positive");
  }
  if (request.temperature < 0.0f) {
    throw std::runtime_error("generation request temperature must be non-negative");
  }
  if (request.top_p <= 0.0f || request.top_p > 1.0f) {
    throw std::runtime_error("generation request top_p must be in (0, 1]");
  }
  const auto response = PerformRequestWithRetry(BuildRequestBody(request));
  const auto raw_text = ParseGenerationResponse(response);
  if (raw_text.empty()) {
    throw std::runtime_error("generation response did not include text");
  }
  // Strip <think>...</think> blocks from Qwen3-style reasoning output,
  // keeping only the final answer for the client.
  const auto text = StripThinkingBlocks(raw_text);
  if (text.empty()) {
    // Model produced only thinking with no actual answer — return raw
    std::cerr << "[GEN] warning: model output was entirely <think> block, returning raw" << std::endl;
    return raw_text;
  }
  if (text.size() < raw_text.size()) {
    std::cerr << "[GEN] stripped thinking: " << raw_text.size() << " -> " << text.size() << " chars" << std::endl;
  }
  return text;
}

std::string LlamaCppGenerationClient::ParseGenerationResponse(const std::string& payload) {
  Poco::JSON::Parser parser{};
  Poco::Dynamic::Var parsed{};
  try {
    parsed = parser.parse(payload);
  } catch (const Poco::Exception& ex) {
    throw std::runtime_error(std::string("generation response is not valid JSON: ") + ex.displayText());
  }

  Poco::JSON::Object::Ptr root{};
  try {
    root = parsed.extract<Poco::JSON::Object::Ptr>();
  } catch (const Poco::Exception&) {
    throw std::runtime_error("generation response root must be a JSON object");
  }

  const auto text = ExtractGenerationText(root);
  if (!text.empty()) {
    return text;
  }
  throw std::runtime_error("generation response does not contain supported text field");
}

std::string LlamaCppGenerationClient::BuildRequestBody(const LlamaCppGenerationRequest& request) const {
  const bool use_chat = !request.system_prompt.empty();
  std::ostringstream out;
  if (config_.api == GenerationApiKind::kOpenAIResponses) {
    out << "{\"model\":\"" << JsonEscape(config_.model_path) << "\"";
    if (!request.system_prompt.empty()) {
      out << ",\"instructions\":\"" << JsonEscape(request.system_prompt) << "\"";
    }
    out << ",\"input\":\"" << JsonEscape(request.prompt) << "\""
        << ",\"max_output_tokens\":" << request.max_tokens
        << ",\"text\":{\"format\":{\"type\":\"text\"},\"verbosity\":\"low\"}";
    if (!config_.reasoning_effort.empty()) {
      out << ",\"reasoning\":{\"effort\":\"" << JsonEscape(config_.reasoning_effort) << "\"}";
    }
    out << "}";
  } else if (config_.api == GenerationApiKind::kOpenAICompatibleChatCompletions) {
    out << "{\"model\":\"" << JsonEscape(config_.model_path) << "\""
        << ",\"messages\":[";
    bool need_comma = false;
    if (!request.system_prompt.empty()) {
      out << "{\"role\":\"system\",\"content\":\"" << JsonEscape(request.system_prompt) << "\"}";
      need_comma = true;
    }
    if (need_comma) {
      out << ",";
    }
    out << "{\"role\":\"user\",\"content\":\"" << JsonEscape(request.prompt) << "\"}"
        << "]"
        << ",\"max_completion_tokens\":" << request.max_tokens
        << ",\"temperature\":" << request.temperature
        << ",\"top_p\":" << request.top_p
        << "}";
  } else if (use_chat) {
    // OpenAI-compatible /v1/chat/completions format.
    // llama-server applies the model's chat template (Qwen3: <think> separation).
    out << "{\"messages\":["
        << "{\"role\":\"system\",\"content\":\"" << JsonEscape(request.system_prompt) << "\"},"
        << "{\"role\":\"user\",\"content\":\"" << JsonEscape(request.prompt) << "\"}"
        << "]"
        << ",\"max_tokens\":" << request.max_tokens
        << ",\"temperature\":" << request.temperature
        << ",\"top_p\":" << request.top_p
        << "}";
  } else {
    // Legacy /completion format (raw prompt, no chat template).
    out << "{\"prompt\":\"" << JsonEscape(request.prompt) << "\""
        << ",\"n_predict\":" << request.max_tokens
        << ",\"temperature\":" << request.temperature
        << ",\"top_p\":" << request.top_p
        << "}";
  }
  return out.str();
}

std::string LlamaCppGenerationClient::PerformRequest(const std::string& body) const {
  if (config_.request_fn != nullptr) {
    return config_.request_fn(body);
  }

  Poco::URI uri(config_.endpoint);
  std::string path;
  if (config_.api == GenerationApiKind::kOpenAIResponses) {
    path = ResolveOpenAIPath(uri, "responses");
  } else if (config_.api == GenerationApiKind::kOpenAICompatibleChatCompletions) {
    path = ResolveOpenAIPath(uri, "chat/completions");
  } else {
    // Detect chat vs completion format from request body.
    const bool is_chat = body.find("\"messages\"") != std::string::npos;
    if (is_chat) {
      // Override path to /v1/chat/completions regardless of configured endpoint.
      path = "/v1/chat/completions";
    } else {
      path = uri.getPathEtc();
      if (path.empty()) {
        path = "/";
      }
    }
  }

  std::unique_ptr<Poco::Net::HTTPClientSession> session;
  if (uri.getScheme() == "https") {
    session = std::make_unique<Poco::Net::HTTPSClientSession>(
        uri.getHost(), uri.getPort(), OpenAIClientContext());
  } else {
    session = std::make_unique<Poco::Net::HTTPClientSession>(uri.getHost(), uri.getPort());
  }
  session->setTimeout(Poco::Timespan(0, config_.timeout_ms * 1000));

  Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, path, Poco::Net::HTTPMessage::HTTP_1_1);
  request.setContentType("application/json");
  request.setContentLength(static_cast<int>(body.size()));
  request.set("Accept", "application/json");
  if (!config_.api_key.empty()) {
    request.set("Authorization", "Bearer " + config_.api_key);
  }

  std::ostream& req_stream = session->sendRequest(request);
  req_stream.write(body.data(), static_cast<std::streamsize>(body.size()));

  Poco::Net::HTTPResponse response{};
  std::istream& resp_stream = session->receiveResponse(response);
  std::ostringstream payload{};
  payload << resp_stream.rdbuf();

  if (response.getStatus() >= 400) {
    throw std::runtime_error(
        "generation endpoint returned HTTP " + std::to_string(response.getStatus()) +
        " (" + response.getReason() + "): " + TruncateForError(payload.str()));
  }
  return payload.str();
}

std::string LlamaCppGenerationClient::PerformRequestWithRetry(const std::string& body) const {
  std::exception_ptr last_error{};
  const int total_attempts = config_.max_retries + 1;
  for (int attempt = 1; attempt <= total_attempts; ++attempt) {
    try {
      return PerformRequest(body);
    } catch (...) {
      last_error = std::current_exception();
      if (attempt >= total_attempts) {
        break;
      }
      if (config_.retry_backoff_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.retry_backoff_ms));
      }
    }
  }
  if (last_error != nullptr) {
    std::rethrow_exception(last_error);
  }
  throw std::runtime_error("generation request retry failed without exception");
}

}  // namespace waxcpp::server
