#include "waxcpp/vector_math.hpp"
#include "../test_logger.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace waxcpp;
using namespace waxcpp::tests;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* label) {
  if (condition) {
    ++g_pass;
    Log(std::string("  PASS: ") + label);
  } else {
    ++g_fail;
    LogError(std::string("  FAIL: ") + label);
  }
}

bool Approx(float a, float b, float tol = 1e-5f) {
  return std::abs(a - b) <= tol;
}

// ============================================================
// 1. NormalizeL2
// ============================================================

void TestNormalizeL2() {
  Log("=== TestNormalizeL2 ===");
  std::vector<float> v = {3.0f, 4.0f};
  auto result = VectorMath::NormalizeL2(v);
  Check(result.size() == 2, "size preserved");
  Check(Approx(result[0], 0.6f), "x component");
  Check(Approx(result[1], 0.8f), "y component");
  // Original should be unchanged.
  Check(Approx(v[0], 3.0f), "original unchanged");
}

void TestNormalizeL2Empty() {
  Log("=== TestNormalizeL2Empty ===");
  auto result = VectorMath::NormalizeL2({});
  Check(result.empty(), "empty returns empty");
}

void TestNormalizeL2Zero() {
  Log("=== TestNormalizeL2Zero ===");
  std::vector<float> v = {0.0f, 0.0f, 0.0f};
  auto result = VectorMath::NormalizeL2(v);
  Check(Approx(result[0], 0.0f) && Approx(result[1], 0.0f) && Approx(result[2], 0.0f),
        "zero vector unchanged");
}

void TestNormalizeL2InPlace() {
  Log("=== TestNormalizeL2InPlace ===");
  std::vector<float> v = {3.0f, 4.0f};
  VectorMath::NormalizeL2InPlace(v);
  Check(Approx(v[0], 0.6f), "x component in-place");
  Check(Approx(v[1], 0.8f), "y component in-place");
}

// ============================================================
// 2. DotProduct
// ============================================================

void TestDotProduct() {
  Log("=== TestDotProduct ===");
  std::vector<float> a = {1.0f, 2.0f, 3.0f};
  std::vector<float> b = {4.0f, 5.0f, 6.0f};
  float result = VectorMath::DotProduct(a, b);
  Check(Approx(result, 32.0f), "dot product = 1*4 + 2*5 + 3*6 = 32");
}

void TestDotProductEmpty() {
  Log("=== TestDotProductEmpty ===");
  std::vector<float> a, b;
  Check(Approx(VectorMath::DotProduct(a, b), 0.0f), "empty dot product = 0");
}

void TestDotProductOrthogonal() {
  Log("=== TestDotProductOrthogonal ===");
  std::vector<float> a = {1.0f, 0.0f};
  std::vector<float> b = {0.0f, 1.0f};
  Check(Approx(VectorMath::DotProduct(a, b), 0.0f), "orthogonal dot product = 0");
}

// ============================================================
// 3. Cosine Similarity
// ============================================================

void TestCosineSimilarityNormalized() {
  Log("=== TestCosineSimilarityNormalized ===");
  std::vector<float> a = {1.0f, 0.0f};
  std::vector<float> b = {0.0f, 1.0f};
  Check(Approx(VectorMath::CosineSimilarityNormalized(a, b), 0.0f),
        "orthogonal cosine similarity = 0");
}

void TestCosineSimilaritySameDirection() {
  Log("=== TestCosineSimilaritySameDirection ===");
  std::vector<float> a = {3.0f, 4.0f};
  std::vector<float> b = {6.0f, 8.0f};
  float cos_sim = VectorMath::CosineSimilarityNormalized(a, b);
  Check(Approx(cos_sim, 1.0f), "parallel vectors cosine similarity = 1");
}

void TestCosineSimilarityOpposite() {
  Log("=== TestCosineSimilarityOpposite ===");
  std::vector<float> a = {1.0f, 0.0f};
  std::vector<float> b = {-1.0f, 0.0f};
  float cos_sim = VectorMath::CosineSimilarityNormalized(a, b);
  Check(Approx(cos_sim, -1.0f), "opposite vectors cosine similarity = -1");
}

// ============================================================
// 4. Euclidean Distance
// ============================================================

void TestSquaredEuclideanDistance() {
  Log("=== TestSquaredEuclideanDistance ===");
  std::vector<float> a = {1.0f, 2.0f, 3.0f};
  std::vector<float> b = {4.0f, 6.0f, 3.0f};
  float result = VectorMath::SquaredEuclideanDistance(a, b);
  // (4-1)^2 + (6-2)^2 + (3-3)^2 = 9 + 16 + 0 = 25
  Check(Approx(result, 25.0f), "squared euclidean distance = 25");
}

void TestEuclideanDistance() {
  Log("=== TestEuclideanDistance ===");
  std::vector<float> a = {1.0f, 2.0f, 3.0f};
  std::vector<float> b = {4.0f, 6.0f, 3.0f};
  Check(Approx(VectorMath::EuclideanDistance(a, b), 5.0f), "euclidean distance = 5");
}

void TestEuclideanDistanceSame() {
  Log("=== TestEuclideanDistanceSame ===");
  std::vector<float> a = {1.0f, 2.0f};
  Check(Approx(VectorMath::EuclideanDistance(a, a), 0.0f), "distance to self = 0");
}

// ============================================================
// 5. Magnitude
// ============================================================

void TestMagnitude() {
  Log("=== TestMagnitude ===");
  std::vector<float> v = {3.0f, 4.0f};
  Check(Approx(VectorMath::Magnitude(v), 5.0f), "magnitude of (3,4) = 5");
}

void TestMagnitudeEmpty() {
  Log("=== TestMagnitudeEmpty ===");
  Check(Approx(VectorMath::Magnitude({}), 0.0f), "empty magnitude = 0");
}

void TestIsNormalizedL2() {
  Log("=== TestIsNormalizedL2 ===");
  std::vector<float> v = {0.6f, 0.8f};
  Check(VectorMath::IsNormalizedL2(v), "unit vector is normalized");
  Check(!VectorMath::IsNormalizedL2({3.0f, 4.0f}), "non-unit vector is not normalized");
  Check(!VectorMath::IsNormalizedL2({}), "empty vector is not normalized");
}

// ============================================================
// 6. Element-wise Operations
// ============================================================

void TestAdd() {
  Log("=== TestAdd ===");
  std::vector<float> a = {1.0f, 2.0f, 3.0f};
  std::vector<float> b = {4.0f, 5.0f, 6.0f};
  auto result = VectorMath::Add(a, b);
  Check(result.size() == 3, "size");
  Check(Approx(result[0], 5.0f) && Approx(result[1], 7.0f) && Approx(result[2], 9.0f),
        "element-wise addition");
}

void TestSubtract() {
  Log("=== TestSubtract ===");
  std::vector<float> a = {5.0f, 7.0f, 9.0f};
  std::vector<float> b = {4.0f, 5.0f, 6.0f};
  auto result = VectorMath::Subtract(a, b);
  Check(Approx(result[0], 1.0f) && Approx(result[1], 2.0f) && Approx(result[2], 3.0f),
        "element-wise subtraction");
}

void TestScale() {
  Log("=== TestScale ===");
  std::vector<float> v = {1.0f, 2.0f, 3.0f};
  auto result = VectorMath::Scale(v, 2.5f);
  Check(Approx(result[0], 2.5f) && Approx(result[1], 5.0f) && Approx(result[2], 7.5f),
        "scalar multiplication");
}

void TestScaleEmpty() {
  Log("=== TestScaleEmpty ===");
  auto result = VectorMath::Scale({}, 2.0f);
  Check(result.empty(), "empty scale returns empty");
}

// ============================================================
// 7. Large vector (auto-vectorization smoke test)
// ============================================================

void TestLargeVectorDotProduct() {
  Log("=== TestLargeVectorDotProduct ===");
  const int dims = 384;  // Typical MiniLM embedding dimension.
  std::vector<float> a(static_cast<std::size_t>(dims), 1.0f);
  std::vector<float> b(static_cast<std::size_t>(dims), 1.0f);
  Check(Approx(VectorMath::DotProduct(a, b), static_cast<float>(dims), 0.01f),
        "384-dim all-ones dot product = 384");
}

void TestLargeVectorNormalize() {
  Log("=== TestLargeVectorNormalize ===");
  const int dims = 1536;  // Typical OpenAI embedding dimension.
  std::vector<float> v(static_cast<std::size_t>(dims));
  for (int i = 0; i < dims; ++i) {
    v[static_cast<std::size_t>(i)] = static_cast<float>(i + 1);
  }
  auto result = VectorMath::NormalizeL2(v);
  Check(VectorMath::IsNormalizedL2(result), "normalized 1536-dim vector has unit length");
}

}  // namespace

int main() {
  Log("== VectorMath Tests ==");

  TestNormalizeL2();
  TestNormalizeL2Empty();
  TestNormalizeL2Zero();
  TestNormalizeL2InPlace();
  TestDotProduct();
  TestDotProductEmpty();
  TestDotProductOrthogonal();
  TestCosineSimilarityNormalized();
  TestCosineSimilaritySameDirection();
  TestCosineSimilarityOpposite();
  TestSquaredEuclideanDistance();
  TestEuclideanDistance();
  TestEuclideanDistanceSame();
  TestMagnitude();
  TestMagnitudeEmpty();
  TestIsNormalizedL2();
  TestAdd();
  TestSubtract();
  TestScale();
  TestScaleEmpty();
  TestLargeVectorDotProduct();
  TestLargeVectorNormalize();

  std::cout << "\n== Results: " << g_pass << " passed, " << g_fail << " failed ==\n";
  return g_fail > 0 ? 1 : 0;
}
