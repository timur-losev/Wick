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

SYSTEM_PROMPT = """You describe Unreal Engine Blueprints in ONE sentence based on structural facts.

OUTPUT RULES (strict):
1. Start with an action verb in present tense: "Spawns", "Applies", "Tracks", "Plays",
   "Restores", "Displays", "Opens", "Drives", "Enables", "Handles", "Maps", etc.
2. NEVER start with "This blueprint", "<Name> is", "A blueprint", "An actor blueprint".
3. Do NOT restate the kind or parent class — the caller already has that info.
   The sentence should describe BEHAVIOR, not classification.
4. Use the asset-name prefix to infer intent:
     GA_   → GameplayAbility      (describe what the ability does when activated)
     GE_   → GameplayEffect       (describe what stat/state it changes on targets)
     GC_ / GCNL_ → GameplayCue    (describe the visual/audio effect)
     W_ / WBP_   → Widget         (describe the UI element's visual role)
     ABP_ / ALI_ → AnimBlueprint  (describe which animation state it drives)
     AN_   → AnimNotify           (describe what fires at the notify point)
     BP_ / B_ → Actor             (describe the actor's gameplay function)
     IMC_  → InputMappingContext  (describe which actions it maps)
     IA_   → InputAction          (describe the player intent it represents)
     EQS_  → EQS query            (describe what it searches for)
     FX_ / NS_   → Niagara system
     PP_   → Post-process / prop
5. Focus on GAMEPLAY/UI effect, not implementation. Prefer "heals the caster for N" over
   "calls ApplyGameplayEffect with Heal spec".
6. Under 30 words. One sentence, no trailing period needed.

EXAMPLES (imitate this style):
Name: GA_Heal          → Restores health to the ability user while triggering a heal cue
Name: GA_SpawnEffect   → Applies a spawn-in effect and disables player input for a short delay, then restores input when the ability ends
Name: GE_DamageOverTime → Deals periodic damage to affected targets for a configured duration
Name: W_ScoreBoard     → Displays current match score and per-team stats with live updates
Name: W_RespawnTimer   → Shows a countdown while the player waits to respawn, then hides itself
Name: BP_Door          → Opens and closes an animated door when a character overlaps the trigger
Name: ABP_Weap_Pistol  → Drives pistol-specific animation updates each frame from the pawn owner
Name: AN_Melee         → Fires melee gameplay events on the owning actor at the notify point
Name: IMC_Default      → Maps core movement, jump, look, and fire actions to keyboard and gamepad
Name: GCNL_Dash        → Plays the dash visual effect and boosts the pawn briefly in its facing direction"""


def _folder_hint(asset_path: str) -> str:
    """Derive a short 'in .../Foo/Bar' hint from the asset path.

    Asset paths look like '/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect'.
    The last two directory segments usually carry the strongest semantic hint
    (e.g. "Game/Respawn" tells us this is about respawn mechanics).
    """
    if not asset_path or "/" not in asset_path:
        return ""
    # Strip the final ".AssetName" suffix and trailing filename
    trimmed = asset_path.rsplit(".", 1)[0]
    parts = [p for p in trimmed.split("/") if p]
    # parts = ['ShooterCore', 'Game', 'Respawn', 'GA_SpawnEffect']
    if len(parts) < 2:
        return ""
    # Take last 2-3 path segments, drop the leaf (asset name)
    hint_parts = parts[-3:-1] if len(parts) >= 4 else parts[:-1]
    return "/".join(hint_parts)


def build_user_prompt(facts: dict) -> str:
    """Turn structural facts into a compact LLM prompt."""
    lines = [f"Name: {facts['asset_name']}"]

    folder = _folder_hint(facts.get("asset_path", ""))
    if folder:
        lines.append(f"folder: {folder}")

    if facts.get("parent_class_hint"):
        lines.append(f"parent_context: {facts['parent_class_hint']}")
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
    lines.append("Describe what this blueprint does in ONE sentence starting with a verb:")
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


import re

# Patterns we consider generic openers that waste output budget and hurt search.
# Matched case-insensitively, must anchor at the start of the sentence.
#
# Trailing "(?:a\s+|an\s+|the\s+)?" absorbs the leading article of the next
# clause so we don't end up with sentences like "A respawn timer ...".
_GENERIC_OPENER_RES = [
    # "This blueprint manages a door that opens..."  → "Opens..."
    # "This blueprint provides an inventory UI which displays..." → "Displays..."
    re.compile(
        r"^this\s+blueprint\s+"
        r"(?:[a-z]+\s+)+"                   # verb phrase: "manages a ", "provides an "
        r"(?:that|which|for)\s+"
        r"(?:a\s+|an\s+|the\s+)?",
        re.IGNORECASE,
    ),
    # "GA_Foo is a gameplay ability that activates..." → "Activates..."
    # "BP_Foo is an actor blueprint that spawns..."    → "Spawns..."
    # Also handles bare "An actor blueprint that ..."
    re.compile(
        r"^(?:this blueprint\s+)?"
        r"(?:[A-Za-z0-9_]+\s+)?"            # optional asset name
        r"(?:is\s+)?(?:an?\s+)?"
        r"(?:actor\s+blueprint|gameplay\s+(?:ability|effect|cue)|widget|blueprint|anim(?:ation)?\s+(?:blueprint|notify))"
        r"\s+(?:that|which|for|representing|handling|used\s+to)\s+"
        r"(?:a\s+|an\s+|the\s+)?",
        re.IGNORECASE,
    ),
    # "A blueprint for a door that ..." / "A blueprint for controlling ..." → "..."
    re.compile(
        r"^a\s+blueprint\s+(?:for|that|which)\s+"
        r"(?:a\s+|an\s+|the\s+)?",
        re.IGNORECASE,
    ),
]


def _strip_generic_opener(s: str) -> str:
    """Remove the boilerplate intro clauses produced by some model outputs."""
    for pattern in _GENERIC_OPENER_RES:
        m = pattern.match(s)
        if m:
            rest = s[m.end():].lstrip()
            if rest:
                # Capitalise first letter so the sentence still reads well.
                return rest[0].upper() + rest[1:]
    return s


def sanitize_purpose(raw: str) -> str:
    """Clean up LLM output: strip quotes, generic openers, trailing punct, etc."""
    s = raw.strip()
    # Strip markdown code fences FIRST — ``` starts both with a quote-like char
    # and must be handled before the single-quote-char loop below.
    if s.startswith("```") and s.endswith("```") and len(s) >= 6:
        s = s[3:-3].strip()
    # Strip surrounding plain quotes (single or double only — not backticks,
    # because that would mis-strip ``...`` that somehow survived the markdown
    # check).
    for q in ('"', "'"):
        if s.startswith(q) and s.endswith(q) and len(s) >= 2:
            s = s[1:-1].strip()
    # Collapse whitespace
    s = " ".join(s.split())
    # Strip generic openers like "BP_X is an actor blueprint that ..."
    s = _strip_generic_opener(s)
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
