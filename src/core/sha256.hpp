#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace waxcpp::core {

class Sha256 {
 public:
  Sha256();

  void Update(std::span<const std::byte> bytes);
  [[nodiscard]] std::array<std::byte, 32> Finalize();

 private:
  void Transform(const std::uint8_t* chunk);

  std::array<std::uint8_t, 64> buffer_{};
  std::array<std::uint32_t, 8> state_{};
  std::uint64_t bit_length_ = 0;
  std::size_t buffer_len_ = 0;
  bool finalized_ = false;
};

[[nodiscard]] std::array<std::byte, 32> Sha256Digest(std::span<const std::byte> bytes);

}  // namespace waxcpp::core
