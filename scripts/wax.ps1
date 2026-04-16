# wax.ps1 — single-command CLI over the WAX stack.
#
# Avoids PowerShell quoting hell when calling curl by using Invoke-RestMethod
# everywhere internally. Designed to be called from agent shells, prompts,
# or directly by humans.
#
# Examples:
#   .\scripts\wax.ps1 bp      "ability that disables player input"
#   .\scripts\wax.ps1 bp      "vehicle exit seat"  -K 10  -KindFilter actor_blueprint
#   .\scripts\wax.ps1 facts   bp:GA_SpawnEffect
#   .\scripts\wax.ps1 recall  "UCharacterMovementComponent SetMovementMode"
#   .\scripts\wax.ps1 fact    cpp:UCharacterMovementComponent
#   .\scripts\wax.ps1 refresh bp:GA_SpawnEffect
#   .\scripts\wax.ps1 health
#
# All requests go through Invoke-RestMethod / Invoke-WebRequest — no curl,
# no bash, no escaping problems.

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet("bp", "facts", "recall", "fact", "refresh", "health", "help")]
    [string]$Command,

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$RestArgs,

    [int]$K = 5,
    [ValidateSet("knn", "bm25", "hybrid")]
    [string]$Mode = "hybrid",
    [string]$KindFilter = "",
    [int]$Limit = 10,
    [string]$ExportDir = "J:/Temp/BlueprintExports",

    [string]$WaxUrl   = $(if ($env:WAX_URL)        { $env:WAX_URL }        else { "http://127.0.0.1:8080" }),
    [string]$EmbedUrl = $(if ($env:WAX_EMBED_URL)  { $env:WAX_EMBED_URL }  else { "http://127.0.0.1:8088" }),
    [string]$EsUrl    = $(if ($env:WAX_ES_URL)     { $env:WAX_ES_URL }     else { "http://127.0.0.1:9200" }),
    [string]$EsIndex  = $(if ($env:WAX_ES_BP_INDEX) { $env:WAX_ES_BP_INDEX } else { "wax_bp_v1" })
)

$ErrorActionPreference = "Stop"

function Show-Help {
    @"
wax.ps1 — single-command CLI over the WAX stack

Commands:
  bp <query>       Semantic BP search (kNN+BM25 hybrid by default)
                   Flags: -K <n>  -Mode knn|bm25|hybrid  -KindFilter <kind>
  facts <entity>   Exact BP lookup. <entity> must start with 'bp:'
  recall <query>   C++ full-text BM25 search via WAX C++ server
                   Flags: -Limit <n>
  fact <prefix>    EAV facts by entity prefix (typically 'cpp:...')
                   Flags: -Limit <n>
  refresh <entity> Re-parse + re-embed + ES upsert one BP after patch+import
                   Flags: -ExportDir <path>
  health           Health check on all three backends (WAX, ES, embed)
  help             Show this message

Endpoints (override via env or flags):
  -WaxUrl    $WaxUrl
  -EmbedUrl  $EmbedUrl
  -EsUrl     $EsUrl
  -EsIndex   $EsIndex

Examples:
  .\scripts\wax.ps1 bp "disables player input after respawn"
  .\scripts\wax.ps1 bp "weapon pickup with ammo" -K 10 -KindFilter gameplay_ability
  .\scripts\wax.ps1 facts bp:GA_SpawnEffect
  .\scripts\wax.ps1 recall "ULyraAbilitySystemComponent InitAbilityActorInfo"
  .\scripts\wax.ps1 fact cpp:UCharacterMovementComponent -Limit 30
  .\scripts\wax.ps1 refresh bp:GA_SpawnEffect
"@ | Write-Host
}

function Invoke-WaxRpc {
    param(
        [string]$Method,
        $Params
    )
    $body = @{
        jsonrpc = "2.0"
        id      = 1
        method  = $Method
        params  = $Params
    } | ConvertTo-Json -Depth 10 -Compress
    Invoke-RestMethod -Uri $WaxUrl -Method Post -ContentType 'application/json' -Body $body
}

function Get-Embedding {
    param([string]$Text, [bool]$IsQuery = $true)
    $body = @{ text = $Text; is_query = $IsQuery } | ConvertTo-Json -Compress
    (Invoke-RestMethod -Uri "$EmbedUrl/embed" -Method Post -ContentType 'application/json' -Body $body).vector
}

function Format-BpHits {
    param($Hits)
    foreach ($h in $Hits) {
        $score = "{0,6:0.000}" -f $h._score
        $entity = ($h._source.entity).PadRight(45)
        $kind = "[{0}]" -f $h._source.kind
        Write-Host "$score  $entity  $kind" -ForegroundColor Cyan
        if ($h._source.purpose) {
            Write-Host "        $($h._source.purpose)" -ForegroundColor Gray
        }
        if ($h._source.exec_chain) {
            $chain = $h._source.exec_chain
            if ($chain.Length -gt 200) { $chain = $chain.Substring(0, 200) + "…" }
            Write-Host "        exec: $chain" -ForegroundColor DarkGray
        }
    }
}

# ── Dispatch ────────────────────────────────────────────────────────────────

switch ($Command) {

    "help" { Show-Help; exit 0 }

    "health" {
        Write-Host "WAX C++   : " -NoNewline
        try {
            $r = Invoke-WaxRpc 'index.status' @{}
            Write-Host "ok (state=$($r.state))" -ForegroundColor Green
        } catch { Write-Host "DOWN ($($_.Exception.Message))" -ForegroundColor Red }

        Write-Host "Embedding : " -NoNewline
        try {
            $h = Invoke-RestMethod -Uri "$EmbedUrl/health"
            Write-Host "ok (model=$($h.model) dim=$($h.dim) vram=$($h.vram_mb)MB)" -ForegroundColor Green
        } catch { Write-Host "DOWN ($($_.Exception.Message))" -ForegroundColor Red }

        Write-Host "ES        : " -NoNewline
        try {
            $h = Invoke-RestMethod -Uri "$EsUrl/_cluster/health"
            $c = (Invoke-RestMethod -Uri "$EsUrl/$EsIndex/_count").count
            Write-Host "$($h.status), $EsIndex=$c docs" -ForegroundColor Green
        } catch { Write-Host "DOWN ($($_.Exception.Message))" -ForegroundColor Red }
        exit 0
    }

    "bp" {
        if (-not $RestArgs) { Write-Error "usage: wax.ps1 bp <query>"; exit 1 }
        $query = ($RestArgs -join " ").Trim()
        Write-Host "Query: $query  (mode=$Mode, k=$K$(if($KindFilter){', kind='+$KindFilter}))" -ForegroundColor Yellow

        $searchBody = @{
            size    = $K
            _source = @{ excludes = @("embedding") }
        }
        if ($Mode -ne "bm25") {
            $vec = Get-Embedding $query $true
            $searchBody.knn = @{
                field = "embedding"; query_vector = $vec
                k = $K; num_candidates = [Math]::Max(100, $K * 20)
            }
            if ($Mode -eq "hybrid") {
                $searchBody.knn.boost = 0.7
                $searchBody.query = @{ multi_match = @{
                    query = $query
                    fields = @("purpose^2","asset_name^2","text","exec_chain","calls")
                    boost = 0.3
                }}
            }
        } else {
            $searchBody.query = @{ multi_match = @{
                query = $query
                fields = @("purpose^2","asset_name^2","text","exec_chain","calls")
            }}
        }
        if ($KindFilter) {
            $filter = @{ term = @{ kind = $KindFilter } }
            if ($searchBody.knn) { $searchBody.knn.filter = $filter }
            if ($searchBody.query) {
                $searchBody.query = @{ bool = @{ must = @($searchBody.query); filter = @($filter) } }
            } else {
                $searchBody.query = @{ bool = @{ filter = @($filter) } }
            }
        }

        $body = $searchBody | ConvertTo-Json -Depth 10 -Compress
        $resp = Invoke-RestMethod -Uri "$EsUrl/$EsIndex/_search" -Method Post -ContentType 'application/json' -Body $body
        $hits = $resp.hits.hits
        if (-not $hits) { Write-Host "(no hits)" -ForegroundColor Red; exit 0 }
        Format-BpHits $hits
    }

    "facts" {
        if (-not $RestArgs) { Write-Error "usage: wax.ps1 facts bp:<name>"; exit 1 }
        $entity = $RestArgs[0]
        if (-not $entity.StartsWith("bp:")) {
            Write-Error "entity must start with 'bp:' (got $entity)"; exit 1
        }
        try {
            $doc = Invoke-RestMethod -Uri "$EsUrl/$EsIndex/_doc/$([uri]::EscapeDataString($entity))?_source_excludes=embedding"
        } catch {
            if ($_.Exception.Response.StatusCode.value__ -eq 404) {
                Write-Host "$entity not found in $EsIndex" -ForegroundColor Red
                exit 1
            }
            throw
        }
        $s = $doc._source
        Write-Host "$($s.entity)" -ForegroundColor Cyan
        Write-Host ("-" * $s.entity.Length)
        if ($s.kind)         { Write-Host "  kind          : $($s.kind)" }
        if ($s.parent_class) { Write-Host "  parent_class  : $($s.parent_class)" }
        if ($s.asset_path)   { Write-Host "  asset_path    : $($s.asset_path)" }
        Write-Host "  nodes / links : $($s.node_count) / $($s.link_count)"
        if ($s.purpose) {
            Write-Host "`n  purpose:"
            Write-Host "    $($s.purpose)" -ForegroundColor Gray
        }
        if ($s.exec_chain) {
            Write-Host "`n  exec chains:"
            Write-Host "    $($s.exec_chain)" -ForegroundColor Gray
        }
        $sections = @(
            @{ Label = "events";       Data = $s.events }
            @{ Label = "custom_events";Data = $s.custom_events }
            @{ Label = "calls";        Data = $s.calls }
            @{ Label = "calls_owners"; Data = $s.calls_owners }
            @{ Label = "variables";    Data = $s.variables }
            @{ Label = "casts_to";     Data = $s.casts_to }
            @{ Label = "macros";       Data = $s.macros }
        )
        foreach ($sec in $sections) {
            if ($sec.Data -and $sec.Data.Count -gt 0) {
                Write-Host "`n  $($sec.Label) ($($sec.Data.Count)):"
                Write-Host "    $($sec.Data -join ', ')" -ForegroundColor Gray
            }
        }
    }

    "recall" {
        if (-not $RestArgs) { Write-Error "usage: wax.ps1 recall <query>"; exit 1 }
        $query = ($RestArgs -join " ").Trim()
        $r = Invoke-WaxRpc 'recall' @{ query = $query; limit = $Limit }
        Write-Host "Found $($r.count) items, $($r.total_tokens) tokens" -ForegroundColor Yellow
        for ($i = 0; $i -lt $r.items.Count; $i++) {
            $item = $r.items[$i]
            $score = "{0:0.0000}" -f $item.score
            Write-Host "`n[$($i+1)] score=$score kind=$($item.kind)" -ForegroundColor Cyan
            $text = $item.text
            if ($text.Length -gt 400) { $text = $text.Substring(0, 400) + "…" }
            Write-Host "    $text" -ForegroundColor Gray
        }
    }

    "fact" {
        if (-not $RestArgs) { Write-Error "usage: wax.ps1 fact cpp:<prefix>"; exit 1 }
        $prefix = $RestArgs[0]
        $r = Invoke-WaxRpc 'fact.search' @{ entity_prefix = $prefix; limit = $Limit }
        Write-Host "$($r.count) facts for prefix '$prefix'" -ForegroundColor Yellow
        foreach ($f in $r.facts) {
            Write-Host ("  {0,-50} {1,-15} {2}" -f $f.entity, $f.attribute, $f.value)
        }
    }

    "refresh" {
        if (-not $RestArgs) { Write-Error "usage: wax.ps1 refresh bp:<name>"; exit 1 }
        $entity = $RestArgs[0]
        $body = @{ entity = $entity; export_dir = $ExportDir } | ConvertTo-Json -Compress
        try {
            $r = Invoke-RestMethod -Uri "$EmbedUrl/bp_refresh" -Method Post -ContentType 'application/json' -Body $body
            Write-Host "status            : $($r.status)" -ForegroundColor Green
            Write-Host "entity            : $($r.entity)"
            if ($r.structural_hash) { Write-Host "structural_hash   : $($r.structural_hash)" }
            if ($r.prev_hash)       { Write-Host "prev_hash         : $($r.prev_hash)" }
            if ($r.purpose_stale)   { Write-Host "note              : kept old purpose (run scripts/run_full_reindex.ps1 to regen)" -ForegroundColor Yellow }
            if ($r.elapsed_ms)      { Write-Host "elapsed_ms        : $($r.elapsed_ms)" }
        } catch {
            $detail = ""
            try {
                $stream = $_.Exception.Response.GetResponseStream()
                $reader = New-Object System.IO.StreamReader($stream)
                $detail = $reader.ReadToEnd()
            } catch {}
            Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
            if ($detail) { Write-Host $detail -ForegroundColor DarkRed }
            exit 1
        }
    }

    default { Show-Help }
}
