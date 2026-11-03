#!/usr/bin/env bash
set -euo pipefail

# Windows-first entrypoint. Native Linux builds use bsnes/GNUmakefile directly.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/build_windows.sh" "$@"
