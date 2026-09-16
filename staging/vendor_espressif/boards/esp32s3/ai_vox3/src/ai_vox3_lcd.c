/****************************************************************************
 * src/ai_vox3_lcd.c
 *
 * ST7789 240x240 LCD bring-up over FSPI (SPI2) for the emakefun AI-VOX3.
 *
 * Pin mapping (board.h):
 *   MOSI=21, SCL=17, DC=14, CS=15, BL=16, RST=-1
 *
 * The SPI clock/data/CS pins are routed by the ESP32-S3 SPI driver itself
 * (CONFIG_ESP32S3_SPI2_CLKPIN/MOSIPIN/CSPIN in the defconfig) -- the same
 * pattern as the official esp32s3-box LCD bring-up, which does NOT do any
 * manual GPIO-matrix routing for the bus.  Only DC and BL are plain GPIOs.
 * esp32s3_spi2_status/_cmddata are the board hooks the chip driver requires
 * (CONFIG_SPI_CMDDATA drives the D/C line for the ST7789).
 *
 * TODO(real-device): ST7789 color order (RGB vs BGR) and panel rotation must
 * be confirmed on the real LCD.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7789.h>

#include <arch/board/board.h>

#include <esp32s3_gpio.h>
#include <esp32s3_spi.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct lcd_dev_s *g_lcddev = NULL;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_spi2_status
 *
 * Description:
 *   Board-provided SPI status hook (required by the ESP32-S3 chip driver;
 *   no SPI status bits on this board).
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPI2
uint8_t esp32s3_spi2_status(struct spi_dev_s *dev, uint32_t devid)
{
  return 0;
}

/****************************************************************************
 * Name: esp32s3_spi2_cmddata
 *
 * Description:
 *   Board-provided SPI cmd/data hook (CONFIG_SPI_CMDDATA): drives the
 *   ST7789 D/C line.  Low = command, high = data.
 *
 ****************************************************************************/

#ifdef CONFIG_SPI_CMDDATA
int esp32s3_spi2_cmddata(struct spi_dev_s *dev, uint32_t devid, bool cmd)
{
  if (devid == SPIDEV_DISPLAY(0))
    {
      esp32s3_gpiowrite(BOARD_LCD_DC_GPIO, !cmd);
      return OK;
    }

  return -ENODEV;
}
#endif /* CONFIG_SPI_CMDDATA */
#endif /* CONFIG_ESP32S3_SPI2 */

/****************************************************************************
 * Name: ai_vox3_lcd_initialize
 *
 * Description:
 *   Configure control GPIOs, initialize the FSPI bus (the driver routes
 *   SCLK/MOSI/CS itself per CONFIG_ESP32S3_SPI2_*PIN) and bind the ST7789
 *   driver.  Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_lcd_initialize(void)
{
  struct spi_dev_s *spi;

  /* Configure DC and BL as GPIO outputs (RST is absent on this board). */
  esp32s3_configgpio(BOARD_LCD_DC_GPIO, OUTPUT);
  esp32s3_configgpio(BOARD_LCD_BL_GPIO, OUTPUT);

  /* Turn the backlight on (active high). */
  esp32s3_gpiowrite(BOARD_LCD_BL_GPIO, true);

  /* Initialize the FSPI bus controller. */
  spi = esp32s3_spibus_initialize(BOARD_LCD_SPI_PORT);
  if (spi == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize FSPI (port %d)\n",
             BOARD_LCD_SPI_PORT);
      return -ENODEV;
    }

  /* Bind the ST7789 driver to the SPI bus. */
  g_lcddev = st7789_lcdinitialize(spi);
  if (g_lcddev == NULL)
    {
      syslog(LOG_ERR, "ERROR: st7789_lcdinitialize failed\n");
      return -ENODEV;
    }

  syslog(LOG_INFO, "LCD (ST7789 240x240) initialized\n");
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_lcd_getdev
 *
 * Description:
 *   Return the bound LCD device handle for the LCD framework.
 *
 ****************************************************************************/

struct lcd_dev_s *ai_vox3_lcd_getdev(int lcddev)
{
  DEBUGASSERT(lcddev == 0);
  return g_lcddev;
}
