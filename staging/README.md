# AI-VOX3 OpenVela 源码暂存区（staging）

本目录是 **emakefun AI-VOX3（ESP32-S3-R8）** 移植到 **OpenVela（= Apache NuttX + LVGL + openvelaClaw）**
的全部板级支持包（BSP）与应用源码的**暂存区**。代码在此写就，待 Linux 构建机就绪后拷入
OpenVela 仓库编译（本机为 Windows，按官方要求**不可**在 WSL/Docker 内编译）。

> 密钥安全：LLM API Key **仅**以构建变量 / Kconfig 符号 `CONFIG_AIVOX3_LLM_API_KEY`
> 引用，**任何文件中均无明文密钥**。真机烧录前通过 `menuconfig` 或 NVS 注入。

---

## 1. 目录结构

```
staging/
├── vendor_espressif/boards/esp32s3/ai_vox3/   # ★ 板级支持包（本地包名；拷入仓库路径为 vendor/espressif/）
│   ├── configs/{nsh,lcd,audio,wifi,openvela}/defconfig
│   ├── include/board.h                         # 引脚/内存映射（集中定义，三脚存疑处加 TODO）
│   ├── scripts/Make.defs                       # 交叉编译器 / 编译选项
│   └── src/
│       ├── board.c                             # 板级初始化（外设 probe）
│       ├── ai_vox3_lcd.c                       # ST7789 (FSPI)
│       ├── ai_vox3_audio.c                     # ES8311 + I2S
│       ├── ai_vox3_servo.c                     # LEDC PWM 舵机 ×4
│       ├── ai_vox3_ws2812.c                    # WS2812 (RMT 800kHz GRB)
│       ├── ai_vox3_buttons.c                   # 按键 A/B/BOOT
│       ├── ai_vox3_sd.c                         # TF/SD (MMC 1-bit)
│       └── Kconfig                             # menuconfig 菜单项
└── apps/examples/desktop_companion/            # ★ 桌面小跟班应用（拷入 apps/examples/）

# 注意：本暂存目录本地名为 vendor_espressif（下划线），但拷入 OpenVela 仓库后
# 磁盘路径是 vendor/espressif（斜杠，由 manifest 的 path 属性决定）。详见下文第 2 节。
    ├── Makefile, Kconfig
    ├── main.c                                  # 状态机：待机→唤醒→对话→回待机
    ├── ui_lvgl.c/.h                            # 5 种表情 + 时间/天气/待办卡片
    ├── net_services.c/.h                        # Wi-Fi + NTP + 天气(Open-Meteo) + 待办(NVS)
    ├── llm_client.c/.h                          # 调 mimo-v2.5-pro (OpenAI /v1/chat/completions)
    ├── audio_pipeline.c/.h                       # 采集/播放/提示音（ES8311 I2S）
    ├── face_follow.c/.h                         # 无摄像头 → 舵机 demo 降级
    ├── smart_home.c/.h                          # 智能家居接口 stub
    └── config.h                                # 端点/模型/引脚宏（密钥仅引用）
```

---

## 2. 拷入 OpenVela 仓库

在构建机上，OpenVela 仓库根目录（已 `repo sync` 全量）下执行：

```bash
# BSP（注意目标路径是 vendor/espressif，斜杠，由 manifest 决定）
cp -r staging/vendor_espressif/boards/esp32s3/ai_vox3 \
      <openvela>/vendor/espressif/boards/esp32s3/ai_vox3

# 应用
cp -r staging/apps/examples/desktop_companion \
      <openvela>/apps/examples/desktop_companion
```

### 必须追加的一处注册（否则 menuconfig 看不到该 app）

在 `<openvela>/apps/examples/Kconfig` 末尾加入（与其它 example 并列）：

```
source "examples/desktop_companion/Kconfig"
```

并在 `<openvela>/apps/examples/Make.defs` 中追加（让构建系统纳入该目录）：

```
ifeq ($(CONFIG_EXAMPLES_DESKTOP_COMPANION),y)
  CONFIGURED_APPS += $(APPDIR)/examples/desktop_companion
endif
```

> 说明：不同 OpenVela 版本的 `apps/examples` 注册方式可能为 `Make.defs` 的
> `CONFIGURED_APPS` 或 `Application.mk` 自动扫描；以目标树实际约定为准。

---

## 3. 配置与构建

```bash
cd <openvela>

# 先应用全功能配置（含 USB-CDC / LCD / 音频 / WiFi / desktop_companion）
./build.sh vendor/espressif/boards/esp32s3/ai_vox3/configs/openvela/ -j4

# 可选：在 menuconfig 中注入 LLM API Key（不要写进 git）
#   → Desktop Companion (AI-VOX3) example
#     → LLM API key (injected at build; NEVER commit plaintext)
make menuconfig        # 仅当 build.sh 未进入 menuconfig 时；或先 unset 再配

# 进入内核目录烧录
cd nuttx
make -j$(nproc) flash ESPTOOL_PORT=/dev/ttyACM0 ESPTOOL_BINDIR=./
```

> 构建机前置（详见 porting_plan.md §6）：真实 **Ubuntu 22.04**（VM/双系统/云，
> **非 WSL/非 Docker**）、`repo`+Git LFS、`xtensa-esp32s3-elf-12.2.0`、≥40GB 磁盘。

---

## 4. 本机烧录（Windows，已验证 esptool `default_reset`）

固件构建完成后，在本机用已验证的 esptool 链路烧录（原生 USB-CDC，VID:PID=303A:1001）：

```bat
esptool.py --chip esp32s3 --port COMx --baud 921600 ^
           --before default_reset --after hard_reset ^
           write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 nuttx.bin
```

或使用构建产出的合并镜像（如 `nuttx.merged.bin`）一次性烧录。串口工具 115200 可看到 `nsh>`。

---

## 5. 编译前构建机还需具备 / 需真机验证的点（TODO(real-device)）

代码已尽量按 ESP32-S3 NuttX 驱动惯例写成可编译结构，但以下点**必须**在构建机/真机上核实：

1. **I2S 三脚**（最高优先级）：`src/ai_vox3_audio.c` 与 `include/board.h` 中
   `BOARD_ES8311_I2S_WS/DOUT/DIN` 厂商文档自相矛盾，须示波器/实测定稿。
2. **ES8311 Codec 驱动符号**：`CONFIG_AUDIO_ES8311`（或 `CONFIG_AUDIO_ESP32S3_ES8311`）
   需在当前树 `menuconfig` 中确认并启用；`es8311_initialize()` 入参顺序亦依树版本核对。
3. **LCD 色序 / 旋转**：ST7789 为 RGB 还是 BGR、面板旋转方向，需真机实测（§3.3/§3.4）。
4. **USB-CDC 控制台**：确认 `CONFIG_ESP32S3_USBSERIAL` + `CONFIG_CDCACM_CONSOLE` 组合
   在目标树能输出 `nsh>`（VID:PID 303A:1001）。
5. **电池 ADC**：分压比 / mV-per-LSB 未给，需标定（board.h `BOARD_BAT_ADC_*`）。
6. **TLS**：LLM/天气为 HTTPS，需 `mbedTLS` + `CONFIG_NETUTILS_WEBCLIENT` TLS 支持。
7. **NVS 分区**：待办存储依赖 NVS，需在分区表定义 NVS 区并启用 `CONFIG_NVS_ENABLED`。
8. **RMT / LEDC 接口**：`ai_vox3_ws2812.c`、`ai_vox3_servo.c` 所用 arch 函数名（如
   `esp32s3_rmt_tx_init` / `esp32s3_ledc_set_timer`）依目标树头文件核对。

---

## 6. 降级实现说明（对应 porting_plan.md §7）

- **人脸跟随**：无摄像头 → `face_follow.c` 实现舵机 demo（讲话事件触发头部位舵机摆动/归中）；
  `face_follow_update(x,y)` 接口预留，便于日后接 OV2640 + ESP-DL。
- **语音闭环**：无 ASR/TTS → 按键/串口触发 → `llm_client` 文本请求 → LCD/串口回显；
  麦克风采集已实现（`audio_capture_*`）但 `asr_transcribe`/`tts_synthesize` 返回 `ENOSYS`。
- **智能家居**：`smart_home.c` 解析意图 + 日志 + 返回“未配置”（`CONFIG_SMART_HOME_BACKEND=none`）。
