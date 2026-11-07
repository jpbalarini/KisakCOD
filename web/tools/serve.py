#!/usr/bin/env python3
"""Dev server for the KisakCOD web port.

  python3 web/tools/serve.py [--port 8080] [--root web/build/app] [--data "<.../Call of Duty 4 Data>"]

* serves the build output directory with the COOP/COEP headers SharedArrayBuffer (wasm threads) needs;
* serves the user's *local* game data under /data/<path> with HTTP Range support (the engine reads the
  multi-GB fastfiles through sync XHR range requests from its worker thread);
* /data-manifest.json lists every file in the data dir ({"files": {"main/xx.iwd": size, ...}, "stamps": {"main/xx.iwd": mtime, ...}});
* keeps "load traces" (which byte ranges of the data each load phase reads) under GET/POST /traces/<phase>.json (--traces DIR, default
  <root>/../traces), so a cold client can fetch them in parallel ahead of the engine (web/app/io-worker.js);
* also starts the multiplayer relay (relay.py, a WebSocket virtual-LAN switch) on --relay-port (default 8081; 0 = off).

The game data is never copied or uploaded anywhere. By default it is only served on localhost; --lan opens the HTTP server and the relay to
the local network (see below) so other computers and phones can play.

  --lan                          listen on all interfaces; the relay also accepts pages served from private-network addresses
  --tls-cert FILE --tls-key FILE serve https:// and wss:// (e.g. a certificate made with `mkcert 192.168.1.100 my-mac.local`)
  --allow-origin ORIGIN          let one more page origin/host use the relay (repeatable)

  --dedicated                    also run the headless dedicated game server (the engine under node, web/build/dedicated, no browser tab
                                 needed): web server + relay + game server from one command. Players join it at 10.77.0.1 or from the
                                 in-game server browser. Needs `node` (22+) and the build: python3 web/tools/build.py --link dedicated.
                                 Type engine commands here (status, map mp_crash, kick ...): they go to the server's console.
  --dedicated-cfg FILE           its config (default web/dedicated/server.cfg); --dedicated-args "+set sv_hostname X +map_rotate"

Browsers only enable SharedArrayBuffer (wasm threads) and AudioWorklet in a *secure context*: https://, or http://localhost. A plain
http://192.168.x.y:8080 page therefore cannot run the game. Either use --tls-cert/--tls-key with a certificate the client trusts, or, in
Chrome on the client, open chrome://flags/#unsafely-treat-insecure-origin-as-secure and add the http://192.168.x.y:8080 origin.
"""
import argparse, atexit, json, mimetypes, os, re, shlex, shutil, signal, socket, ssl, subprocess, sys, threading, time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import unquote, urlparse

mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/javascript", ".mjs")

ARGS = None
_manifest = None
_manifest_lock = threading.Lock()


def build_manifest(data_root):
    files, stamps = {}, {}
    for dirpath, dirnames, filenames in os.walk(data_root):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, data_root).replace(os.sep, "/")
            if fn.startswith(".") or fn.endswith(".DS_Store"):
                continue
            try:
                st = os.stat(full)
                files[rel] = st.st_size
                stamps[rel] = int(st.st_mtime)
            except OSError:
                pass
    return {"files": files, "stamps": stamps}


def get_manifest():
    global _manifest
    with _manifest_lock:
        if _manifest is None:
            _manifest = json.dumps(build_manifest(ARGS.data)).encode()
        return _manifest


class Server(ThreadingHTTPServer):
    request_queue_size = 128      # the default backlog (5) overflows when a browser opens several connections at once: dropped SYNs retry after 1 s
    daemon_threads = True


TRACE_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
TRACE_MAX_BODY = 8 << 20
TRACE_MAX_BYTES = int(1.5e9)
_trace_lock = threading.Lock()


def trace_file(name):
    return os.path.join(ARGS.traces, name + ".json")


def validate_trace(raw):
    """Check an uploaded load trace ({"v":1,"files":[path..],"ext":[[file index,start,end]..]}) against the data manifest.
    Returns normalised JSON bytes, or None. Anything that does not fit a real data file is dropped, so a bogus upload can at worst make a
    client prefetch (at most TRACE_MAX_BYTES of) real game data."""
    try:
        tr = json.loads(raw)
        paths, ext = tr["files"], tr["ext"]
        sizes = json.loads(get_manifest())["files"]
        if not isinstance(paths, list) or not isinstance(ext, list) or len(ext) > 50000:
            return None
        out, total = [], 0
        for fi, s, e in ext:
            if not (isinstance(fi, int) and isinstance(s, int) and isinstance(e, int)) or not 0 <= fi < len(paths):
                continue
            size = sizes.get(paths[fi])
            if size is None or not 0 <= s < e <= size:
                continue
            total += e - s
            if total > TRACE_MAX_BYTES:
                return None
            out.append([fi, s, e])
        if not out:
            return None
        return json.dumps({"v": 1, "files": paths, "ext": out}, separators=(",", ":")).encode()
    except (ValueError, KeyError, TypeError):
        return None


BOOT_PHASES = ("startup", "code_post_gfx_mp", "localized_code_post_gfx_mp", "common_mp", "localized_common_mp", "ui_mp")
START_FACTOR = 1.25   # a first start downloads more than the traces list: merged gaps, read-ahead past the recorded reads
                      # (measured: 527 MB downloaded vs 420 MB of traced reads for the menus + mp_crash)


def read_trace(name):
    try:
        with open(trace_file(name), "rb") as f:
            return json.loads(f.read())
    except (OSError, ValueError):
        return None


def traced_bytes(traces):
    """Bytes covered by the union of the extents of these load traces (a range read by several phases counts once)."""
    per = {}
    for tr in traces:
        for fi, s, e in tr["ext"]:
            per.setdefault(tr["files"][fi], []).append((s, e))
    total = 0
    for spans in per.values():
        spans.sort()
        cs, ce = spans[0]
        for s, e in spans[1:]:
            if s <= ce:
                ce = max(ce, e)
            else:
                total += ce - cs
                cs, ce = s, e
        total += ce - cs
    return total


def start_info():
    """What the start screen shows: the size of all game data on the server, and an estimate of what a first start downloads (the menus
    plus the largest recorded map), or None while the server has not recorded those load traces yet."""
    sizes = json.loads(get_manifest())["files"]
    info = {"dataBytes": sum(sizes.values()), "firstStartBytes": None, "extraMapBytes": None}
    boot = [read_trace(n) for n in BOOT_PHASES]
    maps = []
    try:
        names = [n[:-5] for n in os.listdir(ARGS.traces) if n.endswith(".json")]
    except OSError:
        names = []
    for n in names:
        if n.startswith("mp_") and not n.endswith("_load"):
            tr = read_trace(n)
            if tr:
                maps.append(tr)
    if maps and all(boot):
        base = traced_bytes(boot)
        biggest = max(traced_bytes(boot + [m]) for m in maps)
        info["firstStartBytes"] = int(biggest * START_FACTOR)
        info["extraMapBytes"] = int((biggest - base) * START_FACTOR)      # what one more map adds on top of the menus
    return info


class Handler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    disable_nagle_algorithm = True    # TCP_NODELAY: with keep-alive, Nagle holds back the last partial segment of every response until the
                                      # client's delayed ACK (40-200 ms) -- per request, on a real network (not on loopback)

    def log_message(self, fmt, *args):
        if ARGS.verbose or not (self.path.startswith("/data/")):
            sys.stderr.write("[serve] " + (fmt % args) + "\n")

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        path = unquote(urlparse(path).path)
        rel = os.path.normpath(path.lstrip("/"))
        return os.path.join(ARGS.root, rel)

    def do_GET(self):
        p = unquote(urlparse(self.path).path)
        if p == "/data-manifest.json":
            body = get_manifest()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if p.startswith("/data/"):
            return self.serve_data(p[len("/data/"):])
        if p == "/start-info.json":
            body = json.dumps(start_info()).encode() if ARGS.data else b"{}"
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if p.startswith("/traces/"):
            name = p[len("/traces/"):]
            name = name[:-5] if name.endswith(".json") else name
            if not TRACE_NAME.match(name) or not os.path.isfile(trace_file(name)):
                self.send_error(404)
                return
            with open(trace_file(name), "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return super().do_GET()

    def do_POST(self):
        """POST /traces/<phase>.json: a client that loaded a phase the server has no load trace for uploads what it read (first writer wins;
        ?replace=1 overwrites). See web/app/io-worker.js."""
        u = urlparse(self.path)
        p = unquote(u.path)
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if 0 < n <= TRACE_MAX_BODY else b""
        if n > TRACE_MAX_BODY:
            self.close_connection = True      # the body was not read: this connection cannot be reused
        if not p.startswith("/traces/") or not raw:
            self.send_error(400 if raw or n == 0 else 413)
            return
        name = p[len("/traces/"):]
        name = name[:-5] if name.endswith(".json") else name
        body = validate_trace(raw) if TRACE_NAME.match(name) else None
        if body is None:
            self.send_error(400)
            return
        with _trace_lock:
            if os.path.exists(trace_file(name)) and "replace=1" not in u.query:
                status = 409
            else:
                os.makedirs(ARGS.traces, exist_ok=True)
                tmp = trace_file(name) + ".tmp"
                with open(tmp, "wb") as f:
                    f.write(body)
                os.replace(tmp, trace_file(name))
                status = 201
                sys.stderr.write(f"[serve] load trace '{name}' saved ({len(body) // 1024} KB)\n")
        self.send_response(status)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_HEAD(self):
        p = unquote(urlparse(self.path).path)
        if p.startswith("/data/"):
            return self.serve_data(p[len("/data/"):], head=True)
        return super().do_HEAD()

    def serve_data(self, rel, head=False):
        rel = os.path.normpath(rel)
        if rel.startswith("..") or os.path.isabs(rel):
            self.send_error(403)
            return
        full = os.path.join(ARGS.data, rel)
        if not os.path.isfile(full):
            self.send_error(404)
            return
        size = os.path.getsize(full)
        start, end = 0, size - 1
        status = 200
        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng)
            if m:
                if m.group(1) == "" and m.group(2) != "":
                    start = max(0, size - int(m.group(2)))
                else:
                    start = int(m.group(1) or 0)
                    if m.group(2) != "":
                        end = min(int(m.group(2)), size - 1)
                status = 206
        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        length = end - start + 1
        if ARGS.sim_rtt_ms:
            time.sleep(ARGS.sim_rtt_ms / 1000.0)
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if head:
            return
        with open(full, "rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                left -= len(chunk)
                if ARGS.sim_mbps:
                    time.sleep(len(chunk) * 8 / (ARGS.sim_mbps * 1e6))


def lan_addresses():
    """This machine's IPv4 addresses on the local network (best effort; the UDP connect sends nothing)."""
    found = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            found.append(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            if info[4][0] not in found and not info[4][0].startswith("127."):
                found.append(info[4][0])
    except OSError:
        pass
    return found


# ---- the headless dedicated game server -------------------------------------------------------------------------------------------------
class DedicatedServer:
    """Runs web/build/dedicated/dedicated.js under node as a child process: restarts it if it crashes, forwards this process's stdin to its
    console, and prints its output with a [dedi] prefix (the full output also goes to <build>/server.log)."""

    # Lines that are noise on a server console (they stay in server.log): a headless engine never registers the per-channel shock volume dvars,
    # so every map load warns once per channel and shock file; the rest is start-up chatter and the per-join network statistics dumps (a joining
    # player's first snapshot prints a page of "fields changed" lines, the handshake one line per stats packet).
    QUIET = re.compile(r"unknown dvar 'bg_shock_volume_|^Adding channel|iwd \(\d+ files\)|^\s+localized assets iwd|^\^6|^Precached weapon:|^[a-z0-9_]+_mp: WEAPON_|"
                       r"^execing |^\s{4,}\w+ \".*\"$|^Trying to load file|^Loading fastfile|"
                       r"fields changed: \(format|^(begin|end)-{20,}|^\d+\] \d+ byte snapshot|^Client .*'s snapshot$|"
                       r"^(Entity deltas|New entities|Removed entities|Temp entity|Client deltas|New clients|Removed clients|Playerstate deltas|"
                       r"Playerstate nodelta|Server commands) - \d+ bits|^Received packet \d+ of stats data|^Gamestate has |^\s+(Largest|Average) config string")

    def __init__(self, node, script, data, relay_url, cfg, engine_args, restart=True, verbose=False):
        self.node, self.script, self.data, self.relay_url = node, script, data, relay_url
        self.cfg, self.engine_args, self.restart, self.verbose = cfg, engine_args, restart, verbose
        self.proc = None
        self.stopping = False
        self.log = None

    def start(self):
        out_dir = os.path.dirname(self.script)
        self.log = open(os.path.join(out_dir, "server.log"), "ab", buffering=0)
        threading.Thread(target=self._supervise, name="dedicated", daemon=True).start()
        threading.Thread(target=self._forward_stdin, name="dedicated-stdin", daemon=True).start()
        atexit.register(self.stop)

    def _spawn(self):
        home = os.path.join(os.path.dirname(self.script), "home")   # the child's working directory: stray relative-path writes land here, never in the game data
        os.makedirs(home, exist_ok=True)
        # KISAK_EXIT_ON_STDIN_EOF: the server stops when this process dies (its stdin pipe closes), so it is never left running by itself
        env = dict(os.environ, KISAK_INSTALL_PATH=self.data, KISAK_EXIT_ON_STDIN_EOF="1")
        cmd = [self.node, "--stack-size=7000", self.script, f"--relay={self.relay_url}"]
        if self.cfg:
            cmd.append(f"--cfg={self.cfg}")
        cmd += self.engine_args
        self.proc = subprocess.Popen(cmd, cwd=home, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        print(f"[serve] dedicated server started (pid {self.proc.pid}); its full log: {self.log.name}", flush=True)

    def _pump(self, proc):
        up = False
        for raw in iter(proc.stdout.readline, b""):
            self.log.write(raw)
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not up and "local address" in line and line.startswith("Network:"):
                up = True
                m = re.search(r"local address ([0-9.]+)", line)
                print(f"[serve] dedicated server is on the relay: players join it at {m.group(1) if m else '10.77.0.1'}, or from the in-game server browser", flush=True)
            if self.verbose or not self.QUIET.search(line):
                print(f"[dedi] {line}", flush=True)

    def _supervise(self):
        backoff = 2
        while not self.stopping:
            t0 = time.time()
            try:
                self._spawn()
            except OSError as e:
                print(f"[serve] cannot start the dedicated server: {e}", flush=True)
                return
            self._pump(self.proc)
            rc = self.proc.wait()
            if self.stopping:
                return
            lived = time.time() - t0
            if rc == 0:
                print("[serve] the dedicated server quit (exit code 0); it is not restarted. Restart this program to run it again.", flush=True)
                return
            print(f"[serve] the dedicated server died (exit code {rc}) after {lived:.0f} s", flush=True)
            if not self.restart:
                return
            backoff = 2 if lived > 60 else min(backoff * 2, 30)
            print(f"[serve] restarting it in {backoff} s", flush=True)
            for _ in range(backoff * 10):
                if self.stopping:
                    return
                time.sleep(0.1)

    def _forward_stdin(self):
        try:
            for line in sys.stdin:
                p = self.proc
                if p and p.poll() is None:
                    try:
                        p.stdin.write(line.encode())
                        p.stdin.flush()
                    except OSError:
                        pass
        except Exception:
            pass

    def stop(self):
        if self.stopping:
            return
        self.stopping = True
        p = self.proc
        if p and p.poll() is None:
            try:
                p.stdin.write(b"quit\n")
                p.stdin.flush()
            except OSError:
                pass
            try:
                p.wait(4)
            except subprocess.TimeoutExpired:
                p.terminate()
                try:
                    p.wait(3)
                except subprocess.TimeoutExpired:
                    p.kill()


def node_major(node):
    try:
        return int(subprocess.check_output([node, "--version"], text=True).strip().lstrip("v").split(".")[0])
    except (OSError, ValueError, subprocess.CalledProcessError):
        return 0


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--root", default=os.path.join(os.path.dirname(__file__), "..", "build", "app"))
    ap.add_argument("--data", default=os.environ.get("KISAK_INSTALL_PATH", ""))
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--relay-port", type=int, default=8081, help="multiplayer relay port (0 disables it)")
    ap.add_argument("--net-stats", action="store_true", help="relay: ping every player once a second and print its network round trip every 5 s (diagnoses ping)")
    ap.add_argument("--lan", action="store_true", help="listen on all interfaces so other devices on the network can connect")
    ap.add_argument("--tls-cert", help="serve https/wss with this certificate (PEM)")
    ap.add_argument("--tls-key", help="private key for --tls-cert (PEM)")
    ap.add_argument("--allow-origin", action="append", default=[], help="extra page origin/host allowed to use the relay")
    ap.add_argument("--sim-rtt-ms", type=float, default=0, help="simulated network latency added to every game-data request")
    ap.add_argument("--sim-mbps", type=float, default=0, help="simulated bandwidth of each game-data connection, in Mbit/s")
    ap.add_argument("--traces", default="", help="where learned load traces are kept (default: <root>/../traces)")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--dedicated", action="store_true", help="also run the headless dedicated game server (see the top of this file)")
    ap.add_argument("--dedicated-build", default=os.path.join(here, "..", "build", "dedicated"), help="directory with dedicated.js (python3 web/tools/build.py --link dedicated)")
    ap.add_argument("--dedicated-cfg", default=os.path.join(here, "..", "dedicated", "server.cfg"), help="server config for --dedicated (empty = none)")
    ap.add_argument("--dedicated-args", default="", help='extra engine arguments for the dedicated server, e.g. "+set sv_hostname MyServer +map_rotate"')
    ap.add_argument("--dedicated-verbose", action="store_true", help="show the dedicated server's start-up chatter on the console too (server.log has everything)")
    ap.add_argument("--no-dedicated-restart", action="store_true", help="do not restart the dedicated server when it crashes")
    ap.add_argument("--node", default="node", help="the node executable for --dedicated (22 or newer)")
    ARGS = ap.parse_args()
    if bool(ARGS.tls_cert) != bool(ARGS.tls_key):
        ap.error("--tls-cert and --tls-key go together")
    ARGS.root = os.path.abspath(ARGS.root)
    if not ARGS.traces:
        ARGS.traces = os.path.join(os.path.dirname(ARGS.root), "traces")
    ARGS.traces = os.path.abspath(ARGS.traces)
    if ARGS.data:
        ARGS.data = os.path.abspath(ARGS.data)
    dedicated = None
    if ARGS.dedicated:
        script = os.path.abspath(os.path.join(ARGS.dedicated_build, "dedicated.js"))
        node = shutil.which(ARGS.node)
        problems = []
        if not ARGS.data or not os.path.isdir(ARGS.data):
            problems.append("--dedicated needs the game data: --data \"<.../Call of Duty 4 Data>\" (or KISAK_INSTALL_PATH)")
        if not ARGS.relay_port:
            problems.append("--dedicated needs the relay (--relay-port is 0)")
        if not os.path.isfile(script):
            problems.append(f"{script} does not exist: build it with  python3 web/tools/build.py --link dedicated")
        if not node:
            problems.append(f"node ('{ARGS.node}') not found: install node 22 or newer (the dedicated server runs on it)")
        elif node_major(node) < 22:
            problems.append(f"node is too old ({node_major(node)}): the dedicated server needs 22 or newer (WebSocket built in)")
        cfg = os.path.abspath(ARGS.dedicated_cfg) if ARGS.dedicated_cfg else ""
        if cfg and not os.path.isfile(cfg):
            problems.append(f"server config {cfg} does not exist (--dedicated-cfg '' runs without one)")
        if problems:
            ap.error("\n  ".join(problems))
    scheme = "https" if ARGS.tls_cert else "http"
    bind = "0.0.0.0" if ARGS.lan else "127.0.0.1"
    print(f"[serve] root={ARGS.root}\n[serve] data={ARGS.data or '(none)'}\n[serve] {scheme}://localhost:{ARGS.port}/", flush=True)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import relay
    ssl_ctx = relay.make_ssl_context(ARGS.tls_cert, ARGS.tls_key) if ARGS.tls_cert else None
    if ARGS.relay_port:
        rt = relay.start_in_thread(bind, ARGS.relay_port, ARGS.verbose, ARGS.allow_origin, ARGS.lan, ssl_ctx, ARGS.net_stats,
                                   server_ip="10.77.0.1" if ARGS.dedicated else None)
        if ARGS.dedicated:
            if not getattr(rt, "local_port", None):
                sys.exit("[serve] the relay did not start (see above), so there is nothing for the dedicated server to connect to")
            dedicated = DedicatedServer(node, script, ARGS.data, f"ws://127.0.0.1:{rt.local_port}/?role=server", cfg, shlex.split(ARGS.dedicated_args),
                                        restart=not ARGS.no_dedicated_restart, verbose=ARGS.dedicated_verbose)
    httpd = Server((bind, ARGS.port), Handler)
    if ssl_ctx:
        # handshake lazily in the per-connection handler thread so one stalled client cannot block accept()
        httpd.socket = ssl_ctx.wrap_socket(httpd.socket, server_side=True, do_handshake_on_connect=False)
    if ARGS.lan:
        ips = lan_addresses()
        print("[serve] --lan: reachable from the local network (the game data is served to anyone who can reach this port):", flush=True)
        for ip in ips:
            print(f"[serve]   {scheme}://{ip}:{ARGS.port}/", flush=True)
        if not ARGS.tls_cert:
            print("[serve] these http:// pages are not a secure context, so the game will not start in them as is. On each client either use\n"
                  "[serve] --tls-cert/--tls-key, or in Chrome open chrome://flags/#unsafely-treat-insecure-origin-as-secure, add\n"
                  f"[serve] {', '.join(f'{scheme}://{ip}:{ARGS.port}' for ip in ips) or 'the http://<this-ip>:' + str(ARGS.port) + ' origin'} and relaunch Chrome.", flush=True)
    if dedicated:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))   # run the cleanup (stop the game server) when this program is stopped
        dedicated.start()
    try:
        httpd.serve_forever()
    except (KeyboardInterrupt, SystemExit):   # Ctrl+C, or SIGTERM (the handler above)
        pass
    finally:
        if dedicated:
            dedicated.stop()
    # Leave without finalizing the interpreter: the relay and output-pump threads are daemons that may still be printing, and Python 3.13+
    # aborts ("could not acquire lock for <stdout> at interpreter shutdown") when it finalizes under them.
    for f in (sys.stdout, sys.stderr):
        try:
            f.flush()
        except Exception:
            pass
    os._exit(0)


if __name__ == "__main__":
    main()
