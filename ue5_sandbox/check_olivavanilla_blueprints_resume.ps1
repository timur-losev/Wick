# ============================================================
#  OlivaVanilla blueprint resume diff check
#  Compares the exported Blueprint JSON tree against the cached file manifest.
#  No indexing is started here.
# ============================================================

param(
    [string]$ExportRoot = 'J:\UE4\Projects\OlivaVanilla\Saved\BlueprintExports',
    [string]$CheckpointNamespace = 'olivavanilla_blueprints',
    [string]$CheckpointFile = '',
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;

public static class WaxDiffHash {
    public static string Fnv1a64Hex(byte[] bytes) {
        unchecked {
            ulong hash = 14695981039346656037UL;
            const ulong prime = 1099511628211UL;
            foreach (byte b in bytes) {
                hash ^= b;
                hash *= prime;
            }
            return hash.ToString("x16");
        }
    }
}
"@

function Get-Fnv1a64Hex {
    param([byte[]]$Bytes)
    return [WaxDiffHash]::Fnv1a64Hex($Bytes)
}

function Get-RelativePath {
    param(
        [string]$RootPath,
        [string]$FullPath
    )

    if (-not $FullPath.StartsWith($RootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    $relative = $FullPath.Substring($RootPath.Length).TrimStart('\', '/')
    if ([string]::IsNullOrWhiteSpace($relative) -or $relative.StartsWith('..')) {
        return $null
    }

    return $relative.Replace('\', '/')
}

function Get-CurrentFiles {
    param(
        [string]$RootPath,
        [string[]]$IncludeExtensions,
        [string[]]$ExcludeDirs
    )

    $includeSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($ext in $IncludeExtensions) {
        [void]$includeSet.Add($ext)
    }
    $excludeSet = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($dir in $ExcludeDirs) {
        [void]$excludeSet.Add($dir)
    }

    $rootFull = [System.IO.Path]::GetFullPath($RootPath)
    if (-not $rootFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $rootFull += [System.IO.Path]::DirectorySeparatorChar
    }
    if (-not [System.IO.Directory]::Exists($rootFull)) {
        throw "scan repo_root does not exist: $RootPath"
    }

    $results = New-Object 'System.Collections.Generic.List[object]'
    $stack = New-Object 'System.Collections.Generic.Stack[string]'
    $stack.Push($rootFull.TrimEnd([System.IO.Path]::DirectorySeparatorChar))

    while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        try {
            $entries = [System.IO.Directory]::EnumerateFileSystemEntries($dir)
        } catch {
            continue
        }

        foreach ($entry in $entries) {
            try {
                if ([System.IO.Directory]::Exists($entry)) {
                    $name = [System.IO.Path]::GetFileName($entry)
                    if ($excludeSet.Contains($name)) {
                        continue
                    }
                    $stack.Push($entry)
                    continue
                }

                if (-not [System.IO.File]::Exists($entry)) {
                    continue
                }

                $ext = [System.IO.Path]::GetExtension($entry)
                if ([string]::IsNullOrEmpty($ext) -or -not $includeSet.Contains($ext)) {
                    continue
                }

                $relative = Get-RelativePath -RootPath $rootFull -FullPath $entry
                if ($null -eq $relative) {
                    continue
                }

                $info = [System.IO.FileInfo]::new($entry)
                $results.Add([pscustomobject]@{
                    relative_path = $relative
                    size_bytes    = [uint64]$info.Length
                    full_path     = $entry
                }) | Out-Null
            } catch {
                continue
            }
        }
    }

    return $results
}

function Parse-FileManifest {
    param([string]$ManifestPath)

    $items = New-Object 'System.Collections.Generic.List[object]'
    foreach ($line in [System.IO.File]::ReadLines($ManifestPath)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $parts = $line -split "`t", 3
        if ($parts.Count -ne 3) {
            continue
        }
        [uint64]$size = 0
        if (-not [uint64]::TryParse($parts[1], [ref]$size)) {
            continue
        }
        $items.Add([pscustomobject]@{
            relative_path = $parts[0]
            size_bytes    = [uint64]$size
            content_hash  = $parts[2]
        }) | Out-Null
    }
    return $items
}

function Write-SectionHeader {
    param([string]$Title, [string]$Color = 'Cyan')
    Write-Host $Title -ForegroundColor $Color
    Write-Host ('-' * $Title.Length) -ForegroundColor DarkGray
}

$exportRootFull = [System.IO.Path]::GetFullPath($ExportRoot)
$exportRootForward = $exportRootFull.TrimEnd([System.IO.Path]::DirectorySeparatorChar).Replace('\', '/')
$scriptDataDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\bin\data'))
if ([string]::IsNullOrWhiteSpace($CheckpointFile)) {
    if ([string]::IsNullOrWhiteSpace($CheckpointNamespace)) {
        $CheckpointFile = Join-Path $scriptDataDir 'wax-server.mv2s.index.checkpoint'
    } else {
        $CheckpointFile = Join-Path $scriptDataDir ("wax-server.mv2s.index.$CheckpointNamespace.checkpoint")
    }
}
$CheckpointFile = [System.IO.Path]::GetFullPath($CheckpointFile)
$FileManifest = $CheckpointFile + '.file_manifest'

Write-Host '============================================================'
Write-Host ' OlivaVanilla blueprint resume diff check'
Write-Host " Source: $ExportRoot"
Write-Host '============================================================'
Write-Host ''
Write-Host "Checkpoint:   $CheckpointFile"
Write-Host "File manifest: $FileManifest"
Write-Host ''

if (-not (Test-Path -LiteralPath $CheckpointFile)) {
    Write-Host 'Check failed: missing checkpoint file.' -ForegroundColor Red
    Write-Host "  $CheckpointFile"
    Write-Host ''
    if (-not $NoPause) { pause }
    exit 1
}

if (-not (Test-Path -LiteralPath $FileManifest)) {
    Write-Host 'Check failed: missing file manifest.' -ForegroundColor Red
    Write-Host "  $FileManifest"
    Write-Host ''
    if (-not $NoPause) { pause }
    exit 1
}

$checkpointRepoRootLine = Select-String -LiteralPath $CheckpointFile -Pattern '^repo_root=' | Select-Object -First 1
if ($null -eq $checkpointRepoRootLine) {
    Write-Host 'Check failed: checkpoint is missing repo_root.' -ForegroundColor Red
    Write-Host "  $CheckpointFile"
    Write-Host ''
    if (-not $NoPause) { pause }
    exit 1
}

$checkpointRepoRoot = $checkpointRepoRootLine.Line.Substring('repo_root='.Length)
if ($checkpointRepoRoot -ne $exportRootForward) {
    Write-Host 'Check failed: checkpoint repo_root does not match this export tree.' -ForegroundColor Red
    Write-Host "  expected:  $exportRootForward"
    Write-Host "  found:     $checkpointRepoRoot"
    Write-Host ''
    if (-not $NoPause) { pause }
    exit 1
}

Write-Host "Preflight ok: checkpoint matches $exportRootForward"
Write-Host ''

$baselineItems = Parse-FileManifest -ManifestPath $FileManifest
$baselineByPath = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::Ordinal)
foreach ($item in $baselineItems) {
    $baselineByPath[$item.relative_path] = $item
}

$currentItems = Get-CurrentFiles -RootPath $exportRootFull -IncludeExtensions @('.bpl_json') -ExcludeDirs @()

$seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$sameSizeCandidates = New-Object 'System.Collections.Generic.List[object]'

$unchangedCount = 0
$changedCount = 0
$newCount = 0
$missingCount = 0

Write-SectionHeader 'Current tree vs manifest'
foreach ($current in ($currentItems | Sort-Object relative_path)) {
    if ($baselineByPath.ContainsKey($current.relative_path)) {
        $baseline = $baselineByPath[$current.relative_path]
        [void]$seen.Add($current.relative_path)
        if ($baseline.size_bytes -ne $current.size_bytes) {
            $changedCount++
            Write-Host ("[CHANGED] {0}" -f $current.relative_path) -ForegroundColor Yellow
            Write-Host ("  prev: size={0} hash={1}" -f $baseline.size_bytes, $baseline.content_hash) -ForegroundColor DarkYellow
            Write-Host ("  curr: size={0}" -f $current.size_bytes) -ForegroundColor DarkYellow
            continue
        }

        $sameSizeCandidates.Add([pscustomobject]@{
            current  = $current
            baseline = $baseline
        }) | Out-Null
    } else {
        $newCount++
        Write-Host ("[NEW] {0}" -f $current.relative_path) -ForegroundColor Cyan
        Write-Host ("  curr: size={0}" -f $current.size_bytes) -ForegroundColor DarkCyan
    }
}

foreach ($candidate in $sameSizeCandidates) {
    $bytes = [System.IO.File]::ReadAllBytes($candidate.current.full_path)
    $currentHash = Get-Fnv1a64Hex -Bytes $bytes
    if ($currentHash -eq $candidate.baseline.content_hash) {
        $unchangedCount++
        continue
    }

    $changedCount++
    Write-Host ("[CHANGED] {0}" -f $candidate.current.relative_path) -ForegroundColor Yellow
    Write-Host ("  prev: size={0} hash={1}" -f $candidate.baseline.size_bytes, $candidate.baseline.content_hash) -ForegroundColor DarkYellow
    Write-Host ("  curr: size={0} hash={1}" -f $candidate.current.size_bytes, $currentHash) -ForegroundColor DarkYellow
}

foreach ($baseline in ($baselineItems | Sort-Object relative_path)) {
    if ($seen.Contains($baseline.relative_path)) {
        continue
    }

    $missingCount++
    Write-Host ("[MISSING] {0}" -f $baseline.relative_path) -ForegroundColor Magenta
    Write-Host ("  prev: size={0} hash={1}" -f $baseline.size_bytes, $baseline.content_hash) -ForegroundColor DarkMagenta
}

Write-Host ''
Write-Host ("Summary: scanned={0} unchanged={1} changed={2} new={3} missing={4}" -f $currentItems.Count, $unchangedCount, $changedCount, $newCount, $missingCount)
if ($changedCount -eq 0 -and $newCount -eq 0 -and $missingCount -eq 0) {
    Write-Host 'Check ok: no file-level differences detected.' -ForegroundColor Green
} else {
    Write-Host 'Check complete: file-level differences detected above.' -ForegroundColor Yellow
}
Write-Host ''
if (-not $NoPause) {
    pause
}
exit 0
