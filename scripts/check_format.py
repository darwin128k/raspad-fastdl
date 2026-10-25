"""Check maintained C sources without rewriting vendor code or legacy files."""
from pathlib import Path
import argparse
import shutil
import subprocess

formatter = shutil.which('clang-format')
try:
    import clang_format
    packaged = Path(clang_format.__file__).parent / 'data/bin/clang-format'
    if packaged.is_file():
        formatter = str(packaged)
except ImportError:
    pass
parser = argparse.ArgumentParser()
parser.add_argument('--formatter', default=formatter)
args = parser.parse_args()
if not args.formatter:
    raise SystemExit('Install clang-format 18.1.8 or pass --formatter PATH')
root = Path(__file__).resolve().parents[1]
authum = root / 'lib/authum'
files = list((root / 'src').rglob('*.c'))
files += list((root / 'include').rglob('*.h'))
files += list((root / 'tests').glob('*.c'))
files += list((authum / 'src/authum/host/http').glob('*.[ch]'))
for name in ['http', 'clock', 'thread']:
    files += [authum / f'include/authum/{name}.h', authum / f'src/authum/api/{name}.c']
files += [authum / name for name in [
    'src/authum/host/http.h', 'src/authum/host/http_windows.c',
    'src/authum/host/clock.h', 'src/authum/host/clock.c',
    'src/authum/host/threads.h', 'src/authum/host/threads.c',
    'tests/http.c', 'tests/threads.c']]
subprocess.run([args.formatter, '--dry-run', '--Werror', '-style=file',
                *map(str, sorted(set(files)))], check=True)
print(f'Formatting OK: {len(set(files))} files')
