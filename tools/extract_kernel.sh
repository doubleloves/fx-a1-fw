#!/bin/bash
# 获取基础内核 A1_kernel.bin
#
# 用法:
#   ./tools/extract_kernel.sh                  # 自动从公开基础固件下载并提取
#   ./tools/extract_kernel.sh <firmware.bin>   # 从本地固件提取
#   ./tools/extract_kernel.sh <url>            # 从 URL 下载后提取
#
# 说明: 基础内核 = 兼容整机固件的前 0x75000 字节。
# 默认来源: zA1 v0.3.1 公开固件 (GitHub Release)，也可自行指定等价固件。
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$1"
OUT="${OUT:-$ROOT/kernel/A1_kernel.bin}"
CACHE="$ROOT/.cache/base_firmware.bin"

DEFAULT_URL="https://github.com/a2633063/zA1/releases/download/v0.3.1/zA1%40MK3080B%40moc.ota.bin"
EXPECT_MD5="cd3618b81ca8cd26b43caca885fbc9bf"

if [ -z "$SRC" ]; then
    mkdir -p "$(dirname "$CACHE")"
    if [ -f "$CACHE" ]; then
        echo "使用已缓存的基础固件: $CACHE"
    else
        echo "下载基础固件: $DEFAULT_URL"
        curl -fL --retry 3 --connect-timeout 20 -o "$CACHE" "$DEFAULT_URL"
    fi
    SRC="$CACHE"
elif [[ "$SRC" =~ ^https?:// ]]; then
    mkdir -p "$(dirname "$CACHE")"
    echo "下载: $SRC"
    curl -fL --retry 3 --connect-timeout 20 -o "$CACHE" "$SRC"
    SRC="$CACHE"
fi

[ -f "$SRC" ] || { echo "文件不存在: $SRC"; exit 1; }
[ "$(stat -c%s "$SRC")" -ge $((0x75000)) ] || { echo "文件太小，不是有效的整机固件"; exit 1; }

mkdir -p "$(dirname "$OUT")"
dd if="$SRC" of="$OUT" bs=1 count=$((0x75000)) status=none

echo "已提取基础内核: $OUT"
MD5="$(md5sum "$OUT" | cut -d' ' -f1)"
echo "md5: $MD5"
if [ "$MD5" != "$EXPECT_MD5" ]; then
    echo "⚠️  md5 与已验证的 002.024 内核不同（期望 $EXPECT_MD5）。"
    echo "    可能不兼容 A1，请确认你用的是 zA1 v0.3.1 或等价的基础固件。"
fi
