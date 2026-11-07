#!/usr/bin/env python3
"""package_server.py -- bundle everything a standalone machine needs to run the web server + relay + headless dedicated game server.

    python3 web/tools/package_server.py                  writes web/build/kisak-server.tar.gz
    python3 web/tools/package_server.py --out /tmp/x.tgz
    python3 web/tools/package_server.py --no-traces      leave out the learned load traces (web/build/traces)

Build both targets first:   python3 web/tools/build.py --link app && python3 web/tools/build.py --link dedicated

On the other machine (a Mac or a Linux box with python3 and node 22+ -- no emscripten, no compiler):

    tar xzf kisak-server.tar.gz && cd kisak-server
    ./run-server.sh --data "/path/to/Call of Duty 4 Data"        (see README.txt in the bundle)

The bundle holds only our code and the wasm builds. It does NOT contain any game data: copy your own CoD4 install to that machine
(the game data is never shipped, hosted or published by this project).
"""
import argparse, io, os, sys, tarfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.normpath(os.path.join(HERE, ".."))
ROOT = os.path.normpath(os.path.join(WEB, ".."))
TOP = "kisak-server"

RUN_SERVER = """#!/bin/bash
# Starts the web server (the game page + your game data), the relay and the headless dedicated game server, all from this one command.
#   ./run-server.sh --data "/path/to/Call of Duty 4 Data"            (or set KISAK_INSTALL_PATH instead of --data)
# Everything after the script name goes to web/tools/serve.py (see its --help): --port, --tls-cert/--tls-key, --dedicated-args "+set ...", ...
# --lan (listen on all interfaces so other computers can join) is on by default here; PORT=8080 is the web port, 8081 the relay.
HERE="$(cd "$(dirname "$0")" && pwd)"
command -v node >/dev/null 2>&1 || { echo "node (22 or newer) is needed for the dedicated server: https://nodejs.org/" >&2; exit 1; }
exec python3 "$HERE/web/tools/serve.py" --port "${PORT:-8080}" --root "$HERE/web/build/app" --dedicated --lan "$@"
"""

README = """KisakCOD web server bundle (web server + relay + headless dedicated game server)
===============================================================================================

What is in here
  web/tools/serve.py        the web server (game page + your game data, read-only) that also starts the other two parts
  web/tools/relay.py        the relay: a virtual LAN for the browsers (they cannot send UDP). The game server is always 10.77.0.1
  web/tools/probe.py        asks the game server for its status ("is it up?")
  web/build/app/            the browser build of the game
  web/build/dedicated/      the same engine as a headless server (runs under node, no browser, no GPU)
  web/dedicated/server.cfg  the server's settings: name, slots, game type, map rotation
  web/bots/                 Bot Warfare (AI players): scripts and waypoints. None are added by themselves: type  addbot  in the console
  web/build/traces/         learned load orders that make the first map load faster for players (optional)

Needs: python3 (3.9+), node 22 or newer, and a copy of your own Call of Duty 4 Modern Warfare game data (this bundle has none;
the data directory is only ever read, the server writes nothing into it). The game data is for your own use: do not put this
server on the public internet.

Run it
  ./run-server.sh --data "/path/to/Call of Duty 4 Data"
Players open  http://<this machine>:8080/  in Chrome and use Join Game; the server is listed as "KisakCoD4 web server"
(or type  connect 10.77.0.1  in the console).

Browsers only run the game on a secure origin: https, or http://localhost. For other computers on the network give the server a
certificate they trust (for example one made with mkcert) :
  ./run-server.sh --data "..." --tls-cert cert.pem --tls-key key.pem
or, in the player's Chrome, add this server's address under chrome://flags/#unsafely-treat-insecure-origin-as-secure.

Run the game server by itself (a stand-alone relay somewhere else): see the top of web/tools/serve.py, or
  KISAK_INSTALL_PATH="..." node --stack-size=7000 web/build/dedicated/dedicated.js --relay="ws://host:8081/?role=server" --cfg=web/dedicated/server.cfg

Operating it
  The terminal that runs run-server.sh is the server console: type  status , map mp_backlot , map_rotate , kick <name> , say <text> , quit .
  Bots: the server starts with none.  addbot [count]  adds that many (they stay across the map rotation),  kickbots  removes them all;
  see web/bots/README.md for the settings (skill level, keeping the server filled up to N players, ...).
  Stopping serve.py (Ctrl+C, kill, closing the terminal) stops the game server too. If the game server crashes it is restarted
  automatically (2 s, then 4, 8, ... up to 30 s); after  quit  it stays down. The full server log is web/build/dedicated/server.log.
  Check it from anywhere:  python3 web/tools/probe.py --relay <this machine>:8081 [--tls]
  Settings: edit web/dedicated/server.cfg, or  --dedicated-args "+set sv_hostname Foo +set g_gametype dm"  (command line wins).

License: the engine is KisakCOD, GPL-3.0 (see LICENSE); the source of this build is the repository it was built from.
"""


def add_file(tar, src, arc, mode=None):
    ti = tar.gettarinfo(src, arcname=f"{TOP}/{arc}")
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    ti.mode = mode if mode is not None else (0o755 if ti.mode & 0o111 else 0o644)
    with open(src, "rb") as f:
        tar.addfile(ti, f)
    return ti.size


def add_text(tar, text, arc, mode=0o644):
    data = text.encode()
    ti = tarfile.TarInfo(f"{TOP}/{arc}")
    ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
    tar.addfile(ti, io.BytesIO(data))
    return len(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(WEB, "build", "kisak-server.tar.gz"))
    ap.add_argument("--no-traces", action="store_true", help="leave out web/build/traces")
    args = ap.parse_args()

    files = []   # (source path, path inside the bundle)
    def need(rel):
        p = os.path.join(WEB, rel)
        if not os.path.isfile(p):
            sys.exit(f"missing {p}\nbuild first:  python3 web/tools/build.py --link app && python3 web/tools/build.py --link dedicated")
        files.append((p, "web/" + rel))
    for t in ("serve.py", "relay.py", "probe.py"):
        need("tools/" + t)
    app = os.path.join(WEB, "build", "app")
    for name in ("index.html", "index.js", "index.wasm", "io-worker.js", "audio-worklet.js"):
        need("build/app/" + name)
    if os.path.isfile(os.path.join(app, "hero.webp")):
        need("build/app/hero.webp")
    need("build/dedicated/dedicated.js")
    need("build/dedicated/dedicated.wasm")
    need("dedicated/server.cfg")
    for dp, dns, fns in os.walk(os.path.join(WEB, "bots")):   # Bot Warfare scripts + waypoints (serve.py --dedicated-bots)
        dns.sort()
        for n in sorted(fns):
            if n != ".DS_Store":
                full = os.path.join(dp, n)
                files.append((full, "web/bots/" + os.path.relpath(full, os.path.join(WEB, "bots")).replace(os.sep, "/")))
    if not args.no_traces:
        td = os.path.join(WEB, "build", "traces")
        if os.path.isdir(td):
            for n in sorted(os.listdir(td)):
                if n.endswith(".json"):
                    files.append((os.path.join(td, n), "web/build/traces/" + n))
    if os.path.isfile(os.path.join(ROOT, "LICENSE")):
        files.append((os.path.join(ROOT, "LICENSE"), "LICENSE"))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    total = 0
    with tarfile.open(args.out, "w:gz") as tar:
        for src, arc in files:
            total += add_file(tar, src, arc)
        total += add_text(tar, RUN_SERVER, "run-server.sh", 0o755)
        total += add_text(tar, README, "README.txt")
    print(f"{args.out}: {len(files) + 2} files, {total / 1e6:.1f} MB uncompressed, {os.path.getsize(args.out) / 1e6:.1f} MB packed")


if __name__ == "__main__":
    main()
