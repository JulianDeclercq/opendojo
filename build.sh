#!/bin/sh
set -e
# ponytail: pause so a double-clicked window stays open; skipped when not a tty (CI/agent).
[ -t 0 ] && trap 'echo; echo "Press Enter to close..."; read _' EXIT
cd "$(dirname "$0")/dll"
cmake -B build -A x64 -DOPENDOJO_DEPLOY_DIR="C:/Program Files (x86)/Steam/steamapps/common/TEKKEN 8/Polaris/Binaries/Win64/plugins"
cmake --build build --config Release
