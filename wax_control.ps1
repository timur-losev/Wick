# ============================================================
#  WAX RAG Server - Interactive Control Panel (PowerShell)
#  Usage: powershell -ExecutionPolicy Bypass -File wax_control.ps1
# ============================================================

param(
    [string]$WaxUrl = "http://127.0.0.1:8080"
)

$Host.UI.RawUI.WindowTitle = "WAX Control Panel"

# --- Helpers ---

function Invoke-WaxRpc {
    param(
        [string]$Method,
        [hashtable]$Params = @{}
    )
    $body = @{
        jsonrpc = "2.0"
        id      = 1
        method  = $Method
        params  = $Params
    } | ConvertTo-Json -Depth 5 -Compress

    try {
        $resp = Invoke-RestMethod -Uri $WaxUrl -Method Post `
            -ContentType "application/json" -Body $body -TimeoutSec 120
        return $resp
    }
    catch {
        return @{ error = $_.Exception.Message }
    }
}

function Format-Duration {
    param([long]$Ms)
    $ts = [TimeSpan]::FromMilliseconds($Ms)
    if ($ts.TotalHours -ge 1) { return "{0:00}h {1:00}m {2:00}s" -f [int]$ts.TotalHours, $ts.Minutes, $ts.Seconds }
    if ($ts.TotalMinutes -ge 1) { return "{0:00}m {1:00}s" -f [int]$ts.TotalMinutes, $ts.Seconds }
    return "{0:0.0}s" -f $ts.TotalSeconds
}

function Format-Size {
    param([double]$MB)
    if ($MB -ge 1024) { return "{0:0.1} GB" -f ($MB / 1024) }
    return "{0:0} MB" -f $MB
}

function Format-Number {
    param([long]$N)
    return $N.ToString("N0")
}

function Write-Color {
    param([string]$Text, [string]$Color = "White")
    Write-Host $Text -ForegroundColor $Color -NoNewline
}

function Write-Header {
    Clear-Host
    Write-Host ""
    Write-Host "  =============================================" -ForegroundColor Cyan
    Write-Host "   WAX RAG Server - Control Panel" -ForegroundColor Cyan
    Write-Host "   $WaxUrl" -ForegroundColor DarkCyan
    Write-Host "  =============================================" -ForegroundColor Cyan
    Write-Host ""
}

function Show-IndexStatus {
    $s = Invoke-WaxRpc -Method "index.status"

    if ($s.error) {
        Write-Host "  [ERROR] $($s.error)" -ForegroundColor Red
        return $null
    }

    # State color
    $stateColor = switch ($s.state) {
        "running"   { "Green" }
        "completed" { "Cyan" }
        "stopped"   { "Yellow" }
        "failed"    { "Red" }
        default     { "Gray" }
    }
    $phaseColor = switch ($s.phase) {
        "scanning"  { "Yellow" }
        "ingesting" { "Green" }
        "flushing"  { "Magenta" }
        default     { "Gray" }
    }

    Write-Host "  INDEX STATUS" -ForegroundColor White
    Write-Host "  ----------------------------------------------------" -ForegroundColor DarkGray

    Write-Host "  State:       " -NoNewline; Write-Host "$($s.state)" -ForegroundColor $stateColor -NoNewline
    Write-Host "  |  Phase: " -NoNewline; Write-Host "$($s.phase)" -ForegroundColor $phaseColor
    Write-Host "  Job ID:      $($s.job_id)" -ForegroundColor DarkGray
    Write-Host "  Repo:        $($s.repo_root)" -ForegroundColor DarkGray
    Write-Host ""

    # Progress bar
    if ($s.state -eq "running" -and $s.indexed_chunks -gt 0) {
        # Use total_chunks from server if available, otherwise estimate
        if ($s.total_chunks -gt 0) {
            $estimatedTotal = $s.total_chunks
        } else {
            $estimatedTotal = [Math]::Max($s.scanned_files * 6, $s.indexed_chunks + 1)
        }
        $pct = [Math]::Min(99, [int](($s.indexed_chunks / $estimatedTotal) * 100))
        $barWidth = 40
        $filled = [int]($barWidth * $pct / 100)
        $empty = $barWidth - $filled
        $bar = ("{0}{1}" -f ("=" * $filled), ("-" * $empty))

        Write-Host "  Progress:    [" -NoNewline
        Write-Host $bar.Substring(0, $filled) -ForegroundColor Green -NoNewline
        Write-Host $bar.Substring($filled) -ForegroundColor DarkGray -NoNewline
        Write-Host "] ${pct}%  ($($s.indexed_chunks) / $estimatedTotal)" -ForegroundColor Green
        Write-Host ""
    }

    Write-Host "  Scanned:     $(Format-Number $s.scanned_files) files" -ForegroundColor White
    Write-Host "  Indexed:     $(Format-Number $s.indexed_chunks) chunks   @ $([Math]::Round($s.indexed_chunks_per_sec, 1)) chunks/sec" -ForegroundColor White
    Write-Host "  Committed:   $(Format-Number $s.committed_chunks) chunks   @ $([Math]::Round($s.committed_chunks_per_sec, 1)) chunks/sec" -ForegroundColor White
    Write-Host "  Elapsed:     $(Format-Duration $s.elapsed_ms)" -ForegroundColor White
    Write-Host "  RAM:         $(Format-Size $s.process_rss_mb)" -ForegroundColor White

    if ($s.last_error) {
        Write-Host "  Last error:  $($s.last_error)" -ForegroundColor Red
    }

    # ETA estimate
    if ($s.state -eq "running" -and $s.indexed_chunks_per_sec -gt 0) {
        if ($s.total_chunks -gt 0) {
            $estimatedTotal = $s.total_chunks
        } else {
            $estimatedTotal = [Math]::Max($s.scanned_files * 6, $s.indexed_chunks + 1)
        }
        $remaining = [Math]::Max(0, $estimatedTotal - $s.indexed_chunks)
        if ($remaining -gt 0) {
            $etaMs = ($remaining / $s.indexed_chunks_per_sec) * 1000
            Write-Host "  ETA:         ~$(Format-Duration $etaMs)" -ForegroundColor Yellow
        }
    }

    Write-Host "  ----------------------------------------------------" -ForegroundColor DarkGray
    Write-Host ""
    return $s
}

# --- Store file info ---
function Show-StoreInfo {
    $storeDir = "G:\Proj\Wick\build\bin"
    $storeFile = Join-Path $storeDir "wax-server.mv2s"
    if (Test-Path $storeFile) {
        $info = Get-Item $storeFile
        $sizeMB = $info.Length / 1MB
        Write-Host "  STORE FILE" -ForegroundColor White
        Write-Host "  Path:   $storeFile" -ForegroundColor DarkGray
        Write-Host "  Size:   $(Format-Size $sizeMB)" -ForegroundColor White
        Write-Host "  Updated: $($info.LastWriteTime.ToString('HH:mm:ss'))" -ForegroundColor DarkGray
        Write-Host ""
    }
}

# --- Interactive recall ---
function Invoke-Recall {
    Write-Host ""
    Write-Host "  RECALL (semantic search)" -ForegroundColor Cyan
    $query = Read-Host "  Query"
    if ([string]::IsNullOrWhiteSpace($query)) { return }
    $topK = Read-Host "  Top K results [5]"
    if ([string]::IsNullOrWhiteSpace($topK)) { $topK = 5 } else { $topK = [int]$topK }

    Write-Host "  Searching..." -ForegroundColor Yellow
    $result = Invoke-WaxRpc -Method "recall" -Params @{ query = $query; top_k = $topK }

    if ($result.error) {
        Write-Host "  [ERROR] $($result.error)" -ForegroundColor Red
        return
    }

    Write-Host ""
    # Response is now a JSON object: { items: [...], count: N, total_tokens: N }
    $items = $result.items
    $count = $result.count
    $tokens = $result.total_tokens

    if ($items -and $items.Count -gt 0) {
        Write-Host "  Found $count results ($tokens tokens):" -ForegroundColor Green
        Write-Host ""
        $i = 1
        foreach ($item in $items) {
            $text = if ($item.text.Length -gt 200) { $item.text.Substring(0, 200) + "..." } else { $item.text }
            $score = if ($item.score) { " (score: $([Math]::Round($item.score, 3)))" } else { "" }
            Write-Host "  [$i]$score" -ForegroundColor Cyan
            Write-Host "      $text" -ForegroundColor Gray
            Write-Host ""
            $i++
        }
    }
    elseif ($count -eq 0) {
        Write-Host "  No results found." -ForegroundColor Yellow
    }
    else {
        Write-Host "  $($result | ConvertTo-Json -Depth 3)" -ForegroundColor Gray
    }
}

# --- Interactive answer.generate ---
function Invoke-AnswerGenerate {
    Write-Host ""
    Write-Host "  ANSWER GENERATE (RAG)" -ForegroundColor Cyan
    $query = Read-Host "  Question"
    if ([string]::IsNullOrWhiteSpace($query)) { return }

    Write-Host "  Generating answer (may take a while)..." -ForegroundColor Yellow
    $result = Invoke-WaxRpc -Method "answer.generate" -Params @{
        query              = $query
        max_context_items  = 10
        max_context_tokens = 4000
        max_output_tokens  = 768
    }

    if ($result.error) {
        Write-Host "  [ERROR] $($result.error)" -ForegroundColor Red
        return
    }

    Write-Host ""
    Write-Host "  ANSWER:" -ForegroundColor Green
    Write-Host "  -------" -ForegroundColor DarkGray

    if ($result.answer) {
        Write-Host "  $($result.answer)" -ForegroundColor White
    }
    elseif ($result -is [string]) {
        Write-Host "  $result" -ForegroundColor White
    }
    else {
        Write-Host "  $($result | ConvertTo-Json -Depth 5)" -ForegroundColor Gray
    }

    if ($result.citations) {
        Write-Host ""
        Write-Host "  CITATIONS:" -ForegroundColor Yellow
        foreach ($c in $result.citations) {
            Write-Host "    - $($c.file_path): $($c.text.Substring(0, [Math]::Min(100, $c.text.Length)))..." -ForegroundColor DarkGray
        }
    }
    Write-Host ""
}

# --- Index start ---
function Invoke-IndexStart {
    Write-Host ""
    Write-Host "  START INDEXING" -ForegroundColor Cyan
    $repoRoot = Read-Host "  Repo root path [j:/UE5.2SRC/Engine/Source]"
    if ([string]::IsNullOrWhiteSpace($repoRoot)) { $repoRoot = "j:/UE5.2SRC/Engine/Source" }
    $resume = Read-Host "  Resume from checkpoint? [Y/n]"
    $resumeBool = ($resume -ne "n" -and $resume -ne "N")

    Write-Host "  Starting index..." -ForegroundColor Yellow
    $result = Invoke-WaxRpc -Method "index.start" -Params @{
        repo_root          = $repoRoot
        resume             = $resumeBool
        flush_every_chunks = 128
        ingest_batch_size  = 1
    }

    Write-Host "  $($result | ConvertTo-Json -Depth 3 -Compress)" -ForegroundColor Gray
    Write-Host ""
}

# --- Auto-refresh status mode ---
function Invoke-AutoRefresh {
    $interval = Read-Host "  Refresh interval in seconds [3]"
    if ([string]::IsNullOrWhiteSpace($interval)) { $interval = 3 } else { $interval = [int]$interval }

    Write-Host "  Auto-refreshing every ${interval}s. Press any key to stop..." -ForegroundColor Yellow
    Write-Host ""

    while (-not [Console]::KeyAvailable) {
        Write-Header
        Write-Host "  AUTO-REFRESH MODE (press any key to stop)" -ForegroundColor Yellow
        Write-Host ""
        $status = Show-IndexStatus
        Show-StoreInfo

        $waited = 0
        while ($waited -lt ($interval * 10) -and -not [Console]::KeyAvailable) {
            Start-Sleep -Milliseconds 100
            $waited++
        }
    }
    [void][Console]::ReadKey($true)
}

# --- Main menu loop ---
function Show-Menu {
    Write-Host "  ACTIONS:" -ForegroundColor White
    Write-Host "  [1] Index Status          (single check)" -ForegroundColor Gray
    Write-Host "  [2] Auto-Refresh Status   (live monitor)" -ForegroundColor Gray
    Write-Host "  [3] Start Indexing" -ForegroundColor Gray
    Write-Host "  [4] Stop Indexing" -ForegroundColor Gray
    Write-Host "  [5] Recall (search)" -ForegroundColor Gray
    Write-Host "  [6] Answer Generate (RAG)" -ForegroundColor Gray
    Write-Host "  [7] Flush" -ForegroundColor Gray
    Write-Host "  [0] Exit" -ForegroundColor Gray
    Write-Host ""
}

# --- Main ---
while ($true) {
    Write-Header
    $status = Show-IndexStatus
    Show-StoreInfo
    Show-Menu

    $choice = Read-Host "  Select action"

    switch ($choice) {
        "1" {
            Write-Header
            Show-IndexStatus | Out-Null
            Show-StoreInfo
            Read-Host "  Press Enter to continue"
        }
        "2" { Invoke-AutoRefresh }
        "3" { Invoke-IndexStart; Read-Host "  Press Enter to continue" }
        "4" {
            Write-Host "  Stopping index..." -ForegroundColor Yellow
            $r = Invoke-WaxRpc -Method "index.stop"
            Write-Host "  $($r | ConvertTo-Json -Depth 3 -Compress)" -ForegroundColor Gray
            Read-Host "  Press Enter to continue"
        }
        "5" { Invoke-Recall; Read-Host "  Press Enter to continue" }
        "6" { Invoke-AnswerGenerate; Read-Host "  Press Enter to continue" }
        "7" {
            Write-Host "  Flushing..." -ForegroundColor Yellow
            $r = Invoke-WaxRpc -Method "flush"
            Write-Host "  $($r | ConvertTo-Json -Depth 3 -Compress)" -ForegroundColor Gray
            Read-Host "  Press Enter to continue"
        }
        "0" { Write-Host "  Bye!" -ForegroundColor Cyan; exit 0 }
        default { Write-Host "  Unknown option" -ForegroundColor Red; Start-Sleep -Seconds 1 }
    }
}
