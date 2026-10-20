#!/bin/bash
# 刷写自定义固件到内核分区(3)
# 用法: ./flash.sh [image.bin]   不带参数则刷最新的 dist/FX_A1_v*.bin
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
IMG="${1:-$(ls -t "$ROOT"/dist/FX_A1_v*.bin 2>/dev/null | head -1)}"
[ -n "$IMG" ] || { echo "找不到固件，请先 ./build.sh"; exit 1; }
[ -f "$IMG" ] || { echo "文件不存在: $IMG"; exit 1; }
SIZE=$(stat -c%s "$IMG")
[ "$SIZE" -ge $((0x75000)) ] || { echo "文件太小，不是有效的整机固件: $IMG"; exit 1; }

echo "固件: $IMG"
echo "大小: $SIZE 字节"
md5sum "$IMG"
echo
echo "⚠️  刷写时不要接 220V 市电，避免带电操作/触电。"
echo "⚠️  必须装好滤芯，或压住滤芯微动开关，否则设备不上电/不工作。"
echo

# 串口为第一个 /dev/ttyUSB*，bootloader @ 921600，写入分区 ID3
python3 "$ROOT/tools/a1_flash_part.py" 3 "$IMG"
