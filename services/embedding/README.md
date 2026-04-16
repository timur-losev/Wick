# WAX Embedding Service

Local HTTP service that wraps the [Qodo-Embed-1-1.5B](https://huggingface.co/Qodo/Qodo-Embed-1-1.5B) code-specialized embedding model. Used by the Blueprint semantic pipeline to produce 1536-dim vectors that land in Elasticsearch.

## Why a separate service

- WAX is C++ — keeping embedding in Python avoids pulling torch/CUDA into the C++ build
- Model weights live in VRAM once at startup; all subsequent requests reuse the loaded model
- Swappable — replace Qodo with another model by changing `WAX_EMBED_MODEL`
- Independently observable (logs, /health, VRAM usage)

## Quick start

```powershell
# one-time setup: create venv, install torch-cuda, sentence-transformers, download model
powershell -ExecutionPolicy Bypass -File ..\..\scripts\setup_embedding.ps1

# run the service
powershell -ExecutionPolicy Bypass -File ..\..\scripts\start_services.ps1 -NoDocker

# verify
curl http://127.0.0.1:8088/health
```

## Endpoints

| Method | Path | Body | Returns |
|--------|------|------|---------|
| GET  | `/health`        | — | `{status, model, device, dim, vram_mb, ...}` |
| POST | `/embed`         | `{text, is_query?}` | `{vector, dim, elapsed_ms}` |
| POST | `/embed_batch`   | `{texts[], is_query?}` | `{vectors[], dim, batch_size, elapsed_ms}` |
| POST | `/similarity`    | `{a, b}` | `{score}` |

### Example

```bash
curl -X POST http://127.0.0.1:8088/embed \
  -H "Content-Type: application/json" \
  -d '{"text": "applies a spawn effect and disables input"}'
```

## Configuration (env vars)

| Variable | Default | Purpose |
|----------|---------|---------|
| `WAX_EMBED_MODEL`    | `Qodo/Qodo-Embed-1-1.5B` | HuggingFace repo id |
| `WAX_EMBED_CACHE`    | `G:\Proj\Agents1\Models` | HF cache root |
| `WAX_EMBED_DEVICE`   | `cuda` | `cuda`, `cpu`, `cuda:0` |
| `WAX_EMBED_DTYPE`    | `float16` | `float16` / `bfloat16` / `float32` |
| `WAX_EMBED_MAX_BATCH`| `32` | max batch size for `/embed_batch` |
| `WAX_EMBED_NORMALIZE`| `1` | L2-normalize output (recommended for ES cosine) |
| `WAX_EMBED_HOST`     | `127.0.0.1` | bind address |
| `WAX_EMBED_PORT`     | `8088` | port |

## Model notes — Qodo-Embed-1-1.5B

- 1.5B parameters, **1536-dim output**
- Specifically trained for code retrieval (SOTA on CoIR benchmark)
- No task-specific instruction prefix needed (unlike nomic-embed)
- fp16 on RTX 4090: ~3 GB VRAM, ~40 ms single query, ~80 ms batch of 32
- License: Apache 2.0

## Integration with Elasticsearch

The index mapping in `scripts/es_setup_bp_index.py` uses `dense_vector` with:
- `dims: 1536`
- `similarity: cosine`
- HNSW index (`m=16`, `ef_construction=100`)

Since we normalize vectors in this service, `similarity: dot_product` would also work and be slightly faster — change both settings together if you switch.

## Troubleshooting

- **`torch.cuda.is_available() == False`**: driver too old or PyTorch CPU-only. Re-run setup with `-SkipVenv -SkipDeps -SkipModel` flags removed.
- **OOM on load**: reduce VRAM usage by setting `WAX_EMBED_DTYPE=float16` (default already is).
- **Model not found in cache**: delete `G:\Proj\Agents1\Models\Qodo--Qodo-Embed-1-1.5B\` and rerun setup.
- **Slow first request**: HuggingFace `trust_remote_code` compilation on first load (~5 s one-time).
