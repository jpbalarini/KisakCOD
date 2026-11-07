#!/usr/bin/env python3
"""relay.py -- WebSocket "virtual LAN" switch for the KisakCOD web port (M4).

Browsers cannot send UDP, so the web build tunnels every engine datagram through one WebSocket frame (see web/platform/net_web.cpp).
This relay gives each connected tab a virtual IPv4 address (10.77.x.y, port 28960) and forwards datagrams between tabs by address:

  client -> relay : binary [dst ip: 4][dst port: 2, big endian][payload]     dst 255.255.255.255 = every other client (LAN broadcast)
                                                                           dst 127.0.0.1       = back to the sender
  relay -> client : binary [src ip: 4][src port: 2, big endian][payload]     src = the sender's virtual address
  relay -> client : text   {"ip": "10.77.0.2", "port": 28960}               sent once after the handshake

The relay never looks inside the payload: it is not a game server, only a switch. One tab hosts (`devmap mp_crash`), the others
`connect <its address>`. A dedicated server (web/build/dedicated, started by `serve.py --dedicated`) connects to ws://host:8081/?role=server
instead: with --server-ip (default 10.77.0.1 under serve.py --dedicated) that address is reserved for it, so players can always
`connect 10.77.0.1` and the server keeps its address across restarts. Only a connection from this machine can take it (--server-any-host
lifts that), and tabs are never given it.

    python3 web/tools/relay.py [--host 127.0.0.1] [--port 8081] [--verbose]       run standalone
    python3 web/tools/relay.py --selftest                                           check routing with two raw clients
(web/tools/serve.py starts it automatically next to the HTTP server.)

By default only pages served from localhost may connect (the Origin header is checked), so a random web page you visit cannot reach it.
With --lan the relay listens on all interfaces and also accepts pages served from a private-network address (192.168.x.y, 10.x.y.z, a
`*.local` name, ...), so phones and other computers on the same network can join. Origins are matched by IP literal / reserved name, never by
resolving a public hostname, so a web page on the internet still cannot reach it (DNS rebinding included). --allow-origin adds more.
Standard library only (RFC 6455 subset: text/binary/continuation/ping/pong/close, masked client frames).
"""
import argparse, asyncio, base64, hashlib, ipaddress, json, os, ssl, struct, sys, threading, time
from urllib.parse import urlparse, parse_qs

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
GAME_PORT = 28960
MAX_FRAME = 32 * 1024            # a netchan fragment is ~1.3 KB; anything near this is not engine traffic
MAX_PPS = 4000                   # per-client packets/second before the relay starts dropping
LOCAL_ORIGINS = {"localhost", "127.0.0.1", "[::1]", "::1"}


def is_lan_host(host):
    """True for names that can only mean "this machine / this network": localhost, *.local (mDNS) and private/loopback/link-local IP literals."""
    host = (host or "").lower().rstrip(".")
    if host == "localhost" or host.endswith(".localhost") or host.endswith(".local"):
        return True
    try:
        ip = ipaddress.ip_address(host.strip("[]"))
    except ValueError:
        return False
    return ip.is_loopback or ip.is_private or ip.is_link_local


def make_ssl_context(cert, key):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    return ctx


class Client:
    def __init__(self, ip, writer, peer):
        self.ip = ip                      # 4 bytes
        self.writer = writer
        self.peer = peer
        self.window = int(time.time())
        self.count = 0
        self.rx = self.tx = self.dropped = 0
        self.rtts = []                    # WebSocket ping->pong round trips (ms) since the last --stats report
        self.mark = (0, 0)                # (rx, tx) at the last report

    @property
    def name(self):
        return ".".join(map(str, self.ip))


class Relay:
    def __init__(self, verbose=False, allow_origins=None, lan=False, stats=False, server_ip=None, server_any_host=False):
        self.clients = {}                 # ip bytes -> Client
        self.server_ip = bytes(map(int, server_ip.split("."))) if isinstance(server_ip, str) else server_ip   # reserved for role=server
        self.server_any_host = server_any_host
        self.verbose = verbose
        self.allow_origins = set(allow_origins or [])
        self.lan = lan                    # also accept pages served from private-network addresses
        self.stats = stats                # --stats: ping every client once a second and print the round trips every 5 s

    async def _pinger(self, client):
        """WebSocket ping every second. The browser answers it without the engine's help, but only while the page's thread is free: a pong
        time = Wi-Fi + TCP (a lost packet stalls the stream until it is retransmitted) + this relay + how long the tab's event loop was busy
        (a blocked engine frame / a hidden tab's slow loop show up here too). Seconds-long pongs = the page was frozen (loading a map)."""
        try:
            while True:
                client.writer.write(self.frame(9, struct.pack(">d", time.monotonic())))
                await asyncio.sleep(1.0)
        except Exception:
            pass

    async def _reporter(self):
        while True:
            await asyncio.sleep(5.0)
            for c in list(self.clients.values()):
                rx, tx = c.rx - c.mark[0], c.tx - c.mark[1]
                c.mark = (c.rx, c.tx)
                r, c.rtts = c.rtts, []
                rtt = (f"rtt avg {sum(r) / len(r):5.1f} ms  max {max(r):5.1f} ms  min {min(r):5.1f} ms  ({len(r)} pings)" if r
                       else "rtt: no pong (page frozen? connection stalled?)")
                print(f"[relay] {c.name} ({c.peer[0]}): {rtt}   from client {rx / 5:.0f}/s, to client {tx / 5:.0f}/s", flush=True)

    def _alloc_ip(self):
        for n in range(1, 65535):
            ip = bytes([10, 77, n >> 8, n & 255])
            if ip not in self.clients and ip != self.server_ip:
                return ip
        return None

    def _origin_ok(self, origin):
        if not origin:
            return True                   # non-browser client (tests, curl): there is no ambient browser authority to protect
        host = urlparse(origin).hostname or ""
        if host in LOCAL_ORIGINS or origin in self.allow_origins or host in self.allow_origins:
            return True
        return self.lan and is_lan_host(host)

    # ---- websocket framing -------------------------------------------------------------------------------------------------------
    @staticmethod
    def frame(opcode, payload):
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126: head += bytes([n])
        elif n < 65536: head += bytes([126]) + struct.pack(">H", n)
        else: head += bytes([127]) + struct.pack(">Q", n)
        return head + payload

    @staticmethod
    async def read_frame(reader):
        h = await reader.readexactly(2)
        fin, opcode = h[0] & 0x80, h[0] & 0x0F
        masked, n = h[1] & 0x80, h[1] & 0x7F
        if n == 126: n = struct.unpack(">H", await reader.readexactly(2))[0]
        elif n == 127: n = struct.unpack(">Q", await reader.readexactly(8))[0]
        if n > MAX_FRAME:
            raise ValueError("frame too large")
        mask = await reader.readexactly(4) if masked else None
        data = await reader.readexactly(n) if n else b""
        if mask:
            # XOR with the repeating 4-byte mask; int arithmetic keeps this fast enough for ~1 KB frames
            m = int.from_bytes((mask * (n // 4 + 1))[:n], "big")
            data = (int.from_bytes(data, "big") ^ m).to_bytes(n, "big") if n else b""
        return bool(fin), opcode, data

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername")
        client = None
        pinger = None
        try:
            request = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
            lines = request.decode("latin1").split("\r\n")
            headers = {}
            for ln in lines[1:]:
                if ":" in ln:
                    k, v = ln.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            if headers.get("upgrade", "").lower() != "websocket" or "sec-websocket-key" not in headers:
                writer.write(b"HTTP/1.1 426 Upgrade Required\r\nContent-Length: 0\r\n\r\n")
                return
            if not self._origin_ok(headers.get("origin", "")):
                writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
                print(f"[relay] refused origin {headers.get('origin')}", flush=True)
                return
            accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + GUID).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
            ip = None
            parts = lines[0].split(" ")
            role = parse_qs(urlparse(parts[1] if len(parts) > 1 else "/").query).get("role", [""])[0]
            if role == "server" and self.server_ip is not None:
                host = (peer[0] if peer else "").replace("::ffff:", "")
                if self.server_any_host or host in ("127.0.0.1", "::1"):
                    old = self.clients.pop(self.server_ip, None)      # a previous instance (restarted server): this connection takes over
                    if old is not None:
                        try:
                            old.writer.close()
                        except Exception:
                            pass
                    ip = self.server_ip
                else:
                    print(f"[relay] {peer[0]} asked for the server address but is not on this machine: treated as a normal client", flush=True)
            if ip is None:
                ip = self._alloc_ip()
            if ip is None:
                writer.write(self.frame(8, struct.pack(">H", 1013)))
                return
            client = Client(ip, writer, peer)
            self.clients[ip] = client
            print(f"[relay] {client.name} connected from {peer[0]}:{peer[1]} ({len(self.clients)} online)", flush=True)
            writer.write(self.frame(1, json.dumps({"ip": client.name, "port": GAME_PORT}).encode()))
            await writer.drain()
            if self.stats:
                pinger = asyncio.ensure_future(self._pinger(client))

            message = b""
            while True:
                fin, opcode, data = await self.read_frame(reader)
                if opcode == 8:
                    writer.write(self.frame(8, data[:2]))
                    return
                if opcode == 9:
                    writer.write(self.frame(10, data))
                    continue
                if opcode == 10:
                    if len(data) == 8:
                        client.rtts.append((time.monotonic() - struct.unpack(">d", data)[0]) * 1000.0)
                    continue
                if opcode in (1, 2, 0):
                    message += data
                    if len(message) > MAX_FRAME:
                        return
                    if fin:
                        if opcode != 1 and message:
                            self.route(client, message)
                        message = b""
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, ValueError):
            pass
        finally:
            if pinger is not None:
                pinger.cancel()
            if client is not None:
                if self.clients.get(client.ip) is client:             # (a replaced server's old connection must not free the new one's address)
                    self.clients.pop(client.ip, None)
                print(f"[relay] {client.name} disconnected (rx {client.rx} tx {client.tx} dropped {client.dropped}, {len(self.clients)} online)", flush=True)
            try:
                writer.close()
            except Exception:
                pass

    def route(self, src, msg):
        if len(msg) < 6:
            return
        now = int(time.time())
        if now != src.window:
            src.window, src.count = now, 0
        src.count += 1
        if src.count > MAX_PPS:
            src.dropped += 1
            return
        dst, payload = msg[:4], msg[6:]
        out = self.frame(2, src.ip + struct.pack(">H", GAME_PORT) + payload)
        if dst == b"\xff\xff\xff\xff":
            targets = [c for c in self.clients.values() if c is not src]
        elif dst == b"\x7f\x00\x00\x01":
            targets = [src]
        else:
            c = self.clients.get(dst)
            targets = [c] if c else []
        src.rx += 1
        for c in targets:
            try:
                c.writer.write(out)
                c.tx += 1
            except Exception:
                pass
        if self.verbose:
            print(f"[relay] {src.name} -> {'.'.join(map(str, dst))} {len(payload)}B {payload[:24]!r}", flush=True)

    async def serve(self, host, port, ssl_ctx=None):
        server = await asyncio.start_server(self.handle, host, port, ssl=ssl_ctx)
        print(f"[relay] virtual LAN switch on {'wss' if ssl_ctx else 'ws'}://{host}:{port}/ (addresses 10.77.x.y:{GAME_PORT})", flush=True)
        if self.stats:
            asyncio.ensure_future(self._reporter())
        async with server:
            await server.serve_forever()


def start_in_thread(host="127.0.0.1", port=8081, verbose=False, allow_origins=None, lan=False, ssl_ctx=None, stats=False, server_ip=None, server_any_host=False):
    """Run the relay on its own event loop in a daemon thread (used by serve.py). The returned thread has `local_port`: a port on 127.0.0.1 for
    processes on this machine (the dedicated server). It is `port` itself, unless the relay serves TLS: then it is an extra plain listener on
    the same switch, so a local process does not have to trust the certificate."""
    ready = threading.Event()
    holder = {}

    def run():
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        relay = Relay(verbose, allow_origins, lan, stats, server_ip, server_any_host)

        async def main():
            try:
                server = await asyncio.start_server(relay.handle, host, port, ssl=ssl_ctx)
            except OSError as e:
                print(f"[relay] cannot listen on {host}:{port}: {e}", flush=True)
                ready.set()
                return
            print(f"[relay] virtual LAN switch on {'wss' if ssl_ctx else 'ws'}://{host}:{port}/ (addresses 10.77.x.y:{GAME_PORT})", flush=True)
            holder["local_port"] = port
            if ssl_ctx:
                local = await asyncio.start_server(relay.handle, "127.0.0.1", 0)
                holder["local_port"] = local.sockets[0].getsockname()[1]
            if stats:
                asyncio.ensure_future(relay._reporter())
            ready.set()
            async with server:
                await server.serve_forever()
        loop.run_until_complete(main())

    t = threading.Thread(target=run, name="relay", daemon=True)
    t.start()
    ready.wait(5)
    t.local_port = holder.get("local_port")
    return t


# ---- minimal blocking client, for the self test and web/tools/relay_probe.py --------------------------------------------------------
class SimpleClient:
    def __init__(self, host, port, origin=None, path="/", tls=False):
        import socket
        self.s = socket.create_connection((host, port), timeout=5)
        if tls:   # wss:// (a self-signed certificate is the normal case on a LAN: the certificate is not verified)
            import ssl
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            self.s = ctx.wrap_socket(self.s, server_hostname=host)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               "Sec-WebSocket-Version: 13\r\n" + (f"Origin: {origin}\r\n" if origin else "") + "\r\n")
        self.s.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.s.recv(4096)
            if not chunk:
                break
            resp += chunk
        head, _, self.buf = resp.partition(b"\r\n\r\n")
        self.status = int(head.split(b" ")[1]) if head else 0
        self.hello = None
        if self.status == 101:
            op, data = self.recv_frame()
            self.hello = json.loads(data)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv_frame(self):
        self._need(2)
        op, n = self.buf[0] & 0x0F, self.buf[1] & 0x7F
        off = 2
        if n == 126:
            self._need(4); n = struct.unpack(">H", self.buf[2:4])[0]; off = 4
        elif n == 127:
            self._need(10); n = struct.unpack(">Q", self.buf[2:10])[0]; off = 10
        self._need(off + n)
        data = self.buf[off:off + n]
        self.buf = self.buf[off + n:]
        return op, data

    def send_datagram(self, ip, port, payload):
        body = bytes(map(int, ip.split("."))) + struct.pack(">H", port) + payload
        mask = os.urandom(4)
        n = len(body)
        head = bytes([0x82]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(body)))

    def recv_datagram(self, timeout=3.0):
        self.s.settimeout(timeout)
        while True:
            op, data = self.recv_frame()
            if op == 2:
                return ".".join(map(str, data[:4])), struct.unpack(">H", data[4:6])[0], data[6:]

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def selftest():
    port = 18081
    start_in_thread("127.0.0.1", port)
    a, b, c = SimpleClient("127.0.0.1", port), SimpleClient("127.0.0.1", port), SimpleClient("127.0.0.1", port)
    assert [a.hello["ip"], b.hello["ip"], c.hello["ip"]] == ["10.77.0.1", "10.77.0.2", "10.77.0.3"], (a.hello, b.hello, c.hello)
    a.send_datagram(b.hello["ip"], 28960, b"\xff\xff\xff\xffgetinfo x")
    assert b.recv_datagram() == ("10.77.0.1", 28960, b"\xff\xff\xff\xffgetinfo x")
    b.send_datagram("10.77.0.1", 28960, b"reply")
    assert a.recv_datagram() == ("10.77.0.2", 28960, b"reply")
    a.send_datagram("255.255.255.255", 28960, b"bcast")                       # broadcast reaches everyone else, not the sender
    assert b.recv_datagram()[2] == b"bcast" and c.recv_datagram()[2] == b"bcast"
    try:
        a.recv_datagram(0.3); raise SystemExit("sender received its own broadcast")
    except OSError:
        pass
    a.send_datagram("127.0.0.1", 28960, b"self")                              # loopback goes back to the sender
    assert a.recv_datagram() == ("10.77.0.1", 28960, b"self")
    a.send_datagram("10.77.9.9", 28960, b"nobody")                            # unknown destination: silently dropped, like UDP
    try:
        a.recv_datagram(0.3); raise SystemExit("unexpected delivery")
    except OSError:
        pass
    b.close(); b2 = SimpleClient("127.0.0.1", port)
    assert b2.hello["ip"] == "10.77.0.2"                                      # freed addresses are reused
    bad = SimpleClient("127.0.0.1", port, origin="https://evil.example")
    assert bad.status == 403, bad.status                                      # foreign web pages are refused
    strict, lan = Relay(), Relay(lan=True)                                    # origin policy: localhost only, or + private networks (--lan)
    for origin, want_strict, want_lan in [("http://localhost:8080", 1, 1), ("http://127.0.0.1:8080", 1, 1), ("https://[::1]:8080", 1, 1),
                                          ("http://192.168.1.100:8080", 0, 1), ("https://10.1.2.3", 0, 1), ("http://172.20.0.5:8080", 0, 1),
                                          ("http://my-mac.local:8080", 0, 1), ("http://169.254.7.7", 0, 1),
                                          ("https://evil.example", 0, 0), ("http://8.8.8.8", 0, 0), ("http://192.168.1.100.evil.example", 0, 0),
                                          ("http://172.32.0.1", 0, 0), ("http://notlocal", 0, 0)]:
        assert strict._origin_ok(origin) == bool(want_strict) and lan._origin_ok(origin) == bool(want_lan), origin
    # reserved address for a dedicated server (role=server from this machine): tabs never get it, a restarted server takes it back
    port2 = 18082
    start_in_thread("127.0.0.1", port2, server_ip="10.77.0.1")
    t1 = SimpleClient("127.0.0.1", port2)
    assert t1.hello["ip"] == "10.77.0.2", t1.hello                            # .1 is kept free for the server
    s1 = SimpleClient("127.0.0.1", port2, path="/?role=server")
    assert s1.hello["ip"] == "10.77.0.1", s1.hello
    t1.send_datagram("10.77.0.1", 28960, b"hi")
    assert s1.recv_datagram() == ("10.77.0.2", 28960, b"hi")
    s2 = SimpleClient("127.0.0.1", port2, path="/?role=server")               # the server restarted: the new connection takes the address over
    assert s2.hello["ip"] == "10.77.0.1", s2.hello
    s1.close(); time.sleep(0.3)                                               # the old connection's cleanup must not free the new one's address
    t1.send_datagram("10.77.0.1", 28960, b"again")
    assert s2.recv_datagram() == ("10.77.0.2", 28960, b"again")
    for cl in (t1, s1, s2):
        cl.close()
    for cl in (a, b2, c, bad):                                                # let the relay log the disconnects before the interpreter exits
        cl.close()
    time.sleep(0.3)
    print("relay selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", default=None, help="interface to listen on (default 127.0.0.1, or 0.0.0.0 with --lan)")
    ap.add_argument("--port", type=int, default=8081)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--stats", action="store_true", help="ping every client once a second and print each one's transport round trip + packet rates every 5 s")
    ap.add_argument("--lan", action="store_true", help="listen on all interfaces and accept pages served from private-network addresses")
    ap.add_argument("--allow-origin", action="append", default=[], help="additional page origin/host allowed to connect")
    ap.add_argument("--tls-cert", help="serve wss:// with this certificate (needed when the pages are served over https)")
    ap.add_argument("--tls-key", help="private key for --tls-cert")
    ap.add_argument("--server-ip", help="virtual address reserved for a dedicated server connecting with ?role=server (e.g. 10.77.0.1)")
    ap.add_argument("--server-any-host", action="store_true", help="let a dedicated server on another machine take --server-ip (default: only this machine)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    ssl_ctx = make_ssl_context(args.tls_cert, args.tls_key) if args.tls_cert else None
    try:
        asyncio.run(Relay(args.verbose, args.allow_origin, args.lan, args.stats, args.server_ip, args.server_any_host).serve(args.host or ("0.0.0.0" if args.lan else "127.0.0.1"), args.port, ssl_ctx))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
