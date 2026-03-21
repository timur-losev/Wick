@echo off
REM ============================================================
REM  WAX RAG Server startup script
REM  Requires llama-server running on 127.0.0.1:8004
REM  (start with: g:\Proj\Agents1\start_llama.bat)
REM ============================================================

REM --- Model paths (required by validation even with remote llama-server) ---
set WAXCPP_GENERATION_MODEL=g:\Proj\Agents1\Models\Qwen\Qwen3-Coder-Next-Q4_K_M.gguf
set WAXCPP_LLAMA_CPP_ROOT=g:\Proj\Agents1\llama-cpp

REM --- llama.cpp connection (matches start_llama.bat) ---
set WAXCPP_LLAMA_API_KEY=1f67a5931a61dfcf7622c7401ff88008

REM --- Generation endpoint (default http://127.0.0.1:8004/completion) ---
REM set WAXCPP_LLAMA_GEN_ENDPOINT=http://127.0.0.1:8004/completion

REM --- Embedding (disabled by default; Qwen3-Coder may not expose /embedding) ---
REM To enable vector search, create a JSON config and set WAXCPP_SERVER_CONFIG.

REM --- Server logging ---
REM `--rpc-log verbose` is the primary switch; env stays as a fallback for older launches.
set WAXCPP_RPC_LOG=verbose
set WAXCPP_SERVER_LOG=1

REM --- LLM enrichment logging (1=verbose prompts+responses, 0=compact one-liners) ---
set WAXCPP_ENRICH_LLM_LOG=1

REM --- LLM enrichment max tokens (default 1024) ---
set WAXCPP_ENRICH_LLM_MAX_TOKENS=4096

REM --- Recall request/response logging (file path, empty=disabled) ---
set WAXCPP_RECALL_LOG=G:\Proj\Wick\build\bin\data\recall.log

REM --- Auto-flush interval (ms, 0=disabled, default 30000) ---
set WAXCPP_AUTO_FLUSH_INTERVAL_MS=30000

REM --- Generation tuning ---
set WAXCPP_LLAMA_GEN_TIMEOUT_MS=600000
REM set WAXCPP_LLAMA_GEN_MAX_RETRIES=3

REM --- RAG recall tuning ---
set WAXCPP_RAG_MAX_SNIPPETS=12
set WAXCPP_RAG_SEARCH_TOP_K=12

REM --- Orchestrator tuning for large repos ---
REM set WAXCPP_ORCH_INGEST_CONCURRENCY=1
REM set WAXCPP_ORCH_INGEST_BATCH_SIZE=32

echo ============================================================
echo  WAX RAG Server
echo  API: http://127.0.0.1:8080  (JSON-RPC)
echo  Store: wax-server.mv2s (current directory)
echo  Model: %WAXCPP_GENERATION_MODEL%
echo  llama-cpp root: %WAXCPP_LLAMA_CPP_ROOT%
echo ============================================================
echo.

cd /d "G:\Proj\Wick\build\bin"
if exist waxcpp_rag_server.exe (
  waxcpp_rag_server.exe --rpc-log verbose
) else (
  waxcpp_rag_server_d.exe --rpc-log verbose
)
