#include "sha256.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace waxcpp::core {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};

constexpr std::uint32_t RotateRight(std::uint32_t value, std::uint32_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

}  // namespace

Sha256::Sha256() : state_({0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}) {}

void Sha256::Update(std::span<const std::byte> bytes) {
  if (finalized_) {
    throw std::logic_error("Sha256::Update called after Finalize");
  }

  for (const std::byte value : bytes) {
    buffer_[buffer_len_++] = static_cast<std::uint8_t>(value);
    if (buffer_len_ == buffer_.size()) {
      Transform(buffer_.data());
      bit_length_ += 512;
      buffer_len_ = 0;
    }
  }
}

std::array<std::byte, 32> Sha256::Finalize() {
  if (!finalized_) {
    const std::uint64_t total_bits = bit_length_ + static_cast<std::uint64_t>(buffer_len_ * 8U);

    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > 56) {
      while (buffer_len_ < buffer_.size()) {
        buffer_[buffer_len_++] = 0x00;
      }
      Transform(buffer_.data());
      buffer_len_ = 0;
    }
    while (buffer_len_ < 56) {
      buffer_[buffer_len_++] = 0x00;
    }

    for (int i = 7; i >= 0; --i) {
      buffer_[buffer_len_++] = static_cast<std::uint8_t>((total_bits >> (i * 8)) & 0xFFU);
    }
    Transform(buffer_.data());
    finalized_ = true;
  }

  std::array<std::byte, 32> out{};
  for (std::size_t i = 0; i < state_.size(); ++i) {
    out[i * 4 + 0] = static_cast<std::byte>((state_[i] >> 24) & 0xFFU);
    out[i * 4 + 1] = static_cast<std::byte>((state_[i] >> 16) & 0xFFU);
    out[i * 4 + 2] = static_cast<std::byte>((state_[i] >> 8) & 0xFFU);
    out[i * 4 + 3] = static_cast<std::byte>(state_[i] & 0xFFU);
  }
  return out;
}

void Sha256::Transform(const std::uint8_t* chunk) {
  std::uint32_t message_schedule[64] = {};
  for (int i = 0; i < 16; ++i) {
    const int base = i * 4;
    message_schedule[i] = (static_cast<std::uint32_t>(chunk[base + 0]) << 24) |
                          (static_cast<std::uint32_t>(chunk[base + 1]) << 16) |
                          (static_cast<std::uint32_t>(chunk[base + 2]) << 8) |
                          (static_cast<std::uint32_t>(chunk[base + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = RotateRight(message_schedule[i - 15], 7) ^
                             RotateRight(message_schedule[i - 15], 18) ^
                             (message_schedule[i - 15] >> 3);
    const std::uint32_t s1 = RotateRight(message_schedule[i - 2], 17) ^
                             RotateRight(message_schedule[i - 2], 19) ^
                             (message_schedule[i - 2] >> 10);
    message_schedule[i] = message_schedule[i - 16] + s0 + message_schedule[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + message_schedule[i];
    const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

std::array<std::byte, 32> Sha256Digest(std::span<const std::byte> bytes) {
  Sha256 hasher;
  hasher.Update(bytes);
  return hasher.Finalize();
}

}  // namespace waxcpp::core
