@echo off
REM ============================================================
REM  Export Oliva Blueprints to JSON (export only)
REM
REM  Step 1: UnrealEditor-Cmd exports all BP_* graphs to JSON
REM
REM  Requires:
REM    - Oliva project compiled with OlivaBlueprintRAG plugin
REM ============================================================

set UE_EDITOR=J:\UE5.2SRC\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
set UPROJECT=J:\UE4\Projects\perforce_DESKTOP-UGMULAU_trunk_1659\Oliva\LyraStarterGame.uproject
set EXPORT_DIR=J:\UE4\Projects\perforce_DESKTOP-UGMULAU_trunk_1659\Oliva\Saved\BlueprintExports

echo ============================================================
echo  Oliva Blueprint Export (JSON only)
echo ============================================================
echo.

echo Exporting Blueprints to JSON ...
echo   Editor:  %UE_EDITOR%
echo   Project: %UPROJECT%
echo   Output:  %EXPORT_DIR%
echo.

"%UE_EDITOR%" "%UPROJECT%" -run=BlueprintGraphExport -Root=/Game -Prefix=BP_ -ExportDir="%EXPORT_DIR%" -unattended -nop4

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Blueprint export failed (exit code %ERRORLEVEL%).
    echo        Make sure OlivaBlueprintRAG plugin is compiled.
    pause
    exit /b 1
)

echo.
echo Export complete. Counting files...
for /f %%A in ('dir /b /a-d "%EXPORT_DIR%\*.bpl_json" 2^>nul ^| find /c /v ""') do set JSON_COUNT=%%A
echo   Found %JSON_COUNT% JSON files.
echo.
pause
