#!/usr/bin/env bash
# spear.desktop.in の @SPEAR_DIR@ をこのリポジトリの絶対パスで埋めて ~/.local/share/applications/ に置く(タップで起動するため)。
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$DEST"
sed "s|@SPEAR_DIR@|$HERE|g" "$HERE/spear.desktop.in" > "$DEST/spear.desktop"
chmod +x "$DEST/spear.desktop"
echo "installed $DEST/spear.desktop -> $HERE/spear.sh"
