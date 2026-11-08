#!/usr/bin/env python3
"""Verify vendored WSJT-X files against the recorded SHA-256 manifest."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
vendor = root / 'vendor/wsjtx'
manifest = json.loads((vendor / 'UPSTREAM.json').read_text())
for name, expected in manifest['files'].items():
    actual = hashlib.sha256((vendor / name).read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f'Modified upstream file: {name}')
for name, fragment in manifest['fragments'].items():
    if hashlib.sha256((vendor / name).read_bytes()).hexdigest() != fragment['sha256']:
        raise SystemExit(f'Modified upstream fragment: {name}')
sample = root / 'tests/data/210703_133430.wav'
if hashlib.sha256(sample.read_bytes()).hexdigest() != manifest['sample']['sha256']:
    raise SystemExit('Modified upstream sample')
if (root / 'COPYING').read_bytes() != (vendor / 'COPYING').read_bytes():
    raise SystemExit('Root license differs from upstream')
print(f"Verified {len(manifest['files'])} upstream files, stdcall fragment and recording at {manifest['commit']}")
