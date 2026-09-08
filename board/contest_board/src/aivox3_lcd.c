/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 ST7789 LCD board support
 *
 * 1.54" ST7789 on the ESP32-S3 FSPI (SPI2) bus:
 *   MOSI=21, SCLK=17, DC=14, CS=15, BL=16, RST=13
 *
 * Mirrors the Espressif esp32s3-eye ST7789 bring-up.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/signal.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7789.h>

#include <arch/board/board.h>

#include "esp32s3_gpio.h"
#include "esp32s3_spi.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "aivox3.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct spi_dev_s *g_spidev;
static struct lcd_dev_s *g_lcd;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_lcd_initialize
 ****************************************************************************/

int board_lcd_initialize(void)
{
  /* Initialize non-SPI GPIOs (DC, backlight). CS is driven by the SPI
   * controller, so it is configured through the SPI2 Kconfig, not here. */

  esp32s3_configgpio(AIVOX3_LCD_DC_GPIO, OUTPUT);
  esp32s3_configgpio(AIVOX3_LCD_BL_GPIO, OUTPUT);

  /* Backlight is active-low on AI-VOX3 */
  esp32s3_gpiowrite(AIVOX3_LCD_BL_GPIO, false);

  g_spidev = esp32s3_spibus_initialize(AIVOX3_LCD_SPI);
  if (!g_spidev)
    {
      lcderr("ERROR: Failed to initialize SPI bus %d\n", AIVOX3_LCD_SPI);
      return -ENODEV;
    }

  g_lcd = st7789_lcdinitialize(g_spidev);
  if (!g_lcd)
    {
      lcderr("ERROR: st7789_lcdinitialize() failed\n");
      return -ENODEV;
    }

  return OK;
}

/****************************************************************************
 * Name: board_lcd_getdev
 ****************************************************************************/

struct lcd_dev_s *board_lcd_getdev(int devno)
{
  if (!g_lcd)
    {
      lcderr("ERROR: LCD not initialized\n");
      return NULL;
    }

  return g_lcd;
}

/****************************************************************************
 * Name: board_lcd_uninitialize
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
  if (g_lcd)
    {
      g_lcd->setpower(g_lcd, 0);
    }
}
