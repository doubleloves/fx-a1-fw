#!/bin/bash
# 从兼容固件中提取基础内核（前 0x75000 字节）
# 用法: ./tools/extract_kernel.sh <base_firmware.bin> [输出路径]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$1"
OUT="${2:-$ROOT/kernel/A1_kernel.bin}"

[ -n "$SRC" ] || { echo "用法: $0 <base_firmware.bin> [输出路径]"; exit 1; }
[ -f "$SRC" ] || { echo "文件不存在: $SRC"; exit 1; }
[ "$(stat -c%s "$SRC")" -ge $((0x75000)) ] || { echo "文件太小，不是有效的整机固件"; exit 1; }

mkdir -p "$(dirname "$OUT")"
dd if="$SRC" of="$OUT" bs=1 count=$((0x75000)) status=none
echo "已提取基础内核: $OUT"
md5sum "$OUT"
