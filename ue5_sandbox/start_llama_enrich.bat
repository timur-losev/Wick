@echo off
REM ============================================================
REM  llama-server optimized for WAX LLM enrichment
REM  Small context (8K) -> all layers on GPU -> 10-15x faster
REM  Port 8004 (same as default, stop main llama-server first!)
REM ============================================================
REM set MODEL=g:\Proj\Agents1\Models\Qwen\qwen2.5-coder-7b-instruct-q8_0.gguf
REM set MODEL=g:\Proj\Agents1\Models\Qwen\qwen2.5-coder-32b-instruct-q4_k_m\qwen2.5-coder-32b-instruct-q4_k_m.gguf
set MODEL=g:\Proj\Agents1\Models\Qwen\qwen3-coder-30b-a3b-instruct-q4_k_s.gguf
set API_KEY=1f67a5931a61dfcf7622c7401ff88008

echo ============================================================
echo  llama-server (ENRICHMENT mode)
echo  Model: %MODEL%
echo  URL:   http://127.0.0.1:8004
echo  Context: 4096 (compressed enrichment prompts are ~200-500 tokens)
echo  GPU layers: 99 (all layers on GPU)
echo ============================================================
echo.

g:\Proj\Agents1\llama-cpp\llama-server.exe ^
  --model "%MODEL%" ^
  --host 127.0.0.1 ^
  --port 8004 ^
  --api-key %API_KEY% ^
  --n-gpu-layers 99 ^
  --ctx-size 4096 ^
  --parallel 3 ^
  --threads 16 ^
  --alias qwen3-coder-30b-a3b-instruct
