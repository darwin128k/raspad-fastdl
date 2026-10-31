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
files = list((root / 'src').rglob('*.c'))
files += list((root / 'include').rglob('*.h'))
files += list((root / 'tests').glob('*.c'))
# The SDK's own sources are formatted by the SDK: its .clang-format needs a
# newer clang-format than the one this project pins, so they are not checked here.
subprocess.run([args.formatter, '--dry-run', '--Werror', '-style=file',
                *map(str, sorted(set(files)))], check=True)
print(f'Formatting OK: {len(set(files))} files')
