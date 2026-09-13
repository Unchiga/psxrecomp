"""Check raster output against the unoptimized c75132a6 corpus (64 cases).

Includes native, 1x–4x mirrors, wide targets, filtering, clipping, UV wrap,
negative UV spans, mask bits, transparency, wrapping fills and dirty rows.
Run directly with Python; requires a C compiler, no SDL or game assets.
"""
import difflib
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
runtime = here.parent
with tempfile.TemporaryDirectory(prefix='psx-sw-parity-') as scratch:
    exe = Path(scratch) / 'raster'
    subprocess.run([os.environ.get('CC', 'cc'), '-O3', '-I', str(runtime / 'include'),
                    '-I', str(runtime / 'src'), str(here / 'test_gpu_sw_raster_parity.c'),
                    str(runtime / 'src/gpu_vram_dirty.c'), '-lm', '-o', str(exe)], check=True)
    actual = subprocess.check_output([str(exe)], text=True)
expected = (here / 'test_gpu_sw_raster_parity.expected').read_text()
if actual != expected:
    raise AssertionError(''.join(difflib.unified_diff(expected.splitlines(True),
                                                    actual.splitlines(True),
                                                    fromfile='original', tofile='current')))
print('ok: 64 raster cases match original VRAM, mirrors and dirty rows exactly')
