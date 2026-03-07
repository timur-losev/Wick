#pragma once

#include <cstddef>
#include <vector>

namespace waxcpp {

/// High-performance vector math operations for embedding computation.
/// C++ equivalent of Swift's VectorMath (Accelerate/vDSP), using portable
/// loops that modern compilers auto-vectorize to SSE/AVX/NEON.
namespace VectorMath {

// ---------- L2 Normalization ----------

/// Normalizes a vector to unit length (L2 norm = 1).
/// Returns the original vector if it's empty or has zero magnitude.
std::vector<float> NormalizeL2(const std::vector<float>& vector);

/// Normalizes a vector in-place to unit length (L2 norm = 1).
/// More efficient when you don't need to preserve the original.
void NormalizeL2InPlace(std::vector<float>& vector);

// ---------- Dot Product ----------

/// Computes the dot product of two equally-sized vectors.
/// Undefined behavior if sizes differ (debug builds assert).
float DotProduct(const float* a, const float* b, std::size_t n);
float DotProduct(const std::vector<float>& a, const std::vector<float>& b);

// ---------- Cosine Similarity ----------

/// Computes cosine similarity assuming vectors are already normalized.
/// For pre-normalized embeddings this is equivalent to DotProduct.
float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

/// Computes cosine similarity, normalizing vectors first.
float CosineSimilarityNormalized(const std::vector<float>& a, const std::vector<float>& b);

// ---------- Euclidean Distance ----------

/// Computes squared Euclidean distance (avoids sqrt).
float SquaredEuclideanDistance(const std::vector<float>& a, const std::vector<float>& b);

/// Computes Euclidean distance.
float EuclideanDistance(const std::vector<float>& a, const std::vector<float>& b);

// ---------- Magnitude ----------

/// Computes the L2 magnitude (length) of a vector.
float Magnitude(const std::vector<float>& vector);

/// Returns true if the vector is approximately unit length.
bool IsNormalizedL2(const std::vector<float>& vector, float tolerance = 1e-3f);

// ---------- Element-wise Operations ----------

/// Adds two vectors element-wise.
std::vector<float> Add(const std::vector<float>& a, const std::vector<float>& b);

/// Subtracts vector b from vector a element-wise.
std::vector<float> Subtract(const std::vector<float>& a, const std::vector<float>& b);

/// Multiplies a vector by a scalar.
std::vector<float> Scale(const std::vector<float>& vector, float scalar);

}  // namespace VectorMath

}  // namespace waxcpp
