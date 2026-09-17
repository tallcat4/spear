#!/usr/bin/env bash
# S.P.E.A.R. 実機起動スクリプト(タップで起動できるように、環境と既定値をここに集める)
#   ./spear.sh                    B210 で全画面起動
#   ./spear.sh --source synthetic 合成データ
#   ./spear.sh --windowed         ウィンドウ表示(開発用)
# 追加引数はそのまま spear-gui へ渡す。ログ・録音・イベントログは ~/spear/ 配下。個体設定は ~/spear/site.conf。
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/build/gui/spear-gui"
DATA="${SPEAR_DATA:-$HOME/spear}"
mkdir -p "$DATA/recordings" "$DATA/logs"

if [ ! -x "$BIN" ]; then
  echo "spear-gui not built: $BIN  (cmake -S . -B build -G Ninja && ninja -C build)" >&2
  exit 1
fi

# Plasma Wayland セッションから起動する場合の既定(端末外から起動されても動くように)
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -S "$XDG_RUNTIME_DIR/wayland-0" ]; then export WAYLAND_DISPLAY=wayland-0; fi
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
export QT_FORCE_STDERR_LOGGING=1

MODE="--fullscreen"
ARGS=()
for a in "$@"; do
  case "$a" in
    --windowed) MODE="" ;;
    *) ARGS+=("$a") ;;
  esac
done

# 個体・現場固有の設定: $DATA/site.conf の key=value 行を --set で渡す(例: std_t98.freq_err_hz=<個体の LO 誤差 Hz>)
if [ -f "$DATA/site.conf" ]; then
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    ARGS+=(--set "$line")
  done < "$DATA/site.conf"
fi

LOG="$DATA/logs/spear-$(date -u +%Y%m%d_%H%M%S).log"
cd "$HERE"
exec "$BIN" $MODE "${ARGS[@]}" \
  --record-dir "$DATA/recordings" \
  --event-log "$DATA/logs/events.jsonl" \
  --rate 4e6 --freq 100e6 --gain 30 \
  2>&1 | tee "$LOG"
