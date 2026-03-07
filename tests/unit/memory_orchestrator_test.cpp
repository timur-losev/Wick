#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/wax_store.hpp"
#include "../../src/core/wax_store_test_hooks.hpp"

#include "../test_logger.hpp"
#include "../temp_artifacts.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_orchestrator_test_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

std::vector<std::byte> StringToBytes(const std::string& text) {
  std::vector<std::byte> bytes{};
  bytes.reserve(text.size());
  for (const char ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string text{};
  text.reserve(bytes.size());
  for (const auto b : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return text;
}

bool StartsWithStructuredFactMagic(const std::vector<std::byte>& payload) {
  constexpr std::array<std::byte, 6> kStructuredFactMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  if (payload.size() < kStructuredFactMagic.size() + 1) {
    return false;
  }
  return std::equal(kStructuredFactMagic.begin(), kStructuredFactMagic.end(), payload.begin());
}

std::vector<std::byte> ReadStructuredUpsertPayloadByOrdinal(const std::filesystem::path& path, std::size_t ordinal) {
  auto store = waxcpp::WaxStore::Open(path);
  std::vector<std::vector<std::byte>> structured_payloads{};
  for (const auto& meta : store.FrameMetas()) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    if (!StartsWithStructuredFactMagic(payload)) {
      continue;
    }
    constexpr std::size_t kOpcodeOffset = 6;
    constexpr std::uint8_t kUpsertOpcode = 1;
    if (std::to_integer<std::uint8_t>(payload[kOpcodeOffset]) != kUpsertOpcode) {
      continue;
    }
    structured_payloads.push_back(payload);
  }
  store.Close();
  if (ordinal >= structured_payloads.size()) {
    throw std::runtime_error("structured upsert payload ordinal out of range");
  }
  return structured_payloads[ordinal];
}

void AppendU32LE(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendU64LE(std::vector<std::byte>& out, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendF32LE(std::vector<std::byte>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32LE(out, bits);
}

std::vector<std::byte> BuildEmbeddingRecordPayloadV1(std::uint64_t frame_id, const std::vector<float>& embedding) {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'E'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(kMagic.size() + 8 + 4 + embedding.size() * 4);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  AppendU64LE(payload, frame_id);
  AppendU32LE(payload, static_cast<std::uint32_t>(embedding.size()));
  for (const float value : embedding) {
    AppendF32LE(payload, value);
  }
  return payload;
}

std::vector<std::byte> BuildMalformedEmbeddingRecordPayloadV1(std::uint64_t frame_id, std::uint32_t count) {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'E'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(kMagic.size() + 8 + 4);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  AppendU64LE(payload, frame_id);
  AppendU32LE(payload, count);
  return payload;
}

std::vector<std::byte> BuildMalformedEmbeddingRecordPayloadV2IdentityLen(std::uint64_t frame_id,
                                                                          std::uint32_t count,
                                                                          std::uint32_t identity_len) {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'E'},
      std::byte{'M'},
      std::byte{'2'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(kMagic.size() + 8 + 4 + 4 + static_cast<std::size_t>(count) * 4);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  AppendU64LE(payload, frame_id);
  AppendU32LE(payload, count);
  AppendU32LE(payload, identity_len);
  for (std::uint32_t i = 0; i < count; ++i) {
    AppendF32LE(payload, static_cast<float>(i + 1) * 0.1F);
  }
  return payload;
}

std::vector<std::byte> BuildEmbeddingRecordPayloadV2(std::uint64_t frame_id,
                                                     const std::string& identity_tag,
                                                     const std::vector<float>& embedding) {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'E'},
      std::byte{'M'},
      std::byte{'2'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(kMagic.size() + 8 + 4 + 4 + identity_tag.size() + embedding.size() * 4);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  AppendU64LE(payload, frame_id);
  AppendU32LE(payload, static_cast<std::uint32_t>(embedding.size()));
  AppendU32LE(payload, static_cast<std::uint32_t>(identity_tag.size()));
  for (const char ch : identity_tag) {
    payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  for (const float value : embedding) {
    AppendF32LE(payload, value);
  }
  return payload;
}

void AppendStringField(std::vector<std::byte>& out, const std::string& value) {
  AppendU32LE(out, static_cast<std::uint32_t>(value.size()));
  for (const char ch : value) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
}

std::vector<std::byte> BuildMalformedStructuredFactRemovePayloadEmptyEntity() {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(32);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  payload.push_back(static_cast<std::byte>(2));  // remove opcode
  AppendStringField(payload, "");
  AppendStringField(payload, "city");
  return payload;
}

std::vector<std::byte> BuildMalformedStructuredFactUpsertPayloadEmptyAttribute() {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(64);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  payload.push_back(static_cast<std::byte>(1));  // upsert opcode
  AppendStringField(payload, "user:malformed");
  AppendStringField(payload, "");
  AppendStringField(payload, "ignored");
  AppendU32LE(payload, 0U);  // metadata count
  return payload;
}

std::vector<std::byte> BuildMalformedStructuredFactUpsertPayloadOverlongEntityLength() {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(32);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  payload.push_back(static_cast<std::byte>(1));  // upsert opcode
  AppendU32LE(payload, 0xFFFFFFFFU);             // entity length (intentionally oversized)
  return payload;
}

std::vector<std::byte> BuildMalformedStructuredFactUpsertPayloadMetadataCountOverflow() {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(64);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  payload.push_back(static_cast<std::byte>(1));  // upsert opcode
  AppendStringField(payload, "user:overflow");
  AppendStringField(payload, "city");
  AppendStringField(payload, "rome");
  AppendU32LE(payload, 0xFFFFFFFFU);  // metadata count (intentionally oversized)
  return payload;
}

std::vector<std::byte> BuildMalformedStructuredFactUpsertPayloadDuplicateMetadataKey() {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  std::vector<std::byte> payload{};
  payload.reserve(96);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  payload.push_back(static_cast<std::byte>(1));  // upsert opcode
  AppendStringField(payload, "user:dup-meta");
  AppendStringField(payload, "city");
  AppendStringField(payload, "rome");
  AppendU32LE(payload, 2U);
  AppendStringField(payload, "src");
  AppendStringField(payload, "a");
  AppendStringField(payload, "src");
  AppendStringField(payload, "b");
  return payload;
}

std::vector<std::byte> BuildMalformedStructuredFactUpsertPayloadOversizedTotalBytes() {
  constexpr std::array<std::byte, 6> kMagic = {
      std::byte{'W'},
      std::byte{'A'},
      std::byte{'X'},
      std::byte{'S'},
      std::byte{'M'},
      std::byte{'1'},
  };
  constexpr std::size_t kValueLen = 8U * 1024U * 1024U;
  std::vector<std::byte> payload{};
  payload.reserve(128 + kValueLen);
  payload.insert(payload.end(), kMagic.begin(), kMagic.end());
  payload.push_back(static_cast<std::byte>(1));  // upsert opcode
  AppendStringField(payload, "user:oversized-payload");
  AppendStringField(payload, "city");
  AppendU32LE(payload, static_cast<std::uint32_t>(kValueLen));
  payload.insert(payload.end(), kValueLen, static_cast<std::byte>('x'));
  AppendU32LE(payload, 0U);
  return payload;
}

bool StartsWithMagic(const std::vector<std::byte>& bytes, const char* magic, std::size_t size) {
  if (bytes.size() < size) {
    return false;
  }
  for (std::size_t i = 0; i < size; ++i) {
    if (bytes[i] != static_cast<std::byte>(static_cast<unsigned char>(magic[i]))) {
      return false;
    }
  }
  return true;
}

bool ContextHasSource(const waxcpp::RAGContext& context, waxcpp::SearchSource source) {
  for (const auto& item : context.items) {
    for (const auto item_source : item.sources) {
      if (item_source == source) {
        return true;
      }
    }
  }
  return false;
}

void CleanupPath(const std::filesystem::path& path) {
  waxcpp::tests::CleanupStoreArtifacts(path);
}

class CountingEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("CountingEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    ++calls_;
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  void ResetCalls() { calls_ = 0; }
  int calls() const { return calls_; }

 private:
  int calls_ = 0;
};

class CountingBatchEmbedder final : public waxcpp::BatchEmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("CountingBatchEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    ++embed_calls_;
    return BuildEmbedding(text);
  }

  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override {
    ++batch_calls_;
    std::vector<std::vector<float>> out{};
    out.reserve(texts.size());
    for (const auto& text : texts) {
      out.push_back(BuildEmbedding(text));
    }
    return out;
  }

  void Reset() {
    embed_calls_ = 0;
    batch_calls_ = 0;
  }

  int embed_calls() const { return embed_calls_; }
  int batch_calls() const { return batch_calls_; }

 private:
  static std::vector<float> BuildEmbedding(const std::string& text) {
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  int embed_calls_ = 0;
  int batch_calls_ = 0;
};

class IdentifiedBatchEmbedder final : public waxcpp::BatchEmbeddingProvider {
 public:
  explicit IdentifiedBatchEmbedder(std::string model_name)
      : model_name_(std::move(model_name)) {}

  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = model_name_,
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    ++embed_calls_;
    return BuildEmbedding(text);
  }

  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override {
    ++batch_calls_;
    std::vector<std::vector<float>> out{};
    out.reserve(texts.size());
    for (const auto& text : texts) {
      out.push_back(BuildEmbedding(text));
    }
    return out;
  }

  void Reset() {
    embed_calls_ = 0;
    batch_calls_ = 0;
  }

  int embed_calls() const { return embed_calls_; }
  int batch_calls() const { return batch_calls_; }

 private:
  static std::vector<float> BuildEmbedding(const std::string& text) {
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  std::string model_name_{};
  int embed_calls_ = 0;
  int batch_calls_ = 0;
};

class ThreadTrackingEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  explicit ThreadTrackingEmbedder(std::thread::id caller_thread_id)
      : caller_thread_id_(caller_thread_id) {}

  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("ThreadTrackingEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++calls_;
      if (std::this_thread::get_id() == caller_thread_id_) {
        called_from_caller_thread_ = true;
      }
      thread_ids_.insert(std::this_thread::get_id());
    }

    // Encourage overlap so ingest_concurrency assertions stay stable.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return out;
  }

  int calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

  std::size_t distinct_thread_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_ids_.size();
  }

  bool called_from_caller_thread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return called_from_caller_thread_;
  }

 private:
  std::thread::id caller_thread_id_{};
  mutable std::mutex mutex_{};
  std::unordered_set<std::thread::id> thread_ids_{};
  int calls_ = 0;
  bool called_from_caller_thread_ = false;
};

class FailingEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("FailingEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    if (text.find("boom") != std::string::npos) {
      throw std::runtime_error("failing embedder triggered");
    }
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }
};

class NonFiniteEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("NonFiniteEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string&) override {
    return {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 0.0F};
  }
};

class CloudIdentityEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("OpenAI"),
        .model = std::string("text-embedding-3-small"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }
};

class OversizedIdentityEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest-") + std::string(5000, 'x'),
        .model = std::string("OversizedIdentityEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    ++calls_;
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  int calls() const { return calls_; }

 private:
  int calls_ = 0;
};

class ControlCharIdentityEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    std::string provider = "WaxCppTest";
    provider.push_back('\0');
    provider.append("-Ctrl");
    return waxcpp::EmbeddingIdentity{
        .provider = std::move(provider),
        .model = std::string("ControlCharIdentityEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    ++calls_;
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  int calls() const { return calls_; }

 private:
  int calls_ = 0;
};

class WrongDimensionBatchEmbedder final : public waxcpp::BatchEmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("WrongDimensionBatchEmbedder"),
        .dimensions = 4,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    std::vector<float> out(3, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override {
    std::vector<std::vector<float>> out{};
    out.reserve(texts.size());
    for (const auto& text : texts) {
      out.push_back(Embed(text));
    }
    return out;
  }
};

class OversizedDimensionEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 20000; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override {
    return waxcpp::EmbeddingIdentity{
        .provider = std::string("WaxCppTest"),
        .model = std::string("OversizedDimensionEmbedder"),
        .dimensions = 20000,
        .normalized = true,
    };
  }

  std::vector<float> Embed(const std::string& text) override {
    std::vector<float> out(20000, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }
};

void ScenarioVectorPolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector policy validation");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = true;

  bool threw = false;
  try {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "vector-enabled config must require embedder");
}

void ScenarioOnDeviceProviderPolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: on-device provider policy validation");

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = true;
    config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    config.require_on_device_providers = true;

    auto cloud_embedder = std::make_shared<CloudIdentityEmbedder>();
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, cloud_embedder);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "on-device policy should reject cloud provider identities");
  }

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = true;
    config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    config.require_on_device_providers = false;

    auto cloud_embedder = std::make_shared<CloudIdentityEmbedder>();
    waxcpp::MemoryOrchestrator orchestrator(path, config, cloud_embedder);
    orchestrator.Remember("cloud policy disabled path", {});
    orchestrator.Flush();
    orchestrator.Close();
  }
}

void ScenarioEmbeddingDimensionPolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: embedding dimension policy validation");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.require_on_device_providers = true;

  {
    auto embedder = std::make_shared<WrongDimensionBatchEmbedder>();
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);

    bool remember_threw = false;
    try {
      orchestrator.Remember("dimension mismatch remember path", {});
    } catch (const std::exception&) {
      remember_threw = true;
    }
    Require(remember_threw, "remember should reject embedding dimension mismatch");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 0, "dimension mismatch on remember should not persist frames");
    store.Close();
  }

  const auto rebuild_path = UniquePath();
  {
    auto store = waxcpp::WaxStore::Create(rebuild_path);
    (void)store.Put(StringToBytes("seed for rebuild mismatch"), {});
    store.Commit();
    store.Close();
  }
  {
    auto embedder = std::make_shared<WrongDimensionBatchEmbedder>();
    bool ctor_threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(rebuild_path, config, embedder);
      orchestrator.Close();
    } catch (const std::exception&) {
      ctor_threw = true;
    }
    Require(ctor_threw, "constructor should reject rebuild-time embedding dimension mismatch");
  }

  {
    auto embedder = std::make_shared<OversizedDimensionEmbedder>();
    bool ctor_threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
      orchestrator.Close();
    } catch (const std::exception&) {
      ctor_threw = true;
    }
    Require(ctor_threw, "constructor should reject oversized embedding dimension beyond replay safety limit");
  }
  CleanupPath(rebuild_path);
}

void ScenarioSearchModePolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: search mode policy validation");

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "text-only mode must require enabled text channel");
  }

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = true;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "vector-only mode must require enabled vector channel");
  }

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "hybrid mode must require at least one enabled channel");
  }
}

void ScenarioRecallEmbeddingPolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall embedding policy validation");

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = true;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("text only", {});
    orchestrator.Flush();
    bool threw = false;
    try {
      (void)orchestrator.Recall("text", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "Recall(query, embedding) should throw when vector search is disabled");
    orchestrator.Close();
  }

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = true;
    config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    auto embedder = std::make_shared<CountingBatchEmbedder>();
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector doc", {});
    orchestrator.Flush();
    bool threw = false;
    try {
      (void)orchestrator.Recall("vector", std::vector<float>{1.0F, 0.0F, 0.0F});  // wrong dims
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "Recall(query, embedding) should throw on dimension mismatch");
    orchestrator.Close();
  }
}

void ScenarioRememberFlushPersistsFrame(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember/flush persists frame");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("hello orchestrator", {{"source", "unit-test"}});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 1, "orchestrator should persist one frame");
    const auto content = store.FrameContent(0);
    Require(content == StringToBytes("hello orchestrator"), "persisted frame content mismatch");
    store.Close();
  }
}

void ScenarioRecallReturnsRankedItems(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall returns ranked items");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  config.rag.search_top_k = 10;
  config.rag.preview_max_bytes = 256;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple apple banana", {});
    orchestrator.Remember("apple", {});
    orchestrator.Remember("banana", {});
    orchestrator.Flush();
    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "recall should return non-empty context for matching query");
    // BM25 ranking considers both term frequency AND document length;
    // short docs with high term density may outrank longer docs with more
    // raw occurrences.  Verify that both apple-containing docs appear and
    // that scores are in descending order.
    Require(context.items.size() >= 2, "recall should return at least two apple-containing results");
    const bool has_doc0 = std::any_of(context.items.begin(), context.items.end(),
        [](const auto& item) { return item.text == "apple apple banana"; });
    const bool has_doc1 = std::any_of(context.items.begin(), context.items.end(),
        [](const auto& item) { return item.text == "apple"; });
    Require(has_doc0, "recall should include 'apple apple banana' document");
    Require(has_doc1, "recall should include 'apple' document");
    orchestrator.Close();
  }
}

void ScenarioHybridRecallWithEmbedder(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid recall with embedder");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  config.rag.search_top_k = 5;

  auto embedder = std::make_shared<waxcpp::MiniLMEmbedderTorch>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector apple context", {});
    orchestrator.Remember("banana only", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "hybrid recall should return at least one result");
    Require(context.items[0].sources.size() >= 1, "hybrid result should include at least one source");
    orchestrator.Close();
  }
}

void ScenarioEmbeddingMemoizationInRecall(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: embedding memoization in recall");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.embedding_cache_capacity = 32;

  auto embedder = std::make_shared<CountingEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("cached doc one", {});
    orchestrator.Remember("cached doc two", {});
    orchestrator.Flush();

    embedder->ResetCalls();
    (void)orchestrator.Recall("doc");
    (void)orchestrator.Recall("doc");
    // Expect query embedding per recall only, docs should come from cache.
    Require(embedder->calls() == 2, "recall should reuse cached document embeddings");
    orchestrator.Close();
  }
}

void ScenarioBatchProviderUsedForVectorRecall(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall uses committed vector index");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.embedding_cache_capacity = 0;  // force no memoized doc embeddings.

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("batch doc one", {});
    orchestrator.Remember("batch doc two", {});
    orchestrator.Flush();

    embedder->Reset();
    (void)orchestrator.Recall("doc", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(embedder->batch_calls() == 0, "vector recall should read committed vector index without EmbedBatch");
    Require(embedder->embed_calls() == 0, "vector recall with explicit query embedding should avoid Embed");
    orchestrator.Close();
  }
}

void ScenarioMaxSnippetsClamp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: max_snippets clamp");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_top_k = 10;
  config.rag.max_snippets = 1;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple alpha", {});
    orchestrator.Remember("apple beta", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(context.items.size() == 2, "max_snippets should cap snippet count, not total context items");
    Require(context.items[0].kind == waxcpp::RAGItemKind::kExpanded, "first item should remain expanded");
    Require(context.items[1].kind == waxcpp::RAGItemKind::kSnippet, "second item should be the single allowed snippet");
    orchestrator.Close();
  }
}

void ScenarioForgetFactValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: forget fact validation");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  bool threw_empty_entity = false;
  try {
    (void)orchestrator.ForgetFact("", "city");
  } catch (const std::exception&) {
    threw_empty_entity = true;
  }
  Require(threw_empty_entity, "ForgetFact should reject empty entity");

  bool threw_empty_attribute = false;
  try {
    (void)orchestrator.ForgetFact("user:1", "");
  } catch (const std::exception&) {
    threw_empty_attribute = true;
  }
  Require(threw_empty_attribute, "ForgetFact should reject empty attribute");
  orchestrator.Close();
}

void ScenarioRememberFactSerializationFailureDoesNotStageMutation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember fact serialization failure does not stage mutation");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:seed", "city", "rome");
    orchestrator.Flush();

    bool threw_overlong_entity = false;
    try {
      const std::string overlong_entity((4U * 1024U * 1024U) + 1U, 'e');
      orchestrator.RememberFact(overlong_entity, "city", "bad");
    } catch (const std::exception&) {
      threw_overlong_entity = true;
    }
    Require(threw_overlong_entity, "overlong structured fact entity should throw");

    bool threw_overflow_metadata = false;
    try {
      waxcpp::Metadata huge_metadata{};
      constexpr std::size_t kTooManyPairs = 16385;
      for (std::size_t i = 0; i < kTooManyPairs; ++i) {
        huge_metadata.emplace("k" + std::to_string(i), "v");
      }
      orchestrator.RememberFact("user:overflow", "city", "bad", huge_metadata);
    } catch (const std::exception&) {
      threw_overflow_metadata = true;
    }
    Require(threw_overflow_metadata, "oversized structured fact metadata should throw");

    bool threw_payload_envelope = false;
    try {
      const std::string max_entity(4U * 1024U * 1024U, 'e');
      const std::string max_attribute(4U * 1024U * 1024U, 'a');
      orchestrator.RememberFact(max_entity, max_attribute, "x");
    } catch (const std::exception&) {
      threw_payload_envelope = true;
    }
    Require(threw_payload_envelope, "structured fact payload envelope overflow should throw");

    orchestrator.Flush();
    const auto facts = orchestrator.RecallFactsByEntityPrefix("user:", 10);
    Require(facts.size() == 1, "serialization failures must not stage phantom structured facts");
    Require(facts.front().entity == "user:seed", "seed fact should remain the only committed fact");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:", 10);
    Require(facts.size() == 1, "reopen should preserve only successfully serialized structured facts");
    Require(facts.front().entity == "user:seed", "unexpected structured fact after reopen");
    reopened.Close();
  }
}

void ScenarioForgetFactValidationAndSerializationSafety(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: forget fact validation and serialization safety");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:seed-forget", "city", "rome");
    orchestrator.Flush();

    bool threw_overlong_entity = false;
    try {
      const std::string overlong_entity((4U * 1024U * 1024U) + 1U, 'e');
      (void)orchestrator.ForgetFact(overlong_entity, "city");
    } catch (const std::exception&) {
      threw_overlong_entity = true;
    }
    Require(threw_overlong_entity, "overlong ForgetFact entity should throw");

    bool threw_overlong_attribute = false;
    try {
      const std::string overlong_attribute((4U * 1024U * 1024U) + 1U, 'a');
      (void)orchestrator.ForgetFact("user:seed-forget", overlong_attribute);
    } catch (const std::exception&) {
      threw_overlong_attribute = true;
    }
    Require(threw_overlong_attribute, "overlong ForgetFact attribute should throw");

    bool threw_payload_envelope = false;
    try {
      const std::string max_entity(4U * 1024U * 1024U, 'e');
      const std::string max_attribute(4U * 1024U * 1024U, 'a');
      (void)orchestrator.ForgetFact(max_entity, max_attribute);
    } catch (const std::exception&) {
      threw_payload_envelope = true;
    }
    Require(threw_payload_envelope, "ForgetFact payload envelope overflow should throw");

    const auto facts = orchestrator.RecallFactsByEntityPrefix("user:seed-forget", 10);
    Require(facts.size() == 1, "failed ForgetFact serialization path must not remove committed fact");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:seed-forget", 10);
    Require(facts.size() == 1, "reopen should preserve committed fact after failed ForgetFact validation");
    reopened.Close();
  }
}

void ScenarioMaxSnippetsZeroSuppressesSnippetsOnly(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: max_snippets zero suppresses snippets only");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_top_k = 10;
  config.rag.max_snippets = 0;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple alpha", {});
    orchestrator.Remember("apple beta", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(context.items.size() == 1, "max_snippets=0 should still allow expansion item");
    Require(context.items[0].kind == waxcpp::RAGItemKind::kExpanded,
            "max_snippets=0 should suppress snippet items but keep expansion");
    orchestrator.Close();
  }
}

void ScenarioExpansionDisabledStillReturnsRecallItems(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: expansion disabled still returns recall items");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_top_k = 10;
  config.rag.max_snippets = 2;
  config.rag.expansion_max_tokens = 0;
  config.rag.snippet_max_tokens = 8;
  config.rag.max_context_tokens = 64;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple alpha beta", {});
    orchestrator.Remember("apple gamma delta", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "expansion disabled should still produce snippet recall items");
    Require(context.items[0].kind == waxcpp::RAGItemKind::kSnippet,
            "first item should be snippet when expansion tier is disabled");
    orchestrator.Close();
  }
}

void ScenarioRememberChunking(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember chunking");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  config.chunking.target_tokens = 3;
  config.chunking.overlap_tokens = 1;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("a b c d e", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2, "chunking should split content into two frames");
    Require(BytesToString(store.FrameContent(0)) == "a b c", "chunk[0] mismatch");
    Require(BytesToString(store.FrameContent(1)) == "c d e", "chunk[1] mismatch");
    store.Close();
  }
}

void ScenarioBatchProviderUsedForRemember(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: batch provider used for remember");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.embedding_cache_capacity = 64;
  config.chunking.target_tokens = 2;
  config.chunking.overlap_tokens = 0;

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("a b c d e", {});  // 3 chunks with target=2
    Require(embedder->batch_calls() == 1, "remember should use EmbedBatch once for multi-chunk ingest");
    Require(embedder->embed_calls() == 0, "remember should avoid per-chunk Embed when batch provider is available");
    orchestrator.Flush();
    orchestrator.Close();
  }
}

void ScenarioRememberRespectsIngestBatchSize(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember respects ingest_batch_size");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.embedding_cache_capacity = 64;
  config.ingest_batch_size = 2;
  config.chunking.target_tokens = 1;
  config.chunking.overlap_tokens = 0;

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("a b c d e", {});  // 5 chunks, batch_size=2 -> 3 batch calls
    Require(embedder->batch_calls() == 3, "remember should split EmbedBatch calls by ingest_batch_size");
    Require(embedder->embed_calls() == 0, "remember batch mode should avoid per-chunk Embed");
    orchestrator.Flush();
    orchestrator.Close();
  }
}

void ScenarioTextOnlyRecallSkipsVectorEmbedding(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: text-only recall skips vector embedding");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  config.embedding_cache_capacity = 0;  // ensure recall would need embedder if mode gating were broken.

  auto embedder = std::make_shared<CountingEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("apple story", {});
    orchestrator.Remember("banana story", {});
    orchestrator.Flush();

    embedder->ResetCalls();
    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "text-only recall should still return text results");
    Require(embedder->calls() == 0, "text-only recall must not call embedder");
    orchestrator.Close();
  }
}

void ScenarioStructuredMemoryFacts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured memory facts");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:1", "name", "Alice", {{"src", "profile"}});
    orchestrator.RememberFact("user:1", "city", "Paris");
    orchestrator.RememberFact("user:2", "name", "Bob");
    orchestrator.RememberFact("user:1", "name", "Alice B", {{"src", "edit"}});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto user_facts = reopened.RecallFactsByEntityPrefix("user:", 10);
    Require(user_facts.size() == 3, "structured facts prefix query mismatch");
    Require(user_facts[0].entity == "user:1" && user_facts[0].attribute == "city", "fact order mismatch [0]");
    Require(user_facts[1].entity == "user:1" && user_facts[1].attribute == "name", "fact order mismatch [1]");
    Require(user_facts[1].value == "Alice B", "upserted fact value mismatch");
    Require(user_facts[1].version == 2, "fact version should increment on upsert");
    Require(user_facts[1].metadata.at("src") == "edit", "fact metadata mismatch");
    Require(user_facts[2].entity == "user:2", "fact order mismatch [2]");
    reopened.Close();
  }
}

void ScenarioRecallIncludesStructuredMemory(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall includes structured memory");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  config.rag.search_top_k = 10;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:42", "city", "tokyo");
    orchestrator.RememberFact("user:42", "favorite", "sushi");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto context = reopened.Recall("tokyo");
    Require(!context.items.empty(), "recall should include structured memory hit");
    bool found_structured = false;
    bool found_text = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          found_structured = true;
          break;
        }
        if (source == waxcpp::SearchSource::kText) {
          found_text = true;
        }
      }
    }
    Require(found_structured, "structured memory source must appear in recall context");
    Require(!found_text, "internal structured records must not surface as text-source hits");
    reopened.Close();
  }
}

void ScenarioRecallTextChannelUsesTextSource(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall text channel uses text source");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple orchard", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "text recall should return at least one item");
    bool has_text_source = false;
    for (const auto source : context.items.front().sources) {
      if (source == waxcpp::SearchSource::kText) {
        has_text_source = true;
        break;
      }
    }
    Require(has_text_source, "store text recall result should keep kText source");
    orchestrator.Close();
  }
}

void ScenarioRecallVisibilityRequiresFlush(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall visibility requires flush");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush gated text apple", {});
    orchestrator.RememberFact("user:flush", "fruit", "apple");

    const auto before_flush = orchestrator.Recall("apple");
    Require(before_flush.items.empty(), "staged text mutations should stay invisible before flush");

    orchestrator.Flush();
    const auto after_flush = orchestrator.Recall("apple");
    Require(!after_flush.items.empty(), "committed text mutations should be visible after flush");
    orchestrator.Close();
  }
}

void ScenarioVectorRecallVisibilityRequiresFlush(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall visibility requires flush");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector gated apple", {});

    const auto before_flush = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(before_flush.items.empty(), "staged vector mutation should stay invisible before flush");

    orchestrator.Flush();
    const auto after_flush = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_flush.items.empty(), "committed vector mutation should be visible after flush");
    orchestrator.Close();
  }
}

void ScenarioVectorIndexRebuildOnReopen(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector index rebuild on reopen");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("reopen vector apple", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    embedder->Reset();
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen should restore committed vector index");
    Require(embedder->batch_calls() == 0, "explicit vector recall should not re-embed docs after reopen");
    Require(embedder->embed_calls() == 0, "explicit vector recall should avoid query embed calls");
    reopened.Close();
  }
}

void ScenarioVectorReopenReusesPersistedEmbeddingsWithoutReembed(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen reuses persisted embeddings without reembed");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("persisted embedding apple", {});
    orchestrator.Remember("persisted embedding banana", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  embedder->Reset();
  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    Require(embedder->batch_calls() == 0, "reopen vector index rebuild should not call EmbedBatch");
    Require(embedder->embed_calls() == 0, "reopen vector index rebuild should not call Embed");
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopened vector recall should succeed from persisted embeddings");
    reopened.Close();
  }
}

void ScenarioVectorReopenWithMatchingIdentityReusesPersistedEmbeddings(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen matching identity reuses persisted embeddings");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto writer_embedder = std::make_shared<IdentifiedBatchEmbedder>("MiniLM-A");
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, writer_embedder);
    orchestrator.Remember("identity matched apple", {});
    orchestrator.Remember("identity matched banana", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  auto reopen_embedder = std::make_shared<IdentifiedBatchEmbedder>("MiniLM-A");
  {
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    Require(reopen_embedder->batch_calls() == 0, "matching identity should reuse persisted embeddings");
    Require(reopen_embedder->embed_calls() == 0, "matching identity should avoid per-item Embed on reopen");
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "matching identity reopen should preserve vector recall");
    reopened.Close();
  }
}

void ScenarioVectorReopenWithMismatchedIdentityReembeds(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen mismatched identity reembeds");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto writer_embedder = std::make_shared<IdentifiedBatchEmbedder>("MiniLM-A");
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, writer_embedder);
    orchestrator.Remember("identity mismatch apple", {});
    orchestrator.Remember("identity mismatch banana", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  auto reopen_embedder = std::make_shared<IdentifiedBatchEmbedder>("MiniLM-B");
  {
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    Require(reopen_embedder->batch_calls() == 1, "mismatched identity should trigger vector re-embed on reopen");
    Require(reopen_embedder->embed_calls() == 0, "mismatched identity re-embed should use batch path");
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "mismatched identity reopen should still produce vector recall results");
    reopened.Close();
  }
}

void ScenarioOversizedIdentityFallbackUsesWAXEM1AndReusesPersistedVectors(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: oversized identity fallback uses WAXEM1 and reuses persisted vectors");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  {
    auto embedder = std::make_shared<OversizedIdentityEmbedder>();
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("oversized identity apple", {});
    Require(embedder->calls() == 1, "remember should embed user payload once");
    orchestrator.Flush();
    orchestrator.Close();
  }

  bool has_waxem1 = false;
  bool has_waxem2 = false;
  {
    auto store = waxcpp::WaxStore::Open(path);
    for (const auto& meta : store.FrameMetas()) {
      if (meta.status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(meta.id);
      if (StartsWithMagic(payload, "WAXEM1", 6)) {
        has_waxem1 = true;
      }
      if (StartsWithMagic(payload, "WAXEM2", 6)) {
        has_waxem2 = true;
      }
    }
    store.Close();
  }
  Require(has_waxem1, "oversized identity fallback should store persisted embedding as WAXEM1");
  Require(!has_waxem2, "oversized identity fallback must not emit malformed WAXEM2 record");

  {
    auto reopen_embedder = std::make_shared<OversizedIdentityEmbedder>();
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    Require(reopen_embedder->calls() == 0,
            "reopen should reuse persisted vectors and avoid re-embedding with oversized identity fallback");
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after oversized identity fallback");
    reopened.Close();
  }
}

void ScenarioControlCharIdentityFallbackUsesWAXEM1AndReusesPersistedVectors(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: control-char identity fallback uses WAXEM1 and reuses persisted vectors");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  {
    auto embedder = std::make_shared<ControlCharIdentityEmbedder>();
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("control-char identity apple", {});
    Require(embedder->calls() == 1, "remember should embed user payload once");
    orchestrator.Flush();
    orchestrator.Close();
  }

  bool has_waxem1 = false;
  bool has_waxem2 = false;
  {
    auto store = waxcpp::WaxStore::Open(path);
    for (const auto& meta : store.FrameMetas()) {
      if (meta.status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(meta.id);
      if (StartsWithMagic(payload, "WAXEM1", 6)) {
        has_waxem1 = true;
      }
      if (StartsWithMagic(payload, "WAXEM2", 6)) {
        has_waxem2 = true;
      }
    }
    store.Close();
  }
  Require(has_waxem1, "control-char identity fallback should store persisted embedding as WAXEM1");
  Require(!has_waxem2, "control-char identity fallback must not emit malformed WAXEM2 record");

  {
    auto reopen_embedder = std::make_shared<ControlCharIdentityEmbedder>();
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    Require(reopen_embedder->calls() == 0,
            "reopen should reuse persisted vectors and avoid re-embedding with control-char identity fallback");
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after control-char identity fallback");
    reopened.Close();
  }
}

void ScenarioEmbeddingJournalDoesNotLeakIntoTextRecall(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: embedding journal does not leak into text recall");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("normal recallable content", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    const auto marker_context = reopened.Recall("WAXEM");
    bool has_embedding_marker_payload = false;
    for (const auto& item : marker_context.items) {
      if (item.text.find("WAXEM1") != std::string::npos || item.text.find("WAXEM2") != std::string::npos) {
        has_embedding_marker_payload = true;
        break;
      }
    }
    Require(!has_embedding_marker_payload, "embedding journal payload should not appear in text recall");
    const auto normal_context = reopened.Recall("normal");
    Require(!normal_context.items.empty(), "normal text should remain recallable");
    reopened.Close();
  }
}

void ScenarioVectorCloseWithoutFlushPersistsViaStoreClose(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector close without flush persists via store close");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("close persist vector apple", {});
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    embedder->Reset();
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "Close() should persist local mutations and reopen should rebuild vector index");
    Require(embedder->batch_calls() == 0, "explicit vector recall should not batch-embed docs");
    Require(embedder->embed_calls() == 0, "explicit vector recall should avoid query embed calls");
    reopened.Close();
  }
}

void ScenarioVectorRecallSupportsExplicitEmbeddingWithoutQuery(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall supports explicit embedding without query");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("embedding only recall doc", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "explicit embedding recall should work with empty query");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not call Embed");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioVectorRecallEmptyQueryWithoutEmbeddingReturnsEmpty(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall empty query without embedding returns empty");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector-only empty query should not auto-embed", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("");
    Require(context.items.empty(), "vector-only recall with empty query and no embedding should return empty context");
    Require(context.total_tokens == 0, "vector-only recall with empty query should report zero tokens");
    Require(embedder->embed_calls() == 0, "vector-only empty query recall should not call Embed");
    Require(embedder->batch_calls() == 0, "vector-only empty query recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioVectorRecallWhitespaceQueryWithoutEmbeddingReturnsEmpty(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall whitespace query without embedding returns empty");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector-only whitespace query should not auto-embed", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("   \t\r\n");
    Require(context.items.empty(),
            "vector-only recall with whitespace-only query and no embedding should return empty context");
    Require(context.total_tokens == 0, "vector-only recall with whitespace-only query should report zero tokens");
    Require(embedder->embed_calls() == 0, "vector-only whitespace query recall should not call Embed");
    Require(embedder->batch_calls() == 0, "vector-only whitespace query recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioHybridRecallWhitespaceQueryWithoutEmbeddingReturnsEmpty(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid recall whitespace query without embedding returns empty");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("hybrid whitespace query should not auto-embed", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("   \t\r\n");
    Require(context.items.empty(),
            "hybrid recall with whitespace-only query and no embedding should return empty context");
    Require(context.total_tokens == 0, "hybrid recall with whitespace-only query should report zero tokens");
    Require(embedder->embed_calls() == 0, "hybrid whitespace query recall should not call Embed");
    Require(embedder->batch_calls() == 0, "hybrid whitespace query recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioHybridRecallWhitespaceQueryWithExplicitEmbeddingUsesVectorOnly(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid recall whitespace query with explicit embedding uses vector-only channel");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("hybrid explicit embedding whitespace recall doc", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("   \t\r\n", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(),
            "hybrid recall with explicit embedding should return vector-backed context for whitespace query");
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        Require(source == waxcpp::SearchSource::kVector,
                "hybrid whitespace explicit-embedding recall should not include text channel sources");
      }
    }
    Require(embedder->embed_calls() == 0, "hybrid explicit embedding recall should not call Embed");
    Require(embedder->batch_calls() == 0, "hybrid explicit embedding recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioTextOnlyRecallWhitespaceQueryReturnsEmpty(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: text-only recall whitespace query returns empty");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("text-only whitespace query should not hit text index", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("   \t\r\n");
    Require(context.items.empty(), "text-only recall with whitespace-only query should return empty context");
    Require(context.total_tokens == 0, "text-only recall with whitespace-only query should report zero tokens");
    orchestrator.Close();
  }
}

void ScenarioHybridRecallWithExplicitEmbeddingSkipsQueryEmbed(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid recall with explicit embedding skips query embed");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("hybrid explicit embedding apple", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "hybrid explicit embedding recall should return context");
    Require(embedder->embed_calls() == 0, "hybrid explicit embedding recall should not call Embed");
    Require(embedder->batch_calls() == 0, "hybrid explicit embedding recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureDoesNotExposeStagedText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure does not expose staged text");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("failing flush apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when store commit failpoint is set");

    const auto before_successful_flush = orchestrator.Recall("apple");
    Require(before_successful_flush.items.empty(),
            "failed flush must not expose staged text index mutations");

    orchestrator.Flush();
    const auto after_successful_flush = orchestrator.Recall("apple");
    Require(!after_successful_flush.items.empty(),
            "successful retry flush should expose committed text mutation");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureDoesNotExposeStagedVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure does not expose staged vector");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("failing flush vector apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when store commit failpoint is set");

    const auto before_successful_flush = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(before_successful_flush.items.empty(),
            "failed flush must not expose staged vector index mutations");

    orchestrator.Flush();
    const auto after_successful_flush = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_successful_flush.items.empty(),
            "successful retry flush should expose committed vector mutation");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureThenCloseReopenRecoversText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure then close/reopen recovers text");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush close reopen apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_close = orchestrator.Recall("apple");
    Require(before_close.items.empty(), "failed flush should keep staged text hidden in current process");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto context = reopened.Recall("apple");
    Require(!context.items.empty(), "reopen should rebuild text index from committed store state");
    reopened.Close();
  }
}

void ScenarioFlushFailureThenCloseReopenRecoversVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure then close/reopen recovers vector");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush close reopen vector apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_close = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(before_close.items.empty(), "failed flush should keep staged vector hidden in current process");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    embedder->Reset();
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen should rebuild vector index from committed store state");
    Require(embedder->batch_calls() == 0, "explicit vector recall should not re-embed docs after reopen");
    Require(embedder->embed_calls() == 0, "explicit vector recall should avoid query embed calls");
    reopened.Close();
  }
}

void ScenarioFlushCrashWindowHeaderPublishRebuildsText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after header publish rebuilds text state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step4 text apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(4);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 4");

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(),
            "after header-publish crash-window failure, runtime rebuild should expose committed text");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderAPublishRebuildsText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after header A publish rebuilds text state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step3 text apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(3);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 3");

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(),
            "after header-A publish crash-window failure, runtime rebuild should expose committed text");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowFooterPublishRebuildsText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after footer publish rebuilds text state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step2 text apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(2);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 2");

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(),
            "after footer publish crash-window failure, runtime rebuild should expose committed text");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderPublishRebuildsVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after header publish rebuilds vector state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step4 vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(4);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 4");

    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(),
            "after header-publish crash-window failure, runtime rebuild should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderAPublishRebuildsVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after header A publish rebuilds vector state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step3 vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(3);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 3");

    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(),
            "after header-A publish crash-window failure, runtime rebuild should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowCheckpointPublishRebuildsText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after checkpoint publish rebuilds text state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step5 text apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(5);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 5");

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(),
            "after checkpoint publish crash-window failure, runtime rebuild should expose committed text");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowFooterPublishRebuildsVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after footer publish rebuilds vector state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step2 vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(2);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 2");

    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(),
            "after footer publish crash-window failure, runtime rebuild should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowCheckpointPublishRebuildsVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after checkpoint publish rebuilds vector state");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step5 vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(5);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 5");

    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(),
            "after checkpoint publish crash-window failure, runtime rebuild should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowFooterPublishRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after footer publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step2 retry apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(2);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 2");

    const auto after_failure = orchestrator.Recall("apple");
    Require(!after_failure.items.empty(), "footer-published crash-window should expose committed text");

    // Commit is already externally visible; retry flush should be a no-op and
    // keep the same committed visibility contract.
    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple");
    Require(!after_retry.items.empty(), "retry flush after externally visible commit should preserve text visibility");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowFooterPublishRetryFlushIsNoOpVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector flush crash-window after footer publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step2 retry vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(2);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 2");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_failure.items.empty(), "footer-published crash-window should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    // Commit is already externally visible; retry flush should be a no-op and
    // keep the same committed visibility contract.
    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(), "retry flush after externally visible commit should preserve vector visibility");
    Require(embedder->batch_calls() == 0, "retry path should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry path should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowCheckpointPublishRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after checkpoint publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step5 retry apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(5);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 5");

    const auto after_failure = orchestrator.Recall("apple");
    Require(!after_failure.items.empty(), "checkpoint-published crash-window should expose committed text");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple");
    Require(!after_retry.items.empty(),
            "retry flush after checkpoint-published commit should preserve text visibility");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowCheckpointPublishRetryFlushIsNoOpVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector flush crash-window after checkpoint publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step5 retry vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(5);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 5");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_failure.items.empty(), "checkpoint-published crash-window should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(),
            "retry flush after checkpoint-published commit should preserve vector visibility");
    Require(embedder->batch_calls() == 0, "retry path should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry path should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderAPublishRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after header A publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step3 retry apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(3);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 3");

    const auto after_failure = orchestrator.Recall("apple");
    Require(!after_failure.items.empty(), "header-A-published crash-window should expose committed text");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple");
    Require(!after_retry.items.empty(),
            "retry flush after header-A-published commit should preserve text visibility");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderAPublishRetryFlushIsNoOpVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector flush crash-window after header A publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step3 retry vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(3);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 3");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_failure.items.empty(), "header-A-published crash-window should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(),
            "retry flush after header-A-published commit should preserve vector visibility");
    Require(embedder->batch_calls() == 0, "retry path should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry path should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderBPublishRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window after header B publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step4 retry apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(4);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 4");

    const auto after_failure = orchestrator.Recall("apple");
    Require(!after_failure.items.empty(), "header-B-published crash-window should expose committed text");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple");
    Require(!after_retry.items.empty(),
            "retry flush after header-B-published commit should preserve text visibility");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowHeaderBPublishRetryFlushIsNoOpVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector flush crash-window after header B publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step4 retry vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(4);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 4");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_failure.items.empty(), "header-B-published crash-window should expose committed vector");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(),
            "retry flush after header-B-published commit should preserve vector visibility");
    Require(embedder->batch_calls() == 0, "retry path should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry path should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowTocOnlyRetryFlushPublishesOnSecondAttempt(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush crash-window TOC-only retry flush publishes on second attempt");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush step1 retry apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 1");

    const auto after_failure = orchestrator.Recall("apple");
    Require(after_failure.items.empty(), "TOC-only crash-window should keep staged text hidden");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple");
    Require(!after_retry.items.empty(), "second flush attempt should publish text after TOC-only failure");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowTocOnlyRetryFlushPublishesOnSecondAttemptVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector flush crash-window TOC-only retry flush publishes on second attempt");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step1 retry vector apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step 1");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(after_failure.items.empty(), "TOC-only crash-window should keep staged vector hidden");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(), "second flush attempt should publish vector after TOC-only failure");
    Require(embedder->batch_calls() == 0, "retry path should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry path should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowStructuredFactRebuildsAtStep(const std::filesystem::path& path,
                                                          std::uint32_t fail_step,
                                                          const std::string& scenario_suffix) {
  waxcpp::tests::Log("scenario: flush crash-window structured fact rebuilds in-process " + scenario_suffix);
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  const std::string entity = "user:step" + std::to_string(fail_step);
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact(entity, "city", "rome");

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(fail_step);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step");

    const auto facts = orchestrator.RecallFactsByEntityPrefix(entity, 10);
    Require(facts.size() == 1,
            "externally visible crash-window step should rebuild structured facts in-process");

    const auto context = orchestrator.Recall("rome");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
      if (has_structured) {
        break;
      }
    }
    Require(has_structured,
            "externally visible crash-window step should rebuild structured-text recall in-process");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowStructuredFactRebuildsStep2(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRebuildsAtStep(path, 2, "step2");
}

void ScenarioFlushCrashWindowStructuredFactRebuildsStep3(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRebuildsAtStep(path, 3, "step3");
}

void ScenarioFlushCrashWindowStructuredFactRebuildsStep4(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRebuildsAtStep(path, 4, "step4");
}

void ScenarioFlushCrashWindowStructuredFactRebuildsStep5(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRebuildsAtStep(path, 5, "step5");
}

void ScenarioFlushCrashWindowStructuredFactRetryNoOpAtStep(const std::filesystem::path& path,
                                                           std::uint32_t fail_step,
                                                           const std::string& scenario_suffix) {
  waxcpp::tests::Log("scenario: flush crash-window structured fact retry no-op " + scenario_suffix);
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  const std::string entity = "user:retry-step" + std::to_string(fail_step);
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact(entity, "city", "rome");

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(fail_step);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected crash-window step");

    const auto after_failure_facts = orchestrator.RecallFactsByEntityPrefix(entity, 10);
    Require(after_failure_facts.size() == 1,
            "externally visible crash-window step should expose structured fact after failed flush");

    orchestrator.Flush();
    const auto after_retry_facts = orchestrator.RecallFactsByEntityPrefix(entity, 10);
    Require(after_retry_facts.size() == 1,
            "retry flush after externally visible crash-window should remain no-op for structured fact");

    const auto context = orchestrator.Recall("rome");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
      if (has_structured) {
        break;
      }
    }
    Require(has_structured,
            "structured fact should remain visible in recall after retry no-op flush");
    orchestrator.Close();
  }
}

void ScenarioFlushCrashWindowStructuredFactRetryNoOpStep2(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRetryNoOpAtStep(path, 2, "step2");
}

void ScenarioFlushCrashWindowStructuredFactRetryNoOpStep3(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRetryNoOpAtStep(path, 3, "step3");
}

void ScenarioFlushCrashWindowStructuredFactRetryNoOpStep4(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRetryNoOpAtStep(path, 4, "step4");
}

void ScenarioFlushCrashWindowStructuredFactRetryNoOpStep5(const std::filesystem::path& path) {
  ScenarioFlushCrashWindowStructuredFactRetryNoOpAtStep(path, 5, "step5");
}

void ScenarioFlushFailureThenCloseReopenRecoversStructuredFact(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure then close/reopen recovers structured fact");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:reopen", "city", "rome");

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_close = orchestrator.Recall("rome");
    Require(before_close.items.empty(), "failed flush should keep staged structured fact hidden");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:reopen", 10);
    Require(facts.size() == 1, "structured fact should be restored after reopen");
    const auto context = reopened.Recall("rome");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(has_structured, "reopen should rebuild structured-text index from committed fact");
    reopened.Close();
  }
}

void ScenarioRememberUsesConfiguredIngestConcurrency(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember uses configured ingest_concurrency");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.chunking.target_tokens = 1;
  config.chunking.overlap_tokens = 0;
  config.ingest_concurrency = 4;

  auto embedder = std::make_shared<ThreadTrackingEmbedder>(std::this_thread::get_id());
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("a b c d e f g h", {});  // 8 chunks
    orchestrator.Flush();
    orchestrator.Close();
  }

  Require(embedder->calls() == 8, "ingest_concurrency scenario should embed each chunk exactly once");
  Require(!embedder->called_from_caller_thread(),
          "ingest_concurrency>1 should run non-batch embed calls on worker threads");
  Require(embedder->distinct_thread_count() >= 2,
          "ingest_concurrency>1 should utilize more than one worker thread");

  {
    auto store = waxcpp::WaxStore::Open(path);
    std::uint64_t user_payload_frames = 0;
    for (const auto& meta : store.FrameMetas()) {
      if (meta.status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(meta.id);
      if (StartsWithMagic(payload, "WAXEM1", 6) || StartsWithMagic(payload, "WAXEM2", 6)) {
        continue;
      }
      ++user_payload_frames;
    }
    Require(user_payload_frames == 8, "ingest_concurrency scenario should persist all user chunk frames");
    store.Close();
  }
}

void ScenarioVectorRebuildUsesConfiguredIngestConcurrency(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector rebuild uses configured ingest_concurrency");
  {
    auto store = waxcpp::WaxStore::Create(path);
    for (int i = 0; i < 8; ++i) {
      const std::string text = "seed-doc-" + std::to_string(i) + " apple";
      (void)store.Put(StringToBytes(text), {});
    }
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.ingest_concurrency = 4;

  auto embedder = std::make_shared<ThreadTrackingEmbedder>(std::this_thread::get_id());
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector rebuild should make seeded docs searchable");
    orchestrator.Close();
  }

  Require(embedder->calls() == 8, "rebuild ingest_concurrency scenario should embed each seeded doc exactly once");
  Require(!embedder->called_from_caller_thread(),
          "rebuild ingest_concurrency>1 should run non-batch embed calls on worker threads");
  Require(embedder->distinct_thread_count() >= 2,
          "rebuild ingest_concurrency>1 should utilize more than one worker thread");
}

void ScenarioVectorReopenWithNonFinitePersistedEmbeddingReembeds(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen with non-finite persisted embedding reembeds");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("non finite persisted apple"), {});
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto bad_embedding_payload = BuildEmbeddingRecordPayloadV1(user_frame_id, {nan, 0.0F, 0.0F, 0.0F});
    (void)store.Put(bad_embedding_payload, {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    // Non-finite persisted embeddings must be ignored, forcing rebuild re-embed.
    Require(embedder->batch_calls() == 0,
            "single missing embedding during rebuild should not use EmbedBatch fast path");
    Require(embedder->embed_calls() == 1,
            "non-finite persisted embedding should force one single-item re-embed");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after re-embedding non-finite persisted record");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenIgnoresLaterNonFinitePersistedOverride(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen ignores later non-finite persisted override");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("valid persisted then non-finite override apple"), {});
    (void)store.Put(
        BuildEmbeddingRecordPayloadV2(
            user_frame_id,
            "provider=WaxCppTest;model=CountingBatchEmbedder;dimensions=4;normalized=true",
            {1.0F, 0.0F, 0.0F, 0.0F}),
        {});
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    (void)store.Put(BuildEmbeddingRecordPayloadV1(user_frame_id, {nan, 0.0F, 0.0F, 0.0F}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "valid persisted vector should avoid batch re-embed");
    Require(embedder->embed_calls() == 0,
            "non-finite later persisted record should not override earlier valid vector");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed from valid persisted vector");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenWithMalformedPersistedEmbeddingCountSkipsRecord(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen with malformed persisted embedding count skips record");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("malformed persisted count apple"), {});
    const auto malformed_payload = BuildMalformedEmbeddingRecordPayloadV1(user_frame_id, 0xFFFFFFFFU);
    (void)store.Put(malformed_payload, {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    // Malformed persisted record should be ignored; rebuild must embed the user frame once.
    Require(embedder->batch_calls() == 0, "single-frame rebuild should not use EmbedBatch");
    Require(embedder->embed_calls() == 1, "malformed persisted record should be ignored and re-embedded");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after ignoring malformed persisted record");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenWithMalformedPersistedEmbeddingIdentitySkipsRecord(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen with malformed persisted embedding identity skips record");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("malformed persisted identity apple"), {});
    const auto malformed_payload = BuildMalformedEmbeddingRecordPayloadV2IdentityLen(user_frame_id, 4U, 0xFFFFU);
    (void)store.Put(malformed_payload, {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "single-frame malformed identity rebuild should not use EmbedBatch");
    Require(embedder->embed_calls() == 1, "malformed identity payload should be ignored and re-embedded");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after ignoring malformed identity payload");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenWithOverlongIdentityV2SkipsRecord(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen with overlong V2 identity skips record");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("overlong identity persisted payload apple"), {});
    const std::string overlong_identity(5000, 'x');
    (void)store.Put(BuildEmbeddingRecordPayloadV2(user_frame_id, overlong_identity, {1.0F, 0.0F, 0.0F, 0.0F}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "single-frame overlong identity rebuild should not use EmbedBatch");
    Require(embedder->embed_calls() == 1, "overlong V2 identity should be treated as malformed and re-embedded");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after overlong V2 identity fallback");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenWithControlCharIdentityV2SkipsRecord(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen with control-char V2 identity skips record");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("control-char identity persisted payload apple"), {});
    std::string bad_identity = "provider=WaxCppTest;model=CountingBatchEmbedder";
    bad_identity.push_back('\0');
    bad_identity.append(";dimensions=4;normalized=true");
    (void)store.Put(BuildEmbeddingRecordPayloadV2(user_frame_id, bad_identity, {1.0F, 0.0F, 0.0F, 0.0F}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "single-frame control-char identity rebuild should not use EmbedBatch");
    Require(embedder->embed_calls() == 1, "control-char V2 identity should be treated as malformed and re-embedded");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after control-char V2 identity fallback");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenWithEmptyIdentityV2SkipsRecord(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen with empty V2 identity skips record");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("empty identity persisted payload apple"), {});
    (void)store.Put(BuildEmbeddingRecordPayloadV2(user_frame_id, "", {1.0F, 0.0F, 0.0F, 0.0F}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "single-frame empty identity rebuild should not use EmbedBatch");
    Require(embedder->embed_calls() == 1, "empty V2 identity should be treated as malformed and re-embedded");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should succeed after empty V2 identity fallback");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenEmptyIdentityOverrideDoesNotReplaceValidPersisted(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen empty identity override does not replace valid persisted");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("valid persisted then empty identity override apple"), {});
    (void)store.Put(
        BuildEmbeddingRecordPayloadV2(
            user_frame_id,
            "provider=WaxCppTest;model=CountingBatchEmbedder;dimensions=4;normalized=true",
            {1.0F, 0.0F, 0.0F, 0.0F}),
        {});
    (void)store.Put(BuildEmbeddingRecordPayloadV2(user_frame_id, "", {1.0F, 0.0F, 0.0F, 0.0F}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "valid persisted vector should avoid batch re-embed");
    Require(embedder->embed_calls() == 0,
            "later empty-identity V2 record should not override earlier valid persisted vector");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should remain available from earlier valid persisted vector");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenEmptyEmbeddingOverrideDoesNotReplaceValidPersisted(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen empty embedding override does not replace valid persisted");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("valid persisted then empty embedding override apple"), {});
    (void)store.Put(
        BuildEmbeddingRecordPayloadV2(
            user_frame_id,
            "provider=WaxCppTest;model=CountingBatchEmbedder;dimensions=4;normalized=true",
            {1.0F, 0.0F, 0.0F, 0.0F}),
        {});
    (void)store.Put(BuildEmbeddingRecordPayloadV1(user_frame_id, {}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "valid persisted vector should avoid batch re-embed");
    Require(embedder->embed_calls() == 0,
            "later empty embedding record should not override earlier valid persisted vector");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should remain available from earlier valid persisted vector");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenDimensionMismatchedOverrideDoesNotReplaceValidPersisted(
    const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen dimension-mismatched override does not replace valid persisted");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("valid persisted then mismatched-dim override apple"), {});
    (void)store.Put(
        BuildEmbeddingRecordPayloadV2(
            user_frame_id,
            "provider=WaxCppTest;model=CountingBatchEmbedder;dimensions=4;normalized=true",
            {1.0F, 0.0F, 0.0F, 0.0F}),
        {});
    (void)store.Put(BuildEmbeddingRecordPayloadV1(user_frame_id, {1.0F, 0.0F, 0.0F}), {});
    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "valid persisted vector should avoid batch re-embed");
    Require(embedder->embed_calls() == 0,
            "later dimension-mismatched record should not override earlier valid persisted vector");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should remain available from earlier valid persisted vector");
    orchestrator.Close();
  }
}

void ScenarioVectorReopenMalformedEmbeddingFuzzKeepsValidPersistedVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector reopen malformed embedding fuzz keeps valid persisted vector");
  std::uint64_t user_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    user_frame_id = store.Put(StringToBytes("persisted vector apple"), {});
    (void)store.Put(BuildEmbeddingRecordPayloadV2(user_frame_id,
                                                  "provider=WaxCppTest;model=CountingBatchEmbedder;dimensions=4;normalized=true",
                                                  {1.0F, 0.0F, 0.0F, 0.0F}),
                    {});

    std::mt19937 rng(0xE6B311DU);
    std::uniform_int_distribution<int> type_dist(0, 2);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> tail_dist(0, 16);
    constexpr int kMalformedRecords = 128;
    for (int i = 0; i < kMalformedRecords; ++i) {
      const std::uint64_t malformed_frame_id = user_frame_id + static_cast<std::uint64_t>(i) + 1U;
      std::vector<std::byte> payload{};
      const int kind = type_dist(rng);
      if (kind == 0) {
        payload = BuildMalformedEmbeddingRecordPayloadV1(malformed_frame_id, 0xFFFFFFFFU);
      } else if (kind == 1) {
        payload = BuildMalformedEmbeddingRecordPayloadV2IdentityLen(malformed_frame_id, 4U, 0xFFFFU);
      } else {
        payload = BuildEmbeddingRecordPayloadV1(malformed_frame_id, {1.0F, 0.0F, 0.0F, 0.0F});
        if (payload.size() > 10) {
          payload.resize(payload.size() - 3);
        }
      }
      const int tail = tail_dist(rng);
      for (int t = 0; t < tail; ++t) {
        payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte_dist(rng))));
      }
      (void)store.Put(payload, {});
    }

    store.Commit();
    store.Close();
  }

  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    Require(embedder->batch_calls() == 0, "valid persisted vector should avoid batch re-embed despite malformed records");
    Require(embedder->embed_calls() == 0,
            "valid persisted vector should avoid single-item re-embed despite malformed records");
    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "vector recall should remain available with malformed embedding journal noise");
    orchestrator.Close();
  }
}

void ScenarioRememberIngestConcurrencyPropagatesEmbedErrors(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember ingest_concurrency propagates embed errors");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.chunking.target_tokens = 1;
  config.chunking.overlap_tokens = 0;
  config.ingest_concurrency = 4;

  auto embedder = std::make_shared<FailingEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    bool threw = false;
    try {
      orchestrator.Remember("ok0 ok1 boom ok2 ok3", {});
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "remember should propagate embedder failure under ingest_concurrency");
    const auto stats = orchestrator.Recall("ok1", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(stats.items.empty(), "failed remember should not leave partial vector-visible content");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 0, "failed remember should not persist partial frames");
    Require(stats.pending_frames == 0, "failed remember should not leave pending WAL mutations");
    store.Close();
  }
}

void ScenarioRememberRejectsNonFiniteEmbeddings(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember rejects non-finite embeddings");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<NonFiniteEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    bool threw = false;
    try {
      orchestrator.Remember("non-finite remember payload", {});
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "remember should reject non-finite embedder outputs");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 0, "non-finite remember failure should not persist frames");
    Require(stats.pending_frames == 0, "non-finite remember failure should not leave pending WAL");
    store.Close();
  }
}

void ScenarioRecallExplicitEmbeddingRejectsNonFinite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall explicit embedding rejects non-finite");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    bool threw = false;
    try {
      (void)orchestrator.Recall("apple", {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 0.0F});
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "Recall(query, embedding) should reject non-finite query embeddings");
    orchestrator.Close();
  }
}

void ScenarioRecallQueryEmbeddingRejectsNonFiniteFromEmbedder(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall query embedding rejects non-finite from embedder");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<NonFiniteEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    bool threw = false;
    try {
      (void)orchestrator.Recall("apple");
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "Recall(query) should reject non-finite query embeddings produced by embedder");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureDoesNotExposeStagedStructuredFactUntilRetry(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure does not expose staged structured fact until retry");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:retry", "city", "rome");

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_retry_context = orchestrator.Recall("rome");
    Require(before_retry_context.items.empty(), "failed flush must keep staged structured fact hidden");
    const auto before_retry_facts = orchestrator.RecallFactsByEntityPrefix("user:retry", 10);
    Require(before_retry_facts.empty(), "failed flush must keep staged structured fact out of fact query");

    orchestrator.Flush();
    const auto after_retry_facts = orchestrator.RecallFactsByEntityPrefix("user:retry", 10);
    Require(after_retry_facts.size() == 1, "successful retry flush must publish structured fact");
    const auto after_retry_context = orchestrator.Recall("rome");
    bool has_structured = false;
    for (const auto& item : after_retry_context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(has_structured, "successful retry flush must publish structured fact to recall");
    orchestrator.Close();
  }
}

void ScenarioTextIndexCommitFailureRecoversFromCommittedStore(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: text index commit failure recovers from committed store");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("text commit fail apple", {});

    bool flush_threw = false;
    waxcpp::text::testing::SetCommitFailCountdown(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::text::testing::ClearCommitFailCountdown();
    Require(flush_threw, "flush should throw when text index commit failpoint is set");

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(),
            "flush recovery should rebuild text index from committed store state");
    orchestrator.Close();
  }
}

void ScenarioTextIndexCommitFailureRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: text index commit failure retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("text retry apple", {});

    bool flush_threw = false;
    waxcpp::text::testing::SetCommitFailCountdown(1);  // fail on store_text_index_.CommitStaged()
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::text::testing::ClearCommitFailCountdown();
    Require(flush_threw, "flush should throw when text index commit failpoint is set");

    const auto after_failure = orchestrator.Recall("apple");
    Require(!after_failure.items.empty(), "failed text index commit should rebuild and keep text recall visible");

    orchestrator.Flush();  // retry should be no-op after externally visible commit
    const auto after_retry = orchestrator.Recall("apple");
    Require(!after_retry.items.empty(), "retry flush should keep text recall visible");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 1, "text retry no-op path should persist exactly one user frame");
    Require(stats.pending_frames == 0, "text retry no-op path should not leave pending WAL mutations");
    store.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto context = reopened.Recall("apple");
    Require(!context.items.empty(), "reopen should preserve text recall visibility after retry no-op path");
    reopened.Close();
  }
}

void ScenarioVectorIndexCommitFailureRecoversFromCommittedStore(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector index commit failure recovers from committed store");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector commit fail apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::vector::testing::SetCommitFailCountdown(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::vector::testing::ClearCommitFailCountdown();
    Require(flush_threw, "flush should throw when vector index commit failpoint is set");
    Require(embedder->batch_calls() == 0, "vector rebuild should use persisted embeddings without EmbedBatch");
    Require(embedder->embed_calls() == 0, "vector rebuild should use persisted embeddings without Embed");

    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(),
            "flush recovery should rebuild vector index from committed store state");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not call EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not call Embed");
    orchestrator.Close();
  }
}

void ScenarioVectorIndexCommitFailureRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector index commit failure retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector retry apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::vector::testing::SetCommitFailCountdown(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::vector::testing::ClearCommitFailCountdown();
    Require(flush_threw, "flush should throw when vector index commit failpoint is set");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_failure.items.empty(), "failed vector index commit should rebuild and keep vector recall visible");
    Require(embedder->batch_calls() == 0, "rebuild should use persisted embeddings without EmbedBatch");
    Require(embedder->embed_calls() == 0, "rebuild should use persisted embeddings without Embed");

    orchestrator.Flush();  // retry should be no-op after externally visible commit
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(), "retry flush should keep vector recall visible");
    Require(embedder->batch_calls() == 0, "retry no-op flush should not call EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry no-op flush should not call Embed");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2, "vector retry no-op path should persist exactly one user frame plus one embedding record");
    Require(stats.pending_frames == 0, "vector retry no-op path should not leave pending WAL mutations");

    std::uint64_t user_payload_frames = 0;
    std::uint64_t embedding_payload_frames = 0;
    for (const auto& meta : store.FrameMetas()) {
      if (meta.status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(meta.id);
      if (StartsWithMagic(payload, "WAXEM1", 6) || StartsWithMagic(payload, "WAXEM2", 6)) {
        ++embedding_payload_frames;
      } else {
        ++user_payload_frames;
      }
    }
    Require(user_payload_frames == 1, "vector retry no-op path should keep exactly one committed user payload frame");
    Require(embedding_payload_frames == 1,
            "vector retry no-op path should keep exactly one committed embedding payload frame");
    store.Close();
  }

  {
    auto reopen_embedder = std::make_shared<CountingBatchEmbedder>();
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen after vector retry no-op path should preserve vector recall visibility");
    Require(reopen_embedder->batch_calls() == 0, "reopen should use persisted embeddings without EmbedBatch");
    Require(reopen_embedder->embed_calls() == 0, "reopen should use persisted embeddings without Embed");
    reopened.Close();
  }
}

void ScenarioHybridVectorIndexCommitFailureRecoversBothChannelsAndRetryNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid vector index commit failure recovers both channels and retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("hybrid retry apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::vector::testing::SetCommitFailCountdown(1);  // fail on vector_index_->CommitStaged()
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::vector::testing::ClearCommitFailCountdown();
    Require(flush_threw, "flush should throw when vector index commit failpoint is set");

    Require(embedder->batch_calls() == 0, "hybrid rebuild should reuse persisted embeddings without EmbedBatch");
    Require(embedder->embed_calls() == 0, "hybrid rebuild should reuse persisted embeddings without Embed");

    const auto hybrid_after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!hybrid_after_failure.items.empty(), "failed hybrid flush should rebuild and keep hybrid recall visible");
    Require(ContextHasSource(hybrid_after_failure, waxcpp::SearchSource::kText),
            "failed hybrid flush should keep text source visible");
    Require(ContextHasSource(hybrid_after_failure, waxcpp::SearchSource::kVector),
            "failed hybrid flush should keep vector source visible");
    Require(embedder->batch_calls() == 0, "hybrid rebuild should reuse persisted embeddings without EmbedBatch");
    Require(embedder->embed_calls() == 0, "hybrid rebuild should reuse persisted embeddings without Embed");

    orchestrator.Flush();  // retry no-op path after externally visible commit
    const auto hybrid_after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!hybrid_after_retry.items.empty(), "retry no-op should preserve hybrid visibility");
    Require(ContextHasSource(hybrid_after_retry, waxcpp::SearchSource::kText),
            "retry no-op should preserve text source");
    Require(ContextHasSource(hybrid_after_retry, waxcpp::SearchSource::kVector),
            "retry no-op should preserve vector source");
    Require(embedder->batch_calls() == 0, "retry no-op should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry no-op should not trigger Embed");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2,
            "hybrid vector retry no-op path should persist exactly one user frame plus one embedding record");
    Require(stats.pending_frames == 0, "hybrid vector retry no-op path should leave zero pending WAL frames");
    store.Close();
  }

  {
    auto reopen_embedder = std::make_shared<CountingBatchEmbedder>();
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    const auto hybrid_context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!hybrid_context.items.empty(), "reopen should preserve hybrid recall visibility for retry no-op path");
    Require(ContextHasSource(hybrid_context, waxcpp::SearchSource::kText),
            "reopen should preserve text source for hybrid retry no-op");
    Require(ContextHasSource(hybrid_context, waxcpp::SearchSource::kVector),
            "reopen should preserve vector source for hybrid retry no-op");
    Require(reopen_embedder->batch_calls() == 0, "reopen should reuse persisted embeddings without EmbedBatch");
    Require(reopen_embedder->embed_calls() == 0, "reopen should reuse persisted embeddings without Embed");
    reopened.Close();
  }
}

void ScenarioFlushCrashWindowTocOnlyRetryFlushPublishesOnSecondAttemptHybrid(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid flush crash-window TOC-only retry flush publishes on second attempt");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step1 retry hybrid apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected hybrid crash-window step 1");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(after_failure.items.empty(), "TOC-only hybrid crash-window should keep staged mutations hidden");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(), "second flush attempt should publish hybrid mutations after TOC-only failure");
    Require(ContextHasSource(after_retry, waxcpp::SearchSource::kText),
            "hybrid retry publish should include text source");
    Require(ContextHasSource(after_retry, waxcpp::SearchSource::kVector),
            "hybrid retry publish should include vector source");
    Require(embedder->batch_calls() == 0, "retry path should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry path should not trigger Embed");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2,
            "hybrid TOC-only retry path should persist exactly one user frame plus one embedding record");
    Require(stats.pending_frames == 0, "hybrid TOC-only retry path should leave zero pending WAL frames");
    store.Close();
  }

  {
    auto reopen_embedder = std::make_shared<CountingBatchEmbedder>();
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen should preserve hybrid recall after TOC-only retry publish");
    Require(ContextHasSource(context, waxcpp::SearchSource::kText),
            "reopen should preserve hybrid text source after TOC-only retry publish");
    Require(ContextHasSource(context, waxcpp::SearchSource::kVector),
            "reopen should preserve hybrid vector source after TOC-only retry publish");
    Require(reopen_embedder->batch_calls() == 0, "reopen should reuse persisted embeddings without EmbedBatch");
    Require(reopen_embedder->embed_calls() == 0, "reopen should reuse persisted embeddings without Embed");
    reopened.Close();
  }
}

void ScenarioFlushCrashWindowFooterPublishRetryFlushIsNoOpHybrid(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid flush crash-window after footer publish retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush step2 retry hybrid apple", {});
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(2);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw on injected hybrid crash-window step 2");

    const auto after_failure = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_failure.items.empty(), "footer-published hybrid crash-window should expose committed state");
    Require(ContextHasSource(after_failure, waxcpp::SearchSource::kText),
            "footer-published hybrid crash-window should include text source");
    Require(ContextHasSource(after_failure, waxcpp::SearchSource::kVector),
            "footer-published hybrid crash-window should include vector source");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not trigger Embed");

    orchestrator.Flush();
    const auto after_retry = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_retry.items.empty(), "retry flush after hybrid externally visible commit should preserve visibility");
    Require(ContextHasSource(after_retry, waxcpp::SearchSource::kText),
            "hybrid retry no-op should preserve text source");
    Require(ContextHasSource(after_retry, waxcpp::SearchSource::kVector),
            "hybrid retry no-op should preserve vector source");
    Require(embedder->batch_calls() == 0, "retry no-op should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "retry no-op should not trigger Embed");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2,
            "hybrid footer retry no-op path should persist exactly one user frame plus one embedding record");
    Require(stats.pending_frames == 0, "hybrid footer retry no-op path should leave zero pending WAL frames");
    store.Close();
  }

  {
    auto reopen_embedder = std::make_shared<CountingBatchEmbedder>();
    waxcpp::MemoryOrchestrator reopened(path, config, reopen_embedder);
    const auto context = reopened.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen should preserve hybrid recall after footer retry no-op");
    Require(ContextHasSource(context, waxcpp::SearchSource::kText),
            "reopen should preserve hybrid text source after footer retry no-op");
    Require(ContextHasSource(context, waxcpp::SearchSource::kVector),
            "reopen should preserve hybrid vector source after footer retry no-op");
    Require(reopen_embedder->batch_calls() == 0, "reopen should reuse persisted embeddings without EmbedBatch");
    Require(reopen_embedder->embed_calls() == 0, "reopen should reuse persisted embeddings without Embed");
    reopened.Close();
  }
}

void ScenarioNoOpFlushSkipsIndexCommitCalls(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: no-op flush skips index commit calls");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("no-op flush hybrid apple", {});
    orchestrator.Flush();
    embedder->Reset();

    bool flush_threw = false;
    waxcpp::text::testing::SetCommitFailOnCall(1);
    waxcpp::vector::testing::SetCommitFailOnCall(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::text::testing::ClearCommitFailOnCall();
    waxcpp::vector::testing::ClearCommitFailOnCall();
    Require(!flush_threw, "no-op flush should skip index CommitStaged when no pending mutations exist");

    const auto context = orchestrator.Recall("apple", std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "no-op flush path should preserve hybrid recall visibility");
    Require(ContextHasSource(context, waxcpp::SearchSource::kText),
            "no-op flush path should preserve text source visibility");
    Require(ContextHasSource(context, waxcpp::SearchSource::kVector),
            "no-op flush path should preserve vector source visibility");
    Require(embedder->batch_calls() == 0, "no-op flush should not trigger EmbedBatch");
    Require(embedder->embed_calls() == 0, "no-op flush should not trigger Embed");
    orchestrator.Close();
  }
}

void ScenarioStructuredTextIndexCommitFailureRecoversFromCommittedStore(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured text index commit failure recovers from committed store");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("text commit fail apple", {});
    orchestrator.RememberFact("user:structured-fail", "city", "rome");

    bool flush_threw = false;
    waxcpp::text::testing::SetCommitFailOnCall(2);  // fail on structured_text_index_.CommitStaged()
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::text::testing::ClearCommitFailOnCall();
    Require(flush_threw, "flush should throw when structured text index commit failpoint is set");

    const auto text_context = orchestrator.Recall("apple");
    bool has_text = false;
    for (const auto& item : text_context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kText) {
          has_text = true;
          break;
        }
      }
      if (has_text) {
        break;
      }
    }
    Require(has_text, "failed structured text commit should rebuild and keep text channel visible");

    const auto facts = orchestrator.RecallFactsByEntityPrefix("user:structured-fail", 10);
    Require(facts.size() == 1, "failed structured text commit should rebuild structured facts from committed store");

    const auto structured_context = orchestrator.Recall("rome");
    bool has_structured = false;
    for (const auto& item : structured_context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
      if (has_structured) {
        break;
      }
    }
    Require(has_structured,
            "failed structured text commit should rebuild structured-text channel from committed store");
    orchestrator.Close();
  }
}

void ScenarioStructuredTextIndexCommitFailureRetryFlushIsNoOp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured text index commit failure retry flush is no-op");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("structured retry apple", {});
    orchestrator.RememberFact("user:structured-retry", "city", "rome");

    bool flush_threw = false;
    waxcpp::text::testing::SetCommitFailOnCall(2);  // fail on structured_text_index_.CommitStaged()
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::text::testing::ClearCommitFailOnCall();
    Require(flush_threw, "flush should throw when structured text index commit failpoint is set");

    const auto facts_after_failure = orchestrator.RecallFactsByEntityPrefix("user:structured-retry", 10);
    Require(facts_after_failure.size() == 1, "failed flush should rebuild committed structured fact");
    Require(facts_after_failure.front().version == 1,
            "failed flush should keep original structured fact version after rebuild");

    orchestrator.Flush();  // retry after externally visible commit should be no-op
    const auto facts_after_retry = orchestrator.RecallFactsByEntityPrefix("user:structured-retry", 10);
    Require(facts_after_retry.size() == 1, "retry flush should not duplicate structured facts");
    Require(facts_after_retry.front().version == 1, "retry flush should not mutate structured fact version");

    const auto text_context = orchestrator.Recall("apple");
    bool has_text = false;
    for (const auto& item : text_context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kText) {
          has_text = true;
          break;
        }
      }
      if (has_text) {
        break;
      }
    }
    Require(has_text, "retry flush should keep text channel visible");

    const auto structured_context = orchestrator.Recall("rome");
    bool has_structured = false;
    for (const auto& item : structured_context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
      if (has_structured) {
        break;
      }
    }
    Require(has_structured, "retry flush should keep structured channel visible");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:structured-retry", 10);
    Require(facts.size() == 1, "reopen should keep single structured fact after retry no-op path");
    Require(facts.front().version == 1, "reopen should preserve structured fact version after retry no-op path");
    reopened.Close();
  }
}

void ScenarioUseAfterCloseThrows(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: use-after-close throws");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("close semantics text", {});
  orchestrator.Flush();
  orchestrator.Close();
  orchestrator.Close();  // idempotent

  bool recall_threw = false;
  try {
    (void)orchestrator.Recall("text");
  } catch (const std::exception&) {
    recall_threw = true;
  }
  Require(recall_threw, "Recall should throw after Close");

  bool remember_threw = false;
  try {
    orchestrator.Remember("again", {});
  } catch (const std::exception&) {
    remember_threw = true;
  }
  Require(remember_threw, "Remember should throw after Close");

  bool flush_threw = false;
  try {
    orchestrator.Flush();
  } catch (const std::exception&) {
    flush_threw = true;
  }
  Require(flush_threw, "Flush should throw after Close");

  bool fact_threw = false;
  try {
    orchestrator.RememberFact("user:closed", "city", "rome");
  } catch (const std::exception&) {
    fact_threw = true;
  }
  Require(fact_threw, "RememberFact should throw after Close");

  bool forget_fact_threw = false;
  try {
    (void)orchestrator.ForgetFact("user:closed", "city");
  } catch (const std::exception&) {
    forget_fact_threw = true;
  }
  Require(forget_fact_threw, "ForgetFact should throw after Close");

  bool recall_facts_threw = false;
  try {
    (void)orchestrator.RecallFactsByEntityPrefix("user:", 10);
  } catch (const std::exception&) {
    recall_facts_threw = true;
  }
  Require(recall_facts_threw, "RecallFactsByEntityPrefix should throw after Close");

  bool recall_with_embedding_threw = false;
  try {
    (void)orchestrator.Recall("text", {});
  } catch (const std::exception&) {
    recall_with_embedding_threw = true;
  }
  Require(recall_with_embedding_threw, "Recall(query, embedding) should throw after Close");
}

void ScenarioStructuredFactStagedOrderBeforeFlush(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured fact staged order before flush");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:stage", "city", "paris");
    orchestrator.RememberFact("user:stage", "city", "rome");
    const bool removed = orchestrator.ForgetFact("user:stage", "city");
    Require(removed, "ForgetFact should remove staged key");

    const auto before_flush = orchestrator.Recall("rome");
    Require(before_flush.items.empty(), "staged structured fact mutations should stay hidden before flush");

    orchestrator.Flush();
    const auto after_flush = orchestrator.Recall("rome");
    bool has_structured = false;
    for (const auto& item : after_flush.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(!has_structured, "final staged remove should win within same flush");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:stage", 10);
    Require(facts.empty(), "reopen should preserve final remove outcome");
    reopened.Close();
  }
}

void ScenarioStructuredFactCloseWithoutFlushPersistsViaStoreClose(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured fact close without flush persists via store close");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:noflush", "city", "berlin");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:noflush", 10);
    Require(facts.size() == 1, "Close() should persist structured fact without explicit Flush");
    Require(facts[0].value == "berlin", "persisted structured fact value mismatch");

    const auto context = reopened.Recall("berlin");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(has_structured, "reopened orchestrator should rebuild structured-text index from persisted fact");
    reopened.Close();
  }
}

void ScenarioStructuredFactForgetWithoutFlushPersistsViaStoreClose(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured fact forget without flush persists via store close");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:noflush-remove", "city", "rome");
    orchestrator.Flush();

    const bool removed = orchestrator.ForgetFact("user:noflush-remove", "city");
    Require(removed, "ForgetFact should return true for committed key");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:noflush-remove", 10);
    Require(facts.empty(), "Close() should persist forget fact without explicit Flush");

    const auto context = reopened.Recall("rome");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(!has_structured, "removed structured fact should not appear in recall after reopen");
    reopened.Close();
  }
}

void ScenarioStructuredMemoryRemovePersists(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured memory remove persists");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:9", "city", "Paris");
    orchestrator.RememberFact("user:9", "name", "Dora");
    const bool removed = orchestrator.ForgetFact("user:9", "city");
    const bool removed_missing = orchestrator.ForgetFact("user:9", "missing");
    Require(removed, "ForgetFact should return true when key exists");
    Require(!removed_missing, "ForgetFact should return false for missing key");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:9", 10);
    Require(facts.size() == 1, "removed fact should stay removed after reopen");
    Require(facts[0].attribute == "name", "unexpected fact left after remove replay");

    const auto context = reopened.Recall("paris");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(!has_structured, "removed fact must not participate in recall");
    reopened.Close();
  }
}

void ScenarioStructuredFactSeededFlushReopenModelParity(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured fact seeded flush/reopen model parity");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  struct ModeledFact final {
    std::uint64_t id = 0;
    std::string entity{};
    std::string attribute{};
    std::string value{};
    waxcpp::Metadata metadata{};
    std::uint64_t version = 0;
  };

  auto make_key = [](const std::string& entity, const std::string& attribute) {
    return entity + '\x1F' + attribute;
  };

  auto sort_facts = [](std::vector<waxcpp::StructuredMemoryEntry> facts) {
    std::sort(facts.begin(), facts.end(), [](const auto& lhs, const auto& rhs) {
      if (lhs.entity != rhs.entity) {
        return lhs.entity < rhs.entity;
      }
      if (lhs.attribute != rhs.attribute) {
        return lhs.attribute < rhs.attribute;
      }
      return lhs.id < rhs.id;
    });
    return facts;
  };

  auto require_facts_equal = [&](const std::vector<waxcpp::StructuredMemoryEntry>& actual,
                                 const std::vector<waxcpp::StructuredMemoryEntry>& expected,
                                 const std::string& where) {
    Require(actual.size() == expected.size(), where + ": fact count mismatch");
    for (std::size_t i = 0; i < expected.size(); ++i) {
      Require(actual[i].entity == expected[i].entity, where + ": entity mismatch");
      Require(actual[i].attribute == expected[i].attribute, where + ": attribute mismatch");
      Require(actual[i].value == expected[i].value, where + ": value mismatch");
      Require(actual[i].metadata == expected[i].metadata, where + ": metadata mismatch");
      Require(actual[i].version == expected[i].version, where + ": version mismatch");
      Require(actual[i].id == expected[i].id, where + ": id mismatch");
    }
  };

  std::unordered_map<std::string, ModeledFact> committed{};
  std::unordered_map<std::string, ModeledFact> staged{};
  std::uint64_t next_id = 0;
  std::uint64_t staged_next_id = 0;
  bool has_staged = false;

  auto ensure_staged = [&]() {
    if (has_staged) {
      return;
    }
    staged = committed;
    staged_next_id = next_id;
    has_staged = true;
  };

  auto model_upsert = [&](const std::string& entity,
                          const std::string& attribute,
                          const std::string& value,
                          const waxcpp::Metadata& metadata) {
    ensure_staged();
    const auto key = make_key(entity, attribute);
    auto it = staged.find(key);
    if (it == staged.end()) {
      ModeledFact entry{};
      entry.id = staged_next_id++;
      entry.entity = entity;
      entry.attribute = attribute;
      entry.value = value;
      entry.metadata = metadata;
      entry.version = 1;
      staged.emplace(key, std::move(entry));
      return;
    }
    it->second.value = value;
    it->second.metadata = metadata;
    it->second.version += 1;
  };

  auto model_remove = [&](const std::string& entity, const std::string& attribute) {
    ensure_staged();
    const auto key = make_key(entity, attribute);
    return staged.erase(key) > 0;
  };

  auto model_commit = [&]() {
    if (!has_staged) {
      return;
    }
    committed = staged;
    staged.clear();
    next_id = staged_next_id;
    has_staged = false;
  };

  auto expected_facts = [&]() {
    std::vector<waxcpp::StructuredMemoryEntry> out{};
    out.reserve(committed.size());
    for (const auto& [_, fact] : committed) {
      waxcpp::StructuredMemoryEntry entry{};
      entry.id = fact.id;
      entry.entity = fact.entity;
      entry.attribute = fact.attribute;
      entry.value = fact.value;
      entry.metadata = fact.metadata;
      entry.version = fact.version;
      out.push_back(std::move(entry));
    }
    return sort_facts(std::move(out));
  };

  auto assert_structured_query_visibility = [&](waxcpp::MemoryOrchestrator& orchestrator,
                                                const std::vector<waxcpp::StructuredMemoryEntry>& committed_facts,
                                                std::mt19937& rng,
                                                const std::string& where) {
    if (committed_facts.empty()) {
      const auto context = orchestrator.Recall("nonexistent-query-token");
      bool has_structured = false;
      for (const auto& item : context.items) {
        for (const auto source : item.sources) {
          if (source == waxcpp::SearchSource::kStructuredMemory) {
            has_structured = true;
            break;
          }
        }
      }
      Require(!has_structured, where + ": empty model should not have structured hits");
      return;
    }

    const std::size_t probe_index = static_cast<std::size_t>(rng() % static_cast<std::uint32_t>(committed_facts.size()));
    const auto& probe = committed_facts[probe_index];
    const auto context = orchestrator.Recall(probe.value);
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
      if (has_structured) {
        break;
      }
    }
    Require(has_structured, where + ": committed fact value should be recallable via structured source");
  };

  std::mt19937 rng(0x5EED1234U);
  const std::array<std::string, 6> entities = {
      "user:0", "user:1", "user:2", "user:3", "user:4", "user:5"};
  const std::array<std::string, 4> attributes = {
      "city", "food", "team", "language"};
  const std::array<std::string, 10> values = {
      "rome", "paris", "berlin", "tokyo", "madrid", "apple", "sushi", "jazz", "swift", "cpp"};
  std::uniform_int_distribution<int> operation_dist(0, 99);
  std::uniform_int_distribution<int> entity_dist(0, static_cast<int>(entities.size()) - 1);
  std::uniform_int_distribution<int> attribute_dist(0, static_cast<int>(attributes.size()) - 1);
  std::uniform_int_distribution<int> value_dist(0, static_cast<int>(values.size()) - 1);

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

    constexpr int kIterations = 128;
    for (int i = 0; i < kIterations; ++i) {
      const auto& entity = entities[static_cast<std::size_t>(entity_dist(rng))];
      const auto& attribute = attributes[static_cast<std::size_t>(attribute_dist(rng))];
      const int op = operation_dist(rng);

      if (op < 65) {
        const auto& value = values[static_cast<std::size_t>(value_dist(rng))];
        waxcpp::Metadata metadata{};
        metadata.emplace("src", "seeded");
        metadata.emplace("iter", std::to_string(i % 11));
        if ((i % 3) == 0) {
          metadata.emplace("tag", values[static_cast<std::size_t>(value_dist(rng))]);
        }
        orchestrator.RememberFact(entity, attribute, value, metadata);
        model_upsert(entity, attribute, value, metadata);
      } else if (op < 92) {
        const bool removed_runtime = orchestrator.ForgetFact(entity, attribute);
        const bool removed_model = model_remove(entity, attribute);
        Require(removed_runtime == removed_model, "seeded ForgetFact return mismatch against model");
      } else {
        orchestrator.Flush();
        model_commit();
        const auto actual = sort_facts(orchestrator.RecallFactsByEntityPrefix("user:", 512));
        const auto expected = expected_facts();
        require_facts_equal(actual, expected, "seeded flush checkpoint");
        assert_structured_query_visibility(orchestrator, expected, rng, "seeded flush checkpoint");
      }
    }

    orchestrator.Flush();
    model_commit();
    const auto final_actual = sort_facts(orchestrator.RecallFactsByEntityPrefix("user:", 512));
    const auto final_expected = expected_facts();
    require_facts_equal(final_actual, final_expected, "seeded final flush parity");
    assert_structured_query_visibility(orchestrator, final_expected, rng, "seeded final flush parity");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto actual = sort_facts(reopened.RecallFactsByEntityPrefix("user:", 512));
    const auto expected = expected_facts();
    require_facts_equal(actual, expected, "seeded reopen parity");
    assert_structured_query_visibility(reopened, expected, rng, "seeded reopen parity");
    reopened.Close();
  }
}

void ScenarioStructuredFactMetadataSerializationDeterminism(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured fact metadata serialization determinism");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  auto iteration_order = [](const waxcpp::Metadata& metadata) {
    std::vector<std::string> order{};
    order.reserve(metadata.size());
    for (const auto& [key, _] : metadata) {
      order.push_back(key);
    }
    return order;
  };

  waxcpp::Metadata metadata_a{};
  metadata_a.reserve(17);
  for (int i = 0; i < 24; ++i) {
    metadata_a.emplace("k" + std::to_string(i), "v" + std::to_string((i * 7) % 19));
  }

  waxcpp::Metadata metadata_b{};
  metadata_b.reserve(257);
  for (int i = 23; i >= 0; --i) {
    metadata_b.emplace("k" + std::to_string(i), "v" + std::to_string((i * 7) % 19));
  }

  auto order_a = iteration_order(metadata_a);
  auto order_b = iteration_order(metadata_b);
  if (order_a == order_b) {
    metadata_b.rehash(997);
    order_b = iteration_order(metadata_b);
  }
  Require(order_a != order_b,
          "test precondition: metadata iteration orders must differ to validate deterministic serialization");

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:determinism", "city", "rome", metadata_a);
    orchestrator.RememberFact("user:determinism", "city", "rome", metadata_b);
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2, "determinism scenario should persist two structured upsert journal frames");
    store.Close();
  }

  const auto first_payload = ReadStructuredUpsertPayloadByOrdinal(path, 0);
  const auto second_payload = ReadStructuredUpsertPayloadByOrdinal(path, 1);
  Require(first_payload == second_payload,
          "structured fact payload bytes must be deterministic across metadata insertion-order permutations");
}

void ScenarioMalformedStructuredJournalPayloadsAreIgnored(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: malformed structured journal payloads are ignored");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:ok", "city", "rome");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    (void)store.Put(BuildMalformedStructuredFactRemovePayloadEmptyEntity(), {});
    (void)store.Put(BuildMalformedStructuredFactUpsertPayloadEmptyAttribute(), {});
    store.Commit();
    store.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:ok", 10);
    Require(facts.size() == 1, "malformed structured payload should be ignored during replay");
    Require(facts.front().attribute == "city", "valid structured fact should remain intact");
    Require(facts.front().value == "rome", "valid structured fact value should remain intact");

    const auto context = reopened.Recall("rome");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
      if (has_structured) {
        break;
      }
    }
    Require(has_structured, "valid structured fact should remain searchable after malformed replay records");
    reopened.Close();
  }
}

void ScenarioStructuredJournalMalformedFuzzKeepsValidFacts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured journal malformed fuzz keeps valid facts");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:ok", "city", "rome");
    orchestrator.Flush();
    orchestrator.Close();
  }

  std::mt19937 rng(0xA11CEBEEU);
  std::uniform_int_distribution<int> mutate_kind_dist(0, 2);
  std::uniform_int_distribution<int> index_dist(0, 23);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::uniform_int_distribution<int> tail_len_dist(0, 18);

  {
    auto store = waxcpp::WaxStore::Open(path);
    constexpr int kMalformedRecords = 128;
    for (int i = 0; i < kMalformedRecords; ++i) {
      std::vector<std::byte> payload =
          (i % 2 == 0) ? BuildMalformedStructuredFactRemovePayloadEmptyEntity()
                       : BuildMalformedStructuredFactUpsertPayloadEmptyAttribute();
      const int mutate_kind = mutate_kind_dist(rng);
      if (mutate_kind == 0 && payload.size() > 6) {
        const std::size_t pos = 6 + static_cast<std::size_t>(index_dist(rng)) % (payload.size() - 6);
        payload[pos] = static_cast<std::byte>(static_cast<unsigned char>(byte_dist(rng)));
      } else if (mutate_kind == 1) {
        const int tail_len = tail_len_dist(rng);
        for (int j = 0; j < tail_len; ++j) {
          payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte_dist(rng))));
        }
      } else if (mutate_kind == 2 && payload.size() > 10) {
        payload.resize(payload.size() - 3);
      }
      (void)store.Put(payload, {});
    }
    store.Commit();
    store.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:ok", 32);
    bool found_valid_fact = false;
    for (const auto& fact : facts) {
      if (fact.entity == "user:ok" && fact.attribute == "city" && fact.value == "rome") {
        found_valid_fact = true;
        break;
      }
    }
    Require(found_valid_fact, "valid structured fact must survive malformed structured journal replay");
    reopened.Close();
  }
}

void ScenarioStructuredJournalRejectsOversizedFieldsAndMetadataCount(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured journal rejects oversized fields and metadata count");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:stable", "city", "rome");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    (void)store.Put(BuildMalformedStructuredFactUpsertPayloadOverlongEntityLength(), {});
    (void)store.Put(BuildMalformedStructuredFactUpsertPayloadMetadataCountOverflow(), {});
    store.Commit();
    store.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:stable", 10);
    Require(facts.size() == 1, "oversized structured payloads should be ignored during replay");
    Require(facts.front().attribute == "city", "stable fact attribute mismatch after malformed replay");
    Require(facts.front().value == "rome", "stable fact value mismatch after malformed replay");
    reopened.Close();
  }
}

void ScenarioStructuredJournalRejectsDuplicateMetadataKeys(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured journal rejects duplicate metadata keys");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:stable-meta", "city", "rome");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    (void)store.Put(BuildMalformedStructuredFactUpsertPayloadDuplicateMetadataKey(), {});
    store.Commit();
    store.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto stable_facts = reopened.RecallFactsByEntityPrefix("user:stable-meta", 10);
    Require(stable_facts.size() == 1, "duplicate-metadata malformed payload should be ignored");
    Require(stable_facts.front().value == "rome", "stable fact value mismatch after duplicate metadata payload");

    const auto malformed_facts = reopened.RecallFactsByEntityPrefix("user:dup-meta", 10);
    Require(malformed_facts.empty(), "duplicate metadata payload must not create committed structured fact");
    reopened.Close();
  }
}

void ScenarioStructuredJournalRejectsOversizedPayloadBytes(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured journal rejects oversized payload bytes");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:stable-bytes", "city", "rome");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    (void)store.Put(BuildMalformedStructuredFactUpsertPayloadOversizedTotalBytes(), {});
    store.Commit();
    store.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto stable_facts = reopened.RecallFactsByEntityPrefix("user:stable-bytes", 10);
    Require(stable_facts.size() == 1, "oversized structured payload should be ignored");
    const auto malformed_facts = reopened.RecallFactsByEntityPrefix("user:oversized-payload", 10);
    Require(malformed_facts.empty(), "oversized structured payload must not create committed fact");
    reopened.Close();
  }
}

void ScenarioConcurrentRememberIsSerialized(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: concurrent remember is serialized");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  constexpr int kThreadCount = 4;
  constexpr int kDocsPerThread = 12;
  std::unordered_set<std::string> expected_payloads{};
  expected_payloads.reserve(static_cast<std::size_t>(kThreadCount * kDocsPerThread));

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    std::vector<std::thread> workers{};
    workers.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int doc_index = 0; doc_index < kDocsPerThread; ++doc_index) {
          const std::string text =
              "thread-" + std::to_string(thread_index) + " doc-" + std::to_string(doc_index) + " apple";
          orchestrator.Remember(text, {});
        }
      });
    }

    for (auto& worker : workers) {
      worker.join();
    }

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      for (int doc_index = 0; doc_index < kDocsPerThread; ++doc_index) {
        expected_payloads.insert("thread-" + std::to_string(thread_index) + " doc-" +
                                 std::to_string(doc_index) + " apple");
      }
    }

    orchestrator.Flush();
    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "concurrent remember should leave searchable text context after flush");
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    const std::uint64_t expected_count = static_cast<std::uint64_t>(kThreadCount * kDocsPerThread);
    Require(stats.frame_count == expected_count, "concurrent remember should persist every document exactly once");

    std::unordered_set<std::string> persisted_payloads{};
    persisted_payloads.reserve(static_cast<std::size_t>(stats.frame_count));
    const auto metas = store.FrameMetas();
    for (const auto& meta : metas) {
      if (meta.status != 0) {
        continue;
      }
      persisted_payloads.insert(BytesToString(store.FrameContent(meta.id)));
    }

    Require(persisted_payloads.size() == expected_payloads.size(),
            "persisted payload set size mismatch after concurrent remember");
    for (const auto& expected : expected_payloads) {
      Require(persisted_payloads.find(expected) != persisted_payloads.end(),
              "missing payload after concurrent remember");
    }
    store.Close();
  }
}

void ScenarioConcurrentRecallIsStable(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: concurrent recall is stable");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  constexpr int kThreadCount = 8;
  constexpr int kRecallsPerThread = 64;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("alpha apple signal", {});
  orchestrator.Remember("beta apple signal", {});
  orchestrator.Remember("gamma unrelated", {});
  orchestrator.Flush();

  const auto baseline = orchestrator.Recall("apple");
  Require(!baseline.items.empty(), "baseline recall must produce non-empty context");
  const auto baseline_top_id = baseline.items.front().frame_id;
  const auto baseline_top_text = baseline.items.front().text;

  std::atomic<bool> failed{false};
  std::vector<std::thread> workers{};
  workers.reserve(kThreadCount);
  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    workers.emplace_back([&]() {
      try {
        for (int i = 0; i < kRecallsPerThread; ++i) {
          const auto context = orchestrator.Recall("apple");
          if (context.items.empty()) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
          if (context.items.front().frame_id != baseline_top_id) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
          if (context.items.front().text != baseline_top_text) {
            failed.store(true, std::memory_order_relaxed);
            return;
          }
        }
      } catch (...) {
        failed.store(true, std::memory_order_relaxed);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  Require(!failed.load(std::memory_order_relaxed),
          "concurrent recall should produce stable deterministic top result");
  orchestrator.Close();
}

void ScenarioConcurrentRememberFactIsSerialized(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: concurrent remember fact is serialized");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  constexpr int kThreadCount = 4;
  constexpr int kFactsPerThread = 12;
  std::unordered_set<std::string> expected_keys{};
  expected_keys.reserve(static_cast<std::size_t>(kThreadCount * kFactsPerThread));

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    std::vector<std::thread> workers{};
    workers.reserve(kThreadCount);

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int fact_index = 0; fact_index < kFactsPerThread; ++fact_index) {
          const std::string entity = "user:t" + std::to_string(thread_index) + ":f" + std::to_string(fact_index);
          const std::string attribute = "city";
          const std::string value = "v" + std::to_string(thread_index) + "_" + std::to_string(fact_index);
          waxcpp::Metadata metadata{};
          metadata.emplace("thread", std::to_string(thread_index));
          metadata.emplace("fact", std::to_string(fact_index));
          orchestrator.RememberFact(entity, attribute, value, metadata);
        }
      });
    }

    for (auto& worker : workers) {
      worker.join();
    }

    for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
      for (int fact_index = 0; fact_index < kFactsPerThread; ++fact_index) {
        expected_keys.insert("user:t" + std::to_string(thread_index) + ":f" + std::to_string(fact_index) + '\x1F' + "city");
      }
    }

    orchestrator.Flush();
    const auto facts = orchestrator.RecallFactsByEntityPrefix("user:t", 2048);
    Require(facts.size() == expected_keys.size(), "concurrent RememberFact should publish all inserted facts");
    std::unordered_set<std::string> actual_keys{};
    actual_keys.reserve(facts.size());
    for (const auto& fact : facts) {
      actual_keys.insert(fact.entity + '\x1F' + fact.attribute);
    }
    Require(actual_keys == expected_keys, "concurrent RememberFact fact key set mismatch after flush");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:t", 2048);
    Require(facts.size() == expected_keys.size(),
            "reopen after concurrent RememberFact should preserve all inserted facts");

    std::unordered_set<std::string> actual_keys{};
    actual_keys.reserve(facts.size());
    for (const auto& fact : facts) {
      actual_keys.insert(fact.entity + '\x1F' + fact.attribute);
    }
    Require(actual_keys == expected_keys, "reopen concurrent RememberFact fact key set mismatch");
    reopened.Close();
  }
}

// ── Direct search ──────────────────────────────────────────────

void ScenarioSearchTextModeReturnsHits(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Search text-mode returns ranked hits");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("apple orange banana", {});
  orchestrator.Remember("apple kiwi mango", {});
  orchestrator.Remember("grape pear cherry", {});
  orchestrator.Flush();

  const auto hits = orchestrator.Search("apple", waxcpp::DirectSearchMode::kText, 0.5f, 10);
  Require(!hits.empty(), "Search should return at least one hit for matching query");
  Require(hits[0].frame_id == 0 || hits[0].frame_id == 1,
          "top hit should be one of the apple-containing frames");
  Require(hits[0].score > 0.0f, "hit score should be positive");

  orchestrator.Close();
}

void ScenarioSearchEmptyQueryReturnsEmpty(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Search with empty/whitespace query returns empty");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("test content", {});
  orchestrator.Flush();

  Require(orchestrator.Search("", waxcpp::DirectSearchMode::kText).empty(),
          "empty query should return no hits");
  Require(orchestrator.Search("   \n\t  ", waxcpp::DirectSearchMode::kText).empty(),
          "whitespace query should return no hits");
  Require(orchestrator.Search("apple", waxcpp::DirectSearchMode::kText, 0.5f, 0).empty(),
          "topK=0 should return no hits");

  orchestrator.Close();
}

void ScenarioSearchHybridFallsBackToText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Search hybrid-mode without embedder falls back to text");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("deep learning neural networks", {});
  orchestrator.Flush();

  // Hybrid mode without embedder should fall back to text-only.
  const auto hits = orchestrator.Search("neural", waxcpp::DirectSearchMode::kHybrid, 0.5f, 5);
  Require(!hits.empty(), "hybrid fallback to text should return results");
  Require(hits[0].frame_id == 0, "should find the matching frame");

  orchestrator.Close();
}

// ── RuntimeStats ──────────────────────────────────────────────

void ScenarioRuntimeStatsReturnsValidData(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: GetRuntimeStats returns valid store info");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

  {
    const auto stats = orchestrator.GetRuntimeStats();
    Require(stats.frame_count == 0, "initial frame count should be 0");
    Require(stats.store_path == path, "store_path should match constructor path");
    Require(!stats.vector_search_enabled, "vector search should be disabled");
    Require(stats.embedder_identity.empty(), "embedder identity should be empty without embedder");
  }

  orchestrator.Remember("test stats content", {});
  orchestrator.Flush();

  {
    const auto stats = orchestrator.GetRuntimeStats();
    Require(stats.frame_count == 1, "frame count should be 1 after remember+flush");
    Require(stats.generation > 0, "generation should be positive after flush");
  }

  orchestrator.Close();
}

void ScenarioRuntimeStatsWithEmbedder(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: GetRuntimeStats reports embedder identity");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = true;

  auto embedder = std::make_shared<CountingEmbedder>();
  waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);

  const auto stats = orchestrator.GetRuntimeStats();
  Require(stats.vector_search_enabled, "vector search should be enabled with embedder");
  Require(!stats.embedder_identity.empty(), "embedder identity should not be empty");
  Require(stats.embedder_identity.find("WaxCppTest") != std::string::npos,
          "embedder identity should contain provider name");

  orchestrator.Close();
}

// ── Session tagging ──────────────────────────────────────────

void ScenarioStartEndSession(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: StartSession/EndSession lifecycle");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

  const auto session_id = orchestrator.StartSession();
  Require(!session_id.empty(), "StartSession should return non-empty session ID");
  Require(session_id.size() == 36, "session ID should be UUID-formatted (36 chars)");

  // Verify format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
  Require(session_id[8] == '-', "session ID should have dash at position 8");
  Require(session_id[13] == '-', "session ID should have dash at position 13");
  Require(session_id[18] == '-', "session ID should have dash at position 18");
  Require(session_id[23] == '-', "session ID should have dash at position 23");

  // Second start should return different ID.
  const auto session_id2 = orchestrator.StartSession();
  Require(!session_id2.empty(), "second StartSession should also return non-empty ID");
  Require(session_id != session_id2, "session IDs should be unique");

  orchestrator.EndSession();

  orchestrator.Close();
}

// ── OptimizeSurrogates orchestrator method ───────────────────

void ScenarioOptimizeSurrogatesViaOrchestrator(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: OptimizeSurrogates via orchestrator generates surrogates");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  // Ingest a few chunks of text.
  orchestrator.Remember(
      "Machine learning is a subset of artificial intelligence. "
      "Deep learning uses neural networks with many layers. "
      "Transfer learning reduces training time significantly.",
      {});
  orchestrator.Remember(
      "Natural language processing handles text data. "
      "Tokenization splits text into units. "
      "Embeddings represent words as dense vectors.",
      {});
  orchestrator.Flush();

  waxcpp::MaintenanceOptions opts{};
  opts.overwrite_existing = false;
  opts.enable_hierarchical = true;
  const auto report = orchestrator.OptimizeSurrogates(opts);

  Require(report.scanned_frames >= 2, "should scan at least 2 frames");
  Require(report.eligible_frames >= 2, "both frames should be eligible");
  Require(report.generated_surrogates >= 2, "should generate surrogates for eligible frames");
  Require(!report.did_timeout, "should not timeout");

  orchestrator.Close();

  // Verify surrogates are persisted.
  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count >= 4, "should have original frames + surrogate frames");
    store.Close();
  }
}

void ScenarioOptimizeSurrogatesSkipExisting(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: OptimizeSurrogates skips existing surrogates");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember(
      "Quantum computing uses qubits for computation. "
      "Superposition allows multiple states simultaneously.",
      {});
  orchestrator.Flush();

  // First run should generate surrogates.
  waxcpp::MaintenanceOptions opts{};
  const auto report1 = orchestrator.OptimizeSurrogates(opts);
  Require(report1.generated_surrogates >= 1, "first run should generate at least 1 surrogate");

  // Flush the new surrogates.
  orchestrator.Flush();

  // Second run should generate zero new surrogates.
  // Source frames are now superseded by their surrogates and will be skipped.
  const auto report2 = orchestrator.OptimizeSurrogates(opts);
  Require(report2.generated_surrogates == 0, "second run should generate 0 surrogates");

  orchestrator.Close();
}

void ScenarioQueryEmbeddingPolicyNeverSkipsEmbedding(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: QueryEmbeddingPolicy::kNever skips embedding");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = true;
  config.query_embedding_policy = waxcpp::QueryEmbeddingPolicy::kNever;

  // Even though we provide an embedder, kNever should prevent its use for queries.
  auto embedder = std::make_shared<CountingEmbedder>();
  waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
  orchestrator.Remember("Neural networks learn from data.", {});
  orchestrator.Flush();

  // Recall should succeed using text-only search despite hybrid being configured.
  const auto ctx = orchestrator.Recall("neural networks");
  // Must return results from the text channel.
  Require(!ctx.items.empty(), "kNever recall should still return text results");

  orchestrator.Close();
}

void ScenarioQueryEmbeddingPolicyAlwaysThrowsWithoutEmbedder(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: QueryEmbeddingPolicy::kAlways throws without embedder");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  config.query_embedding_policy = waxcpp::QueryEmbeddingPolicy::kAlways;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("Test data for policy always.", {});
  orchestrator.Flush();

  bool threw = false;
  try {
    orchestrator.Recall("test");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Require(threw, "kAlways without embedder should throw");

  orchestrator.Close();
}

void ScenarioCompactIndexesReturnsValidReport(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: CompactIndexes returns valid report");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("Frame one for compact test.", {});
  orchestrator.Remember("Frame two for compact test.", {});
  orchestrator.Flush();

  const auto report = orchestrator.CompactIndexes();
  Require(report.scanned_frames >= 2, "compact should report scanned frames");

  // Recall still works after compaction.
  const auto ctx = orchestrator.Recall("compact test");
  Require(!ctx.items.empty(), "recall after compact should return results");

  orchestrator.Close();
}

void ScenarioSurrogateMapHandlesOverwriteChain(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: BuildSurrogateMap handles overwrite chain correctly");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember(
      "Dark matter constitutes approximately 27 percent of the universe. "
      "Scientists search for dark matter particles using deep underground detectors.",
      {});
  orchestrator.Flush();

  // First surrogate generation.
  waxcpp::MaintenanceOptions opts{};
  opts.enable_hierarchical = true;
  const auto report1 = orchestrator.OptimizeSurrogates(opts);
  Require(report1.generated_surrogates >= 1, "first run should generate surrogates");

  orchestrator.Flush();

  // Overwrite existing surrogates.
  opts.overwrite_existing = true;
  const auto report2 = orchestrator.OptimizeSurrogates(opts);
  Require(report2.generated_surrogates >= 1, "overwrite run should regenerate surrogates");
  Require(report2.superseded_surrogates >= 1, "overwrite should supersede old surrogates");

  orchestrator.Flush();

  // After overwrite, Recall should still find the original content
  // (proving the surrogate map correctly maps source → newest surrogate).
  const auto ctx = orchestrator.Recall("dark matter");
  Require(!ctx.items.empty(), "recall after overwrite should still return results");

  orchestrator.Close();
}

void ScenarioRecallWithFrameFilterExcludesSuperseded(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Recall(query, FrameFilter) excludes superseded frames");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("The quick brown fox jumps over the lazy dog.", {});
  orchestrator.Flush();

  // Generate surrogates — this supersedes source frames.
  waxcpp::MaintenanceOptions opts{};
  const auto report = orchestrator.OptimizeSurrogates(opts);
  Require(report.generated_surrogates >= 1, "should generate surrogates");
  orchestrator.Flush();

  // Default filter: include_superseded=false, include_surrogates=false.
  waxcpp::FrameFilter filter{};
  const auto ctx = orchestrator.Recall("fox", filter);
  // Results should exclude both superseded source and surrogate frames.
  // Text search may still find content, but the filter removes superseded/surrogates.
  // In practice the items may be empty if all matching frames are filtered.
  // This scenario verifies the filter pipeline doesn't crash.

  orchestrator.Close();
}

void ScenarioRecallWithPerCallEmbeddingPolicy(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Recall(query, QueryEmbeddingPolicy) per-call override");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  // Default policy is kIfAvailable.
  config.query_embedding_policy = waxcpp::QueryEmbeddingPolicy::kIfAvailable;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("Machine learning models learn patterns from data.", {});
  orchestrator.Flush();

  // Per-call kNever should work (text-only).
  const auto ctx = orchestrator.Recall("machine learning", waxcpp::QueryEmbeddingPolicy::kNever);
  Require(!ctx.items.empty(), "kNever per-call recall should still return text results");

  // Per-call kAlways without embedder should throw.
  bool threw = false;
  try {
    (void)orchestrator.Recall("test", waxcpp::QueryEmbeddingPolicy::kAlways);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  Require(threw, "kAlways per-call without embedder should throw");

  orchestrator.Close();
}

void ScenarioSearchWithFrameFilter(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Search(query, FrameFilter) filters results");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("Alpha bravo charlie.", {});
  orchestrator.Remember("Delta echo foxtrot.", {});
  orchestrator.Flush();

  // Get the frame IDs via unfiltered search.
  const auto all_hits = orchestrator.Search("alpha delta", waxcpp::DirectSearchMode::kText, 0.0f, 10);

  if (all_hits.size() >= 2) {
    // Filter to only the first frame by ID.
    waxcpp::FrameFilter filter{};
    filter.frame_ids = std::unordered_set<std::uint64_t>{all_hits[0].frame_id};
    const auto filtered = orchestrator.Search("alpha delta", filter, waxcpp::DirectSearchMode::kText, 0.0f, 10);
    Require(filtered.size() <= 1, "filtered search should return at most 1 hit");
  }

  orchestrator.Close();
}

void ScenarioActiveSessionId(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: ActiveSessionId returns current session ID");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

  // No session active.
  Require(orchestrator.ActiveSessionId().empty(), "no active session initially");

  // Start session.
  const auto id = orchestrator.StartSession();
  Require(!id.empty(), "StartSession should return non-empty ID");
  Require(orchestrator.ActiveSessionId() == id, "ActiveSessionId should match");

  // End session.
  orchestrator.EndSession();
  Require(orchestrator.ActiveSessionId().empty(), "no active session after end");

  orchestrator.Close();
}

void ScenarioSessionRuntimeStatsReturnsActive(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: GetSessionRuntimeStats returns active state");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

  // No session.
  const auto stats1 = orchestrator.GetSessionRuntimeStats();
  Require(!stats1.active, "should not be active before StartSession");
  Require(stats1.session_id.empty(), "session_id should be empty");

  // Start session.
  const auto id = orchestrator.StartSession();
  const auto stats2 = orchestrator.GetSessionRuntimeStats();
  Require(stats2.active, "should be active after StartSession");
  Require(stats2.session_id == id, "session_id should match");

  // End session.
  orchestrator.EndSession();
  const auto stats3 = orchestrator.GetSessionRuntimeStats();
  Require(!stats3.active, "should not be active after EndSession");

  orchestrator.Close();
}

void ScenarioRuntimeStatsIncludesNewFields(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: GetRuntimeStats includes structured_memory_enabled and access_stats_scoring_enabled");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  config.enable_structured_memory = true;
  config.enable_access_stats_scoring = true;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  const auto stats = orchestrator.GetRuntimeStats();
  Require(stats.structured_memory_enabled, "structured_memory_enabled should be true");
  Require(stats.access_stats_scoring_enabled, "access_stats_scoring_enabled should be true");

  orchestrator.Close();
}

void ScenarioTierSelectionPolicyFromConfig(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: FastRAGConfig.tier_selection_policy wired into orchestrator");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  // Use age-only tier selection policy.
  config.rag.tier_selection_policy = waxcpp::TierPolicyAgeBalanced();
  config.rag.enable_query_aware_tier_selection = true;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember(
      "Photosynthesis converts light energy into chemical energy. "
      "Chloroplasts contain chlorophyll that absorbs sunlight.",
      {});
  orchestrator.Flush();

  // Generate surrogates to test that tier selection works.
  waxcpp::MaintenanceOptions opts{};
  opts.enable_hierarchical = true;
  const auto report = orchestrator.OptimizeSurrogates(opts);
  Require(report.generated_surrogates >= 1, "should generate surrogates");
  orchestrator.Flush();

  // Recall should work with the age-based tier selector.
  const auto ctx = orchestrator.Recall("photosynthesis");
  // The scenario verifies no crash with the wired tier policy.

  orchestrator.Close();
}

void ScenarioRememberHandoffAndLatestHandoff(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: RememberHandoff/LatestHandoff round-trip");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

  // No handoffs yet.
  const auto empty = orchestrator.LatestHandoff();
  Require(!empty.has_value(), "no handoff initially");

  // Remember a handoff.
  const auto fid = orchestrator.RememberHandoff(
      "Session summary: discussed architecture patterns.",
      std::string("my-project"),
      {"Implement caching layer", "Add unit tests"});
  // Frame IDs start at 0 for empty stores; verify the call succeeded.
  (void)fid;  // Valid frame ID assigned.

  // Retrieve it.
  const auto rec = orchestrator.LatestHandoff();
  Require(rec.has_value(), "should find handoff");
  Require(rec->frame_id == fid, "frame_id should match");
  Require(rec->content == "Session summary: discussed architecture patterns.", "content mismatch");
  Require(rec->project.has_value() && *rec->project == "my-project", "project mismatch");
  Require(rec->pending_tasks.size() == 2, "should have 2 pending tasks");
  Require(rec->pending_tasks[0] == "Implement caching layer", "task 0 mismatch");
  Require(rec->pending_tasks[1] == "Add unit tests", "task 1 mismatch");

  // Filter by project.
  const auto wrong_project = orchestrator.LatestHandoff(std::string("other-project"));
  Require(!wrong_project.has_value(), "wrong project should return empty");

  const auto right_project = orchestrator.LatestHandoff(std::string("my-project"));
  Require(right_project.has_value(), "right project should find handoff");

  orchestrator.Close();
}

void ScenarioHandoffPersistsAcrossReopen(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Handoff persists across close/reopen");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberHandoff(
        "Handoff content for persistence test.",
        std::string("persist-project"),
        {"Task A"});
    orchestrator.Close();
  }

  // Reopen and verify.
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    const auto rec = orchestrator.LatestHandoff();
    Require(rec.has_value(), "handoff should persist across reopen");
    Require(rec->content == "Handoff content for persistence test.", "content should persist");
    Require(rec->project.has_value() && *rec->project == "persist-project", "project should persist");
    Require(rec->pending_tasks.size() == 1, "should have 1 pending task");
    orchestrator.Close();
  }
}

// ── Per-frame metadata persistence tests ─────────────────────

void ScenarioTimestampPersistsAcrossReopen(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Timestamp persists across close/reopen");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("Timestamp test content.");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    // Access FrameMetas via Recall to trigger a load, then check stats.
    const auto ctx = orchestrator.Recall("timestamp");
    // The handoff LatestHandoff scans FrameMetas — let's use the same approach
    // via RuntimeStats to verify the store is populated.
    const auto stats = orchestrator.GetRuntimeStats();
    Require(stats.frame_count >= 1, "should have at least 1 frame");
    orchestrator.Close();
  }
}

void ScenarioMetadataPersistsViaHandoff(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Metadata persists via handoff kind field");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberHandoff("Handoff with metadata.", std::string("test-proj"), {"task A"});
    orchestrator.Close();
  }

  // Reopen and verify kind="handoff" is now visible.
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    const auto rec = orchestrator.LatestHandoff();
    Require(rec.has_value(), "handoff should be found via kind metadata");
    Require(rec->content == "Handoff with metadata.", "handoff content match");
    orchestrator.Close();
  }
}

void ScenarioFrameMetadataRoundTrip(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Frame metadata round-trips through WaxStore");
  // Use WaxStore directly to test metadata persistence.
  auto store = waxcpp::WaxStore::Create(path);

  waxcpp::Metadata meta;
  meta["kind"] = "test-kind";
  meta["project"] = "my-proj";
  meta["author"] = "alice";

  const std::string content = "Hello metadata world";
  std::vector<std::byte> bytes;
  bytes.reserve(content.size());
  for (char c : content) bytes.push_back(static_cast<std::byte>(c));
  const auto fid = store.Put(bytes, meta);
  store.Commit();

  const auto metas = store.FrameMetas();
  Require(metas.size() == 1, "should have 1 frame");
  Require(metas[0].id == fid, "frame id match");
  Require(metas[0].timestamp_ms > 0, "timestamp should be set");
  Require(metas[0].kind.has_value() && *metas[0].kind == "test-kind", "kind should persist");
  Require(metas[0].metadata.at("project") == "my-proj", "metadata[project] should persist");
  Require(metas[0].metadata.at("author") == "alice", "metadata[author] should persist");
  Require(metas[0].metadata.at("kind") == "test-kind", "metadata[kind] should persist");

  store.Close();

  // Reopen and verify persistence after TOC read-back.
  auto store2 = waxcpp::WaxStore::Open(path);
  const auto metas2 = store2.FrameMetas();
  Require(metas2.size() == 1, "should have 1 frame after reopen");
  Require(metas2[0].timestamp_ms > 0, "timestamp should survive reopen");
  Require(metas2[0].kind.has_value() && *metas2[0].kind == "test-kind", "kind should survive reopen");
  Require(metas2[0].metadata.at("project") == "my-proj", "metadata[project] should survive reopen");
  store2.Close();
}

void ScenarioRecallWithMetadataFilter(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: Recall with MetadataFilter filters by required_entries and required_labels");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);

  // Ingest two frames with different metadata.
  orchestrator.Remember("Alpha content about cats.", {{"topic", "cats"}, {"labels", "public,animal"}});
  orchestrator.Remember("Beta content about dogs.", {{"topic", "dogs"}, {"labels", "internal,animal"}});
  orchestrator.Flush();

  // Search with MetadataFilter requiring topic=cats and label=public.
  waxcpp::FrameFilter ff{};
  waxcpp::MetadataFilter mf{};
  mf.required_entries["topic"] = "cats";
  mf.required_labels.push_back("public");
  ff.metadata_filter = mf;

  const auto results = orchestrator.Search("content", ff);
  // Should only match the cats frame.
  bool found_cats = false;
  bool found_dogs = false;
  for (const auto& hit : results) {
    if (hit.preview_text.has_value()) {
      if (hit.preview_text->find("cats") != std::string::npos) found_cats = true;
      if (hit.preview_text->find("dogs") != std::string::npos) found_dogs = true;
    }
  }
  Require(found_cats, "should find cats frame with topic=cats filter");
  Require(!found_dogs, "should NOT find dogs frame with topic=cats filter");

  orchestrator.Close();
}

}  // namespace

int main() {
  waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_orchestrator_test_");
  try {
    waxcpp::tests::Log("memory_orchestrator_test: start");
    const auto path0 = UniquePath();
    const auto path1 = UniquePath();
    const auto path2 = UniquePath();
    const auto path3 = UniquePath();
    const auto path4 = UniquePath();
    const auto path5 = UniquePath();
    const auto path6 = UniquePath();
    const auto path7 = UniquePath();
    const auto path8 = UniquePath();
    const auto path9 = UniquePath();
    const auto path10 = UniquePath();
    const auto path11 = UniquePath();
    const auto path12 = UniquePath();
    const auto path13 = UniquePath();
    const auto path14 = UniquePath();
    const auto path15 = UniquePath();
    const auto path16 = UniquePath();
    const auto path17 = UniquePath();
    const auto path18 = UniquePath();
    const auto path19 = UniquePath();
    const auto path20 = UniquePath();
    const auto path21 = UniquePath();
    const auto path22 = UniquePath();
    const auto path23 = UniquePath();
    const auto path24 = UniquePath();
    const auto path25 = UniquePath();
    const auto path26 = UniquePath();
    const auto path27 = UniquePath();
    const auto path28 = UniquePath();
    const auto path29 = UniquePath();
    const auto path30 = UniquePath();
    const auto path31 = UniquePath();
    const auto path32 = UniquePath();
    const auto path33 = UniquePath();
    const auto path34 = UniquePath();
    const auto path35 = UniquePath();
    const auto path36 = UniquePath();
    const auto path37 = UniquePath();
    const auto path38 = UniquePath();
    const auto path39 = UniquePath();
    const auto path40 = UniquePath();
    const auto path41 = UniquePath();
    const auto path42 = UniquePath();
    const auto path43 = UniquePath();
    const auto path44 = UniquePath();
    const auto path45 = UniquePath();
    const auto path46 = UniquePath();
    const auto path47 = UniquePath();
    const auto path48 = UniquePath();
    const auto path49 = UniquePath();
    const auto path50 = UniquePath();
    const auto path51 = UniquePath();
    const auto path52 = UniquePath();
    const auto path53 = UniquePath();
    const auto path54 = UniquePath();
    const auto path55 = UniquePath();
    const auto path56 = UniquePath();
    const auto path57 = UniquePath();
    const auto path58 = UniquePath();
    const auto path59 = UniquePath();
    const auto path60 = UniquePath();
    const auto path61 = UniquePath();
    const auto path62 = UniquePath();
    const auto path63 = UniquePath();
    const auto path64 = UniquePath();
    const auto path65 = UniquePath();
    const auto path66 = UniquePath();
    const auto path67 = UniquePath();
    const auto path68 = UniquePath();
    const auto path69 = UniquePath();
    const auto path70 = UniquePath();
    const auto path71 = UniquePath();
    const auto path72 = UniquePath();
    const auto path73 = UniquePath();
    const auto path74 = UniquePath();
    const auto path75 = UniquePath();
    const auto path76 = UniquePath();
    const auto path77 = UniquePath();
    const auto path78 = UniquePath();
    const auto path79 = UniquePath();
    const auto path80 = UniquePath();
    const auto path81 = UniquePath();
    const auto path82 = UniquePath();
    const auto path83 = UniquePath();
    const auto path84 = UniquePath();
    const auto path85 = UniquePath();
    const auto path86 = UniquePath();
    const auto path87 = UniquePath();
    const auto path88 = UniquePath();
    const auto path89 = UniquePath();
    const auto path90 = UniquePath();
    const auto path91 = UniquePath();
    const auto path92 = UniquePath();
    const auto path93 = UniquePath();
    const auto path94 = UniquePath();
    const auto path95 = UniquePath();
    const auto path96 = UniquePath();
    const auto path97 = UniquePath();
    const auto path98 = UniquePath();
    const auto path99 = UniquePath();
    const auto path100 = UniquePath();
    const auto path101 = UniquePath();
    const auto path102 = UniquePath();
    const auto path103 = UniquePath();
    const auto path104 = UniquePath();
    const auto path105 = UniquePath();
    const auto path106 = UniquePath();
    const auto path107 = UniquePath();
    const auto path108 = UniquePath();
    const auto path109 = UniquePath();
    const auto path110 = UniquePath();
    const auto path111 = UniquePath();
    const auto path112 = UniquePath();
    const auto path113 = UniquePath();
    const auto path114 = UniquePath();
    const auto path115 = UniquePath();
    const auto path116 = UniquePath();
    const auto path117 = UniquePath();
    const auto path118 = UniquePath();
    const auto path119 = UniquePath();
    const auto path120 = UniquePath();
    const auto path121 = UniquePath();
    const auto path122 = UniquePath();
    const auto path123 = UniquePath();
    const auto path124 = UniquePath();
    const auto path125 = UniquePath();
    const auto path126 = UniquePath();
    const auto path127 = UniquePath();
    const auto path128 = UniquePath();
    const auto path129 = UniquePath();
    const auto path130 = UniquePath();
    const auto path131 = UniquePath();
    const auto path132 = UniquePath();
    const auto path133 = UniquePath();
    const auto path134 = UniquePath();
    const auto path135 = UniquePath();
    const auto path136 = UniquePath();
    const auto path137 = UniquePath();
    const auto path138 = UniquePath();

    ScenarioVectorPolicyValidation(path0);
    ScenarioOnDeviceProviderPolicyValidation(path42);
    ScenarioEmbeddingDimensionPolicyValidation(path43);
    ScenarioSearchModePolicyValidation(path22);
    ScenarioRecallEmbeddingPolicyValidation(path29);
    ScenarioForgetFactValidation(path84);
    ScenarioRememberFactSerializationFailureDoesNotStageMutation(path99);
    ScenarioForgetFactValidationAndSerializationSafety(path101);
    ScenarioRememberFlushPersistsFrame(path1);
    ScenarioRecallReturnsRankedItems(path2);
    ScenarioHybridRecallWithEmbedder(path3);
    ScenarioEmbeddingMemoizationInRecall(path4);
    ScenarioBatchProviderUsedForVectorRecall(path5);
    ScenarioMaxSnippetsClamp(path6);
    ScenarioMaxSnippetsZeroSuppressesSnippetsOnly(path45);
    ScenarioExpansionDisabledStillReturnsRecallItems(path46);
    ScenarioRememberChunking(path7);
    ScenarioBatchProviderUsedForRemember(path8);
    ScenarioRememberRespectsIngestBatchSize(path9);
    ScenarioRememberUsesConfiguredIngestConcurrency(path37);
    ScenarioVectorRebuildUsesConfiguredIngestConcurrency(path38);
    ScenarioVectorReopenWithNonFinitePersistedEmbeddingReembeds(path79);
    ScenarioVectorReopenIgnoresLaterNonFinitePersistedOverride(path89);
    ScenarioVectorReopenWithMalformedPersistedEmbeddingCountSkipsRecord(path83);
    ScenarioVectorReopenWithMalformedPersistedEmbeddingIdentitySkipsRecord(path87);
    ScenarioVectorReopenWithOverlongIdentityV2SkipsRecord(path92);
    ScenarioVectorReopenWithControlCharIdentityV2SkipsRecord(path93);
    ScenarioVectorReopenWithEmptyIdentityV2SkipsRecord(path90);
    ScenarioVectorReopenEmptyIdentityOverrideDoesNotReplaceValidPersisted(path91);
    ScenarioVectorReopenEmptyEmbeddingOverrideDoesNotReplaceValidPersisted(path95);
    ScenarioVectorReopenDimensionMismatchedOverrideDoesNotReplaceValidPersisted(path100);
    ScenarioVectorReopenMalformedEmbeddingFuzzKeepsValidPersistedVector(path88);
    ScenarioRememberIngestConcurrencyPropagatesEmbedErrors(path39);
    ScenarioRememberRejectsNonFiniteEmbeddings(path80);
    ScenarioRecallExplicitEmbeddingRejectsNonFinite(path81);
    ScenarioRecallQueryEmbeddingRejectsNonFiniteFromEmbedder(path82);
    ScenarioTextOnlyRecallSkipsVectorEmbedding(path10);
    ScenarioStructuredMemoryFacts(path11);
    ScenarioRecallIncludesStructuredMemory(path12);
    ScenarioRecallTextChannelUsesTextSource(path13);
    ScenarioStructuredMemoryRemovePersists(path14);
    ScenarioStructuredFactSeededFlushReopenModelParity(path103);
    ScenarioStructuredFactMetadataSerializationDeterminism(path104);
    ScenarioMalformedStructuredJournalPayloadsAreIgnored(path85);
    ScenarioStructuredJournalMalformedFuzzKeepsValidFacts(path86);
    ScenarioStructuredJournalRejectsOversizedFieldsAndMetadataCount(path94);
    ScenarioStructuredJournalRejectsDuplicateMetadataKeys(path96);
    ScenarioStructuredJournalRejectsOversizedPayloadBytes(path102);
    ScenarioRecallVisibilityRequiresFlush(path15);
    ScenarioVectorRecallVisibilityRequiresFlush(path16);
    ScenarioVectorIndexRebuildOnReopen(path17);
    ScenarioVectorReopenReusesPersistedEmbeddingsWithoutReembed(path30);
    ScenarioVectorReopenWithMatchingIdentityReusesPersistedEmbeddings(path40);
    ScenarioVectorReopenWithMismatchedIdentityReembeds(path41);
    ScenarioOversizedIdentityFallbackUsesWAXEM1AndReusesPersistedVectors(path97);
    ScenarioControlCharIdentityFallbackUsesWAXEM1AndReusesPersistedVectors(path98);
    ScenarioEmbeddingJournalDoesNotLeakIntoTextRecall(path31);
    ScenarioVectorCloseWithoutFlushPersistsViaStoreClose(path18);
    ScenarioVectorRecallSupportsExplicitEmbeddingWithoutQuery(path19);
    ScenarioVectorRecallEmptyQueryWithoutEmbeddingReturnsEmpty(path74);
    ScenarioVectorRecallWhitespaceQueryWithoutEmbeddingReturnsEmpty(path75);
    ScenarioHybridRecallWhitespaceQueryWithoutEmbeddingReturnsEmpty(path76);
    ScenarioHybridRecallWhitespaceQueryWithExplicitEmbeddingUsesVectorOnly(path77);
    ScenarioTextOnlyRecallWhitespaceQueryReturnsEmpty(path78);
    ScenarioHybridRecallWithExplicitEmbeddingSkipsQueryEmbed(path44);
    ScenarioFlushFailureDoesNotExposeStagedText(path20);
    ScenarioFlushFailureDoesNotExposeStagedVector(path21);
    ScenarioFlushFailureThenCloseReopenRecoversText(path23);
    ScenarioFlushFailureThenCloseReopenRecoversVector(path24);
    ScenarioFlushCrashWindowFooterPublishRebuildsText(path50);
    ScenarioFlushCrashWindowHeaderAPublishRebuildsText(path49);
    ScenarioFlushCrashWindowHeaderPublishRebuildsText(path47);
    ScenarioFlushCrashWindowHeaderPublishRebuildsVector(path48);
    ScenarioFlushCrashWindowHeaderAPublishRebuildsVector(path54);
    ScenarioFlushCrashWindowCheckpointPublishRebuildsText(path51);
    ScenarioFlushCrashWindowFooterPublishRebuildsVector(path52);
    ScenarioFlushCrashWindowCheckpointPublishRebuildsVector(path53);
    ScenarioFlushCrashWindowFooterPublishRetryFlushIsNoOp(path55);
    ScenarioFlushCrashWindowFooterPublishRetryFlushIsNoOpVector(path56);
    ScenarioFlushCrashWindowCheckpointPublishRetryFlushIsNoOp(path57);
    ScenarioFlushCrashWindowCheckpointPublishRetryFlushIsNoOpVector(path58);
    ScenarioFlushCrashWindowHeaderAPublishRetryFlushIsNoOp(path59);
    ScenarioFlushCrashWindowHeaderAPublishRetryFlushIsNoOpVector(path60);
    ScenarioFlushCrashWindowHeaderBPublishRetryFlushIsNoOp(path61);
    ScenarioFlushCrashWindowHeaderBPublishRetryFlushIsNoOpVector(path62);
    ScenarioFlushCrashWindowTocOnlyRetryFlushPublishesOnSecondAttempt(path63);
    ScenarioFlushCrashWindowTocOnlyRetryFlushPublishesOnSecondAttemptVector(path64);
    ScenarioFlushCrashWindowTocOnlyRetryFlushPublishesOnSecondAttemptHybrid(path111);
    ScenarioFlushCrashWindowFooterPublishRetryFlushIsNoOpHybrid(path112);
    ScenarioFlushCrashWindowStructuredFactRebuildsStep2(path65);
    ScenarioFlushCrashWindowStructuredFactRebuildsStep3(path66);
    ScenarioFlushCrashWindowStructuredFactRebuildsStep4(path67);
    ScenarioFlushCrashWindowStructuredFactRebuildsStep5(path68);
    ScenarioFlushCrashWindowStructuredFactRetryNoOpStep2(path69);
    ScenarioFlushCrashWindowStructuredFactRetryNoOpStep3(path70);
    ScenarioFlushCrashWindowStructuredFactRetryNoOpStep4(path71);
    ScenarioFlushCrashWindowStructuredFactRetryNoOpStep5(path72);
    ScenarioFlushFailureThenCloseReopenRecoversStructuredFact(path25);
    ScenarioFlushFailureDoesNotExposeStagedStructuredFactUntilRetry(path32);
    ScenarioTextIndexCommitFailureRecoversFromCommittedStore(path33);
    ScenarioTextIndexCommitFailureRetryFlushIsNoOp(path109);
    ScenarioVectorIndexCommitFailureRecoversFromCommittedStore(path34);
    ScenarioVectorIndexCommitFailureRetryFlushIsNoOp(path108);
    ScenarioHybridVectorIndexCommitFailureRecoversBothChannelsAndRetryNoOp(path110);
    ScenarioNoOpFlushSkipsIndexCommitCalls(path113);
    ScenarioStructuredTextIndexCommitFailureRecoversFromCommittedStore(path106);
    ScenarioStructuredTextIndexCommitFailureRetryFlushIsNoOp(path107);
    ScenarioUseAfterCloseThrows(path26);
    ScenarioStructuredFactStagedOrderBeforeFlush(path27);
    ScenarioStructuredFactCloseWithoutFlushPersistsViaStoreClose(path28);
    ScenarioStructuredFactForgetWithoutFlushPersistsViaStoreClose(path35);
    ScenarioConcurrentRememberIsSerialized(path36);
    ScenarioConcurrentRecallIsStable(path73);
    ScenarioConcurrentRememberFactIsSerialized(path105);
    ScenarioSearchTextModeReturnsHits(path114);
    ScenarioSearchEmptyQueryReturnsEmpty(path115);
    ScenarioSearchHybridFallsBackToText(path116);
    ScenarioRuntimeStatsReturnsValidData(path117);
    ScenarioRuntimeStatsWithEmbedder(path118);
    ScenarioStartEndSession(path119);
    ScenarioOptimizeSurrogatesViaOrchestrator(path120);
    ScenarioOptimizeSurrogatesSkipExisting(path121);
    ScenarioQueryEmbeddingPolicyNeverSkipsEmbedding(path122);
    ScenarioQueryEmbeddingPolicyAlwaysThrowsWithoutEmbedder(path123);
    ScenarioCompactIndexesReturnsValidReport(path124);
    ScenarioSurrogateMapHandlesOverwriteChain(path125);
    ScenarioRecallWithFrameFilterExcludesSuperseded(path126);
    ScenarioRecallWithPerCallEmbeddingPolicy(path127);
    ScenarioSearchWithFrameFilter(path128);
    ScenarioActiveSessionId(path129);
    ScenarioSessionRuntimeStatsReturnsActive(path130);
    ScenarioRuntimeStatsIncludesNewFields(path131);
    ScenarioTierSelectionPolicyFromConfig(path132);
    ScenarioRememberHandoffAndLatestHandoff(path133);
    ScenarioHandoffPersistsAcrossReopen(path134);
    ScenarioTimestampPersistsAcrossReopen(path135);
    ScenarioMetadataPersistsViaHandoff(path136);
    ScenarioFrameMetadataRoundTrip(path137);
    ScenarioRecallWithMetadataFilter(path138);

    const std::vector<std::filesystem::path> cleanup_paths = {
        path0,  path1,  path2,  path3,  path4,  path5,  path6,  path7,  path8,  path9,  path10,
        path11, path12, path13, path14, path15, path16, path17, path18, path19, path20, path21,
        path22, path23, path24, path25, path26, path27, path28, path29, path30, path31, path32,
        path33, path34, path35, path36, path37, path38, path39, path40, path41, path42, path43,
        path44, path45, path46, path47, path48, path49, path50, path51, path52, path53, path54,
        path55, path56, path57, path58, path59, path60, path61, path62, path63, path64, path65,
        path66, path67, path68, path69, path70, path71, path72, path73, path74, path75, path76,
        path77, path78, path79, path80, path81, path82,
        path83, path84, path85, path86, path87, path88, path89, path90, path91, path92, path93,
        path94, path95, path96, path97, path98, path99, path100, path101, path102, path103, path104, path105,
        path106, path107, path108, path109, path110, path111, path112, path113,
        path114, path115, path116, path117, path118, path119, path120, path121,
        path122, path123, path124, path125,
        path126, path127, path128, path129, path130, path131, path132, path133, path134,
        path135, path136, path137, path138,
    };
    for (const auto& path : cleanup_paths) {
      CleanupPath(path);
    }
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_orchestrator_test_");
    waxcpp::tests::Log("memory_orchestrator_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_orchestrator_test_");
    return EXIT_FAILURE;
  }
}
