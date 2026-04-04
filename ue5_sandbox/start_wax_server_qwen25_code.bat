@echo off
setlocal
REM ============================================================
REM  WAX RAG Server startup script
REM  Local llama-server + qwen2.5-coder-7b-instruct-q8_0 preset
REM ============================================================

set "WAXCPP_GENERATION_MODEL=G:\Proj\Agents1\Models\Qwen\qwen2.5-coder-7b-instruct-q8_0.gguf"
set "WAXCPP_LLAMA_CPP_ROOT=G:\Proj\Agents1\llama-cpp"
set "WAXCPP_LLAMA_API_KEY=1f67a5931a61dfcf7622c7401ff88008"
set "WAXCPP_RPC_LOG=0"
set "WAXCPP_SERVER_LOG=0"
set "WAXCPP_ENRICH_LLM_LOG=0"
set "WAXCPP_ENRICH_LLM_MAX_TOKENS=1024"
set "WAXCPP_ENRICH_LLM_CONCURRENCY=2"
set "WAXCPP_RECALL_LOG=G:\Proj\Wick\build\bin\base\recall.log"
set "WAXCPP_AUTO_FLUSH_INTERVAL_MS=30000"
set "WAXCPP_LLAMA_GEN_TIMEOUT_MS=600000"
set "WAXCPP_RAG_MAX_SNIPPETS=12"
set "WAXCPP_RAG_SEARCH_TOP_K=12"

if not exist "%WAXCPP_GENERATION_MODEL%" (
  echo ERROR: generation model file not found:
  echo   %WAXCPP_GENERATION_MODEL%
  pause
  exit /b 1
)

if not exist "%WAXCPP_LLAMA_CPP_ROOT%\llama-server.exe" (
  echo ERROR: llama.cpp root does not contain llama-server.exe:
  echo   %WAXCPP_LLAMA_CPP_ROOT%
  pause
  exit /b 1
)

echo ============================================================
echo  WAX RAG Server
echo  API: http://127.0.0.1:8080  (JSON-RPC)
echo  Store: wax-server.mv2s (current directory)
echo  Runtime: llama_cpp
echo  Model:   %WAXCPP_GENERATION_MODEL%
echo  LLM:     http://127.0.0.1:8004
echo  LLM max tokens: %WAXCPP_ENRICH_LLM_MAX_TOKENS%
echo  LLM concurrency: %WAXCPP_ENRICH_LLM_CONCURRENCY%
echo ============================================================
echo.

cd /d "G:\Proj\Wick\build\bin"
if exist waxcpp_rag_server.exe (
  waxcpp_rag_server.exe
  goto :EOF
)

if exist waxcpp_rag_server_d.exe (
  waxcpp_rag_server_d.exe
  goto :EOF
)

echo ERROR: waxcpp_rag_server executable not found in G:\Proj\Wick\build\bin
pause
exit /b 1
