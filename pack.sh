#!/bin/bash
# 生成可刷写的最终固件 dist/FX_A1_v<版本>.bin
# 用法: ./pack.sh [build_type]   build_type: release(默认) | release_log | debug
# 原理: 内核段(A1_kernel.bin, 0x75000) + 应用段(编译产物 0x75000 之后) + 16字节 MD5
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BT="${1:-release}"
KERNEL="$ROOT/kernel/A1_kernel.bin"
BUILD="$ROOT/build/a1@MK3080B@mocOS@mocIP@mocSSL@$BT/binary/a1@MK3080B@mocOS@mocIP@mocSSL@$BT.ota.bin"
VER="$(sed -n 's/^#define A1_VERSION "\(.*\)"/\1/p' "$ROOT/a1/main.c" | head -1)"
OUT="$ROOT/dist/FX_A1_v${VER}.bin"

[ -f "$KERNEL" ] || { echo "缺少内核文件: $KERNEL"; echo "请先用兼容固件提取: ./tools/extract_kernel.sh <base_firmware.bin>"; exit 1; }
[ -f "$BUILD" ]  || { echo "缺少编译产物: $BUILD"; exit 1; }
mkdir -p "$ROOT/dist"

python3 - "$KERNEL" "$BUILD" "$OUT" <<'EOF'
import sys, hashlib
kernel = open(sys.argv[1], 'rb').read()
build = open(sys.argv[2], 'rb').read()
OFF = 0x75000
if len(kernel) > OFF:
    sys.exit("内核文件超过 0x75000")
body = kernel + b'\xff' * (OFF - len(kernel)) + build[OFF:-16]
open(sys.argv[3], 'wb').write(body + hashlib.md5(body).digest())
EOF

echo "输出: $OUT ($(stat -c%s "$OUT") 字节)"
md5sum "$OUT"
