/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 internal board header
 ****************************************************************************/

#ifndef __BOARDS_VENDOR_OPENVELA_BOARDS_CONTEST2026_393_BOARD_AIVOX3_H
#define __BOARDS_VENDOR_OPENVELA_BOARDS_CONTEST2026_393_BOARD_AIVOX3_H

#include <nuttx/config.h>
#include <arch/board/board.h>

/* LCD is on the ESP32-S3 FSPI bus, which is exposed by the esp32s3 SPI
 * driver as SPI bus 2. */
#define AIVOX3_LCD_SPI 2

/* LEDC timers assigned to the two pan/tilt servos.
 * Channel / GPIO mapping is done via the esp32s3 LEDC Kconfig
 * (CONFIG_ESP32S3_LEDC_CHANNELx_GPIO / _TIMER). */
#define AIVOX3_LEDC_TIMER_PAN  0
#define AIVOX3_LEDC_TIMER_TILT 1

/* Button bit definitions returned by board_buttons().
 * Buttons are active-low on GPIO46/45/0. */
#define AIVOX3_BTN1_BIT (1 << 0)  /* GPIO46 */
#define AIVOX3_BTN2_BIT (1 << 1)  /* GPIO45 */
#define AIVOX3_BTN3_BIT (1 << 2)  /* GPIO0  */

/* Public board functions */
int  aivox3_bringup(void);
#ifdef CONFIG_AIVOX3_SERVO
int  aivox3_servo_initialize(void);
#endif
#ifdef CONFIG_AIVOX3_WS2812
int  aivox3_ws2812_initialize(void);
#endif

#endif /* __BOARDS_VENDOR_OPENVELA_BOARDS_CONTEST2026_393_BOARD_AIVOX3_H */
