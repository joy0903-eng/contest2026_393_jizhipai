/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 board boot
 *
 * AI-VOX3 = ESP32-S3-R8 (16 MB Flash / 8 MB OCT PSRAM), native USB-CDC
 * console (VID:PID 303A:1001).
 *
 * openvela_board_initialize() is the board bring-up hook called by the
 * openvela board init chain.
 ****************************************************************************/

#include <nuttx/board.h>

void openvela_board_initialize(void)
{
  /* M1 (L0): minimal NSH bring-up. The esp32s3 arch code initializes the
   * CPU clock, OPI PSRAM and the native USB-CDC serial console from the
   * defconfig. Board-level peripheral drivers (servo LEDC, ES8311 audio,
   * ST7789 LCD, WS2812, SD, buttons) are registered in M2 from
   * board_bringup(). */
}
