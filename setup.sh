#!/bin/bash
# 准备构建环境：拉取 mico-os SDK、打补丁、创建 MiCoder 工具软链
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"

SDK_URL="https://github.com/MXCHIP/mico-os.git"
SDK_COMMIT="9b09de78164940ff3876d2053f8e7dd42ca2b8ba"

# ---- 1. 获取 SDK ----
if [ ! -d "$ROOT/mico-os" ]; then
    echo "==> 克隆 mico-os ($SDK_COMMIT)"
    git clone "$SDK_URL" "$ROOT/mico-os"
fi
if [ -d "$ROOT/mico-os/.git" ]; then
    git -C "$ROOT/mico-os" checkout "$SDK_COMMIT"
else
    echo "==> 使用本地已有的 mico-os（非 git 仓库），跳过 checkout"
fi

# ---- 2. 应用补丁 ----
echo "==> 应用补丁"
for p in "$ROOT"/patches/*.patch; do
    [ -f "$p" ] || continue
    echo "    $(basename "$p")"
    patch -d "$ROOT/mico-os" -p1 -N < "$p" >/dev/null || echo "    (已应用或冲突，跳过)"
done

# ---- 3. MiCoder 软链 ----
echo "==> 创建 MiCoder 软链"
GCC_BIN="${GCC_BIN:-$ROOT/tools/gnu-mcu-eclipse/arm-none-eabi-gcc/6.3.1-1.1-20180331-0618/bin}"
if [ -d "$GCC_BIN" ]; then
    LINK="$ROOT/mico-os/MiCoder/compiler/arm-none-eabi-5_4-2016q2-20160622/Linux64"
    mkdir -p "$LINK"
    ln -sfn "$GCC_BIN" "$LINK/bin"
    ln -sfn "$(dirname "$GCC_BIN")" "$LINK/root"
else
    echo "    !! 未找到 ARM GCC: $GCC_BIN"
    echo "       请安装 arm-none-eabi-gcc 6.3.1 后重跑，或设置 GCC_BIN=<.../bin>"
fi

CMD_DIR="$ROOT/mico-os/MiCoder/cmd/Linux64"
mkdir -p "$CMD_DIR"
for t in bash sh dash rm echo awk cp cat mv perl grep sed make mkdir; do
    p="$(command -v "$t" 2>/dev/null || true)"
    [ -n "$p" ] && ln -sfn "$p" "$CMD_DIR/$t"
done

# ---- 4. 检查 Python 2.7 与基础内核 ----
PY="$ROOT/tools/py27/bin/python"
if [ -x "$PY" ]; then
    echo "==> Python: $("$PY" --version 2>&1)"
else
    echo "!! 未找到 SDK 构建所需的 Python 2.7: $PY"
    echo "   可参考项目记录第 3 节自行安装（如 micromamba/conda 创建 py27 环境）。"
fi

if [ ! -f "$ROOT/kernel/A1_kernel.bin" ]; then
    echo "!! 缺少 kernel/A1_kernel.bin（第三方二进制内核，未随仓库提供）"
    echo "   请用兼容的基础固件提取: ./tools/extract_kernel.sh <base_firmware.bin>"
fi

echo "==> 完成。构建: ./build.sh"
