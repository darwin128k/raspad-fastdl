"""Package the single installed Authum module and its configuration."""
from pathlib import Path
import tarfile
import zipfile

dist = Path(__file__).resolve().parents[1] / 'dist'
source = dist / 'linux32'
module = source / 'addons/authum/modules/fastdl/fastdl.so'
if not module.is_file():
    raise SystemExit('Install the fastdl CMake component before packaging')
with tarfile.open(dist / 'fastdl-linux32-authum.tar.gz', 'w:gz') as tar, \
     zipfile.ZipFile(dist / 'fastdl-linux32-authum.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(source.rglob('*')):
        if not path.is_file():
            continue
        name = path.relative_to(source).as_posix()
        if path.name == 'fastdl-http':
            continue  # Never ship obsolete helper binaries from older installations.
        mode = 0o644
        entry = tar.gettarinfo(str(path), arcname=name)
        entry.mode = mode
        entry.uid = entry.gid = 0
        entry.uname = entry.gname = ''
        with path.open('rb') as file:
            tar.addfile(entry, file)
        z = zipfile.ZipInfo(name)
        z.create_system = 3
        z.external_attr = (0o100000 | mode) << 16
        z.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(z, path.read_bytes())
print('Created FastDL TAR.GZ and ZIP packages')
