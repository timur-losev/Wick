"""Shared pytest fixtures for WAX Python tests."""
from __future__ import annotations

import sys
from pathlib import Path

# Make `scripts/` importable so tests can `from parse_bp_facts import parse_bp`
REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def load_fixture(name: str) -> bytes:
    """Load a .bpl_json fixture as raw bytes (including BOM if present)."""
    path = FIXTURES / name
    if not path.is_file():
        raise FileNotFoundError(f"fixture missing: {path}")
    return path.read_bytes()
