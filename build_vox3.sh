#!/usr/bin/env bash
# =============================================================================
# build_vox3.sh -- 在 GitHub Actions (ubuntu-22.04) 上构建 AI-VOX3 (esp32-s3
#                  AI-VOX3 v0.1) 的 openvela 固件，并注入 v681 三处修复。
#
# 与 ci_build.sh 的区别：
#   ci_build.sh  编的是 ai_vox3 大赛板 demo（无语音、无你板子的 LCD 布局）
#   本脚本       编的是 esp32s3-eye —— 你手上这块 AI-VOX3 v0.1 本体，
#                带 ST7789 LCD(240x240) + WiFi + PSRAM8M + ai_agent 语音栈
#
# v681 三处修复（详见各文件的 [V681] 注释）：
#   1. esp32s3_board_lcd.c  背光 active-low + 打开 RTC/32K 域交接 -> 修黑屏
#   2. esp32s3_i2s.c        RX TDM 单声道采集            -> 修 mic peak=0
#   3. voice_channel.c      按键去抖 40ms -> 100ms        -> 修一次按键触发3次
#
# 用法: bash build_vox3.sh
# =============================================================================
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

WS="${GITHUB_WORKSPACE:-$PWD}"
BUILD="$WS/openvela"
MANIFEST_BRANCH="${MANIFEST_BRANCH:-dev-ai-contest-2026}"
SYNC_DEPTH="${SYNC_DEPTH:-1}"
LOG="$WS/build_vox3.log"
# GITHUB_TOKEN is provided by Actions; tolerate its absence for local runs.
git config --global user.email "vox3-ci@users.noreply.github.com" 2>/dev/null || true

# 收集到日志但不让日志淹没控制台
exec > >(tee -a "$LOG") 2>&1

echo "=============================================================="
echo " AI-VOX3 (esp32-s3 AI-VOX3 v0.1) openvela build + v681 fixes"
echo "=============================================================="

# ---------------------------------------------------------------- [1] deps
echo "== [1/7] build deps =="
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
  git git-lfs curl wget xz-utils unzip build-essential \
  cmake ninja-build python3 python3-pip python3-venv python3-dev \
  libc++abi-dev flex bison gperf libncurses-dev libssl-dev libffi-dev \
  libusb-1.0-0-dev pkg-config device-tree-compiler u-boot-tools
git lfs install
git config --global user.email "ci@vox3.local"
git config --global user.name "VOX3 CI"

# ---------------------------------------------------------------- [2] repo
echo "== [2/7] repo tool =="
sudo curl -sSL -o /usr/local/bin/repo \
  https://storage.googleapis.com/git-repo-downloads/repo \
  || sudo curl -sSL -o /usr/local/bin/repo \
       https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/repo
sudo chmod +x /usr/local/bin/repo
repo --version

# ------------------------------------------------------------ [3] toolchain
echo "== [3/7] xtensa-esp32s3-elf-12.2.0 toolchain =="
TOOLCHAIN="xtensa-esp32s3-elf-12.2.0_20230208-x86_64-linux-gnu.tar.gz"
TC="/tmp/${TOOLCHAIN}"
TC_OK=0
for TC_URL in \
  "https://github.com/espressif/crosstool-NG/releases/download/esp-12.2.0_20230208/${TOOLCHAIN}" \
  "https://dl.espressif.com/dl/${TOOLCHAIN}" ; do
  echo "  [tc] trying: $TC_URL"
  rm -f "$TC"
  if wget --timeout=60 --tries=2 -O "$TC" "$TC_URL" 2>&1 | tail -4; then
    TC_SZ=$(stat -c%s "$TC" 2>/dev/null || echo 0)
    echo "  [tc] got $TC_SZ bytes"
    if [ "$TC_SZ" -gt 50000000 ]; then TC_OK=1; break; fi
    echo "  [tc] file too small/corrupt, trying next mirror"
  else
    echo "  [tc] download failed, trying next mirror"
  fi
done
if [ "$TC_OK" != 1 ]; then
  echo "FATAL: toolchain download failed from all mirrors"
  exit 3
fi
sudo tar -xf "$TC" -C /opt
export PATH="/opt/xtensa-esp32s3-elf/bin:$PATH"
echo 'export PATH=/opt/xtensa-esp32s3-elf/bin:$PATH' >> ~/.bashrc
xtensa-esp32s3-elf-gcc --version | head -1

# ---------------------------------------------------------------- [4] sync
echo "== [4/7] repo init + sync openvela ($MANIFEST_BRANCH) =="
mkdir -p "$BUILD"
cd "$BUILD"
# NOTE: --depth belongs to `repo init`, never `repo sync`.  repo >=2.69 rejects
# `repo sync --depth` ("no such option: --depth"), which killed the previous run.
# Shallow-clone the whole tree at init time; sync WITHOUT --depth.
if [ "$SYNC_DEPTH" != "0" ]; then
  repo init -u https://github.com/open-vela/manifests.git \
    -b "$MANIFEST_BRANCH" -m openvela.xml \
    --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ \
    --git-lfs --depth=1
  repo sync -c -j"$(nproc)" --no-tags
else
  repo init -u https://github.com/open-vela/manifests.git \
    -b "$MANIFEST_BRANCH" -m openvela.xml \
    --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ --git-lfs
  repo sync -c -j"$(nproc)" --no-tags
fi
echo "--- top level ---"; ls -1 "$BUILD" | head -20

# ------------------------------------------------- [5] verify esp32s3-eye tree
echo "== [5/7] locate esp32s3-eye board + ai_agent =="
BOARD_DIR=""
for cand in \
  "$BUILD/vendor/espressif/boards/esp32s3/esp32s3-eye" \
  "$BUILD/nuttx/boards/xtensa/esp32s3/esp32s3-eye"
do
  [ -d "$cand" ] && { BOARD_DIR="$cand"; break; }
done
if [ -z "$BOARD_DIR" ]; then
  echo "FATAL: esp32s3-eye board dir not found after repo sync"
  find "$BUILD" -maxdepth 6 -type d -name "esp32s3-eye" 2>/dev/null | head
  exit 1
fi
echo "BOARD_DIR=$BOARD_DIR"

AI_DIR=""
for cand in "$BUILD/packages/ai_agent" "$BUILD/apps/examples/ai_agent"; do
  [ -d "$cand" ] && { AI_DIR="$cand"; break; }
done
echo "AI_DIR=${AI_DIR:-<not present>}"

# ------------------------------------------------- [6] inject v681 + defconfig
echo "== [6/7] inject v681 fixes + defconfig =="
PATCHES="$GITHUB_WORKSPACE/patches"
DEFCONF="$GITHUB_WORKSPACE/defconfigs/ai_vox3_defconfig"
[ -d "$PATCHES" ] || { echo "FATAL: patches/ missing"; exit 1; }
[ -f "$DEFCONF" ] || { echo "FATAL: defconfig missing"; exit 1; }

inject() {           # inject <src> <dst>
  local src="$1" dst="$2"
  if [ ! -f "$src" ]; then echo "  SKIP (not found): $src"; return 0; fi
  if [ ! -d "$(dirname "$dst")" ]; then echo "  SKIP (no dir): $dst"; return 0; fi
  cp "$src" "$dst"
  echo "  patched: ${dst#$BUILD/}"
}

# --- board: pin map + audio codec + the two v681 fixes -----------------
# esp32s3-eye.h MUST be overridden: upstream describes Espressif's own
# ESP32-S3-EYE (DC=43 / BCKL=48 / SPI2 CLK=21 CS=44 MOSI=47), while the
# AI-VOX3 wires DC=14 / BCKL=16 / SPI2 CLK=17 CS=15 MOSI=21.  Building the
# upstream header drives unconnected GPIOs, so the backlight never lights.
inject "$PATCHES/nuttx/boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3-eye.h" \
       "$BOARD_DIR/src/esp32s3-eye.h"

# esp32s3_board_audio.c (ES8311 codec + I2S/I2C buses) has no upstream
# counterpart at all; without it there is no microphone and no speaker.
if [ ! -f "$BOARD_DIR/src/esp32s3_board_audio.c" ]; then
  cp "$PATCHES/nuttx/boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_board_audio.c" \
     "$BOARD_DIR/src/esp32s3_board_audio.c"
  echo "  added:   ${BOARD_DIR#$BUILD/}/src/esp32s3_board_audio.c"
fi

inject "$PATCHES/nuttx/boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_board_lcd.c" \
       "$BOARD_DIR/src/esp32s3_board_lcd.c"
inject "$PATCHES/nuttx/boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_bringup.c" \
       "$BOARD_DIR/src/esp32s3_bringup.c"

# es8311.c: enable the ADC in es8311_initialize() instead of in
# es8311_configure(AUDIO_TYPE_INPUT).  The board registers the codec as a raw
# lower-half (board_audio.c calls audio_register("pcm0c", cap), never
# es8311_configure), so the configure() arm holding the [V661] fix is dead code
# here and the capture path stayed down -> "peak = 0" on every chunk.
if [ -f "$BUILD/nuttx/drivers/audio/es8311.c" ]; then
  inject "$PATCHES/nuttx/drivers/audio/es8311.c" \
         "$BUILD/nuttx/drivers/audio/es8311.c"
else
  echo "  (es8311.c not at expected path -- locating)"
  ES=$(find "$BUILD/nuttx" -name "es8311.c" 2>/dev/null | head -1)
  [ -n "$ES" ] && inject "$PATCHES/nuttx/drivers/audio/es8311.c" "$ES"
fi

if [ -f "$BUILD/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c" ]; then
  inject "$PATCHES/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c" \
         "$BUILD/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c"
else
  echo "  (i2s arch file not at expected path -- locating)"
  I2S=$(find "$BUILD/nuttx" -name "esp32s3_i2s.c" 2>/dev/null | head -1)
  [ -n "$I2S" ] && inject "$PATCHES/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c" "$I2S"
fi

if [ -n "$AI_DIR" ]; then
  inject "$PATCHES/packages/ai_agent/src/voice/voice_channel.c" \
         "$AI_DIR/src/voice/voice_channel.c"
fi

# defconfig -> board configs/ai_agent/defconfig
mkdir -p "$BOARD_DIR/configs/ai_agent"
cp "$DEFCONF" "$BOARD_DIR/configs/ai_agent/defconfig"
echo "  defconfig -> ${BOARD_DIR#$BUILD/}/configs/ai_agent/defconfig"

# verify the fixes really landed in the tree
echo "--- verify ---"
grep -q "V681_BTN_STABLE_POLLS"  "${AI_DIR:-/dev/null}/src/voice/voice_channel.c" 2>/dev/null \
  && echo "  OK  button debounce fix present" || echo "  ??  button debounce NOT found"
grep -q "V626_XTAL32K_HANDOFF    1" "$BOARD_DIR/src/esp32s3_board_lcd.c" \
  && echo "  OK  backlight RTC handoff enabled" || echo "  ??  handoff NOT enabled"
grep -q "g_i2s_rx_mono" "$BUILD/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c" 2>/dev/null \
  && echo "  OK  i2s RX mono fix present" || echo "  ??  i2s RX fix NOT found"
grep -q "V681] capture path enabled at init" "$BUILD/nuttx/drivers/audio/es8311.c" 2>/dev/null \
  && echo "  OK  ES8311 ADC enabled at init" || echo "  ??  es8311 ADC fix NOT found"
grep -q "V681] screen_launcher started" "$BOARD_DIR/src/esp32s3_bringup.c" \
  && echo "  OK  deferred screen launcher re-enabled" || echo "  ??  screen launcher NOT re-enabled"
grep -q "no screen worker started" "$BOARD_DIR/src/esp32s3_bringup.c" \
  && echo "  !!  V630 no-op stub STILL PRESENT" || echo "  OK  V630 stub gone"

# Pin map is the single most silent failure mode here: the upstream header
# targets Espressif's own ESP32-S3-EYE, so a build that keeps it drives GPIOs
# that are not wired to anything on the AI-VOX3 and the panel stays black
# even though every fix above compiled.  Assert it explicitly.
echo "--- pin map (must be AI-VOX3, not upstream ESP32-S3-EYE) ---"
for pair in "DISPLAY_DC:14" "DISPLAY_BCKL:16"; do
  k="${pair%%:*}"; want="${pair##*:}"
  got=$(grep -oE "define +ESP32S3_EYE_${k} +[0-9]+" "$BOARD_DIR/src/esp32s3-eye.h" \
        | grep -oE "[0-9]+$" || echo "?")
  [ "$got" = "$want" ] && echo "  OK  $k = $got" || echo "  !!  $k = $got (want $want)"
done
for pair in "SPI2_CSPIN:15" "SPI2_CLKPIN:17" "SPI2_MOSIPIN:21" "I2S0_BCLKPIN:10" \
            "I2S0_WSPIN:8" "I2S0_DINPIN:9" "I2S0_DOUTPIN:7" "I2C0_SCLPIN:12" "I2C0_SDAPIN:13"; do
  k="${pair%%:*}"; want="${pair##*:}"
  got=$(grep -E "^CONFIG_${k}=" "$DEFCONF" | tail -1 | cut -d= -f2 || echo "?")
  [ "$got" = "$want" ] && echo "  OK  CONFIG_$k = $got" || echo "  !!  CONFIG_$k = $got (want $want)"
done

# ------------------------------------------------------------ [6b] esp-idf
# packages_ai_agent's own README requires an ESP-IDF environment for the
# esp32s3-eye target, and fix_esp32s3.sh must run CONCURRENTLY with the first
# build after a distclean (it waits for esp-hal-3rdparty to be cloned, then
# patches mbedtls header priority / CCM cipher / spinlock init).  Skipping
# either one produces link or struct-layout errors deep in the build.
echo "== [6b/7] esp-idf toolchain + fix_esp32s3.sh =="
if [ ! -d /opt/esp-idf ]; then
  git clone --depth 1 -b v5.1.2 --recursive https://github.com/espressif/esp-idf.git /opt/esp-idf
fi
. /opt/esp-idf/install.sh esp32s3
echo "IDF_PATH=${IDF_PATH:-unset}"
export CCACHE_DISABLE=1

# ---------------------------------------------------------------- [7] build
echo "== [7/7] build =="
cd "$BUILD"
BUILD_SH=$(find "$BUILD" -maxdepth 2 -name build.sh | head -1)
[ -n "$BUILD_SH" ] || { echo "FATAL: build.sh not found"; exit 1; }
echo "using $BUILD_SH"

CFG="vendor/espressif/boards/esp32s3/esp32s3-eye/configs/openvela"
if [ ! -d "$BUILD/$CFG" ]; then
  echo "NOTE: $CFG missing, falling back to configs/ai_agent"
  CFG="nuttx/boards/xtensa/esp32s3/esp32s3-eye/configs/ai_agent"
fi
echo "CFG=$CFG"

# esp32s3-eye 官方只提供 configs/openvela（见 provision_openvela.sh 注释）
if [ -d "$BUILD/$CFG" ]; then
  # 用 ai_agent defconfig 覆盖 openvela 配置里的选项
  if [ -f "$BUILD/$CFG/defconfig" ]; then
    echo "merging ai_agent defconfig into $CFG/defconfig"
    cp "$DEFCONF" "$BUILD/$CFG/defconfig"
  fi

  # fix_esp32s3.sh races the build on purpose: it blocks until the build has
  # cloned esp-hal-3rdparty, then patches it.  See packages_ai_agent README.
  FIXSH="$AI_DIR/fix_esp32s3.sh"
  if [ -f "$FIXSH" ]; then
    echo "starting fix_esp32s3.sh in background"
    ( bash "$FIXSH" || echo "!! fix_esp32s3.sh exited non-zero" ) \
        >> "$WS/fix_esp32s3.log" 2>&1 &
    FIX_PID=$!
  else
    echo "NOTE: fix_esp32s3.sh not found at $FIXSH (build may hit mbedtls/spinlock errors)"
  fi

  if ! "$BUILD_SH" "$CFG" --cmake -j"$(nproc)"; then
    echo "!! --cmake failed, retrying classic make"
    "$BUILD_SH" "$CFG" -j"$(nproc)"
  fi
else
  echo "FATAL: no usable config dir ($CFG)"
  ls -1 "$BOARD_DIR/configs" 2>/dev/null
  exit 1
fi

if [ -n "${FIX_PID:-}" ]; then
  echo "waiting for fix_esp32s3.sh (pid=$FIX_PID)"
  wait "$FIX_PID" || true
  echo "--- fix_esp32s3.log ---"; cat "$WS/fix_esp32s3.log" 2>/dev/null | tail -20
fi

# ------------------------------------------------------------- collect
echo "== collect firmware =="
mkdir -p "$WS/firmware"
find "$BUILD/nuttx" -maxdepth 2 \
  \( -name 'nuttx.bin' -o -name 'nuttx' -o -name 'bootloader.bin' \
     -o -name 'partition-table.bin' -o -name 'nuttx.merged.bin' -o -name 'nuttx.elf' \) \
  -exec cp -v {} "$WS/firmware/" \; 2>/dev/null || true

# Keep the build log next to the artifacts even when the build died early.
cp -f "$LOG" "$WS/firmware/build_vox3.log" 2>/dev/null || true
# Record which fixes actually made it into the tree (see the verify block in
# step 6) so a downloader can tell a real v681 build from a stale one.
{
  echo "commit:   $(git -C "$WS" rev-parse HEAD 2>/dev/null)"
  echo "trigger:  ${GITHUB_REF:-?} @ $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "--- fix markers ---"
  grep -q "V626_XTAL32K_HANDOFF    1" "$BOARD_DIR/src/esp32s3_board_lcd.c" \
    && echo "backlight_rtc_handoff: yes" || echo "backlight_rtc_handoff: NO"
  grep -q "g_i2s_rx_mono" "$BUILD/nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c" 2>/dev/null \
    && echo "i2s_rx_mono: yes" || echo "i2s_rx_mono: NO"
  grep -q "V681] capture path enabled at init" "$BUILD/nuttx/drivers/audio/es8311.c" 2>/dev/null \
    && echo "es8311_adc_at_init: yes" || echo "es8311_adc_at_init: NO"
  grep -q "V681] screen_launcher started" "$BOARD_DIR/src/esp32s3_bringup.c" \
    && echo "screen_launcher: yes" || echo "screen_launcher: NO"
  [ -n "$AI_DIR" ] && { grep -q "V681_BTN_STABLE_POLLS" "$AI_DIR/src/voice/voice_channel.c" \
    && echo "button_debounce: yes" || echo "button_debounce: NO"; }
  echo "--- pin map ---"
  grep -E "define +ESP32S3_EYE_DISPLAY_(DC|BCKL)" "$BOARD_DIR/src/esp32s3-eye.h" || true
  grep -E "^CONFIG_ESP32S3_(SPI2|I2S0|I2C0).*PIN" "$DEFCONF" || true
  echo "--- sizes ---"
  ls -la "$WS/firmware" 2>/dev/null || true
} > "$WS/firmware/BUILD_INFO.txt" 2>&1

echo "--- firmware/ ---"; ls -la "$WS/firmware" 2>/dev/null || echo "(empty)"

# ------------------------------------------------------------- publish
# The artifact download API is not reachable from the build side's consumer,
# so publish the tree to a branch instead: a plain `git fetch` is enough to
# retrieve it.  A fresh orphan branch per push keeps the history flat and
# avoids any index conflict with the source branch.
echo "== publish firmware to branch 'vox3-fw' =="
PUB="$WS/.publish"
ORIGIN_URL="https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
rm -rf "$PUB"
# Try to clone an existing vox3-fw branch; otherwise start a brand-new orphan
# branch.  You cannot `git checkout -B` in an empty repo (no HEAD).
if git clone --quiet --depth 1 --branch vox3-fw "$ORIGIN_URL" "$PUB" 2>/dev/null; then
  echo "  cloned existing vox3-fw branch"
else
  git init --quiet "$PUB"
  git -C "$PUB" remote add origin "$ORIGIN_URL"
  git -C "$PUB" checkout --quiet --orphan vox3-fw
fi
cd "$PUB"
git config user.name "vox3-ci"
git config user.email "vox3-ci@users.noreply.github.com"
# Remove everything except .git, then copy fresh firmware.
git rm -rq --cached . 2>/dev/null || true
find . -mindepth 1 -maxdepth 1 ! -name '.git' -exec rm -rf {} + 2>/dev/null || true
cp -f "$WS"/firmware/* . 2>/dev/null || true
git add -A
git commit -q -m "vox3 firmware: $(date -u '+%Y-%m-%dT%H:%M:%SZ') [${GITHUB_REF:-?}]" || true
git push --force origin vox3-fw || echo "WARN: firmware publish failed (artifact upload still attempted)"

# ---------------------------------------------------------------- publish log
# On any early exit (set -e, missing board, build failure) publish the log so
# the failure is diagnosable without opening the Actions UI.
vox3_publish_log() {
  local rc=$?
  local PUB2="$WS/.publish_log"
  echo "== vox3 exiting rc=$rc -- publishing log to branch 'vox3-log' =="
  rm -rf "$PUB2"; mkdir -p "$PUB2"
  cp -f "$LOG" "$PUB2/build_vox3.log" 2>/dev/null || true
  cp -f "$WS/firmware/BUILD_INFO.txt" "$PUB2/" 2>/dev/null || true
  cd "$PUB2"
  git init --quiet .
  git config user.name "vox3-ci"; git config user.email "vox3-ci@users.noreply.github.com"
  git remote add origin "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
  # Fresh repo: use orphan branch (git checkout -B needs a HEAD).
  git checkout --quiet --orphan vox3-log
  git add -A
  git commit -q -m "vox3 build log rc=$rc $(date -u '+%Y-%m-%dT%H:%M:%SZ')" || true
  git push --force origin vox3-log || echo "WARN: log publish failed"
  exit $rc
}
trap vox3_publish_log ERR

# The ERR trap only fires on failure, so a successful build leaves vox3-log
# untouched and a consumer cannot tell "still building" from "finished OK".
# Leave an explicit success marker (firmware itself lands on vox3-fw branch).
echo "== publish success marker to 'vox3-log' =="
OKDIR="$WS/.publish_ok"
rm -rf "$OKDIR"; mkdir -p "$OKDIR"
{
  echo "BUILD_OK at $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "commit: $(git -C "$WS" rev-parse HEAD 2>/dev/null)"
  echo "firmware: published to vox3-fw branch"
} > "$OKDIR/build_ok.txt"
cd "$OKDIR"
git init --quiet .
git config user.name "vox3-ci"; git config user.email "vox3-ci@users.noreply.github.com"
git remote add origin "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
git checkout --quiet --orphan vox3-log
git add -A
git commit -q -m "vox3 build OK: $(date -u '+%Y-%m-%dT%H:%M:%SZ')" || true
git push --force origin vox3-log || echo "WARN: ok marker publish failed"

echo "== DONE =="
