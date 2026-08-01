#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this uninstaller as root (for example: sudo ./uninstall.sh)." >&2
  exit 1
fi

rm -f /etc/systemd/system/fprintd.service.d/egis057e-local-libfprint.conf
rm -f /opt/egis057e-libfprint/lib/libfprint-2.so
rm -f /opt/egis057e-libfprint/lib/libfprint-2.so.2
rm -f /opt/egis057e-libfprint/lib/libfprint-2.so.2.0.0
rmdir /opt/egis057e-libfprint/lib 2>/dev/null || true
rmdir /opt/egis057e-libfprint 2>/dev/null || true
rmdir /etc/systemd/system/fprintd.service.d 2>/dev/null || true

systemctl daemon-reload
systemctl try-restart fprintd.service

echo "Removed the experimental EH57E libfprint build."
