/****************************************************************************
 * src/ai_vox3_lcd.c
 *
 * ST7789 240x240 LCD bring-up over FSPI (SPI2) for the emakefun AI-VOX3.
 *
 * Pin mapping (board.h):
 *   MOSI=21, SCL=17, DC=14, CS=15, BL=16, RST=-1
 *
 * The SPI clock/data pins are routed to the FSPI peripheral via the GPIO
 * matrix; DC / CS / BL are driven as plain GPIOs (the ST7789 NuttX driver
 * controls DC and CS through these pins).
 *
 * TODO(real-device): ST7789 color order (RGB vs BGR) and panel rotation must
 * be confirmed on the real LCD. They are typically set via Kconfig
 * (CONFIG_LCD_ST7789_* ) or passed to the driver — adjust once verified.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7789.h>

#include <arch/board/board.h>
#include <esp32s3_gpio.h>


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* FSPI (SPI2) output signals on ESP32-S3 for GPIO matrix routing.
 * TODO(real-device): confirm exact signal enum names in the current tree
 * (esp32s3_gpio_sigmap.h). The names below match the common ESP32-S3 SDK. */
#define AI_VOX3_FSPICLK_OUT    FSPICLK_OUT    /* SCL */
#define AI_VOX3_FSPID_OUT      FSPID_OUT      /* MOSI (data out) */
#define AI_VOX3_FSPICS0_OUT    FSPICS0_OUT    /* CS */

/* GPIO attribute helpers (esp32s3). */
#define GPIO_OUTPUT_PIN(gpio)  (GPIO_OUTPUT_PINMUX(gpio))

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct lcd_dev_s *g_lcddev = NULL;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_lcd_initialize
 *
 * Description:
 *   Initialize the FSPI bus, route pins, configure control GPIOs and bind the
 *   ST7789 LCD driver. Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_lcd_initialize(void)
{
  struct spi_dev_s *spi;
  int ret;

  /* Route FSPI clock + MOSI to the LCD pins via the GPIO matrix. */
  esp32s3_gpio_matrix_out(BOARD_LCD_SCL_GPIO, AI_VOX3_FSPICLK_OUT, false, false);
  esp32s3_gpio_matrix_out(BOARD_LCD_MOSI_GPIO, AI_VOX3_FSPID_OUT, false, false);
  esp32s3_gpio_matrix_out(BOARD_LCD_CS_GPIO, AI_VOX3_FSPICS0_OUT, false, false);

  /* Configure DC and BL as GPIO outputs (RST is absent on this board). */
  esp32s3_gpio_config(GPIO_OUTPUT_PIN(BOARD_LCD_DC_GPIO));
  esp32s3_gpio_config(GPIO_OUTPUT_PIN(BOARD_LCD_BL_GPIO));

  /* Turn the backlight on (active high). */
  esp32s3_gpio_write(BOARD_LCD_BL_GPIO, true);

  /* Initialize the FSPI bus controller. */
  spi = esp32s3_spibus_initialize(BOARD_LCD_SPI_PORT);
  if (spi == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize FSPI (port %d)\n",
             BOARD_LCD_SPI_PORT);
      return -ENODEV;
    }

  /* Bind the ST7789 driver to the SPI bus.
   * Note: upstream signature is st7789_lcdinitialize(spi); if your tree uses
   * st7789_lcdinitialize(spi, devno) adjust accordingly. */
  g_lcddev = st7789_lcdinitialize(spi);
  if (g_lcddev == NULL)
    {
      syslog(LOG_ERR, "ERROR: st7789_lcdinitialize failed\n");
      return -ENODEV;
    }

  /* Clear to black so the panel is not showing random garbage. */
  ret = g_lcddev->clear(g_lcddev, 0);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: LCD clear returned %d\n", ret);
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
