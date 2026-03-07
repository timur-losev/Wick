#pragma once

#include <cstdint>

namespace waxcpp::core::testing {

void SetCommitFailStep(std::uint32_t step);
void ClearCommitFailStep();

}  // namespace waxcpp::core::testing
