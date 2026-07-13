#!/usr/bin/env bash
# See check.py for why this gate exists and why its allowlist is built to SHRINK.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$HERE/check.py"
