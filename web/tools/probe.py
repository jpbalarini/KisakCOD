#!/usr/bin/env python3
"""probe.py -- ask a game server on the relay for its status, like the in-game server browser does. A quick "is the dedicated server up?" check.

    python3 web/tools/probe.py                          getstatus from 10.77.0.1 via the relay on 127.0.0.1:8081
    python3 web/tools/probe.py getinfo                  the short info a server browser lists
    python3 web/tools/probe.py --relay 192.168.1.50:8081 --tls   a relay started with --tls-cert/--tls-key

Exit status: 0 = the server answered, 1 = no answer (not running, relay down, wrong address).
It joins the relay as an ordinary client for a moment (it takes a virtual address, then leaves); it does not need a browser.
"""
import argparse, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from relay import SimpleClient

OOB = b"\xff\xff\xff\xff"


def parse_info(text):
    parts = text.split("\\")[1:]
    return dict(zip(parts[0::2], parts[1::2]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("what", nargs="?", default="getstatus", choices=("getstatus", "getinfo"))
    ap.add_argument("--relay", default="127.0.0.1:8081", help="host:port of the relay (default 127.0.0.1:8081)")
    ap.add_argument("--server", default="10.77.0.1", help="virtual address of the game server (default 10.77.0.1)")
    ap.add_argument("--tls", action="store_true", help="the relay speaks wss:// (serve.py --tls-cert): connect with TLS, certificate not verified")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--raw", action="store_true", help="print the raw response")
    args = ap.parse_args()
    host, _, port = args.relay.rpartition(":")
    try:
        c = SimpleClient(host or "127.0.0.1", int(port), tls=args.tls)
    except OSError as e:
        print(f"cannot reach the relay at {args.relay}: {e}", file=sys.stderr)
        return 1
    if c.status != 101:
        print(f"the relay at {args.relay} refused the connection (HTTP {c.status})", file=sys.stderr)
        return 1
    try:
        t0 = time.time()
        c.send_datagram(args.server, 28960, OOB + (b"getinfo probe\n" if args.what == "getinfo" else b"getstatus\n"))
        deadline = t0 + args.timeout
        while time.time() < deadline:
            try:
                ip, _, data = c.recv_datagram(max(0.1, deadline - time.time()))
            except OSError:
                break
            if ip != args.server or not data.startswith(OOB):
                continue
            ms = (time.time() - t0) * 1000
            body = data[4:].decode("latin1")
            if args.raw:
                print(body)
                return 0
            head, _, rest = body.partition("\n")
            if not head.startswith(("statusResponse", "infoResponse")):
                continue
            lines = rest.split("\n")
            info = parse_info(lines[0])
            print(f"{args.server} answered {head} in {ms:.0f} ms")
            for k in ("sv_hostname", "mapname", "g_gametype", "sv_maxclients", "clients", "sv_privateClients", "shortversion", "protocol", "hw", "pure", "mod"):
                if k in info:
                    print(f"  {k:18} {info[k]}")
            players = [l for l in lines[1:] if l.strip()]
            if head.startswith("statusResponse"):
                print(f"  players: {len(players)}")
                for l in players:
                    print("   ", l)
            return 0
        print(f"no answer from {args.server} within {args.timeout:.0f} s", file=sys.stderr)
        return 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
