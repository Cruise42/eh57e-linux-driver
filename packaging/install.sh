#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
  echo "Run this installer as root (for example: sudo ./install.sh)." >&2
  exit 1
fi

package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
library="$package_dir/lib/libfprint-2.so.2.0.0"

if [ ! -f "$library" ]; then
  echo "The packaged libfprint library is missing." >&2
  exit 1
fi

if ldd "$library" | grep -q 'not found'; then
  echo "A required shared-library dependency is missing:" >&2
  ldd "$library" | grep 'not found' >&2
  exit 1
fi

install -d -m 0755 /opt/egis057e-libfprint/lib
install -m 0755 "$library" /opt/egis057e-libfprint/lib/libfprint-2.so.2.0.0
ln -sfn libfprint-2.so.2.0.0 /opt/egis057e-libfprint/lib/libfprint-2.so.2
ln -sfn libfprint-2.so.2 /opt/egis057e-libfprint/lib/libfprint-2.so

install -d -m 0755 /etc/systemd/system/fprintd.service.d
install -m 0644 "$package_dir/systemd/egis057e-local-libfprint.conf" \
  /etc/systemd/system/fprintd.service.d/egis057e-local-libfprint.conf

systemctl daemon-reload
systemctl try-restart fprintd.service

echo "Installed the experimental EH57E libfprint build."
echo "Test it with: fprintd-enroll && fprintd-verify"
