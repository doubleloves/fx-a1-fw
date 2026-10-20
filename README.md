# FX_A1 — 斐讯 A1 空气净化器自制固件

基于 MXCHIP MiCO / EMW3080B（RTL8710B）平台，从零编写的 A1 空气净化器固件。
支持触摸面板、WS2812 灯环、网页控制、热点配网、MQTT / Home Assistant、OTA 和定时任务。

> 本项目只包含**应用层源码**。MiCO SDK、编译工具链、第三方二进制内核、设备原厂备份**均不在仓库中**，需按下方说明自行准备。

---

## 功能

- 开关 / 风速（1–100，网页滑条 + 面板滑条）
- 5 种模式：睡眠 / 低速 / 中速 / 高速 / 极速
- 触摸面板（I2C 地址 `0x08`）：滑条调速、短按开关机、长按约 5s 重置 WiFi
- 31 颗 WS2812 灯环：随速 / 固定色 / 跑马灯 / 流光 / 呼吸 / 静态彩虹，色相与亮度可调
- 热点配网：`A1_xxxx` @ `192.168.4.1`，网页扫描 WiFi 并连接
- REST API + 深色网页控制界面
- MQTT + Home Assistant 自动发现
- OTA 升级（网页上传本地 `.bin`）
- 5 组定时任务，SNTP 自动对时
- 滤芯计时 / 童锁 / 运行时间 / 串口日志

## 硬件

| 项目 | 说明 |
|---|---|
| WiFi 模块 | EMW3080B（RTL8710B / AmebaZ），板级名 MK3080B，Cortex-M4 |
| 风机 | PWM 通道 2（`MICO_PWM_3`），占空比反相：`duty = 100 - 风速` |
| 灯环 | 31 颗 WS2812，数据脚 `MICO_GPIO_13`（寄存器 `0x40001000` bit12），GRB |
| 触摸面板 | I2C 地址 `0x08`（SCL=GPIO_7，SDA=GPIO_8） |
| 应用串口 | 模块 24/25 脚，115200 |
| Bootloader | 模块 9/10 脚，921600；19 脚接 GND 上电进入 |

## 仓库结构

```
.
├── a1/                 # 应用源码（main.c / a1.mk / mico_config.h）
├── patches/            # 对 MXCHIP mico-os SDK 的少量修改
├── tools/              # 烧写、备份、内核提取脚本
├── kernel/             # 基础内核放置目录（*.bin 不入库）
├── build.sh            # 编译 + 自动打包
├── pack.sh             # 内核 + 应用 → dist/FX_A1_v<版本>.bin
├── setup.sh            # 拉取 SDK、打补丁、创建工具链软链
├── flash.sh            # 刷写
├── restore.sh          # 恢复原固件
└── 项目记录.md          # 开发过程与踩坑记录（详细）
```

## 快速开始

### 1. 准备环境

- Linux x86_64
- **Python 2.7**（SDK 构建脚本依赖 Py2），默认路径 `tools/py27/bin/python`
- **ARM GCC 6.3.1**（GNU MCU Eclipse），默认路径 `tools/gnu-mcu-eclipse/arm-none-eabi-gcc/6.3.1-1.1-20180331-0618/bin`
  （可用环境变量 `GCC_BIN` 指定其它路径）
- 刷写脚本需要 Python 3 + `pyserial`

### 2. 获取 SDK 并打补丁

```bash
./setup.sh
```

会自动克隆 `MXCHIP/mico-os` 到固定 commit `9b09de78...`，应用 `patches/` 下的修改，并创建 MiCoder 软链。

### 3. 准备基础内核（重要）

SDK 自带的内核在 A1 上无法启动，必须使用兼容的基础内核。仓库不提供该二进制文件：

```bash
./tools/extract_kernel.sh <基础固件.bin>
```

该脚本提取固件前 `0x75000` 字节作为 `kernel/A1_kernel.bin`。

### 4. 编译

```bash
./build.sh                # 默认 release
./build.sh a1 release_log # 带串口日志（调试用）
```

产物：`dist/FX_A1_v1.0.bin`

## 刷写

有两种方式。**日常升级用 OTA（网页上传）**；串口只用于首次刷写/救砖/恢复。

### 方式一：网页 OTA（推荐，日常升级）

设备已联网时，打开设备网页 →「OTA 固件升级」→ 选择本地 `dist/FX_A1_v*.bin` →「上传并升级」。

- 设备会把文件写入 OTA 分区、校验、自动重启，bootloader 再把它拷回内核分区
- 网页显示 `1/3 上传 → 2/3 写入校验重启 → 3/3 确认新版本`
- 要求：文件为同格式、**小于约 741KB（0xb5000）**
- 失败/断电不影响当前固件，可再用串口方式恢复

### 方式二：串口 + Bootloader（首次刷写 / 救砖）

1. 设备断电，模块 **19 脚接 GND**，再上电进入 `MXCHIP>` bootloader 菜单
2. TTL 串口接模块 **9/10 脚**（bootloader 菜单），波特率 **921600**；应用日志则是 **24/25 脚 @ 115200**
3. 执行：

```bash
./flash.sh                        # 刷最新的 dist/FX_A1_v*.bin
./flash.sh dist/FX_A1_v1.0.bin    # 或指定文件
# 等价于脚本内部的: 3 -id 3  （写入内核分区 ID3）
```

4. 完成后断电、断开 19 脚、重新上电

> `tools/a1_flash_part.py` 默认打开第一个 `/dev/ttyUSB*`、921600。
> 也可使用综合脚本：`python3 tools/a1_flash.py --port /dev/ttyUSB0 flash <file>`（会自动尝试波特率）。

> ⚠️ **刷写前必读（安全）**
> - **刷写时不要接 220V 市电**，避免带电操作/触电风险。
> - 必须**装好滤芯，或压住滤芯微动开关**，否则设备不上电/无法正常工作。
> - 刷写前请务必备份原机 Flash（`backup/`，不入库），并确认已理解变砖风险。

## 使用

- 未配网：连接热点 `A1_xxxx`，打开 `http://192.168.4.1` 配置 WiFi
- 已配网：访问设备 IP 打开控制网页
- MQTT topic：`device/a1/<mac>/{set,state,availability}`
- Home Assistant 自动发现：`homeassistant/fan/<name>/config`

### 主要 API

```
/api/status  /api/power  /api/speed  /api/mode  /api/led
/api/lock    /api/filter/reset        /api/reboot  /api/factory
/api/wifi/scan  /api/wifi/status  /api/wifi/set
/api/mqtt   /api/ota   /api/task
诊断: /api/diag/pwm  /api/diag/gpio  /api/diag/i2c  /api/diag/i2cscan
```

## 说明与限制

- `kernel/A1_kernel.bin` 是**第三方二进制内核**（非本仓库源码），版权归其原始提供方所有。
- 本仓库不包含 MiCO SDK、工具链、原厂固件备份，以避免体积与版权问题。
- 修改配置结构时需递增 `A1_CFG_VERSION`，否则旧配置可能导致异常。
- 详细开发记录、硬件逆向结论与踩坑经验见 [`项目记录.md`](项目记录.md)。

## 致谢

- [MXCHIP / mico-os](https://github.com/MXCHIP/mico-os) — MiCO SDK
- 公开的 A1 兼容固件项目 — 提供可用的基础内核

## License

应用层源码可视为 MIT；第三方 SDK、基础内核及原厂固件遵循各自原始许可，不在本项目授权范围内。
