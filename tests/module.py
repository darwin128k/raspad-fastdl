from pathlib import Path
import socket
import os
import subprocess
import sys
import tempfile

runner,module=map(lambda p:str(Path(p).resolve()),sys.argv[1:])
skipped=[]
reported=[]

def run(args):
    """Run the driver, then echo the platform line it reports about itself."""
    done=subprocess.run(args,check=False,timeout=10,capture_output=True,text=True)
    for line in done.stdout.splitlines():
        if line.startswith('platform=') and line not in reported:
            reported.append(line); print(line)
    if done.returncode!=0:
        print(done.stdout); print(done.stderr)
        done.check_returncode()
    return done

with tempfile.TemporaryDirectory(prefix='fastdl-module-') as tmp:
    base=Path(tmp); (base/'configs').mkdir(); (base/'data'/'public').mkdir(parents=True)
    with socket.socket() as busy:
        if hasattr(socket,'SO_EXCLUSIVEADDRUSE'):
            # The host listener sets SO_REUSEADDR, which on Windows lets a
            # second bind succeed: there the port has to be held exclusively.
            busy.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
        busy.bind(('127.0.0.1',0)); busy.listen()
        occupied=busy.getsockname()[1]
        cfg=base/'configs'/'fastdl.cfg'
        cfg.write_text(f'port = {occupied}\nroot = {base / "data" / "public"}\n')
        run([runner,module,str(base),'0'])
    (base/'data'/'public'/'maps').mkdir()
    (base/'data'/'public'/'maps'/'large.bsp').write_bytes(b'x'*4000000)
    # Reuse the now-free port and exercise 20 cycles in one loaded DSO.
    run([runner,module,str(base),'1'])
    # The affinity mask is a Linux-only check: where the platform has no
    # affinity call, the driver never sees an explicit CPU.
    if hasattr(os,'sched_getaffinity'):
        with cfg.open('a') as f:
            f.write(f'cpu = {min(os.sched_getaffinity(0))}\n')
        run([runner,module,str(base),'1'])
    else:
        skipped.append('cpu_affinity_configuration')
    # Standard layout: both absent and blank root resolve to the game root.
    standard=base/'game'/'addons'/'authum'/'modules'/'fastdl'
    (standard/'configs').mkdir(parents=True)
    (standard/'data').mkdir()
    for root_setting in ['', 'root =\n']:
        (standard/'configs'/'fastdl.cfg').write_text(f'port = {occupied}\n'+root_setting)
        run([runner,module,str(standard),'1'])
    # No helper is installed anywhere: the DSO owns its HTTP thread.
    (standard/'configs'/'fastdl.cfg').write_text('source = {unknown}\n')
    run([runner,module,str(standard),'0'])
    cfg.write_text('port = invalid\n')
    run([runner,module,str(base),'0'])
print('SKIP: '+(', '.join(skipped) if skipped else 'none'))
print('PASS: real i386 module load, thread startup, occupied-port failure, repeated start/stop, thread/FD cleanup and host-state preservation, invalid configuration')
