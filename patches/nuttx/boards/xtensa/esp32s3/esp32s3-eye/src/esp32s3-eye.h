/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3-eye.h
 *
 * AI-VOX3 (esp32-s3 AI-VOX3 v0.1) pin map -- OVERRIDE of the upstream
 * esp32s3-eye header.
 *
 * Why this override exists
 * ------------------------
 * Upstream open-vela/nuttx boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3-eye.h
 * describes Espressif's own ESP32-S3-EYE accessory board, whose LCD sits on
 * DC=43 / BCKL=48 and whose SPI2 pins are CLK=21 / CS=44 / MISO=1 / MOSI=47.
 *
 * The emakefun AI-VOX3 v0.1 reuses the esp32s3-eye board directory but wires
 * the ST7789 and the ES8311 differently.  Building the upstream header against
 * AI-VOX3 hardware drives GPIOs that are not connected to anything, so the
 * panel never lights -- and, worse, GPIO48 is an ordinary RTC-domain GPIO, so
 * the backlight pad-handoff in esp32s3_board_lcd.c would appear to "work"
 * while lighting nothing.
 *
 * The values below are the authoritative AI-VOX3 wiring, taken from the
 * shipping firmware's own .config and confirmed against its boot log:
 *
 *   [LCDDIAG] board: DC(14) level=0, BCKL(16) level=0
 *
 * which is the only place the running image reports its own pins, so these
 * match what the hardware and the proven-good build both use.
 *
 * LCD (ST7789 240x240, FSPI/SPI2)
 *   SCLK = 17   MOSI = 21   CS = 15   DC = 14   BL = 16   RST = not wired
 *
 * BL is the pad that needs the RTC handoff: GPIO16 is the XTAL_32K_N leg of
 * the 32.768 kHz crystal and belongs to the RTC power domain, which is why
 * esp32s3_board_lcd.c calls ai_vox3_bl_force_gpio() before driving it.
 *
 * Audio (ES8311 codec, I2S0 + I2C0)
 *   I2S0: BCLK = 10   WS = 8   DIN = 9   DOUT = 7   MCLK = 11
 *   I2C0: SCL = 12    SDA = 13        (codec addr 0x18, 400 kHz)
 *
 ****************************************************************************/

#ifndef __BOARDS_XTENSA_ESP32S3_ESP32S3_EYE_SRC_ESP32S3_EYE_H
#define __BOARDS_XTENSA_ESP32S3_ESP32S3_EYE_SRC_ESP32S3_EYE_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ESP32-S3-EYE GPIOs *******************************************************/

/* BOOT Button (active low, internal pull-up) */

#define BUTTON_BOOT         0

/* Display -- AI-VOX3 wiring, NOT the upstream ESP32-S3-EYE accessory */

#define ESP32S3_EYE_DISPLAY_SPI         2
#define ESP32S3_EYE_DISPLAY_DC          14
#define ESP32S3_EYE_DISPLAY_BCKL        16

/* ES8311 audio codec */

#define ESP32S3_EYE_AUDIO_I2C_PORT      0
#define ESP32S3_EYE_AUDIO_I2C_ADDR      0x18
#define ESP32S3_EYE_AUDIO_I2C_FREQ      400000
#define ESP32S3_EYE_AUDIO_I2S_PORT      0

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y && CONFIG_BOARDCTL=y :
 *     Called from the NSH library via board_app_initialize()
 *
 ****************************************************************************/

int esp32s3_bringup(void);

/****************************************************************************
 * Name: esp32s3_gpio_init
 *
 * Description:
 *   Configure the GPIO driver.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of the error.
 *
 ****************************************************************************/

#ifdef CONFIG_DEV_GPIO
int esp32s3_gpio_init(void);
#endif

/****************************************************************************
 * Name: board_spiflash_init
 *
 * Description:
 *   Initialize the SPIFLASH and register the MTD device.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of the error.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPIFLASH
int board_spiflash_init(void);
#endif

/****************************************************************************
 * Name: board_audio_initialize
 *
 * Description:
 *   Initialize the ES8311 audio codec and the I2S/I2C buses it hangs off.
 *   Defined by esp32s3_board_audio.c.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_I2S
int board_audio_initialize(void);
#endif

/****************************************************************************
 * Name: board_lcd_initialize
 *
 * Description:
 *   Initialize the LCD video hardware. The initial state of the LCD is fully
 *   initialized, display memory cleared, and the LCD ready to use, but with
 *   the power setting at 0 (full off).
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_EYE_LCD
int board_lcd_initialize(void);
#endif

/****************************************************************************
 * Name: board_lcd_getdev
 *
 * Description:
 *   Return a reference to the LCD object for the specified LCD. This allows
 *   support for multiple LCD devices.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_EYE_LCD
struct lcd_dev_s *board_lcd_getdev(int lcddev);
#endif

/****************************************************************************
 * Name: board_lcd_uninitialize
 *
 * Description:
 *   Uninitialize the LCD support.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_EYE_LCD
void board_lcd_uninitialize(void);
#endif

/****************************************************************************
 * Name: board_i2c_init
 *
 * Description:
 *   Initialize the I2C driver.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of the error.
 *
 ****************************************************************************/

#ifdef CONFIG_I2C_DRIVER
int board_i2c_init(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_XTENSA_ESP32S3_ESP32S3_EYE_SRC_ESP32S3_EYE_H */