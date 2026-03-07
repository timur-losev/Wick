#include "waxcpp/fts5_search_engine.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ScenarioBasicRanking() {
  waxcpp::tests::Log("scenario: basic ranking");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(10, "apple banana");
  engine.Index(11, "apple apple");
  engine.Index(12, "banana");

  const auto results = engine.Search("apple banana", 10);
  Require(results.size() == 3, "expected 3 search results");
  Require(results[0].frame_id == 10, "frame 10 should rank first for mixed query");
  Require(results[1].frame_id == 11, "frame 11 should rank second in mixed query");
  Require(results[0].score >= results[1].score, "top score must be greater or equal");
  Require(results[0].preview_text.has_value(), "preview text must be present");
}

void ScenarioCaseInsensitiveTokenization() {
  waxcpp::tests::Log("scenario: case-insensitive tokenization");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(1, "Hello, WORLD!!!");

  const auto results = engine.Search("world", 5);
  Require(results.size() == 1, "expected one result for case-insensitive match");
  Require(results[0].frame_id == 1, "matched frame_id mismatch");
}

void ScenarioAsciiTokenizerDeterminism() {
  waxcpp::tests::Log("scenario: ASCII tokenizer determinism");
  waxcpp::FTS5SearchEngine engine;

  std::string non_ascii_separator_doc{"alpha"};
  non_ascii_separator_doc.push_back(static_cast<char>(0xC0));
  non_ascii_separator_doc.append("BETA");
  engine.Index(20, non_ascii_separator_doc);

  const auto alpha_results = engine.Search("alpha", 10);
  Require(!alpha_results.empty(), "alpha should be tokenized before non-ASCII separator byte");
  Require(alpha_results[0].frame_id == 20, "alpha lookup returned unexpected frame_id");

  const auto beta_results = engine.Search("beta", 10);
  Require(!beta_results.empty(), "beta should be tokenized after non-ASCII separator byte");
  Require(beta_results[0].frame_id == 20, "beta lookup returned unexpected frame_id");
}

void ScenarioDeterministicTieBreak() {
  waxcpp::tests::Log("scenario: deterministic tie-break by frame_id");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(42, "equal score token");
  engine.Index(7, "equal score token");

  const auto results = engine.Search("token", 10);
  Require(results.size() == 2, "expected two results in tie-break scenario");
  Require(results[0].score == results[1].score, "scores should be equal in tie-break scenario");
  Require(results[0].frame_id == 7, "lower frame_id must win tie-break");
  Require(results[1].frame_id == 42, "higher frame_id must come second on tie");
}

void ScenarioRemoveAndTopK() {
  waxcpp::tests::Log("scenario: remove and top_k clamp");
  waxcpp::FTS5SearchEngine engine;
  engine.IndexBatch({1, 2, 3}, {"alpha beta", "alpha", "beta"});
  engine.Remove(2);

  const auto results = engine.Search("alpha beta", 1);
  Require(results.size() == 1, "top_k=1 must clamp output to single result");
  Require(results[0].frame_id != 2, "removed frame must not appear in results");
}

void ScenarioIndexBatchMismatchThrows() {
  waxcpp::tests::Log("scenario: batch size mismatch throws");
  waxcpp::FTS5SearchEngine engine;
  bool threw = false;
  try {
    engine.IndexBatch({1, 2}, {"only-one"});
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "IndexBatch must throw on size mismatch");
}

void ScenarioEmptyInputs() {
  waxcpp::tests::Log("scenario: empty query/top_k");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(1, "content");

  Require(engine.Search("", 10).empty(), "empty query must return no results");
  Require(engine.Search("content", 0).empty(), "top_k=0 must return no results");
  Require(engine.Search("content", -1).empty(), "negative top_k must return no results");
}

void ScenarioStagedMutationsRequireCommit() {
  waxcpp::tests::Log("scenario: staged mutations require commit");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndex(1, "alpha staged");
  Require(engine.PendingMutationCount() == 1, "pending mutation count mismatch after stage index");
  Require(engine.Search("alpha", 10).empty(), "staged index should not be visible before commit");

  engine.CommitStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after commit");
  Require(engine.Search("alpha", 10).size() == 1, "committed staged index should be searchable");

  engine.StageRemove(1);
  Require(engine.Search("alpha", 10).size() == 1, "staged remove should not be visible before commit");
  engine.CommitStaged();
  Require(engine.Search("alpha", 10).empty(), "committed staged remove should clear document");
}

void ScenarioRollbackStagedMutations() {
  waxcpp::tests::Log("scenario: rollback staged mutations");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndexBatch({10, 11}, {"alpha", "beta"});
  Require(engine.PendingMutationCount() == 2, "pending mutation count mismatch after stage batch");
  engine.RollbackStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after rollback");
  Require(engine.Search("alpha", 10).empty(), "rolled-back staged index should not be visible");
}

void ScenarioStagedOrderDeterminism() {
  waxcpp::tests::Log("scenario: staged order determinism");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndex(7, "old");
  engine.StageIndex(7, "new");
  engine.StageRemove(7);
  engine.StageIndex(7, "final");
  Require(engine.PendingMutationCount() == 4, "pending mutation count mismatch in order scenario");
  engine.CommitStaged();

  const auto results = engine.Search("final", 10);
  Require(results.size() == 1, "final staged state should leave one searchable document");
  Require(results[0].frame_id == 7, "unexpected frame_id after staged mutation order apply");
}

void ScenarioInjectedCommitFailurePreservesPendingState() {
  waxcpp::tests::Log("scenario: injected commit failure preserves pending state");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndex(1, "alpha");
  engine.StageIndex(2, "beta");
  Require(engine.PendingMutationCount() == 2, "pending count mismatch before injected failure");

  waxcpp::text::testing::SetCommitFailCountdown(1);
  bool threw = false;
  try {
    engine.CommitStaged();
  } catch (const std::exception&) {
    threw = true;
  }
  waxcpp::text::testing::ClearCommitFailCountdown();
  Require(threw, "CommitStaged should throw when failure injection is active");
  Require(engine.PendingMutationCount() == 2, "failed commit must keep pending mutations for retry");
  Require(engine.Search("alpha", 10).empty(), "failed commit must not publish staged document");

  engine.CommitStaged();
  Require(engine.PendingMutationCount() == 0, "retry commit should clear pending mutations");
  const auto results = engine.Search("alpha", 10);
  Require(results.size() == 1, "successful retry commit should publish staged document");
  Require(results[0].frame_id == 1, "unexpected frame_id after retry commit");
}

void ScenarioInjectedCommitFailureOnSecondCall() {
  waxcpp::tests::Log("scenario: injected commit failure on second call");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndex(1, "alpha");
  engine.StageIndex(2, "beta");

  waxcpp::text::testing::SetCommitFailOnCall(2);
  bool first_threw = false;
  try {
    engine.CommitStaged();
  } catch (const std::exception&) {
    first_threw = true;
  }
  Require(!first_threw, "first CommitStaged should succeed when fail-on-call is set to second call");
  Require(engine.PendingMutationCount() == 0, "first successful commit should clear pending state");

  engine.StageIndex(3, "gamma");
  bool second_threw = false;
  try {
    engine.CommitStaged();
  } catch (const std::exception&) {
    second_threw = true;
  }
  waxcpp::text::testing::ClearCommitFailOnCall();
  Require(second_threw, "second CommitStaged should fail under fail-on-call hook");
  Require(engine.PendingMutationCount() == 1, "failed second commit should preserve pending staged mutation");
  Require(engine.Search("gamma", 10).empty(), "failed second commit must not publish staged document");
}

void ScenarioMoveSemanticsPreserveIndexState() {
  waxcpp::tests::Log("scenario: move semantics preserve index state");
  waxcpp::FTS5SearchEngine source;
  source.Index(101, "move alpha");
  source.Index(102, "move beta");

  waxcpp::FTS5SearchEngine moved = std::move(source);
  const auto moved_results = moved.Search("alpha", 10);
  Require(moved_results.size() == 1, "moved engine should preserve indexed documents");
  Require(moved_results[0].frame_id == 101, "moved engine returned unexpected frame_id");

  waxcpp::FTS5SearchEngine reassigned;
  reassigned = std::move(moved);
  const auto reassigned_results = reassigned.Search("beta", 10);
  Require(reassigned_results.size() == 1, "move-assigned engine should preserve indexed documents");
  Require(reassigned_results[0].frame_id == 102, "move-assigned engine returned unexpected frame_id");
}

void ScenarioSeededFuzzDeterminismAndInvariants() {
  waxcpp::tests::Log("scenario: seeded fuzz determinism and invariants");

  auto scores_equal = [](float lhs, float rhs) -> bool { return std::fabs(lhs - rhs) < 1e-6F; };

  auto require_same_results = [&](const std::vector<waxcpp::SearchResult>& lhs,
                                  const std::vector<waxcpp::SearchResult>& rhs,
                                  const std::string& where) {
    Require(lhs.size() == rhs.size(), where + ": result size mismatch");
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      Require(lhs[i].frame_id == rhs[i].frame_id, where + ": frame_id mismatch");
      Require(scores_equal(lhs[i].score, rhs[i].score), where + ": score mismatch");
      Require(lhs[i].preview_text == rhs[i].preview_text, where + ": preview mismatch");
      Require(lhs[i].sources == rhs[i].sources, where + ": sources mismatch");
    }
  };

  std::mt19937 rng(0x51F5A11U);
  std::uniform_int_distribution<int> doc_count_dist(1, 32);
  std::uniform_int_distribution<int> token_count_dist(1, 8);
  std::uniform_int_distribution<int> token_len_dist(1, 7);
  std::uniform_int_distribution<int> letter_dist(0, 25);
  std::uniform_int_distribution<int> sep_dist(0, 5);
  std::uniform_int_distribution<int> bool_dist(0, 1);
  std::uniform_int_distribution<int> topk_dist(1, 16);
  std::uniform_int_distribution<int> pick_doc_dist(0, 31);
  std::uniform_int_distribution<int> pick_token_dist(0, 7);

  auto random_token = [&]() -> std::string {
    const int len = token_len_dist(rng);
    std::string out{};
    out.reserve(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) {
      char ch = static_cast<char>('a' + letter_dist(rng));
      if (bool_dist(rng) != 0) {
        ch = static_cast<char>(ch - ('a' - 'A'));
      }
      out.push_back(ch);
    }
    return out;
  };

  auto random_separator = [&]() -> std::string {
    switch (sep_dist(rng)) {
      case 0:
        return " ";
      case 1:
        return "-";
      case 2:
        return ".";
      case 3:
        return "/";
      case 4:
        return "_";
      default: {
        std::string non_ascii{};
        non_ascii.push_back(static_cast<char>(0xC0));
        return non_ascii;
      }
    }
  };

  constexpr std::size_t kIterations = 256;
  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    const int doc_count = doc_count_dist(rng);
    std::vector<std::uint64_t> frame_ids(static_cast<std::size_t>(doc_count));
    for (int i = 0; i < doc_count; ++i) {
      frame_ids[static_cast<std::size_t>(i)] = static_cast<std::uint64_t>(1000 + i);
    }

    std::vector<std::vector<std::string>> doc_tokens{};
    std::vector<std::pair<std::uint64_t, std::string>> docs{};
    doc_tokens.reserve(static_cast<std::size_t>(doc_count));
    docs.reserve(static_cast<std::size_t>(doc_count));

    for (int i = 0; i < doc_count; ++i) {
      const int token_count = token_count_dist(rng);
      std::vector<std::string> tokens{};
      tokens.reserve(static_cast<std::size_t>(token_count));
      std::string text{};
      for (int t = 0; t < token_count; ++t) {
        const auto token = random_token();
        tokens.push_back(token);
        if (t > 0) {
          text.append(random_separator());
        }
        text.append(token);
      }
      doc_tokens.push_back(tokens);
      docs.emplace_back(frame_ids[static_cast<std::size_t>(i)], text);
    }

    waxcpp::FTS5SearchEngine engine_forward;
    for (const auto& [frame_id, text] : docs) {
      engine_forward.Index(frame_id, text);
    }

    auto docs_reversed = docs;
    std::reverse(docs_reversed.begin(), docs_reversed.end());
    waxcpp::FTS5SearchEngine engine_reversed;
    for (const auto& [frame_id, text] : docs_reversed) {
      engine_reversed.Index(frame_id, text);
    }

    const int query_doc_index = pick_doc_dist(rng) % doc_count;
    const auto& query_tokens = doc_tokens[static_cast<std::size_t>(query_doc_index)];
    const int query_token_count = std::max(1, static_cast<int>(query_tokens.size() / 2));
    std::string query{};
    for (int i = 0; i < query_token_count; ++i) {
      if (!query.empty()) {
        query.push_back(' ');
      }
      query.append(query_tokens[static_cast<std::size_t>(pick_token_dist(rng) % query_tokens.size())]);
    }

    const int top_k = std::min(topk_dist(rng), doc_count + 4);
    const auto forward = engine_forward.Search(query, top_k);
    const auto reversed = engine_reversed.Search(query, top_k);
    require_same_results(forward, reversed, "seeded-fuzz permutation invariance");
    Require(forward.size() <= static_cast<std::size_t>(top_k), "top_k clamp violated");

    std::unordered_map<std::uint64_t, std::string> expected_preview{};
    for (const auto& [frame_id, text] : docs) {
      expected_preview.emplace(frame_id, text);
    }

    for (std::size_t i = 0; i < forward.size(); ++i) {
      const auto& result = forward[i];
      Require(result.preview_text.has_value(), "result preview should always be present");
      const auto expected_it = expected_preview.find(result.frame_id);
      Require(expected_it != expected_preview.end(), "result frame_id not found in corpus");
      Require(*result.preview_text == expected_it->second, "preview text mismatch against indexed corpus");
      if (i == 0) {
        continue;
      }
      const auto& previous = forward[i - 1];
      Require(previous.score >= result.score, "result ordering must be score-desc");
      if (scores_equal(previous.score, result.score)) {
        Require(previous.frame_id < result.frame_id, "equal-score ordering must use frame_id asc tie-break");
      }
    }
  }
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("fts5_search_engine_test: start");
    ScenarioBasicRanking();
    ScenarioCaseInsensitiveTokenization();
    ScenarioAsciiTokenizerDeterminism();
    ScenarioDeterministicTieBreak();
    ScenarioRemoveAndTopK();
    ScenarioIndexBatchMismatchThrows();
    ScenarioEmptyInputs();
    ScenarioStagedMutationsRequireCommit();
    ScenarioRollbackStagedMutations();
    ScenarioStagedOrderDeterminism();
    ScenarioInjectedCommitFailurePreservesPendingState();
    ScenarioInjectedCommitFailureOnSecondCall();
    ScenarioMoveSemanticsPreserveIndexState();
    ScenarioSeededFuzzDeterminismAndInvariants();
    waxcpp::tests::Log("fts5_search_engine_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
