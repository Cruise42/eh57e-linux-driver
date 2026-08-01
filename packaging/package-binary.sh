#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "Usage: $0 VERSION LIBFPRINT_SO OUTPUT_DIRECTORY" >&2
  exit 2
fi

version=$1
library=$2
output_dir=$3
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
archive_base="eh57e-libfprint-${version}-linux-mint-22.3-x86_64"
stage_root=$(mktemp -d)
stage="$stage_root/$archive_base"
trap 'rm -rf "$stage_root"' EXIT HUP INT TERM

if [ ! -f "$library" ]; then
  echo "Library not found: $library" >&2
  exit 1
fi

mkdir -p "$stage/lib" "$stage/systemd" "$stage/source" "$output_dir"
install -m 0755 "$library" "$stage/lib/libfprint-2.so.2.0.0"
strip --strip-unneeded "$stage/lib/libfprint-2.so.2.0.0"
ln -s libfprint-2.so.2.0.0 "$stage/lib/libfprint-2.so.2"
ln -s libfprint-2.so.2 "$stage/lib/libfprint-2.so"
install -m 0755 "$project_root/packaging/install.sh" "$stage/install.sh"
install -m 0755 "$project_root/packaging/uninstall.sh" "$stage/uninstall.sh"
install -m 0644 "$project_root/packaging/README.md" "$stage/README.md"
install -m 0644 "$project_root/LICENSE" "$stage/LICENSE"
install -m 0644 "$project_root/NOTICE" "$stage/NOTICE"
install -m 0644 "$project_root/examples/systemd/egis057e-local-libfprint.conf" \
  "$stage/systemd/egis057e-local-libfprint.conf"
install -m 0644 "$project_root/src/egis057e.c" "$stage/source/egis057e.c"
install -m 0644 "$project_root/src/egis057e.h" "$stage/source/egis057e.h"
install -m 0644 "$project_root/patches/libfprint-integration.patch" \
  "$stage/source/libfprint-integration.patch"

tar --sort=name --owner=0 --group=0 --numeric-owner \
  -czf "$output_dir/$archive_base.tar.gz" -C "$stage_root" "$archive_base"
sha256sum "$output_dir/$archive_base.tar.gz" > \
  "$output_dir/$archive_base.tar.gz.sha256"

echo "$output_dir/$archive_base.tar.gz"
