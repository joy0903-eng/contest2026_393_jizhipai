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

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

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
