@echo off
REM ============================================================
REM  Index OlivaVanilla C++ source code with LLM enrichment
REM
REM  Scans Source/ and Plugins/ for .h/.hpp/.cpp/.inl/.inc
REM  ~930 files, LLM enrichment extracts structured facts
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080
REM    - llama-server running on 127.0.0.1:8004
REM ============================================================

set PROJECT_ROOT=J:\UE4\Projects\OlivaVanilla

echo ============================================================
echo  OlivaVanilla C++ Code Indexing (with LLM enrichment)
echo  Source: %PROJECT_ROOT%
echo  Extensions: .h .hpp .cpp .inl .inc
echo  enrich_regex=true, enrich_llm=true
echo ============================================================
echo.

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%PROJECT_ROOT:\=/%\",\"resume\":false,\"flush_every_chunks\":1000,\"ingest_batch_size\":1,\"target_tokens\":400,\"max_chunks\":0,\"include_extensions\":[\".h\",\".hpp\",\".cpp\",\".inl\",\".inc\"],\"exclude_dirs\":[\"DerivedDataCache\",\"Intermediate\",\"Saved\",\"Binaries\",\"Build\",\"Content\"],\"enrich_regex\":true,\"enrich_llm\":true}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
