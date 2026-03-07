@echo off
REM ============================================================
REM  Export Oliva Blueprints to JSON + Index in WAX with LLM enrichment
REM
REM  Step 1: UnrealEditor-Cmd exports all BP_* graphs to JSON
REM  Step 2: WAX RAG server indexes exported JSONs
REM
REM  target_tokens = 1000  (larger chunks for Blueprint JSON, ~15K chunks)
REM  max_chunks   = 100   (test run; set to 0 for full indexing)
REM  enrich_regex = false  (C++ enricher, not useful for Blueprint JSON)
REM  enrich_llm   = true   (LLM extracts Blueprint semantics from JSON)
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080  (start_wax_server.bat)
REM    - llama-server running on 127.0.0.1:8004 (start_llama_enrich.bat)
REM    - Oliva project compiled with OlivaBlueprintRAG plugin
REM ============================================================

set UE_EDITOR=J:\UE5.2SRC\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
set UPROJECT=J:\UE4\Projects\perforce_DESKTOP-UGMULAU_trunk_1659\Oliva\LyraStarterGame.uproject
set EXPORT_DIR=J:\UE4\Projects\perforce_DESKTOP-UGMULAU_trunk_1659\Oliva\Saved\BlueprintExports

echo ============================================================
echo  Oliva Blueprint RAG Pipeline
echo  1) Export BP_* graphs to JSON
echo  2) Index JSONs in WAX with LLM enrichment
echo ============================================================
echo.

REM ---- Step 1: Export Blueprints to JSON ----
echo [Step 1/2] Exporting Blueprints to JSON ...
echo   Editor:  %UE_EDITOR%
echo   Project: %UPROJECT%
echo   Output:  %EXPORT_DIR%
echo.

"%UE_EDITOR%" "%UPROJECT%" -run=BlueprintGraphExport -Root=/Game -Prefix=BP_ -ExportDir="%EXPORT_DIR%" -unattended -nop4

REM UE may return non-zero due to Blueprint compilation errors — that's OK.
REM We check for actual exported files instead of exit code.
echo.
echo Export finished (exit code %ERRORLEVEL%). Counting files...
for /f %%A in ('dir /b /a-d "%EXPORT_DIR%\*.bpl_json" 2^>nul ^| find /c /v ""') do set JSON_COUNT=%%A
echo   Found %JSON_COUNT% JSON files.

if "%JSON_COUNT%"=="0" (
    echo.
    echo ERROR: No JSON files exported. Check UE log for details.
    pause
    exit /b 1
)
echo.

REM ---- Step 2: Index in WAX ----
echo [Step 2/2] Starting WAX indexing with LLM enrichment ...
echo   Server: http://127.0.0.1:8080
echo   LLM:    http://127.0.0.1:8004 (llama-server)
echo   Source:  %EXPORT_DIR%
echo.

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%EXPORT_DIR:\=/%\",\"resume\":false,\"flush_every_chunks\":1000,\"ingest_batch_size\":1,\"target_tokens\":3000,\"max_chunks\":1000,\"include_extensions\":[\".bpl_json\"],\"exclude_dirs\":[],\"enrich_regex\":false,\"enrich_llm\":true}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
