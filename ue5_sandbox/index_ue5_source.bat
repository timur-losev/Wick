@echo off
REM ============================================================
REM  Index UE5 Engine source code (NO LLM enrichment)
REM
REM  ~118K C++ files in Engine/Source + Engine/Plugins
REM  Text index only + regex enrichment (no LLM — too large)
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080
REM    - (llama-server NOT required)
REM ============================================================

set UE5_SOURCE=J:\UE5.2SRC\Engine
set CHECKPOINT_NAMESPACE=ue5_source

echo ============================================================
echo  UE5 Engine Source Indexing (no LLM enrichment)
echo  Source: %UE5_SOURCE%
echo  Extensions: .h .hpp .cpp .inl .inc
echo  enrich_regex=true, enrich_llm=false
echo ============================================================
echo.

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%UE5_SOURCE:\=/%\",\"resume\":false,\"checkpoint_namespace\":\"%CHECKPOINT_NAMESPACE%\",\"flush_every_chunks\":5000,\"ingest_batch_size\":32,\"target_tokens\":400,\"max_chunks\":0,\"include_extensions\":[\".h\",\".hpp\",\".cpp\",\".inl\",\".inc\"],\"exclude_dirs\":[\"ThirdParty\",\"Intermediate\",\"DerivedDataCache\",\"Binaries\"],\"enrich_regex\":true,\"enrich_llm\":false}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
