@echo off
REM ============================================================
REM  WAX RAG Server startup script
REM  OpenAI Responses API + gpt-5-mini preset
REM ============================================================

set OPENAI_API_KEY=REDACTED-OPENAI-KEY
set WAXCPP_ENRICH_LLM_CONCURRENCY=4
if not defined OPENAI_API_KEY if not defined WAXCPP_OPENAI_API_KEY (
  echo ============================================================
  echo  ERROR: OPENAI_API_KEY is not set.
  echo  Set OPENAI_API_KEY or WAXCPP_OPENAI_API_KEY and run again.
  echo ============================================================
  echo.
  pause
  exit /b 1
)

set WAXCPP_GENERATION_RUNTIME=openai
set WAXCPP_GENERATION_MODEL=gpt-5-mini
set WAXCPP_OPENAI_BASE_URL=https://api.openai.com
set WAXCPP_OPENAI_REASONING_EFFORT=low
set WAXCPP_RPC_LOG=0
set WAXCPP_SERVER_LOG=0
set WAXCPP_ENRICH_LLM_LOG=0
set WAXCPP_ENRICH_LLM_MAX_TOKENS=768
set WAXCPP_RECALL_LOG=G:\Proj\Wick\build\bin\base\recall.log
set WAXCPP_AUTO_FLUSH_INTERVAL_MS=30000
set WAXCPP_OPENAI_TIMEOUT_MS=180000
set WAXCPP_RAG_MAX_SNIPPETS=12
set WAXCPP_RAG_SEARCH_TOP_K=12

set WAXCPP_OPENAI_API_KEY=%OPENAI_API_KEY%

echo ============================================================
echo  WAX RAG Server
echo  API: http://127.0.0.1:8080  (JSON-RPC)
echo  Store: wax-server.mv2s (current directory)
echo  Provider: OpenAI Responses API
echo  Runtime:  %WAXCPP_GENERATION_RUNTIME%
echo  Model:    %WAXCPP_GENERATION_MODEL%
echo  Base URL: %WAXCPP_OPENAI_BASE_URL%
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
