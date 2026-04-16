# Setup script for WAX embedding service.
#
#   1. Creates a Python venv at G:/Proj/Wick/py_env (if missing)
#   2. Installs PyTorch with CUDA 12.4 wheels
#   3. Installs remaining dependencies from requirements.txt
#   4. Downloads Qodo-Embed-1-1.5B into G:/Proj/Agents1/Models (cached via HuggingFace)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File G:/Proj/Wick/scripts/setup_embedding.ps1
#
# To re-run only a specific step:
#   -SkipVenv       # skip venv creation
#   -SkipTorch      # skip PyTorch install
#   -SkipDeps       # skip requirements.txt install
#   -SkipModel      # skip model download

param(
    [switch]$SkipVenv,
    [switch]$SkipTorch,
    [switch]$SkipDeps,
    [switch]$SkipModel,
    [string]$ModelId = "Qodo/Qodo-Embed-1-1.5B",
    [string]$ModelCache = "G:\Proj\Agents1\Models"
)

$ErrorActionPreference = "Stop"
$RepoRoot   = "G:\Proj\Wick"
$VenvDir    = Join-Path $RepoRoot "py_env"
$VenvPython = Join-Path $VenvDir "Scripts\python.exe"
$VenvPip    = Join-Path $VenvDir "Scripts\pip.exe"
$ReqPath    = Join-Path $RepoRoot "services\embedding\requirements.txt"

function Step($msg) {
    Write-Host "`n==== $msg ====" -ForegroundColor Cyan
}

# ── Step 1: Create venv ─────────────────────────────────────────────────────
if (-not $SkipVenv) {
    if (Test-Path $VenvPython) {
        Write-Host "venv already exists at $VenvDir" -ForegroundColor Yellow
    } else {
        Step "Creating Python venv at $VenvDir"
        python -m venv $VenvDir
        if (-not (Test-Path $VenvPython)) {
            throw "venv creation failed — $VenvPython does not exist"
        }
    }

    Step "Upgrading pip in venv"
    & $VenvPython -m pip install --upgrade pip setuptools wheel
}

# ── Step 2: Install PyTorch with CUDA ───────────────────────────────────────
if (-not $SkipTorch) {
    Step "Installing PyTorch with CUDA 12.4 wheels (may take several minutes, ~2.5 GB)"
    & $VenvPip install --index-url https://download.pytorch.org/whl/cu124 torch
    Step "Verifying torch.cuda availability"
    & $VenvPython -c "import torch; print(f'torch={torch.__version__} cuda={torch.cuda.is_available()} device={torch.cuda.get_device_name(0) if torch.cuda.is_available() else \"cpu\"}')"
}

# ── Step 3: Install rest of requirements ────────────────────────────────────
if (-not $SkipDeps) {
    if (-not (Test-Path $ReqPath)) {
        throw "requirements.txt not found at $ReqPath"
    }
    Step "Installing requirements.txt"
    & $VenvPip install -r $ReqPath
}

# ── Step 4: Download Qodo-Embed-1-1.5B ──────────────────────────────────────
if (-not $SkipModel) {
    Step "Downloading $ModelId into $ModelCache (~6 GB, cached by HuggingFace)"
    $env:HF_HOME = $ModelCache
    $env:TRANSFORMERS_CACHE = $ModelCache
    $env:SENTENCE_TRANSFORMERS_HOME = $ModelCache

    $downloadScript = @"
import os, sys
from sentence_transformers import SentenceTransformer
os.makedirs(r'$ModelCache', exist_ok=True)
print(f'Downloading $ModelId to $ModelCache')
m = SentenceTransformer('$ModelId', trust_remote_code=True, cache_folder=r'$ModelCache')
v = m.encode(['sanity probe'], normalize_embeddings=True)
print(f'Model loaded. Output dim = {v.shape[1]}')
"@
    & $VenvPython -c $downloadScript
}

Step "Setup complete. Next steps:"
Write-Host "  1. Start Elasticsearch:   cd docker && docker compose up -d"
Write-Host "  2. Start embedding svc:   .\scripts\start_services.ps1"
Write-Host "  3. Create ES index:       .\py_env\Scripts\python.exe scripts\es_setup_bp_index.py"
Write-Host "  4. Sanity check:          .\py_env\Scripts\python.exe scripts\test_embed_roundtrip.py"
