#pragma once

#include "waxcpp/token_counter.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace waxcpp {

/// Hierarchical surrogate tier token budgets.
struct SurrogateTierConfig {
  int full_max_tokens = 100;
  int gist_max_tokens = 25;
  int micro_max_tokens = 8;

  static SurrogateTierConfig Default() { return {100, 25, 8}; }
  static SurrogateTierConfig Compact() { return {50, 15, 5}; }
  static SurrogateTierConfig Verbose() { return {150, 40, 12}; }
};

/// Output of hierarchical surrogate generation.
struct SurrogateTiers {
  std::string full;    // Maximum fidelity (~100 tokens).
  std::string gist;    // Balanced compression (~25 tokens).
  std::string micro;   // Minimal, entity + topic only (~8 tokens).
  int version = 1;     // Algorithm version.
};

/// Extractive surrogate generator: sentence segmentation, scoring, MMR selection.
/// Uses TokenCounter for accurate truncation when available.
class ExtractiveSurrogateGenerator {
 public:
  static constexpr const char* kAlgorithmID = "extractive_v1";

  ExtractiveSurrogateGenerator() = default;
  explicit ExtractiveSurrogateGenerator(const TokenCounter* counter);

  /// Generate a single-tier surrogate with max token budget.
  std::string Generate(std::string_view source_text, int max_tokens) const;

  /// Generate hierarchical surrogates (full/gist/micro).
  SurrogateTiers GenerateTiers(std::string_view source_text,
                               const SurrogateTierConfig& config = SurrogateTierConfig::Default()) const;

 private:
  const TokenCounter* counter_ = nullptr;
};

}  // namespace waxcpp
