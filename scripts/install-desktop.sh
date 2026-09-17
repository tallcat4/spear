#!/usr/bin/env bash
# S.P.E.A.R. を KDE Plasma のスタートメニュー・タスクバー(パネルのタスクマネージャ)・デスクトップに登録する。
#   scripts/install-desktop.sh            全部(メニュー + タスクバー + デスクトップ)
#   scripts/install-desktop.sh --remove   登録を外す
# 非キオスク(通常の Plasma セッション)で ./spear.sh を毎回打たずに済ませるための一時策。キオスク化(専用セッションで自動起動)
# したらこの登録は要らなくなる。
# 仕組み: spear.desktop.in の @SPEAR_DIR@ をこのリポジトリの絶対パスで埋め、
#   メニュー   : ~/.local/share/applications/spear.desktop(+ アイコン ~/.local/share/icons/hicolor/scalable/apps/spear.svg)
#   タスクバー : ~/.config/plasma-org.kde.plasma.desktop-appletsrc の icontasks の launchers= に applications:spear.desktop を足し、plasmashell を再起動
#   デスクトップ: ~/Desktop/spear.desktop(Plasma のフォルダービュー。初回クリックで「実行を許可」を聞かれることがある)
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS="$DATA/applications"
ICONS="$DATA/icons/hicolor/scalable/apps"
DESKTOP_DIR="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")"
PLASMARC="${XDG_CONFIG_HOME:-$HOME/.config}/plasma-org.kde.plasma.desktop-appletsrc"
ENTRY="applications:spear.desktop"

taskbar_edit() {   # $1 = add | remove。icontasks / taskmanager アプレットの launchers= を書き換える
  [ -f "$PLASMARC" ] || { echo "taskbar: $PLASMARC not found (not Plasma?)"; return 0; }
  python3 - "$PLASMARC" "$1" "$ENTRY" <<'PY'
import re, sys
path, mode, entry = sys.argv[1:4]
lines = open(path, encoding="utf-8").read().split("\n")
# アプレット節 → plugin を集め、タスクマネージャの節の [Configuration][General] にある launchers= を編集する
applets = {}
section = None
for l in lines:
    m = re.match(r"^\[(.*)\]$", l)
    if m: section = m.group(1); continue
    if section and l.startswith("plugin=") and l.split("=", 1)[1] in ("org.kde.plasma.icontasks", "org.kde.plasma.taskmanager"):
        applets[section] = True
changed = False
out = []
section = None
for l in lines:
    m = re.match(r"^\[(.*)\]$", l)
    if m: section = m.group(1)
    elif section and section.endswith("][Configuration][General") and section[: -len("][Configuration][General")] in applets and l.startswith("launchers="):
        items = [x for x in l[len("launchers="):].split(",") if x]
        if mode == "add" and entry not in items: items.append(entry); changed = True
        if mode == "remove" and entry in items: items.remove(entry); changed = True
        l = "launchers=" + ",".join(items)
    out.append(l)
if changed:
    open(path, "w", encoding="utf-8").write("\n".join(out))
    print("taskbar: launchers updated (" + mode + ")")
else:
    print("taskbar: nothing to change")
sys.exit(0 if changed else 3)
PY
}

# plasmashell は終了時に自分の設定を書き戻すので、動いたまま編集すると上書きされて消える。止めてから編集し、起動し直す
taskbar_update() {   # $1 = add | remove
  [ -f "$PLASMARC" ] || { echo "taskbar: $PLASMARC not found (not Plasma?)"; return 0; }
  # 既に望む状態なら plasmashell を止めない(冪等)
  if [ "$1" = add ] && grep -q "^launchers=.*$ENTRY" "$PLASMARC"; then echo "taskbar: already pinned"; return 0; fi
  if [ "$1" = remove ] && ! grep -q "^launchers=.*$ENTRY" "$PLASMARC"; then echo "taskbar: not pinned"; return 0; fi
  local running=0
  if systemctl --user is-active --quiet plasma-plasmashell.service 2>/dev/null; then
    running=1
    systemctl --user stop plasma-plasmashell.service
    for _ in 1 2 3 4 5 6 7 8 9 10; do systemctl --user is-active --quiet plasma-plasmashell.service || break; sleep 0.5; done
    sleep 1   # 設定の書き戻しを待つ
  fi
  echo "taskbar before: $(grep -m1 '^launchers=' "$PLASMARC")"
  taskbar_edit "$1" || true
  echo "taskbar after:  $(grep -m1 '^launchers=' "$PLASMARC")"
  if [ "$running" = 1 ]; then
    systemctl --user start plasma-plasmashell.service && echo "plasmashell restarted"
  else
    echo "plasmashell: start it (or log out / in) to see the taskbar change"
  fi
}

if [ "${1:-}" = "--remove" ]; then
  rm -f "$APPS/spear.desktop" "$ICONS/spear.svg" "$DESKTOP_DIR/spear.desktop"
  taskbar_update remove
  command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
  echo "removed"
  exit 0
fi

# メニュー + アイコン
mkdir -p "$APPS" "$ICONS"
sed "s|@SPEAR_DIR@|$HERE|g" "$HERE/spear.desktop.in" > "$APPS/spear.desktop"
chmod +x "$APPS/spear.desktop"
cp "$HERE/spear.svg" "$ICONS/spear.svg"
command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
echo "menu: $APPS/spear.desktop -> $HERE/spear.sh"
# デスクトップ
if [ -d "$DESKTOP_DIR" ]; then
  cp "$APPS/spear.desktop" "$DESKTOP_DIR/spear.desktop"
  chmod +x "$DESKTOP_DIR/spear.desktop"
  echo "desktop: $DESKTOP_DIR/spear.desktop"
fi
# タスクバー
taskbar_update add
