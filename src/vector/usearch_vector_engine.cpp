#include "waxcpp/vector_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace waxcpp {
namespace {

std::atomic<std::uint32_t> g_test_commit_fail_countdown{0};
std::atomic<std::uint32_t> g_test_commit_fail_on_call{0};
std::atomic<std::uint32_t> g_test_commit_call_index{0};

float Dot(std::span<const float> lhs, std::span<const float> rhs) {
  float dot = 0.0F;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    dot += lhs[i] * rhs[i];
  }
  return dot;
}

float L2SquaredDistance(std::span<const float> lhs, std::span<const float> rhs) {
  float sum = 0.0F;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const float delta = lhs[i] - rhs[i];
    sum += delta * delta;
  }
  return sum;
}

float Norm(std::span<const float> v) {
  const auto dot = Dot(v, v);
  return std::sqrt(std::max(dot, 0.0F));
}

float CosineSimilarity(std::span<const float> lhs, std::span<const float> rhs) {
  const auto lhs_norm = Norm(lhs);
  const auto rhs_norm = Norm(rhs);
  if (lhs_norm <= 0.0F || rhs_norm <= 0.0F) {
    return 0.0F;
  }
  return Dot(lhs, rhs) / (lhs_norm * rhs_norm);
}

void MaybeInjectCommitFailure() {
  const auto call_index = g_test_commit_call_index.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto fail_on_call = g_test_commit_fail_on_call.load(std::memory_order_relaxed);
  if (fail_on_call > 0 && call_index == fail_on_call) {
    throw std::runtime_error("USearchVectorEngine::CommitStaged injected failure");
  }

  auto remaining = g_test_commit_fail_countdown.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (g_test_commit_fail_countdown.compare_exchange_weak(remaining,
                                                           remaining - 1,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
      throw std::runtime_error("USearchVectorEngine::CommitStaged injected failure");
    }
  }
}

}  // namespace

USearchVectorEngine::USearchVectorEngine(int dimensions, VecSimilarity similarity)
    : dimensions_(dimensions), similarity_(similarity) {
  if (dimensions_ <= 0) {
    throw std::runtime_error("USearchVectorEngine dimensions must be positive");
  }
  if (similarity_ != VecSimilarity::kCosine &&
      similarity_ != VecSimilarity::kDot &&
      similarity_ != VecSimilarity::kL2) {
    throw std::runtime_error("USearchVectorEngine similarity must be one of cosine/dot/l2");
  }
}

int USearchVectorEngine::dimensions() const {
  return dimensions_;
}

VecSimilarity USearchVectorEngine::similarity() const {
  return similarity_;
}

std::vector<std::pair<std::uint64_t, float>> USearchVectorEngine::Search(const std::vector<float>& vector,
                                                                          int top_k) const {
  if (vector.size() != static_cast<std::size_t>(dimensions_)) {
    throw std::runtime_error("USearchVectorEngine::Search dimension mismatch");
  }
  if (top_k <= 0 || vectors_.empty()) {
    return {};
  }

  std::vector<std::pair<std::uint64_t, float>> results{};
  results.reserve(vectors_.size());
  for (const auto& [frame_id, candidate] : vectors_) {
    const auto query = std::span<const float>(vector.data(), vector.size());
    const auto doc = std::span<const float>(candidate.data(), candidate.size());
    float score = 0.0F;
    switch (similarity_) {
      case VecSimilarity::kCosine:
        score = CosineSimilarity(query, doc);
        break;
      case VecSimilarity::kDot:
        score = Dot(query, doc);
        break;
      case VecSimilarity::kL2:
        // Swift parity: score is negative distance; higher is better.
        score = -L2SquaredDistance(query, doc);
        break;
    }
    results.emplace_back(frame_id, score);
  }

  std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second > rhs.second;
    }
    return lhs.first < rhs.first;
  });

  const auto target_size = std::min<std::size_t>(results.size(), static_cast<std::size_t>(top_k));
  results.resize(target_size);
  return results;
}

void USearchVectorEngine::StageAdd(std::uint64_t frame_id, const std::vector<float>& vector) {
  if (vector.size() != static_cast<std::size_t>(dimensions_)) {
    throw std::runtime_error("USearchVectorEngine::StageAdd dimension mismatch");
  }
  pending_mutations_.push_back(PendingMutation{PendingMutationType::kAdd, frame_id, vector});
}

void USearchVectorEngine::StageAddBatch(const std::vector<std::uint64_t>& frame_ids,
                                        const std::vector<std::vector<float>>& vectors) {
  if (frame_ids.size() != vectors.size()) {
    throw std::runtime_error("USearchVectorEngine::StageAddBatch size mismatch");
  }
  pending_mutations_.reserve(pending_mutations_.size() + frame_ids.size());
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    StageAdd(frame_ids[i], vectors[i]);
  }
}

void USearchVectorEngine::StageRemove(std::uint64_t frame_id) {
  pending_mutations_.push_back(PendingMutation{PendingMutationType::kRemove, frame_id, {}});
}

void USearchVectorEngine::CommitStaged() {
  MaybeInjectCommitFailure();
  for (auto& mutation : pending_mutations_) {
    if (mutation.type == PendingMutationType::kAdd) {
      vectors_[mutation.frame_id] = std::move(mutation.vector);
      continue;
    }
    vectors_.erase(mutation.frame_id);
  }
  pending_mutations_.clear();
}

void USearchVectorEngine::RollbackStaged() {
  pending_mutations_.clear();
}

std::size_t USearchVectorEngine::PendingMutationCount() const {
  return pending_mutations_.size();
}

void USearchVectorEngine::Add(std::uint64_t frame_id, const std::vector<float>& vector) {
  StageAdd(frame_id, vector);
  CommitStaged();
}

void USearchVectorEngine::AddBatch(const std::vector<std::uint64_t>& frame_ids,
                                   const std::vector<std::vector<float>>& vectors) {
  StageAddBatch(frame_ids, vectors);
  CommitStaged();
}

void USearchVectorEngine::Remove(std::uint64_t frame_id) {
  StageRemove(frame_id);
  CommitStaged();
}

std::vector<std::byte> USearchVectorEngine::SerializeMetalSegment() const {
  std::vector<std::uint64_t> frame_ids{};
  frame_ids.reserve(vectors_.size());
  for (const auto& [frame_id, _] : vectors_) {
    frame_ids.push_back(frame_id);
  }
  std::sort(frame_ids.begin(), frame_ids.end());

  std::vector<float> vectors{};
  vectors.reserve(frame_ids.size() * static_cast<std::size_t>(dimensions_));
  for (const auto frame_id : frame_ids) {
    const auto it = vectors_.find(frame_id);
    if (it == vectors_.end()) {
      throw std::runtime_error("USearchVectorEngine::SerializeMetalSegment missing frame");
    }
    vectors.insert(vectors.end(), it->second.begin(), it->second.end());
  }

  VecSegmentInfo info{};
  info.similarity = similarity_;
  info.dimension = static_cast<std::uint32_t>(dimensions_);
  info.vector_count = static_cast<std::uint64_t>(frame_ids.size());
  info.payload_length =
      static_cast<std::uint64_t>(vectors.size()) * static_cast<std::uint64_t>(sizeof(float));
  return EncodeMetalVecSegment(info, vectors, frame_ids);
}

void USearchVectorEngine::LoadMetalSegment(std::span<const std::byte> segment_bytes) {
  const auto decoded = DecodeVecSegment(segment_bytes);
  if (!std::holds_alternative<VecMetalPayload>(decoded)) {
    throw std::runtime_error("USearchVectorEngine::LoadMetalSegment requires metal encoding");
  }
  const auto& metal = std::get<VecMetalPayload>(decoded);
  if (metal.info.dimension != static_cast<std::uint32_t>(dimensions_)) {
    throw std::runtime_error("USearchVectorEngine::LoadMetalSegment dimension mismatch");
  }
  if (metal.info.similarity != similarity_) {
    throw std::runtime_error("USearchVectorEngine::LoadMetalSegment similarity mismatch");
  }

  vectors_.clear();
  pending_mutations_.clear();
  for (std::size_t i = 0; i < metal.frame_ids.size(); ++i) {
    const auto begin = i * static_cast<std::size_t>(dimensions_);
    const auto end = begin + static_cast<std::size_t>(dimensions_);
    vectors_.emplace(metal.frame_ids[i], std::vector<float>(metal.vectors.begin() + begin, metal.vectors.begin() + end));
  }
}

namespace vector::testing {

void SetCommitFailCountdown(std::uint32_t countdown) {
  g_test_commit_fail_countdown.store(countdown, std::memory_order_relaxed);
  g_test_commit_fail_on_call.store(0, std::memory_order_relaxed);
  g_test_commit_call_index.store(0, std::memory_order_relaxed);
}

void ClearCommitFailCountdown() {
  g_test_commit_fail_countdown.store(0, std::memory_order_relaxed);
  g_test_commit_call_index.store(0, std::memory_order_relaxed);
}

void SetCommitFailOnCall(std::uint32_t call_index) {
  g_test_commit_fail_countdown.store(0, std::memory_order_relaxed);
  g_test_commit_fail_on_call.store(call_index, std::memory_order_relaxed);
  g_test_commit_call_index.store(0, std::memory_order_relaxed);
}

void ClearCommitFailOnCall() {
  g_test_commit_fail_on_call.store(0, std::memory_order_relaxed);
  g_test_commit_call_index.store(0, std::memory_order_relaxed);
}

}  // namespace vector::testing

}  // namespace waxcpp
