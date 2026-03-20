@echo off
REM ============================================================
REM  Export OlivaVanilla Blueprints + Index in WAX with LLM enrichment
REM
REM  Step 1: UnrealEditor-Cmd exports all BP_* graphs to .bpl_json
REM  Step 2: WAX indexes .bpl_json (facts only, no raw JSON in text index)
REM
REM  target_tokens = 3000  (larger chunks for Blueprint JSON)
REM  enrich_regex  = false (C++ enricher, not useful for Blueprint JSON)
REM  enrich_llm    = true  (LLM extracts Blueprint semantics as facts)
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080
REM    - llama-server running on 127.0.0.1:8004
REM    - OlivaVanilla project compiled with OlivaBlueprintRAG plugin
REM ============================================================

set UE_EDITOR=J:\UE5.2SRC\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
set UPROJECT=J:\UE4\Projects\OlivaVanilla\LyraStarterGame.uproject
set EXPORT_DIR=J:\UE4\Projects\OlivaVanilla\Saved\BlueprintExports
set CHECKPOINT_NAMESPACE=olivavanilla_blueprints

echo ============================================================
echo  OlivaVanilla Blueprint RAG Pipeline
echo  1) Export BP_* graphs to .bpl_json
echo  2) Index .bpl_json in WAX with LLM enrichment (facts only)
echo ============================================================
echo.

REM ---- Step 1: Export Blueprints to .bpl_json ----
echo [Step 1/2] Exporting Blueprints to .bpl_json ...
echo   Editor:  %UE_EDITOR%
echo   Project: %UPROJECT%
echo   Output:  %EXPORT_DIR%
echo.

REM  -Root=/ scans ALL mount points (game + plugins), not just /Game
REM  bRecursiveClasses=true in plugin catches UBlueprint + all subclasses
REM "%UE_EDITOR%" "%UPROJECT%" -run=BlueprintGraphExport -Root=/ -ExportDir="%EXPORT_DIR%" -unattended -nop4

REM UE may return non-zero due to Blueprint compilation errors — that's OK.
echo.
echo Export finished (exit code %ERRORLEVEL%). Counting files...
for /f %%A in ('dir /b /a-d "%EXPORT_DIR%\*.bpl_json" 2^>nul ^| find /c /v ""') do set BPL_COUNT=%%A
echo   Found %BPL_COUNT% .bpl_json files.

if "%BPL_COUNT%"=="0" (
    echo.
    echo ERROR: No .bpl_json files exported. Check UE log for details.
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
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%EXPORT_DIR:\=/%\",\"resume\":false,\"checkpoint_namespace\":\"%CHECKPOINT_NAMESPACE%\",\"flush_every_chunks\":1000,\"ingest_batch_size\":1,\"target_tokens\":3000,\"max_chunks\":0,\"include_extensions\":[\".bpl_json\"],\"exclude_dirs\":[],\"enrich_regex\":false,\"enrich_llm\":true}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
