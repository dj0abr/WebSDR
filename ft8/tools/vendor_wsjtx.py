#!/usr/bin/env python3
"""Extract the pinned WSJT-X source subset; no network access required."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

COMMIT = '967c85a6119110788b0df3782cf796485b2cee14'
FILES = '''
lib/ft8_decode.f90
lib/timer_module.f90
lib/fftw3mod.f90
lib/crc.f90
lib/crc14.cpp
lib/four2a.f90
lib/packjt.f90
lib/pfx.f90
lib/77bit/packjt77.f90
lib/chkcall.f90
lib/fmtmsg.f90
lib/grid2deg.f90
lib/deg2grid.f90
lib/db.f90
lib/indexx.f90
lib/pctile.f90
lib/shell.f90
lib/smo121.f90
lib/polyfit.f90
lib/determ.f90
lib/peakup.f90
lib/platanh.f90
lib/nuttal_window.f90
lib/ft2/gfsk_pulse.f90
lib/ft8/ft8_params.f90
lib/ft8/ft8_a7.f90
lib/ft8/ft8_a8d.f90
lib/ft8/ft8apset.f90
lib/ft8/ft8b.f90
lib/ft8/ft8_downsample.f90
lib/ft8/sync8.f90
lib/ft8/sync8d.f90
lib/ft8/subtractft8.f90
lib/ft8/twkfreq1.f90
lib/ft8/genft8.f90
lib/ft8/gen_ft8wave.f90
lib/ft8/decode174_91.f90
lib/ft8/encode174_91.f90
lib/ft8/encode174_91_nocrc.f90
lib/ft8/osd174_91.f90
lib/ft8/get_crc14.f90
lib/ft8/get_spectrum_baseline.f90
lib/ft8/baseline.f90
lib/ft8/ldpc_174_91_c_generator.f90
lib/ft8/ldpc_174_91_c_parity.f90
'''.split()

def main():
    if len(sys.argv) != 2:
        sys.exit('Usage: vendor_wsjtx.py /path/to/wsjtx-checkout')
    upstream = Path(sys.argv[1]).resolve()
    commit = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != COMMIT:
        sys.exit(f'Expected WSJT-X commit {COMMIT}, got {commit}')
    subprocess.run(['git', '-C', str(upstream), 'diff', '--quiet', 'HEAD', '--'] +
                   FILES + ['AUTHORS', 'COPYING', 'lib/qra/q65/q65_set_list.f90',
                            'samples/FT8/210703_133430.wav'], check=True)
    root = Path(__file__).resolve().parents[1]
    dest = root / 'vendor/wsjtx'
    manifest = {'repository': 'https://github.com/WSJTX/wsjtx', 'commit': COMMIT, 'files': {}}
    for name in FILES + ['AUTHORS', 'COPYING']:
        source = upstream / name
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        manifest['files'][name] = hashlib.sha256(target.read_bytes()).hexdigest()
    # stdcall shares its upstream file with an unrelated Q65 encoder.
    name = 'lib/qra/q65/q65_set_list.f90'
    text = (upstream / name).read_text()
    fragment = text[text.index('subroutine stdcall('):]
    (dest / 'lib/stdcall.f90').write_text(fragment)
    manifest['fragments'] = {'lib/stdcall.f90': {'source': name, 'first_line': 66,
        'sha256': hashlib.sha256(fragment.encode()).hexdigest(),
        'source_sha256': hashlib.sha256((upstream / name).read_bytes()).hexdigest()}}
    sample = 'samples/FT8/210703_133430.wav'
    shutil.copyfile(upstream / sample, root / 'tests/data/210703_133430.wav')
    manifest['sample'] = {'source': sample, 'sha256': hashlib.sha256((upstream / sample).read_bytes()).hexdigest()}
    shutil.copyfile(upstream / 'COPYING', root / 'COPYING')
    (dest / 'UPSTREAM.json').write_text(json.dumps(manifest, indent=2) + '\n')
    sources = sorted('vendor/wsjtx/' + f for f in FILES if f.endswith(('.f90', '.cpp'))
                     and f not in ('lib/ft8_decode.f90', 'lib/ft8/ft8_params.f90') and not '/ldpc_' in f and not f.endswith('/pfx.f90'))
    sources.append('vendor/wsjtx/lib/stdcall.f90')
    (dest / 'sources.cmake').write_text('set(WSJTX_SOURCES\n' + ''.join('  ' + f + '\n' for f in sources) + ')\n')
    print(f'Extracted {len(FILES)} upstream files and stdcall at {COMMIT}')

if __name__ == '__main__':
    main()
