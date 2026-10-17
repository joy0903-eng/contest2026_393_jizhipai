/****************************************************************************
 * src/board.c
 *
 * Board-specific initialization for the emakefun AI-VOX3 (ESP32-S3-R8).
 *
 * This file wires up all on-board peripherals by delegating to the
 * per-peripheral modules in ai_vox3_*.c. It follows the standard NuttX
 * ESP32-S3 board bring-up pattern (esp_board_initialize + board_*
 * hooks consumed by the frameworks).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/arch.h>
#include <nuttx/kthread.h>
#include <nuttx/lcd/lcd_dev.h>

#include <stdint.h>
#include <stdlib.h>

#include <debug.h>
#include <syslog.h>

#include <arch/board/board.h>

#ifdef CONFIG_BOARDCTL
#include <sys/boardctl.h>
#endif

#ifdef CONFIG_ESP32S3_WIFI
#include "esp32s3_wlan.h"
#endif

#ifdef CONFIG_ESP32S3_SPIRAM
#include "esp32s3_spiram.h"
#endif

void esp32s3_board_initialize(void);
void xtensa_netinitialize(void);

extern int ets_printf(const char *fmt, ...);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_board_initialize
 *
 * Description:
 *   Mandatory board hook called unconditionally by __esp32s3_start()
 *   (arch/xtensa/src/esp32s3/esp32s3_start.c:423) before nx_start().
 *   Follows the official esp32s3-eye / esp32s3-box pattern: an empty
 *   function body.  Real peripheral bring-up happens later in
 *   esp_board_initialize() below, where OS services are available.
 *
 ****************************************************************************/

void esp32s3_board_initialize(void)
{
  /* Intentionally empty (official pattern). */
}

/****************************************************************************
 * Name: xtensa_netinitialize
 *
 * Description:
 *   Mandatory arch hook called by up_initialize()
 *   (arch/xtensa/src/common/xtensa_initialize.c).  The prototype only
 *   exists when CONFIG_NET=y && !CONFIG_NETDEV_LATEINIT
 *   (arch/xtensa/src/common/xtensa.h); otherwise the call is #defined
 *   away, so this definition is gated the same way.  The WLAN station
 *   netdev is registered from esp_board_initialize() instead, which runs
 *   through BOARDIOC_INIT before NSH performs its netinit.
 *
 ****************************************************************************/

#if defined(CONFIG_NET) && !defined(CONFIG_NETDEV_LATEINIT)
void xtensa_netinitialize(void)
{
  /* Intentionally empty: netdev registration happens in
   * esp_board_initialize() via esp32s3_wlan_sta_initialize(). */
}
#endif

/****************************************************************************
 * Name: esp_board_initialize
 *
 * Description:
 *   Initialize and configure all on-board peripherals. Called once during
 *   early boot (from up_initialize / boardctrl).
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on peripheral failure.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: periph_init_entry
 *
 * Description:
 *   Background kernel thread that brings up the non-essential peripherals
 *   (audio / servo / ws2812 / buttons / sd).  Run asynchronously so that a
 *   hang or failure here can NEVER block board_app_initialize() and delay the
 *   UI bring-up -- e.g. the RMT/ws2812 init was observed to hang and would
 *   otherwise stall the whole bring-up, leaving the screen dark.  ui_init()
 *   runs as soon as esp_board_initialize() returns to the caller.
 *
 ****************************************************************************/

static int periph_init_entry(int argc, char *argv[])
{
  int ret;

  ets_printf("[BSP] periph thread start\n");

#ifdef CONFIG_AUDIO
  ets_printf("[BSP] -> ai_vox3_audio_initialize\n");
  ret = ai_vox3_audio_initialize();
  ets_printf("[BSP] <- ai_vox3_audio_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR audio init: %d\n", ret);
    }
#endif

#ifdef CONFIG_AI_VOX3_SERVO
  ets_printf("[BSP] -> ai_vox3_servo_initialize\n");
  ret = ai_vox3_servo_initialize();
  ets_printf("[BSP] <- ai_vox3_servo_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR servo init: %d\n", ret);
    }
#endif

#ifdef CONFIG_AI_VOX3_WS2812
  ets_printf("[BSP] -> ai_vox3_ws2812_initialize\n");
  ret = ai_vox3_ws2812_initialize();
  ets_printf("[BSP] <- ai_vox3_ws2812_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR ws2812 init: %d\n", ret);
    }
#endif

#ifdef CONFIG_AI_VOX3_BUTTONS
  ets_printf("[BSP] -> ai_vox3_buttons_initialize\n");
  ret = ai_vox3_buttons_initialize();
  ets_printf("[BSP] <- ai_vox3_buttons_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR buttons init: %d\n", ret);
    }
#endif

#ifdef CONFIG_MMCSD
  ets_printf("[BSP] -> ai_vox3_sd_initialize\n");
  ret = ai_vox3_sd_initialize();
  ets_printf("[BSP] <- ai_vox3_sd_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR sd init: %d\n", ret);
    }
#endif

  ets_printf("[BSP] periph thread done\n");
  return 0;
}

/****************************************************************************
 * Name: bsp_dump_heap
 *
 * Description:
 *   Report, at O(1) cost only, the two facts that decide whether
 *   esp32s3_wlan_sta_initialize() succeeds or dies with -12 (-ENOMEM):
 *   whether the PSRAM hardware came up, and whether the allocator was
 *   actually given the extra region.
 *
 *   Why not mallinfo(): the first version of this helper called it and
 *   WEDGED THE BOARD -- run#134 booted to "[BSP] esp_board_initialize
 *   enter" and then went silent forever (120 s of capture, still 79 lines).
 *   mallinfo()/kmm_mallinfo() walks the whole heap chunk list, and with the
 *   8 MB PSRAM region now really in the heap that walk is far too expensive
 *   to run from board init (and on this config it appears not to terminate
 *   at all).  Never call it from early boot again.
 *
 *   The decisive signals, all constant-time:
 *     - esp_spiram_is_initialized()        : the 8 MB hardware is up
 *     - esp_spiram_allocable_vaddr_*()      : the mapped window the
 *                                             allocator would be handed
 *     - CONFIG_MM_REGIONS                   : >1 means xtensa_add_region()
 *                                             is compiled in and DID run
 *                                             (arch/xtensa/src/esp32s3/
 *                                             esp32s3_allocateheap.c:201,
 *                                             gated on #if
 *                                             CONFIG_MM_REGIONS > 1; the
 *                                             #else branch is an empty
 *                                             macro per xtensa.h:252)
 *   PSRAM initialised + MM_REGIONS>1 together mean the heap really does
 *   contain the extra ~8 MB region, which is what Wi-Fi needs.
 *
 *   Output goes through ets_printf (direct ROM console) and not syslog(),
 *   because this runs before/whether-or-not the syslog channel is usable.
 *
 ****************************************************************************/

static void bsp_dump_heap(const char *tag)
{
#ifdef CONFIG_ESP32S3_SPIRAM
  ets_printf("[BSP] psram[%s]: inited=%d vaddr=0x%08x..0x%08x span=%d "
             "mm_regions=%d\n",
             tag, (int)esp_spiram_is_initialized(),
             (unsigned int)esp_spiram_allocable_vaddr_start(),
             (unsigned int)esp_spiram_allocable_vaddr_end(),
             (int)(esp_spiram_allocable_vaddr_end() -
                   esp_spiram_allocable_vaddr_start()),
             (int)CONFIG_MM_REGIONS);
#else
  ets_printf("[BSP] psram[%s]: DISABLED (CONFIG_ESP32S3_SPIRAM not set) "
             "mm_regions=%d\n", tag, (int)CONFIG_MM_REGIONS);
#endif
}

/****************************************************************************
 * Name: bsp_probe_alloc
 *
 * Description:
 *   Probe what the allocator can actually hand out right now, at O(1)-ish
 *   cost.  NEVER call mallinfo()/kmm_mallinfo() here -- that walks the whole
 *   chunk list and wedged the board on run#134 (see bsp_dump_heap).
 *
 *   Two probes, because the two heap regions behave differently:
 *     big   (1 MB) -- only satisfiable from the 8 MB PSRAM region.  If this
 *                     fails the PSRAM is NOT in the heap, whatever the
 *                     config says.
 *     small (4 KB) -- satisfiable from internal SRAM.  If THIS fails the
 *                     internal heap is exhausted, which is exactly the
 *                     condition Wi-Fi's DMA buffers hit.
 *
 *   Each probe is freed immediately, so this perturbs nothing.
 *
 ****************************************************************************/

/* Did kmm_malloc() serve this block from the PSRAM heap region?  Mirrors the
 * esp32s3_ptr_extram() test used by esp_malloc_internal() and friends, but
 * expressed through the PSRAM allocable window (which is exactly the range
 * the extra heap region covers).  Anything outside it is internal DRAM.
 */

static int bsp_ptr_in_psram(void *p)
{
  return (p != NULL &&
          (uintptr_t)p >= (uintptr_t)esp_spiram_allocable_vaddr_start() &&
          (uintptr_t)p <  (uintptr_t)esp_spiram_allocable_vaddr_end());
}

/* Ladder probe: replicate the esp_*_internal() allocation semantics
 * (kmm_malloc + reject-PSRAM) so we can see the largest contiguous
 * INTERNAL-DRAM block available at this point in boot.  The Wi-Fi driver
 * needs several such blocks during esp_wifi_init(); if the ladder tops out
 * below what Wi-Fi asks for, that is the -12 root cause.
 */

static void bsp_probe_ladder(const char *tag)
{
  static const uint32_t sizes[] =
    {
      4096, 16384, 32768, 65536, 131072, 262144
    };

  unsigned i;

  for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
      void *p = malloc(sizes[i]);
      const char *where;

      if (p == NULL)
        {
          where = "FAIL";
        }
      else if (bsp_ptr_in_psram(p))
        {
          where = "PSRAM";
        }
      else
        {
          where = "INT";
        }

      ets_printf("[BSP] ladder[%s]: %6u -> %s\n", tag, sizes[i], where);

      if (p != NULL)
        {
          free(p);
        }
    }
}

static void bsp_probe_alloc(const char *tag)
{
  void *big   = malloc(1024u * 1024u);
  void *small = malloc(4096);

  ets_printf("[BSP] alloc[%s]: 1MB=%s 4KB=%s\n", tag,
             big != NULL ? "OK" : "FAIL",
             small != NULL ? "OK" : "FAIL");

  if (big != NULL)
    {
      free(big);
    }

  if (small != NULL)
    {
      free(small);
    }

  bsp_probe_ladder(tag);
}

int esp_board_initialize(void)
{
  int ret = OK;

  ets_printf("[BSP] esp_board_initialize enter\n");

#if defined(CONFIG_ESP32S3_WIFI) && defined(ESP32S3_WLAN_HAS_STA)
  /* Snapshot the heap right before Wi-Fi init: `arena` tells us whether the
   * PSRAM is in the heap at all (see bsp_dump_heap above). */

  bsp_dump_heap("pre-wlan");
  bsp_probe_alloc("pre-wlan");

  ets_printf("[BSP] -> wlan_sta_initialize\n");
  ret = esp32s3_wlan_sta_initialize();
  ets_printf("[BSP] <- wlan_sta_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR wlan sta init: %d\n", ret);
    }

  /* And after, so the Wi-Fi driver's actual footprint is visible. */

  bsp_dump_heap("post-wlan");
  bsp_probe_alloc("post-wlan");
#endif

  ets_printf("[BSP] past wlan block\n");

#ifdef CONFIG_LCD
  /* This NuttX's lcddev_register() only calls board_lcd_getdev() -- it does
   * NOT call board_lcd_initialize().  So we must initialize the LCD ourselves
   * (board_lcd_initialize() -> ai_vox3_lcd_initialize() turns on the backlight
   * and binds the ST7789, setting g_lcddev) BEFORE registering /dev/lcd0.
   * Reference pattern: esp32s3-box/esp32s3_bringup.c (board_lcd_initialize()
   * then lcddev_register(0)).  Without this, board_lcd_getdev() returns NULL
   * and lcddev_register() fails with -ENODEV (-19). */
  ets_printf("[BSP] -> board_lcd_initialize\n");
  ret = board_lcd_initialize();
  ets_printf("[BSP] <- board_lcd_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR lcd init: %d\n", ret);
    }

  ets_printf("[BSP] -> lcddev_register(0)\n");
  ret = lcddev_register(0);
  ets_printf("[BSP] <- lcddev_register ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR lcd register: %d\n", ret);
    }
#endif

  /* Bring up the non-essential peripherals (audio/servo/ws2812/buttons/sd)
   * in a BACKGROUND kernel thread (see periph_init_entry).  These are needed
   * for the voice/servo product features but MUST NOT block
   * board_app_initialize()/ui_init(): a hang in e.g. the RMT/ws2812 init
   * would otherwise stall bring-up and keep the screen dark.  ui_init() runs
   * as soon as we return from this function. */
  kthread_create("periph", 50, 8192, periph_init_entry, NULL);

  ets_printf("[BSP] esp_board_initialize done ret=%d\n", ret);
  return OK;
}

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Called after the OS is up to perform any application-level board setup.
 *   The desktop_companion application registers itself as a builtin and is
 *   launched by NSH, so nothing extra is required here.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARDCTL
int board_app_initialize(uintptr_t arg)
{
  /* Official esp32s3-box pattern (esp32s3_appinit.c): defined UNDER
   * CONFIG_BOARDCTL with the (uintptr_t arg) signature matching
   * include/nuttx/board.h, and it performs the board bring-up because this
   * board does not use CONFIG_BOARD_LATE_INITIALIZE.  NSH (CONFIG_NSH_
   * ARCHINIT=y) reaches it through BOARDIOC_INIT.  The previous version
   * here used "#ifndef CONFIG_BOARDCTL" -- compiling the function out
   * exactly when NSH needed it -- and a non-matching (int, char**)
   * signature. */

  ets_printf("[BSP] board_app_initialize enter\n");
  int r = esp_board_initialize();
  ets_printf("[BSP] board_app_initialize -> esp_board_initialize ret=%d\n", r);
  return r;
}
#endif /* CONFIG_BOARDCTL */

/****************************************************************************
 * Name: board_lcd_initialize / board_lcd_getdev
 *
 * Description:
 *   Standard LCD framework hooks. Delegate to the AI-VOX3 LCD module.
 *
 ****************************************************************************/

#ifdef CONFIG_LCD
int board_lcd_initialize(void)
{
  return ai_vox3_lcd_initialize();
}

struct lcd_dev_s *board_lcd_getdev(int lcddev)
{
  return ai_vox3_lcd_getdev(lcddev);
}

void board_lcd_uninitialize(void)
{
  /* Nothing special to tear down; the framebuffer is PSRAM-backed.
   * Signature matches the framework prototype (void)void -- the previous
   * (struct lcd_dev_s *dev) form conflicted with include/nuttx/lcd/lcd.h
   * (run#28 conflicting-types diagnostic). */
}
#endif /* CONFIG_LCD */
