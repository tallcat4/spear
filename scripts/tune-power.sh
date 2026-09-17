#!/usr/bin/env bash
# S.P.E.A.R. — Linux 電源管理調整 (要件 §13.1)。root で実行。
# overflow の主因は性能不足ではなく電源管理の介入。優先順:
#   1. CPU governor を performance に固定   2. USB autosuspend 無効化
#   3. RX thread の SCHED_FIFO (アプリ側: uhd::set_thread_priority_safe → rtprio 権限が要る)
#   4. mlockall (アプリ側 --mlock)
set -euo pipefail
MODE="${1:-apply}"   # apply | show | restore

show() {
  echo "governor:        $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
  echo "usb autosuspend: $(cat /sys/module/usbcore/parameters/autosuspend)  (-1 = disabled)"
  for d in /sys/bus/usb/devices/*/power/control; do
    v=$(cat "$d"); id=$(dirname "$(dirname "$d")")
    if [ -f "$id/idVendor" ] && [ "$(cat "$id/idVendor")" = "2500" ]; then
      echo "B2xx power/control: $v  ($id)"
    fi
  done
  echo "rtprio limit:    $(ulimit -r)"
  echo "on AC:           $(cat /sys/class/power_supply/AC*/online 2>/dev/null || echo '?')"
}

case "$MODE" in
  show) show ;;
  apply)
    [ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g"; done
    echo -1 > /sys/module/usbcore/parameters/autosuspend
    for d in /sys/bus/usb/devices/*/power/control; do echo on > "$d" 2>/dev/null || true; done
    # rtprio: /etc/security/limits.d/spear.conf に "@audio - rtprio 99" 等を置く(再ログイン要)
    show ;;
  restore)
    [ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo powersave > "$g"; done
    echo 2 > /sys/module/usbcore/parameters/autosuspend
    for d in /sys/bus/usb/devices/*/power/control; do echo auto > "$d" 2>/dev/null || true; done
    show ;;
  *) echo "usage: $0 [apply|show|restore]"; exit 1 ;;
esac
