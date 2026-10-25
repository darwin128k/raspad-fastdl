from pathlib import Path
import socket
import os
import subprocess
import sys
import tempfile

runner,module=map(lambda p:str(Path(p).resolve()),sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='fastdl-module-') as tmp:
    base=Path(tmp); (base/'configs').mkdir(); (base/'data'/'public').mkdir(parents=True)
    with socket.socket() as busy:
        busy.bind(('127.0.0.1',0)); busy.listen()
        occupied=busy.getsockname()[1]
        cfg=base/'configs'/'fastdl.cfg'
        cfg.write_text(f'port = {occupied}\nroot = {base / "data" / "public"}\n')
        subprocess.run([runner,module,str(base),'0'],check=True,timeout=10)
    (base/'data'/'public'/'maps').mkdir()
    (base/'data'/'public'/'maps'/'large.bsp').write_bytes(b'x'*4000000)
    # Reuse the now-free port and exercise 20 cycles in one loaded DSO.
    subprocess.run([runner,module,str(base),'1'],check=True,timeout=10)
    subprocess.run([runner,module,str(base),'0'],env={**os.environ,'FASTDL_TEST_OLD_HOST':'1'},check=True,timeout=10)
    with cfg.open('a') as f:
        f.write(f'cpu = {min(os.sched_getaffinity(0))}\n')
    subprocess.run([runner,module,str(base),'1'],check=True,timeout=10)
    # Standard layout: both absent and blank root resolve to the game root.
    standard=base/'game'/'addons'/'authum'/'modules'/'fastdl'
    (standard/'configs').mkdir(parents=True)
    (standard/'data').mkdir()
    for root_setting in ['', 'root =\n']:
        (standard/'configs'/'fastdl.cfg').write_text(f'port = {occupied}\n'+root_setting)
        subprocess.run([runner,module,str(standard),'1'],check=True,timeout=10)
    # No helper is installed anywhere: the DSO owns its HTTP thread.
    (standard/'configs'/'fastdl.cfg').write_text('source = {unknown}\n')
    subprocess.run([runner,module,str(standard),'0'],check=True,timeout=10)
    cfg.write_text('port = invalid\n')
    subprocess.run([runner,module,str(base),'0'],check=True,timeout=10)
print('PASS: real i386 DSO load, thread startup, occupied-port failure, repeated start/stop, thread/FD cleanup and host-state preservation, invalid configuration')
