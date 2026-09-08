# 提交指南（SUBMIT）

本目录是 AI-VOX3 参加 **2026 首届 openvela AI 硬件开发者大赛**（队伍 `jizhipai` / 编号 `393`）的参赛树。
当前已完成 **M2：完整板级移植 + 桌面伴侣演示应用**（全 BSP：LCD ST7789、舵机 LEDC、按键、ES8311 I2C；WS2812/SD 留 M3）。

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
  `repo init/sync`（清华镜像）→ overlay 本仓 → `./build.sh .../configs/openvela --cmake` → 合并 `nuttx.merged.bin`。
- 在仓库 **Actions** 页找到该次运行，下载产物 `aivox3-openvela-firmware`（含 `nuttx.merged.bin`）。
- 烧录：`esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x0 nuttx.merged.bin`
- 上电后 USB-CDC（VID:PID 303A:1001）出现 `nsh>`，运行 `desktop_companion` 即可演示。

## 3. CLA 与截止

- 首次贡献需在 https://openvela.com/#/community/cla 签署 CLA；PR 上 `/check-cla` 复检通过。
- 提交截止：**2026-09-20**。

## 4. M3（下一步，CI 绿后）

- `src/aivox3_ws2812.c`：实现 GPIO41 单线 WS2812（bit-bang / RMT 底层），注册 `/dev/leds0`。
- `src/aivox3_sd.c`：microSD（SDMMC，CMD=38/CLK=39/DAT0=40）挂载。
- 接入官方 `packages/ai_agent` 框架，把 BSP 作为"耳朵/手"，做对话式 AI 桌面伴侣。

## 5. 若授权可写 GitHub 令牌

把当前 GitHub 连接器授权为 **repo 写权限**（或提供 `repo` scope 的 PAT），AI 即可直接 `push` + 开 PR，并在 CI 报错时读日志迭代修复，省去你本地操作。
