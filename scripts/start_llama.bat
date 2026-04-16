@echo off
REM Starts llama-server for WAX purpose generation.
REM
REM Default model: qwen2.5-coder-7b-instruct-q8_0 (smaller, faster for single-sentence tasks)
REM Alt: GLM-4.7-Flash (pass --glm to use this instead)
REM
REM Port: 8090 (distinct from WAX :8080 and embedding :8088)
REM API: OpenAI-compatible at http://127.0.0.1:8090/v1/chat/completions

setlocal enabledelayedexpansion

set LLAMA_SERVER=g:\Proj\Agents1\llama-cpp\llama-server.exe
set MODEL_QWEN=g:\Proj\Agents1\Models\Qwen\qwen2.5-coder-7b-instruct-q8_0.gguf
set MODEL_GLM=g:\Proj\Agents1\Models\GLM-4.7-Flash-UD-Q5_K_XL.gguf

set MODEL=%MODEL_QWEN%
set ALIAS=qwen2.5-coder-7b

if /I "%~1"=="--glm" (
    set MODEL=%MODEL_GLM%
    set ALIAS=glm-4.7-flash
)

if not exist "%LLAMA_SERVER%" (
    echo ERROR: llama-server not found at %LLAMA_SERVER%
    exit /b 1
)
if not exist "%MODEL%" (
    echo ERROR: model not found at %MODEL%
    exit /b 1
)

echo ============================================================
echo Starting llama-server for WAX purpose generation
echo   Server:  %LLAMA_SERVER%
echo   Model:   %MODEL%
echo   Alias:   %ALIAS%
echo   Port:    8090
echo ============================================================

"%LLAMA_SERVER%" ^
  --model "%MODEL%" ^
  --host 127.0.0.1 ^
  --port 8090 ^
  --alias %ALIAS% ^
  --n-gpu-layers 99 ^
  --ctx-size 8192 ^
  --threads 8 ^
  --parallel 1 ^
  --batch-size 512 ^
  --no-mmap ^
  --flash-attn on

endlocal
