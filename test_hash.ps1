param([string]$FilePath)
$bytes = [System.IO.File]::ReadAllBytes($FilePath)
[uint64]$hash = 14695981039346656037
[uint64]$prime = 1099511628211
foreach ($b in $bytes) {
    $hash = $hash -bxor [uint64]$b
    $hash = $hash * $prime
}
Write-Host ("Hash: {0:x16}" -f $hash)
Write-Host ("Size: {0}" -f $bytes.Length)
