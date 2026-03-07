#include "waxcpp/types.hpp"

#include "../test_logger.hpp"

#include <cstdlib>
#include <iostream>

int main() {
  waxcpp::tests::Log("smoke_test: start");
  waxcpp::OrchestratorConfig config;
  if (!config.enable_text_search) {
    std::cerr << "enable_text_search default mismatch\n";
    return EXIT_FAILURE;
  }
  if (!config.enable_vector_search) {
    std::cerr << "enable_vector_search default mismatch\n";
    return EXIT_FAILURE;
  }
  if (config.chunking.target_tokens <= 0) {
    std::cerr << "chunking target_tokens must be positive\n";
    return EXIT_FAILURE;
  }

  waxcpp::tests::Log("smoke_test: finished");
  std::cout << "waxcpp smoke test passed\n";
  return EXIT_SUCCESS;
}
