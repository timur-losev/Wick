@echo off
setlocal
REM ============================================================
REM  llama-server preset for WAX C++ enrichment
REM  Model: qwen2.5-coder-7b-instruct-q8_0
REM  Port:  8004
REM ============================================================

set "MODEL=G:\Proj\Agents1\Models\Qwen\qwen2.5-coder-7b-instruct-q8_0.gguf"
set "LLAMA_SERVER=G:\Proj\Agents1\llama-cpp\llama-server.exe"
set "API_KEY=1f67a5931a61dfcf7622c7401ff88008"

if not exist "%MODEL%" (
  echo ERROR: model file not found:
  echo   %MODEL%
  pause
  exit /b 1
)

if not exist "%LLAMA_SERVER%" (
  echo ERROR: llama-server executable not found:
  echo   %LLAMA_SERVER%
  pause
  exit /b 1
)

echo ============================================================
echo  llama-server (C++ enrichment mode)
echo  Model: %MODEL%
echo  URL:   http://127.0.0.1:8004
echo  Context: 8192
echo  GPU layers: 99
echo ============================================================
echo.

"%LLAMA_SERVER%" ^
  --model "%MODEL%" ^
  --host 127.0.0.1 ^
  --port 8004 ^
  --api-key %API_KEY% ^
  --n-gpu-layers 99 ^
  --ctx-size 8192 ^
  --threads 16 ^
  --alias qwen2.5-coder-7b-instruct

