#!/bin/bash
# Serve the M1 browser build + your local game data at http://localhost:8080  (Chrome recommended: WebGL2 + SharedArrayBuffer)
#   build first:  python3 web/tools/build.py --link m1
# The game data is served only on localhost and never copied anywhere.
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${KISAK_DATA:-/Users/jpb/Development/Projects/COD4Remake/Call of Duty 4 Modern Warfare[MAC-ENG]/Call of Duty 4/Call of Duty 4 Data}"
exec python3 "$HERE/tools/serve.py" --port "${PORT:-8080}" --root "$HERE/build/m1" --data "$DATA"
