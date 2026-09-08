# AI-VOX3 桌面小跟班（OpenVela 移植）

## 一、作品简介

**AI-VOX3 桌面小跟班** 是一台基于 **ESP32-S3-R8**（16 MB Flash / 8 MB OCT PSRAM）的桌面 AI 陪伴机器人：双舵机云台（PAN/TILT）负责"转头/点头"，1.54" ST7789 LCD 负责表情，WS2812 RGB 灯充当状态指示，ES8311 + I2S 构成语音链路，microSD 存资源，三颗按键做交互。本作品把整块板子从 Arduino/ESP-IDF 生态**完整移植到 OpenVela（openvelaClaw 体系）**，目标是让这块自研硬件在 OpenVela 上跑通 NSH 最小系统，并以此为基面逐步叠加语音/显示/运动控制等子系统。

- 亮点：纯自研 BSP（板级初始化、舵机 LEDC、LCD、音频 I2S、WS2812、SD、按键）接入 OpenVela 编译树；原生 USB-CDC 控制台（VID:PID 303A:1001）零额外串口线即可调试；CI 一键云编译产出可烧录 `nuttx.merged.bin`。

## 二、选题方向

**新硬件适配**（兼 AI 硬件产品创新）。AI-VOX3 是一块未被 OpenVela 适配过的 ESP32-S3 衍生板，本仓库即其适配工程；后续可进一步接入官方 `packages/ai_agent` 框架做对话式 AI 桌面伴侣。

## 三、目录结构

本仓只动一个文件夹（其余由 manifest 的 `<linkfile>` 软链进 OpenVela 编译树）：

- `board/contest_board/` — **AI-VOX3 板级适配**（本作品核心）
  - `Kconfig` / `CMakeLists.txt` — 板级标识 `ARCH_BOARD_CONTEST2026_393_BOARD`，接入 `vendor/openvela/boards/contest2026_393_board`
  - `src/board_boot.c` — `openvela_board_initialize()` 板级初始化入口
  - `include/board.h` — 引脚定义（舵机/WS2812/LCD/ES8311/I2S/按键/电池/SD）
  - `configs/nsh/defconfig` — M1 最小 NSH 基线（esp32s3 + OPI PSRAM + USB-CDC）
- `app/hello_app/` — 官方示例应用骨架（占位，M2 替换为 aivox3 演示）
- `quickapp/hello_quickapp/` — 官方示例快应用骨架
- `.github/workflows/build.yml` — CI：repo 云编译 → 产出 `nuttx.merged.bin` 产物
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

2. 进入 OpenVela 工作区根目录（本仓的上一级），用统一入口编译：

   ```bash
   cd ..
   ./build.sh vendor/openvela/boards/contest2026_393_board/configs/nsh --cmake -j8
   ```

3. 产物位于编译树内 `nuttx.bin`；CI 会进一步合并为 `nuttx.merged.bin`（bootloader + partition + app）。

4. 烧录（整片）：

   ```bash
   esptool.py --chip esp32s3 --port <PORT> --baud 921600 \
     write_flash 0x0 nuttx.merged.bin
   ```

5. 上电后，USB-CDC 串口（303A:1001）出现 `nsh>` 提示符即代表 L0 启动成功。

## 五、AI Coding 使用说明

本作品在需求拆解、BSP 结构对齐、CI 工作流编写、文档生成等环节均借助 AI 辅助：

- **方案对齐**：依据官方大赛文档（新硬件适配赛道指引、最小 NSH 基线、已适配板 esp32s3-eye 参考）确定板级目录结构与 Kconfig 符号约定。
- **BSP 与构建**：参照 `vendor_espressif/boards/esp32s3/esp32s3-eye` 的 esp32s3 arch/chip/串口/PSRAM 配置，落地 AI-VOX3 的 NSH 基线 defconfig 与 CMake 板级骨架。
- **CI 交付**：编写 `.github/workflows/build.yml`，通过 Tsinghua 镜像 `repo init/sync` + 本仓 overlay + `./build.sh` 完成云端一键编译并上传固件产物。
- **阶段规划**：M1 = 最小 NSH 基线（本 PR）；M2 = 叠加舵机/音频/LCD/WS2812/SD 驱动与 aivox3 演示应用，并接入 `packages/ai_agent` 做对话式 AI。

完整对话日志见 `logs/` 目录。

## 六、提交说明（替换自官方模板）

本文件已由官方使用说明书替换为作品说明。截止 2026-09-20 前持续更新。
