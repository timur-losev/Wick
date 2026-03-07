#pragma once

#include "waxcpp/mv2v_format.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace waxcpp {

class VectorSearchEngine {
 public:
  virtual ~VectorSearchEngine() = default;

  virtual int dimensions() const = 0;
  virtual std::vector<std::pair<std::uint64_t, float>> Search(const std::vector<float>& vector, int top_k) const = 0;
  virtual void StageAdd(std::uint64_t frame_id, const std::vector<float>& vector) = 0;
  virtual void StageAddBatch(const std::vector<std::uint64_t>& frame_ids,
                             const std::vector<std::vector<float>>& vectors) = 0;
  virtual void StageRemove(std::uint64_t frame_id) = 0;
  virtual void CommitStaged() = 0;
  virtual void RollbackStaged() = 0;
  virtual std::size_t PendingMutationCount() const = 0;
  virtual void Add(std::uint64_t frame_id, const std::vector<float>& vector) = 0;
  virtual void AddBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::vector<float>>& vectors) = 0;
  virtual void Remove(std::uint64_t frame_id) = 0;
};

class USearchVectorEngine final : public VectorSearchEngine {
 public:
  explicit USearchVectorEngine(int dimensions, VecSimilarity similarity = VecSimilarity::kCosine);

  int dimensions() const override;
  [[nodiscard]] VecSimilarity similarity() const;
  std::vector<std::pair<std::uint64_t, float>> Search(const std::vector<float>& vector, int top_k) const override;
  void StageAdd(std::uint64_t frame_id, const std::vector<float>& vector) override;
  void StageAddBatch(const std::vector<std::uint64_t>& frame_ids,
                     const std::vector<std::vector<float>>& vectors) override;
  void StageRemove(std::uint64_t frame_id) override;
  void CommitStaged() override;
  void RollbackStaged() override;
  std::size_t PendingMutationCount() const override;
  void Add(std::uint64_t frame_id, const std::vector<float>& vector) override;
  void AddBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::vector<float>>& vectors) override;
  void Remove(std::uint64_t frame_id) override;
  [[nodiscard]] std::vector<std::byte> SerializeMetalSegment() const;
  void LoadMetalSegment(std::span<const std::byte> segment_bytes);

 private:
  enum class PendingMutationType {
    kAdd,
    kRemove,
  };
  struct PendingMutation {
    PendingMutationType type = PendingMutationType::kAdd;
    std::uint64_t frame_id = 0;
    std::vector<float> vector{};
  };

  int dimensions_;
  VecSimilarity similarity_ = VecSimilarity::kCosine;
  std::unordered_map<std::uint64_t, std::vector<float>> vectors_;
  std::vector<PendingMutation> pending_mutations_;
};

namespace vector::testing {

void SetCommitFailCountdown(std::uint32_t countdown);
void ClearCommitFailCountdown();
void SetCommitFailOnCall(std::uint32_t call_index);
void ClearCommitFailOnCall();

}  // namespace vector::testing

}  // namespace waxcpp
