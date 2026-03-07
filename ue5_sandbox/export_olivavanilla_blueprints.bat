@echo off
REM ============================================================
REM  Export OlivaVanilla Blueprints to .bpl_json (export only)
REM
REM  Requires:
REM    - OlivaVanilla project compiled with OlivaBlueprintRAG plugin
REM ============================================================

set UE_EDITOR=J:\UE5.2SRC\Engine\Binaries\Win64\UnrealEditor-Cmd.exe
set UPROJECT=J:\UE4\Projects\OlivaVanilla\LyraStarterGame.uproject
set EXPORT_DIR=J:\UE4\Projects\OlivaVanilla\Saved\BlueprintExports

echo ============================================================
echo  OlivaVanilla Blueprint Export (.bpl_json)
echo ============================================================
echo.

echo Exporting Blueprints to .bpl_json ...
echo   Editor:  %UE_EDITOR%
echo   Project: %UPROJECT%
echo   Output:  %EXPORT_DIR%
echo.

REM  -Root=/ scans ALL mount points (game + plugins), not just /Game
REM  AssetRegistry already filters by UBlueprint class — no prefix needed.
REM  Optional: -Prefix=BP_,B_Lyra  -PathContains=Blueprints  to narrow scope.
"%UE_EDITOR%" "%UPROJECT%" -run=BlueprintGraphExport -Root=/ -ExportDir="%EXPORT_DIR%" -unattended -nop4

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Blueprint export failed (exit code %ERRORLEVEL%).
    echo        Make sure OlivaBlueprintRAG plugin is compiled.
    pause
    exit /b 1
)

echo.
echo Export complete. Counting files...
for /f %%A in ('dir /b /a-d "%EXPORT_DIR%\*.bpl_json" 2^>nul ^| find /c /v ""') do set BPL_COUNT=%%A
echo   Found %BPL_COUNT% .bpl_json files.
echo.
pause
