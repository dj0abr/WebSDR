#!/usr/bin/env python3
"""Call the standalone decoder from Python. SPDX-License-Identifier: GPL-3.0-only"""
import argparse
import json
from pathlib import Path
import subprocess

DEFAULT_BINARY = Path(__file__).resolve().parents[1] / 'build/ft8-decoder'

def decode_pcm16(samples: bytes, binary=DEFAULT_BINARY, utc=None):
    """Decode one 13..15s UTC-aligned mono 12kHz s16le audio block.

    Returns decode dictionaries. Raises on malformed audio or process failure.
    stdout contains one JSON object per line and ends with a completion object.
    """
    args = [str(binary), '--format', 's16le']
    if utc is not None:
        args += ['--utc', utc]
    result = subprocess.run(args + ['-'], input=samples, capture_output=True, timeout=45)
    if result.returncode:
        raise RuntimeError(f'Decoder exited {result.returncode}: {result.stderr.decode().strip()}')
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    if not rows or rows[-1].get('type') != 'complete':
        raise RuntimeError('Decoder did not finish its JSONL response')
    decodes = rows[:-1]
    if any(row.get('type') != 'decode' for row in decodes) or rows[-1]['decodes'] != len(decodes):
        raise RuntimeError('Invalid decoder response')
    return decodes

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw_file', type=Path, help='mono 12000 Hz signed PCM16 little-endian')
    parser.add_argument('--binary', type=Path, default=DEFAULT_BINARY)
    parser.add_argument('--utc', help='UTC slot label HHMMSS')
    args = parser.parse_args()
    for row in decode_pcm16(args.raw_file.read_bytes(), args.binary, args.utc):
        print(f"{row['snr_db']:+3d} dB  {row['dt_s']:+.3f} s  {row['frequency_hz']:.1f} Hz  {row['message']}")

if __name__ == '__main__':
    main()
