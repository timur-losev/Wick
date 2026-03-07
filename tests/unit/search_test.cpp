#include "waxcpp/search.hpp"
#include "waxcpp/query_analyzer.hpp"

#include "../test_logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ScenarioDeterministicOrderingAndTopK() {
  waxcpp::tests::Log("scenario: deterministic ordering and top_k");
  waxcpp::SearchRequest request{};
  request.query = "alpha";
  request.top_k = 2;
  request.preview_max_bytes = 100;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 5, .score = 1.0F, .preview_text = std::string("five"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("two"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 1, .score = 3.0F, .preview_text = std::string("one"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "top_k should clamp context item count");
  Require(context.items[0].frame_id == 1, "highest score should rank first");
  Require(context.items[1].frame_id == 2, "tie should break by lower frame_id");
}

void ScenarioPreviewClampAndTokenCount() {
  waxcpp::tests::Log("scenario: preview clamp and token count");
  waxcpp::SearchRequest request{};
  request.query = "query";
  request.top_k = 10;
  request.preview_max_bytes = 5;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 1.0F, .preview_text = std::string("alpha beta"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 0.5F, .preview_text = std::string(""), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 3, .score = 0.25F, .preview_text = std::nullopt, .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 3, "empty or missing previews should produce surrogate entries");
  Require(context.items[0].text == "alpha", "preview_max_bytes should truncate snippet");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSurrogate, "empty preview should map to surrogate");
  Require(context.items[2].kind == waxcpp::RAGItemKind::kSurrogate, "missing preview should map to surrogate");
  Require(context.total_tokens == 5, "token counting mismatch");
}

void ScenarioNaNScoreNormalization() {
  waxcpp::tests::Log("scenario: nan score normalization");
  waxcpp::SearchRequest request{};
  request.query = "nan";
  request.top_k = 10;
  request.preview_max_bytes = 100;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = std::numeric_limits<float>::quiet_NaN(), .preview_text = std::string("nan"), .sources = {}},
      {.frame_id = 2, .score = 0.1F, .preview_text = std::string("good"), .sources = {}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "expected two context items");
  Require(context.items[0].frame_id == 2, "numeric score should outrank NaN");
  Require(context.items[1].score == 0.0F, "NaN score should be normalized to zero");
}

void ScenarioUnifiedSearchModesAndHybridRrf() {
  waxcpp::tests::Log("scenario: unified search modes and hybrid rrf");
  const std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 10, .score = 4.0F, .preview_text = std::string("t10"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 20, .score = 2.0F, .preview_text = std::string("t20"), .sources = {waxcpp::SearchSource::kText}},
  };
  const std::vector<waxcpp::SearchResult> vector_results = {
      {.frame_id = 20, .score = 3.0F, .preview_text = std::string("v20"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 30, .score = 1.0F, .preview_text = std::string("v30"), .sources = {waxcpp::SearchSource::kVector}},
  };

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
    request.top_k = 10;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(response.results.size() == 2, "text-only mode should use text channel only");
    Require(response.results[0].frame_id == 10, "text-only top result mismatch");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    request.top_k = 10;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(response.results.size() == 2, "vector-only mode should use vector channel only");
    Require(response.results[0].frame_id == 20, "vector-only top result mismatch");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
    request.top_k = 10;
    request.rrf_k = 60;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(response.results.size() == 3, "hybrid mode should merge both channels");
    Require(response.results[0].frame_id == 20, "frame present in both channels should win hybrid RRF");
    Require(response.results[0].sources.size() == 2, "merged frame should carry both sources");
  }
}

void ScenarioDuplicateFrameIdsAreDeduplicated() {
  waxcpp::tests::Log("scenario: duplicate frame ids are deduplicated");

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
    request.top_k = 10;
    const std::vector<waxcpp::SearchResult> text_results = {
        {.frame_id = 1, .score = 1.0F, .preview_text = std::string("first"), .sources = {waxcpp::SearchSource::kText}},
        {.frame_id = 1, .score = 0.5F, .preview_text = std::string("second"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
        {.frame_id = 2, .score = 0.9F, .preview_text = std::string("two"), .sources = {waxcpp::SearchSource::kText}},
    };

    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, {});
    Require(response.results.size() == 2, "single-channel duplicate frame ids should collapse");
    Require(response.results[0].frame_id == 1, "deduped result should keep best-scoring frame entry");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
    request.top_k = 10;
    request.rrf_k = 60;
    const std::vector<waxcpp::SearchResult> text_results = {
        {.frame_id = 100, .score = 4.0F, .preview_text = std::string("dup-a"), .sources = {waxcpp::SearchSource::kText}},
        {.frame_id = 100, .score = 3.0F, .preview_text = std::string("dup-b"), .sources = {waxcpp::SearchSource::kText}},
    };
    const std::vector<waxcpp::SearchResult> vector_results = {
        {.frame_id = 50, .score = 5.0F, .preview_text = std::string("vec"), .sources = {waxcpp::SearchSource::kVector}},
    };

    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(!response.results.empty(), "hybrid dedup scenario should produce results");
    Require(response.results[0].frame_id == 50,
            "duplicate entries in one channel must not double-count RRF contribution");
  }
}

void ScenarioHybridAlphaClamp() {
  waxcpp::tests::Log("scenario: hybrid alpha clamp");
  const std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 10, .score = 4.0F, .preview_text = std::string("t10"), .sources = {waxcpp::SearchSource::kText}},
  };
  const std::vector<waxcpp::SearchResult> vector_results = {
      {.frame_id = 20, .score = 5.0F, .preview_text = std::string("v20"), .sources = {waxcpp::SearchSource::kVector}},
  };

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, -1.0F};  // clamp to 0 => vector-only weight.
    request.top_k = 10;
    request.rrf_k = 60;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(!response.results.empty(), "hybrid alpha<0 should still produce response");
    Require(response.results[0].frame_id == 20, "alpha<0 clamp should prioritize vector channel");
    Require(response.results.size() == 1,
            "alpha<0 clamp should exclude zero-weight text-only candidates from fusion output");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, 2.0F};  // clamp to 1 => text-only weight.
    request.top_k = 10;
    request.rrf_k = 60;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(!response.results.empty(), "hybrid alpha>1 should still produce response");
    Require(response.results[0].frame_id == 10, "alpha>1 clamp should prioritize text channel");
    Require(response.results.size() == 1,
            "alpha>1 clamp should exclude zero-weight vector-only candidates from fusion output");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, std::numeric_limits<float>::quiet_NaN()};
    request.top_k = 10;
    request.rrf_k = 60;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(!response.results.empty(), "hybrid alpha=NaN should still produce response");
    Require(response.results[0].frame_id == 20,
            "alpha=NaN should clamp via Swift parity path (min(1,max(0,alpha))) to vector-prioritized");
  }
}

void ScenarioRrfKClampMatchesSwiftParity() {
  waxcpp::tests::Log("scenario: rrf_k clamp matches swift parity");
  const std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 10, .score = 4.0F, .preview_text = std::string("t10"), .sources = {waxcpp::SearchSource::kText}},
  };

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, 1.0F};  // text-only weight after alpha clamp
    request.top_k = 10;
    request.rrf_k = 0;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, {});
    Require(response.results.size() == 1, "rrf_k=0 should still produce results");
    Require(response.results[0].frame_id == 10, "rrf_k=0 top frame mismatch");
    Require(std::fabs(response.results[0].score - 1.0F) < 1e-6F,
            "rrf_k=0 must score top-1 as 1/(0+1)=1.0");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, 1.0F};  // text-only weight after alpha clamp
    request.top_k = 10;
    request.rrf_k = -7;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, {});
    Require(response.results.size() == 1, "negative rrf_k should still produce results");
    Require(std::fabs(response.results[0].score - 1.0F) < 1e-6F,
            "negative rrf_k must clamp to zero and score top-1 as 1.0");
  }
}

void ScenarioContextTokenBudgetClamp() {
  waxcpp::tests::Log("scenario: context token budget clamp");
  waxcpp::SearchRequest request{};
  request.query = "budget";
  request.top_k = 10;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 2;
  request.snippet_max_tokens = 2;
  request.max_context_tokens = 3;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 2.0F, .preview_text = std::string("one two three"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("four five six"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "budget clamp should keep partial second item");
  Require(context.items[0].text == "one two", "snippet per-item token clamp mismatch");
  Require(context.items[1].text == "four", "remaining-budget truncation mismatch");
  Require(context.total_tokens == 3, "context token budget mismatch");
}

void ScenarioRagItemKindPolicy() {
  waxcpp::tests::Log("scenario: rag item kind policy");
  waxcpp::SearchRequest request{};
  request.query = "kinds";
  request.top_k = 10;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 4;
  request.snippet_max_tokens = 2;
  request.max_context_tokens = 20;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 2.0F, .preview_text = std::string("a b c d e"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("x y z"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "expected two items");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kExpanded, "first item should be expanded");
  Require(context.items[0].text == "a b c d", "expanded token clamp mismatch");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSnippet, "second item should be snippet");
  Require(context.items[1].text == "x y", "snippet token clamp mismatch");
}

void ScenarioMaxSnippetsCapsSnippetsOnly() {
  waxcpp::tests::Log("scenario: max_snippets caps snippets only");
  waxcpp::SearchRequest request{};
  request.query = "snippets";
  request.top_k = 10;
  request.max_snippets = 1;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 8;
  request.snippet_max_tokens = 8;
  request.max_context_tokens = 64;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 3.0F, .preview_text = std::string("one two"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 2.0F, .preview_text = std::string("three four"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 3, .score = 1.0F, .preview_text = std::string("five six"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "max_snippets should allow expansion plus one snippet");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kExpanded, "first item should be expanded");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSnippet, "second item should be the single allowed snippet");

  request.max_snippets = 0;
  const auto context_zero = waxcpp::BuildFastRAGContext(request, response);
  Require(context_zero.items.size() == 1, "max_snippets=0 should suppress snippets but keep expansion");
  Require(context_zero.items[0].kind == waxcpp::RAGItemKind::kExpanded, "expansion must remain when snippets are disabled");
}

void ScenarioMaxSnippetsCapsSurrogateFallbackItems() {
  waxcpp::tests::Log("scenario: max_snippets caps surrogate fallback items");
  waxcpp::SearchRequest request{};
  request.query = "surrogate-cap";
  request.top_k = 10;
  request.max_snippets = 1;
  request.preview_max_bytes = 0;  // force surrogate fallback for all items
  request.expansion_max_tokens = 8;
  request.snippet_max_tokens = 8;
  request.max_context_tokens = 64;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 3.0F, .preview_text = std::string("first"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 2.0F, .preview_text = std::string("second"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 3, .score = 1.0F, .preview_text = std::string("third"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2,
          "max_snippets should cap non-first surrogate fallback items the same as snippet items");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kSurrogate,
          "first item may become surrogate without consuming snippet cap");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSurrogate,
          "second item surrogate should consume the single snippet slot");
}

void ScenarioExpansionDisabledStillYieldsSnippets() {
  waxcpp::tests::Log("scenario: expansion disabled still yields snippets");
  waxcpp::SearchRequest request{};
  request.query = "no-expansion";
  request.top_k = 10;
  request.max_snippets = 2;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 0;
  request.snippet_max_tokens = 4;
  request.max_context_tokens = 64;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 2.0F, .preview_text = std::string("alpha beta gamma"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("delta epsilon"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "expansion disabled should still emit snippet-tier context items");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kSnippet, "first item should be snippet when expansion is disabled");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSnippet, "second item should be snippet when expansion is disabled");
}

void ScenarioSurrogateFallback() {
  waxcpp::tests::Log("scenario: surrogate fallback");
  waxcpp::SearchRequest request{};
  request.query = "surrogate";
  request.top_k = 10;
  request.preview_max_bytes = 0;  // force empty preview path
  request.expansion_max_tokens = 5;
  request.snippet_max_tokens = 2;
  request.max_context_tokens = 10;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 9, .score = 1.0F, .preview_text = std::string("unavailable"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 1, "surrogate fallback should emit one item");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kSurrogate, "item kind should be surrogate");
  Require(context.items[0].text == "frame 9", "surrogate text mismatch");
}

void ScenarioContextDeduplicatesDuplicateFrames() {
  waxcpp::tests::Log("scenario: context deduplicates duplicate frames");
  waxcpp::SearchRequest request{};
  request.query = "dup";
  request.top_k = 10;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 10;
  request.snippet_max_tokens = 10;
  request.max_context_tokens = 100;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 7, .score = 2.0F, .preview_text = std::string("alpha beta"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 7, .score = 1.0F, .preview_text = std::string("ignored"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 9, .score = 1.5F, .preview_text = std::string("gamma"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "context should collapse duplicate frame ids");
  Require(context.items[0].frame_id == 7, "best duplicate score should define rank");
  Require(context.items[1].frame_id == 9, "second unique frame id mismatch");
  Require(context.items[0].sources.size() == 2, "duplicate merge should union sources");
}

void ScenarioContextNormalizesSourcesOrdering() {
  waxcpp::tests::Log("scenario: context normalizes sources ordering");
  waxcpp::SearchRequest request{};
  request.query = "sources";
  request.top_k = 4;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 8;
  request.snippet_max_tokens = 8;
  request.max_context_tokens = 32;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1,
       .score = 1.0F,
       .preview_text = std::string("alpha"),
       .sources = {waxcpp::SearchSource::kVector,
                   waxcpp::SearchSource::kText,
                   waxcpp::SearchSource::kVector,
                   waxcpp::SearchSource::kStructuredMemory}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 1, "expected single context item");
  Require(context.items[0].sources.size() == 3, "context should dedupe duplicate sources");
  Require(context.items[0].sources[0] == waxcpp::SearchSource::kText, "sources must be sorted by enum order");
  Require(context.items[0].sources[1] == waxcpp::SearchSource::kVector, "sources must be sorted by enum order");
  Require(context.items[0].sources[2] == waxcpp::SearchSource::kStructuredMemory,
          "sources must be sorted by enum order");
}

void ScenarioAsciiWhitespaceTokenization() {
  waxcpp::tests::Log("scenario: ascii whitespace tokenization");
  waxcpp::SearchRequest request{};
  request.query = "ws";
  request.top_k = 10;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 10;
  request.snippet_max_tokens = 10;
  request.max_context_tokens = 100;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 1.0F, .preview_text = std::string("a\tb\nc\r\nd"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 1, "expected single context item");
  Require(context.items[0].text == "a b c d", "ascii whitespace tokenization must normalize tabs/newlines");
  Require(context.total_tokens == 4, "ascii whitespace tokenization token count mismatch");
}

void ScenarioRequestClampingParity() {
  waxcpp::tests::Log("scenario: request clamping parity");

  {
    waxcpp::SearchRequest request{};
    request.query = "q";
    request.top_k = 0;
    request.max_context_tokens = 128;
    request.expansion_max_tokens = 32;
    request.snippet_max_tokens = 16;
    request.preview_max_bytes = 64;

    waxcpp::SearchResponse response{};
    response.results = {
        {.frame_id = 1, .score = 1.0F, .preview_text = std::string("alpha beta"), .sources = {waxcpp::SearchSource::kText}},
    };

    const auto context = waxcpp::BuildFastRAGContext(request, response);
    Require(context.items.empty(), "top_k=0 must yield empty context");
    Require(context.total_tokens == 0, "top_k=0 must keep token count at zero");
  }

  {
    waxcpp::SearchRequest request{};
    request.query = "q";
    request.top_k = 8;
    request.max_context_tokens = -5;
    request.expansion_max_tokens = 32;
    request.snippet_max_tokens = 16;
    request.preview_max_bytes = 64;

    waxcpp::SearchResponse response{};
    response.results = {
        {.frame_id = 1, .score = 1.0F, .preview_text = std::string("alpha beta"), .sources = {waxcpp::SearchSource::kText}},
    };

    const auto context = waxcpp::BuildFastRAGContext(request, response);
    Require(context.items.empty(), "negative max_context_tokens must clamp to zero");
    Require(context.total_tokens == 0, "negative max_context_tokens must keep token count at zero");
  }

  {
    waxcpp::SearchRequest request{};
    request.query = "q";
    request.top_k = 8;
    request.max_context_tokens = 64;
    request.expansion_max_tokens = -1;
    request.snippet_max_tokens = -3;
    request.preview_max_bytes = 64;

    waxcpp::SearchResponse response{};
    response.results = {
        {.frame_id = 1, .score = 2.0F, .preview_text = std::string("alpha beta"), .sources = {waxcpp::SearchSource::kText}},
        {.frame_id = 2, .score = 1.0F, .preview_text = std::string("gamma delta"), .sources = {waxcpp::SearchSource::kText}},
    };

    const auto context = waxcpp::BuildFastRAGContext(request, response);
    Require(context.items.empty(), "negative per-item token caps must clamp to zero and suppress output");
    Require(context.total_tokens == 0, "negative per-item token caps must keep token count at zero");
  }
}

void ScenarioPermutationInvariance() {
  waxcpp::tests::Log("scenario: permutation invariance");

  const waxcpp::SearchRequest hybrid_request = [] {
    waxcpp::SearchRequest request{};
    request.query = "permute";
    request.mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
    request.top_k = 16;
    request.rrf_k = 60;
    request.preview_max_bytes = 256;
    request.expansion_max_tokens = 16;
    request.snippet_max_tokens = 16;
    request.max_context_tokens = 64;
    return request;
  }();

  const std::vector<waxcpp::SearchResult> text_base = {
      {.frame_id = 10, .score = 5.0F, .preview_text = std::string("ten"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 20, .score = 4.0F, .preview_text = std::string("twenty"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 20, .score = 3.0F, .preview_text = std::string("twenty-dup"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
      {.frame_id = 30, .score = 2.0F, .preview_text = std::string("thirty"), .sources = {waxcpp::SearchSource::kText}},
  };
  const std::vector<waxcpp::SearchResult> vector_base = {
      {.frame_id = 20, .score = 6.0F, .preview_text = std::string("v20"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 40, .score = 5.0F, .preview_text = std::string("v40"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 40, .score = 1.0F, .preview_text = std::string("v40-dup"), .sources = {waxcpp::SearchSource::kVector}},
  };

  const auto baseline_response = waxcpp::UnifiedSearchWithCandidates(hybrid_request, text_base, vector_base);

  auto text_reversed = text_base;
  auto vector_reversed = vector_base;
  std::reverse(text_reversed.begin(), text_reversed.end());
  std::reverse(vector_reversed.begin(), vector_reversed.end());
  const auto reversed_response = waxcpp::UnifiedSearchWithCandidates(hybrid_request, text_reversed, vector_reversed);

  Require(reversed_response.results.size() == baseline_response.results.size(),
          "permutation invariance: hybrid result size mismatch after reversing inputs");
  for (std::size_t i = 0; i < baseline_response.results.size(); ++i) {
    const auto& lhs = baseline_response.results[i];
    const auto& rhs = reversed_response.results[i];
    Require(lhs.frame_id == rhs.frame_id, "permutation invariance: frame order changed after reversing inputs");
    Require(std::fabs(lhs.score - rhs.score) < 1e-6F,
            "permutation invariance: score changed after reversing inputs");
    Require(lhs.sources == rhs.sources, "permutation invariance: source set changed after reversing inputs");
    Require(lhs.preview_text == rhs.preview_text, "permutation invariance: preview text changed after reversing inputs");
  }

  const auto baseline_context = waxcpp::BuildFastRAGContext(hybrid_request, baseline_response);

  waxcpp::SearchResponse permuted_context_input = baseline_response;
  std::reverse(permuted_context_input.results.begin(), permuted_context_input.results.end());
  const auto permuted_context = waxcpp::BuildFastRAGContext(hybrid_request, permuted_context_input);
  Require(permuted_context.items.size() == baseline_context.items.size(),
          "context permutation invariance: item count mismatch");
  Require(permuted_context.total_tokens == baseline_context.total_tokens,
          "context permutation invariance: total_tokens mismatch");
  for (std::size_t i = 0; i < baseline_context.items.size(); ++i) {
    const auto& lhs = baseline_context.items[i];
    const auto& rhs = permuted_context.items[i];
    Require(lhs.frame_id == rhs.frame_id, "context permutation invariance: frame ordering changed");
    Require(lhs.kind == rhs.kind, "context permutation invariance: item kind changed");
    Require(std::fabs(lhs.score - rhs.score) < 1e-6F, "context permutation invariance: score changed");
    Require(lhs.sources == rhs.sources, "context permutation invariance: sources changed");
    Require(lhs.text == rhs.text, "context permutation invariance: text changed");
  }
}

void ScenarioEqualScoreDuplicatePreviewTieBreakIsOrderIndependent() {
  waxcpp::tests::Log("scenario: equal-score duplicate preview tie-break is order-independent");

  waxcpp::SearchRequest request{};
  request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  request.top_k = 8;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 16;
  request.snippet_max_tokens = 16;
  request.max_context_tokens = 64;

  const std::vector<waxcpp::SearchResult> forward = {
      {.frame_id = 42, .score = 1.0F, .preview_text = std::string("zeta"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 42, .score = 1.0F, .preview_text = std::string("alpha"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
  };
  const std::vector<waxcpp::SearchResult> reversed = {
      {.frame_id = 42, .score = 1.0F, .preview_text = std::string("alpha"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
      {.frame_id = 42, .score = 1.0F, .preview_text = std::string("zeta"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto response_forward = waxcpp::UnifiedSearchWithCandidates(request, forward, {});
  const auto response_reversed = waxcpp::UnifiedSearchWithCandidates(request, reversed, {});
  Require(response_forward.results.size() == 1 && response_reversed.results.size() == 1,
          "duplicate tie-break scenario should collapse to one frame");
  Require(response_forward.results[0].preview_text.has_value() && response_reversed.results[0].preview_text.has_value(),
          "duplicate tie-break scenario should preserve preview text");
  Require(*response_forward.results[0].preview_text == "alpha",
          "duplicate tie-break should choose deterministic lexicographically smaller preview");
  Require(*response_reversed.results[0].preview_text == "alpha",
          "duplicate tie-break must be independent of input order");

  waxcpp::SearchResponse context_response_forward{.results = forward};
  waxcpp::SearchResponse context_response_reversed{.results = reversed};
  const auto context_forward = waxcpp::BuildFastRAGContext(request, context_response_forward);
  const auto context_reversed = waxcpp::BuildFastRAGContext(request, context_response_reversed);
  Require(context_forward.items.size() == 1 && context_reversed.items.size() == 1,
          "context duplicate tie-break scenario should collapse to one item");
  Require(context_forward.items[0].text == context_reversed.items[0].text,
          "context text should be order-independent for equal-score duplicate previews");
}

void ScenarioEqualScoreDuplicatePrefersPresentPreviewOverNullopt() {
  waxcpp::tests::Log("scenario: equal-score duplicate prefers present preview over nullopt");

  waxcpp::SearchRequest request{};
  request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  request.top_k = 8;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 16;
  request.snippet_max_tokens = 16;
  request.max_context_tokens = 64;

  const std::vector<waxcpp::SearchResult> forward = {
      {.frame_id = 77, .score = 1.0F, .preview_text = std::nullopt, .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 77, .score = 1.0F, .preview_text = std::string("present"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
  };
  const std::vector<waxcpp::SearchResult> reversed = {
      {.frame_id = 77, .score = 1.0F, .preview_text = std::string("present"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
      {.frame_id = 77, .score = 1.0F, .preview_text = std::nullopt, .sources = {waxcpp::SearchSource::kText}},
  };

  const auto response_forward = waxcpp::UnifiedSearchWithCandidates(request, forward, {});
  const auto response_reversed = waxcpp::UnifiedSearchWithCandidates(request, reversed, {});
  Require(response_forward.results.size() == 1 && response_reversed.results.size() == 1,
          "nullopt-preview duplicate scenario should collapse to one frame");
  Require(response_forward.results[0].preview_text.has_value() && response_reversed.results[0].preview_text.has_value(),
          "nullopt-preview duplicate scenario should keep present preview");
  Require(*response_forward.results[0].preview_text == "present",
          "present preview must win over nullopt for equal-score duplicates");
  Require(*response_reversed.results[0].preview_text == "present",
          "present preview winner must be input-order independent");
}

void ScenarioEqualScoreDuplicateMergesSourcesDeterministically() {
  waxcpp::tests::Log("scenario: equal-score duplicate merges sources deterministically");

  waxcpp::SearchRequest request{};
  request.query = "src-merge";
  request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  request.top_k = 8;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 16;
  request.snippet_max_tokens = 16;
  request.max_context_tokens = 64;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 99, .score = 1.0F, .preview_text = std::string("same"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 99,
       .score = 1.0F,
       .preview_text = std::string("same"),
       .sources = {waxcpp::SearchSource::kStructuredMemory, waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 1, "equal-score duplicate source-merge scenario should collapse to one item");
  Require(context.items[0].sources.size() == 3, "equal-score duplicate source-merge should union all sources");
  Require(context.items[0].sources[0] == waxcpp::SearchSource::kText,
          "merged sources should be normalized in deterministic enum order");
  Require(context.items[0].sources[1] == waxcpp::SearchSource::kVector,
          "merged sources should be normalized in deterministic enum order");
  Require(context.items[0].sources[2] == waxcpp::SearchSource::kStructuredMemory,
          "merged sources should be normalized in deterministic enum order");
}

void ScenarioLowerScorePreviewFallbackIsOrderIndependent() {
  waxcpp::tests::Log("scenario: lower-score preview fallback is order-independent");

  waxcpp::SearchRequest request{};
  request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  request.top_k = 8;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 16;
  request.snippet_max_tokens = 16;
  request.max_context_tokens = 64;

  const std::vector<waxcpp::SearchResult> forward = {
      {.frame_id = 123, .score = 5.0F, .preview_text = std::nullopt, .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 123, .score = 3.0F, .preview_text = std::string("zeta"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 123, .score = 2.0F, .preview_text = std::string("alpha"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
  };
  const std::vector<waxcpp::SearchResult> reversed = {
      {.frame_id = 123, .score = 2.0F, .preview_text = std::string("alpha"), .sources = {waxcpp::SearchSource::kStructuredMemory}},
      {.frame_id = 123, .score = 3.0F, .preview_text = std::string("zeta"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 123, .score = 5.0F, .preview_text = std::nullopt, .sources = {waxcpp::SearchSource::kText}},
  };

  const auto response_forward = waxcpp::UnifiedSearchWithCandidates(request, forward, {});
  const auto response_reversed = waxcpp::UnifiedSearchWithCandidates(request, reversed, {});
  Require(response_forward.results.size() == 1 && response_reversed.results.size() == 1,
          "lower-score preview fallback scenario should collapse to one frame");
  Require(response_forward.results[0].preview_text.has_value() && response_reversed.results[0].preview_text.has_value(),
          "lower-score preview fallback scenario should keep fallback preview");
  Require(*response_forward.results[0].preview_text == "alpha",
          "fallback preview should deterministically use lexicographically smallest available preview");
  Require(*response_reversed.results[0].preview_text == "alpha",
          "fallback preview selection must be independent of input order");
}

void ScenarioSeededFuzzPermutationInvariance() {
  waxcpp::tests::Log("scenario: seeded fuzz permutation invariance");

  auto equal_scores = [](float lhs, float rhs) -> bool {
    return std::fabs(lhs - rhs) < 1e-6F;
  };

  auto require_response_equal = [&](const waxcpp::SearchResponse& expected,
                                    const waxcpp::SearchResponse& actual,
                                    const std::string& where) {
    Require(expected.results.size() == actual.results.size(),
            where + ": response size changed after permutation");
    for (std::size_t i = 0; i < expected.results.size(); ++i) {
      const auto& lhs = expected.results[i];
      const auto& rhs = actual.results[i];
      Require(lhs.frame_id == rhs.frame_id, where + ": frame_id changed after permutation");
      Require(equal_scores(lhs.score, rhs.score), where + ": score changed after permutation");
      Require(lhs.preview_text == rhs.preview_text, where + ": preview changed after permutation");
      Require(lhs.sources == rhs.sources, where + ": sources changed after permutation");
    }
  };

  auto require_context_equal = [&](const waxcpp::RAGContext& expected,
                                   const waxcpp::RAGContext& actual,
                                   const std::string& where) {
    Require(expected.query == actual.query, where + ": query changed after permutation");
    Require(expected.total_tokens == actual.total_tokens, where + ": total_tokens changed after permutation");
    Require(expected.items.size() == actual.items.size(), where + ": context size changed after permutation");
    for (std::size_t i = 0; i < expected.items.size(); ++i) {
      const auto& lhs = expected.items[i];
      const auto& rhs = actual.items[i];
      Require(lhs.frame_id == rhs.frame_id, where + ": context frame_id changed after permutation");
      Require(equal_scores(lhs.score, rhs.score), where + ": context score changed after permutation");
      Require(lhs.kind == rhs.kind, where + ": context kind changed after permutation");
      Require(lhs.text == rhs.text, where + ": context text changed after permutation");
      Require(lhs.sources == rhs.sources, where + ": context sources changed after permutation");
    }
  };

  std::mt19937 rng(0xC0DEFACEU);
  std::uniform_int_distribution<int> mode_dist(0, 2);
  std::uniform_real_distribution<float> alpha_dist(-1.5F, 2.5F);
  std::uniform_int_distribution<int> topk_dist(0, 12);
  std::uniform_int_distribution<int> rrf_dist(-20, 100);
  std::uniform_int_distribution<int> preview_bytes_dist(0, 96);
  std::uniform_int_distribution<int> context_tokens_dist(0, 64);
  std::uniform_int_distribution<int> per_item_tokens_dist(-4, 24);
  std::uniform_int_distribution<int> max_snippets_dist(-2, 6);
  std::uniform_int_distribution<int> channel_size_dist(0, 20);
  std::uniform_int_distribution<int> frame_id_dist(1, 14);
  std::uniform_real_distribution<float> score_dist(-5.0F, 5.0F);
  std::uniform_int_distribution<int> bool_dist(0, 1);
  std::uniform_int_distribution<int> source_dist(0, 2);
  std::uniform_int_distribution<int> word_len_dist(1, 6);
  std::uniform_int_distribution<int> word_count_dist(1, 4);
  std::uniform_int_distribution<int> letter_dist(0, 25);
  std::uniform_int_distribution<int> nan_dist(0, 19);  // ~5%

  auto random_word = [&]() -> std::string {
    const int len = word_len_dist(rng);
    std::string out{};
    out.reserve(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) {
      out.push_back(static_cast<char>('a' + letter_dist(rng)));
    }
    return out;
  };

  auto random_text = [&]() -> std::string {
    const int words = word_count_dist(rng);
    std::string out{};
    for (int i = 0; i < words; ++i) {
      if (i > 0) {
        out.push_back(' ');
      }
      out.append(random_word());
    }
    return out;
  };

  auto random_sources = [&]() -> std::vector<waxcpp::SearchSource> {
    std::vector<waxcpp::SearchSource> sources{};
    const int source_count = 1 + (rng() % 3);
    for (int i = 0; i < source_count; ++i) {
      const int kind = source_dist(rng);
      if (kind == 0) {
        sources.push_back(waxcpp::SearchSource::kText);
      } else if (kind == 1) {
        sources.push_back(waxcpp::SearchSource::kVector);
      } else {
        sources.push_back(waxcpp::SearchSource::kStructuredMemory);
      }
    }
    return sources;
  };

  auto random_result = [&]() -> waxcpp::SearchResult {
    waxcpp::SearchResult result{};
    result.frame_id = static_cast<std::uint64_t>(frame_id_dist(rng));
    if (nan_dist(rng) == 0) {
      result.score = std::numeric_limits<float>::quiet_NaN();
    } else {
      result.score = score_dist(rng);
    }
    if (bool_dist(rng) == 0) {
      result.preview_text = random_text();
    } else if (bool_dist(rng) == 0) {
      result.preview_text = std::string{};
    } else {
      result.preview_text = std::nullopt;
    }
    result.sources = random_sources();
    return result;
  };

  constexpr std::size_t kIterations = 256;
  for (std::size_t i = 0; i < kIterations; ++i) {
    waxcpp::SearchRequest request{};
    if (bool_dist(rng) == 0) {
      request.query = random_text();
    }
    request.mode = {static_cast<waxcpp::SearchModeKind>(mode_dist(rng)), alpha_dist(rng)};
    request.top_k = topk_dist(rng);
    request.rrf_k = rrf_dist(rng);
    request.preview_max_bytes = preview_bytes_dist(rng);
    request.max_context_tokens = context_tokens_dist(rng);
    request.expansion_max_tokens = per_item_tokens_dist(rng);
    request.snippet_max_tokens = per_item_tokens_dist(rng);
    request.max_snippets = max_snippets_dist(rng);

    std::vector<waxcpp::SearchResult> text_results{};
    text_results.reserve(static_cast<std::size_t>(channel_size_dist(rng)));
    const int text_n = channel_size_dist(rng);
    for (int k = 0; k < text_n; ++k) {
      text_results.push_back(random_result());
    }

    std::vector<waxcpp::SearchResult> vector_results{};
    vector_results.reserve(static_cast<std::size_t>(channel_size_dist(rng)));
    const int vector_n = channel_size_dist(rng);
    for (int k = 0; k < vector_n; ++k) {
      vector_results.push_back(random_result());
    }

    const auto baseline = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    const auto baseline_context = waxcpp::BuildFastRAGContext(request, baseline);

    for (int perm = 0; perm < 4; ++perm) {
      auto text_perm = text_results;
      auto vector_perm = vector_results;
      std::shuffle(text_perm.begin(), text_perm.end(), rng);
      std::shuffle(vector_perm.begin(), vector_perm.end(), rng);

      const auto permuted_response = waxcpp::UnifiedSearchWithCandidates(request, text_perm, vector_perm);
      require_response_equal(baseline, permuted_response, "seeded fuzz unified search");

      auto response_for_context = permuted_response;
      std::shuffle(response_for_context.results.begin(), response_for_context.results.end(), rng);
      const auto permuted_context = waxcpp::BuildFastRAGContext(request, response_for_context);
      require_context_equal(baseline_context, permuted_context, "seeded fuzz context build");
    }
  }
}

void ScenarioAdaptiveFusionSelectsWeightsByQueryType() {
  waxcpp::tests::Log("scenario: adaptive fusion selects weights by query type");

  // Text and vector candidates with clear separation.
  std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 1, .score = 10.0F, .preview_text = std::string("text hit"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 5.0F, .preview_text = std::string("text hit 2"), .sources = {waxcpp::SearchSource::kText}},
  };
  std::vector<waxcpp::SearchResult> vector_results = {
      {.frame_id = 3, .score = 10.0F, .preview_text = std::string("vector hit"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 4, .score = 5.0F, .preview_text = std::string("vector hit 2"), .sources = {waxcpp::SearchSource::kVector}},
  };

  // Factual query -> bm25=0.7, vector=0.3 (text-heavy).
  waxcpp::SearchRequest factual_req{};
  factual_req.query = "what is a vector database?";
  factual_req.mode = {waxcpp::SearchModeKind::kHybrid, 0.5f};
  factual_req.top_k = 4;
  factual_req.rrf_k = 60;

  auto factual_response = waxcpp::UnifiedSearchAdaptive(factual_req, text_results, vector_results);
  Require(!factual_response.results.empty(), "factual should return results");

  // Semantic query -> bm25=0.3, vector=0.7 (vector-heavy).
  waxcpp::SearchRequest semantic_req{};
  semantic_req.query = "how does the search algorithm work?";
  semantic_req.mode = {waxcpp::SearchModeKind::kHybrid, 0.5f};
  semantic_req.top_k = 4;
  semantic_req.rrf_k = 60;

  auto semantic_response = waxcpp::UnifiedSearchAdaptive(semantic_req, text_results, vector_results);
  Require(!semantic_response.results.empty(), "semantic should return results");

  // Verify that the factual query boosts text results more than the semantic query.
  // In factual: alpha=0.7 (text weight), so text frames should have higher RRF scores.
  // In semantic: alpha=0.3 (text weight), so vector frames should have higher RRF scores.
  float factual_text_score = 0.0f;
  float factual_vector_score = 0.0f;
  for (const auto& r : factual_response.results) {
    if (r.frame_id == 1) factual_text_score = r.score;
    if (r.frame_id == 3) factual_vector_score = r.score;
  }

  float semantic_text_score = 0.0f;
  float semantic_vector_score = 0.0f;
  for (const auto& r : semantic_response.results) {
    if (r.frame_id == 1) semantic_text_score = r.score;
    if (r.frame_id == 3) semantic_vector_score = r.score;
  }

  // Factual: text rank-1 score = 0.7 / (60+1) > vector rank-1 score = 0.3 / (60+1).
  Require(factual_text_score > factual_vector_score,
          "factual query should weight text higher than vector");

  // Semantic: vector rank-1 score = 0.7 / (60+1) > text rank-1 score = 0.3 / (60+1).
  Require(semantic_vector_score > semantic_text_score,
          "semantic query should weight vector higher than text");
}

void ScenarioAdaptiveFusionNonHybridPassthrough() {
  waxcpp::tests::Log("scenario: adaptive fusion passes through for non-hybrid modes");

  std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 1, .score = 1.0F, .preview_text = std::string("text"), .sources = {waxcpp::SearchSource::kText}},
  };
  std::vector<waxcpp::SearchResult> vector_results = {
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("vec"), .sources = {waxcpp::SearchSource::kVector}},
  };

  // Text-only mode should ignore adaptive weights.
  waxcpp::SearchRequest text_req{};
  text_req.query = "how does it work?";  // semantic -> would be vector-heavy
  text_req.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5f};
  text_req.top_k = 10;

  auto response = waxcpp::UnifiedSearchAdaptive(text_req, text_results, vector_results);
  Require(response.results.size() == 1, "text-only should return text result only");
  Require(response.results[0].frame_id == 1, "should be the text frame");

  // Vector-only mode.
  waxcpp::SearchRequest vec_req{};
  vec_req.query = "what is a thing?";  // factual -> would be text-heavy
  vec_req.mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5f};
  vec_req.top_k = 10;

  auto vec_response = waxcpp::UnifiedSearchAdaptive(vec_req, text_results, vector_results);
  Require(vec_response.results.size() == 1, "vector-only should return vector result only");
  Require(vec_response.results[0].frame_id == 2, "should be the vector frame");
}

void ScenarioAdaptiveFusionCustomConfig() {
  waxcpp::tests::Log("scenario: adaptive fusion with custom config");

  std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 1, .score = 10.0F, .preview_text = std::string("text"), .sources = {waxcpp::SearchSource::kText}},
  };
  std::vector<waxcpp::SearchResult> vector_results = {
      {.frame_id = 2, .score = 10.0F, .preview_text = std::string("vec"), .sources = {waxcpp::SearchSource::kVector}},
  };

  // Custom config: factual queries should be 100% vector.
  std::unordered_map<waxcpp::QueryType, waxcpp::FusionWeights> custom = {
      {waxcpp::QueryType::kFactual, {0.0f, 1.0f, 0.0f}},
  };
  waxcpp::AdaptiveFusionConfig custom_config(custom);

  waxcpp::SearchRequest req{};
  req.query = "what is RAG?";  // factual
  req.mode = {waxcpp::SearchModeKind::kHybrid, 0.5f};
  req.top_k = 10;
  req.rrf_k = 60;

  auto response = waxcpp::UnifiedSearchAdaptive(req, text_results, vector_results, custom_config);
  // With alpha=0 (bm25=0), text weight is 0 so only vector contributes.
  Require(response.results.size() == 1, "zero text weight should exclude text-only results");
  Require(response.results[0].frame_id == 2, "only vector result should appear");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("search_test: start");
    ScenarioDeterministicOrderingAndTopK();
    ScenarioPreviewClampAndTokenCount();
    ScenarioNaNScoreNormalization();
    ScenarioUnifiedSearchModesAndHybridRrf();
    ScenarioDuplicateFrameIdsAreDeduplicated();
    ScenarioHybridAlphaClamp();
    ScenarioRrfKClampMatchesSwiftParity();
    ScenarioContextTokenBudgetClamp();
    ScenarioRagItemKindPolicy();
    ScenarioMaxSnippetsCapsSnippetsOnly();
    ScenarioMaxSnippetsCapsSurrogateFallbackItems();
    ScenarioExpansionDisabledStillYieldsSnippets();
    ScenarioSurrogateFallback();
    ScenarioContextDeduplicatesDuplicateFrames();
    ScenarioContextNormalizesSourcesOrdering();
    ScenarioAsciiWhitespaceTokenization();
    ScenarioRequestClampingParity();
    ScenarioPermutationInvariance();
    ScenarioEqualScoreDuplicatePreviewTieBreakIsOrderIndependent();
    ScenarioEqualScoreDuplicatePrefersPresentPreviewOverNullopt();
    ScenarioEqualScoreDuplicateMergesSourcesDeterministically();
    ScenarioLowerScorePreviewFallbackIsOrderIndependent();
    ScenarioSeededFuzzPermutationInvariance();
    ScenarioAdaptiveFusionSelectsWeightsByQueryType();
    ScenarioAdaptiveFusionNonHybridPassthrough();
    ScenarioAdaptiveFusionCustomConfig();
    waxcpp::tests::Log("search_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
