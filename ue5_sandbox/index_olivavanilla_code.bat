@echo off
setlocal
REM ============================================================
REM  Index OlivaVanilla C++ source code with LLM enrichment
REM
REM  Scans Source/ and Plugins/ for .h/.hpp/.cpp/.inl/.inc
REM  ~930 files, LLM enrichment extracts structured facts
REM  resume=true reuses the previous file manifest and skips unchanged files
REM
REM  Requires:
REM    - WAX server running on 127.0.0.1:8080
REM    - llama-server running on 127.0.0.1:8004
REM ============================================================

set PROJECT_ROOT=J:\UE4\Projects\OlivaVanilla
set CHECKPOINT_NAMESPACE=olivavanilla_code
set PROJECT_ROOT_FWD=%PROJECT_ROOT:\=/%
for %%I in ("%~dp0..\build\bin\base") do set "SERVER_DATA_DIR=%%~fI"
set "CHECKPOINT_FILE=%SERVER_DATA_DIR%\wax-server.mv2s.index.%CHECKPOINT_NAMESPACE%.checkpoint"
set "FILE_MANIFEST=%CHECKPOINT_FILE%.file_manifest"
set "LEGACY_CHECKPOINT_FILE=%SERVER_DATA_DIR%\wax-server.mv2s.index.checkpoint"
set "LEGACY_FILE_MANIFEST=%LEGACY_CHECKPOINT_FILE%.file_manifest"

echo ============================================================
echo  OlivaVanilla C++ Code Indexing (with LLM enrichment)
echo  Source: %PROJECT_ROOT%
echo  Extensions: .h .hpp .cpp .inl .inc
echo  enrich_regex=true, enrich_llm=true
echo  resume=auto, file-level reuse when checkpoint exists
echo ============================================================
echo.
echo Preflight:
echo   Checkpoint: %CHECKPOINT_FILE%
echo   File manifest: %FILE_MANIFEST%
echo.

set "RESUME_MODE=true"
set "RESUME_REASON=existing checkpoint"

if not exist "%CHECKPOINT_FILE%" (
    if exist "%LEGACY_CHECKPOINT_FILE%" (
        findstr /b /c:"repo_root=%PROJECT_ROOT_FWD%" "%LEGACY_CHECKPOINT_FILE%" >nul
        if not errorlevel 1 (
            copy /Y "%LEGACY_CHECKPOINT_FILE%" "%CHECKPOINT_FILE%" >nul
            if exist "%LEGACY_FILE_MANIFEST%" (
                copy /Y "%LEGACY_FILE_MANIFEST%" "%FILE_MANIFEST%" >nul
            )
            echo Migrated legacy baseline into %CHECKPOINT_FILE%.
            echo.
        )
    )
)

if not exist "%CHECKPOINT_FILE%" (
    set "RESUME_MODE=false"
    set "RESUME_REASON=checkpoint missing"
) else if not exist "%FILE_MANIFEST%" (
    set "RESUME_MODE=false"
    set "RESUME_REASON=file manifest missing"
) else (
    findstr /b /c:"repo_root=%PROJECT_ROOT_FWD%" "%CHECKPOINT_FILE%" >nul
    if errorlevel 1 (
        set "RESUME_MODE=false"
        set "RESUME_REASON=checkpoint repo_root mismatch"
    ) else (
        for %%I in ("%FILE_MANIFEST%") do set "FILE_MANIFEST_SIZE=%%~zI"
        if "%FILE_MANIFEST_SIZE%"=="0" (
            set "RESUME_MODE=false"
            set "RESUME_REASON=file manifest empty"
        )
    )
)

if /I "%RESUME_MODE%"=="true" (
    echo Preflight ok: resume baseline found.
    echo   repo_root:     %PROJECT_ROOT_FWD%
    echo   checkpoint:    %CHECKPOINT_FILE%
    echo   file_manifest: %FILE_MANIFEST%
) else (
    echo Preflight notice: running bootstrap/full pass.
    echo   reason:        %RESUME_REASON%
    echo   repo_root:     %PROJECT_ROOT_FWD%
)
echo.

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.start\",\"params\":{\"repo_root\":\"%PROJECT_ROOT:\=/%\",\"resume\":%RESUME_MODE%,\"checkpoint_namespace\":\"%CHECKPOINT_NAMESPACE%\",\"flush_every_chunks\":1000,\"ingest_batch_size\":1,\"target_tokens\":400,\"max_chunks\":0,\"include_extensions\":[\".h\",\".hpp\",\".cpp\",\".inl\",\".inc\"],\"exclude_dirs\":[\"DerivedDataCache\",\"Intermediate\",\"Saved\",\"Binaries\",\"Build\",\"Content\"],\"enrich_regex\":true,\"enrich_llm\":true}}"

echo.
echo.
echo Index job submitted. Check status with:
echo   index_status.bat
echo.
echo Stop indexing with:
echo   curl -s -X POST http://127.0.0.1:8080/ -H "Content-Type: application/json" -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.stop\",\"params\":{}}"
echo.
pause
