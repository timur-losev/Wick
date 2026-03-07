#include "runtime_config.hpp"
#include "server_utils.hpp"

#include <Poco/Exception.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Dynamic/Var.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace waxcpp::server {

namespace {

std::optional<Poco::JSON::Object::Ptr> OptObject(const Poco::JSON::Object::Ptr& root,
                                                 const std::string& key) {
  if (root.isNull() || !root->has(key)) {
    return std::nullopt;
  }
  try {
    return root->getObject(key);
  } catch (const Poco::Exception&) {
    return std::nullopt;
  }
}

void OverlayModelSpec(const Poco::JSON::Object::Ptr& object, waxcpp::ModelRuntimeSpec& out) {
  if (object.isNull()) {
    return;
  }
  out.runtime = object->optValue<std::string>("runtime", out.runtime);
  out.model_path = object->optValue<std::string>("model_path", out.model_path);
}

void OverlayLlamaCppRoot(const Poco::JSON::Object::Ptr& object, waxcpp::RuntimeModelsConfig& models) {
  if (object.isNull()) {
    return;
  }
  const auto value = object->optValue<std::string>("llama_cpp_root", models.llama_cpp_root);
  if (!value.empty()) {
    models.llama_cpp_root = value;
  }
}

void OverlayFromJson(const Poco::JSON::Object::Ptr& root, waxcpp::RuntimeModelsConfig& models) {
  if (root.isNull()) {
    return;
  }
  OverlayLlamaCppRoot(root, models);
  models.enable_vector_search = root->optValue<bool>("enable_vector_search", models.enable_vector_search);
  models.require_distinct_models =
      root->optValue<bool>("require_distinct_models", models.require_distinct_models);

  if (const auto runtimes = OptObject(root, "runtimes"); runtimes.has_value()) {
    OverlayLlamaCppRoot(*runtimes, models);
  }

  if (const auto retrieval = OptObject(root, "retrieval"); retrieval.has_value()) {
    models.enable_vector_search =
        (*retrieval)->optValue<bool>("enable_vector_search", models.enable_vector_search);
    models.require_distinct_models =
        (*retrieval)->optValue<bool>("require_distinct_models", models.require_distinct_models);
  }

  if (const auto models_obj = OptObject(root, "models"); models_obj.has_value()) {
    OverlayLlamaCppRoot(*models_obj, models);
    if (const auto generation = OptObject(*models_obj, "generation_model"); generation.has_value()) {
      OverlayModelSpec(*generation, models.generation_model);
    }
    if (const auto embedding = OptObject(*models_obj, "embedding_model"); embedding.has_value()) {
      OverlayModelSpec(*embedding, models.embedding_model);
    }
  }

  if (const auto generation = OptObject(root, "generation_model"); generation.has_value()) {
    OverlayModelSpec(*generation, models.generation_model);
  }
  if (const auto embedding = OptObject(root, "embedding_model"); embedding.has_value()) {
    OverlayModelSpec(*embedding, models.embedding_model);
  }
}

}  // namespace

ServerRuntimeConfig DefaultServerRuntimeConfig() {
  ServerRuntimeConfig config{};
  config.models.generation_model.runtime = "llama_cpp";
  config.models.generation_model.model_path =
      EnvString("WAXCPP_GENERATION_MODEL").value_or(std::string{});
  config.models.embedding_model.runtime = "disabled";
  config.models.embedding_model.model_path = "";
  if (const auto env_root = EnvString("WAXCPP_LLAMA_CPP_ROOT"); env_root.has_value() && !env_root->empty()) {
    config.models.llama_cpp_root = *env_root;
  }
  config.models.enable_vector_search = false;
  config.models.require_distinct_models = true;
  return config;
}

std::optional<std::filesystem::path> ResolveServerRuntimeConfigPathFromEnv() {
  const auto env = EnvString("WAXCPP_SERVER_CONFIG");
  if (!env.has_value() || env->empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(*env);
}

ServerRuntimeConfig LoadServerRuntimeConfig(const std::optional<std::filesystem::path>& config_path) {
  auto config = DefaultServerRuntimeConfig();
  if (!config_path.has_value()) {
    waxcpp::ValidateRuntimeModelsConfig(config.models);
    return config;
  }

  const auto path = *config_path;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open server runtime config: " + path.string());
  }

  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in.good() && !in.eof()) {
    throw std::runtime_error("failed to read server runtime config: " + path.string());
  }

  Poco::JSON::Parser parser;
  Poco::Dynamic::Var parsed;
  try {
    parsed = parser.parse(buffer.str());
  } catch (const Poco::Exception& ex) {
    throw std::runtime_error(std::string("invalid JSON in server runtime config: ") + ex.displayText());
  }

  Poco::JSON::Object::Ptr root;
  try {
    root = parsed.extract<Poco::JSON::Object::Ptr>();
  } catch (const Poco::Exception&) {
    throw std::runtime_error("server runtime config must be a JSON object");
  }

  OverlayFromJson(root, config.models);
  waxcpp::ValidateRuntimeModelsConfig(config.models);
  return config;
}

}  // namespace waxcpp::server
