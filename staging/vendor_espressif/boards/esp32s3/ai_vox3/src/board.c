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

int esp_board_initialize(void)
{
  int ret = OK;

#if defined(CONFIG_ESP32S3_WIFI) && defined(ESP32S3_WLAN_HAS_STA)
  /* Register the WLAN station netdev.  BOARDIOC_INIT (NSH archinit)
   * reaches here before the NSH network initialization, so the netdev
   * exists by the time ifup runs.  ESP32S3_WLAN_HAS_STA is defined by
   * esp32s3_wifi_adapter.h (pulled in through esp32s3_wlan.h). */
  ret = esp32s3_wlan_sta_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: wlan sta init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_LCD
  ret = ai_vox3_lcd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: LCD init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_AUDIO
  ret = ai_vox3_audio_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_AI_VOX3_SERVO
  ret = ai_vox3_servo_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: servo init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_AI_VOX3_WS2812
  ret = ai_vox3_ws2812_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ws2812 init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_AI_VOX3_BUTTONS
  ret = ai_vox3_buttons_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: buttons init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_MMCSD
  ret = ai_vox3_sd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: sd init failed: %d\n", ret);
    }
#endif

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

  return esp_board_initialize();
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
