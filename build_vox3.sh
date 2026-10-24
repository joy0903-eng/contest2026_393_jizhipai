#!/usr/bin/env bash
# =============================================================================
# build_vox3.sh -- 在 GitHub Actions (ubuntu-22.04) 上构建 AI-VOX3 (ESP32-S3-R8)
# 的 openvela 固件，使用官方配方（vendor/espressif/boards/esp32s3/ai_vox3 +
# desktop_companion 应用，后者已内置 mimo 端点 token-plan-cn.xiaomimimo.com）。
#
# 这个脚本忠实复刻 .github/workflows/build.yml 的关键步骤（staging 覆盖、nuttx
# 补丁、双堆注入、esp-hal 预置、esptool 4.8.1、Makefile 链 `m`、零地址段修复、
# SIMPLE_BOOT/审计闸门），并把产物发布到 vox3-fw / vox3-log 分支供 monitor 下载。
#
# 关键修复（相对更早误用 esp32s3-eye 的版本）：
#   - 构建目标从 esp32s3-eye(不完整/src 缺失/引脚错) 改为 ai_vox3（完整、引脚正确）
#   - 麦克风数据线引脚已修正（DOUT/mic=7, DIN/playback=9）-> 修"录到乱码/听不懂"
#   - 输出音量 CONFIG_ES8311_OUTPUT_INITVOLUME=1000 -> 修"听不清"
#
# 用法: bash build_vox3.sh   （由 .github/workflows/build-vox3.yml 在 push 到
#        vox3 分支时调用；也可 workflow_dispatch 手动触发）
# =============================================================================
# 注意：不要用 `set -u`。openvela 的 build/envsetup.sh 内部会引用若干
# 当时尚未定义的变量（如 $TOP/$OUT 等），在 `set -u` 下 `source` 它时一旦
# 碰到未绑定变量就会直接 `exit`，脚本当场死掉，连 `||` 兜底都来不及跑，
# 日志停在 "[6/7] build..." 之后一片空白（第 9 次 CI 就是这个坑）。
# 官方 build.yml 能过是因为 GitHub 默认 `set -e` 不带 `-u`。
set -o pipefail
export DEBIAN_FRONTEND=noninteractive

WS="${GITHUB_WORKSPACE:-$PWD}"
BUILD="$WS/openvela"
MANIFEST_BRANCH="${MANIFEST_BRANCH:-dev-ai-contest-2026}"
SYNC_DEPTH="${SYNC_DEPTH:-1}"
LOG="$WS/build_vox3.log"
# GITHUB_TOKEN 由 build-vox3.yml 注入；本地跑可留空（publish 会 WARN 跳过）。
GITHUB_TOKEN="${GITHUB_TOKEN:-}"
GITHUB_REPOSITORY="${GITHUB_REPOSITORY:-${GITHUB_REPO:-}}"

git config --global user.email "vox3-ci@users.noreply.github.com" 2>/dev/null || true
git config --global user.name  "vox3-ci" 2>/dev/null || true

# 收集到日志但不让日志淹没控制台。
# 注意：早期版本用 `exec > >(tee -a "$LOG")`，经管道 tee 是块缓冲；CI 在 job
# 超时(6h)被 SIGKILL 时缓冲区未刷盘，真实报错丢失，failure.txt 只剩 [6/7] 之前。
# 改成直接重定向：write() 进内核页缓存即刻落盘，SIGKILL 也不丢，失败标记 step
# 就能 `git show origin/vox3-log:build_vox3.log` 拿到完整日志定位真因。
exec > "$LOG" 2>&1

echo "=============================================================="
echo " AI-VOX3 openvela build -- ai_vox3 board + desktop_companion/mimo"
echo "=============================================================="

# ---------------------------------------------------------------- [1] deps
echo "== [1/7] build deps =="
sudo apt-get update -y
sudo apt-get install -y --no-install-recommends \
  git git-lfs curl wget xz-utils unzip build-essential \
  cmake ninja-build python3 python3-pip python3-dev rsync \
  libc++abi-dev flex bison gperf libncurses-dev libssl-dev libffi-dev \
  libusb-1.0-0-dev pkg-config device-tree-compiler u-boot-tools python-is-python3
git lfs install
git config --global user.email "ci@vox3.local"
git config --global user.name "VOX3 CI"
# esptool: 升级 pip 后装 4.8.1（满足 MKIMAGE >=4.8.0，且避开 5.x 的 merge_bin 改名问题）
{ python3 -m pip install --user --upgrade pip || python3 -m pip install --upgrade pip; } > pip_esptool.log 2>&1 || true
{ python3 -m pip install --user --upgrade "esptool==4.8.1" || python3 -m pip install --upgrade "esptool==4.8.1"; } >> pip_esptool.log 2>&1 \
  || { echo "== ESPTOOL INSTALL FAILED =="; cat pip_esptool.log; }
python3 -m esptool version || true

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
    if [ "$TC_SZ" -gt 50000000 ]; then TC_OK=1; break; fi
  fi
done
if [ "$TC_OK" != 1 ]; then
  echo "FATAL: toolchain download failed"; exit 3
fi
sudo tar -xf "$TC" -C /opt
export PATH="/opt/xtensa-esp32s3-elf/bin:$PATH"
xtensa-esp32s3-elf-gcc --version | head -1

# ---------------------------------------------------------------- [4] sync
echo "== [4/7] repo init + sync openvela ($MANIFEST_BRANCH) =="
mkdir -p "$BUILD"
cd "$BUILD"
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

# ----------------------------------------------- [5] staging BSP + app overlay
echo "== [5/7] install AI-VOX3 BSP (ai_vox3 board) + desktop_companion app =="
mkdir -p "$BUILD/apps/examples"
mkdir -p "$BUILD/vendor/espressif/boards/esp32s3"
# 证明 staging 源存在
for src in "$WS/staging/vendor_espressif/boards/esp32s3/ai_vox3" \
           "$WS/staging/apps/examples/desktop_companion" \
           "$WS/staging/vendor_espressif/boards/esp32s3/common" ; do
  if [ ! -e "$src" ]; then
    echo "== BSP FATAL: staging source missing: $src =="; exit 1
  fi
  echo "  staging source OK: $src"
done
cp -r "$WS/staging/vendor_espressif/boards/esp32s3/ai_vox3" \
      "$BUILD/vendor/espressif/boards/esp32s3/ai_vox3"
cp -r "$WS/staging/apps/examples/desktop_companion" \
      "$BUILD/apps/examples/desktop_companion"
cp -r "$WS/staging/vendor_espressif/boards/esp32s3/common" \
      "$BUILD/vendor/espressif/boards/esp32s3/common"
# 注册 app Kconfig（绝对路径，避免 $srctree 未设导致 olddefconfig 丢弃）
if grep -q 'desktop_companion' "$BUILD/apps/examples/Kconfig" 2>/dev/null; then
  echo "  desktop_companion already registered"
else
  echo "source \"$BUILD/apps/examples/desktop_companion/Kconfig\"" \
    >> "$BUILD/apps/examples/Kconfig"
fi
# vendor board-common 必须是真目录（tools/Config.mk 与 Unix.mk 都会解引用它）
mkdir -p "$BUILD/vendor/espressif/boards/esp32s3/common/scripts"
cp "$BUILD/nuttx/boards/xtensa/esp32s3/common/scripts/"*.ld \
   "$BUILD/vendor/espressif/boards/esp32s3/common/scripts/" \
   || { echo "== LDSCRIPT COPY FAILED =="; exit 1; }
mkdir -p "$BUILD/vendor/espressif/boards/esp32s3/common/configs"

# ----------------------------------------------- [5b] nuttx source patches
echo "== [5b] apply staging nuttx patches =="
PATCHDIR="$WS/staging/patches"
patch_failures=0
if [ -d "$PATCHDIR" ]; then
  shopt -s nullglob
  for p in "$PATCHDIR"/*.patch; do
    echo "== NUTTX PATCH: $(basename "$p") =="
    if (cd "$BUILD/nuttx" && patch -p1 --dry-run --forward < "$p" >/dev/null 2>&1); then
      (cd "$BUILD/nuttx" && patch -p1 --forward < "$p") \
        || { echo "== APPLY FAILED: $(basename "$p") =="; patch_failures=$((patch_failures+1)); }
      echo "  applied: $(basename "$p")"
    elif (cd "$BUILD/nuttx" && patch -p1 --dry-run --reverse < "$p" >/dev/null 2>&1); then
      echo "  already applied, skipping: $(basename "$p")"
    else
      echo "== NOT APPLICABLE: $(basename "$p") =="
      (cd "$BUILD/nuttx" && patch -p1 --dry-run --forward < "$p") 2>&1 | head -20 || true
      patch_failures=$((patch_failures+1))
    fi
  done
  shopt -u nullglob
fi
# 重拷已打补丁的 .ld 到 vendor common（BOARD_COMMON_DIR 指向它）
cp -f "$BUILD/nuttx/boards/xtensa/esp32s3/common/scripts/"*.ld \
      "$BUILD/vendor/espressif/boards/esp32s3/common/scripts/" \
  || { echo "== LD RE-COPY FAILED =="; exit 1; }
if [ "$patch_failures" != "0" ]; then
  echo "== BSP FATAL: $patch_failures patch(es) failed =="; exit 1
fi
# [V636] 双堆：Wi-Fi blob 需要内部 DRAM 堆，否则 wlan init -12
mkdir -p "$BUILD/nuttx/arch/xtensa/src/esp32s3"
if [ ! -e "$BUILD/nuttx/arch/xtensa/src/chip" ]; then
  ln -s esp32s3 "$BUILD/nuttx/arch/xtensa/src/chip"
fi

# ----------------------------------------------- [6] build ai_vox3 openvela
echo "== [6/7] build ai_vox3/configs/openvela =="
CFG="vendor/espressif/boards/esp32s3/ai_vox3/configs/openvela"
cd "$BUILD"
pip3 install --quiet kconfiglib || pip install --quiet kconfiglib || true
{ echo "PWD=$PWD"; ls -d nuttx apps build "$BUILD/vendor/espressif/boards/esp32s3/ai_vox3" 2>&1; } > build.log 2>&1 || true
source build/envsetup.sh >> build.log 2>&1 || { echo "== ENSETUP FAILED ==" >> build.log; exit 1; }
echo "ENSETUP OK" >> build.log
lunch "$CFG" >> build.log 2>&1 || { echo "== LUNCH FAILED ==" >> build.log; exit 1; }
echo "LUNCH OK" >> build.log
# 预先生成 .config（与 `m` 内部 configure 同命令），以便审计在 40 分钟编译前跑
if [ ! -f nuttx/.config ]; then
  nuttx/tools/configure.sh -e "$PWD/nuttx/../vendor/espressif/boards/esp32s3/ai_vox3/configs/openvela" \
    >> build.log 2>&1 || { echo "== PRE-CONFIGURE FAILED ==" >> build.log; exit 1; }
  echo "PRE-CONFIGURE OK" >> build.log
fi
# 双堆注入（CONFIG_MM_KERNEL_HEAP 非 Kconfig 符号，只能直接写 .config）
if ! grep -qx 'CONFIG_MM_KERNEL_HEAP=y' nuttx/.config; then
  echo 'CONFIG_MM_KERNEL_HEAP=y' >> nuttx/.config
  echo "[V636] injected CONFIG_MM_KERNEL_HEAP=y" >> build.log
fi
# SIMPLE_BOOT 闸门：必须 ROM 直启，否则 ESP32-S3 rev0.2 被 2 级 bootloader 拒
if grep -qE '^CONFIG_ESP32S3_APP_FORMAT_(LEGACY|MCUBOOT)=y' nuttx/.config; then
  echo "== AUDIT FAILED: APP_FORMAT selected -> SIMPLE_BOOT off ==" >> build.log; exit 1
fi
if ! grep -qE '^CONFIG_ESPRESSIF_SIMPLE_BOOT=y' nuttx/.config; then
  echo "== AUDIT FAILED: SIMPLE_BOOT not y ==" >> build.log; exit 1
fi
# 音频/外设 审计
for sym in CONFIG_AUDIO=y CONFIG_DRIVERS_AUDIO=y CONFIG_AUDIO_ES8311=y CONFIG_AUDIO_I2S=y \
           CONFIG_I2C=y CONFIG_I2S=y CONFIG_ESP32S3_I2C0=y CONFIG_ESP32S3_I2S=y \
           CONFIG_LCD_ST7789=y CONFIG_AI_VOX3_SERVO=y CONFIG_AI_VOX3_BUTTONS=y \
           CONFIG_ESP32S3_LEDC=y CONFIG_ESP32S3_USBSERIAL=y \
           CONFIG_INIT_ENTRYPOINT='"desktop_companion_main"'; do
  grep -qx "$sym" nuttx/.config || { echo "== AUDIT FAILED: $sym missing ==" >> build.log; exit 1; }
done
echo "CONFIG AUDIT OK" >> build.log
# esp-hal-3rdparty 预置（修正 spinlock 初始化）-> 避免 "invalid initializer"
{
  HALDIR="nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty"
  if [ ! -d "$HALDIR" ]; then
    rm -rf "$HALDIR"
    git init -q "$HALDIR"
    git -C "$HALDIR" remote add origin https://github.com/espressif/esp-hal-3rdparty.git
    git -C "$HALDIR" fetch -q --depth 1 origin 9fc713a95b1ff150dd0b0647e465d3c624056bb1 \
      || { rm -rf "$HALDIR"; git clone -q https://github.com/espressif/esp-hal-3rdparty.git "$HALDIR"; }
    git -C "$HALDIR" checkout -q FETCH_HEAD 2>/dev/null || git -C "$HALDIR" checkout -q 9fc713a95b1ff150dd0b0647e465d3c624056bb1
  fi
  grep -rl "LOCK_INITIALIZER_UNLOCKED" "$HALDIR/components" | xargs -r sed -i \
    's/^#define LOCK_INITIALIZER_UNLOCKED[[:space:]]*0[[:space:]]*$/#define LOCK_INITIALIZER_UNLOCKED SP_UNLOCKED/'
  echo "esp-hal preseeded + lock patched"
} >> build.log 2>&1 || { echo "== HAL PRESEED FAILED ==" >> build.log; exit 1; }
echo "HAL OK" >> build.log
# esptool 4.8.1 在 `m` 之前装好，避免被覆盖
python3 -m pip install --quiet --upgrade "esptool==4.8.1" \
  || pip install --quiet --upgrade "esptool==4.8.1" \
  || { echo "== ESPTOOL PREP FAILED =="; exit 1; }
echo "ESPTOOL OK" >> build.log

# 生成零地址段修复 + 校验脚本（SIMPLE_BOOT 下 ROM 不跳过 la=0 段 -> 必 brick）
cat > /tmp/segcheck.py <<'SEGCHECK_PY'
import os, struct, sys
IMAGE_MAGIC=0xE9; HEADER_LEN=24; SEG_HEADER_LEN=8; MIN_SEGMENTS=3
class ImageError(Exception): pass
def parse_image(blob):
    if len(blob) < HEADER_LEN+SEG_HEADER_LEN: raise ImageError("too small")
    if blob[0]!=IMAGE_MAGIC: raise ImageError("bad magic")
    segcount=blob[1]; entry=struct.unpack_from("<I",blob,4)[0]
    if segcount<1: raise ImageError("no segments")
    segs=[]; off=HEADER_LEN
    for i in range(segcount):
        if off+SEG_HEADER_LEN>len(blob): raise ImageError("truncated header")
        la,dn=struct.unpack_from("<II",blob,off)
        do=off+SEG_HEADER_LEN
        if do+dn>len(blob): raise ImageError("truncated data")
        segs.append({"i":i,"la":la,"dn":dn}); off=do+dn
    return {"segcount":segcount,"entry":entry,"segs":segs}
def check(path):
    if not os.path.isfile(path):
        print("FAIL: no file %s"%path); return 1
    blob=open(path,"rb").read()
    try: image=parse_image(blob)
    except ImageError as e:
        print("FAIL: cannot parse: %s"%e); return 1
    zero=[s for s in image["segs"] if s["la"]==0]
    print("SEGCOUNT=%d ZERO_LOAD_ADDR_SEGMENTS=%d"%(image["segcount"],len(zero)))
    for s in image["segs"]:
        print("  seg%d la=0x%08x len=0x%x"%(s["i"],s["la"],s["dn"]))
    if zero or image["segcount"]<MIN_SEGMENTS:
        print("ZERO-ADDR GATE FAIL: zero-laddr segs=%d segcount=%d"%(len(zero),image["segcount"]))
        return 1
    print("ZERO-ADDR GATE OK")
    return 0
if __name__=="__main__":
    rc=0
    for a in sys.argv[1:]:
        if check(a)!=0: rc=1
    sys.exit(rc)
SEGCHECK_PY
cat > /tmp/patch_zero_segs.py <<'PATCH_ZERO_SEGS'
import hashlib, struct, sys
# ESP32-S3: internal SRAM is dual-mapped.  DRAM 0x3FC88000 (data bus) and IRAM
# 0x40370000 (instruction bus) are two views of the SAME physical memory, so
#   iram_addr == dram_addr + DRAM2IRAM_ALIAS
# Verified from the last known-good build: it carries real .data at DRAM
# 0x3fc88000 (-> IRAM 0x40370000, the reserved window) while its IRAM code
# lives at 0x40380000, and it boots.
# Consequence: a relocated (formerly la=0) padding segment must clear BOTH the
# loaded DRAM segments in the DRAM window AND the loaded IRAM segments in the
# IRAM alias window, or it silently zeroes live code (-> IllegalInstruction).
DRAM_BASE=0x3FC88000; IRAM_BASE=0x40370000
DRAM2IRAM_ALIAS=0x6E8000
MARGIN_ALIAS=0x2000     # keep the alias clear above the last IRAM byte
MARGIN_DRAM=0x1000      # keep clear below the first DRAM byte
ROM_STACK_GUARD=0x3FCE0000   # ROM stack lives ~0x3FCEBxxx
def parse(path):
    data=bytearray(open(path,"rb").read())
    if len(data)<24 or data[0]!=0xE9: raise SystemExit("bad image")
    n=data[1]; off=24; entries=[]
    for i in range(n):
        if off+8>len(data): raise SystemExit("truncated %d"%i)
        la,ln=struct.unpack_from("<II",data,off)
        if off+8+ln>len(data): raise SystemExit("seg %d data truncated"%i)
        entries.append((i,off,la,ln)); off+=8+ln
    return data,entries
def is_dram(la): return DRAM_BASE<=la<0x3FCF0000
def is_iram(la): return IRAM_BASE<=la<0x403E0000
def main(src,dst):
    data,entries=parse(src)
    real=[r for r in entries if r[2]!=0]
    pads =[r for r in entries if r[2]==0]
    iram_max=max([r[2]+r[3] for r in real if is_iram(r[2])] or [IRAM_BASE])
    dram_min=min([r[2]      for r in real if is_dram(r[2])] or [0x3FCF0000])
    # highest DRAM-space address whose IRAM alias is still BELOW iram_max
    clash_ceil = iram_max - DRAM2IRAM_ALIAS + MARGIN_ALIAS
    ceil  = min(dram_min - MARGIN_DRAM, ROM_STACK_GUARD - MARGIN_DRAM)
    total = sum(r[3] for r in pads)
    start = ceil - total
    print("iram_max=0x%08x dram_min=0x%08x total_pad=0x%x"%(iram_max,dram_min,total))
    print("window: start 0x%08x end 0x%08x (must be >= 0x%08x)"%(start,ceil,clash_ceil))
    scratch=start
    if pads and start < clash_ceil:
        raise SystemExit("no safe scratch window: start 0x%x < 0x%x (would clobber IRAM code via alias)"%(start,clash_ceil))
    for i,off,la,ln in pads:
        payload=data[off+8:off+8+ln]
        if any(payload): raise SystemExit("seg%d la=0 not zeros"%i)
        struct.pack_into("<I",data,off,scratch)
        print("  seg%d 0x0->0x%08x len=0x%x (iram alias 0x%08x)"%(i,scratch,ln,scratch+DRAM2IRAM_ALIAS))
        scratch+=ln
    if data[23]&1:
        if len(data)<56: raise SystemExit("too small for digest")
        doff=len(data)-32; data[doff:]=hashlib.sha256(bytes(data[:doff])).digest()
        print("  sha256 refreshed")
    open(dst,"wb").write(bytes(data))
    d2,e2=parse(dst); bad=sum(1 for r in e2 if r[2]==0)
    print("verify segcount=%d zero=%d"%(len(e2),bad)); return 0 if bad==0 else 1
if __name__=="__main__":
    if len(sys.argv)!=3: sys.exit(2)
    sys.exit(main(sys.argv[1],sys.argv[2]))
PATCH_ZERO_SEGS

export VELA_USE_MAKEFILE=1
export VELA_EXTRA_FLAGS="-Wno-cpp -Wno-deprecated-declarations"
# `m` 是 build/envsetup.sh 定义的 shell 函数，必须在已 source envsetup 的
# 当前 shell 里直接调用；不能用 `timeout m ...` 包裹——timeout 是独立进程，
# 只认外部命令、看不到 shell 函数，会报 "No such file or directory"（第10次CI 就是这个坑）。
# 硬超时交给 build-vox3.yml 的 job timeout-minutes: 240 兜底。
set +e
echo "== [6b] starting 'm' build (envsetup shell function, current shell) =="
m -j"$(nproc)"
BUILD_RC=$?
if [ "$BUILD_RC" -ne 0 ]; then
  echo "== BUILD FAILED (rc=$BUILD_RC): see build log above for the real error ==" >> build.log
fi
echo "M-CHAIN rc=$BUILD_RC" >> build.log
ls -la nuttx/nuttx nuttx/nuttx.bin nuttx/nuttx.merged.bin >> build.log 2>&1
if [ -f nuttx/nuttx.bin ] && [ -f /tmp/patch_zero_segs.py ]; then
  # HARD GATE: a la=0 segment bricks the board under SIMPLE_BOOT (ROM does not
  # skip it).  If relocation or the audit fails, fail the job LOUDLY instead
  # of publishing an image that would brick the device on flash.
  python3 /tmp/patch_zero_segs.py nuttx/nuttx.bin nuttx/nuttx.bin >> build.log 2>&1 \
    || { echo "== ZERO-ADDR relocation FAILED (rc=$?) -- refusing to publish a bricking image ==" >> build.log; exit 1; }
  python3 /tmp/segcheck.py nuttx/nuttx.bin >> build.log 2>&1 \
    || { echo "== ZERO-ADDR GATE FAILED post-relocation -- refusing to publish ==" >> build.log; exit 1; }
fi
echo "$BUILD_RC" > build_rc.txt

# ---------------------------------------------------------------- [7] publish
# Collect every flashable image the `m` build produced into firmware/ so the
# workflow's upload-artifact (path: firmware/) succeeds, and so the local
# monitor (and flash_vox3.py) can pull a concrete binary.  For SIMPLE_BOOT the
# ROM loads NuttX directly, so nuttx.bin is the image flashed at 0x0; if a
# merged image exists it is preferred by the flash step.
echo "== [7/7] publish firmware + logs (BUILD_RC=$BUILD_RC) =="
FIRM="$WS/firmware"
mkdir -p "$FIRM"
for f in nuttx/nuttx nuttx/nuttx.bin nuttx/nuttx.merged.bin nuttx/nuttx.hex; do
  if [ -f "$f" ]; then
    cp -v "$f" "$FIRM/" >> build.log 2>&1
    echo "  published: $f ($(stat -c%s "$f" 2>/dev/null) bytes)"
  fi
done
{
  echo "BUILD_RC=$BUILD_RC"
  echo "DATE=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "BOARD=ai_vox3"
  echo "APP=desktop_companion (mimo endpoint: token-plan-cn.xiaomimimo.com)"
  echo "ARTIFACTS:"; ls -la "$FIRM" 2>/dev/null
} > "$FIRM/manifest.txt"
# Push firmware to the vox3-fw branch so the local monitor can fetch it
# (GITHUB_TOKEN + GITHUB_REPOSITORY are injected by build-vox3.yml).
if [ -n "${GITHUB_TOKEN:-}" ] && [ -n "${GITHUB_REPOSITORY:-}" ]; then
  FW_REPO=$(mktemp -d)
  git init -q "$FW_REPO"
  git -C "$FW_REPO" remote add origin \
    "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
  git -C "$FW_REPO" checkout --quiet --orphan vox3-fw 2>/dev/null \
    || git -C "$FW_REPO" checkout -b vox3-fw
  cp -r "$FIRM"/* "$FW_REPO"/
  ( cd "$FW_REPO" \
    && git add -A \
    && git commit -m "vox3 firmware $(date -u '+%H:%M:%SZ') rc=$BUILD_RC" \
    && git push --force origin vox3-fw ) >> build.log 2>&1 \
    || echo "== vox3-fw push failed (non-fatal; artifact upload still runs) ==" >> build.log
fi
echo "== publish done =="
# Exit with the build's return code so the workflow's failure() path fires
# (publish failure marker + log upload) when the build itself failed.
exit "$BUILD_RC"
