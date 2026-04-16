# Orchestrates the full WAX Blueprint re-index pipeline:
#   Phase 1  parse facts            (CPU, ~30 sec)
#   Phase 2  generate purposes      (GPU via llama-server, ~10-15 min)
#   Phase 3  embed + index to ES    (GPU via embedding service, ~2-3 min)
#
# The GPU is the serialization point — embedding model and LLM cannot coexist in
# VRAM at the same time, so this script stops/starts them between phases.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File G:/Proj/Wick/scripts/run_full_reindex.ps1
#
# Flags:
#   -SkipPhase1 -SkipPhase2 -SkipPhase3    # skip individual phases
#   -RecreateIndex                          # drop wax_bp_v1 before Phase 3
#   -Limit N                                # process only first N BPs (for testing)
#   -UseGlm                                 # use GLM-4.7-Flash instead of Qwen for purposes

param(
    [switch]$SkipPhase1,
    [switch]$SkipPhase2,
    [switch]$SkipPhase3,
    [switch]$RecreateIndex,
    [int]$Limit = 0,
    [switch]$UseGlm
)

$ErrorActionPreference = "Stop"
$Root = "G:\Proj\Wick"
$Py = Join-Path $Root "py_env\Scripts\python.exe"
$EmbedApp = Join-Path $Root "services\embedding"
$LlamaBat = Join-Path $Root "scripts\start_llama.bat"

function Step($msg) { Write-Host "`n==== $msg ====" -ForegroundColor Cyan }
function Info($msg) { Write-Host "    $msg" -ForegroundColor Gray }

# ── Helpers for service lifecycle ───────────────────────────────────────────

function Stop-EmbedService {
    Step "Stopping embedding service (free VRAM for llama-server)"
    # uvicorn runs as python.exe with "uvicorn app:app" in the cmdline.
    # Kill only processes matching that pattern to avoid hitting unrelated pythons.
    $procs = Get-CimInstance Win32_Process | Where-Object {
        $_.Name -eq "python.exe" -and $_.CommandLine -like "*uvicorn*app:app*8088*"
    }
    foreach ($p in $procs) {
        Info "killing pid=$($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 2
    # Verify port freed
    try {
        $r = Invoke-WebRequest -Uri "http://127.0.0.1:8088/health" -TimeoutSec 1 -UseBasicParsing -ErrorAction Stop
        Info "warning: :8088 still responding"
    } catch {
        Info "port 8088 free"
    }
}

function Stop-LlamaServer {
    Step "Stopping llama-server (free VRAM for embedding)"
    $procs = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq "llama-server.exe" }
    foreach ($p in $procs) {
        Info "killing pid=$($p.ProcessId)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 3
}

function Start-LlamaServer {
    param([switch]$Glm)
    Step "Starting llama-server"
    $args = if ($Glm) { "--glm" } else { "" }
    # Launch in a new console so we can keep track of it via process name.
    Start-Process -FilePath $LlamaBat -ArgumentList $args -WindowStyle Minimized
    Info "waiting for /v1/models on :8090 (up to 90s)"
    for ($i = 0; $i -lt 45; $i++) {
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:8090/v1/models" -TimeoutSec 2 -UseBasicParsing
            if ($r.StatusCode -eq 200) {
                Info "llama-server ready"
                return
            }
        } catch { Start-Sleep -Seconds 2 }
    }
    throw "llama-server did not become ready in 90s"
}

function Start-EmbedService {
    Step "Starting embedding service on :8088"
    $cmd = "& `"$Py`" -m uvicorn app:app --host 127.0.0.1 --port 8088 --log-level info"
    Start-Process -FilePath "powershell.exe" -ArgumentList "-NoExit","-Command","cd `"$EmbedApp`"; $cmd" -WindowStyle Minimized
    Info "waiting for /health on :8088 (up to 60s)"
    for ($i = 0; $i -lt 30; $i++) {
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:8088/health" -TimeoutSec 2 -UseBasicParsing
            if ($r.StatusCode -eq 200) {
                $body = $r.Content | ConvertFrom-Json
                if ($body.status -eq "ok") {
                    Info "embedding service ready: model=$($body.model) dim=$($body.dim) vram=$($body.vram_mb) MB"
                    return
                }
            }
        } catch { Start-Sleep -Seconds 2 }
    }
    throw "embedding service did not become ready in 60s"
}

# ── Phase 1: parse facts (CPU only) ─────────────────────────────────────────

if (-not $SkipPhase1) {
    Step "Phase 1: parse structural facts from .bpl_json"
    & $Py "$Root\scripts\parse_bp_facts.py"
    if ($LASTEXITCODE -ne 0) { throw "Phase 1 failed (exit=$LASTEXITCODE)" }
}

# ── Phase 2: generate purposes via llama-server ─────────────────────────────

if (-not $SkipPhase2) {
    # Ensure embedding is stopped (VRAM)
    Stop-EmbedService
    Start-LlamaServer -Glm:$UseGlm
    try {
        Step "Phase 2: generate BP purposes via LLM"
        $pArgs = @("$Root\scripts\generate_bp_purposes.py")
        if ($Limit -gt 0) { $pArgs += @("--limit", "$Limit") }
        & $Py @pArgs
        if ($LASTEXITCODE -ne 0) { throw "Phase 2 failed (exit=$LASTEXITCODE)" }
    } finally {
        Stop-LlamaServer
    }
}

# ── Phase 3: embed + bulk index to ES ───────────────────────────────────────

if (-not $SkipPhase3) {
    Start-EmbedService
    Step "Phase 3: embed + index to Elasticsearch"
    $pArgs = @("$Root\scripts\index_bp_to_es.py")
    if ($RecreateIndex) { $pArgs += "--recreate" }
    if ($Limit -gt 0) { $pArgs += @("--limit", "$Limit") }
    & $Py @pArgs
    if ($LASTEXITCODE -ne 0) { throw "Phase 3 failed (exit=$LASTEXITCODE)" }
}

Step "All phases complete"
Write-Host "  - Kibana:       http://127.0.0.1:5601" -ForegroundColor Green
Write-Host "  - ES:           http://127.0.0.1:9200/wax_bp_v1/_count" -ForegroundColor Green
Write-Host "  - Embed svc:    http://127.0.0.1:8088/health" -ForegroundColor Green
