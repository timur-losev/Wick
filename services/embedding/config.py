"""Configuration for the WAX embedding HTTP service.

Values can be overridden via environment variables (see .env.example).
"""
from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    # Model identity (HuggingFace repo id).
    # Qodo-Embed-1-1.5B = SOTA on MTEB code retrieval, 1536-dim output.
    model_id: str = os.environ.get("WAX_EMBED_MODEL", "Qodo/Qodo-Embed-1-1.5B")

    # Local cache path. All weights end up under <cache>/<model_id>/...
    # Set HF_HOME so HuggingFace CLI & transformers agree on the location.
    model_cache: str = os.environ.get(
        "WAX_EMBED_CACHE", r"G:\Proj\Agents1\Models"
    )

    # cuda, cpu, or specific device like cuda:0
    device: str = os.environ.get("WAX_EMBED_DEVICE", "cuda")

    # Half precision (fp16) for Qodo-1.5B ≈ ~3 GB VRAM vs ~6 GB fp32.
    # RTX 4090 has plenty of headroom either way.
    dtype: str = os.environ.get("WAX_EMBED_DTYPE", "float16")

    # Max batch size for /embed_batch.
    max_batch_size: int = int(os.environ.get("WAX_EMBED_MAX_BATCH", "32"))

    # Normalize embeddings (L2-norm) so cosine = dot product.
    # ES dense_vector with similarity=cosine also normalizes internally, but
    # pre-normalizing means we can use similarity=dot_product for speed.
    normalize: bool = os.environ.get("WAX_EMBED_NORMALIZE", "1") != "0"

    # Instruction prefix for query vs document (Qodo follows sentence-transformers conv).
    # Qodo specifically uses no instruction prefix — the README says the model was
    # trained without task instructions. Leave empty unless fine-tuning says otherwise.
    query_prefix: str = os.environ.get("WAX_EMBED_QUERY_PREFIX", "")
    document_prefix: str = os.environ.get("WAX_EMBED_DOC_PREFIX", "")

    # HTTP service bind address.
    host: str = os.environ.get("WAX_EMBED_HOST", "127.0.0.1")
    port: int = int(os.environ.get("WAX_EMBED_PORT", "8088"))

    # Elasticsearch (used by scripts, not by the service).
    es_url: str = os.environ.get("WAX_ES_URL", "http://127.0.0.1:9200")
    es_bp_index: str = os.environ.get("WAX_ES_BP_INDEX", "wax_bp_v1")


SETTINGS = Settings()
