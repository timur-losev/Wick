"""Phase 2: Generate purpose descriptions for each BP via llama-server.

Reads structural facts from bp_facts.json, calls llama-server's
OpenAI-compatible /v1/chat/completions endpoint for each BP,
saves results to bp_purposes.json.

Only the `purpose` field depends on the LLM. Everything else is structural
and already determined by Phase 1.

Usage:
    python scripts/generate_bp_purposes.py [--input FILE] [--output FILE] [--limit N]

Designed to be restartable: if --output already exists, it's loaded and only
BPs whose `structural_hash` changed get re-generated.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import URLError

LLAMA_URL = os.environ.get("WAX_LLAMA_URL", "http://127.0.0.1:8090")
DEFAULT_MAX_TOKENS = 140
DEFAULT_TEMPERATURE = 0.0

SYSTEM_PROMPT = """You describe Unreal Engine Blueprints in one concise sentence.

You are given:
- The Blueprint's asset name (use naming conventions: GA_ = GameplayAbility, GE_ = GameplayEffect,
  GC_ = GameplayCue, BP_/B_ = Actor, W_/WBP_ = Widget, ABP_ = AnimBlueprint, IMC_ = InputMapping,
  AN_ = AnimNotify, EQS_ = EQS query, etc.)
- Its kind and parent class
- Events it overrides, variables, called functions, exec chains

ALWAYS produce a sentence, even with minimal facts — infer purpose from the name and kind.
Focus on WHAT the Blueprint does gameplay-wise. Do NOT describe its implementation details.
Output ONLY one sentence, no quotes, no prefix like "This blueprint".
Keep it under 35 words."""


def build_user_prompt(facts: dict) -> str:
    """Turn structural facts into a compact LLM prompt."""
    lines = [f"Blueprint: {facts['asset_name']}"]
    lines.append(f"kind: {facts['kind']}")
    if facts.get("parent_class_hint"):
        lines.append(f"inherits_event_from: {facts['parent_class_hint']}")
    if facts.get("events"):
        lines.append(f"events: {', '.join(facts['events'])}")
    if facts.get("custom_events"):
        lines.append(f"custom_events: {', '.join(facts['custom_events'])}")

    vars_ = facts.get("variables") or []
    if vars_:
        vars_short = [f"{v['name']}({v['type']})" for v in vars_[:8]]
        lines.append(f"variables: {', '.join(vars_short)}")

    calls = facts.get("calls") or []
    if calls:
        # cap to keep prompt compact
        lines.append(f"calls: {', '.join(calls[:15])}")

    casts = facts.get("casts_to") or []
    if casts:
        lines.append(f"casts_to: {', '.join(casts)}")

    chains = facts.get("exec_chains") or {}
    for evt, chain in list(chains.items())[:3]:
        lines.append(f"on {evt}: {chain}")

    lines.append("")
    lines.append("Describe what this blueprint does in one sentence:")
    return "\n".join(lines)


def call_llama(prompt: str, system_prompt: str = SYSTEM_PROMPT,
               max_tokens: int = DEFAULT_MAX_TOKENS,
               temperature: float = DEFAULT_TEMPERATURE,
               timeout: float = 60.0) -> str:
    body = {
        "model": "auto",
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": prompt},
        ],
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": False,
    }
    data = json.dumps(body).encode("utf-8")
    req = Request(
        f"{LLAMA_URL}/v1/chat/completions",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(req, timeout=timeout) as r:
        resp = json.loads(r.read())
    choices = resp.get("choices") or []
    if not choices:
        return ""
    msg = choices[0].get("message") or {}
    return (msg.get("content") or "").strip()


def sanitize_purpose(raw: str) -> str:
    """Clean up LLM output: strip quotes, trailing punct, etc."""
    s = raw.strip()
    # Strip surrounding quotes
    for q in ("\"", "'", "`"):
        if s.startswith(q) and s.endswith(q) and len(s) >= 2:
            s = s[1:-1].strip()
    # Strip markdown fences
    if s.startswith("```") and s.endswith("```"):
        s = s.strip("`").strip()
    # If model answered with a leading "A " or "The " prefix — fine, keep
    # Collapse whitespace
    s = " ".join(s.split())
    # Drop trailing period for consistency
    if s.endswith("."):
        s = s[:-1]
    return s


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default=r"G:\Proj\Wick\data\bp_facts.json")
    parser.add_argument("--output", default=r"G:\Proj\Wick\data\bp_purposes.json")
    parser.add_argument("--limit", type=int, default=0, help="0 = all BPs")
    parser.add_argument("--max-tokens", type=int, default=DEFAULT_MAX_TOKENS)
    parser.add_argument("--temperature", type=float, default=DEFAULT_TEMPERATURE)
    parser.add_argument("--force", action="store_true",
                        help="regenerate even if structural_hash matches cached purpose")
    args = parser.parse_args()

    # Wait for llama-server to be ready
    print(f"Connecting to llama-server at {LLAMA_URL}...")
    for attempt in range(30):
        try:
            with urlopen(f"{LLAMA_URL}/v1/models", timeout=3) as r:
                models = json.loads(r.read())
                if models.get("data"):
                    mid = models["data"][0].get("id", "?")
                    print(f"  ✓ llama-server ready, model={mid}")
                    break
        except (URLError, json.JSONDecodeError, TimeoutError):
            time.sleep(2)
    else:
        print(f"ERROR: llama-server not responding at {LLAMA_URL}", file=sys.stderr)
        return 2

    # Load structural facts
    input_path = Path(args.input)
    if not input_path.is_file():
        print(f"ERROR: facts file not found: {input_path}", file=sys.stderr)
        print("       Run parse_bp_facts.py first.", file=sys.stderr)
        return 1

    with open(input_path, encoding="utf-8") as fp:
        all_facts = json.load(fp)
    print(f"Loaded {len(all_facts)} BP fact sets from {input_path}")

    # Load existing purposes (for restart)
    output_path = Path(args.output)
    existing: dict[str, dict] = {}
    if output_path.is_file() and not args.force:
        with open(output_path, encoding="utf-8") as fp:
            for item in json.load(fp):
                existing[item["entity"]] = item
        print(f"Loaded {len(existing)} cached purposes from {output_path}")

    if args.limit > 0:
        all_facts = all_facts[: args.limit]
        print(f"--limit={args.limit} → processing {len(all_facts)} BPs")

    # Generate
    results: list[dict] = []
    skipped_cache = 0
    empty_out = 0
    errors = 0
    total_ms = 0.0
    t_all = time.perf_counter()

    for i, facts in enumerate(all_facts, 1):
        entity = facts["entity"]
        cached = existing.get(entity)
        if cached and cached.get("structural_hash") == facts.get("structural_hash") and not args.force:
            results.append(cached)
            skipped_cache += 1
            continue

        prompt = build_user_prompt(facts)
        try:
            t0 = time.perf_counter()
            raw = call_llama(prompt, max_tokens=args.max_tokens, temperature=args.temperature)
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            total_ms += elapsed_ms
            purpose = sanitize_purpose(raw) or "unknown"
            if purpose == "unknown":
                empty_out += 1
        except Exception as e:
            purpose = ""
            elapsed_ms = 0.0
            errors += 1
            print(f"  ! {entity}: {e}", file=sys.stderr)

        results.append({
            "entity": entity,
            "asset_name": facts["asset_name"],
            "structural_hash": facts.get("structural_hash", ""),
            "purpose": purpose,
            "llm_ms": round(elapsed_ms, 1),
        })

        if i % 20 == 0 or i == 1 or i == len(all_facts):
            dt = time.perf_counter() - t_all
            rate = i / dt if dt > 0 else 0
            eta = (len(all_facts) - i) / rate if rate > 0 else 0
            print(f"  [{i:>3}/{len(all_facts)}] {entity[:45]:<45}  "
                  f"{elapsed_ms:>5.0f}ms  rate={rate:.2f}/s  eta={eta:.0f}s")
            print(f"         → {purpose[:100]}")

    # Save periodically in case of interruption is done via atexit, but write now
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as fp:
        json.dump(results, fp, ensure_ascii=False, indent=2)

    elapsed = time.perf_counter() - t_all
    print(f"\n✓ Wrote {len(results)} purposes to {output_path}")
    print(f"  Total time: {elapsed:.1f}s  (skipped from cache: {skipped_cache}, errors: {errors}, empty: {empty_out})")
    if total_ms > 0 and (len(all_facts) - skipped_cache) > 0:
        avg = total_ms / max(1, len(all_facts) - skipped_cache)
        print(f"  Avg LLM call: {avg:.0f} ms")

    return 0


if __name__ == "__main__":
    sys.exit(main())
