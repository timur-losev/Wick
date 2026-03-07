@echo off
REM ============================================================
REM  Resume Blueprint LLM enrichment (skip export, skip already committed chunks)
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080
REM    - llama-server running on 127.0.0.1:8004
REM ============================================================

set EXPORT_DIR=J:\UE4\Projects\perforce_DESKTOP-UGMULAU_trunk_1659\Oliva\Saved\BlueprintExports

echo ============================================================
echo  Resume Blueprint LLM enrichment
echo  Source: %EXPORT_DIR%
echo  resume=true, max_chunks=0 (all remaining)
echo ============================================================
echo.

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%EXPORT_DIR:\=/%\",\"resume\":true,\"flush_every_chunks\":1000,\"ingest_batch_size\":1,\"target_tokens\":3000,\"max_chunks\":0,\"include_extensions\":[\".bpl_json\"],\"exclude_dirs\":[],\"enrich_regex\":false,\"enrich_llm\":true}}"

echo.
echo.
echo Index resume submitted. Check status:
echo   index_status.bat
echo.
pause
