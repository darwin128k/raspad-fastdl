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
WINDOWS = os.name == 'nt'
STOP_TIMEOUT = 5
skipped = []

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

def stop(proc):
    """Stop the driver the way its platform asks for, and report its exit code."""
    if WINDOWS:
        # Windows has no SIGTERM for another process: the console control
        # event is what the driver handles.
        try:
            proc.send_signal(signal.CTRL_BREAK_EVENT)
        except OSError:
            proc.terminate()
    else:
        proc.terminate()
    proc.wait(timeout=STOP_TIMEOUT)
    return proc.returncode

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
    # A symlink needs a privilege Windows grants only to administrators: say so
    # instead of pretending the escape through a link was refused.
    symlinks_ok=True
    for link,target,is_dir in [(root/'maps'/'link.bsp',outside/'secret.bsp',False),
                               (root/'maps'/'linkdir',outside,True)]:
        try:
            link.symlink_to(target, target_is_directory=is_dir)
        except OSError as error:
            symlinks_ok=False
            skipped.append(f'symlink_policy ({error.strerror})')
            break
    link_denied=['/maps/link.bsp','/maps/linkdir/secret.bsp'] if symlinks_ok else []
    def start(extra='', control=False, hold_logs=False):
        with socket.socket() as s:
            s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
        cfg=base/f'{port}.cfg'
        cfg.write_text(f'root = {root}\nport = {port}\nrequests_per_second = 1000\nper_ip = 64\nper_ip_pending = 16\nmax_clients = 64\ntimeout_seconds = 1\ndeny_prefixes = maps/private.bsp\n'+extra)
        args=[binary,'--config',str(cfg),'--base',str(base),'--game-dir',str(root)]
        parent=child=None
        listener=None
        if control:
            if WINDOWS:
                # A descriptor cannot cross processes on Windows: the driver
                # connects back over the loopback instead.
                listener=socket.socket(); listener.bind(('127.0.0.1',0)); listener.listen(1)
                args+=['--control-port',str(listener.getsockname()[1])]
            else:
                parent,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
                args+=['--control-fd',str(child.fileno())]
        log_file=tempfile.TemporaryFile()
        options={}
        if WINDOWS:
            options['creationflags']=subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            options['pass_fds']=() if child is None else (child.fileno(),)
        proc=subprocess.Popen(args,stdout=subprocess.DEVNULL,stderr=log_file,
                              env={**os.environ,**({'FASTDL_TEST_HOLD_LOGS':'1'} if hold_logs else {})},
                              **options)
        proc.log_file=log_file
        if child: child.close()
        if control:
            if WINDOWS:
                listener.settimeout(STOP_TIMEOUT)
                parent,_=listener.accept()
                listener.close()
            parent.settimeout(STOP_TIMEOUT); assert parent.recv(256)==b'READY'
        else:
            deadline=time.monotonic()+5
            while True:
                if proc.poll() is not None:
                    raise AssertionError(f'driver exited with {proc.returncode}: '
                                         f'{log_text(proc) or "(stderr empty)"}')
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
        denied=['/maps/../outside/secret.bsp','/maps/%2e%2e/secret.bsp']
        denied+=link_denied
        denied+=['/maps//test.bsp','/maps/.hidden.bsp','/maps/secret.cfg','/maps/private.bsp',
                 '/addons/test.bsp','/maps/','/test.bsp',
                 '//maps/test.bsp','/maps/test.bsp%00.cfg','/maps%2ftest.bsp',
                 '/maps/%5c..%5ctest.bsp','/maps/PRIVATE.BSP','/MAPS/private.bsp',
                 '/maps/test.bsp.','/maps/test.bsp%20','/maps/TEST~1.BSP','/maps/test.bsp::$DATA',
                 '/maps/%2e%2e%2fsecret.bsp','/maps/%252e%252e/test.bsp']
        for path in denied:
            assert request(port,path).startswith(b'HTTP/1.0 404 '),path
        # The target is decoded once; the query and fragment never name a file.
        for path in ['/maps/test.bsp?x=1','/maps/test.bsp#top','/maps/test%2Ebsp',
                     '/maps/%74est.bsp?a=%2e%2e/','/maps/test.BSP' if WINDOWS else '/maps/test.bsp']:
            assert request(port,path).split(b'\r\n\r\n',1)[1]==payload,path
        for path in ['/maps/test.bsp%','/maps/test.bsp%2','/maps/te%zzst.bsp','maps/test.bsp']:
            assert request(port,path).startswith(b'HTTP/1.0 400 '),path
        # An over-long target is 414, an over-long header block 431.
        # (Exactly one full buffer is sent: unread bytes would turn the close into a reset.)
        def raw(data):
            with socket.create_connection(('127.0.0.1',port),timeout=4) as s:
                s.sendall(data); out=b''
                while True:
                    b=s.recv(65536)
                    if not b: return out
                    out+=b
        assert raw(b'GET /maps/'+b'a'*(8192-10)).startswith(b'HTTP/1.0 414 ')
        head_line=b'GET /maps/test.bsp HTTP/1.0\r\nX-Pad: '
        assert raw(head_line+b'a'*(8192-len(head_line))).startswith(b'HTTP/1.0 431 ')
        many=b''.join(b'X-%d: 1\r\n'%i for i in range(80))
        assert request(port,'/maps/test.bsp',extra=many).startswith(b'HTTP/1.0 431 ')
        # An explicit empty body is fine on GET; any other body framing is not.
        assert request(port,'/maps/test.bsp',extra=b'Content-Length: 0\r\n').endswith(payload)
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
        # On Windows the listener holds its address exclusively: another
        # socket asking for SO_REUSEADDR must not be able to bind next to it.
        if WINDOWS:
            with socket.socket() as thief:
                thief.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
                try:
                    thief.bind(('127.0.0.1',port)); stolen=True
                except OSError:
                    stolen=False
            assert not stolen,'a second socket could bind the FastDL port'
        # The host listener sets SO_REUSEADDR, which on Windows lets a second
        # bind succeed: there the port is taken exclusively instead.
        if WINDOWS:
            with socket.socket() as taken:
                taken.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
                taken.bind(('127.0.0.1',0)); taken_port=taken.getsockname()[1]
                taken_cfg=base/'taken.cfg'
                taken_cfg.write_text(f'root = {root}\nport = {taken_port}\n')
                busy=subprocess.run([binary,'--config',str(taken_cfg)],capture_output=True,timeout=5)
        else:
            busy=subprocess.run([binary,'--config',str(cfg)],capture_output=True,timeout=5)
        assert busy.returncode!=0 and b'cannot listen on' in busy.stderr
        # The reason names the OS error, not "unknown error".
        assert b'(os error ' in busy.stderr and b'unknown error' not in busy.stderr,busy.stderr
    finally:
        assert stop(proc)==0,log_text(proc)
    proc,port,_,control=start(control=True)
    control.close(); proc.wait(timeout=STOP_TIMEOUT); assert proc.returncode==0
    # Global transfer budget is enforced, rather than only per connection.
    proc,port,_,_=start('bytes_per_second = 100000\ntimeout_seconds = 5\n')
    try:
        before=time.monotonic(); result=request(port,'/maps/test.bsp')
        assert result.endswith(payload) and time.monotonic()-before>1.4
    finally:
        stop(proc)
    # Active connections and connection attempts share a bounded per-IP budget.
    proc,port,_,control=start('per_ip = 1\nrequests_per_second = 1\n',control=True)
    try:
        with socket.create_connection(('127.0.0.1',port)) as slow:
            slow.sendall(b'GET /maps/')
            time.sleep(.05)
            try: result=request(port,'/maps/test.bsp')
            except (ConnectionResetError, ConnectionAbortedError): result=b''
            assert not result
    finally:
        control.close(); proc.wait(timeout=STOP_TIMEOUT)
    # Ordered trees, all three placeholders, spaces in configured paths,
    # and security policy on every fallback source.
    extra=base/'extra tree'; (extra/'maps').mkdir(parents=True)
    (extra/'maps'/'test.bsp').write_bytes(b'override')
    (extra/'maps'/'extra.bsp').write_bytes(b'extra')
    data=base/'data'; (data/'maps').mkdir(parents=True)
    (data/'maps'/'data.bsp').write_bytes(b'data')
    (data/'maps'/'secret.cfg').write_text('secret')
    if symlinks_ok:
        (data/'maps'/'link.bsp').symlink_to(outside/'secret.bsp')
    proc,port,_,_=start('source = {module}/extra tree\nsource = {game}\nsource = {data}\n')
    try:
        assert request(port,'/maps/test.bsp').endswith(b'override')
        assert request(port,'/maps/extra.bsp').endswith(b'extra')
        assert request(port,'/maps/data.bsp').endswith(b'data')
        assert request(port,'/maps/empty.bsp').startswith(b'HTTP/1.0 200 ')
        denied_extra=['/maps/secret.cfg']
        if symlinks_ok: denied_extra+=['/maps/link.bsp']
        denied_extra+=['/maps/../outside/secret.bsp']
        for path in denied_extra:
            assert request(port,path).startswith(b'HTTP/1.0 404 ')
    finally:
        stop(proc)
    # Access records include peer, request, selected source, actual body bytes,
    # HEAD (zero body), status, and timeout. A disabled log remains silent.
    proc,port,_,_=start('access_log = 2\n')
    try:
        assert request(port,'/maps/test.bsp').endswith(payload)
        request(port,'/maps/test.bsp','HEAD')
        request(port,'/maps/missing.bsp')
        request(port,'/maps/"injection.bsp')
        with socket.create_connection(('127.0.0.1',port)) as slow:
            slow.sendall(b'GET /maps/')
            time.sleep(1.3)
    finally:
        stop(proc)
    lines=log_text(proc).splitlines()
    assert any('event=connect' in line and 'ip=127.0.0.1:' in line for line in lines)
    assert any('event=request' in line and 'path="/maps/test.bsp"' in line for line in lines)
    assert any('event=complete' in line and 'method=GET' in line and f'bytes={len(payload)} ' in line and 'source=1 ' in line for line in lines)
    assert any('event=complete' in line and 'method=HEAD' in line and 'bytes=0 ' in line for line in lines)
    assert any('status=404' in line and 'path="/maps/missing.bsp"' in line for line in lines)
    assert any('event=timeout' in line for line in lines)
    assert any('path="/maps/_injection.bsp"' in line for line in lines)
    assert all('\r' not in line for line in lines)
    assert any('reason=not_found' in line and 'missing.bsp' in line for line in lines)
    # Level 1: one line per request, no connect lines; refusals carry a reason.
    proc,port,_,_=start()
    try:
        assert request(port,'/maps/test.bsp').endswith(payload)
        request(port,'/maps/secret.cfg')
        request(port,'/maps/PRIVATE.bsp')
        request(port,'/maps/te%zzst.bsp')
    finally:
        stop(proc)
    lines=log_text(proc).splitlines()
    assert not any('event=connect' in line or 'event=request ' in line for line in lines)
    assert any('event=complete' in line and 'status=200' in line for line in lines)
    assert any('reason=ext_not_allowed' in line for line in lines)
    assert any('reason=denied_prefix' in line for line in lines)
    assert any('reason=bad_encoding' in line and 'status=400' in line for line in lines)
    # Refused connections are counted into one summary line per second, and
    # refused requests beyond 20 a second are only counted.
    proc,port,_,_=start('per_ip_pending = 1\n')
    try:
        time.sleep(.2)  # the readiness probe's connection must be gone first
        with socket.create_connection(('127.0.0.1',port)) as hold:
            hold.sendall(b'GET /maps/')
            time.sleep(.1)
            for _ in range(5):
                try: assert request(port,'/maps/test.bsp')==b''
                except (ConnectionResetError, ConnectionAbortedError): pass
        for _ in range(60):
            request(port,'/maps/missing.bsp')
        time.sleep(1.2)
        request(port,'/maps/test.bsp','HEAD')
    finally:
        stop(proc)
    lines=log_text(proc).splitlines()
    summary=[line for line in lines if 'event=refused_summary' in line]
    # The five refusals may straddle a second, and so two summary lines.
    pending=sum(int(re.search(r'per_ip_pending=(\d+) ',line).group(1)) for line in summary)
    assert summary and pending==5,summary
    assert any('unlogged_refusals=' in line and 'unlogged_refusals=0 ' not in line for line in summary),summary
    assert len([line for line in lines if 'missing.bsp' in line])<=40
    # With every slot taken by unfinished requests, the oldest one makes room.
    proc,port,_,_=start('max_clients = 2\nper_ip_pending = 8\n')
    try:
        time.sleep(.2)
        holds=[socket.create_connection(('127.0.0.1',port)) for _ in range(2)]
        for h in holds: h.sendall(b'GET /maps/')
        time.sleep(.4)
        assert request(port,'/maps/test.bsp').endswith(payload)
        for h in holds: h.close()
    finally:
        stop(proc)
    assert 'event=evicted' in log_text(proc),log_text(proc)
    # Behind a trusted proxy the client is the rightmost untrusted hop of
    # X-Forwarded-For; limits apply to it. Without trust the header is ignored.
    proc,port,_,_=start('trusted_proxy = 127.0.0.0/8, 192.0.2.1\nrequests_per_second = 1\n')
    try:
        fwd=lambda value: request(port,'/maps/test.bsp','HEAD',extra=b'X-Forwarded-For: '+value+b'\r\n')
        assert fwd(b'198.51.100.7').startswith(b'HTTP/1.0 200 ')
        assert fwd(b'198.51.100.7').startswith(b'HTTP/1.0 429 ')
        assert fwd(b'203.0.113.9, 198.51.100.8, 192.0.2.1').startswith(b'HTTP/1.0 200 ')
        assert fwd(b'not-an-ip, 198.51.100.9').startswith(b'HTTP/1.0 200 ')
        assert request(port,'/maps/test.bsp','HEAD').startswith(b'HTTP/1.0 200 ')
    finally:
        stop(proc)
    lines=log_text(proc).splitlines()
    assert any('ip=198.51.100.7:' in line and 'via=127.0.0.1' in line for line in lines)
    assert any('ip=198.51.100.8:' in line for line in lines)
    assert not any('ip=203.0.113.9:' in line for line in lines)
    assert any('ip=198.51.100.9:' in line for line in lines)
    proc,port,_,_=start()
    try:
        assert request(port,'/maps/test.bsp','HEAD',extra=b'X-Forwarded-For: 198.51.100.7\r\n').startswith(b'HTTP/1.0 200 ')
    finally:
        stop(proc)
    assert 'ip=198.51.100.7' not in log_text(proc)
    # The game thread waits at most about a second for a stuck worker.
    proc,port,_,control=start(control=True)
    control.close(); proc.wait(timeout=STOP_TIMEOUT)
    assert int(re.search(r'stop_ms=(\d+)',log_text(proc)).group(1))<200
    # After it gives up, the stuck worker frees the server itself when its
    # call returns (or the owner does, if the worker finished first), and the
    # reaper joins the thread without waiting on it.
    for drop_delay in (None,'2500'):
        os.environ['FASTDL_TEST_STALL_STOP_MS']='2000'
        if drop_delay: os.environ['FASTDL_TEST_DROP_DELAY_MS']=drop_delay
        try:
            proc,port,_,control=start(control=True)
            control.close(); proc.wait(timeout=STOP_TIMEOUT+10)
        finally:
            del os.environ['FASTDL_TEST_STALL_STOP_MS']
            os.environ.pop('FASTDL_TEST_DROP_DELAY_MS',None)
        text=log_text(proc)
        assert proc.returncode==0 and 'event=stop_timeout' in text,text
        assert 900<=int(re.search(r'stop_ms=(\d+)',text).group(1))<1500,text
        # A worker that ended while the owner waited is reaped by the first call.
        detached='detached=0' if drop_delay else 'detached=1'
        assert detached in text and re.search(r'reaped_after_ms=\d+ left=0',text),text
        reaped=int(re.search(r'reaped_after_ms=(\d+)',text).group(1))
        assert (reaped<1500) if not drop_delay else (reaped<100),text
    proc,port,_,_=start('access_log = 0\n')
    request(port,'/maps/test.bsp')
    stop(proc)
    assert 'http event=' not in log_text(proc)
    proc,port,_,_=start('access_log = 2\n',hold_logs=True)
    for _ in range(100):
        request(port,'/maps/test.bsp','HEAD')
    stop(proc)
    overflow=log_text(proc).splitlines()
    assert any('event=log_overflow dropped=' in line for line in overflow)
    assert len([line for line in overflow if line.startswith('http event=')])<=257
    memory={}
    for count in [1,64,256]:
        proc,port,_,control=start(f'max_clients = {count}\n',control=True)
        control.close(); proc.wait(timeout=STOP_TIMEOUT); assert proc.returncode==0
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
    for setting in ['port = 70000\n','access_log = 3\n','per_ip_pending = 0\n',
                    'trusted_proxy = 10.0.0.0/33\n','trusted_proxy = proxy.local\n']:
        bad.write_text(f'root = {root}\nport = 1\n'+setting)
        failed=subprocess.run([binary,'--config',str(bad)],capture_output=True,timeout=5)
        assert failed.returncode!=0,setting
        if 'trusted' in setting: assert b'trusted_proxy entry' in failed.stderr,failed.stderr
    # Config quirks: a UTF-8 BOM is skipped; repeats, dotted extensions and a
    # root that sources override are reported, not silently taken.
    good=base/'quirks.cfg'
    with socket.socket() as s:
        s.bind(('127.0.0.1',0)); quirk_port=s.getsockname()[1]
    good.write_bytes(b'\xef\xbb\xbfport = 1\n'+f'port = {quirk_port}\nroot = {root}\nsource = {{game}}\nallow_extensions = .bsp wad\n'.encode())
    # Its own process group on Windows, as for every driver: stop() sends it a
    # console break, which without a group reaches every process of the console
    # (ctest and the CI runner included).
    q=subprocess.Popen([binary,'--config',str(good),'--base',str(base),'--game-dir',str(root)],stderr=subprocess.PIPE,stdout=subprocess.DEVNULL,
                       **({'creationflags':subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {}))
    try:
        deadline=time.monotonic()+5
        while True:
            try:
                with socket.create_connection(('127.0.0.1',quirk_port),timeout=.1): break
            except OSError:
                assert q.poll() is None and time.monotonic()<deadline
                time.sleep(.02)
        assert request(quirk_port,'/maps/test.bsp').endswith(payload)
    finally:
        stop(q)
    err=q.stderr.read()
    assert b"repeated setting" in err and b"'port'" in err,err
    assert b'allow_extensions are written without a dot' in err,err
    assert b'root is ignored' in err,err
print('SKIP: '+(', '.join(skipped) if skipped else 'none'))
print('PASS: content, HEAD, concurrency, traversal/symlinks, policy, decoding/query/case, 414/431, framing, timeouts, busy port + OS error text, parent EOF, bandwidth, per-IP limit, pending cap/eviction, refusal summary, trusted proxy, bounded stop, configuration')
