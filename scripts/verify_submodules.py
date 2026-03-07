#!/usr/bin/env python3
import argparse
import configparser
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
from typing import Dict, List, Optional

ROOT = pathlib.Path(__file__).resolve().parents[1]
GITMODULES = ROOT / '.gitmodules'
LOCK = ROOT / 'submodules.lock'

REQUIRED_PATHS = {
    'third_party/usearch',
    'third_party/sqlite',
    'third_party/googletest',
    'third_party/poco',
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Validate submodule lock policy and manifest checksums.')
    parser.add_argument(
        '--enforce-pin-required',
        action='store_true',
        help='Fail if any pinned_commit remains as <PIN_REQUIRED> placeholder.'
    )
    parser.add_argument(
        '--require-checksum-submodules-present',
        action='store_true',
        help='Fail if any checksum-verified submodule checkout is missing locally.'
    )
    parser.add_argument(
        '--require-gitlinks-present',
        action='store_true',
        help='Fail if required submodule gitlinks (mode 160000) are missing from repository index.'
    )
    return parser.parse_args()


def env_truthy(name: str) -> bool:
    value = os.getenv(name, '').strip().lower()
    return value in {'1', 'true', 'yes', 'on'}


def fail(msg: str) -> None:
    print(f'[verify_submodules] ERROR: {msg}', file=sys.stderr)
    sys.exit(1)


def warn(msg: str) -> None:
    print(f'[verify_submodules] WARNING: {msg}')


def parse_bool(text: str) -> Optional[bool]:
    normalized = text.strip().lower()
    if normalized == 'true':
        return True
    if normalized == 'false':
        return False
    return None


def parse_lock_file(text: str) -> Dict[str, Dict[str, object]]:
    entries_by_path: Dict[str, Dict[str, object]] = {}
    current_path: Optional[str] = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('path:'):
            current_path = line.split(':', 1)[1].strip()
            if current_path:
                entries_by_path.setdefault(current_path, {})
            continue
        if current_path is None:
            continue
        if line.startswith('pinned_commit:'):
            pinned = line.split(':', 1)[1].strip().strip('"')
            entries_by_path[current_path]['pinned_commit'] = pinned
            continue
        if line.startswith('remote:'):
            remote = line.split(':', 1)[1].strip().strip('"')
            entries_by_path[current_path]['remote'] = remote
            continue
        if line.startswith('verify_checksum:'):
            raw_value = line.split(':', 1)[1].strip()
            parsed = parse_bool(raw_value)
            if parsed is None:
                fail(f'invalid verify_checksum boolean for {current_path}: {raw_value}')
            entries_by_path[current_path]['verify_checksum'] = parsed
            continue
        if line.startswith('required_manifest:'):
            manifest = line.split(':', 1)[1].strip().strip('"')
            entries_by_path[current_path]['required_manifest'] = manifest
            continue
    return entries_by_path


def parse_submodule_status() -> Dict[str, str]:
    cmd = ['git', 'submodule', 'status', '--recursive']
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
    status: Dict[str, str] = {}
    for raw_line in result.stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        # Format: "<prefix><sha> <path> ..."
        match = re.match(r'^[\-\+U ]?([0-9a-f]{40})\s+([^\s]+)', line)
        if not match:
            continue
        sha = match.group(1)
        path = match.group(2)
        status[path] = sha
    return status


def parse_index_gitlinks() -> Dict[str, str]:
    cmd = ['git', 'ls-files', '--stage']
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
    gitlinks: Dict[str, str] = {}
    for raw_line in result.stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        # Format: "<mode> <sha> <stage>\t<path>"
        match = re.match(r'^([0-9]{6})\s+([0-9a-f]{40})\s+[0-3]\t(.+)$', line)
        if not match:
            continue
        mode = match.group(1)
        sha = match.group(2)
        path = match.group(3)
        if mode == '160000':
            gitlinks[path] = sha
    return gitlinks


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def manifest_artifacts(manifest_path: pathlib.Path) -> List[Dict[str, object]]:
    try:
        data = json.loads(manifest_path.read_text(encoding='utf-8'))
    except Exception as exc:  # pragma: no cover - defensive parse guard
        fail(f'failed to parse manifest {manifest_path}: {exc}')

    artifacts = None
    if isinstance(data, list):
        artifacts = data
    elif isinstance(data, dict):
        for key in ('artifacts', 'files', 'entries'):
            value = data.get(key)
            if isinstance(value, list):
                artifacts = value
                break
    if not isinstance(artifacts, list) or not artifacts:
        fail(f'manifest {manifest_path} does not define a non-empty artifact list')
    for entry in artifacts:
        if not isinstance(entry, dict):
            fail(f'manifest {manifest_path} contains non-object artifact entry')
    return artifacts


def validate_manifest_checksums(submodule_root: pathlib.Path, manifest_rel: str) -> None:
    manifest_path = submodule_root / manifest_rel
    if not manifest_path.exists():
        fail(f'missing required manifest: {manifest_path}')

    artifacts = manifest_artifacts(manifest_path)
    root_resolved = submodule_root.resolve()
    validated = 0
    for idx, artifact in enumerate(artifacts):
        rel_path = artifact.get('path')
        if not isinstance(rel_path, str) or not rel_path.strip():
            rel_path = artifact.get('file')
        if not isinstance(rel_path, str) or not rel_path.strip():
            fail(f'manifest {manifest_path} artifact #{idx} missing path/file field')

        expected = artifact.get('sha256')
        if not isinstance(expected, str) or not expected.strip():
            expected = artifact.get('sha256sum')
        if not isinstance(expected, str):
            fail(f'manifest {manifest_path} artifact #{idx} missing sha256')
        expected = expected.strip().lower()
        if re.fullmatch(r'[0-9a-f]{64}', expected) is None:
            fail(f'manifest {manifest_path} artifact #{idx} has invalid sha256: {expected}')

        candidate = (submodule_root / rel_path).resolve()
        try:
            candidate.relative_to(root_resolved)
        except ValueError:
            fail(f'manifest {manifest_path} artifact #{idx} escapes submodule root: {rel_path}')
        if not candidate.exists():
            fail(f'manifest {manifest_path} artifact #{idx} file is missing: {candidate}')
        if candidate.is_dir():
            fail(f'manifest {manifest_path} artifact #{idx} points to directory: {candidate}')
        if candidate.stat().st_size == 0:
            fail(f'manifest {manifest_path} artifact #{idx} file is empty: {candidate}')

        actual = sha256_file(candidate)
        if actual != expected:
            fail(f'checksum mismatch for {candidate}: expected {expected}, got {actual}')
        validated += 1

    print(f'[verify_submodules] checksum OK: {validated} artifacts validated from {manifest_path}')


if not GITMODULES.exists():
    fail('.gitmodules file is missing')

if not LOCK.exists():
    fail('submodules.lock is missing')

parser = configparser.ConfigParser()
parser.read(GITMODULES, encoding='utf-8')

paths = set()
remote_by_path: Dict[str, str] = {}
for section in parser.sections():
    if not section.startswith('submodule '):
        continue
    path = parser.get(section, 'path', fallback='').strip()
    url = parser.get(section, 'url', fallback='').strip()
    branch = parser.get(section, 'branch', fallback='').strip()
    if not path:
        fail(f'{section} missing path')
    if not url:
        fail(f'{section} missing url')
    if branch:
        fail(f'{section} uses branch tracking ({branch}); commit-pinned policy requires no branch field')
    paths.add(path)
    remote_by_path[path] = url

missing = REQUIRED_PATHS - paths
if missing:
    fail(f'missing required submodule paths: {sorted(missing)}')

lock_text = LOCK.read_text(encoding='utf-8')
for path in sorted(REQUIRED_PATHS):
    if path not in lock_text:
        fail(f'lock file does not mention {path}')

lock_entries = parse_lock_file(lock_text)
for path in sorted(REQUIRED_PATHS):
    if path not in lock_entries:
        fail(f'lock file missing entry for {path}')
    if 'pinned_commit' not in lock_entries[path]:
        fail(f'lock file missing pinned_commit for {path}')
    lock_remote = lock_entries[path].get('remote')
    if not isinstance(lock_remote, str) or not lock_remote.strip():
        fail(f'lock file missing remote for {path}')
    declared_remote = remote_by_path.get(path, '')
    if declared_remote != lock_remote:
        fail(f'remote mismatch for {path}: .gitmodules has {declared_remote}, lock has {lock_remote}')

args = parse_args()
enforce_pin_required = args.enforce_pin_required or env_truthy('WAXCPP_ENFORCE_PIN_REQUIRED')
require_checksum_submodules_present = (
    args.require_checksum_submodules_present or env_truthy('WAXCPP_REQUIRE_CHECKSUM_SUBMODULES_PRESENT')
)
require_gitlinks_present = (
    args.require_gitlinks_present or env_truthy('WAXCPP_REQUIRE_GITLINKS_PRESENT')
)
placeholder_paths = sorted(
    path for path, entry in lock_entries.items() if entry.get('pinned_commit') == '<PIN_REQUIRED>'
)
if placeholder_paths:
    joined = ', '.join(placeholder_paths)
    message = (
        'pinned commits are placeholders and must be updated before release '
        f'(unresolved: {joined})'
    )
    if enforce_pin_required:
        fail(message)
    warn(message)

status = parse_submodule_status()
if not status:
    warn('no initialized submodule gitlinks found in index yet')
else:
    for path in sorted(REQUIRED_PATHS):
        if path not in status:
            warn(f'{path} is declared but not initialized')
            continue
        pinned = str(lock_entries.get(path, {}).get('pinned_commit', '<PIN_REQUIRED>'))
        if pinned != '<PIN_REQUIRED>' and pinned != status[path]:
            fail(f'commit mismatch for {path}: expected {pinned}, got {status[path]}')

index_gitlinks = parse_index_gitlinks()
missing_gitlinks = sorted(path for path in REQUIRED_PATHS if path not in index_gitlinks)
if missing_gitlinks:
    message = f'required submodule gitlinks are missing from index: {missing_gitlinks}'
    if require_gitlinks_present:
        fail(message)
    warn(message)

for path in sorted(REQUIRED_PATHS):
    entry = lock_entries.get(path, {})
    if entry.get('verify_checksum') is not True:
        continue
    manifest_rel = str(entry.get('required_manifest', '')).strip()
    if not manifest_rel:
        fail(f'checksum-verified submodule {path} is missing required_manifest')

    submodule_root = ROOT / path
    if not submodule_root.exists():
        message = f'{path} checksum verification skipped: submodule checkout is missing locally'
        if require_checksum_submodules_present:
            fail(message)
        warn(message)
        continue
    validate_manifest_checksums(submodule_root, manifest_rel)

print('[verify_submodules] OK: submodule policy files are consistent')
