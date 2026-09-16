/****************************************************************************
 * src/ai_vox3_ws2812.c
 *
 * WS2812 (single addressable RGB LED) driver for the emakefun AI-VOX3.
 *
 *  - GPIO: IO41
 *  - Transport: ESP32-S3 RMT @ 800 kHz, GRB color order.
 *
 * The single pixel is driven by encoding 24 bits (G,R,B) into RMT items
 * (WS2812 bit timing: T0H=0.4us / T0L=0.85us, T1H=0.8us / T1L=0.45us) and
 * transmitting them on the RMT TX channel bound to IO41. A >50 us low
 * "reset" pulse ends the frame.
 *
 * TODO(real-device): confirm RMT clock divider / item timing on the real
 * board; the tick counts below assume an 80 MHz RMT clock (12.5 ns/tick).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include <esp32s3_rmt.h>


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RMT clock assumed 80 MHz -> 12.5 ns per tick. */
#define RMT_TICK_NS            12500

/* WS2812 800 kHz bit timing (ns). */
#define WS2812_T0H_NS          400
#define WS2812_T0L_NS          850
#define WS2812_T1H_NS          800
#define WS2812_T1L_NS          450
#define WS2812_RESET_NS        60000   /* >50 us reset/latch */

#define NS_TO_TICKS(ns)        ((ns) / RMT_TICK_NS)

/* Number of RMT items: 24 bits * 1 item/bit + 1 reset item. */
#define WS2812_ITEM_COUNT      (24 + 1)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_ws2812_inited = false;

/* Local copy of the RMT item type if the arch header uses a different name;
 * rmt_item32_t is the standard ESP32-S3 RMT item (level/duration pairs). */
#ifndef RMT_ITEM32_DEFINED
typedef struct
{
  uint32_t duration0 : 15;
  uint32_t level0     : 1;
  uint32_t duration1 : 15;
  uint32_t level1     : 1;
} rmt_item32_t;
#define RMT_ITEM32_DEFINED 1
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ws2812_encode_bit
 *
 * Description:
 *   Fill one RMT item for a single data bit (b != 0 -> '1' timing).
 *
 ****************************************************************************/

static void ws2812_encode_bit(rmt_item32_t *item, int b)
{
  if (b)
    {
      item->level0   = 1;
      item->duration0 = (uint32_t)NS_TO_TICKS(WS2812_T1H_NS);
      item->level1   = 0;
      item->duration1 = (uint32_t)NS_TO_TICKS(WS2812_T1L_NS);
    }
  else
    {
      item->level0   = 1;
      item->duration0 = (uint32_t)NS_TO_TICKS(WS2812_T0H_NS);
      item->level1   = 0;
      item->duration1 = (uint32_t)NS_TO_TICKS(WS2812_T0L_NS);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_ws2812_initialize
 *
 * Description:
 *   Initialize the RMT TX channel bound to BOARD_WS2812_GPIO. Returns OK.
 *
 ****************************************************************************/

int ai_vox3_ws2812_initialize(void)
{
  int ret;

  if (g_ws2812_inited)
    {
      return OK;
    }

  /* esp32s3_rmt_tx_init configures the channel in TX mode at 800 kHz-ish
   * granularity. Adjust the init signature to match the tree's RMT API. */
  ret = esp32s3_rmt_tx_init(BOARD_WS2812_RMT_CH, BOARD_WS2812_GPIO);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: WS2812 RMT init failed: %d\n", ret);
      return ret;
    }

  g_ws2812_inited = true;
  syslog(LOG_INFO, "WS2812 (RMT ch%d, IO%d) initialized\n",
         BOARD_WS2812_RMT_CH, BOARD_WS2812_GPIO);
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_ws2812_set_rgb
 *
 * Description:
 *   Set the single WS2812 pixel to the given RGB color (GRB order on wire).
 *   Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
  rmt_item32_t items[WS2812_ITEM_COUNT];
  int i;
  int bit;
  uint8_t grb[3];

  if (!g_ws2812_inited)
    {
      return -EAGAIN;
    }

  /* WS2812 expects GRB byte order on the wire. */
  grb[0] = g;
  grb[1] = r;
  grb[2] = b;

  memset(items, 0, sizeof(items));

  /* Encode 24 bits (3 bytes, MSB first). */
  for (i = 0; i < 3; i++)
    {
      for (bit = 7; bit >= 0; bit--)
        {
          ws2812_encode_bit(&items[i * 8 + (7 - bit)], (grb[i] >> bit) & 0x1);
        }
    }

  /* Final reset item: hold low for >50 us. */
  items[24].level0    = 0;
  items[24].duration0 = (uint32_t)NS_TO_TICKS(WS2812_RESET_NS);
  items[24].level1    = 0;
  items[24].duration1 = 0;

  return esp32s3_rmt_tx(BOARD_WS2812_RMT_CH, items, WS2812_ITEM_COUNT);
}
