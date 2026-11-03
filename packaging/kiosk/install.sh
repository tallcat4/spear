#!/usr/bin/env bash
# S.P.E.A.R. キオスクセッションと OS の調整(要件 §11 の段階 2、§11.1)を入れる / 外す。root で実行する。
#   sudo packaging/kiosk/install.sh            入れる
#   sudo packaging/kiosk/install.sh --remove   外す(Plasma だけの状態に戻る)
# 入れるもの:
#   /usr/local/bin/spear-session-select   次の自動ログイン先(spear / plasma)を選び、--restart で SDDM を再起動する(root)
#   /usr/local/bin/spear-session          キオスクセッション本体(cage + このリポジトリの spear.sh)
#   /usr/share/wayland-sessions/spear.desktop
#   /etc/sudoers.d/spear-session          利用者に spear-session-select だけをパスワードなしで許す
#   /etc/udev/rules.d/70-spear-b2xx.rules B2xx の USB autosuspend を切る
#   /etc/security/limits.d/spear.conf     rtprio 99 / memlock unlimited(UHD の RX thread を SCHED_FIFO に。再ログインで効く)
# CPU の性能は入れない: App の実行中だけ spear(Core の PerformanceHold)が power-profiles-daemon に要求する。
# デスクトップのアイコン(タップでキオスクへ)は利用者の権限で scripts/install-desktop.sh が置く。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0 ${1:-}"; exit 1; }
USER_NAME="${SUDO_USER:-}"
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || { echo "run with sudo from the user who runs S.P.E.A.R."; exit 1; }

FILES=(/usr/local/bin/spear-session-select /usr/local/bin/spear-session /usr/share/wayland-sessions/spear.desktop
       /etc/sudoers.d/spear-session /etc/udev/rules.d/70-spear-b2xx.rules /etc/security/limits.d/spear.conf
       /etc/sddm.conf.d/zz-spear-autologin.conf)

if [ "${1:-}" = --remove ]; then
  rm -f "${FILES[@]}"
  udevadm control --reload-rules
  echo "removed. Next login is Plasma (autologin.conf unchanged). limits apply until the next login."
  exit 0
fi

command -v cage >/dev/null || { echo "cage is not installed: sudo pacman -S cage"; exit 1; }

install -m 0755 -o root -g root "$HERE/spear-session-select" /usr/local/bin/spear-session-select
sed "s|@SPEAR_DIR@|$REPO|g" "$HERE/spear-session.in" > /usr/local/bin/spear-session
chown root:root /usr/local/bin/spear-session
chmod 0755 /usr/local/bin/spear-session
install -D -m 0644 "$HERE/spear-session.desktop" /usr/share/wayland-sessions/spear.desktop

sudoers=$(mktemp)
printf '# S.P.E.A.R. kiosk (packaging/kiosk): session switch only\n%s ALL=(root) NOPASSWD: /usr/local/bin/spear-session-select\n' "$USER_NAME" > "$sudoers"
visudo -cf "$sudoers" >/dev/null
install -m 0440 -o root -g root "$sudoers" /etc/sudoers.d/spear-session
rm -f "$sudoers"

install -m 0644 "$HERE/70-spear-b2xx.rules" /etc/udev/rules.d/70-spear-b2xx.rules
udevadm control --reload-rules
udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=2500 || true

install -d -m 0755 /etc/security/limits.d   # Arch の既定では無い
printf '# S.P.E.A.R. (packaging/kiosk): SCHED_FIFO for the UHD RX thread, mlock\n%s - rtprio 99\n%s - memlock unlimited\n' "$USER_NAME" "$USER_NAME" > /etc/security/limits.d/spear.conf

/usr/local/bin/spear-session-select plasma
echo "installed for $USER_NAME (spear.sh: $REPO/spear.sh)"
echo "  switch now:   sudo -n /usr/local/bin/spear-session-select spear --restart   (or the desktop icon: scripts/install-desktop.sh)"
echo "  back:         EXIT in S.P.E.A.R.; if stuck, Ctrl+Alt+F3 and: sudo /usr/local/bin/spear-session-select plasma --restart"
echo "  remove all:   sudo $0 --remove"
echo "  rtprio/memlock take effect at the next login"
