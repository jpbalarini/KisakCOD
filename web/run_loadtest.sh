#!/bin/zsh
# Run the node load test (loads the fastfiles with the unmodified engine loader and reports the assets) against a CoD4 install dir (default: the Mac retail data next to this repo).
DATA="${KISAK_INSTALL_PATH:-/Users/jpb/Development/Projects/COD4Remake/Call of Duty 4 Modern Warfare[MAC-ENG]/Call of Duty 4/Call of Duty 4 Data}"
HERE="${0:A:h}"
cd "$DATA" && KISAK_INSTALL_PATH="$DATA" exec node --stack-size=8000 "$HERE/build/loadtest/loadtest.js" "$@"
