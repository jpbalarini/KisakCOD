#!/bin/bash
# Serve the browser build + your local game data at http://localhost:8080  (Chrome recommended: WebGL2 + SharedArrayBuffer)
#   build first:  python3 web/tools/build.py --link app
# The game data is served only on localhost and never copied anywhere, unless you pass --lan (other devices on your network: see tools/serve.py).
#   web/run.sh [--lan] [--tls-cert F --tls-key F] [--verbose]    extra arguments go to serve.py
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${KISAK_DATA:-/Users/jpb/Development/Projects/COD4Remake/Call of Duty 4 Modern Warfare[MAC-ENG]/Call of Duty 4/Call of Duty 4 Data}"
exec python3 "$HERE/tools/serve.py" --port "${PORT:-8080}" --root "$HERE/build/app" --data "$DATA" "$@"
