@echo off
REM ============================================================
REM  Start Oliva project indexing via WAX RAG Server
REM  Requires: WAX server running on 127.0.0.1:8080
REM            llama-server running on 127.0.0.1:8004
REM            (start with: g:\Proj\Agents1\start_llama.bat)
REM
REM  enrich_regex = true   (UCLASS/UPROPERTY/UFUNCTION/includes)
REM  enrich_llm   = true   (~3K chunks — LLM extracts semantics)
REM  resume       = false  (full re-index to pick up enriched facts)
REM ============================================================

echo ============================================================
echo  Oliva Indexing with Regex + LLM Enrichment
echo  Target: j:\UE4\Projects\perforce_DESKTOP-UGMULAU_trunk_1659\Oliva
echo  Server: http://127.0.0.1:8080
echo  LLM:    http://127.0.0.1:8004 (llama-server)
echo ============================================================
echo.
echo WARNING: LLM enrichment is enabled. Make sure llama-server
echo          is running, or LLM facts will be silently skipped.
echo.

REM --- index.start: begins background indexing ---
curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"j:/UE4/Projects/perforce_DESKTOP-UGMULAU_trunk_1659/Oliva\",\"resume\":false,\"flush_every_chunks\":131072,\"ingest_batch_size\":1,\"include_extensions\":[\".h\",\".hpp\",\".cpp\",\".inl\",\".inc\"],\"enrich_regex\":true,\"enrich_llm\":true}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
