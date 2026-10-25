# raspad-fastdl

Linux GoldSrc FastDL for Authum (GameDLL host, no Metamod). The 32-bit module
contains an HTTP server running on an Authum-managed thread inside HLDS. No helper
executable or child process is needed. No compression or TLS. GET/HEAD, one request per connection.

## Configuration

No absolute paths or duplicate resource trees are required:

```ini
bind = 0.0.0.0
port = 17015

source = {game}
# Additional optional trees, checked in order:
# source = {data}/public
# source = {game}/custom/resources

allow_dirs = maps models sound sprites gfx overviews
allow_extensions = bsp mdl wav mp3 spr tga bmp txt res wad
```

Repeat `source` for up to 16 resource trees. A request for `/maps/example.bsp`
looks for `maps/example.bsp` inside each tree; the first available allowed file
wins. Every configured source must exist and be readable, or startup fails.
Comment out optional sources until their directories exist.

Placeholders: `{game}` is the actual game directory supplied by Authum,
`{module}` is this module directory, `{data}` is its data directory. Plain
relative paths start from `{module}`. Paths with spaces work without quotes.
Unknown placeholders fail startup. If source is omitted, `{game}` is used.
The legacy `root` key is accepted only as the fallback when no source entries
are given; empty root also means `{game}`. Source entries take precedence.

`allow_dirs` controls the first URL directory and `allow_extensions` controls
file extensions. `deny_prefixes` excludes full paths or directory subtrees.
These rules apply to every source. Root-level files are WAD-only. No directory
listings, uploads, executable extensions by default, hidden components, URL
escapes or symlink traversal. Resource names must use ASCII letters, digits,
underscore, hyphen, dots, dollar signs and slashes. Names and extensions are case-sensitive.
Publication is trusted administration: do not expose private data through hard
links or bind mounts, and keep the allowed trees restricted to game resources.

## Layout and installation

```text
addons/authum/modules/fastdl/
  fastdl.so
  configs/fastdl.cfg
  licenses/...
```

Add `fastdl` to Authum's module list. Configure the public URL in the game:

```text
sv_allowdownload 1
sv_downloadurl "http://YOUR_PUBLIC_IP:17015/"
```

The shipped config binds loopback only. Change bind to a local server IPv4
address or `0.0.0.0` before external use. The public URL can differ because
of NAT. Open the TCP port in the firewall/provider. A busy port fails startup;
no automatic port substitution occurs. Maintain map `.res` manifests as
required by the game. Reload the module after configuration changes.

When upgrading older versions, stop/unload the old module first, replace
fastdl.so, and remove obsolete fastdl-http files from the module root/data.
Keep your local config. Sources, placeholders and limits are unchanged.

FastDL 0.4 requires the shared HTTP/clock API in Authum 4.2 (ABI 1.2). Update
`addons/authum/authum.so` together with fastdl.so while HLDS is stopped. Older
modules remain ABI-compatible; this FastDL fails cleanly on older hosts.

## Build and package

```sh
git submodule update --init --recursive
# Debian/Ubuntu: gcc-multilib libc6-dev-i386 cmake ninja-build python3
cmake -S . -B build-linux32 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux32
ctest --test-dir build-linux32 --output-on-failure
cmake --install build-linux32 --component fastdl --prefix "$PWD/dist/linux32"
# Also build the updated host (needed by FastDL 0.3):
cmake -S lib/authum -B build-authum -G Ninja -DCMAKE_BUILD_TYPE=Release -DAUTHUM_BUILD_EXAMPLES=OFF -DAUTHUM_BUILD_API_DOCS=OFF
cmake --build build-authum --target authum
python3 scripts/package.py
```

Authum is a Git submodule; its own pinned submodules include picohttpparser. Upstream license notices
are included in the install. fastdl.so is i386, including the HTTP code. The test driver is not installed
or needed at runtime. `-DFASTDL_BUILD_MODULE=OFF` builds the HTTP core and
its tests for the native architecture with a test-only Authum thread host.

## Operations and verification

FastDL contains configuration, source/placeholder adaptation and log routing only.
HTTP implementation and picohttpparser live once in Authum, behind its API tables.
The HTTP worker never calls engine or module callbacks. Configuration, source
handles and the listening socket are prepared before start returns; port/source
errors therefore fail module startup immediately. Source paths are opened using
directory handles: the module never changes the process working directory.

Up to 256 clients; buffers are allocated for the configured max_clients only
(default 64: about 0.84 MiB of HTTP state, excluding thread stack, kernel buffers
and allocator overhead, versus about 2.51 MiB at 256). 8 KiB request buffers, 64 headers, per-IP connection
and attempt budgets, header/idle timeouts, five-minute maximum lifetime, file
size cap, and global response-byte bandwidth budget with up to 100 ms burst.
Access logging is enabled by default (access_log=1). sendfile is used; cold disk I/O can delay the
worker. Range requests receive a complete representation.

Authum signals the worker through the registered wake callback and joins it
before calling module stop. FastDL then frees its state
or unloading code. Clients, sources, pipe and listener are closed. No forced
thread cancellation or unsafe unload timeout is used. Shutdown normally wakes
immediately, but a slow/stalled filesystem operation can delay join. The worker
shares HLDS memory: an HTTP memory bug can crash the game. No child-process
sandbox or process-crash isolation is provided.

cpu=-1 inherits affinity; a configured CPU index applies only to the HTTP
thread, preserving game-thread affinity. Authum blocks SIGPIPE in the worker for
sendfile; process signal handlers, cwd, resource limits and unrelated descriptors
are left intact. No HTTP work runs in Authum's task worker. Disk/network remain
shared with the game, and provider filtering is required for link-saturating
DDoS. Compare actual game frame latency against Nginx before replacing it.

Integration tests cover binary contents, HEAD, concurrency, ordered sources,
all placeholders, security policy on fallback trees, traversal/symlinks,
malformed requests, timeouts, occupied port, shutdown wakeup, bandwidth, per-IP
limits, real i386 module loading, 20 start/stop cycles with active clients, descriptor/
thread cleanup, and preservation of host cwd, signals, CPU affinity and limits. Sanitizers:

```sh
cmake -S . -B build-sanitize -G Ninja -DFASTDL_BUILD_MODULE=OFF -DFASTDL_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

These are local checks, not a security audit or live HLDS/Steam-client test.

## Access log and a quick check

Access records go through Authum at info level into
`addons/authum/modules/fastdl/logs/YYYY-MM-DD.log`. Set
`fastdl.log_level = info` in Authum's config if a stricter filter is configured.
Console visibility is controlled separately by Authum's `log_console` setting.
Existing FastDL configs also default to access_log=1; set access_log=0 to disable.

Events: connect, request, complete, disconnect, timeout, shutdown, reject_limit.
Every connection has an id, peer IP/port, method/path, response status, actual
body bytes handed to the socket, source index (1-based, matching startup logs),
and duration in milliseconds. HEAD transfers zero body bytes. A complete
record means the server finished writing the HTTP response, not proof that a
client stored the file. A status of zero means no HTTP response was prepared.
The HTTP client provides an IP address, not a trustworthy SteamID or player name.

A fixed 256-record single-producer/single-consumer queue separates the worker
from Authum's logger. Normal flushing is limited to 8 lines/frame and 100
lines/second. Excess records are dropped with a log_overflow summary so a flood
cannot grow memory without bounds. Unload joins the worker and flushes up to
257 final lines. Request strings are sanitized and truncated to prevent log
injection. During a map load without frames, log flushing waits for frames or
unload. No disk/host logging occurs in the HTTP worker.

Use an existing resource (replace the map name) to verify a full response:

```sh
curl --fail -o /dev/null http://127.0.0.1:17015/maps/YOUR_MAP.bsp
tail -f addons/authum/modules/fastdl/logs/$(date +%F).log
```

A request for a missing/disallowed resource returns 404; this still confirms
HTTP is answering, but does not confirm the source/path configuration is correct.
For remote reachability, repeat the request from another machine using the
public address. A game client with all files already cached will not download
those files and therefore will not produce HTTP requests for them.

## Readable components and API reuse

The module config.c contains a declarative setting schema, sources.c expands
placeholders, and http.c builds Authum HTTP options. The Authum bridge handles
module lifecycle and game-thread log flushing. There is no socket, HTTP parser
or OS clock implementation in FastDL.

The reusable constructor and component map are documented in
[Authum HTTP API](lib/authum/docs/http.md). Application state uses Authum types;
libc/OS boundary types remain native where required. Formatting follows
.clang-format (Allman braces, no single-line control flow, 100-column limit).
CI checks formatting with clang-format 18.1.8 and scripts/check_format.py.

## Asynchronous Authum disk logging

Authum 4.2.1 copies accepted file records into a fixed 1024-slot queue.
One Authum-managed writer handles file creation, daily rotation, retention,
writing, flushing and closing. Files flush every 100ms while the disk responds;
errors flush when the worker handles them. Accepted records drain on normal
shutdown. A crash can lose queued records and buffered output.

The producer never waits for the disk or queue space. When full, the newest
file record is dropped, and the writer reports the number lost in the host
log. Memory is bounded: about 1.7MiB for records and at most 32 open buffered
files. No disk logging implementation is linked into fastdl.so.

Formatting, channel configuration and console mirroring still run on the game
thread. Engine console callbacks cannot run on the file writer. Worker-side
logging continues to use the existing capture/delivery mechanism; the channel
registry is not a concurrent API. Journal routing and log format are unchanged.

See [Authum logging internals](lib/authum/docs/logging.md) for lifecycle and checks.
