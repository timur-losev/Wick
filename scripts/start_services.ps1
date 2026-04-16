# Start both services needed for WAX semantic BP search:
#   1. Elasticsearch + Kibana (Docker Compose)
#   2. Embedding service (uvicorn on port 8088)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File G:/Proj/Wick/scripts/start_services.ps1
#
# Flags:
#   -NoDocker       # skip ES (useful if already running)
#   -NoEmbedding    # skip embedding svc
#   -StopAll        # stop everything

param(
    [switch]$NoDocker,
    [switch]$NoEmbedding,
    [switch]$StopAll
)

$ErrorActionPreference = "Stop"
$RepoRoot   = "G:\Proj\Wick"
$DockerDir  = Join-Path $RepoRoot "docker"
$VenvPython = Join-Path $RepoRoot "py_env\Scripts\python.exe"
$AppDir     = Join-Path $RepoRoot "services\embedding"

function Step($msg) { Write-Host "`n==== $msg ====" -ForegroundColor Cyan }

if ($StopAll) {
    Step "Stopping Elasticsearch + Kibana"
    Push-Location $DockerDir
    docker compose down
    Pop-Location
    Step "Stopping uvicorn (if running)"
    Get-Process | Where-Object { $_.ProcessName -eq "python" -and $_.CommandLine -match "uvicorn" } |
        ForEach-Object { Stop-Process -Id $_.Id -Force }
    exit 0
}

# ── Elasticsearch + Kibana ──────────────────────────────────────────────────
if (-not $NoDocker) {
    Step "Starting Elasticsearch + Kibana via Docker Compose"
    Push-Location $DockerDir
    docker compose up -d
    Pop-Location

    Step "Waiting for Elasticsearch (up to 60 seconds)"
    $ready = $false
    for ($i = 0; $i -lt 30; $i++) {
        try {
            $r = Invoke-WebRequest -Uri "http://127.0.0.1:9200/_cluster/health?wait_for_status=yellow&timeout=2s" -UseBasicParsing -TimeoutSec 5
            if ($r.StatusCode -eq 200) { $ready = $true; break }
        } catch { Start-Sleep -Seconds 2 }
    }
    if ($ready) {
        Write-Host "Elasticsearch ready at http://127.0.0.1:9200" -ForegroundColor Green
        Write-Host "Kibana at        http://127.0.0.1:5601" -ForegroundColor Green
    } else {
        Write-Host "Elasticsearch did not become healthy in time. Check: docker compose logs elasticsearch" -ForegroundColor Red
    }
}

# ── Embedding service ───────────────────────────────────────────────────────
if (-not $NoEmbedding) {
    if (-not (Test-Path $VenvPython)) {
        Write-Host "venv missing — run setup_embedding.ps1 first" -ForegroundColor Red
        exit 1
    }
    Step "Starting embedding service on 127.0.0.1:8088"
    Write-Host "Service logs will print below. Ctrl+C stops only this service (ES keeps running)."
    Push-Location $AppDir
    & $VenvPython -m uvicorn app:app --host 127.0.0.1 --port 8088 --log-level info
    Pop-Location
}
