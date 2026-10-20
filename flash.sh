#!/bin/bash
# 刷写自定义固件到内核分区(3)
# 用法: ./flash.sh [image.bin]   不带参数则刷最新的 dist/FX_A1_v*.bin
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
IMG="${1:-$(ls -t "$ROOT"/dist/FX_A1_v*.bin 2>/dev/null | head -1)}"
[ -n "$IMG" ] || { echo "找不到固件，请先 ./build.sh"; exit 1; }
echo "刷写: $IMG"
python3 "$ROOT/tools/a1_flash_part.py" 3 "$IMG"
