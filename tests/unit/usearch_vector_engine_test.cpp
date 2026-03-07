#include "waxcpp/vector_engine.hpp"
#include "waxcpp/mv2v_format.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ScenarioCtorValidation() {
  waxcpp::tests::Log("scenario: constructor validation");
  bool threw = false;
  try {
    waxcpp::USearchVectorEngine invalid(0);
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "dimensions <= 0 must throw");
}

void ScenarioAddSearchAndTieBreak() {
  waxcpp::tests::Log("scenario: add/search/tie-break");
  waxcpp::USearchVectorEngine engine(3);
  engine.Add(10, {1.0F, 0.0F, 0.0F});
  engine.Add(2, {1.0F, 0.0F, 0.0F});
  engine.Add(7, {0.0F, 1.0F, 0.0F});

  const auto results = engine.Search({1.0F, 0.0F, 0.0F}, 3);
  Require(results.size() == 3, "expected three vector results");
  Require(results[0].first == 2, "tie-break should prefer lower frame_id");
  Require(results[1].first == 10, "second tie entry should be higher frame_id");
  Require(results[0].second == results[1].second, "tie entries should have equal score");
  Require(results[2].first == 7, "orthogonal vector should rank lower");
}

void ScenarioMetricScoringParity() {
  waxcpp::tests::Log("scenario: metric scoring parity");
  {
    waxcpp::USearchVectorEngine dot_engine(2, waxcpp::VecSimilarity::kDot);
    dot_engine.Add(10, {2.0F, 0.0F});
    dot_engine.Add(2, {1.0F, 1.0F});
    dot_engine.Add(7, {0.0F, 1.0F});
    const auto results = dot_engine.Search({1.0F, 1.0F}, 3);
    Require(results.size() == 3, "dot metric result count mismatch");
    Require(results[0].first == 2, "dot metric tie-break should prefer lower frame_id");
    Require(results[1].first == 10, "dot metric second tie result mismatch");
    Require(std::fabs(results[0].second - results[1].second) <= 1e-6F,
            "dot metric tie scores should match");
  }

  {
    waxcpp::USearchVectorEngine l2_engine(2, waxcpp::VecSimilarity::kL2);
    l2_engine.Add(1, {1.0F, 0.0F});
    l2_engine.Add(2, {2.0F, 0.0F});
    l2_engine.Add(3, {3.0F, 0.0F});
    const auto results = l2_engine.Search({1.0F, 0.0F}, 3);
    Require(results.size() == 3, "l2 metric result count mismatch");
    Require(results[0].first == 1, "l2 metric nearest vector should rank first");
    Require(results[0].second >= results[1].second && results[1].second >= results[2].second,
            "l2 metric scores should be monotonic descending");
  }
}

void ScenarioBatchAndRemove() {
  waxcpp::tests::Log("scenario: batch add and remove");
  waxcpp::USearchVectorEngine engine(2);
  engine.AddBatch({1, 2}, {{1.0F, 0.0F}, {0.0F, 1.0F}});
  engine.Remove(2);

  const auto results = engine.Search({0.0F, 1.0F}, 10);
  Require(results.size() == 1, "remove should drop frame from index");
  Require(results[0].first == 1, "remaining frame mismatch");
}

void ScenarioValidationErrors() {
  waxcpp::tests::Log("scenario: validation errors");
  waxcpp::USearchVectorEngine engine(2);

  bool add_threw = false;
  try {
    engine.Add(1, {1.0F});
  } catch (const std::exception&) {
    add_threw = true;
  }
  Require(add_threw, "Add must throw on dimension mismatch");

  bool batch_size_threw = false;
  try {
    engine.AddBatch({1, 2}, {{1.0F, 0.0F}});
  } catch (const std::exception&) {
    batch_size_threw = true;
  }
  Require(batch_size_threw, "AddBatch must throw on size mismatch");

  bool search_threw = false;
  try {
    (void)engine.Search({1.0F}, 10);
  } catch (const std::exception&) {
    search_threw = true;
  }
  Require(search_threw, "Search must throw on dimension mismatch");
}

void ScenarioTopKAndEmptyCases() {
  waxcpp::tests::Log("scenario: top_k and empty cases");
  waxcpp::USearchVectorEngine engine(2);
  engine.Add(1, {1.0F, 0.0F});
  engine.Add(2, {0.0F, 1.0F});

  Require(engine.Search({1.0F, 0.0F}, 0).empty(), "top_k=0 must return empty");
  Require(engine.Search({1.0F, 0.0F}, -1).empty(), "top_k<0 must return empty");

  const auto results = engine.Search({1.0F, 0.0F}, 1);
  Require(results.size() == 1, "top_k clamp must limit result count");
}

void ScenarioStagedMutationsRequireCommit() {
  waxcpp::tests::Log("scenario: staged mutations require commit");
  waxcpp::USearchVectorEngine engine(2);
  engine.StageAdd(1, {1.0F, 0.0F});
  Require(engine.PendingMutationCount() == 1, "pending mutation count mismatch after stage add");
  Require(engine.Search({1.0F, 0.0F}, 10).empty(), "staged vector add should not be visible before commit");

  engine.CommitStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after commit");
  Require(engine.Search({1.0F, 0.0F}, 10).size() == 1, "committed staged vector should be searchable");

  engine.StageRemove(1);
  Require(engine.Search({1.0F, 0.0F}, 10).size() == 1, "staged remove should stay invisible before commit");
  engine.CommitStaged();
  Require(engine.Search({1.0F, 0.0F}, 10).empty(), "committed staged remove should clear vector");
}

void ScenarioRollbackStagedMutations() {
  waxcpp::tests::Log("scenario: rollback staged mutations");
  waxcpp::USearchVectorEngine engine(2);
  engine.StageAddBatch({10, 11}, {{1.0F, 0.0F}, {0.0F, 1.0F}});
  Require(engine.PendingMutationCount() == 2, "pending mutation count mismatch after stage batch");
  engine.RollbackStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after rollback");
  Require(engine.Search({1.0F, 0.0F}, 10).empty(), "rolled-back staged vectors should not be visible");
}

void ScenarioStagedOrderDeterminism() {
  waxcpp::tests::Log("scenario: staged order determinism");
  waxcpp::USearchVectorEngine engine(2);
  engine.StageAdd(7, {1.0F, 0.0F});
  engine.StageAdd(7, {0.0F, 1.0F});
  engine.StageRemove(7);
  engine.StageAdd(7, {1.0F, 0.0F});
  Require(engine.PendingMutationCount() == 4, "pending mutation count mismatch in order scenario");
  engine.CommitStaged();

  const auto results = engine.Search({1.0F, 0.0F}, 10);
  Require(results.size() == 1, "final staged state should leave one vector");
  Require(results[0].first == 7, "unexpected frame_id after staged mutation order apply");
}

void ScenarioCommitFailOnCallHook() {
  waxcpp::tests::Log("scenario: commit fail-on-call hook");
  waxcpp::USearchVectorEngine engine(2);

  engine.StageAdd(10, {1.0F, 0.0F});
  waxcpp::vector::testing::SetCommitFailOnCall(2);
  engine.CommitStaged();  // call #1 succeeds

  engine.StageAdd(11, {0.0F, 1.0F});
  bool threw = false;
  try {
    engine.CommitStaged();  // call #2 fails
  } catch (const std::exception&) {
    threw = true;
  }
  waxcpp::vector::testing::ClearCommitFailOnCall();
  Require(threw, "CommitStaged must fail on configured second call");
  Require(engine.PendingMutationCount() == 1, "failed commit must preserve pending staged mutation");

  const auto after_fail = engine.Search({1.0F, 0.0F}, 10);
  Require(after_fail.size() == 1, "failed second commit must keep only first committed vector");
  Require(after_fail[0].first == 10, "failed second commit should not publish second staged vector");

  engine.CommitStaged();
  Require(engine.PendingMutationCount() == 0, "successful retry commit must clear pending mutations");
  const auto after_retry = engine.Search({0.0F, 1.0F}, 10);
  Require(!after_retry.empty(), "successful retry commit should publish second staged vector");
  Require(after_retry[0].first == 11, "second staged vector should rank first after retry commit");

  engine.StageRemove(10);
  waxcpp::vector::testing::SetCommitFailOnCall(1);
  threw = false;
  try {
    engine.CommitStaged();  // configured first call fails
  } catch (const std::exception&) {
    threw = true;
  }
  waxcpp::vector::testing::ClearCommitFailOnCall();
  Require(threw, "CommitStaged must fail on configured first call");
  Require(engine.PendingMutationCount() == 1, "failed first-call commit must preserve pending remove");

  engine.CommitStaged();
  const auto after_remove = engine.Search({1.0F, 0.0F}, 10);
  Require(!after_remove.empty(), "remaining vector should still be searchable after remove retry");
  Require(after_remove[0].first == 11, "remove retry should drop frame 10 and keep frame 11");
}

void ScenarioMetalSegmentRoundtrip() {
  waxcpp::tests::Log("scenario: metal segment roundtrip");
  waxcpp::USearchVectorEngine source(3);
  source.Add(9, {0.1F, 0.2F, 0.3F});
  source.Add(2, {1.0F, 0.0F, 0.0F});
  source.Add(5, {0.0F, 1.0F, 0.0F});

  const auto segment = source.SerializeMetalSegment();
  waxcpp::USearchVectorEngine loaded(3, waxcpp::VecSimilarity::kCosine);
  loaded.LoadMetalSegment(segment);

  const auto before = source.Search({1.0F, 0.0F, 0.0F}, 10);
  const auto after = loaded.Search({1.0F, 0.0F, 0.0F}, 10);
  Require(before.size() == after.size(), "roundtrip result count mismatch");
  for (std::size_t i = 0; i < before.size(); ++i) {
    Require(before[i].first == after[i].first, "roundtrip frame_id mismatch");
    Require(std::fabs(before[i].second - after[i].second) <= 1e-6F, "roundtrip score mismatch");
  }
}

void ScenarioLoadMetalSegmentValidation() {
  waxcpp::tests::Log("scenario: load metal segment validation");
  waxcpp::USearchVectorEngine engine(2);

  waxcpp::VecSegmentInfo wrong_dim{};
  wrong_dim.similarity = waxcpp::VecSimilarity::kCosine;
  wrong_dim.dimension = 3;
  wrong_dim.vector_count = 1;
  wrong_dim.payload_length = 3 * sizeof(float);
  const std::vector<float> wrong_dim_vectors = {0.1F, 0.2F, 0.3F};
  const std::vector<std::uint64_t> wrong_dim_ids = {11};
  const auto wrong_dim_segment = waxcpp::EncodeMetalVecSegment(wrong_dim, wrong_dim_vectors, wrong_dim_ids);

  bool dim_threw = false;
  try {
    engine.LoadMetalSegment(wrong_dim_segment);
  } catch (const std::exception&) {
    dim_threw = true;
  }
  Require(dim_threw, "LoadMetalSegment must reject dimension mismatch");

  waxcpp::VecSegmentInfo wrong_similarity{};
  wrong_similarity.similarity = waxcpp::VecSimilarity::kDot;
  wrong_similarity.dimension = 2;
  wrong_similarity.vector_count = 1;
  wrong_similarity.payload_length = 2 * sizeof(float);
  const std::vector<float> wrong_similarity_vectors = {0.1F, 0.2F};
  const std::vector<std::uint64_t> wrong_similarity_ids = {15};
  const auto wrong_similarity_segment =
      waxcpp::EncodeMetalVecSegment(wrong_similarity, wrong_similarity_vectors, wrong_similarity_ids);

  bool similarity_threw = false;
  try {
    engine.LoadMetalSegment(wrong_similarity_segment);
  } catch (const std::exception&) {
    similarity_threw = true;
  }
  Require(similarity_threw, "LoadMetalSegment must reject similarity mismatch");

  waxcpp::VecSegmentInfo usearch_info{};
  usearch_info.similarity = waxcpp::VecSimilarity::kCosine;
  usearch_info.dimension = 2;
  usearch_info.vector_count = 1;
  const std::vector<std::byte> payload = {std::byte{0xAA}, std::byte{0xBB}};
  usearch_info.payload_length = payload.size();
  const auto usearch_segment = waxcpp::EncodeUSearchVecSegment(usearch_info, payload);

  bool encoding_threw = false;
  try {
    engine.LoadMetalSegment(usearch_segment);
  } catch (const std::exception&) {
    encoding_threw = true;
  }
  Require(encoding_threw, "LoadMetalSegment must reject non-metal encoding");
}

void ScenarioSeededFuzzDeterminismAndInvariants() {
  waxcpp::tests::Log("scenario: seeded vector fuzz determinism and invariants");

  auto scores_equal = [](float lhs, float rhs) -> bool { return std::fabs(lhs - rhs) < 1e-5F; };
  auto vectors_equal = [&](const std::vector<std::pair<std::uint64_t, float>>& lhs,
                           const std::vector<std::pair<std::uint64_t, float>>& rhs,
                           const std::string& where) {
    Require(lhs.size() == rhs.size(), where + ": size mismatch");
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      Require(lhs[i].first == rhs[i].first, where + ": frame_id mismatch");
      Require(scores_equal(lhs[i].second, rhs[i].second), where + ": score mismatch");
    }
  };

  std::mt19937 rng(0xA117C0DEU);
  std::uniform_int_distribution<int> count_dist(1, 64);
  std::uniform_int_distribution<int> remove_count_dist(0, 12);
  std::uniform_int_distribution<int> topk_dist(1, 32);
  std::uniform_real_distribution<float> value_dist(-1.0F, 1.0F);

  constexpr int kDim = 8;
  constexpr std::size_t kIterations = 256;
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const int count = count_dist(rng);
    std::vector<std::uint64_t> frame_ids{};
    frame_ids.reserve(static_cast<std::size_t>(count));
    std::vector<std::vector<float>> vectors{};
    vectors.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
      frame_ids.push_back(static_cast<std::uint64_t>(5000 + i));
      std::vector<float> vec(static_cast<std::size_t>(kDim), 0.0F);
      for (float& v : vec) {
        v = value_dist(rng);
      }
      vectors.push_back(std::move(vec));
    }

    waxcpp::USearchVectorEngine forward(kDim);
    for (int i = 0; i < count; ++i) {
      forward.Add(frame_ids[static_cast<std::size_t>(i)], vectors[static_cast<std::size_t>(i)]);
    }

    waxcpp::USearchVectorEngine reversed(kDim);
    for (int i = count - 1; i >= 0; --i) {
      reversed.Add(frame_ids[static_cast<std::size_t>(i)], vectors[static_cast<std::size_t>(i)]);
    }

    std::unordered_set<std::uint64_t> removed{};
    const int remove_count = std::min(remove_count_dist(rng), count);
    for (int i = 0; i < remove_count; ++i) {
      const auto remove_index = static_cast<std::size_t>(rng() % static_cast<std::uint32_t>(count));
      const auto frame_id = frame_ids[remove_index];
      removed.insert(frame_id);
      forward.Remove(frame_id);
      reversed.Remove(frame_id);
    }

    std::vector<float> query(static_cast<std::size_t>(kDim), 0.0F);
    for (float& v : query) {
      v = value_dist(rng);
    }

    const int top_k = topk_dist(rng);
    const auto lhs = forward.Search(query, top_k);
    const auto rhs = reversed.Search(query, top_k);
    vectors_equal(lhs, rhs, "seeded vector permutation invariance");
    Require(lhs.size() <= static_cast<std::size_t>(top_k), "top_k clamp violated in vector search");

    for (std::size_t i = 0; i < lhs.size(); ++i) {
      Require(removed.find(lhs[i].first) == removed.end(), "removed frame_id must not appear in results");
      if (i == 0) {
        continue;
      }
      const auto& prev = lhs[i - 1];
      const auto& cur = lhs[i];
      Require(prev.second >= cur.second, "vector results must be sorted by score desc");
      if (scores_equal(prev.second, cur.second)) {
        Require(prev.first < cur.first, "equal-score vector results must tie-break by frame_id asc");
      }
    }
  }
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("usearch_vector_engine_test: start");
    ScenarioCtorValidation();
    ScenarioAddSearchAndTieBreak();
    ScenarioMetricScoringParity();
    ScenarioBatchAndRemove();
    ScenarioValidationErrors();
    ScenarioTopKAndEmptyCases();
    ScenarioStagedMutationsRequireCommit();
    ScenarioRollbackStagedMutations();
    ScenarioStagedOrderDeterminism();
    ScenarioCommitFailOnCallHook();
    ScenarioMetalSegmentRoundtrip();
    ScenarioLoadMetalSegmentValidation();
    ScenarioSeededFuzzDeterminismAndInvariants();
    waxcpp::tests::Log("usearch_vector_engine_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
