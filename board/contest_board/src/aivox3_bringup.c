/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 board bring-up
 *
 * Registers the onboard peripherals as NuttX character devices so the
 * desktop-companion application (and NSH) can use them.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <syslog.h>
#include <errno.h>
#include <nuttx/board.h>
#include <nuttx/fs/fs.h>

#ifdef CONFIG_AIVOX3_LCD
#  include <nuttx/lcd/lcd_dev.h>
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#include "aivox3.h"

/****************************************************************************
 * Name: aivox3_bringup
 ****************************************************************************/

int aivox3_bringup(void)
{
  int ret;

#ifdef CONFIG_AIVOX3_LCD
  ret = board_lcd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: AI-VOX3 LCD init failed: %d\n", ret);
    }
  else
    {
      ret = lcddev_register(0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: lcddev_register(0) failed: %d\n", ret);
        }
      else
        {
          syslog(LOG_INFO, "AI-VOX3 LCD ready at /dev/lcd0\n");
        }
    }
#endif

#ifdef CONFIG_INPUT_BUTTONS
  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: btn_lower_initialize failed: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "AI-VOX3 buttons ready at /dev/buttons\n");
    }
#endif

#ifdef CONFIG_AIVOX3_SERVO
  ret = aivox3_servo_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: servo init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_AIVOX3_WS2812
  aivox3_ws2812_initialize();
#endif

  syslog(LOG_INFO, "AI-VOX3 board bring-up complete\n");
  return OK;
}
