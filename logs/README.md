# WAX session logs

This directory captures everything WAX does during an agent feedback session,
so that results can be reviewed afterwards.

## Log files

| File | Produced by | Contents |
|------|-------------|----------|
| `mcp_tool_calls.log` | `mcp/wax_mcp_server.js` | One JSON line per MCP `tools/call`: tool name, args preview, result preview, elapsed ms, error if any |
| `bp_refresh.log` | `services/embedding/app.py` | One JSON line per `/bp_refresh` or `/bp_reindex_all` request with status, hashes, timing |
| `recall.log` | `server/wax_rag_handler.cpp` (C++ WAX server) | Human-readable block per `recall` call with items and scores. Requires `WAXCPP_RECALL_LOG` env var pointing here |
| `remember.log` | `server/wax_rag_handler.cpp` | One block per `remember` call with metadata and payload preview. Requires `WAXCPP_REMEMBER_LOG` env var |

All `*.log` files are gitignored — only this README and `.gitkeep` are tracked.

## Enabling the C++ server logs

The MCP and embedding-service logs are on by default. The C++ server logs
require environment variables set when you launch `waxcpp_rag_server.exe`.
From PowerShell:

```powershell
$env:WAXCPP_RECALL_LOG   = "G:\Proj\Wick\logs\recall.log"
$env:WAXCPP_REMEMBER_LOG = "G:\Proj\Wick\logs\remember.log"
.\build\bin\waxcpp_rag_server.exe
```

## Watching live

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\tail_logs.ps1
# only one stream:
powershell -ExecutionPolicy Bypass -File .\scripts\tail_logs.ps1 -What mcp
# clear everything and start fresh:
powershell -ExecutionPolicy Bypass -File .\scripts\tail_logs.ps1 -Clear
```

## Disabling

Per-service env vars, all accept `"off"`:
- `WAX_MCP_LOG=off`         → disable MCP logging
- `WAX_BP_REFRESH_LOG=off`  → disable bp_refresh logging
- Unset `WAXCPP_RECALL_LOG` / `WAXCPP_REMEMBER_LOG` to disable the C++ paths

## Formats

### mcp_tool_calls.log — JSON Lines

```json
{"ts":"2026-04-16T22:30:15.123Z","tool":"wax_bp_semantic_search","args":"{\"query\":\"...\"}","elapsed_ms":87,"is_error":false,"result_preview":"Found 5 Blueprint(s)..."}
{"ts":"2026-04-16T22:30:20.456Z","tool":"wax_bp_facts","args":"{\"entity\":\"bp:GA_X\"}","elapsed_ms":5,"is_error":true,"error":"...","stack":"..."}
```

### bp_refresh.log — JSON Lines

```json
{"ts":"...","kind":"single","entity":"bp:GA_SpawnEffect","export_dir":"J:\\Temp\\...","status":"unchanged","structural_hash":"c47767a9707f32ae","elapsed_ms":4.3}
{"ts":"...","kind":"bulk","scanned_all":true,"total":655,"updated":0,"unchanged":640,"indexed":15,"elapsed_ms":5700}
```

### recall.log / remember.log — pretty blocks

Separator lines, timestamps, query text, top results. See the C++ source
(`server/wax_rag_handler.cpp`) for the exact format.

## Quick analysis snippets

Count tool calls by name today:

```powershell
Get-Content logs\mcp_tool_calls.log | ForEach-Object {
    ($_ | ConvertFrom-Json).tool
} | Group-Object | Sort-Object Count -Descending
```

Show errors only:

```powershell
Get-Content logs\mcp_tool_calls.log | ConvertFrom-Json | Where-Object is_error
```

Average latency per tool:

```powershell
Get-Content logs\mcp_tool_calls.log | ConvertFrom-Json |
    Group-Object tool |
    ForEach-Object {
        [pscustomobject]@{
            tool     = $_.Name
            n        = $_.Count
            avg_ms   = [math]::Round(($_.Group | Measure-Object elapsed_ms -Average).Average, 0)
        }
    } | Sort-Object avg_ms -Descending
```
