#!/usr/bin/env bash
# Refresh Ohm overlay colors after `omarchy theme set`.
# Both the source-tree wrapper and the packaged binary accept --theme.
set -euo pipefail
if command -v ohm >/dev/null; then
  ohm --theme
fi
