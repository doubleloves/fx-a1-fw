#!/bin/bash
# A1 air purifier custom firmware build script
# 用法: ./build.sh <app> [build_type]   build_type: release(默认) | release_log | debug
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
PY="$ROOT/tools/py27/bin/python"
APP="${1:-a1}"
BT="${2:-release}"
TARGET="${APP}@MK3080B@mocOS@mocIP@mocSSL@${BT}"

cd "$ROOT"
make -f mico-os/makefiles/Makefile \
     HOST_OS=Linux64 \
     PYTHON="$PY" \
     DISABLE_MICO_BLUENRG=1 \
     "$TARGET"

echo
echo "Output: $ROOT/build/$TARGET/binary/"
ls -la "$ROOT/build/$TARGET/binary/"*.ota.bin "$ROOT/build/$TARGET/binary/"*.all.bin 2>/dev/null || true

if [ "$APP" = "a1" ] && [ -f "$ROOT/kernel/A1_kernel.bin" ]; then
    echo
    "$ROOT/pack.sh" "$BT"
fi
