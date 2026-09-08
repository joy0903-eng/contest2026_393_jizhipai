/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 board header
 *
 * AI-VOX3 (emakefun) = ESP32-S3-R8, 16 MB Flash, 8 MB OCT PSRAM.
 * Native USB-CDC console (VID:PID 303A:1001).
 *
 * Pin map (authoritative; see ai_vox3_pins.h in M2). Values come from the
 * verified hardware bring-up on the Arduino/ESP-IDF side of this project.
 ****************************************************************************/

#ifndef __BOARDS_VENDOR_OPENVELA_BOARDS_CONTEST2026_393_BOARD_BOARD_H
#define __BOARDS_VENDOR_OPENVELA_BOARDS_CONTEST2026_393_BOARD_BOARD_H

/* Servo / pan-tilt (LEDC PWM) */
#define AIVOX3_SERVO_PAN_GPIO       42   /* board expansion pin 42 */
#define AIVOX3_SERVO_TILT_GPIO      43   /* board expansion pin 43 */
#define AIVOX3_SERVO_GPIO2          44
#define AIVOX3_SERVO_GPIO3          48

/* RGB status LED (WS2812, single-wire) */
#define AIVOX3_WS2812_GPIO          41

/* LCD (ST7789, FSPI) */
#define AIVOX3_LCD_MOSI_GPIO        21
#define AIVOX3_LCD_SCLK_GPIO        17
#define AIVOX3_LCD_DC_GPIO          14
#define AIVOX3_LCD_CS_GPIO          15
#define AIVOX3_LCD_BL_GPIO          16
#define AIVOX3_LCD_RST_GPIO         13   /* shared with ES8311 reset on some revs */

/* ES8311 audio codec (I2C + I2S) */
#define AIVOX3_ES8311_I2C_SCL_GPIO  12
#define AIVOX3_ES8311_I2C_SDA_GPIO  13
#define AIVOX3_ES8311_I2C_ADDR       0x18
#define AIVOX3_I2S_MCLK_GPIO        11
#define AIVOX3_I2S_BCLK_GPIO        10
#define AIVOX3_I2S_WS_GPIO           9
#define AIVOX3_I2S_DOUT_GPIO         8   /* codec -> esp32 (mic) */
#define AIVOX3_I2S_DIN_GPIO          7   /* esp32 -> codec (speaker) */

/* Buttons */
#define AIVOX3_BTN_1_GPIO           46
#define AIVOX3_BTN_2_GPIO           45
#define AIVOX3_BTN_3_GPIO           0

/* Battery / power */
#define AIVOX3_BAT_ADC_GPIO         18   /* GPIO18 = ADC2_CH7 */

/* microSD (SDMMC) */
#define AIVOX3_SD_CMD_GPIO          38
#define AIVOX3_SD_CLK_GPIO          39
#define AIVOX3_SD_DAT0_GPIO         40

#endif /* __BOARDS_VENDOR_OPENVELA_BOARDS_CONTEST2026_393_BOARD_BOARD_H */
