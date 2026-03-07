#include "llama_cpp_generation_client.hpp"
#include "server_utils.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/URI.h>

#include <chrono>
#include <iostream>
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

std::string ExtractGenerationText(const Poco::JSON::Object::Ptr& root) {
  if (root.isNull()) {
    return {};
  }
  if (root->has("content")) {
    try {
      return root->getValue<std::string>("content");
    } catch (const Poco::Exception&) {
      return {};
    }
  }
  if (root->has("response")) {
    try {
      return root->getValue<std::string>("response");
    } catch (const Poco::Exception&) {
      return {};
    }
  }
  if (root->has("text")) {
    try {
      return root->getValue<std::string>("text");
    } catch (const Poco::Exception&) {
      return {};
    }
  }
  if (root->has("choices")) {
    try {
      const auto choices = root->getArray("choices");
      if (!choices.isNull() && !choices->empty() && choices->isObject(0)) {
        const auto first = choices->getObject(0);
        if (!first.isNull()) {
          if (first->has("text")) {
            return first->getValue<std::string>("text");
          }
          if (first->has("message")) {
            const auto message = first->getObject("message");
            if (!message.isNull() && message->has("content")) {
              return message->getValue<std::string>("content");
            }
          }
        }
      }
    } catch (const Poco::Exception&) {
      return {};
    }
  }
  return {};
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
    throw std::runtime_error("llama.cpp generation client requires endpoint or request_fn");
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

std::string LlamaCppGenerationClient::BuildRequestBody(const LlamaCppGenerationRequest& request) {
  const bool use_chat = !request.system_prompt.empty();
  std::ostringstream out;
  if (use_chat) {
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

  // Detect chat vs completion format from request body.
  const bool is_chat = body.find("\"messages\"") != std::string::npos;

  Poco::URI uri(config_.endpoint);
  std::string path;
  if (is_chat) {
    // Override path to /v1/chat/completions regardless of configured endpoint.
    path = "/v1/chat/completions";
  } else {
    path = uri.getPathEtc();
    if (path.empty()) {
      path = "/";
    }
  }

  Poco::Net::HTTPClientSession session(uri.getHost(), uri.getPort());
  session.setTimeout(Poco::Timespan(0, config_.timeout_ms * 1000));

  Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, path, Poco::Net::HTTPMessage::HTTP_1_1);
  request.setContentType("application/json");
  request.setContentLength(static_cast<int>(body.size()));
  request.set("Accept", "application/json");
  if (!config_.api_key.empty()) {
    request.set("Authorization", "Bearer " + config_.api_key);
  }

  std::ostream& req_stream = session.sendRequest(request);
  req_stream.write(body.data(), static_cast<std::streamsize>(body.size()));

  Poco::Net::HTTPResponse response{};
  std::istream& resp_stream = session.receiveResponse(response);
  std::ostringstream payload{};
  payload << resp_stream.rdbuf();

  if (response.getStatus() >= 400) {
    throw std::runtime_error("llama.cpp generation endpoint returned HTTP " + std::to_string(response.getStatus()));
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
