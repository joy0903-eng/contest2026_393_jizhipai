/****************************************************************************
 * src/ai_vox3_ws2812.c
 *
 * WS2812 (single addressable RGB LED) driver for the emakefun AI-VOX3.
 *
 *  - GPIO: IO41
 *  - Transport: ESP32-S3 RMT channel 0 @ 800 kHz, GRB color order.
 *
 * 2026-09-17 REWRITE -- why this file changed
 * -------------------------------------------
 * The previous revision was written against a lower-half API that DOES NOT
 * EXIST in this tree.  It did:
 *
 *     #include <esp32s3_rmt.h>
 *     ret = esp32s3_rmt_tx_init(BOARD_WS2812_RMT_CH, BOARD_WS2812_GPIO);
 *     return esp32s3_rmt_tx(BOARD_WS2812_RMT_CH, items, WS2812_ITEM_COUNT);
 *
 * Verified against upstream, every part of that is wrong:
 *   - there is no esp32s3_rmt.h and no esp32s3_rmt.c.  RMT lives in
 *     arch/xtensa/src/common/espressif/esp_rmt.c, header esp_rmt.h.
 *   - the real init function is
 *         struct rmt_dev_s *esp_rmt_tx_init(int ch, int pin);
 *     it RETURNS A HANDLE, not an int.
 *   - there is no esp32s3_rmt_tx() function at all.
 *
 * The old file never compiled because CONFIG_AI_VOX3_WS2812 was never
 * enabled, so none of it was built.  Flipping that switch alone would have
 * traded a dark LED for a hard build break.
 *
 * THE REAL API (verified): the upper-half RMT character driver exposes
 *
 *     int  rmtchar_register(FAR struct rmt_dev_s *rmt);   /* <nuttx/rmt/rmt.h> */
 *
 * which registers "/dev/rmt<minor>".  esp_rmt_tx_init() returns the handle
 * but does NOT register anything, so the two calls go together:
 *
 *     dev = esp_rmt_tx_init(0, GPIO41);    /* build + configure the channel */
 *     rmtchar_register(dev);               /* publish /dev/rmt0              */
 *     fd  = open("/dev/rmt0", O_WRONLY);   /* then write() raw 4-byte items  */
 *
 * The write path asserts (buflen % 4) == 0, i.e. items are 4 bytes each.
 *
 * NOTE: there is no `rmt_put_items()` symbol.  I referenced one in an
 * intermediate draft of this file; it does not exist.  Do not reintroduce it.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/rmt/rmt.h>

#include <arch/board/board.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RMT item is 32 bits: two level/duration pairs, MSB-first halves.
 *   bits [14:0] duration0, [15] level0, [29:16] duration1, [30] level1
 */

#define WS2812_ITEM(d0, l0, d1, l1) \
  (((uint32_t)((d0) & 0x7fff))           | \
   ((uint32_t)((l0) & 0x1) << 15)        | \
   ((uint32_t)((d1) & 0x7fff) << 16)     | \
   ((uint32_t)((l1) & 0x1) << 31))

/* WS2812 bit timing at an 80 MHz RMT clock (12.5 ns/tick):
 *   bit '1': T1H = 0.80us = 64 ticks high, T1L = 0.45us = 36 ticks low
 *   bit '0': T0H = 0.40us = 32 ticks high, T0L = 0.85us = 68 ticks low
 */

#define WS2812_T1H_TICKS   64
#define WS2812_T1L_TICKS   36
#define WS2812_T0H_TICKS   32
#define WS2812_T0L_TICKS   68

/* >50us low reset/latch pulse.  A 15-bit duration at 12.5 ns/tick spans
 * ~409us, so a single item covers it comfortably.
 */

#define WS2812_RESET_TICKS 4000

#define WS2812_BITS        24
#define WS2812_ITEM_COUNT  (WS2812_BITS + 1)   /* 24 data bits + 1 reset */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_ws2812_inited = false;
static int  g_ws2812_fd     = -1;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_ws2812_initialize
 *
 * Description:
 *   Configure RMT TX channel 0 on BOARD_WS2812_GPIO, publish it as a
 *   character device and open it for writing.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int ai_vox3_ws2812_initialize(void)
{
  FAR struct rmt_dev_s *dev;
  int ret;

  if (g_ws2812_inited)
    {
      return OK;
    }

  /* Build and configure the RMT TX channel.  Returns a handle, or NULL. */

  dev = esp_rmt_tx_init(BOARD_WS2812_RMT_CH, BOARD_WS2812_GPIO);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "ERROR: WS2812: esp_rmt_tx_init(ch%d, io%d) failed\n",
             BOARD_WS2812_RMT_CH, BOARD_WS2812_GPIO);
      return -ENODEV;
    }

  /* Publish it as /dev/rmt<minor>.  esp_rmt_tx_init does NOT do this, so
   * without this call the open() below fails with ENOENT.
   */

  ret = rmtchar_register(dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: WS2812: rmtchar_register failed: %d\n", ret);
      return ret;
    }

  g_ws2812_fd = open("/dev/rmt0", O_WRONLY);
  if (g_ws2812_fd < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "ERROR: WS2812: open(/dev/rmt0) failed: %d\n", ret);
      return ret;
    }

  g_ws2812_inited = true;
  syslog(LOG_INFO, "WS2812 initialized (RMT ch%d, IO%d, /dev/rmt0)\n",
         BOARD_WS2812_RMT_CH, BOARD_WS2812_GPIO);
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_ws2812_set_rgb
 *
 * Description:
 *   Set the single WS2812 pixel to the given RGB color (GRB order on the
 *   wire, MSB first).
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int ai_vox3_ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
  uint32_t items[WS2812_ITEM_COUNT];
  uint8_t  grb[3];
  int      idx = 0;
  int      i;
  int      bit;
  ssize_t  nwritten;

  if (!g_ws2812_inited || g_ws2812_fd < 0)
    {
      return -EAGAIN;
    }

  /* WS2812 expects GRB byte order on the wire. */

  grb[0] = g;
  grb[1] = r;
  grb[2] = b;

  /* Encode 24 bits, MSB first.  Each bit is one item: high pulse then low. */

  for (i = 0; i < 3; i++)
    {
      for (bit = 7; bit >= 0; bit--)
        {
          if ((grb[i] >> bit) & 0x1)
            {
              items[idx++] = WS2812_ITEM(WS2812_T1H_TICKS, 1,
                                         WS2812_T1L_TICKS, 0);
            }
          else
            {
              items[idx++] = WS2812_ITEM(WS2812_T0H_TICKS, 1,
                                         WS2812_T0L_TICKS, 0);
            }
        }
    }

  /* Trailing reset/latch item: line held low. */

  items[idx++] = WS2812_ITEM(WS2812_RESET_TICKS, 0, 0, 0);

  nwritten = write(g_ws2812_fd, items, (size_t)idx * sizeof(uint32_t));
  if (nwritten < 0)
    {
      return -errno;
    }

  return OK;
}
