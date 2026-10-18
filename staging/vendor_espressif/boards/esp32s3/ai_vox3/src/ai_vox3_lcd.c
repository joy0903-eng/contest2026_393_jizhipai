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

extern int ets_printf(const char *fmt, ...);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct lcd_dev_s *g_lcddev = NULL;

/****************************************************************************
 * Name: ai_vox3_bl_claim_pad
 *
 * Description:
 *   Take the backlight pad away from the RTC 32 kHz domain and hand it to
 *   the digital GPIO controller.  THIS MUST RUN BEFORE THE BACKLIGHT IS
 *   DRIVEN, otherwise the write below is silently discarded.
 *
 * Why this is needed
 * ------------------
 *   GPIO16 is not a free GPIO on the ESP32-S3: it is the XTAL_32K_N pad
 *   (the negative leg of the 32.768 kHz crystal), and it belongs to the RTC
 *   power domain.  Until the RTC side explicitly releases it, a plain
 *   esp32s3_configgpio()/gpiowrite() on that pad has no electrical effect:
 *   the pad is still muxed to the analog 32K oscillator, so the backlight
 *   never turns on and the panel stays black no matter what value we write.
 *
 *   That is exactly the failure this board exhibited: the ST7789 came up
 *   (SWRESET/SLPOUT/DISPON all ACKed, framebuffer filled) while the screen
 *   stayed dark, and every "backlight level = 0" line in the logs was a
 *   read-back artifact -- esp32s3_configgpio(OUTPUT) never sets FUN_IE, so
 *   the input buffer stays off and gpioread() is hard-wired to 0 for this
 *   pad.  In other words the logs said "0" no matter what was really driven,
 *   which is why polarity experiments alone could never settle this.
 *
 * Register map (ESP32-S3, RTC domain) -- addresses taken from the SoC headers:
 *   RTCIO_XTAL_32N_PAD_REG  0x600084C4
 *     bit19 X32N_MUX_SEL : 1 = pad claimed by the RTC 32K oscillator
 *                          (analog domain, overrides the digital IO_MUX)
 *   RTC_CNTL_RTC_CLK_CONF_REG 0x60008074
 *     bit8  DIG_XTAL32K_EN : digital 32 kHz oscillator enable
 *   RTC_CNTL_RTC_PAD_HOLD_REG  0x600080D8 (RTC pads GPIO0..21, so bit16 here)
 *   RTC_CNTL_DIG_PAD_HOLD_REG  0x600080DC (digital pads, bit16)
 *   GPIO_ENABLE / IO_MUX / OUTSEL for pin 16
 *     IO_MUX   pin16     0x60009000 + 0x04 + 16*4 = 0x60009044
 *     OUTSEL   pin16     0x60004594 (256 = SIG_GPIO_OUT_IDX)
 *     PIN_REG  pin16     0x600040B4
 *
 *   NOTE: the pad HOLD latches live in the RTC power domain and SURVIVE a
 *   USB reset, so a hold bit set by any earlier firmware freezes the pad
 *   forever no matter what every other register says.  Both holds are
 *   therefore cleared explicitly.
 *
 * Cost / risk
 * -----------
 *   The RTC slow clock keeps running: ANA_CLK_RTC_SEL is left at RC_SLOW and
 *   only the pad's digital 32K claim is dropped, so nothing here depends on
 *   an external 32.768 kHz crystal being populated.
 *
 ****************************************************************************/

static void ai_vox3_bl_claim_pad(void)
{
  volatile uint32_t *iomux    = (volatile uint32_t *)0x60009044u;
  volatile uint32_t *outsel   = (volatile uint32_t *)0x60004594u;
  volatile uint32_t *pinreg   = (volatile uint32_t *)0x600040B4u;
  volatile uint32_t *x32n     = (volatile uint32_t *)0x600084C4u;
  volatile uint32_t *clkcf    = (volatile uint32_t *)0x60008074u;
  volatile uint32_t *rtchold  = (volatile uint32_t *)0x600080D8u;
  volatile uint32_t *dighold  = (volatile uint32_t *)0x600080DCu;

  /* Read-modify-write: these registers carry unrelated fields. */

  /* 1) RTC: stop claiming the pad for the 32 kHz oscillator and drop the
   *    digital 32K oscillator.  RTC_CNTL_RTC_CLK_CONF_REG.bit8 and
   *    RTCIO_XTAL_32N_PAD_REG.bit19 respectively.
   */
  *x32n    &= ~(1u << 19);
  *clkcf   &= ~(1u << 8);

  /* 2) Release both pad HOLD latches (they survive a USB reset, so a stale
   *    hold from earlier firmware would keep the pad frozen).
   */
  *rtchold &= ~(1u << 16);
  *dighold &= ~(1u << 16);

  /* 3) Digital: make the pad a plain push-pull GPIO output driven by
   *    GPIO_OUT bit 16.  OUTSEL 256 == SIG_GPIO_OUT_IDX.
   */
  *outsel = 256u;
  *pinreg = 0;    /* push-pull output, no interrupt, no pull */
  *iomux  = (uint32_t)((1u << 12) | (1u << 9) | (2u << 10));

  ets_printf("[LCD] BL pad claimed from RTC (XTAL_32K_N released)\n");
}

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

  ets_printf("[LCD] enter\n");

  /* Hand the backlight pad back from the RTC 32 kHz domain FIRST: GPIO16 is
   * the XTAL_32K_N pad and any write to it before this is a no-op.  Doing it
   * before configgpio() also keeps the IO_MUX assignment below intact (the
   * handoff rewrites IO_MUX/OUTSEL/PIN_REG for this pad).
   */
  ai_vox3_bl_claim_pad();

  /* Configure DC and BL as GPIO outputs (RST is absent on this board). */
  esp32s3_configgpio(BOARD_LCD_DC_GPIO, OUTPUT);
  esp32s3_configgpio(BOARD_LCD_BL_GPIO, OUTPUT);

  /* Turn the backlight on.  AI-VOX3's BL is ACTIVE-LOW: the LED cathode is on
   * this net, so a LOW level lights the panel and HIGH turns it off.  This was
   * verified on real hardware (v530 selftest, same board, WiFi stayed up while
   * the pad was driven).
   */
  esp32s3_gpiowrite(BOARD_LCD_BL_GPIO, false);
  ets_printf("[LCD] backlight on (active-low, pin driven LOW)\n");

  /* Sanity read-back.  INPUT|OUTPUT is required for gpioread() to reflect the
   * real pad level: plain OUTPUT never sets FUN_IE, so the input buffer stays
   * disabled and the read is hard-wired to 0.  This is purely diagnostic.
   */
  esp32s3_configgpio(BOARD_LCD_BL_GPIO, INPUT | OUTPUT);
  ets_printf("[LCD] BL pin level = %d (expect 0 = lit, active-low)\n",
             esp32s3_gpioread(BOARD_LCD_BL_GPIO));

  /* Initialize the FSPI bus controller. */
  spi = esp32s3_spibus_initialize(BOARD_LCD_SPI_PORT);
  if (spi == NULL)
    {
      ets_printf("ERROR: Failed to initialize FSPI (port %d)\n",
             BOARD_LCD_SPI_PORT);
      return -ENODEV;
    }

  ets_printf("[LCD] spibus ok, st7789 init\n");

  /* Bind the ST7789 driver to the SPI bus. */
  g_lcddev = st7789_lcdinitialize(spi);
  ets_printf("[LCD] st7789 done\n");
  if (g_lcddev == NULL)
    {
      ets_printf("ERROR: st7789_lcdinitialize failed\n");
      return -ENODEV;
    }

  ets_printf("LCD (ST7789 240x240) initialized\n");
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
