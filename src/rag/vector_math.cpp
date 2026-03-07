#include "waxcpp/vector_math.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>

namespace waxcpp {
namespace VectorMath {

// ──────────────────────────────────────────────
// L2 Normalization
// ──────────────────────────────────────────────

std::vector<float> NormalizeL2(const std::vector<float>& vector) {
  if (vector.empty()) return vector;

  double sum_sq = 0.0;
  for (const float x : vector) {
    sum_sq += static_cast<double>(x) * static_cast<double>(x);
  }
  if (sum_sq <= 0.0) return vector;

  const auto inv_norm = static_cast<float>(1.0 / std::sqrt(sum_sq));
  std::vector<float> result(vector.size());
  for (std::size_t i = 0; i < vector.size(); ++i) {
    result[i] = vector[i] * inv_norm;
  }
  return result;
}

void NormalizeL2InPlace(std::vector<float>& vector) {
  if (vector.empty()) return;

  double sum_sq = 0.0;
  for (const float x : vector) {
    sum_sq += static_cast<double>(x) * static_cast<double>(x);
  }
  if (sum_sq <= 0.0) return;

  const auto inv_norm = static_cast<float>(1.0 / std::sqrt(sum_sq));
  for (float& x : vector) {
    x *= inv_norm;
  }
}

// ──────────────────────────────────────────────
// Dot Product
// ──────────────────────────────────────────────

float DotProduct(const float* a, const float* b, std::size_t n) {
  // Accumulate in double for numerical stability on large vectors.
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    sum += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  }
  return static_cast<float>(sum);
}

float DotProduct(const std::vector<float>& a, const std::vector<float>& b) {
  assert(a.size() == b.size());
  if (a.empty()) return 0.0f;
  return DotProduct(a.data(), b.data(), a.size());
}

// ──────────────────────────────────────────────
// Cosine Similarity
// ──────────────────────────────────────────────

float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  return DotProduct(a, b);
}

float CosineSimilarityNormalized(const std::vector<float>& a, const std::vector<float>& b) {
  auto norm_a = NormalizeL2(a);
  auto norm_b = NormalizeL2(b);
  return DotProduct(norm_a, norm_b);
}

// ──────────────────────────────────────────────
// Euclidean Distance
// ──────────────────────────────────────────────

float SquaredEuclideanDistance(const std::vector<float>& a, const std::vector<float>& b) {
  assert(a.size() == b.size());
  if (a.empty()) return 0.0f;

  double sum = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += diff * diff;
  }
  return static_cast<float>(sum);
}

float EuclideanDistance(const std::vector<float>& a, const std::vector<float>& b) {
  return std::sqrt(SquaredEuclideanDistance(a, b));
}

// ──────────────────────────────────────────────
// Magnitude
// ──────────────────────────────────────────────

float Magnitude(const std::vector<float>& vector) {
  if (vector.empty()) return 0.0f;

  double sum_sq = 0.0;
  for (const float x : vector) {
    sum_sq += static_cast<double>(x) * static_cast<double>(x);
  }
  return static_cast<float>(std::sqrt(sum_sq));
}

bool IsNormalizedL2(const std::vector<float>& vector, float tolerance) {
  if (vector.empty()) return false;
  const float length = Magnitude(vector);
  return std::abs(length - 1.0f) <= tolerance;
}

// ──────────────────────────────────────────────
// Element-wise Operations
// ──────────────────────────────────────────────

std::vector<float> Add(const std::vector<float>& a, const std::vector<float>& b) {
  assert(a.size() == b.size());
  if (a.empty()) return {};

  std::vector<float> result(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    result[i] = a[i] + b[i];
  }
  return result;
}

std::vector<float> Subtract(const std::vector<float>& a, const std::vector<float>& b) {
  assert(a.size() == b.size());
  if (a.empty()) return {};

  std::vector<float> result(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    result[i] = a[i] - b[i];
  }
  return result;
}

std::vector<float> Scale(const std::vector<float>& vector, float scalar) {
  if (vector.empty()) return vector;

  std::vector<float> result(vector.size());
  for (std::size_t i = 0; i < vector.size(); ++i) {
    result[i] = vector[i] * scalar;
  }
  return result;
}

}  // namespace VectorMath
}  // namespace waxcpp
