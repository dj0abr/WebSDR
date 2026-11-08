#!/usr/bin/env python3
"""End-to-end decoder and subprocess-protocol tests. SPDX-License-Identifier: GPL-3.0-only"""
import array
import io
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

exe, generator, data = sys.argv[1:]
checks = 0

def run(args, payload=None, code=0):
    global checks
    p = subprocess.run([exe, *map(str, args)], input=payload, capture_output=True, timeout=45)
    assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
    if code:
        assert p.stderr and not p.stdout, (args, p.stdout, p.stderr)
        checks += 1
        return []
    assert not p.stderr, (args, p.stderr)
    rows = [json.loads(line) for line in p.stdout.decode().splitlines()]
    assert rows and rows[-1]['type'] == 'complete', rows
    assert rows[-1]['decodes'] == len(rows) - 1, rows
    assert all(r['type'] == 'decode' for r in rows[:-1]), rows
    checks += 1
    return rows

def match(rows, message, frequency, dt, snr=None):
    found = [r for r in rows[:-1] if r['message'] == message]
    assert found, rows
    row = found[0]
    assert abs(row['frequency_hz'] - frequency) < 2, row
    assert abs(row['dt_s'] - dt) < 0.08, row
    if snr is not None:
        assert abs(row['snr_db'] - snr) < 4, row
    return row

def wav(payload, channels=1, rate=12000):
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(payload)
    return buf.getvalue()

sample = Path(data) / '210703_133430.wav'
rows = run(['--utc', '133430', sample])
messages = {r['message'] for r in rows[:-1]}
assert {'W1FC F5BZB -08', 'CQ F5RXL IN94', 'K1JT HA0DU KN07'} <= messages, rows
assert len(messages) == len(rows) - 1, 'Duplicate decodes across receive phases'
assert all(r['utc'] == '133430' for r in rows[:-1]), rows
print('Upstream recording:', rows[-1]['decodes'], 'decodes')

with tempfile.TemporaryDirectory() as temp:
    raw = Path(temp) / 'test.raw'
    subprocess.run([generator, str(raw), 'CQ K1ABC FN42', '1500', '0.2', '-12'], check=True)
    payload = raw.read_bytes()
    for depth in (1, 2, 3):
        rows = run(['--format', 's16le', '--depth', depth, raw])
        match(rows, 'CQ K1ABC FN42', 1500, 0.2, -12)
    # The process pipe path must be usable directly from an SDR application.
    rows = run(['--format', 's16le', '-'], payload)
    match(rows, 'CQ K1ABC FN42', 1500, 0.2)
    values = struct.unpack('<180000h', payload)
    floats = struct.pack('<180000f', *(v / 32768 for v in values))
    rows = run(['--format', 'f32le', '-'], floats)
    match(rows, 'CQ K1ABC FN42', 1500, 0.2)
    # Negative offsets and a different message type exercise timing convention.
    subprocess.run([generator, str(raw), 'K1ABC W9XYZ R-07', '900', '-0.25', '-18'], check=True)
    second = raw.read_bytes()
    rows = run(['--format', 's16le', '-'], second)
    match(rows, 'K1ABC W9XYZ R-07', 900, -0.25, -18)
    # Two simultaneous, unequal-strength signals must both be recovered.
    subprocess.run([generator, str(raw), 'K1ABC W9XYZ R-07', '900', '-0.25', '-14'], check=True)
    second = raw.read_bytes()
    mixed = array.array('h', (max(-32768, min(32767, a + b)) for a, b in
                              zip(values, struct.unpack('<180000h', second))))
    if sys.byteorder != 'little':
        mixed.byteswap()
    rows = run(['--format', 's16le', '-'], mixed.tobytes())
    match(rows, 'CQ K1ABC FN42', 1500, 0.2)
    match(rows, 'K1ABC W9XYZ R-07', 900, -0.25)
    # A search window excluding the signal must produce an empty completion.
    rows = run(['--format', 's16le', '--min-freq', '2200', '--max-freq', '2800', '-'], payload)
    assert rows[-1]['decodes'] == 0, rows
    # PCM16 WAV via stdin, including non-canonical RIFF chunk ordering/padding.
    regular = wav(payload)
    rows = run(['-'], regular)
    match(rows, 'CQ K1ABC FN42', 1500, 0.2)
    extra = b'JUNK' + struct.pack('<I', 3) + b'abc\0'
    body = regular[8:12] + extra + regular[12:]
    rows = run(['-'], b'RIFF' + struct.pack('<I', len(body)) + body)
    match(rows, 'CQ K1ABC FN42', 1500, 0.2)
    # The human-readable mode also returns decoded text.
    p = subprocess.run([exe, '--format', 's16le', '--output', 'text', '-'], input=payload,
                       capture_output=True, timeout=45, check=True)
    assert b'CQ K1ABC FN42' in p.stdout and not p.stderr, p
    checks += 1

silence = b'\0' * 360000
rows = run(['--format', 's16le', '-'], silence)
assert rows == [{'type': 'complete', 'decodes': 0, 'samples': 180000, 'sample_rate': 12000}], rows
rows = run(['--format', 's16le', '-'], silence[:312000])
assert rows[-1]['samples'] == 156000, rows
# Constant nonzero input is also not a message.
assert run(['--format', 's16le', '-'], b'\1\0' * 180000)[-1]['decodes'] == 0

for args in ([], ['missing.wav'], ['--wat'], ['--depth', '4', '-'],
             ['--depth', '-1', '-'], ['--depth', '999999999999999', '-'],
             ['--min-freq', '0', '-'], ['--max-freq', '6000', '-'],
             ['--min-freq', '1500', '--max-freq', '1501', '-'],
             ['--utc', '246000', '-'], ['--utc', '120001', '-'],
             ['--format', 'iq', '-'], ['--output', 'xml', '-'], ['--depth'], ['-', '-']):
    run(args, b'', code=2)
for bad in (b'', b'\0' * 311998, silence + b'\0', silence + b'\0\0'):
    run(['--format', 's16le', '-'], bad, code=2)
for value in (math.nan, math.inf, 1.01, -1.01):
    run(['--format', 'f32le', '-'], struct.pack('<f', value) + b'\0' * (720000 - 4), code=2)
for bad in (b'not a WAV', regular[:-10], wav(silence, rate=48000),
            wav(silence, channels=2), regular[:36] + b'NOPE' + regular[40:]):
    run(['-'], bad, code=2)
for option in ('--help', '--version'):
    p = subprocess.run([exe, option], capture_output=True, text=True, check=True)
    assert p.stdout and not p.stderr
    checks += 1
print(f'{checks} integration checks passed')
