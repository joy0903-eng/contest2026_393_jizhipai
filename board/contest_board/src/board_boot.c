/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 board boot
 *
 * AI-VOX3 = ESP32-S3-R8 (16 MB Flash / 8 MB OCT PSRAM), native USB-CDC
 * console (VID:PID 303A:1001).
 *
 * openvela_board_initialize() is the board bring-up hook called by the
 * openvela board init chain. We delegate all device registration to
 * aivox3_bringup().
 ****************************************************************************/

#include <nuttx/board.h>

#include "aivox3.h"

void openvela_board_initialize(void)
{
  /* Register onboard peripherals (LCD, buttons, servos, ...). Each
   * sub-init is self-contained and logs (never panics) on failure so the
   * NSH console always comes up even if a peripheral is misconfigured. */
  aivox3_bringup();
}
