#!/usr/bin/env python3
"""Dev server for the KisakCOD web port.

  python3 web/tools/serve.py [--port 8080] [--root web/build/m1] [--data "<.../Call of Duty 4 Data>"]

* serves the build output directory with the COOP/COEP headers SharedArrayBuffer (wasm threads) needs;
* serves the user's *local* game data under /data/<path> with HTTP Range support (the engine reads the
  multi-GB fastfiles through sync XHR range requests from its worker thread);
* /data-manifest.json lists every file in the data dir ({"files": {"main/xx.iwd": size, ...}}).

The game data is never copied or uploaded anywhere: it is only served on localhost.
"""
import argparse, json, mimetypes, os, re, sys, threading
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from urllib.parse import unquote, urlparse

mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/javascript", ".mjs")

ARGS = None
_manifest = None
_manifest_lock = threading.Lock()


def build_manifest(data_root):
    files = {}
    for dirpath, dirnames, filenames in os.walk(data_root):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, data_root).replace(os.sep, "/")
            if fn.startswith(".") or fn.endswith(".DS_Store"):
                continue
            try:
                files[rel] = os.path.getsize(full)
            except OSError:
                pass
    return {"files": files}


def get_manifest():
    global _manifest
    with _manifest_lock:
        if _manifest is None:
            _manifest = json.dumps(build_manifest(ARGS.data)).encode()
        return _manifest


class Handler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

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
        return super().do_GET()

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


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--root", default=os.path.join(os.path.dirname(__file__), "..", "build", "m1"))
    ap.add_argument("--data", default=os.environ.get("KISAK_INSTALL_PATH", ""))
    ap.add_argument("--verbose", action="store_true")
    ARGS = ap.parse_args()
    ARGS.root = os.path.abspath(ARGS.root)
    if ARGS.data:
        ARGS.data = os.path.abspath(ARGS.data)
    print(f"[serve] root={ARGS.root}\n[serve] data={ARGS.data or '(none)'}\n[serve] http://localhost:{ARGS.port}/", flush=True)
    ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
