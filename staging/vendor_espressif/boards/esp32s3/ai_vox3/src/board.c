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

#include <debug.h>
#include <syslog.h>

#include <arch/board/board.h>

#ifdef CONFIG_BOARDCTL
#include <sys/boardctl.h>
#endif

#ifdef CONFIG_ESP32S3_WIFI
#include "esp32s3_wlan.h"
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

int esp_board_initialize(void)
{
  int ret = OK;

  ets_printf("[BSP] esp_board_initialize enter\n");

#if defined(CONFIG_ESP32S3_WIFI) && defined(ESP32S3_WLAN_HAS_STA)
  ets_printf("[BSP] -> wlan_sta_initialize\n");
  ret = esp32s3_wlan_sta_initialize();
  ets_printf("[BSP] <- wlan_sta_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      ets_printf("[BSP] ERR wlan sta init: %d\n", ret);
    }
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
