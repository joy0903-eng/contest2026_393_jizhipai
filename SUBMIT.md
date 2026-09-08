# 提交指南（SUBMIT）

本目录是 AI-VOX3 参加 **2026 首届 openvela AI 硬件开发者大赛**（队伍 `jizhipai` / 编号 `393`）的参赛树。
当前已完成 **M1：最小 NSH 基线**（板级 BSP 骨架 + CI 云编译工作流 + 作品说明）。

> ⚠️ AI 助手当前使用的 GitHub 集成仅有**只读**权限，无法 fork / 创建仓库 / 推送。
> 因此本树已初始化为可直接推送的 git 仓库，下面给出**你本地一条命令即可提交**的步骤。

## 1. 推送到你的参赛仓（二选一）

### 方式 A：直接推到队伍专属仓（如果你是 open-vela/contest2026_393_jizhipai 的协作者）
```bash
cd contest_repo
git remote add upstream https://github.com/open-vela/contest2026_393_jizhipai.git
git push upstream dev-ai-contest-2026
```

### 方式 B：先 Fork 再推（在 GitHub 网页点 "Fork" 把 open-vela/contest2026_393_jizhipai 派生到 joy0903-eng 账号）
```bash
cd contest_repo
git remote add fork https://github.com/joy0903-eng/contest2026_393_jizhipai.git
git push fork dev-ai-contest-2026
```
随后在 GitHub 上对 `open-vela/contest2026_393_jizhipai` 发起 Pull Request（base = `dev-ai-contest-2026`），可自行 review 并合入。

## 2. 让 CI 跑起来并拿到固件

- 推送（或合入 PR）后，GitHub Actions 的 **Build AI-VOX3 OpenVela firmware** 工作流会自动运行：
  `repo init/sync`（清华镜像）→ overlay 本仓 → `./build.sh .../configs/nsh --cmake` → 合并 `nuttx.merged.bin`。
- 在仓库 **Actions** 页找到该次运行，下载产物 `aivox3-openvela-firmware`（含 `nuttx.merged.bin`）。
- 烧录：`esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x0 nuttx.merged.bin`
- 上电后 USB-CDC（VID:PID 303A:1001）出现 `nsh>` 即 L0 启动成功。

## 3. CLA 与截止

- 首次贡献需在 https://openvela.com/#/community/cla 签署 CLA；PR 上 `/check-cla` 复检通过。
- 提交截止：**2026-09-20**。

## 4. M2（下一步，待 CI 绿后）

- 在 `board/contest_board/src/` 增量加入舵机 LEDC、ES8311 音频 I2S、ST7789 LCD、WS2812、SD、按键驱动（参考 `vendor_espressif/boards/esp32s3/esp32s3-eye/src/`）。
- 新增 `configs/openvela/defconfig` 启用上述子系统 + `packages/ai_agent` 框架。
- 将 `app/hello_app` 替换为 aivox3 演示应用（linkfile → `packages/demos/contest2026_393_aivox3`）。
