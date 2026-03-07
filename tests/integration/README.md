# C++ Integration Runs (Real Corpus)

This folder contains real-corpus integration assets for the UE5 indexing and answer pipeline.

## Query Pack

- `tests/integration/ue5-query-pack.json`
  - Deterministic query set for corpus influence checks.
  - Each query can define `must_cite_path_substr` to assert source-path evidence in citations.

## Runner Script

- `scripts/ue5/run-ue5-corpus-integration.ps1`
  - Starts `index.start`, polls `index.status`, runs `answer.generate` across query pack, writes summary report.
  - Defaults:
    - `RepoRoot=j:\UE5.2SRC\`
    - `QueriesFile=tests/integration/ue5-query-pack.json`
    - `ReportOut=tests/integration/ue5-corpus-integration-report.json`

## Typical Execution

0. Export llama.cpp runtime env (for `start_llama.bat` on port `8004`):
```powershell
$env:WAXCPP_LLAMA_GEN_ENDPOINT = "http://127.0.0.1:8004/completion"
$env:WAXCPP_LLAMA_API_KEY = "1f67a5931a61dfcf7622c7401ff88008"
```

1. Build server:
```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target waxcpp_rag_server --parallel 12
```

2. Smoke on limited subset:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/ue5/run-ue5-corpus-integration.ps1 `
  -RepoRoot "j:\UE5.2SRC\" `
  -StartServer `
  -ServerExe "build/bin/waxcpp_rag_server.exe" `
  -MaxFiles 250 `
  -MaxChunks 3000 `
  -FlushEveryChunks 128 `
  -IngestBatchSize 8 `
  -MinInfluenceRate 0.5
```

3. Full run:
```powershell
powershell -ExecutionPolicy Bypass -File scripts/ue5/run-ue5-corpus-integration.ps1 `
  -RepoRoot "j:\UE5.2SRC\" `
  -StartServer `
  -ServerExe "build/bin/waxcpp_rag_server.exe" `
  -FlushEveryChunks 256 `
  -IngestBatchSize 16 `
  -MinInfluenceRate 0.7 `
  -MinRequiredPathMatchRate 0.5
```

## Output

- JSON report with:
  - final indexing status
  - per-query citation stats
  - `corpus_influence_rate`
  - `required_path_match_rate`
