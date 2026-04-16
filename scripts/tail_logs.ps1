# Tail every WAX log file in one window so you can watch an agent session live.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File G:/Proj/Wick/scripts/tail_logs.ps1
#
# Flags:
#   -Clear        # truncate all log files before starting (fresh session)
#   -What <kind>  # only one stream: mcp | refresh | recall | remember
#
# By default tails every known log concurrently, prefixing each line with
# [source] so it's obvious which subsystem emitted it.

param(
    [switch]$Clear,
    [ValidateSet("all", "mcp", "refresh", "recall", "remember")]
    [string]$What = "all"
)

$ErrorActionPreference = "Stop"
$LogsDir = "G:\Proj\Wick\logs"
New-Item -ItemType Directory -Force -Path $LogsDir | Out-Null

# All known logs: path → short label
$Logs = [ordered]@{
    "mcp"      = "$LogsDir\mcp_tool_calls.log"
    "refresh"  = "$LogsDir\bp_refresh.log"
    "recall"   = "$LogsDir\recall.log"     # populated when WAXCPP_RECALL_LOG env var points here
    "remember" = "$LogsDir\remember.log"   # populated when WAXCPP_REMEMBER_LOG env var points here
}

# Select which streams to follow
$selected = if ($What -eq "all") { $Logs.Keys } else { @($What) }

# Create files up-front so Get-Content -Wait can attach even before the
# backing service has written anything.
foreach ($k in $selected) {
    $p = $Logs[$k]
    if (-not (Test-Path $p)) { New-Item -ItemType File -Force -Path $p | Out-Null }
}

# Optional truncate
if ($Clear) {
    Write-Host "Clearing log files..." -ForegroundColor Yellow
    foreach ($k in $selected) { Clear-Content $Logs[$k] -ErrorAction SilentlyContinue }
}

Write-Host "Tailing WAX session logs. Press Ctrl+C to stop." -ForegroundColor Cyan
Write-Host "Dir: $LogsDir`n" -ForegroundColor Gray

# One Get-Content -Wait per stream, each on its own background job with a
# colourised prefix so the output interleaves sensibly.
$colors = @{
    mcp      = "Cyan"
    refresh  = "Green"
    recall   = "Magenta"
    remember = "Yellow"
}

$jobs = @()
foreach ($k in $selected) {
    $path  = $Logs[$k]
    $label = "[$k]".PadRight(10)
    $col   = $colors[$k]
    $jobs += Start-Job -ArgumentList $path, $label, $col -ScriptBlock {
        param($p, $label, $col)
        Get-Content -Path $p -Wait -Tail 0 | ForEach-Object {
            # Colour can't cross job boundary easily — just emit with label.
            "$label  $_"
        }
    }
}

# Poll the jobs and stream their output to the console.
try {
    while ($true) {
        foreach ($j in $jobs) {
            Receive-Job -Job $j -Keep:$false | ForEach-Object {
                $line = $_
                # Extract label to colourise
                if ($line -match "^\[(\w+)\]") {
                    $tag = $Matches[1]
                    $col = $colors[$tag]
                    if ($col) { Write-Host $line -ForegroundColor $col; continue }
                }
                Write-Host $line
            }
        }
        Start-Sleep -Milliseconds 250
    }
} finally {
    foreach ($j in $jobs) { Stop-Job -Job $j -ErrorAction SilentlyContinue; Remove-Job -Job $j -Force }
}
