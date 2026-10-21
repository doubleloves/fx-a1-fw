# FX_A1 — 斐讯 A1 空气净化器自制固件

基于 MXCHIP MiCO / EMW3080B（RTL8710B）平台，从零编写的 A1 空气净化器固件。
支持触摸面板、WS2812 灯环、网页控制、热点配网、MQTT / Home Assistant、OTA 和定时任务。

> **当前版本：v1.0**（2026-10-21） · [更新日志](CHANGELOG.md) · 固件 `dist/FX_A1_v1.0.bin`

> 本项目只包含**应用层源码**。MiCO SDK、编译工具链、第三方二进制内核、设备原厂备份**均不在仓库中**，需按下方说明自行准备。

---

## 快速开始（TL;DR）

```bash
./setup.sh                  # 拉取 SDK + 打补丁 + 建工具链软链
./tools/extract_kernel.sh   # 下载基础固件并提取 kernel/A1_kernel.bin
./build.sh                  # 编译 + 打包 → dist/FX_A1_v1.0.bin
```

刷写（传统方式）：19 脚接 GND 上电进 bootloader → 串口 9/10 @921600 → 输入 `3 -id 3` → YMODEM 发送 `dist/FX_A1_v1.0.bin` → `7` 重启。
详细步骤见下文「刷写」，操作前务必阅读安全提示。


## 功能

- 开关 / 风速（1–100，网页滑条 + 面板滑条，面板 100ms 轮询）
- 5 种模式：睡眠 / 低速 / 中速 / 高速 / 极速
- 触摸面板（I2C 地址 `0x08`）：滑条调速、短按开关机、长按约 5s 重置 WiFi
- 31 颗 WS2812 灯环：**默认关闭**；效果 = 随速 / 固定色 / 跑马灯 / 流光 / 呼吸 / 静态彩虹，色相与亮度可调
  - 随速模式：**速度越快越亮**（亮度 10%→100%），同时颜色蓝→红
- 热点配网：`A1_xxxx` @ `192.168.4.1`，网页扫描 WiFi 并连接
- REST API + 深色网页控制界面
- MQTT + Home Assistant 自动发现（⚠️ **未实机验证**）
- OTA 升级（网页上传本地 `.bin`）
- **5 组定时开关**（一次 / 每天 / 工作日 / 周末）+ **倒计时开关机**，SNTP 自动对时（⚠️ **未实机验证**）
- 风扇转速（RPM）/ 滤芯使用时间与剩余寿命 / 童锁 / 运行时间 / 串口日志

### 项目状态

| 功能 | 状态 |
|---|---|
| 开关 / 风速 / 模式 / 面板滑条 | ✅ 已实机验证 |
| 网页控制 / 热点配网 / WiFi 回退 | ✅ 已实机验证 |
| 31 颗 WS2812 灯环 / 特效 | ✅ 驱动已验证；灯珠数 31 待最终确认 |
| 定时开关 / 倒计时 | ⚠️ 已实现，待实机验证 |
| 风扇转速 / 滤芯剩余 | ⚠️ 已实现，待实机验证 |
| MQTT / Home Assistant 自动发现 | ⚠️ 已实现，待实机验证 |
| 网页 OTA 上传升级 | ⚠️ 已实现，待实机验证 |

## 硬件

| 项目 | 说明 |
|---|---|
| WiFi 模块 | EMW3080B（RTL8710B / AmebaZ），板级名 MK3080B，Cortex-M4 |
| 风机 | PWM 通道 2（`MICO_PWM_3`），占空比反相：`duty = 100 - 风速` |
| 灯环 | 31 颗 WS2812，数据脚 `MICO_GPIO_13`（寄存器 `0x40001000` bit12），GRB |
| 触摸面板 | I2C 地址 `0x08`（SCL=GPIO_7，SDA=GPIO_8） |
| 应用串口 | 模块 24/25 脚，115200 |
| Bootloader | 模块 9/10 脚，921600；19 脚接 GND 上电进入 |

### 接线 / 拆机参考

不确定怎么拆机、接线或烧录时，请参考 zA1 项目的 wiki（串口 TTL 和 J-Link SWD 两种方式都有说明）：

- 拆机与烧录总览：https://github.com/a2633063/zA1/wiki/固件烧录
- TTL 串口接线：见上述 wiki 中的「方法一」，其引用的 M1 TTL 教程与 A1 通用（参考 `3.2 EMW3080B 模块引脚定义` ~ `3.6 升级固件`）
- J-Link 烧录：见上述 wiki 中的「方法二」

> 注意：A1 需要**安装滤芯**才会通电；刷写时不要接 220V 市电。

## 仓库结构

```
.
├── a1/                 # 应用源码（main.c / a1.mk / mico_config.h）
├── patches/            # 对 MXCHIP mico-os SDK 的少量修改
├── tools/              # 烧写、备份、内核提取脚本
├── kernel/             # 基础内核放置目录（*.bin 不入库）
├── dist/               # 最终固件输出（FX_A1_v*.bin，不入库）
├── build.sh            # 编译 + 自动打包
├── pack.sh             # 内核 + 应用 → dist/FX_A1_v<版本>.bin
├── setup.sh            # 拉取 SDK、打补丁、创建工具链软链
├── flash.sh            # 刷写（3 -id 3 + YMODEM 自动化，可选）
├── restore.sh          # 恢复原固件
├── CHANGELOG.md        # 更新日志
└── 项目记录.md          # 开发过程与踩坑记录（详细）
```

## 从源码构建

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

SDK 自带的内核在 A1 上无法启动，必须使用兼容的基础内核。仓库不直接提供该二进制文件，但脚本会自动从**公开基础固件**（zA1 v0.3.1 Release）下载并提取：

```bash
./tools/extract_kernel.sh
```

也可以指定本地固件或 URL：

```bash
./tools/extract_kernel.sh /path/to/base_firmware.bin
./tools/extract_kernel.sh https://example.com/base_firmware.bin
```

脚本取固件前 `0x75000` 字节作为 `kernel/A1_kernel.bin`。已验证的 002.024 内核 md5 为：

```
cd3618b81ca8cd26b43caca885fbc9bf
```

如果自动下载失败（`github.com` 访问不了），手动下载基础固件的 `.ota.bin`，再用本地方式提取即可。

### 4. 编译

```bash
./build.sh                # 默认 release
./build.sh a1 release_log # 带串口日志（调试用）
```

产物：`dist/FX_A1_v1.0.bin`

## 刷写

推荐使用传统的 **`3 -id 3` + YMODEM** 串口方式；网页 OTA 作为日常升级的可选方式。

### 方式一：串口 + Bootloader（传统方式，`3 -id 3`）

1. 设备断电，模块 **19 脚接 GND**，再上电进入 `MXCHIP>` bootloader 菜单
2. TTL 串口接模块 **9/10 脚**，波特率 **921600，8N1，无流控**；应用日志则是 **24/25 脚 @ 115200**（接线不确定见上文「接线 / 拆机参考」）
3. 在串口终端里输入下面这条命令并回车（必须以 `\r` 结尾）：

   ```
   3 -id 3
   ```

   等设备提示 `Waiting for the file...`
4. 用串口终端的 **YMODEM 发送**功能，发送 `dist/FX_A1_v1.0.bin`
   - SecureCRT：`Transfer → Send YMODEM`
   - Xshell：`传输 → 用 YMODEM 发送`
   - 其他终端找 `YMODEM send` / `sz --ymodem` 等价功能
5. 等待传输完成（约 597KB）
6. 输入 `7` 重启（`6` 也可启动应用），或断电重上电
7. 断电、断开 19 脚，再正常上电运行

> 分区对照：`3 -id 3` 写入**内核分区 ID3**（本固件目标分区）；`1` 写应用分区，**不要用**。

### 方式二：网页 OTA（可选，日常升级）

设备已联网时，打开设备网页 →「OTA 固件升级」→ 选择本地 `dist/FX_A1_v*.bin` →「上传并升级」。

- 设备会把文件写入 OTA 分区、校验、自动重启，bootloader 再把它拷回内核分区
- 网页显示 `1/3 上传 → 2/3 写入校验重启 → 3/3 确认新版本`
- 要求：文件为同格式、**小于约 741KB（0xb5000）**
- 失败/断电不影响当前固件，可再用串口方式恢复

> 仓库里的 `./flash.sh` 只是把 `3 -id 3` + YMODEM 自动化了，可选；按传统方式手动操作就用上面的 1–7 步。

> ⚠️ **刷写前必读（安全）**
> - **刷写时不要接 220V 市电**，避免带电操作/触电风险。
> - 必须**装好滤芯，或压住滤芯微动开关**，否则设备不上电/无法正常工作。
> - 刷写前请务必备份原机 Flash（`backup/`，不入库），并确认已理解变砖风险。

## 使用

- 未配网：连接热点 `A1_xxxx`，打开 `http://192.168.4.1` 配置 WiFi
- 已配网：访问设备 IP 打开控制网页
- MQTT topic：`device/a1/<mac>/{set,state,availability}`
- Home Assistant 自动发现：`homeassistant/fan/<name>/config`
- ⚠️ **MQTT / Home Assistant 自动发现尚未实机验证**，首次使用请接串口（24/25 脚 @115200）看日志排查。

### 主要 API

```
/api/status  /api/power  /api/speed  /api/mode  /api/led
/api/lock    /api/filter/reset        /api/reboot  /api/factory
/api/wifi/scan  /api/wifi/status  /api/wifi/set
/api/mqtt   /api/ota   /api/task   /api/countdown
诊断: /api/diag/pwm  /api/diag/gpio  /api/diag/i2c  /api/diag/i2cscan  /api/diag/mem
```

- `/api/task?i=0..4&hour=HH&minute=MM&repeat=R&action=A&on=0|1`
  - `repeat`: `0`=一次, `31`=工作日, `96`=周末, `127`=每天
  - `action`: `0`=关机, `1`=开机
- `/api/countdown?sec=秒&action=0|1`（`sec=0` 取消）

## 开发与调试

- **应用日志**：模块 **24/25 脚 @ 115200**；bootloader 菜单 **9/10 脚 @ 921600**
- **带日志构建**：`./build.sh a1 release_log`
- **诊断接口**：
  - `/api/diag/mem` — 堆使用（free/total/alloc/chunks）
  - `/api/diag/i2cscan` — I2C 扫描（触摸面板应为 `0x08`）
  - `/api/diag/i2c?addr=0x08&reg=0&n=4` — 读面板原始数据
  - `/api/diag/pwm?ch=2&duty=N` — 测试风机 PWM
  - `/api/diag/gpioread?pin=N` — 读 GPIO
- **串口 CLI**：`led ...`、`pwm ...`，详见 [`项目记录.md`](项目记录.md)

## 常见问题

**构建时找不到 Python 2.7 / arm-none-eabi-gcc？**
检查 `tools/py27/bin/python` 和 `tools/gnu-mcu-eclipse/.../bin`；GCC 可用 `GCC_BIN` 指定。

**`pack.sh` 提示缺少 `kernel/A1_kernel.bin`？**
运行 `./tools/extract_kernel.sh`；下载失败时手动下载基础固件后传入路径。

**刷完后不启动 / 风扇不转？**
确认刷写时未接 220V、滤芯微动开关已闭合、写入的是分区 ID3（`3 -id 3`）；接串口看日志。

**面板滑条不够顺？**
确认使用新版（面板 I2C 只初始化一次）；可再降低灯环刷新频率或调整轮询间隔。

**转速一直显示 0？**
风机 tacho 需接到 `MICO_GPIO_10`；不同风扇脉冲数/转不同，可改 `A1_TACHO_PPR`。

**定时不执行？**
定时依赖 SNTP 对时，设备需已联网；可先用倒计时验证执行逻辑。

**OTA 上传失败？**
确认文件同格式且 <741KB，并使用 `Content-Length`；失败不影响当前固件，可串口恢复。

## 说明与限制

- `kernel/A1_kernel.bin` 是**第三方二进制内核**（非本仓库源码），版权归其原始提供方所有。
- 本仓库不包含 MiCO SDK、工具链、原厂固件备份，以避免体积与版权问题。
- 修改配置结构时需递增 `A1_CFG_VERSION`，否则旧配置可能导致异常。
- 内存：HTTP 路径已去掉 `malloc/free`，改用静态缓冲，减少堆碎片；可用 `/api/diag/mem` 查看堆使用。
- WiFi 扫描已改为**异步轮询**，不再阻塞网页；网页滑条做 150ms 防抖，减少请求和 flash 写入。
- **MQTT / Home Assistant 自动发现尚未实机验证**，相关功能与 topic 可能仍需调整。
- **OTA 上传升级尚未完成实机验证**，首次使用请盯串口日志。
- **定时开关 / 倒计时尚未实机验证**，建议先短时间倒计时测试。
- **风扇转速**依赖风机 tacho 输出接到 `MICO_GPIO_10`；若显示为 0，说明该脚未接或脉冲数/转不同（可改 `A1_TACHO_PPR`）。
- **滤芯剩余**按 2000h 寿命估算，仅作参考；更换滤芯后请点“滤芯重置”。
- 详细开发记录、硬件逆向结论与踩坑经验见 [`项目记录.md`](项目记录.md)。

## 致谢

- [zA1](https://github.com/a2633063/zA1) — 公开的 A1 兼容固件项目。本项目的**基础内核来自该项目的 v0.3.1 公开固件**，拆机/接线/烧录方式也参考了它的 wiki。
- [MXCHIP / mico-os](https://github.com/MXCHIP/mico-os) — MiCO SDK
- [esphome-phicomm-m1](https://github.com/gitercn/esphome-phicomm-m1) — RTL8710B 内存优化、烧录与协议参考

> 本项目与上述项目的作者无隶属关系；相关二进制与文档版权归原作者所有。

## License

应用层源码可视为 MIT；第三方 SDK、基础内核及原厂固件遵循各自原始许可，不在本项目授权范围内。

## 免责声明

本项目为个人学习与自用项目，与斐讯、MXCHIP、Realtek 及其它第三方项目无隶属关系。
刷机有风险，可能导致设备损坏或变砖；请自行备份原机 Flash 并确认已理解操作风险。
因使用本项目造成的任何直接或间接损失，作者不承担责任。

