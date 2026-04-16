"""FastAPI embedding service for WAX.

Endpoints:
    GET  /health          → status + model info
    POST /embed           → embed a single string  { "text": "..." } → { "vector": [...] }
    POST /embed_batch     → embed a list          { "texts": [...] } → { "vectors": [[...]] }
    POST /similarity      → cosine sim utility    { "a": "...", "b": "..." } → { "score": float }

Run:
    # from G:/Proj/Wick/services/embedding/
    uvicorn app:app --host 127.0.0.1 --port 8088
"""
from __future__ import annotations

import logging
import os
import time
from contextlib import asynccontextmanager
from typing import Optional

import numpy as np
import torch
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field
from sentence_transformers import SentenceTransformer

from config import SETTINGS

logger = logging.getLogger("wax.embedding")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")

# ── Lifespan: load model once, reuse across requests ────────────────────────

_model: Optional[SentenceTransformer] = None
_load_ms: float = 0.0
_dim: int = 0


def _resolve_dtype(name: str) -> torch.dtype:
    mapping = {"float16": torch.float16, "fp16": torch.float16,
               "bfloat16": torch.bfloat16, "bf16": torch.bfloat16,
               "float32": torch.float32, "fp32": torch.float32}
    return mapping.get(name.lower(), torch.float16)


@asynccontextmanager
async def lifespan(_: FastAPI):
    global _model, _load_ms, _dim
    os.environ.setdefault("HF_HOME", SETTINGS.model_cache)
    os.environ.setdefault("TRANSFORMERS_CACHE", SETTINGS.model_cache)
    os.environ.setdefault("SENTENCE_TRANSFORMERS_HOME", SETTINGS.model_cache)

    logger.info("loading model=%s device=%s dtype=%s cache=%s",
                SETTINGS.model_id, SETTINGS.device, SETTINGS.dtype, SETTINGS.model_cache)

    t0 = time.perf_counter()
    model = SentenceTransformer(
        SETTINGS.model_id,
        device=SETTINGS.device,
        trust_remote_code=True,
        cache_folder=SETTINGS.model_cache,
        model_kwargs={"torch_dtype": _resolve_dtype(SETTINGS.dtype)},
    )
    _load_ms = (time.perf_counter() - t0) * 1000.0

    # Probe dimensionality with a single encode call.
    probe = model.encode(["probe"], normalize_embeddings=SETTINGS.normalize, convert_to_numpy=True)
    _dim = int(probe.shape[1])
    logger.info("model ready dim=%d load_ms=%.1f", _dim, _load_ms)

    _model = model
    yield
    _model = None


app = FastAPI(title="WAX Embedding Service", version="0.1.0", lifespan=lifespan)


# ── Schemas ─────────────────────────────────────────────────────────────────

class EmbedRequest(BaseModel):
    text: str = Field(min_length=1, max_length=65536)
    is_query: bool = False  # apply query_prefix vs document_prefix


class EmbedBatchRequest(BaseModel):
    texts: list[str] = Field(min_length=1, max_length=256)
    is_query: bool = False


class SimilarityRequest(BaseModel):
    a: str = Field(min_length=1)
    b: str = Field(min_length=1)


class EmbedResponse(BaseModel):
    vector: list[float]
    dim: int
    model: str
    elapsed_ms: float


class EmbedBatchResponse(BaseModel):
    vectors: list[list[float]]
    dim: int
    model: str
    batch_size: int
    elapsed_ms: float


# ── Helpers ─────────────────────────────────────────────────────────────────

def _require_model() -> SentenceTransformer:
    if _model is None:
        raise HTTPException(status_code=503, detail="model not loaded yet")
    return _model


def _apply_prefix(texts: list[str], is_query: bool) -> list[str]:
    prefix = SETTINGS.query_prefix if is_query else SETTINGS.document_prefix
    if not prefix:
        return texts
    return [f"{prefix}{t}" for t in texts]


def _encode(texts: list[str], is_query: bool) -> np.ndarray:
    model = _require_model()
    prepared = _apply_prefix(texts, is_query)
    with torch.inference_mode():
        vecs = model.encode(
            prepared,
            normalize_embeddings=SETTINGS.normalize,
            convert_to_numpy=True,
            batch_size=SETTINGS.max_batch_size,
            show_progress_bar=False,
        )
    return vecs.astype(np.float32)


# ── Endpoints ───────────────────────────────────────────────────────────────

@app.get("/health")
def health() -> dict:
    cuda_ok = torch.cuda.is_available()
    vram_allocated_mb = (torch.cuda.memory_allocated() / (1024 * 1024)) if cuda_ok else 0.0
    return {
        "status": "ok" if _model is not None else "loading",
        "model": SETTINGS.model_id,
        "device": SETTINGS.device,
        "dtype": SETTINGS.dtype,
        "dim": _dim,
        "load_ms": _load_ms,
        "cuda": cuda_ok,
        "vram_mb": round(vram_allocated_mb, 1),
        "normalize": SETTINGS.normalize,
    }


@app.post("/embed", response_model=EmbedResponse)
def embed(req: EmbedRequest) -> EmbedResponse:
    t0 = time.perf_counter()
    vec = _encode([req.text], req.is_query)[0]
    elapsed = (time.perf_counter() - t0) * 1000.0
    return EmbedResponse(
        vector=vec.tolist(),
        dim=int(vec.shape[0]),
        model=SETTINGS.model_id,
        elapsed_ms=elapsed,
    )


@app.post("/embed_batch", response_model=EmbedBatchResponse)
def embed_batch(req: EmbedBatchRequest) -> EmbedBatchResponse:
    t0 = time.perf_counter()
    vecs = _encode(req.texts, req.is_query)
    elapsed = (time.perf_counter() - t0) * 1000.0
    return EmbedBatchResponse(
        vectors=vecs.tolist(),
        dim=int(vecs.shape[1]),
        model=SETTINGS.model_id,
        batch_size=len(req.texts),
        elapsed_ms=elapsed,
    )


@app.post("/similarity")
def similarity(req: SimilarityRequest) -> dict:
    vecs = _encode([req.a, req.b], is_query=False)
    score = float(np.dot(vecs[0], vecs[1]))  # normalized → cosine
    return {"score": score, "model": SETTINGS.model_id}
