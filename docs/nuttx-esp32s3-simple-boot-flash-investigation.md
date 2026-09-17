# NuttX / OpenVela ESP32-S3 `CONFIG_ESPRESSIF_SIMPLE_BOOT` 官方烧录方式调查报告

> 纯调查，未改任何代码。所有结论均来自本机权威源码，逐条给出 `文件:行号` 证据。
> 参考源码根目录：`C:\Users\admin\Desktop\zzq\toolchain\openvela-ref-nuttx\`
> （本机 vendored 的 OpenVela/NuttX 参考树，含完整 `arch/xtensa/src/esp32s3/`、`tools/esp32s3/`、`boards/xtensa/esp32s3/`）

---

## 结论速览（TL;DR）

| 问题 | 结论 |
|------|------|
| **1. `nuttx.bin` 烧到哪个偏移？** | **`0x0`**。SIMPLE_BOOT 分支硬编码 `APP_OFFSET := 0x0000`。**你们的偏移没写错。** |
| **2. 还需要烧别的文件吗？** | **不需要**。不需要 `bootloader-esp32s3.bin`，不需要 `partition-table.bin`。只需要 `0x0 nuttx.bin` 一个文件。 |
| **3. SIMPLE_BOOT 语义** | **ROM 上电 → 直接进入 `nuttx.bin`（镜像自身含一段 IRAM 引导代码），由这段代码自己初始化硬件 + 建 MMU 映射 XIP**。**没有独立的第 2 级 bootloader**；但**不是 ROM 直接执行 XIP 代码**——XIP 映射是镜像里的 `__start()` 做的。 |
| **4. 烧 0x0 + 串口 0 字节最可能的原因** | 烧录**方式/偏移本身基本没错**。0 字节输出更可能是:(a) **控制台设备不对**（`USBSERIAL` 报文走 USB-CDC,而串口工具连的是 UART0/COM 口,或反着来）；(b) **`nuttx.bin` 不是 SIMPLE_BOOT 镜像**（`--ram-only-header` 缺失,即构建时 SIMPLE_BOOT 实际未生效）；(c) 复位后 USB-CDC 需要重新枚举而工具没重连。**需按第 5 节的命令逐项排除。** |

---

## 1. 官方烧录偏移：`0x0`（证据确凿）

**文件：`tools/esp32s3/Config.mk`**

行 105-112（SIMPLE_BOOT 分支）：
```make
else
#   CONFIG_ESPRESSIF_SIMPLE_BOOT

	APP_OFFSET     := 0x0000
	APP_IMAGE      := nuttx.bin
	FLASH_APP      := $(APP_OFFSET) $(APP_IMAGE)
	ESPTOOL_BINDIR := .
endif
```

行 114 把应用镜像并入烧录列表：
```make
ESPTOOL_BINS += $(FLASH_APP)
```

行 69-84（关键：`ESPTOOL_BINS` 的完整构成）——注意只有 `LEGACY` 和 `MCUBOOT` 两条分支会**往里加 bootloader / partition**，SIMPLE_BOOT 走的是最后的 `else`，**什么都不加**：
```make
ifdef ESPTOOL_BINDIR
	ifeq ($(CONFIG_ESP32S3_APP_FORMAT_LEGACY),y)
		BL_OFFSET       := 0x0
		PT_OFFSET       := $(CONFIG_ESP32S3_PARTITION_TABLE_OFFSET)
		BOOTLOADER      := $(ESPTOOL_BINDIR)/bootloader-esp32s3.bin
		PARTITION_TABLE := $(ESPTOOL_BINDIR)/partition-table-esp32s3.bin
		FLASH_BL        := $(BL_OFFSET) $(BOOTLOADER)
		FLASH_PT        := $(PT_OFFSET) $(PARTITION_TABLE)
		ESPTOOL_BINS    := $(FLASH_BL) $(FLASH_PT)
	else ifeq ($(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT),y)
		BL_OFFSET       := 0x0000
		BOOTLOADER      := $(ESPTOOL_BINDIR)/mcuboot-esp32s3.bin
		FLASH_BL        := $(BL_OFFSET) $(BOOTLOADER)
		ESPTOOL_BINS    := $(FLASH_BL)
	endif
endif
```
SIMPLE_BOOT 命中不了上面任何一个 `ifeq`，所以 `ESPTOOL_BINS` 初始为空 → 行 114 只追加 `0x0000 nuttx.bin`。

**最终 `make flash` 执行的命令**（`tools/esp32s3/Config.mk` 行 218-226）：
```make
$(eval ESPTOOL_OPTS := -c esp32s3 -p $(ESPTOOL_PORT) -b $(ESPTOOL_BAUD) ...)
esptool.py $(ESPTOOL_OPTS) write_flash $(ESPTOOL_WRITEFLASH_OPTS) $(ESPTOOL_BINS)
```
代入 SIMPLE_BOOT 后即：`esptool.py -c esp32s3 -p <port> -b 921600 write_flash -fs 16MB -fm dio -ff 40m 0x0000 nuttx.bin`

> ✅ **偏移 `0x0` 是官方值，只有一个文件 `nuttx.bin`。**

---

## 2. 是否需要额外的 bootloader / partition table？→ 不需要

**文件：`arch/xtensa/src/esp32s3/Bootloader.mk`**

行 89-91（SIMPLE_BOOT 的 `bootloader` target 是个空操作）：
```make
ifeq ($(CONFIG_ESPRESSIF_SIMPLE_BOOT),y)
bootloader:
	$(Q) echo "Using direct bootloader to boot NuttX."
```
对比 `ESP32S3_APP_FORMAT_LEGACY`（行 119-134）才会 `git clone esp-nuttx-bootloader`、`build_idfboot.sh` 生成并拷贝 `bootloader-esp32s3.bin` + `partition-table-esp32s3.bin`；`MCUBOOT`（行 93-113）才生成 `mcuboot-esp32s3.bin`。

**SIMPLE_BOOT 三者都不生成**——所以：
- ❌ 不生成、也不需要 `bootloader-esp32s3.bin`
- ❌ 不生成、也不需要 `partition-table.bin`
- ❌ `nuttx.merged.bin` 只有 `CONFIG_ESP32S3_MERGE_BINS=y` 时才由 `MERGEBIN`（`tools/esp32s3/Config.mk` 行 130-150）生成，且 SIMPLE_BOOT 下它合并的也只是 `nuttx.bin` 一个文件（无意义）

> ⚠️ **团队 `staging/README.md` 第 104-114 行给出的烧录命令是错的**，它用的是 IDF 传统布局：
> ```bat
> write_flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 nuttx.bin   ← 这是 LEGACY 布局
> ```
> 这条命令对应的是 `ESP32S3_APP_FORMAT_LEGACY=y`（`Config.mk` 行 70-77 + 行 86-89：`KERNEL_OFFSET=0x10000`、`PT_OFFSET=0x8000`），而我们的配置是 SIMPLE_BOOT，**两套布局互斥**。若真按 README 那样烧，会把 `nuttx.bin` 放到 `0x10000`，那才是"偏移错了"——但你们说的"烧 0x0"其实是对的。

---

## 3. SIMPLE_BOOT 的准确语义：**ROM 直接引导，但 XIP 映射由镜像内代码完成**

### 3.1 Kconfig 定义
**文件：`arch/xtensa/src/esp32s3/Kconfig` 行 2663-2683**
```kconfig
config ESPRESSIF_SIMPLE_BOOT
	bool
	depends on !ESP32S3_APP_FORMAT_MCUBOOT
	depends on !ESP32S3_APP_FORMAT_LEGACY
	default y
```
三者关系：`MCUBOOT` / `LEGACY` 各自是一个显式的 "2 级引导" 格式；两者都不选时，**隐藏符号 `ESPRESSIF_SIMPLE_BOOT` 自动 `default y`**，即"无 2 级引导、ROM 直接进 NuttX"。三者**互斥**。

> 这也解释了 defconfig 里那条重要注释（`staging/.../openvela/defconfig` 行 42-46, 80-83）：一旦有人加回 `CONFIG_ESP32S3_APP_FORMAT_LEGACY=y`，它的 `select/depends` 链条会把 SIMPLE_BOOT 顶掉，切到 IDF 引导。

### 3.2 镜像格式：`--ram-only-header` 从哪来、为什么
**文件：`tools/esp32s3/Config.mk` 行 181-192**
```make
define MKIMAGE
	...
	$(eval ELF2IMAGE_OPTS := $(if $(CONFIG_ESPRESSIF_SIMPLE_BOOT),--ram-only-header) -fs $(FLASH_SIZE) -fm $(FLASH_MODE) -ff $(FLASH_FREQ))
	esptool.py -c esp32s3 elf2image $(ELF2IMAGE_OPTS) -o nuttx.bin nuttx
```
`--ram-only-header` **仅当 `CONFIG_ESPRESSIF_SIMPLE_BOOT=y` 时附加**。它让 `elf2image` 只把**RAM 段（IRAM/DRAM）**写进镜像头可见段落，**XIP 段（IROM/DROM）不进头部描述**，也**不追加 SHA256 digest**（这正是你们看到的输出 `Image has only RAM segments visible. ROM segments are hidden and SHA256 digest is not appended.`）。

### 3.3 为什么这样能跑 —— **谁把 0x42000000 的 XIP 段映射起来的？**

答案在**镜像自身的 `__start()`**，不在 ROM：

**文件：`arch/xtensa/src/esp32s3/esp32s3_start.c` 行 461-489**
```c
noinstrument_function void IRAM_ATTR __start(void)
{
#if defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT) || defined(CONFIG_ESPRESSIF_SIMPLE_BOOT)
  size_t partition_offset = PRIMARY_SLOT_OFFSET;                 // SIMPLE_BOOT 下 = 0
  uint32_t app_irom_start = partition_offset + (uint32_t)_image_irom_lma;
  uint32_t app_irom_vaddr = (uint32_t)_image_irom_vma;           // 0x42000000 那边
  uint32_t app_drom_start = partition_offset + (uint32_t)_image_drom_lma;
  ...
#  ifdef CONFIG_ESPRESSIF_SIMPLE_BOOT
  __asm__ __volatile__ ("wsr %0, vecbase\n"::"r" (_init_start));
  if (bootloader_init() != 0) { ... }         // ← 第一步：硬件/时钟/flash 初始化
#  endif
  if (map_rom_segments(app_drom_start, app_drom_vaddr, app_drom_size,
                       app_irom_start, app_irom_vaddr, app_irom_size) != 0) { ... }  // ← 第二步：建 MMU + 使能 cache
```
- 行 461 `IRAM_ATTR`：`__start` 本身链接进 **IRAM**，所以 ROM 只需把它拷进 IRAM 就能执行（**不需要 XIP**）。入口地址在 `esp32s3_start.c` 行 145-154：
  ```c
  IRAM_ATTR noreturn_function void __start(void);
  HDR_ATTR static void (*_entry_point)(void) = __start;   // 放进 .entry_addr 段
  ```
- 行 476：**SIMPLE_BOOT 下镜像自己调用 `bootloader_init()`**（来自 esp-hal 的 bootloader 组件）完成时钟/flash/cache 前置初始化。
- 行 483：调用 `map_rom_segments()`。

**`map_rom_segments()` 的实现**：`arch/xtensa/src/common/espressif/esp_loader.c` 行 145-342。
- 行 161-261（`CONFIG_ESPRESSIF_SIMPLE_BOOT` 分支）：**自己解析 flash 里的镜像头 + 段头**，从中定位 `IROM`/`DROM` 段在 flash 的物理偏移（行 227-241），因为 `--ram-only-header` 把这些段从头部隐藏了，所以必须靠 `_image_irom_vma/_image_irom_lma` 符号（行 99-105）回写真实 LMA：
  ```c
  if (IS_IROM(segment_hdr.load_addr) && segment_hdr.load_addr == (uint32_t)_image_irom_vma) {
      app_irom_start = offset + sizeof(esp_image_segment_header_t);   // 真实 flash 物理偏移
      ...
  }
  ```
- 行 286-318（`#else` / !ESP32 分支，即 S3 走这条）：**`mmu_hal_map_region()` 把 flash 物理区映射到 `0x42000000` / `0x3C000000` 虚拟窗口，再 `cache_hal_enable()` 打开 cache**。这就是 XIP 映射的建立者。

**链接脚本**证明 IROM/DROM 段确实锚在 `0x42000000`（XIP 窗口）：
**文件：`boards/xtensa/esp32s3/common/scripts/esp32s3_sections.ld`**
- 行 532-534：`_image_irom_vma/lma/size` 绑定到 `.flash.text`（> irom0_0_seg，即 0x42000000 窗口）
- 行 441-443：`_image_drom_vma/lma/size` 绑定到 `.flash.rodata`（> drom0_0_seg，0x3C000000 窗口）
- 行 107 / 530 / 587：`.iram0.vectors ... AT>ROM`、`.flash.rodata ... AT>ROM`、`.flash.text AT>ROM`——即 RAM 段有 flash LMA（会被 `elf2image --ram-only-header` 写进镜像头），而 XIP 段是 `AT>ROM`（物理地在 flash，运行期靠 MMU 映射）。

（对比：MCUBOOT 走的是 `esp32s3_sections.ld` 行 33-64 的 `.metadata` 段——`LONG(ADDR(.iram0.vectors))` / `LONG(LOADADDR(...))` 等，把 IROM/DRAM 的位置**显式写进镜像头**。SIMPLE_BOOT 没有 `.metadata`，改由上面的 `--ram-only-header` + 运行时回填。）

**一句话**：SIMPLE_BOOT = **ROM 上电 → 加载镜像里那段 IRAM 引导（`__start`）→ `__start` 自己 `bootloader_init()` + `map_rom_segments()` 建 MMU/cache → 跳入 XIP `0x42000000` 执行 NuttX 主体**。不是"ROM 直接执行 XIP"，但也**没有第二个独立的 bootloader 二进制**。

---

## 4. "烧 0x0 → 串口 0 字节" 最可能的原因（按概率排序）

**先排除**：偏移 `0x0` 本身**不是**错误（第 1 节）。所以问题不在偏移。

1. **控制台设备/波特率不对（最可能）**
   `staging/.../openvela/defconfig` 行 105-106 明确 `CONFIG_ESP32S3_USBSERIAL=y` + `CONFIG_UART0_SERIAL_CONSOLE=n`：控制台走 **USB-Serial/JTAG**（VID:PID **303A:1001**），不是 UART0。如果串口工具连的是 UART0 的 COM 口，或者波特率没对上 USB-CDC 侧（应 115200），就会"0 字节"。而 USB-CDC 在硬复位后会**重新枚举**，工具若没自动重连也会表现为 0 字节。

2. **烧进去的 `nuttx.bin` 并非 SIMPLE_BOOT 镜像**
   即构建时 `CONFIG_ESPRESSIF_SIMPLE_BOOT` 实际没生效（例如被 LEGACY 顶掉）。验证方法：看 `elf2image` 输出是否含 `Image has only RAM segments visible...`。若**没有**这句，说明烧进去的是普通/legacy 镜像，ROM 找不到合法入口 → 无输出。你们已确认过这句存在，则此项排除。

3. **黑屏 + 0 字节 = 引导循环（bootloop），非纯显示故障**
   `staging/.../openvela/defconfig` 行 270-278 的注释记录了历史现象：早期用 IDF 引导时 "No bootable app partitions" → 循环复位。若当前 `nuttx.bin` 构建链在别处仍残留 LEGACY 路径，也会重现。

4. **bootloader_init() 内部复位/挂死**
   esp_hal 的 `bootloader_init()`（`esp32s3_start.c` 行 476）失败会让 `__start` 停在 `while(true)`（行 477-480），此时 `ets_printf("Hardware init failed...")` **走的是 ROM UART**，可能不在你监听的设备上 → 表现为 0 字节。

> **注意**：这几条都指向"构建/烧录之外的环节"，**不是偏移错误**。

---

## 5. 下一步要执行的精确命令

### 5.1 先确认烧进 flash 的镜像头到底是哪种格式（离线，最省事）
在构建机 nuttx 目录：
```bash
# 看 elf2image 是否用了 --ram-only-header（SIMPLE_BOOT 特有）
grep -n "ram-only-header" <构建日志>
# 或直接看镜像头前 1 字节 magic（SIMPLE_BOOT/普通 = 0xE9；且无 append digest）
python3 - <<'PY'
b=open("nuttx.bin","rb").read(48)
print("magic=0x%02x segments=%d mode=%d" % (b[0], b[1], b[2]))
print("entry_addr=0x%08x" % int.from_bytes(b[4:8],'little'))
PY
```
- `magic == 0xE9` 且能看到后面段落里第一个 segment 的 `load_addr` 落在 IRAM（`0x40000000+`）→ 是 SIMPLE_BOOT 镜像 ✅

### 5.2 用官方等价的单文件命令烧（SIMPLE_BOOT 唯一正确形式）
```bash
cd nuttx
make flash ESPTOOL_PORT=/dev/ttyACM0 ESPTOOL_BINDIR=./
# 展开后等价于：
esptool.py -c esp32s3 -p /dev/ttyACM0 -b 921600 \
  write_flash -fs 16MB -fm dio -ff 40m \
  0x0000 nuttx.bin
```
**不要**再烧 bootloader / partition-table / 也不要把 nuttx.bin 放 0x10000。

### 5.3 抓控制台（关键：认准 USB-Serial/JTAG，不是 UART0）
```bash
# Linux：使用 USB CDC（VID:PID 303A:1001）那个 /dev/ttyACM*
esptool.py -c esp32s3 -p /dev/ttyACM0 --before default_reset --after hard_reset run
# 同时用 115200 打开同名 ACM 口
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```
- Windows：设备管理器里找带 **303A:1001** 的 COM 口；`esptool.py ... write_flash` 后用 115200 打开**同一个** COM 口。
- 若硬复位后 USB-CDC 重新枚举，**先关串口工具、复位、再重开**。

### 5.4 若仍 0 字节，读 ROM 自己的日志（判断是"没引导"还是"引导后死"）
```bash
python3 - <<'PY'
import serial, time
s=serial.Serial("COMx", 115200, timeout=1)   # 连 303A:1001 那个口
s.setDTR(False); s.setRTS(True); time.sleep(0.1)   # 拉低 EN 复位
s.setRTS(False); time.sleep(3)
print(repr(s.read(4096)))
PY
```
- 若这里有 **ROM 的 `rst:0x.. boot:0x..` 字样**（ROM UART，波特率 115200）→ 芯片在跑，问题在控制台映射；
- 若**连 ROM 字样都没有** → 镜像入口/MAGIC 不对或 flash 内容损坏，回头查 5.1。

---

## 6. 本次调查的覆盖范围（查过的地方）

**本机源码（`C:\Users\admin\Desktop\zzq\toolchain\openvela-ref-nuttx\`，权威参考树）：**
- ✅ `tools/esp32s3/Config.mk`（烧录 offset / MKIMAGE / MERGEBIN / FLASH 定义）— 全部读到
- ✅ `arch/xtensa/src/esp32s3/Bootloader.mk` — 全部读到
- ✅ `arch/xtensa/src/esp32s3/Kconfig`（行 2661-2783 `ESPRESSIF_SIMPLE_BOOT` / 格式 / offset 定义）
- ✅ `arch/xtensa/src/esp32s3/esp32s3_start.c`（`__start` / `PRIMARY_SLOT_OFFSET` / entry）
- ✅ `arch/xtensa/src/common/espressif/esp_loader.c`（`map_rom_segments` 全文）
- ✅ `boards/xtensa/esp32s3/common/scripts/esp32s3_sections.ld`（段/符号布局）
- ✅ `tools/espressif/Config.mk`、`tools/esp32s2/Config.mk`、`tools/esp32/Config.mk`（交叉印证 `--ram-only-header` 用法）

**本机 vendored 项目 BSP（`C:\Users\admin\Desktop\zzq\deliverables\ai-vox3\openvela\contest_repo\staging\`）：**
- ✅ `vendor_espressif/boards/esp32s3/ai_vox3/configs/openvela/defconfig`（确认 SIMPLE_BOOT 生效路径、`USBSERIAL` 控制台）
- ✅ `vendor_espressif/boards/esp32s3/ai_vox3/scripts/Make.defs`（确认 `esp32s3_sections.ld` 选择逻辑）
- ✅ `vendor_espressif/boards/esp32s3/ai_vox3/Kconfig`、`README.md`（发现 README §4 的烧录命令是 LEGACY 布局，与本配置矛盾）

**未能查到的：**
- ❌ `Documentation/platforms/xtensa/esp32s3/index.rst` —— 本机参考树**未包含** `Documentation/` 目录（Glob 无命中），故未引用官方 rst 原文；结论全部由源码本身支撑。
- ❌ esp-hal-3rdparty 的 `bootloader_init.h` / `CONFIG_BOOTLOADER_OFFSET_IN_FLASH` 宏定义本体 —— 该头文件在 **esp-hal-3rdparty 子模块**内，本机参考树未 vendored（Glob 无命中）。由上下文（`esp32s3_start.c:88` 对 SIMPLE_BOOT 强制 `PRIMARY_SLOT_OFFSET=0`）可确定其取值与 `0x0` 一致，但**未直接读到该宏的 `#define` 行**，如实标注。
- ❌ 网络检索 —— 因本地已取得**完整权威源码**，无需网络二次确认；上述所有行号均为本机源码实测，未编造。

---

## 7. 给工程师的一句话交接

> 偏移 **`0x0` 正确、单文件 `nuttx.bin` 正确、不需要 bootloader/partition table**。把注意力从"偏移"移到**控制台设备（USB-CDC 303A:1001 vs UART0）**和**构建产物是否真是 SIMPLE_BOOT 镜像**上。另外 **README.md §4 的 `0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 nuttx.bin` 是错的（那是 LEGACY 布局），必须改掉**，否则会误导后来者。
