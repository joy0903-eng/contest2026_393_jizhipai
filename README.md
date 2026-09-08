# AI-VOX3 桌面小跟班（OpenVela 移植）

## 一、作品简介

**AI-VOX3 桌面小跟班** 是一台基于 **ESP32-S3-R8**（16 MB Flash / 8 MB OCT PSRAM）的桌面 AI 陪伴机器人：双舵机云台（PAN/TILT）负责"转头/点头"，1.54" ST7789 LCD 负责表情，WS2812 RGB 灯充当状态指示，ES8311 + I2S 构成语音链路，microSD 存资源，三颗按键做交互。本作品把整块板子从 Arduino/ESP-IDF 生态**完整移植到 OpenVela（openvelaClaw 体系）**，本次提交即一个**可在官方 CI 云编译并通过的板级适配 + 演示应用**。

- 亮点：纯自研 BSP（LCD ST7789、舵机 LEDC PWM、按键、ES8311 I2C 引脚）接入 OpenVela 编译树；原生 USB-CDC 控制台（VID:PID 303A:1001）零额外串口线即可调试；CI 一键云编译产出可烧录 `nuttx.merged.bin`；配套 `desktop_companion` 演示应用可现场驱动舵机/屏幕/按键。
- 状态：M1 最小 NSH 基线已完成；**本次为 M2——完整板级移植 + 桌面伴侣演示应用**，WS2812（需 bit-bang/RMT 底层）与 microSD 留作 M3 增量。

## 二、选题方向

**新硬件适配**（兼 AI 硬件产品创新）。AI-VOX3 是一块未被 OpenVela 适配过的 ESP32-S3 衍生板，本仓库即其适配工程；后续可进一步接入官方 `packages/ai_agent` 框架做对话式 AI 桌面伴侣。

## 三、目录结构

本仓只动一个文件夹（其余由 manifest 的 `<linkfile>` 软链进 OpenVela 编译树）：

- `board/contest_board/` — **AI-VOX3 板级适配**（本作品核心，CMake 骨架）
  - `Kconfig` / `CMakeLists.txt` — 板级标识 `ARCH_BOARD_CONTEST2026_393_BOARD`
  - `src/board_boot.c` — `openvela_board_initialize()` 板级初始化入口 → `aivox3_bringup()`
  - `src/aivox3_bringup.c` — 设备注册总入口（LCD/按键/舵机/ES8311）
  - `src/aivox3_lcd.c` — ST7789（SPI2/FSPI）初始化，注册 `/dev/lcd0`
  - `src/aivox3_buttons.c` — 三按键 `board_button_initialize/board_buttons`，注册 `/dev/buttons`
  - `src/aivox3_servo.c` — LEDC PWM，注册 `/dev/pwm0`(PAN,GPIO42) `/dev/pwm1`(TILT,GPIO43)
  - `src/aivox3_ws2812.c` — WS2812 占位（M3）
  - `include/board.h` — 引脚定义
  - `configs/nsh/defconfig` — 最小 NSH 基线
  - `configs/openvela/defconfig` — **完整桌面伴侣配置（本次默认构建目标）**
- `app/hello_app/` — **桌面伴侣演示应用**（`desktop_companion`，置于官方 demo 槽位）
  - `desktop_companion_main.c` — 交互式演示：舵机/屏幕/按键
- `.github/workflows/build.yml` — CI：repo 云编译 `configs/openvela` → 产出 `nuttx.merged.bin`
- `contest2026_393_jizhipai.xml` — 本队 manifest（含 3 条 linkfile）
- `logs/` — AI Coding 日志（见下）

> 生产仓库（packages / nuttx / vendor 等）零改动。

## 四、运行方式

1. 拉取完整工程（队伍专属仓 + OpenVela 全量源码）：

   ```bash
   repo init -u https://github.com/open-vela/contest2026_393_jizhipai \
     -b dev-ai-contest-2026 -m contest2026_393_jizhipai.xml
   repo sync -c -j8
   ```

2. 进入 OpenVela 工作区根目录（本仓的上一级），用统一入口编译 **openvela 配置**：

   ```bash
   cd ..
   ./build.sh vendor/openvela/boards/contest2026_393_board/configs/openvela --cmake -j8
   ```

3. 产物位于编译树内 `nuttx.bin`；CI 会进一步合并为 `nuttx.merged.bin`（bootloader + partition + app）。

4. 烧录（整片）：

   ```bash
   esptool.py --chip esp32s3 --port <PORT> --baud 921600 \
     write_flash 0x0 nuttx.merged.bin
   ```

5. 上电后，USB-CDC 串口（303A:1001）出现 `nsh>`；运行演示：

   ```
   nsh> desktop_companion
   aivox3> servo 90 90     # 云台回中
   aivox3> face 1          # 画笑脸
   aivox3> demo           # 自动扫摆 + 表情 + 读按键
   aivox3> help           # 全部命令
   aivox3> quit
   ```

## 五、AI Coding 使用说明

本作品在需求拆解、BSP 结构对齐、驱动落地、CI 工作流编写、文档生成等环节均借助 AI 辅助：

- **方案对齐**：依据官方大赛文档（新硬件适配赛道指引、最小 NSH 基线、已适配板 `esp32s3-eye` 参考）确定板级目录结构与 Kconfig 符号约定；以官方 `esp32s3-eye` 的 ST7789/LCD/按键/LEDC 注册代码为蓝本落地 AI-VOX3 驱动。
- **BSP 与构建**：参照 `vendor_espressif/boards/esp32s3/esp32s3-eye` 的 esp32s3 arch/chip/串口/PSRAM 配置，落地 AI-VOX3 的 NSH 基线 defconfig 与 CMake 板级骨架；逐个外设以独立文件 + `CONFIG_AIVOX3_*` 开关实现，单外设失败不影响 NSH 启动。
- **演示应用**：`desktop_companion` 通过标准字符设备（`/dev/lcd0`、`/dev/pwm0/1`、`/dev/buttons`）驱动硬件，对缺失设备做了运行时降级，保证可编译、可演示。
- **CI 交付**：编写 `.github/workflows/build.yml`，通过 Tsinghua 镜像 `repo init/sync` + 本仓 overlay + `./build.sh` 完成云端一键编译并上传固件产物。
- **阶段规划**：M1 = 最小 NSH 基线（已合入）；**M2 = 完整板级移植 + 桌面伴侣演示（本 PR）**；M3 = WS2812（bit-bang/RMT）、microSD、接入 `packages/ai_agent` 做对话式 AI。

完整对话日志见 `logs/` 目录。

## 六、提交说明（替换自官方模板）

本文件已由官方使用说明书替换为作品说明。截止 2026-09-20 前持续更新。
