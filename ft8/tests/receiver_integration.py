#!/usr/bin/env python3
"""Test the live IQ receiver and its subprocess protocol without SDR hardware."""
from pathlib import Path
import subprocess
import sys
import tempfile

test, generator = (Path(arg).resolve() for arg in sys.argv[1:])
decoder = test.parent / 'ft8-decoder'
with tempfile.TemporaryDirectory(prefix='ft8-live-test-') as directory:
    root = Path(directory)
    signal = root / 'signal.raw'
    subprocess.run([str(generator), str(signal), 'CQ K1ABC FN42', '1500', '0.2', '-12'],
                   check=True, timeout=10)
    scripts = {
        'web.py': '''import sys
assert len(sys.stdin.buffer.read()) == 720000
print(sys.argv[6] + ' -12 0.200 1500.000 ~ CQ K1ABC FN42')
print()
print(sys.argv[6] + ' -8 0.100 1113.500 ~ <...> CT9/DF7EE 73')
''',
        'inspect.py': '''import math, struct, sys
assert sys.argv[1:5] == ['--format','f32le','--output','text']
assert sys.argv[5] == '--utc' and len(sys.argv[6]) == 6 and sys.argv[7] == '-'
data = sys.stdin.buffer.read()
assert len(data) == 720000
values = struct.unpack('<180000f', data)
assert all(math.isfinite(v) and -1 <= v <= 1 for v in values)
assert abs(max(map(abs, values)) - 0.8) < 1e-6
print('OK')
''',
        'duplex.py': '''import sys
sys.stdout.write('X'*131072)
sys.stdout.flush()
assert len(sys.stdin.buffer.read()) == 720000
print('OK')
''',
        'early.py': '''import os, sys
os.close(0)
sys.stderr.write('intentional failure')
sys.exit(3)
''',
        'huge.py': "import sys\nsys.stdout.write('X'*300000)\nsys.stdout.flush()\n",
        'hang.py': 'import time\ntime.sleep(30)\n',
    }
    for name, body in scripts.items():
        path = root / name
        path.write_text('#!' + sys.executable + '\n' + body)
        path.chmod(0o755)
    # Another working directory also verifies that no project-relative input
    # or decoder paths leak into process execution.
    result = subprocess.run([str(test), str(signal), str(decoder), str(root)], cwd=root,
                            capture_output=True, text=True, timeout=120)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    result.check_returncode()
    for cause in ('SDR sample gap', 'IQ sample gap', 'band change', 'timestamp discontinuity'):
        assert cause in result.stderr, f'Missing discard cause: {cause}'
    assert result.stderr.count('discarding partial UTC slot') == 4, result.stderr
    assert 'sample delta +24000 at 2400000 samples/s' in result.stderr
    assert 'sample delta +' in result.stderr and 'collected ' in result.stderr
    assert 'UTC timeline preserved' in result.stderr
