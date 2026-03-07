@echo off
REM ============================================================
REM  Check WAX indexing status
REM  Shows: phase, chunks indexed/committed, speed, RAM usage
REM ============================================================

curl -s -X POST http://127.0.0.1:8080/ ^
  -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"index.status\",\"params\":{}}"

echo.
