"""Loopback checks of the same HTTP worker used by the production module."""
import concurrent.futures
import os
from pathlib import Path
import signal
import socket
import struct
import re
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())

def log_text(proc):
    proc.log_file.seek(0)
    return proc.log_file.read().decode(errors='replace')

def request(port, target, method='GET', extra=b''):
    with socket.create_connection(('127.0.0.1', port), timeout=4) as s:
        s.sendall(method.encode()+b' '+target.encode()+b' HTTP/1.0\r\n'+extra+b'\r\n')
        data=b''
        while True:
            b=s.recv(65536)
            if not b: break
            data+=b
        return data

with tempfile.TemporaryDirectory(prefix='fastdl-test-') as tmp:
    base=Path(tmp); root=base/'public'; (root/'maps').mkdir(parents=True)
    payload=os.urandom(180000)
    (root/'maps'/'test.bsp').write_bytes(payload)
    (root/'maps'/'$1000$.bsp').write_bytes(payload)
    (root/'maps'/'secret.cfg').write_text('password')
    (root/'maps'/'private.bsp').write_text('private')
    (root/'maps'/'empty.bsp').touch()
    (root/'test.wad').write_bytes(b'wad')
    outside=base/'outside'; outside.mkdir(); (outside/'secret.bsp').write_text('secret')
    (root/'maps'/'link.bsp').symlink_to(outside/'secret.bsp')
    (root/'maps'/'linkdir').symlink_to(outside, target_is_directory=True)
    def start(extra='', control=False, hold_logs=False):
        with socket.socket() as s:
            s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
        cfg=base/f'{port}.cfg'
        cfg.write_text(f'root = {root}\nport = {port}\nrequests_per_second = 1000\nper_ip = 64\nmax_clients = 64\ntimeout_seconds = 1\ndeny_prefixes = maps/private.bsp\n'+extra)
        args=[binary,'--config',str(cfg),'--base',str(base),'--game-dir',str(root)]
        parent=child=None
        if control:
            parent,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
            args+=['--control-fd',str(child.fileno())]
        log_file=tempfile.TemporaryFile()
        proc=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=log_file,
                              env={**os.environ,**({'FASTDL_TEST_HOLD_LOGS':'1'} if hold_logs else {})},
                              pass_fds=(() if child is None else (child.fileno(),)))
        proc.log_file=log_file
        if child: child.close()
        if parent:
            parent.settimeout(5); assert parent.recv(256)==b'READY'
        else:
            deadline=time.monotonic()+5
            while True:
                if proc.poll() is not None: raise AssertionError(log_text(proc))
                try:
                    with socket.create_connection(('127.0.0.1',port),timeout=.1): break
                except OSError:
                    if time.monotonic()>deadline: raise
                    time.sleep(.02)
        return proc,port,cfg,parent

    proc,port,cfg,_=start()
    try:
        result=request(port,'/maps/test.bsp')
        assert result.startswith(b'HTTP/1.0 200 ')
        assert result.split(b'\r\n\r\n',1)[1]==payload
        dollar=request(port,'/maps/$1000$.bsp')
        assert dollar.startswith(b'HTTP/1.0 200 ')
        assert dollar.split(b'\r\n\r\n',1)[1]==payload
        head=request(port,'/maps/test.bsp','HEAD')
        assert f'Content-Length: {len(payload)}'.encode() in head
        assert head.endswith(b'\r\n\r\n')
        assert request(port,'/maps/empty.bsp').startswith(b'HTTP/1.0 200 ')
        assert request(port,'/test.wad').endswith(b'wad')
        # Abruptly reset downloads while sendfile is active. The worker must
        # contain SIGPIPE without changing process-wide signal handlers.
        for _ in range(12):
            with socket.create_connection(('127.0.0.1',port)) as reset:
                reset.sendall(b'GET /maps/test.bsp HTTP/1.0\r\n\r\n')
                reset.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
        assert request(port,'/maps/test.bsp').endswith(payload)
        # Fragmented HTTP/1.1 headers must be assembled before parsing.
        with socket.create_connection(('127.0.0.1',port),timeout=4) as fragmented:
            for part in [b'GET /maps/test.bsp HTTP/1.1\r\n', b'Host: localhost\r\n',
                         b'User-Agent: browser-test\r\n', b'\r\n']:
                fragmented.sendall(part)
                time.sleep(.01)
            chunks=[]
            while True:
                part=fragmented.recv(65536)
                if not part: break
                chunks.append(part)
            assert b''.join(chunks).split(b'\r\n\r\n',1)[1]==payload
        for path in ['/maps/../outside/secret.bsp','/maps/%2e%2e/secret.bsp',
                     '/maps/link.bsp','/maps/linkdir/secret.bsp','/maps//test.bsp',
                     '/maps/.hidden.bsp','/maps/secret.cfg','/maps/private.bsp',
                     '/addons/test.bsp','/maps/','/test.bsp','/maps/test.bsp?x=1',
                     '//maps/test.bsp','/maps/test.bsp%00.cfg']:
            assert request(port,path).startswith(b'HTTP/1.0 404 '),path
        assert request(port,'/maps/test.bsp','POST').startswith(b'HTTP/1.0 405 ')
        for extra in [b'Content-Length: 1\r\n',b'Transfer-Encoding: chunked\r\n',b'bad header\r\n']:
            assert request(port,'/maps/test.bsp',extra=extra).startswith(b'HTTP/1.0 400 ')
        for index in range(100):
            prefix=['%2e%2e/', '../', '.hidden/', '..\\', 'x?y/', 'x#/', 'x%00/', './', '//', 'x@/'][index%10]
            invalid=request(port,f'/maps/{prefix}not-published-{index}.bsp')
            assert invalid.startswith((b'HTTP/1.0 400 ',b'HTTP/1.0 404 '))
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            for result in pool.map(lambda _: request(port,'/maps/test.bsp'),range(16)):
                assert result.split(b'\r\n\r\n',1)[1]==payload
        with socket.create_connection(('127.0.0.1',port)) as slow:
            slow.sendall(b'GET /maps/')
            time.sleep(1.3); slow.settimeout(2); assert slow.recv(10)==b''
        busy=subprocess.run([binary,'--config',str(cfg)],capture_output=True,timeout=5)
        assert busy.returncode!=0 and b'Address already in use' in busy.stderr
    finally:
        proc.terminate(); proc.wait(timeout=3)
        assert proc.returncode==0,log_text(proc)
    proc,port,_,control=start(control=True)
    control.close(); proc.wait(timeout=3); assert proc.returncode==0
    # Global transfer budget is enforced, rather than only per connection.
    proc,port,_,_=start('bytes_per_second = 100000\ntimeout_seconds = 5\n')
    try:
        before=time.monotonic(); result=request(port,'/maps/test.bsp')
        assert result.endswith(payload) and time.monotonic()-before>1.4
    finally:
        proc.terminate(); proc.wait(timeout=3)
    # Active connections and connection attempts share a bounded per-IP budget.
    proc,port,_,control=start('per_ip = 1\nrequests_per_second = 1\n',control=True)
    try:
        with socket.create_connection(('127.0.0.1',port)) as slow:
            slow.sendall(b'GET /maps/')
            time.sleep(.05)
            try: result=request(port,'/maps/test.bsp')
            except ConnectionResetError: result=b''
            assert not result
    finally:
        control.close(); proc.wait(timeout=3)
    # Ordered trees, all three placeholders, spaces in configured paths,
    # and security policy on every fallback source.
    extra=base/'extra tree'; (extra/'maps').mkdir(parents=True)
    (extra/'maps'/'test.bsp').write_bytes(b'override')
    (extra/'maps'/'extra.bsp').write_bytes(b'extra')
    data=base/'data'; (data/'maps').mkdir(parents=True)
    (data/'maps'/'data.bsp').write_bytes(b'data')
    (data/'maps'/'secret.cfg').write_text('secret')
    (data/'maps'/'link.bsp').symlink_to(outside/'secret.bsp')
    proc,port,_,_=start('source = {module}/extra tree\nsource = {game}\nsource = {data}\n')
    try:
        assert request(port,'/maps/test.bsp').endswith(b'override')
        assert request(port,'/maps/extra.bsp').endswith(b'extra')
        assert request(port,'/maps/data.bsp').endswith(b'data')
        assert request(port,'/maps/empty.bsp').startswith(b'HTTP/1.0 200 ')
        for path in ['/maps/secret.cfg','/maps/link.bsp','/maps/../outside/secret.bsp']:
            assert request(port,path).startswith(b'HTTP/1.0 404 ')
    finally:
        proc.terminate(); proc.wait(timeout=3)
    # Access records include peer, request, selected source, actual body bytes,
    # HEAD (zero body), status, and timeout. A disabled log remains silent.
    proc,port,_,_=start()
    try:
        assert request(port,'/maps/test.bsp').endswith(payload)
        request(port,'/maps/test.bsp','HEAD')
        request(port,'/maps/missing.bsp')
        request(port,'/maps/"injection.bsp')
        with socket.create_connection(('127.0.0.1',port)) as slow:
            slow.sendall(b'GET /maps/')
            time.sleep(1.3)
    finally:
        proc.terminate(); proc.wait(timeout=3)
    lines=log_text(proc).splitlines()
    assert any('event=connect' in line and 'ip=127.0.0.1:' in line for line in lines)
    assert any('event=request' in line and 'path="/maps/test.bsp"' in line for line in lines)
    assert any('event=complete' in line and 'method=GET' in line and f'bytes={len(payload)} ' in line and 'source=1 ' in line for line in lines)
    assert any('event=complete' in line and 'method=HEAD' in line and 'bytes=0 ' in line for line in lines)
    assert any('status=404' in line and 'path="/maps/missing.bsp"' in line for line in lines)
    assert any('event=timeout' in line for line in lines)
    assert any('path="/maps/_injection.bsp"' in line for line in lines)
    assert all('\r' not in line for line in lines)
    proc,port,_,_=start('access_log = 0\n')
    request(port,'/maps/test.bsp')
    proc.terminate(); proc.wait(timeout=3)
    assert 'http event=' not in log_text(proc)
    proc,port,_,_=start(hold_logs=True)
    for _ in range(100):
        request(port,'/maps/test.bsp','HEAD')
    proc.terminate(); proc.wait(timeout=3)
    overflow=log_text(proc).splitlines()
    assert any('event=log_overflow dropped=' in line for line in overflow)
    assert len([line for line in overflow if line.startswith('http event=')])<=257
    memory={}
    for count in [1,64,256]:
        proc,port,_,control=start(f'max_clients = {count}\n',control=True)
        control.close(); proc.wait(timeout=3)
        memory[count]=int(re.search(r'buffers_bytes=(\d+)',log_text(proc)).group(1))
    stride=(memory[64]-memory[1])//63
    assert stride>8192 and memory[64]-memory[1]==63*stride
    assert memory[256]-memory[64]==192*stride
    print(f'Client allocation: 64={memory[64]} bytes; 256={memory[256]} bytes; saved={memory[256]-memory[64]} bytes')
    bad=base/'bad.cfg'
    for setting in ['source = {unknown}\n','source = /nonexistent-fastdl-test-source\n','source =\n','source = {game}\n'*17]:
        bad.write_text(setting)
        failed=subprocess.run([binary,'--config',str(bad),'--base',str(base),'--game-dir',str(root)],capture_output=True,timeout=5)
        assert failed.returncode!=0,setting
    bad.write_text('port = 70000\n')
    assert subprocess.run([binary,'--config',str(bad)],capture_output=True).returncode!=0
print('PASS: content, HEAD, concurrency, traversal/symlinks, policy, framing, timeouts, busy port, parent EOF, bandwidth, per-IP limit, configuration')
