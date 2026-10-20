#!/bin/bash
# 恢复原厂/原第三方固件 (把出厂备份写回分区3)
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
echo "恢复原始固件: $ROOT/backup/3_kernel.bin"
python3 "$ROOT/tools/a1_flash_part.py" 3 "$ROOT/backup/3_kernel.bin"
