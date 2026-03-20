@echo off
setlocal
REM ============================================================
REM  Resume OlivaVanilla Blueprint LLM enrichment
REM  (skip export, skip already committed chunks)
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080
REM    - llama-server running on 127.0.0.1:8004
REM ============================================================

set EXPORT_DIR=J:\UE4\Projects\OlivaVanilla\Saved\BlueprintExports
set CHECKPOINT_NAMESPACE=olivavanilla_blueprints
set EXPORT_DIR_FWD=%EXPORT_DIR:\=/%
for %%I in ("%~dp0..\build\bin\data") do set "SERVER_DATA_DIR=%%~fI"
set "CHECKPOINT_FILE=%SERVER_DATA_DIR%\wax-server.mv2s.index.%CHECKPOINT_NAMESPACE%.checkpoint"
set "FILE_MANIFEST=%CHECKPOINT_FILE%.file_manifest"

echo ============================================================
echo  Resume OlivaVanilla Blueprint LLM enrichment
echo  Source: %EXPORT_DIR%
echo  resume=true, max_chunks=0 (all remaining)
echo  file-level reuse only
echo ============================================================
echo.
echo Preflight:
echo   Checkpoint: %CHECKPOINT_FILE%
echo   File manifest: %FILE_MANIFEST%
echo.

if not exist "%CHECKPOINT_FILE%" (
    echo Preflight failed: missing checkpoint file.
    echo   %CHECKPOINT_FILE%
    echo Run a baseline index first so resume has something to reuse.
    echo.
    pause
    exit /b 1
)

if not exist "%FILE_MANIFEST%" (
    echo Preflight failed: missing file manifest.
    echo   %FILE_MANIFEST%
    echo Run a baseline index first so resume has something to reuse.
    echo.
    pause
    exit /b 1
)

findstr /b /c:"repo_root=%EXPORT_DIR_FWD%" "%CHECKPOINT_FILE%" >nul
if errorlevel 1 (
    echo Preflight failed: checkpoint repo_root does not match this export directory.
    echo   expected:   %EXPORT_DIR_FWD%
    echo   checkpoint:  %CHECKPOINT_FILE%
    echo.
    pause
    exit /b 1
)

for %%I in ("%FILE_MANIFEST%") do set "FILE_MANIFEST_SIZE=%%~zI"
if "%FILE_MANIFEST_SIZE%"=="0" (
    echo Preflight failed: file manifest is empty.
    echo   %FILE_MANIFEST%
    echo.
    pause
    exit /b 1
)

echo Preflight ok: resume baseline found.
echo   repo_root:     %EXPORT_DIR_FWD%
echo   checkpoint:    %CHECKPOINT_FILE%
echo   file_manifest: %FILE_MANIFEST%
echo.

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%EXPORT_DIR:\=/%\",\"resume\":true,\"checkpoint_namespace\":\"%CHECKPOINT_NAMESPACE%\",\"flush_every_chunks\":1000,\"ingest_batch_size\":1,\"target_tokens\":3000,\"max_chunks\":0,\"include_extensions\":[\".bpl_json\"],\"exclude_dirs\":[],\"enrich_regex\":false,\"enrich_llm\":true}}"

echo.
echo.
echo Index resume submitted. Check status:
echo   index_status.bat
echo.
pause
