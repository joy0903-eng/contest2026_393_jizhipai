/****************************************************************************
 * include/board.h
 *
 * Board-specific definitions for the emakefun AI-VOX3 (ESP32-S3-R8).
 *
 * Pin mapping is sourced verbatim from the "AI-VOX3 技术知识库 §3" and the
 * porting plan appendix B. Pins whose real-device behavior is ambiguous are
 * marked with TODO(real-device).
 *
 * This header is BSP-internal. The desktop_companion application uses its own
 * apps/examples/desktop_companion/config.h for application-level constants.
 *
 ****************************************************************************/

#ifndef __BOARDS_ESP32S3_AI_VOX3_INCLUDE_BOARD_H
#define __BOARDS_ESP32S3_AI_VOX3_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Board Constants
 ****************************************************************************/

/* Master clock / crystal: ESP32-S3 modules use a 40 MHz crystal. */
#define BOARD_XTAL_FREQ          40

/* SPI instance used by the LCD (FSPI = SPI2 on ESP32-S3). */
#define BOARD_LCD_SPI_PORT       2

/* I2C bus used by the ES8311 codec (I2C0). */
#define BOARD_ES8311_I2C_BUS     0

/* I2S port used by the ES8311 codec (I2S0). */
#define BOARD_ES8311_I2S_PORT    0

/* SD/MMC slot (1-bit mode). */
#define BOARD_SD_SLOT            0

/****************************************************************************
 * LCD — ST7789 240x240 (FSPI)
 * MOSI=21, SCL=17, DC=14, CS=15, BL=16, RST=-1 (no hardware reset pin)
 * Color order (RGB vs BGR) and rotation are TODO(real-device).
 ****************************************************************************/

#define BOARD_LCD_MOSI_GPIO      21
#define BOARD_LCD_SCL_GPIO       17
#define BOARD_LCD_DC_GPIO        14
#define BOARD_LCD_CS_GPIO        15
#define BOARD_LCD_BL_GPIO        16
#define BOARD_LCD_RST_GPIO       (-1)   /* not connected */
#define BOARD_LCD_WIDTH          240
#define BOARD_LCD_HEIGHT         240

/****************************************************************************
 * Audio — ES8311 codec (I2C config + I2S data)
 * I2C: SCL=12, SDA=13, addr 0x18
 * I2S: MCLK=11, BCLK=10, WS/LRCK=?, DOUT=?, DIN=?  (THREE DATA PINS AMBIGUOUS)
 * The three I2S data pins are documented inconsistently by the vendor and
 * MUST be oscilloscope-verified on the real board. They are centralized here
 * so a single edit fixes the whole BSP.
 ****************************************************************************/

#define BOARD_ES8311_I2C_SCL     12
#define BOARD_ES8311_I2C_SDA     13
#define BOARD_ES8311_I2C_ADDR    0x18  /* ES8311_ADDRRES_0 */

#define BOARD_ES8311_I2S_MCLK    11
#define BOARD_ES8311_I2S_BCLK    10

/* TODO(real-device): The vendor doc contradicts itself:
 *   WS/LRCK = 9 or 8 ; DOUT(ASDOUT) = 8 or 7 ; DIN(DSDIN) = 7 or 9
 * The current assumption below is ONE of the plausible combinations.
 * Verify on real HW with an oscilloscope before relying on audio capture/play.
 */
#define BOARD_ES8311_I2S_WS      9     /* WS/LRCK  (9 or 8)  */
#define BOARD_ES8311_I2S_DOUT    8     /* ASDOUT    (8 or 7)  */
#define BOARD_ES8311_I2S_DIN     7     /* DSDIN     (7 or 9)  */

/* Audio format used by ES8311 (16-bit, mono, 16 kHz by default). */
#define BOARD_AUDIO_SAMPLE_RATE  16000
#define BOARD_AUDIO_BITS         16
#define BOARD_AUDIO_CHANNELS     1

/****************************************************************************
 * Servo x4 — LEDC PWM @ 50 Hz
 * IO42 / IO43 / IO44 / IO48  (servo index 0..3)
 ****************************************************************************/

#define BOARD_SERVO_COUNT        4
#define BOARD_SERVO0_GPIO        42
#define BOARD_SERVO1_GPIO        43
#define BOARD_SERVO2_GPIO        44
#define BOARD_SERVO3_GPIO        48
#define BOARD_SERVO_FREQ_HZ      50

/****************************************************************************
 * WS2812 — RMT @ 800 kHz, GRB order, single pixel
 * IO41
 ****************************************************************************/

#define BOARD_WS2812_GPIO        41
#define BOARD_WS2812_RMT_CH      0

/****************************************************************************
 * TF/SD — MMC 1-bit
 * CMD=38, CLK=39, DAT0=40
 ****************************************************************************/

#define BOARD_SD_CMD_GPIO        38
#define BOARD_SD_CLK_GPIO        39
#define BOARD_SD_DAT0_GPIO       40

/****************************************************************************
 * Buttons — external pull-up, active LOW
 * A=46, B=45, BOOT=0
 ****************************************************************************/

#define BOARD_BTN_A_GPIO         46
#define BOARD_BTN_B_GPIO         45
#define BOARD_BTN_BOOT_GPIO     0

/* Button identifiers used by the buttons driver. */
#define AI_VOX3_BTN_A            0
#define AI_VOX3_BTN_B            1
#define AI_VOX3_BTN_BOOT         2
#define AI_VOX3_BTN_COUNT        3

/****************************************************************************
 * Battery ADC — IO18
 * Voltage divider ratio is NOT documented; needs calibration on real HW.
 ****************************************************************************/

#define BOARD_BAT_ADC_GPIO       18
#define BOARD_BAT_ADC_CHANNEL    0   /* TODO(real-device): map IO18 -> ADC unit/channel */
/* TODO(real-device): BOARD_BAT_ADC_DIVIDER_RATIO and mV-per-LSB calibration. */
#define BOARD_BAT_ADC_DIVIDER_RATIO   (1.0f)  /* placeholder; calibrate on HW */

/****************************************************************************
 * Public Function Prototypes — per-peripheral board initialization.
 * Implemented in src/ai_vox3_*.c. Called from src/board.c.
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

struct lcd_dev_s;     /* forward decl from nuttx/lcd/lcd.h */
struct i2c_master_s;  /* forward decl from nuttx/i2c/i2c_master.h */
struct i2s_dev_s;     /* forward decl from nuttx/audio/i2s.h */

int  ai_vox3_lcd_initialize(void);
struct lcd_dev_s *ai_vox3_lcd_getdev(int lcddev);

int  ai_vox3_audio_initialize(void);

int  ai_vox3_servo_initialize(void);
int  ai_vox3_servo_set_angle(int servo_id, int angle_deg);

int  ai_vox3_ws2812_initialize(void);
int  ai_vox3_ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b);

int  ai_vox3_buttons_initialize(void);

/* Button press callback type and registration (used by the application). */
typedef void (*ai_vox3_button_callback_t)(int btn_id);
int  ai_vox3_buttons_register_callback(ai_vox3_button_callback_t cb);
int  ai_vox3_buttons_read(int btn_id);

int  ai_vox3_sd_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARDS_ESP32S3_AI_VOX3_INCLUDE_BOARD_H */
