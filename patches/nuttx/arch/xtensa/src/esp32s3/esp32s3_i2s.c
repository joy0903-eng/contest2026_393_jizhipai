/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_i2s.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/* [V644] PROBE: direct ROM console -- does NOT go through the
 * /dev/kmsg ring buffer, so it survives a panic dump.  Declared
 * here because this file uses it before the old [V644] block. */
extern int esp_rom_printf(const char *fmt, ...);
#include <syslog.h>

#ifdef CONFIG_ESP32S3_I2S

#include <debug.h>
#include <sys/param.h>
#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/clock.h>
#include <nuttx/cache.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/mqueue.h>
#include <nuttx/circbuf.h>
#include <nuttx/nuttx.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>

#include <arch/board/board.h>

#include "esp32s3_i2s.h"
#include "esp32s3_gpio.h"
#include "esp32s3_irq.h"
#include "esp32s3_dma.h"

#include "xtensa.h"
#include "hardware/esp32s3_gpio_sigmap.h"
#include "hardware/esp32s3_system.h"
#include "hardware/esp32s3_i2s.h"
#include "hardware/esp32s3_soc.h"
#include "hardware/esp32s3_iomux.h"
#include "hardware/esp32s3_pinmap.h"
#include "hardware/esp32s3_dma.h"

/* The TX FIFO is the first register of the block. */
#define I2S_TX_FIFO_REG(i)   (REG_I2S_BASE(i) + 0x00)

/* [V356] Compiled-out diagnostics.
 *
 * I2S_V350_REARM_PROBE: re-issuing DMA_OUTLINK_START against descriptors
 *   the FSM has already consumed is rejected (OUT_DSCR_ERR) because
 *   AUTO_WRBACK cleared their OWNER; measured dsc_err=6 == one error per
 *   loop iteration.  It also let the GDMA raise completion events while
 *   the transfer was not yet queued as active, which dropped them.
 * I2S_I2SREG_DUMP: read-only register re-sample, but it stalls the caller
 *   for 40 ms mid-stream and buries the real evidence in log noise.
 */
#define I2S_V350_REARM_PROBE 0
#define I2S_I2SREG_DUMP      0

/* Bits the NuttX register header does not name. */
#define DMA_RST_BIT       (1u << 0)
#define GDMA_INT_DONE     (1u << 0)
#define GDMA_INT_SUC_EOF  (1u << 2)
#define GDMA_INT_DSCR_ERR (1u << 3)

/* [V209/V214] I2S TX diagnostics.
 *
 * g_txirq_n is only COUNTED from the ISR (logging there is unsafe); a non-zero
 * value proves the GDMA TX completion interrupt fired at least once.
 * g_txdump_n bounds the register dump to the first few transfers.
 * g_dbg_link_after_enable is sampled inside the spinlocked start path and
 * printed later, so we can tell whether OUTLINK_START was ever really set. */
static volatile int      g_txirq_n;

/* [V342] TX GDMA interrupt forensics.
 *
 * The pre-V342 handler read DMA_OUT_INT_ST, cleared it immediately and then
 * tested it only against DMA_OUT_TOTAL_EOF (bit 3).  Any other event --
 * OUT_DONE (bit 0), OUT_EOF/SUC_EOF (bit 1) or OUT_DSCR_ERR (bit 2) -- was
 * silently swallowed, and because the status register had already been
 * cleared every later register dump read 0 and the real cause was invisible.
 *
 * g_tx_isr_status latches the first non-zero status word (never overwritten),
 * g_tx_isr_n[] counts each event separately, and g_tx_isr_err counts
 * descriptor errors.  All are written from the ISR with plain stores and read
 * by the (non-interrupt) diagnostic dump. */
static volatile uint32_t g_tx_isr_status;
static volatile unsigned int g_tx_isr_n[4];   /* idx: 0=DONE 1=EOF 2=ERR 3=TOTEOF */
static volatile unsigned int g_tx_isr_err;

/* [V345] Completion-path forensics.  These separate "the ISR never scheduled
 * a completion" from "it scheduled but the worker/callback never ran". */
static volatile unsigned int g_sched_n;      /* i2s_tx_schedule() entries     */
static volatile unsigned int g_sched_noact;  /* ... with tx.act empty         */
static volatile unsigned int g_sched_match;  /* ... descriptor matched        */
static volatile unsigned int g_worker_n;     /* i2s_tx_worker() entries       */
static volatile unsigned int g_worker_cb;    /* ... callbacks invoked         */

/* [V233] Streaming state for the FIFO writer.  audio_playback_write() splits
 * PCM into ~4 kB chunks, so the writer is called several times a second;
 * re-running the TX reset/start sequence per chunk would insert an audible
 * discontinuity at every chunk boundary.  Only the first chunk brings the TX
 * side up, and the pacing reference is carried across calls so the stream
 * stays gap-free and at the right rate. */
static int                g_tx_streaming;
static long long          g_tx_start_ns;   /* stream t0                     */
static long long          g_tx_delivered;  /* samples already delivered     */
static int               g_txdump_n;
/* [V354] Write the descriptors and the sample block back to SRAM before
 * the GDMA reads them.  The kernel is built with
 * CONFIG_ESP32S3_DATA_CACHE_32KB / 8-way / 32 B lines (see nuttx/.config)
 * and no esp32s3 driver performs any cache maintenance, so everything the
 * CPU writes into dma_link[] and into the bounce buffer can still be
 * sitting in D-cache when the GDMA engine walks it.  Set to 0 to A/B this
 * without rebuilding anything else. */
static int               g_i2s_tx_cache_clean = 1;
static volatile uint32_t g_dbg_link_after_enable;
static int               g_tx_cfg_done;   /* [V346] V264 sequence ran once    */

/* [V94] Receive-path counters (top-of-file copy so every user sees them).
 * Plain globals, no blocking calls: the DMA setup path runs with the driver
 * spinlock held and interrupts off.  The capture thread beacons them. */

volatile unsigned int g_v94_recv;      /* i2s_receive() calls            */
volatile int          g_v94_setupret;  /* i2s_rxdma_setup() last return  */
volatile unsigned int g_v94_start;     /* i2s_rxdma_start() calls        */
volatile unsigned int g_v94_isr;       /* rx interrupt entries           */
volatile unsigned int g_v94_eof;       /* rx end-of-frame interrupts     */
volatile unsigned int g_v94_pb;        /* es8311_processbegin() calls    */
volatile int          g_v94_i2sret;    /* last I2S_RECEIVE return        */
volatile unsigned int g_v94_done;      /* es8311_processdone() calls     */
volatile int          g_v94_rxonly;    /* current direction override     */


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2S DMA RX/TX description number */

#define I2S_DMADESC_NUM                 (CONFIG_I2S_DMADESC_NUM)

/* I2S Clock */

#define I2S_LL_BASE_CLK                 (2 * APB_CLK_FREQ)

/* [V216] The divider maths below are derived from the selected source clock.
 * NuttX hard-codes CLK160 (2 * APB) and never checks that the source is
 * actually running.  Make it switchable so we can fall back to XTAL when the
 * I2S clock domain turns out to be dead. */
/* [V670] *** The I2S divider base on THIS board. ***
 *
 * The bring-up asked for I2S_PLL_F160M_CLK and told the divider maths that the
 * source runs at 160 MHz (2 * APB_CLK_FREQ).  Two independent measurements say
 * it actually runs at 150 MHz:
 *
 *   1. Pads, 50 ms CCOUNT-bounded windows, reproducible:
 *        BCLK = 470376 Hz / 490679 Hz  ->  MCLK = BCLK * 8 = 3.76-3.92 MHz
 *        MCLK = sclk / (39 + 1/16)     ->  sclk = 147-153 MHz
 *      Both figures read 6% below the 512 kHz / 4.096 MHz the registers claim.
 *
 *   2. Playback accounting: 31232 samples consumed over 500440024 CPU cycles
 *      = 16022 cycles per sample.  CCOUNT runs at 240 MHz and one sample is
 *      one LRCK frame, so the real frame rate is 240e6 / 16022 = 14979 Hz.
 *
 * 15000 / 16000 is exactly 15/16, i.e. exactly 150 MHz against 160 MHz.
 *
 * Effect of the old assumption: with mclk_div latched at 39 + 1/16 the real
 * MCLK came out at 3.84 MHz instead of 4.096 MHz, so BCLK was 480 kHz instead
 * of 512 kHz and the frame rate 15000 Hz instead of 16000 Hz -- playback ran
 * 6.25% slow, roughly a semitone flat. */
#define I2S_PLL_F160M_HZ   150000000u

static uint32_t g_i2s_sclk = I2S_PLL_F160M_HZ;

/* [V668] Desired TX channel mode, remembered by i2s_txchannels() so that
 * i2s_txdma_start() can re-latch it inside its TX-stop window.  Kept next to
 * the clock globals because it has exactly the same lifetime: it describes
 * how the transmitter must be programmed, independent of which of the
 * several reset sites in this file happens to run next. */
static int g_i2s_tx_mono;

/* [V680] RX mirror of g_i2s_tx_mono.  priv->channels cannot be reused as
 * the authority here because it is shared with the TX half, and the RX TDM
 * slot mask must be reprogrammed on every arm anyway. */
static int g_i2s_rx_mono;

/* [V671] *** Light re-arm latch. ***
 *
 * i2s_txdma_start() runs on EVERY buffer and used to re-run the whole
 * V343/V348 configure-reset-start block each time, I2S_TX_RESET and
 * I2S_TX_FIFO_RESET included.  The FIFO reset throws away the ~64 samples
 * (about 4 ms at 16 kHz) that are still buffered in it, so every 2048-byte
 * chunk of TTS audio arrived with a hole in front of it -- roughly thirty
 * dropouts over a two second sentence.  That is the reported stutter.
 *
 * V348 made the block unconditional because, at the time, skipping it left
 * the transmitter idle (STATE=TX_IDLE, outlink parked, OWNER stuck) and the
 * transfer never completed.  That was measured with the TDM channel enable
 * bug and the wrong divider base still in place, i.e. before v669/v670.  Now
 * that CONF, TDM and the clock are right, a transfer that finds the module
 * already configured only needs its descriptor handed to the GDMA.
 *
 * g_v671_cfg_valid is 1 once the module has been fully configured and the
 * transmitter is running.  Every site that pulses I2S_TX_RESET clears it
 * again, so the next transfer reconfigures from scratch: i2s_configure(),
 * i2s_clock_try_source(), i2s_tx_channel_start() and i2s_tx_channel_stop().
 * audio_playback_open() goes through i2s_txsamplerate() ->
 * i2s_clock_try_source() on every speak, so the first transfer of each
 * utterance is always a full configure and the rest are light re-arms.
 *
 * If the light path ever turns out to be unsafe it announces itself loudly:
 * the TX never drains, so audio_playback.c reports
 * "I2S TX completion TIMEOUT after 1.5 s" and there is no sound at all. */
static int g_v671_cfg_valid;
static int g_v671_full_n;
static int g_v671_light_n;
static int      g_sclk_fallback;      /* 1 once we switched to XTAL  */
static int      g_tx_update_spins;    /* spins waiting for tx_update */
static int      g_tx_update_spins2;   /* same, after the fallback    */
static int      g_tx_start_spins;     /* [V343] spins for the post-start update */
static int      g_tx_stop_spins;      /* [V347] spins for the pre-start stop handshake */
static uint32_t g_tx_state_pre;       /* [V347] I2S_STATE before the stop/start toggle */
static uint32_t g_tx_state_post;      /* [V347] I2S_STATE after  the stop/start toggle */
static int      g_tx_channel_start_er;/* [V347] i2s_tx_channel_start() early-return count */
static int      g_tx_txdma_start_n;   /* [V347] i2s_txdma_start() prologue count  */
static int      g_tx_txdma_bail_act;  /* [V347] i2s_txdma_start() bail: tx.act non-empty */
static int      g_tx_txdma_bail_pend; /* [V347] i2s_txdma_start() bail: tx.pend empty    */

/* [V362] Rate accounting + idle-spin attribution. */
static int      g_tx_rate_calls;     /* i2s_txdma_start() total entries   */
static int      g_tx_rate_starts;    /* entries that armed the DMA        */
static int      g_tx_rate_bail_act;  /* early bail: tx.act non-empty      */
static int      g_tx_rate_bail_pend; /* early bail: tx.pend EMPTY (idle!) */
static int      g_tx_rate_skip;      /* cheap gate for the clock read     */
static int64_t  g_tx_rate_next_ms;   /* next 5 s summary deadline         */

/* [V362] Which call site is on the stack, so the 5 s log can attribute
 * every idle spin.  Set immediately before each i2s_txdma_start() call:
 *   1 = i2s_txdma_setup()   (enqueue / write path)
 *   2 = i2s_tx_schedule()   (GDMA EOF interrupt, ISR context)
 */
static int      g_tx_callsite;
static int      g_tx_rate_bail_s1;   /* idle bails attributed to site 1   */
static int      g_tx_rate_bail_s2;   /* idle bails attributed to site 2   */

/* [V364] How often the TX completion interrupt sources were gated. */
static int      g_tx_irq_mask_n;    /* masked: both queues empty        */
static int      g_tx_irq_unmask_n;  /* unmasked: a buffer was queued    */

/* [V362] Req #3: memoized TX clock.  i2s_set_clock() derives the dividers
 * with soft-float arithmetic (there is no FPU on LX7 under NuttX) and it
 * runs inside i2s_txdma_start(), which i2s_tx_schedule() calls straight
 * from the GDMA EOF ISR.  Once derived for a given parameter set we
 * simply re-apply the latched register values instead of redoing the
 * math.  The values are keyed on every input i2s_set_clock() reads.
 */
static int      g_txclk_valid;
static uint32_t g_txclk_sclk;
static uint32_t g_txclk_rate;
static uint32_t g_txclk_slot;
static uint32_t g_txclk_width;
static uint32_t g_txclk_mclk;
static uint32_t g_txclk_role;
static uint32_t g_txclk_clkm;
static uint32_t g_txclk_div;
static uint32_t g_txclk_conf1;
static int      g_rx_update_spins;    /* spins waiting for rx_update */
#define I2S_LL_MCLK_DIVIDER_BIT_WIDTH   (6)
#define I2S_LL_MCLK_DIVIDER_MAX         ((1 << I2S_LL_MCLK_DIVIDER_BIT_WIDTH) - 1)

/* I2S DMA channel number */

#define I2S_DMA_CHANNEL_MAX (2)

#ifdef CONFIG_ESP32S3_I2S0_TX
#  define I2S0_TX_ENABLED 1
#  define I2S_HAVE_TX 1
#else
#  define I2S0_TX_ENABLED 0
#endif

#ifdef CONFIG_ESP32S3_I2S0_RX
#  define I2S0_RX_ENABLED 1
#  define I2S_HAVE_RX 1
#else
#  define I2S0_RX_ENABLED 0
#endif

#ifdef CONFIG_ESP32S3_I2S1_TX
#  define I2S1_TX_ENABLED 1
#  define I2S_HAVE_TX 1
#else
#  define I2S1_TX_ENABLED 0
#endif

#ifdef CONFIG_ESP32S3_I2S1_RX
#  define I2S1_RX_ENABLED 1
#  define I2S_HAVE_RX 1
#else
#  define I2S1_RX_ENABLED 0
#endif

/* Debug ********************************************************************/

#ifdef CONFIG_DEBUG_I2S_INFO
#  define CONFIG_ESP32S3_I2S_DUMPBUFFERS
#else
#  undef CONFIG_ESP32S3_I2S_DUMPBUFFERS
#endif

#define I2S_GPIO_UNUSED -1      /* For signals which are not used */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Role of the I2S port */

typedef enum
{
  I2S_ROLE_MASTER,  /* I2S controller master role, bclk and ws signal will be set to output */
  I2S_ROLE_SLAVE    /* I2S controller slave role, bclk and ws signal will be set to input */
} i2s_role_t;

/* Data width of the I2S channel */

typedef enum
{
  I2S_DATA_BIT_WIDTH_8BIT   = 8,    /* I2S channel data bit-width: 8 */
  I2S_DATA_BIT_WIDTH_16BIT  = 16,   /* I2S channel data bit-width: 16 */
  I2S_DATA_BIT_WIDTH_24BIT  = 24,   /* I2S channel data bit-width: 24 */
  I2S_DATA_BIT_WIDTH_32BIT  = 32,   /* I2S channel data bit-width: 32 */
} i2s_data_bit_width_t;

/* Multiplier of MCLK to sample rate */

typedef enum
{
  I2S_MCLK_MULTIPLE_128 = 128,  /* mclk = sample_rate * 128 */
  I2S_MCLK_MULTIPLE_256 = 256,  /* mclk = sample_rate * 256 */
  I2S_MCLK_MULTIPLE_384 = 384,  /* mclk = sample_rate * 384 */
  I2S_MCLK_MULTIPLE_512 = 512,  /* mclk = sample_rate * 512 */
} i2s_mclk_multiple_t;

/* I2S Clock Source */

typedef enum
{
  I2S_XTAL_CLK = 0,
  I2S_PLL_D2_CLK,
  I2S_PLL_F160M_CLK,
  I2S_MCLK_IN,
} i2s_clk_src_t;

/* I2S Audio Standard Mode */

typedef enum
{
  I2S_TDM_PHILIPS = 0,
  I2S_TDM_MSB,
  I2S_TDM_PCM,
  I2S_PDM,
} i2s_audio_mode_t;

/* Role of the I2S port */

typedef enum
{
  I2S_RX,   /* I2S controller receiver (RX) */
  I2S_TX    /* I2S controller transmitter (TX) */
} i2s_dir_t;

/* I2S Device hardware configuration */

struct esp32s3_i2s_config_s
{
  uint32_t port;              /* I2S port */
  uint32_t role;              /* I2S port role (master or slave) */
  uint8_t data_width;         /* I2S sample data width */
  uint32_t rate;              /* I2S sample-rate */
  uint32_t total_slot;        /* Total slot number */

  bool tx_en;                 /* Is TX enabled? */
  bool rx_en;                 /* Is RX enabled? */
  int8_t mclk_pin;            /* MCLK pin, output */

  int tx_clk_src;             /* Select the I2S TX source clock */
  int rx_clk_src;             /* Select the I2S TX source clock */

  /* BCLK pin, input in slave role, output in master role */

  int8_t bclk_pin;

  /* WS pin, input in slave role, output in master role */

  int8_t ws_pin;

  int8_t dout_pin;            /* DATA pin, output */
  int8_t din_pin;             /* DATA pin, input */

  uint32_t bclk_in_insig;     /* RX channel BCK signal (slave mode) index */
  uint32_t bclk_in_outsig;    /* RX channel BCK signal (master mode) index */
  uint32_t bclk_out_insig;    /* TX channel BCK signal (slave mode) index */
  uint32_t bclk_out_outsig;   /* TX channel BCK signal (master mode) index */
  uint32_t ws_in_insig;       /* RX channel WS signal (slave mode) index */
  uint32_t ws_in_outsig;      /* RX channel WS signal (master mode) index */
  uint32_t ws_out_insig;      /* TX channel WS signal (slave mode) index */
  uint32_t ws_out_outsig;     /* TX channel WS signal (master mode) index */
  uint32_t din_insig;         /* RX channel Data Input signal index */
  uint32_t dout_outsig;       /* TX channel Data Output signal index */
  uint32_t mclk_out_sig;      /* Master clock output index */

  uint8_t  audio_std_mode;    /* Select audio standard (i2s_audio_mode_t) */

  /* WS signal polarity, set true to enable high level first */

  bool ws_pol;
};

struct esp32s3_buffer_s
{
  struct esp32s3_buffer_s *flink; /* Supports a singly linked list */

  /* The associated DMA in/outlink */

  struct esp32s3_dmadesc_s dma_link[I2S_DMADESC_NUM];

  i2s_callback_t callback;      /* DMA completion callback */
  uint32_t timeout;             /* Timeout value of the DMA transfers */
  void *arg;                    /* Callback's argument */
  struct ap_buffer_s *apb;      /* The audio buffer */
  uint8_t *buf;                 /* The DMA's descriptor buffer */
  uint32_t nbytes;              /* The DMA's descriptor buffer size */
  int result;                   /* The result of the transfer */
};

/* Internal buffer must be aligned to the bytes_per_sample. Sometimes,
 * however, the audio buffer is not aligned and additional bytes must
 * be copied to be inserted on the next buffer. This structure keeps
 * track of the bytes that were not written to the internal buffer yet.
 */

struct esp32s3_buffer_carry_s
{
  uint32_t value;
  size_t bytes;
};

/* This structure describes the state of one receiver or transmitter
 * transport.
 */

struct esp32s3_transport_s
{
  sq_queue_t pend;              /* A queue of pending transfers */
  sq_queue_t act;               /* A queue of active transfers */
  sq_queue_t done;              /* A queue of completed transfers */
  struct work_s work;           /* Supports worker thread operations */

  /* Bytes to be written at the beginning of the next DMA buffer */

  struct esp32s3_buffer_carry_s carry;
};

/* The state of the one I2S peripheral */

struct esp32s3_i2s_s
{
  struct i2s_dev_s  dev;        /* Externally visible I2S interface */
  mutex_t           lock;       /* Ensures mutually exclusive access */
  uint8_t           cpu;        /* CPU ID */
  spinlock_t        slock;      /* Device specific lock. */

  /* Port configuration */

  const struct esp32s3_i2s_config_s *config;

  uint32_t    mclk_freq;      /* I2S actual master clock */
  uint32_t    mclk_multiple;  /* The multiple of mclk to the sample rate */
  uint32_t    channels;       /* Audio channels (1:mono or 2:stereo) */
  uint32_t    rate;           /* I2S actual configured sample-rate */
  uint32_t    data_width;     /* I2S actual configured data_width */
  uint32_t    dma_channel;    /* I2S DMA channel being used */

#ifdef I2S_HAVE_TX
  struct esp32s3_transport_s tx;  /* TX transport state */

  int  tx_irq;                    /* TX IRQ */
  bool tx_started;                /* TX channel started */
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
  struct esp32s3_transport_s rx;  /* RX transport state */

  int  rx_irq;                    /* RX IRQ */
  bool rx_started;                /* RX channel started */
#endif /* I2S_HAVE_RX */

  bool streaming;                 /* Is I2S peripheral active? */

  /* Pre-allocated pool of buffer containers */

  sem_t bufsem;                         /* Buffer wait semaphore */
  struct esp32s3_buffer_s *bf_freelist; /* A list a free buffer containers */
  struct esp32s3_buffer_s containers[CONFIG_ESP32S3_I2S_MAXINFLIGHT];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register helpers */

#ifdef CONFIG_ESP32S3_I2S_DUMPBUFFERS
#  define       i2s_dump_buffer(m,b,s) lib_dumpbuffer(m,b,s)
#else
#  define       i2s_dump_buffer(m,b,s)
#endif

/* Buffer container helpers */

/* [V211] CRITICAL: I2S GDMA on the ESP32-S3 can only fetch buffers from
 * internal SRAM, but this board registers its 8 MB PSRAM as the common heap
 * (CONFIG_ESP32S3_SPIRAM_COMMON_HEAP=y), so the per-transfer calloc() in
 * i2s_txdma_setup() handed back PSRAM addresses (measured: 0x3c16a080).
 * esp32s3_dma_setup() only compensates for PSRAM on the RX side
 * ("if (!tx && buffer_in_psram)"), so on TX the descriptor simply pointed at
 * memory the DMA cannot read: the transfer made no progress at all, the GDMA
 * completion interrupt never fired (txirqN=0, OUTLINK_START cleared) and
 * i2s_send() blocked forever -- which presented as "the speaker is silent and
 * the board stops logging".
 *
 * The DMA bounce buffers therefore come from a statically allocated
 * internal-SRAM pool (.bss is always internal), which is guaranteed
 * DMA-capable.  Slots are sized to cover the maximum transfer the I2S upper
 * half may queue (ESP32S3_DMA_BUFLEN_MAX * I2S_DMADESC_NUM). */

/* Slot size matches AGENT_AUDIO_CHUNK_MAX (audio_playback.c), which caps
 * every transfer the application queues.  The pool is deliberately small:
 * internal DRAM on this board has only ~90 KB of heap headroom, so a large
 * BSS pool pushes _sheap past the ROM-reserved top of DRAM0 and the board no
 * longer starts at all.
 *
 * [V317] Raised 2 -> 6.  The old [V312] note blamed a 6-slot pool for
 * the WiFi reason=2/205 storm via ".bss address shift".  That
 * attribution is DISPROVED: V316 grew .bss by ~49 KB (IOB pool,
 * CONFIG_IOB_NBUFFERS 64->160) and WiFi then associated on the FIRST
 * try ([V103] LINKED try#0) with DHCP succeeding first attempt.  The
 * real storm cause was the authmode threshold + faced CPU starvation,
 * both fixed long ago.
 *
 * With 2 slots the capture side (audio_capture_native arms 2 RX
 * buffers) consumes the WHOLE pool, so TX can never get a bounce slot:
 *   [audio_direct] i2s_send failed: -12   (playback, ENOMEM)
 *   [AUDIO-R42] xfer ret=-12              (capture,  ENOMEM)
 * 6 slots leaves 4 free while capture is armed. */
/* [V372] RESTORED to 6.  The [V317] comment above already argued for 6 and
 * even says "raised 2 -> 6", but the literal had been put back to 2:
 * with 2 slots the capture side (audio_capture_native arms 2 RX
 * buffers) consumes the WHOLE pool, so the RX DMA never delivers and
 * audio_capture_native gives up with -EIO after six empty waits --
 * which is exactly the "capture EIO, retry 1/5..5/5" + "no speech for
 * 8000 ms" seen in the V371 log.  6 slots leaves 4 free. */
/* [V377] 5 -> 4.  Capture arms 2 buffers ("[audio_cap_nat] capture started
 * (2 buffers armed)"), so 4 still leaves 2 spare, and the 4096 bytes freed
 * here pay for the enlarged es8311 worker stacks (2 x 2048).  Raising this
 * to 6 is still not an option: it overflows dram0_0_seg by 3056 bytes. */
/* [V638] 2 -> 4.  The V317/V372/V377 notes above all describe 4 (or 6)
 * slots, but the literal had been left at 2 -- which is exactly the case
 * those notes call out as broken: audio_capture_native arms 2 RX buffers,
 * so a 2-slot pool is fully consumed the moment recording starts and the
 * TX / internal-loopback leg of the same codec gets nothing.  That is the
 * state in which the es8311 worker died with
 *   xtensa_user_panic: User Exception: EXCCAUSE=001c task: es8311
 * 4 slots leaves 2 spare.  V377 measured 6 as overflowing dram0_0_seg by
 * 3056 bytes; since then RAMLOG (-24 KB), IOB (-83 KB) and the TLS scratch
 * buffer (-4 KB) were all trimmed, so 4 is comfortably affordable. */
#define I2S_DMABUF_SLOTS     4
#define I2S_DMABUF_SLOT_SZ   4096

static uint8_t g_i2s_dmabuf[I2S_DMABUF_SLOTS][I2S_DMABUF_SLOT_SZ]
               __attribute__((aligned(16)));
static uint8_t g_i2s_dmabuf_used[I2S_DMABUF_SLOTS];

static void *i2s_dmabuf_alloc(size_t nbytes)
{
  irqstate_t flags;
  int i;

  if (nbytes > I2S_DMABUF_SLOT_SZ)
    {
      i2serr("DMA buffer request %u exceeds the internal-SRAM pool slot "
             "size %u\n", (unsigned)nbytes, I2S_DMABUF_SLOT_SZ);
      return NULL;
    }

  flags = spin_lock_irqsave(NULL);
  for (i = 0; i < I2S_DMABUF_SLOTS; i++)
    {
      if (!g_i2s_dmabuf_used[i])
        {
          g_i2s_dmabuf_used[i] = 1;
          spin_unlock_irqrestore(NULL, flags);
          return g_i2s_dmabuf[i];
        }
    }

  spin_unlock_irqrestore(NULL, flags);
  return NULL;
}

static void i2s_dmabuf_release(void *p)
{
  irqstate_t flags;
  int i;

  if (p == NULL)
    {
      return;
    }

  flags = spin_lock_irqsave(NULL);
  for (i = 0; i < I2S_DMABUF_SLOTS; i++)
    {
      if (p == (void *)g_i2s_dmabuf[i])
        {
          g_i2s_dmabuf_used[i] = 0;
          break;
        }
    }

  spin_unlock_irqrestore(NULL, flags);
}

static struct esp32s3_buffer_s *
                i2s_buf_allocate(struct esp32s3_i2s_s *priv);
static void     i2s_buf_free(struct esp32s3_i2s_s *priv,
                             struct esp32s3_buffer_s *bfcontainer);
static int      i2s_buf_initialize(struct esp32s3_i2s_s *priv);

/* DMA support */

#ifdef I2S_HAVE_TX
static IRAM_ATTR int  i2s_txdma_setup(struct esp32s3_i2s_s *priv,
                                      struct esp32s3_buffer_s *bfcontainer);
static void           i2s_tx_worker(void *arg);
static void           i2s_tx_schedule(struct esp32s3_i2s_s *priv,
                                      struct esp32s3_dmadesc_s *outlink);
/* [V338] forward decl: i2s_txdma_start() 定义在 helper 之前 */
static void           i2s_apply_tx_gdma_strategy(struct esp32s3_i2s_s *priv);
static void     i2s_tx_rate_tick(void);   /* [V362] 5 s summary logger  */
static void     i2s_tx_irq_mask(struct esp32s3_i2s_s *priv);
static void     i2s_tx_irq_unmask(struct esp32s3_i2s_s *priv);
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
static IRAM_ATTR int  i2s_rxdma_setup(struct esp32s3_i2s_s *priv,
                                      struct esp32s3_buffer_s *bfcontainer);
static void           i2s_rx_worker(void *arg);
static void           i2s_rx_schedule(struct esp32s3_i2s_s *priv,
                                      struct esp32s3_dmadesc_s *outlink);
#endif /* I2S_HAVE_RX */

/* I2S methods (and close friends) */

static int32_t  i2s_check_mclkfrequency(struct esp32s3_i2s_s *priv);
static uint32_t i2s_set_datawidth(struct esp32s3_i2s_s *priv);
static uint32_t i2s_set_clock(struct esp32s3_i2s_s *priv);
static uint32_t i2s_getmclkfrequency(struct i2s_dev_s *dev);
static uint32_t i2s_setmclkfrequency(struct i2s_dev_s *dev,
                                     uint32_t frequency);
static int      i2s_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg);

#ifdef I2S_HAVE_TX
static void     i2s_tx_channel_start(struct esp32s3_i2s_s *priv);
static void     i2s_tx_channel_stop(struct esp32s3_i2s_s *priv);
static int      i2s_txchannels(struct i2s_dev_s *dev, uint8_t channels);
#define I2S_UPDATE_SPIN_MAX 200000

/* [V216] Does the I2S TX clock domain actually advance?
 *
 * I2S_TX_UPDATE hands the APB-domain configuration to the I2S TX clock domain
 * and clears itself when the transfer completes.  ESP-IDF spins on it before
 * setting tx_start; NuttX never waited.  If it never clears, the selected
 * source clock (CLK160) is not running and NOTHING on the TX side can work --
 * which from the GDMA side looks exactly like "the outlink parks without ever
 * fetching a descriptor". */
static int i2s_clock_domain_probe(int port)
{
  int spins = 0;

  modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
  while (spins < I2S_UPDATE_SPIN_MAX &&
         (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE))
    {
      spins++;
    }

  return spins;
}

/* [V341] *** THE SINGLE PLACE THAT SELECTS THE I2S SOURCE CLOCK ***
 *
 * Ground truth for the field encoding (esp-hal-3rdparty, esp32s3
 * hal/esp32s3/include/hal/i2s_ll.h, i2s_ll_tx_clk_set_src()):
 *     I2S_CLK_SRC_XTAL      -> tx_clk_sel = 0   (40 MHz crystal)
 *     I2S_CLK_SRC_PLL_160M  -> tx_clk_sel = 2   (PLL_F160M, fixed 160 MHz)
 * which is exactly the driver's own i2s_clk_src_t enum.  The value the driver
 * had been writing (2) is therefore PLL_F160M, not "PLL_D2".
 *
 * The measured failure (I2S_TX_CLKM_CONF = 0x34000027, CLK_SEL = 2, CLK_EN = 1,
 * CLK_ACTIVE = 1) coincides with I2S_TX_UPDATE never self-clearing
 * (spins = 200000).  tx_update is the handshake that copies the APB-domain
 * configuration into the I2S TX clock domain; it only completes while that
 * domain is actually being clocked.  So a stuck tx_update == a stalled TX
 * clock domain == the I2S never requests DMA data == PARK / DSCR=0 / silence.
 *
 * Before this patch the source was changed in three unrelated places
 * (i2s_configure(), i2s_txsamplerate() and esp32s3_i2s_set_rxonly()) and the
 * DMA start path additionally hard-coded a 160 MHz divider value
 * (0x003c0201), so the registers could disagree.  Now:
 *
 *   1. i2s_clk_src_select()      - the ONLY writer of I2S_TX/RX_CLK_SEL.
 *   2. i2s_clock_try_source()    - selects a source, programs every divider
 *                                  for it (via i2s_set_clock()) and measures
 *                                  the tx_update handshake.
 *   3. i2s_clock_source_bringup()- tries PLL_F160M first (the source the
 *                                  reference firmware used and the hardware
 *                                  default), and if that domain does not
 *                                  advance, falls back to XTAL (40 MHz), which
 *                                  is guaranteed to run because every other
 *                                  clock in the SoC is derived from it.
 *
 * The chosen source is remembered in g_i2s_clk_sel and the corresponding
 * divider base in g_i2s_sclk, so i2s_set_clock() and the DMA start path stay
 * consistent with it. */

static uint32_t g_i2s_clk_sel = I2S_PLL_F160M_CLK;

static void i2s_clk_src_select(int port, uint32_t sel)
{
  /* Open the module clock gate (bit 29 of TX_CLKM_CONF). */

  modifyreg32(I2S_TX_CLKM_CONF_REG(port), 0, I2S_CLK_EN);

  /* TX: pick the source, then mark the TX clock domain active. */

  modifyreg32(I2S_TX_CLKM_CONF_REG(port), I2S_TX_CLK_SEL_M,
              FIELD_TO_VALUE(I2S_TX_CLK_SEL, sel));
  modifyreg32(I2S_TX_CLKM_CONF_REG(port), 0, I2S_TX_CLK_ACTIVE);

  /* RX: keep the same source so a full-duplex stream cannot tear. */

#ifdef I2S_RX_CLK_SEL_M
  modifyreg32(I2S_RX_CLKM_CONF_REG(port), I2S_RX_CLK_SEL_M,
              FIELD_TO_VALUE(I2S_RX_CLK_SEL, sel));
  modifyreg32(I2S_RX_CLKM_CONF_REG(port), 0, I2S_RX_CLK_ACTIVE);
#endif
}

static int i2s_clock_try_source(struct esp32s3_i2s_s *priv, uint32_t sel,
                                uint32_t hz, uint32_t *ratep)
{
  int port = priv->config->port;

  /* Drop any pending configuration latch left over from a previous attempt
   * so the next tx_update is a fresh rising edge.  tx_reset/tx_fifo_reset do
   * not clear the APB-domain configuration registers (verified against the
   * V264 read-back, and ESP-IDF relies on the same ordering). */

  modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_RESET);
  modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_RESET, 0);
  modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_FIFO_RESET);
  modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_FIFO_RESET, 0);

  g_v671_cfg_valid = 0;   /* [V671] the reset above dropped the module state */

  /* TX must take its own BCK/WS, never the RX side's (see V250). */

  modifyreg32(I2S_TX_CONF_REG(port), I2S_SIG_LOOPBACK, 0);

#ifdef I2S_RX_CLK_SEL_M
  modifyreg32(I2S_RX_CONF_REG(port), 0, I2S_RX_RESET);
  modifyreg32(I2S_RX_CONF_REG(port), I2S_RX_RESET, 0);
#endif

  /* Base frequency for the divider maths, then every divider for this
   * source (integer part, fractional part and BCK divider). */

  g_i2s_sclk = hz;
  i2s_clk_src_select(port, sel);

  if (ratep != NULL)
    {
      *ratep = i2s_set_clock(priv);
    }
  else
    {
      (void)i2s_set_clock(priv);
    }

  /* Does the selected source actually advance the TX clock domain? */

  return i2s_clock_domain_probe(port);
}

static uint32_t i2s_clock_source_bringup(struct esp32s3_i2s_s *priv)
{
  int port = priv->config->port;
  int spins;
  uint32_t rate = 0;

  /* Attempt 1: PLL_F160M -- the hardware default.  The divider base is the
   * measured 150 MHz, not the 160 MHz the pin is named after (see
   * I2S_PLL_F160M_HZ above). */

  spins = i2s_clock_try_source(priv, I2S_PLL_F160M_CLK, I2S_PLL_F160M_HZ,
                               &rate);
  g_tx_update_spins = spins;

  

  if (spins < I2S_UPDATE_SPIN_MAX)
    {
      g_i2s_clk_sel = I2S_PLL_F160M_CLK;
      return rate;
    }

  /* Attempt 2: XTAL (40 MHz) -- always running, independent of the PLL. */

  spins = i2s_clock_try_source(priv, I2S_XTAL_CLK, 40000000u, &rate);
  g_tx_update_spins2 = spins;

  

  g_i2s_clk_sel = I2S_XTAL_CLK;

  if (spins >= I2S_UPDATE_SPIN_MAX)
    {
      /* Neither source advanced the domain: the stall is not caused by the
       * source clock.  Leave the (always-running) crystal selected so the
       * system is deterministic, and say so loudly. */

      
    }

  return rate;
}

static uint32_t i2s_txsamplerate(struct i2s_dev_s *dev, uint32_t rate);
static uint32_t i2s_txdatawidth(struct i2s_dev_s *dev, int bits);
static int      i2s_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                         i2s_callback_t callback, void *arg,
                         uint32_t timeout);
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
static void     i2s_rx_channel_start(struct esp32s3_i2s_s *priv);
static void     i2s_rx_channel_stop(struct esp32s3_i2s_s *priv);
static int      i2s_rxchannels(struct i2s_dev_s *dev, uint8_t channels);
static uint32_t i2s_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate);
static uint32_t i2s_rxdatawidth(struct i2s_dev_s *dev, int bits);
static int      i2s_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                            i2s_callback_t callback, void *arg,
                            uint32_t timeout);
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2s_ops_s g_i2sops =
{
#ifdef I2S_HAVE_TX
  .i2s_txchannels     = i2s_txchannels,
  .i2s_txsamplerate   = i2s_txsamplerate,
  .i2s_txdatawidth    = i2s_txdatawidth,
  .i2s_send           = i2s_send,
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
  .i2s_rxchannels     = i2s_rxchannels,
  .i2s_rxsamplerate   = i2s_rxsamplerate,
  .i2s_rxdatawidth    = i2s_rxdatawidth,
  .i2s_receive        = i2s_receive,
#endif /* I2S_HAVE_RX */

  .i2s_ioctl             = i2s_ioctl,
  .i2s_getmclkfrequency  = i2s_getmclkfrequency,
  .i2s_setmclkfrequency  = i2s_setmclkfrequency,
};

#ifdef CONFIG_ESP32S3_I2S0
static const struct esp32s3_i2s_config_s esp32s3_i2s0_config =
{
  .port             = 0,
#ifdef CONFIG_ESP32S3_I2S0_ROLE_MASTER
  .role             = I2S_ROLE_MASTER,
#else
  .role             = I2S_ROLE_SLAVE,
#endif /* CONFIG_ESP32S3_I2S0_ROLE_MASTER */
  .data_width       = CONFIG_ESP32S3_I2S0_DATA_BIT_WIDTH,
  .rate             = CONFIG_ESP32S3_I2S0_SAMPLE_RATE,
  .total_slot       = 2,
  .tx_en            = I2S0_TX_ENABLED,
  .rx_en            = I2S0_RX_ENABLED,
  .tx_clk_src       = I2S_PLL_D2_CLK,
  .rx_clk_src       = I2S_PLL_D2_CLK,
#ifdef CONFIG_ESP32S3_I2S0_MCLK
  .mclk_pin         = CONFIG_ESP32S3_I2S0_MCLKPIN,
#else
  .mclk_pin         = I2S_GPIO_UNUSED,
#endif /* CONFIG_ESP32S3_I2S0_MCLK */
  .bclk_pin         = CONFIG_ESP32S3_I2S0_BCLKPIN,
  .ws_pin           = CONFIG_ESP32S3_I2S0_WSPIN,
#ifdef CONFIG_ESP32S3_I2S0_DOUTPIN
  .dout_pin         = CONFIG_ESP32S3_I2S0_DOUTPIN,
#else
  .dout_pin         = I2S_GPIO_UNUSED,
#endif /* CONFIG_ESP32S3_I2S0_DOUTPIN */
#ifdef CONFIG_ESP32S3_I2S0_DINPIN
  .din_pin          = CONFIG_ESP32S3_I2S0_DINPIN,
#else
  .din_pin          = I2S_GPIO_UNUSED,
#endif /* CONFIG_ESP32S3_I2S0_DINPIN */
  .bclk_in_insig    = I2S0I_BCK_IN_IDX,
  .bclk_in_outsig   = I2S0I_BCK_OUT_IDX,
  .bclk_out_insig   = I2S0O_BCK_IN_IDX,
  .bclk_out_outsig  = I2S0O_BCK_OUT_IDX,
  .ws_in_insig      = I2S0I_WS_IN_IDX,
  .ws_in_outsig     = I2S0I_WS_OUT_IDX,
  .ws_out_insig     = I2S0O_WS_IN_IDX,
  .ws_out_outsig    = I2S0O_WS_OUT_IDX,
  .din_insig        = I2S0I_SD_IN_IDX,
  .dout_outsig      = I2S0O_SD_OUT_IDX,
  .mclk_out_sig     = I2S0_MCLK_OUT_IDX,
  .audio_std_mode   = I2S_TDM_PHILIPS,
};

static struct esp32s3_i2s_s esp32s3_i2s0_priv =
{
  .dev =
  {
    .ops = &g_i2sops,
  },
  .lock = NXMUTEX_INITIALIZER,
  .config = &esp32s3_i2s0_config,
  .bufsem = SEM_INITIALIZER(0),
};
#endif /* CONFIG_ESP32S3_I2S0 */

#ifdef CONFIG_ESP32S3_I2S1
static const struct esp32s3_i2s_config_s esp32s3_i2s1_config =
{
  .port             = 1,
#ifdef CONFIG_ESP32S3_I2S1_ROLE_MASTER
  .role             = I2S_ROLE_MASTER,
#else
  .role             = I2S_ROLE_SLAVE,
#endif /* CONFIG_ESP32S3_I2S1_ROLE_MASTER */
  .data_width       = CONFIG_ESP32S3_I2S1_DATA_BIT_WIDTH,
  .rate             = CONFIG_ESP32S3_I2S1_SAMPLE_RATE,
  .total_slot       = 2,
  .tx_en            = I2S1_TX_ENABLED,
  .rx_en            = I2S1_RX_ENABLED,
  .tx_clk_src       = I2S_PLL_D2_CLK,
  .tx_clk_src       = I2S_PLL_D2_CLK,
#ifdef CONFIG_ESP32S3_I2S1_MCLK
  .mclk_pin         = CONFIG_ESP32S3_I2S1_MCLKPIN,
#else
  .mclk_pin         = I2S_GPIO_UNUSED,
#endif /* CONFIG_ESP32S3_I2S1_MCLK */
  .bclk_pin         = CONFIG_ESP32S3_I2S1_BCLKPIN,
  .ws_pin           = CONFIG_ESP32S3_I2S1_WSPIN,
#ifdef CONFIG_ESP32S3_I2S1_DOUTPIN
  .dout_pin         = CONFIG_ESP32S3_I2S1_DOUTPIN,
#else
  .dout_pin         = I2S_GPIO_UNUSED,
#endif /* CONFIG_ESP32S3_I2S1_DOUTPIN */
#ifdef CONFIG_ESP32S3_I2S1_DINPIN
  .din_pin          = CONFIG_ESP32S3_I2S1_DINPIN,
#else
  .din_pin          = I2S_GPIO_UNUSED,
#endif /* CONFIG_ESP32S3_I2S1_DINPIN */
  .bclk_in_insig    = I2S1I_BCK_IN_IDX,
  .bclk_in_outsig   = I2S1I_BCK_OUT_IDX,
  .bclk_out_insig   = I2S1O_BCK_IN_IDX,
  .bclk_out_outsig  = I2S1O_BCK_OUT_IDX,
  .ws_in_insig      = I2S1I_WS_IN_IDX,
  .ws_in_outsig     = I2S1I_WS_OUT_IDX,
  .ws_out_insig     = I2S1O_WS_IN_IDX,
  .ws_out_outsig    = I2S1O_WS_OUT_IDX,
  .din_insig        = I2S1I_SD_IN_IDX,
  .dout_outsig      = I2S1O_SD_OUT_IDX,
  .mclk_out_sig     = I2S1_MCLK_OUT_IDX,
  .audio_std_mode   = I2S_TDM_PHILIPS,
};

static struct esp32s3_i2s_s esp32s3_i2s1_priv =
{
  .dev =
  {
    .ops = &g_i2sops,
  },
  .lock = NXMUTEX_INITIALIZER,
  .config = &esp32s3_i2s1_config,
  .bufsem = SEM_INITIALIZER(0),
};
#endif /* CONFIG_ESP32S3_I2S1 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: i2s_buf_allocate
 *
 * Description:
 *   Allocate a buffer container by removing the one at the head of the
 *   free list
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   A non-NULL pointer to the allocate buffer container on success; NULL if
 *   there are no available buffer containers.
 *
 * Assumptions:
 *   The caller does NOT have exclusive access to the I2S state structure.
 *   That would result in a deadlock!
 *
 ****************************************************************************/

static struct esp32s3_buffer_s *i2s_buf_allocate(struct esp32s3_i2s_s *priv)
{
  struct esp32s3_buffer_s *bfcontainer;
  irqstate_t flags;
  int ret;

  /* Set aside a buffer container.  By doing this, we guarantee that we will
   * have at least one free buffer container.
   */

  ret = nxsem_wait_uninterruptible(&priv->bufsem);
  if (ret < 0)
    {
      return NULL;
    }

  /* Get the buffer from the head of the free list */

  flags = spin_lock_irqsave(&priv->slock);
  bfcontainer = priv->bf_freelist;
  DEBUGASSERT(bfcontainer);

  /* Unlink the buffer from the freelist */

  priv->bf_freelist = bfcontainer->flink;
  spin_unlock_irqrestore(&priv->slock, flags);
  
  return bfcontainer;
}

/****************************************************************************
 * Name: i2s_buf_free
 *
 * Description:
 *   Free buffer container by adding it to the head of the free list
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *   bfcontainer - The buffer container to be freed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The caller has exclusive access to the I2S state structure
 *
 ****************************************************************************/

static void i2s_buf_free(struct esp32s3_i2s_s *priv,
                         struct esp32s3_buffer_s *bfcontainer)
{
  irqstate_t flags;

  /* Put the buffer container back on the free list (circbuf) */

  flags = spin_lock_irqsave(&priv->slock);

  /* [V307] If this container still holds a DMA bounce slot (RX path
   * aborted before the worker ran), return it to the pool so the slot
   * cannot leak.  i2s_dmabuf_release() tolerates NULL. */

  i2s_dmabuf_release(bfcontainer->buf);

  bfcontainer->apb = NULL;
  bfcontainer->buf = NULL;
  bfcontainer->nbytes = 0;
  bfcontainer->flink  = priv->bf_freelist;
  priv->bf_freelist = bfcontainer;

  spin_unlock_irqrestore(&priv->slock, flags);

  /* Wake up any threads waiting for a buffer container */

  nxsem_post(&priv->bufsem);
}

/****************************************************************************
 * Name: i2s_buf_initialize
 *
 * Description:
 *   Initialize the buffer container allocator by adding all of the
 *   pre-allocated buffer containers to the free list
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   OK on success; A negated errno value on failure.
 *
 * Assumptions:
 *   Called early in I2S initialization so that there are no issues with
 *   concurrency.
 *
 ****************************************************************************/

static int i2s_buf_initialize(struct esp32s3_i2s_s *priv)
{
#ifdef I2S_HAVE_TX
  priv->tx.carry.bytes = 0;
  priv->tx.carry.value = 0;
#endif /* I2S_HAVE_TX */

  priv->bf_freelist = NULL;
  for (int i = 0; i < CONFIG_ESP32S3_I2S_MAXINFLIGHT; i++)
    {
      i2s_buf_free(priv, &priv->containers[i]);
    }

  return OK;
}

/****************************************************************************
 * Name: i2s_txdma_start
 *
 * Description:
 *   Initiate the next TX DMA transfer. The DMA outlink was previously bound
 *   so it is safe to start the next DMA transfer at interrupt level.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure
 *
 * Assumptions:
 *   Interrupts are disabled
 *
 ****************************************************************************/

/* [V682] *** Which engine drives BCLK/WS on the pads? ***
 *
 * v681 field data during a PTT capture: MCLK(11) toggling, BCLK(10) = 0 over
 * 2000 samples, WS(8) = 2000 (stuck high), txstart = 0, rxconf = 0x00089604.
 *
 * rxconf decoded (esp32s3 i2s register map): bit2 rx_start = 1,
 * bit3 rx_slave_mod = 0, bit19 rx_tdm_en = 1 -> the receiver is running AND
 * is the I2S master (g_v390_rxmaster = 1 is the compile-time default, so
 * i2s_configure() clears I2S_RX_SLAVE_MOD at line 3270).
 *
 * A running I2S master always produces BCK/WS, and the RX side is fully
 * configured: I2S_RX_CLK_ACTIVE is set (line 2935), RX_CLKM_DIV_* is
 * programmed (line 3788) and I2S_RX_BCK_DIV_NUM is written next to the TX one
 * (line 3833).  The clock exists - it just never reaches a pad, because the
 * pad owner is chosen by
 *
 *     if (i2s_rx_active(priv) && !i2s_tx_active(priv))     // line 3001
 *
 * and i2s_tx_active() returns true unconditionally (g_v380_txmaster = 1 at
 * line 2830), so the else branch always wins and binds GPIO10/GPIO8 to
 * I2S0O_BCK_OUT_IDX / I2S0O_WS_OUT_IDX - the *transmitter's* generators -
 * while the receiver is the one holding the clock.  The transmitter in turn
 * only moves while I2S_TX_START is set, and a capture-only session never
 * sets it (i2s_txsamplerate() stops and resets the transmitter after the
 * [V382] clock keeper ran, and i2s_tx_channel_start() never writes TX_START).
 *
 * Real hardware, v682a experiment (2-byte .rodata flip of the two outsig
 * fields 22->26 / 24->27, i.e. the same change this helper makes at run time):
 *
 *   before: BCLK(10)=0    WS(8)=2000  txstart=0  peak=6
 *   after : BCLK(10)=1000 WS(8)=1858  txstart=0  peak=32768, "speech detected"
 *
 * so the diagnosis is confirmed on the device, and flipping the pad owner
 * alone does not disturb playback (RX keeps generating its clock while TX
 * plays - both dividers come from the same source, so the pads carry the same
 * frequency either way).
 *
 * The pads are a single shared resource, so the fix pins them at every
 * direction switch, in the DMA start functions - the order-independent place:
 *
 *   i2s_rxdma_start(): pads <- I2S0I_*_OUT_IDX (RX generator), before RX_START
 *   i2s_txdma_start(): pads <- I2S0O_*_OUT_IDX (TX generator), before TX_START
 *
 * Playback keeps exactly the wiring it uses today (zero regression), capture
 * gets the generator that is actually running.
 *
 * NOTE on the data pins: din_pin = 9 and dout_pin = 7 are correct and are
 * deliberately untouched.  board.h names them from the codec's point of view
 * (codec DOUT/ASDOUT = GPIO7 = ESP32 output to the codec DAC; codec DIN/DSDIN
 * = GPIO9 = ESP32 input from the codec ADC), which reads backwards from the
 * ESP32 side; playback audio is the proof that dout_pin = 7 is right.
 */

static int g_v682_pad_owner = -1;   /* 1 = RX generator, 0 = TX generator */

static void i2s_route_bclk_ws(struct esp32s3_i2s_s *priv, bool rx_owner)
{
  uint32_t port = priv->config->port;
  uint32_t bclk_outsig;
  uint32_t ws_outsig;

  if (priv->config->bclk_pin == I2S_GPIO_UNUSED ||
      priv->config->ws_pin == I2S_GPIO_UNUSED)
    {
      return;
    }

  if (rx_owner)
    {
      bclk_outsig = priv->config->bclk_in_outsig;   /* I2S0I_BCK_OUT_IDX */
      ws_outsig   = priv->config->ws_in_outsig;     /* I2S0I_WS_OUT_IDX  */

      /* The RX divider only advances while the module gate and the RX clock
       * domain are enabled.  i2s_configure() opens them, but its RX half is
       * guarded by "din_pin != I2S_GPIO_UNUSED"; both writes are idempotent.
       */

      modifyreg32(I2S_TX_CLKM_CONF_REG(port), 0, I2S_CLK_EN);
      modifyreg32(I2S_RX_CLKM_CONF_REG(port), 0, I2S_RX_CLK_ACTIVE);
    }
  else
    {
      bclk_outsig = priv->config->bclk_out_outsig;  /* I2S0O_BCK_OUT_IDX */
      ws_outsig   = priv->config->ws_out_outsig;    /* I2S0O_WS_OUT_IDX  */

      modifyreg32(I2S_TX_CLKM_CONF_REG(port), 0,
                  I2S_CLK_EN | I2S_TX_CLK_ACTIVE);
    }

  esp32s3_gpiowrite(priv->config->ws_pin, 1);
  esp32s3_configgpio(priv->config->ws_pin, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(priv->config->ws_pin, ws_outsig, 0, 0);

  esp32s3_gpiowrite(priv->config->bclk_pin, 1);
  esp32s3_configgpio(priv->config->bclk_pin, OUTPUT_FUNCTION_2);
  esp32s3_gpio_matrix_out(priv->config->bclk_pin, bclk_outsig, 0, 0);

  g_v682_pad_owner = rx_owner ? 1 : 0;
}

/* [V682] *** RX TDM: one slot, two-slot frame. ***
 *
 * Measured v682a, with the bit clock finally running, the captured chunks
 * alternate: chunk#1 peak = 32768, chunk#2 peak = 0, chunk#3 peak = 32768.
 * The receiver is handing back two samples per frame (L then R, one of them
 * empty) because both slot enables are set.  Mono capture wants:
 *
 *     TOT_CHAN_NUM = 1      (2 slots - 1)
 *     CHAN0_EN     = 1, CHAN1_EN = 0
 *
 * which is bit-for-bit what ESP-IDF does in i2s_ll_rx_select_std_slot() for
 * I2S_STD_SLOT_LEFT (and what the v680 binary patch did with an immediate).
 * i2s_configure() writes both enables, so the same values are re-asserted on
 * every DMA arm, before the rx_update that latches them reaches the RX clock
 * domain.
 *
 * Risk note: this board captures mono (BOARD_AUDIO_CHANNELS = 1).  A board
 * that needs stereo capture must not call this.
 */

static void i2s_rx_tdm_mono(struct esp32s3_i2s_s *priv)
{
  uint32_t port = priv->config->port;

  modifyreg32(I2S_RX_TDM_CTRL_REG(port), I2S_RX_TDM_TOT_CHAN_NUM_M,
              FIELD_TO_VALUE(I2S_RX_TDM_TOT_CHAN_NUM, 1));
  modifyreg32(I2S_RX_TDM_CTRL_REG(port),
              I2S_RX_TDM_PDM_CHAN0_EN_M | I2S_RX_TDM_PDM_CHAN1_EN_M,
              I2S_RX_TDM_PDM_CHAN0_EN);
}

#ifdef I2S_HAVE_RX
/* [V682] Capture health counters.
 *
 * i2s_rxdma_start() may run on the interrupt stack (i2s_rx_schedule() calls it
 * directly), so these carry no I/O - i2s_receive() prints them from task
 * context instead.
 */

static int g_v682_pad_logs;         /* console report rate gate        */
static int g_v682_start_lost;       /* RX_START did not stick          */
static int g_v682_act_stale;        /* rx.act non-empty, RX_START = 0  */
static int g_v682_rxstart;          /* RX_START read back after arming */
#endif /* I2S_HAVE_RX */

#ifdef I2S_HAVE_TX
static int IRAM_ATTR i2s_txdma_start(struct esp32s3_i2s_s *priv)
{

  /* [V362] *** IDLE-SPIN FIX (team-lead V362 requirement #1). ***
   *
   * Up to V361 the two "nothing to do" bail-outs lived AFTER
   * i2s_apply_tx_gdma_strategy() and after two getreg32() reads, so an
   * empty call still
   *   - re-wrote DMA_OUT_CONF0 / CONF1 / PRI (6 register RMWs),
   *   - read I2S_STATE + I2S_TX_CONF,
   *   - emitted two syslog records,
   * before returning.  i2s_tx_schedule() reaches this function from the
   * GDMA EOF interrupt handler, and it does so exactly when tx.act is
   * empty -- i.e. every empty call was re-programming a live DMA channel
   * from interrupt context.  The measured consequence is 412
   * STA_DISCONNECT events in 240 s: the CPU never gets back to the WiFi
   * driver, LwIP misses its auth/assoc deadlines and the whole network
   * stack dies, which is why ASR and the LLM never even get to run.
   *
   * From here on, if there is nothing to transmit we return before
   * touching ANY register and before calling the GDMA helper.
   */

  g_tx_rate_calls++;

  if (!sq_empty(&priv->tx.act))
    {
      /* A transmission is already in flight -- nothing to arm. */
      g_tx_rate_bail_act++;
      i2s_tx_rate_tick();
      return OK;
    }

  if (sq_empty(&priv->tx.pend))
    {
      /* *** THE IDLE SPIN ***: no active and no queued buffer. */
      g_tx_rate_bail_pend++;

      if (g_tx_callsite == 1)
        {
          g_tx_rate_bail_s1++;
        }
      else if (g_tx_callsite == 2)
        {
          g_tx_rate_bail_s2++;
        }

      /* [V362] Requirement #2: name the caller.  The first few idle
       * entries are printed with their call-site id so the log itself
       * answers "who is calling with an empty buffer?".
       *   1 = i2s_txdma_setup()  (enqueue path)
       *   2 = i2s_tx_schedule()  (GDMA EOF ISR)
       */
      if (g_tx_rate_bail_pend <= 5)
        {
          
        }

      /* [V364] Both queues are empty: nothing is in flight and nothing is
       * queued, so any further TX completion interrupt is spurious.  Gate
       * the source before returning -- this is the actual fix for the
       * interrupt storm; V362 only made each arrival cheap.
       * i2s_tx_irq_unmask() re-enables it the moment a buffer is queued.
       */

      i2s_tx_irq_mask(priv);

      i2s_tx_rate_tick();
      return OK;
    }

  g_tx_rate_starts++;
  i2s_tx_rate_tick();
  /* [V338] 传输前的最后一步：再应用一次 GDMA strategy。
   *
   * V337 证明策略能写进去（CONF0=0x34/CONF1=0x1000/PRI=1），但
   * i2s_tx_channel_start() 里的 DMA_OUT_RST 会把它冲回 0x08。
   * 这里是设 OUTLINK_START 之前，之后不会再有复位，
   * 因此在这里补一次才能保证传输时策略在位。 */

  i2s_apply_tx_gdma_strategy(priv);

  struct esp32s3_buffer_s *bfcontainer;
  int light;                       /* [V671] 1 = DMA re-arm only */

  /* [V347] Prologue probe: answer team-lead's question "does the 2nd transfer
   * reach the I2S_TX_START write at all, or is it swallowed by an early
   * return?".  Log once per transfer what state we enter with. */

  g_tx_txdma_start_n++;
  

  /* If there is already an active transmission in progress, then bail
   * returning success.
   */

  if (!sq_empty(&priv->tx.act))
    {
      g_tx_txdma_bail_act++;
      
      return OK;
    }

  /* If there are no pending transfer, then bail returning success */

  if (sq_empty(&priv->tx.pend))
    {
      g_tx_txdma_bail_pend++;
      
      return OK;
    }

  /* [V671] *** Which path does this transfer take? ***
   *
   * light == 1 means the module is configured and the transmitter is already
   * running, so all this transfer needs is a descriptor for the GDMA.  That
   * is what upstream NuttX does, and it is the only way to keep the TX FIFO
   * from being flushed between two 2048-byte chunks (see g_v671_cfg_valid).
   *
   * light == 0 means a reset path ran since the last transfer, so re-run the
   * full configure/reset/start sequence exactly as before. */

  light = (g_v671_cfg_valid != 0 &&
           (getreg32(I2S_TX_CONF_REG(priv->config->port)) & I2S_TX_START) != 0);

  if (light)
    {
      g_v671_light_n++;
    }
  else
    {
      g_v671_full_n++;
    }

  /* Start transmission if no data is already being transmitted */

  bfcontainer = (struct esp32s3_buffer_s *)sq_remfirst(&priv->tx.pend);

  /* [V682] The TX twin of the pad pinning in i2s_rxdma_start(): playback owns
   * BCLK/WS while it streams.  This restores exactly the signal pair the pads
   * carry today (I2S0O_*_OUT_IDX), so the working playback path is unchanged -
   * it only stops a preceding capture from leaving the pads on the receiver's
   * generator, whose RX channel is stopped by then (no clock, silent codec).
   *
   * It sits before the [V349] stop / [V264] configure / I2S_TX_START
   * sequence because TX_START is the last write of that sequence and the
   * frame sync has to be re-opened with the right pad owner already in place.
   *
   * Certainty: 已证实 (v682a: playback, display and Wi-Fi all normal with the
   * pad owner changed at run time).
   */

  if (priv->config->role == I2S_ROLE_MASTER)
    {
      i2s_route_bclk_ws(priv, false);
    }

  /* [V349] *** Stop the transmitter BEFORE the configure/reset block. ***
   *
   * The v347/v348 register trace isolates the one thing that differs between
   * the transfer that works and the transfers that do not:
   *
   *   first transfer: I2S_TX_CONF.START = 0 (TXCONF=0x00081200) when the
   *                   configure/reset block runs -> the reset really takes
   *                   effect -> after the start edge STATE=0x00000000 (TX
   *                   running), the DMA request is raised, transfer completes.
   *
   *   later transfers: I2S_TX_CONF.START = 1 (TXCONF=0x00081204, the bit the
   *                   previous transfer left behind) when the configure/reset
   *                   block runs -> the module is reset while tx_start is
   *                   already asserted, so the later start edge is swallowed
   *                   and STATE stays 0x00000001 (TX idle): no DMA request,
   *                   GDMA outlink parked, OWNER stuck, "completion TIMEOUT".
   *
   * (v345 ran this block every transfer and still failed; v346 ran it once and
   * failed; v347 added a stop->start toggle but placed the stop AFTER the
   * reset, so the reset still saw START=1; v348 restored the per-transfer
   * reset but likewise left START set.  The fix is ordering: stop first.)
   *
   * Do exactly what ESP-IDF's i2s_ll_tx_stop() does -- drive tx_start 0 and
   * latch it -- but do it here, ahead of the reset, and only when it is
   * actually asserted so the very first transfer is untouched. */

  if (!light &&
      (getreg32(I2S_TX_CONF_REG(priv->config->port)) & I2S_TX_START))
    {
      int pspins = 0;

      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_START, 0);
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_UPDATE);
      while (pspins < 200000 &&
             (getreg32(I2S_TX_CONF_REG(priv->config->port)) & I2S_TX_UPDATE))
        {
          pspins++;
        }

      
    }

  /* [V264] *** One block, in ESP-IDF's order: configure, LATCH, then start ***
   *
   * This replaces the V258 and V262 blocks, which were ordered wrongly: V262
   * wrote the official register values AFTER V258 had already run its
   * tx_update + tx_start, so those values only ever existed in the APB domain.
   * I2S_TX_UPDATE is what hands tx_conf to the TX clock domain; anything
   * written after it -- and in particular after tx_start -- is invisible to the
   * transmitter.  The clock domain therefore kept the old, wrong settings
   * (tx_stop_en 0 from V217, tx_left_align 1 from V236, tx_mono/chan_equal 1)
   * and the register read-backs looked "aligned" while the hardware was not.
   *
   * ESP-IDF's order, which this now follows literally:
   *
   *     configure -> tx_reset -> tx_fifo_reset -> tx_update (wait) -> tx_start
   *
   * Values are the ones measured on a WORKING image on this exact board:
   *   TX_CONF 0x00083204 (mono 0, chan_equal 0, left_align 0, stop_en 1)
   *   TX_TDM  0x00010003, TX_P2P 0x004aa004
   *   GDMA OUT CONF0 0x08, CONF1 0x00, PRI 0x00, PD_CONF 0x20
   *
   * Nothing below this block touches tx_conf, TX_TDM_CTRL, TX_PCM2PDM_CONF,
   * I2S_TX_UPDATE or the GDMA channel registers again. */

  /* [V346] Run the heavy configure/reset/start sequence once per channel
   * activation, not on every transfer.
   *
   * Upstream NuttX starts the I2S transmitter once -- at channel start -- and
   * every later transfer only re-arms the GDMA.  This block instead reset the
   * whole TX module (I2S_TX_RESET + I2S_TX_FIFO_RESET), re-derived the clock
   * dividers and re-ran tx_update/tx_start for every buffer, and because
   * i2s_tx_schedule() calls i2s_txdma_start() it did so from interrupt context
   * too.  The measurement shows the consequence: the first transfer starts and
   * completes (txirqN 0->1 and the completion callback fires) while later
   * transfers are left with the transmitter idle (I2S_STATE TX_IDLE=1, DSCR=0,
   * descriptor OWNER still 1) and never complete -- which is why the second
   * and later beeps time out.  Configure the module once; afterwards only
   * re-arm the DMA (the code below, outside this guard, still runs every
   * transfer). */

  /* [V348] *** Run the full configure/reset/start sequence on EVERY transfer.
   *
   * V346 made this block run only once (on the first transfer) to match the
   * upstream "start once, re-arm the DMA per transfer" model.  The V347 trace
   * shows that is wrong for THIS hardware path: every transfer that SKIPS the
   * block ends with the transmitter idle -- STATE=0x00000001, GDMA outlink
   * parked, descriptor OWNER stuck at 1, "I2S TX completion TIMEOUT" -- while
   * every transfer that RUNS it starts cleanly (STATE=0x00000000) and
   * completes:
   *
   *   txdma_start #1 (cfg_done=0): tx stop -> start -> STATE=0x00000000, OK
   *   txdma_start #3 (cfg_done=1): tx stop -> start -> STATE=0x00000001, stuck
   *
   * The load-bearing part is the module reset (I2S_TX_RESET + I2S_TX_FIFO_RESET)
   * plus the re-derived clock; without them the stop->start edge does not make
   * the TX core actually run.  So make every transfer look exactly like the
   * first, known-good one. */

  if (!light)
  {
    uint32_t port = priv->config->port;
    uint32_t ch   = priv->dma_channel;
    int sp = 0;

    g_tx_cfg_done = 1;   /* [V346] kept for the prologue diagnostic log only */

    /* ---- 1. configuration ---- */

    /* [V343] tx_stop_en is cleared, NOT set.  The V264 block used to write it
     * as 1 ("measured on a working image"), but that value is the direct
     * cause of the deadlock the V217 note already predicted: with tx_stop_en
     * set the bit clock is gated off the moment the TX FIFO is empty, and the
     * I2S never asserts its DMA request again, so the GDMA outlink has nothing
     * to move and parks (DSCR=0, I2S TX_IDLE=1, DOUT stuck at 0).  Clearing it
     * keeps BCK running so the FIFO can drain and the DMA request is raised.
     * The V217 clear inside i2s_tx_channel_start() was being undone here. */

    modifyreg32(I2S_TX_CONF_REG(port),
                I2S_TX_MONO_M | I2S_TX_CHAN_EQUAL_M |
                I2S_TX_LEFT_ALIGN_M | I2S_TX_STOP_EN_M,
                0);

    /* [V669] *** THE 2x-FAST BUG. ***
     *
     * This used to be a hard-coded
     *
     *     putreg32(0x00010003u, I2S_TX_TDM_CTRL_REG(port));
     *
     * i.e. TOT_CHAN_NUM = 1 (two slots per frame) with BOTH CHAN0_EN (bit 0)
     * and CHAN1_EN (bit 1) set.  Two enabled channels means the transmitter
     * pulls TWO samples out of the FIFO per LRCK period.  The mono PCM MiMo
     * synthesises is then played at exactly double rate -- pitch up an
     * octave, unintelligibly fast speech, the reported symptom.
     *
     * i2s_txchannels(1) already programs the right value for this board
     * (TOT_CHAN_NUM = 1 + CHAN0_EN only, measured as tdm = 0x00010001) but
     * this write, which runs on EVERY buffer, overrode it again.  That is
     * also why chasing MONO / CHAN_EQUAL went nowhere: those two bits only
     * control what is *transmitted* (duplicating the left sample onto the
     * right slot), never how many samples a frame consumes.  Latched into the
     * TX domain with the transmitter stopped (v668), the read-back was a
     * clean "mono_bit=1 eq_bit=1" and the rate did not move: 28 ksample/s
     * against an LRCK of 15109 Hz, i.e. still two samples per frame.
     *
     * Keep the two-slot frame structure (TOT_CHAN_NUM = 1) so BCLK stays at
     * 32 bits per frame and the ES8311 needs no change; just stop feeding the
     * right-hand slot out of the FIFO unless stereo is actually wanted.
     *
     * Same provenance as the I2S_TX_PCM2PDM_CONF_REG write below: 0x004aa004
     * is a value measured on the struggling image, kept as-is. */

    {
      uint32_t tdm = FIELD_TO_VALUE(I2S_TX_TDM_TOT_CHAN_NUM, 1) |
                     I2S_TX_TDM_CHAN0_EN;

      if (!g_i2s_tx_mono)
        {
          tdm |= I2S_TX_TDM_CHAN1_EN;
        }

      putreg32(tdm, I2S_TX_TDM_CTRL_REG(port));

    }
    putreg32(0x004aa004u, I2S_TX_PCM2PDM_CONF_REG(port));

    /* [V341] The clock divider is no longer hard-coded.  The old value
     * 0x003c0201 was measured on a 160 MHz-sourced image and is simply wrong
     * for any other source.  i2s_set_clock() re-derives TX/RX_CLKM_DIV_CONF
     * and the BCK divider from g_i2s_sclk (the source the common bring-up
     * actually selected) a few lines below, after the module reset. */

    /* [V339] 原来这里硬编码的是工作镜像上量到的复位值
     *   CONF0=0x08 / CONF1=0x00 / PRI=0x00
     * 而这三样恰恰是 V251 指出的致命项：
     *   out_auto_wrback(bit2)=0  ⇒ 发完不清 OWNER 位、不存传输长度
     *   out_dscr_burst(bit4)=out_data_burst(bit5)=0
     *   out_check_owner(conf1 bit12)=0
     *   out_pri=0
     * ⇒ outlink FSM 无法越过第一个描述符 ⇒ DSCR 恒 0、FIFO 永不排空。
     * 而且它写在 i2s_apply_tx_gdma_strategy() 之后，会把策略整个覆盖掉
     * —— 这就是之前怎么补 CONF0 都会回到 0x08 的原因。
     *
     * 改为 ESP-IDF 的配置值（owner_check=TRUE / desc_write_back=TRUE /
     * eof_till_popped=FALSE / burst 全开 / PRI=1）：
     *   CONF0 = 0x34 (AUTO_WRBACK|DSCR_BURST|DATA_BURST, EOF_MODE=0)
     *   CONF1 = 0x1000 (CHECK_OWNER)
     *   PRI   = 0x1 */
    putreg32(0x00000034u, DMA_OUT_CONF0_CH0_REG + ch * GDMA_REG_OFFSET);
    putreg32(0x00001000u, DMA_OUT_CONF1_CH0_REG + ch * GDMA_REG_OFFSET);
    putreg32(0x00000001u, DMA_OUT_PRI_CH0_REG + ch * GDMA_REG_OFFSET);

    modifyreg32(DMA_PD_CONF_REG,
                DMA_DMA_RAM_FORCE_PD_M | DMA_DMA_RAM_CLK_FO_M,
                DMA_DMA_RAM_FORCE_PU_M);

    /* ---- 2. reset the module and its FIFO ---- */

    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_RESET);
    modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_RESET, 0);
    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_FIFO_RESET);
    modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_FIFO_RESET, 0);

    /* [V341] Re-derive every clock divider for the source clock that the
     * common bring-up selected (g_i2s_sclk / g_i2s_clk_sel).  This replaces
     * the old hard-coded 160 MHz value and keeps DIV_CONF, CLKM_DIV_NUM and
     * the BCK divider consistent with the actual source. */

    /* [V362] Requirement #3: do not re-derive the TX clock on every
     * buffer.
     *
     * i2s_set_clock() divides with float/double (no FPU on LX7 under
     * NuttX) and then walks a/b up to I2S_LL_MCLK_DIVIDER_MAX looking
     * for the best fraction -- several hundred soft-float operations
     * executed from the GDMA EOF ISR, once per buffer.  Its result
     * depends only on (sclk, role, rate, total_slot, data_width,
     * mclk_freq), so derive it once and afterwards just re-apply the
     * latched register values.  We RE-APPLY rather than skip, because
     * the I2S_TX_RESET that runs a few lines above may have reverted
     * the divider registers; the values written are bit-identical to
     * what i2s_set_clock() would have produced.
     */

    if (!g_txclk_valid ||
        g_txclk_sclk  != (uint32_t)g_i2s_sclk ||
        g_txclk_role  != (uint32_t)priv->config->role ||
        g_txclk_rate  != (uint32_t)priv->rate ||
        g_txclk_slot  != (uint32_t)priv->config->total_slot ||
        g_txclk_width != (uint32_t)priv->data_width ||
        g_txclk_mclk  != (uint32_t)priv->mclk_freq)
      {
        (void)i2s_set_clock(priv);

        g_txclk_sclk  = (uint32_t)g_i2s_sclk;
        g_txclk_role  = (uint32_t)priv->config->role;
        g_txclk_rate  = (uint32_t)priv->rate;
        g_txclk_slot  = (uint32_t)priv->config->total_slot;
        g_txclk_width = (uint32_t)priv->data_width;
        g_txclk_mclk  = (uint32_t)priv->mclk_freq;

        g_txclk_clkm  = getreg32(I2S_TX_CLKM_CONF_REG(port));
        g_txclk_div   = getreg32(I2S_TX_CLKM_DIV_CONF_REG(port));
        g_txclk_conf1 = getreg32(I2S_TX_CONF1_REG(port));
        g_txclk_valid = 1;

        
      }
    else
      {
        putreg32(g_txclk_clkm,  I2S_TX_CLKM_CONF_REG(port));
        putreg32(g_txclk_div,   I2S_TX_CLKM_DIV_CONF_REG(port));
        putreg32(g_txclk_conf1, I2S_TX_CONF1_REG(port));
      }

    /* ---- 3. hand the configuration to the TX clock domain ---- */

    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
    while (sp < 200000 &&
           (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE))
      {
        sp++;
      }

    

    

    /* [V265] tx_start deliberately NOT written here.  The original driver
     * armed the GDMA first and started the I2S last, and that ordering has
     * never been tested with the registers at their official values.  Starting
     * the transmitter while the FIFO is still empty is the one thing that can
     * make it go idle immediately; arming the DMA first means data is already
     * waiting when tx_start lands. */
  }

  /* [V352] Step-0 diagnostic: dump the whole outlink chain BEFORE the GDMA
   * is armed.
   *
   * DMA_OUT_AUTO_WRBACK (OUT_CONF0 bit 2) is set, so when the GDMA finishes a
   * descriptor it writes that descriptor back to SRAM: it clears OWNER and
   * rewrites the length field.  If anything downstream reuses the chain
   * without rebuilding it, a later transfer hands the GDMA a descriptor whose
   * OWNER is already 0 -- the outlink then parks immediately (DSCR=0x0,
   * PARK=1), the TX FIFO never receives data and the transmitter falls back
   * to TX_IDLE.  That is word for word what v349/v350 measured.
   *
   * Printing this once per i2s_txdma_start() and comparing transfer 1 against
   * transfers 2 and 3 decides it in a single run:
   *   identical  -> the outlink is rebuilt every transfer, not the cause;
   *   different  -> especially OWNER cleared or length zeroed, the chain is
   *                 being consumed and never restored. */

  if (g_tx_txdma_start_n <= 6)
    {
      for (int di = 0; di < I2S_DMADESC_NUM; di++)
        {
          volatile struct esp32s3_dmadesc_s *d =
            (volatile struct esp32s3_dmadesc_s *)&bfcontainer->dma_link[di];

          
        }
    }

  /* [V354] *** Hand the freshly written outlink and samples to physical SRAM. ***
   *
   * Everything above wrote bfcontainer->dma_link[] (size / length / EOF /
   * OWNER / next) and bfcontainer->buf[] (the PCM bytes) through the data
   * cache.  With CONFIG_ESP32S3_DATA_CACHE_32KB enabled and no cache
   * maintenance anywhere in the esp32s3 drivers, none of that is guaranteed to
   * be visible to the GDMA engine, which reads physical SRAM directly.
   *
   * A stale descriptor would look exactly like the reported failure: the FSM
   * has nothing it may legally fetch, so DMA_OUT_DSCR stays 0, the outlink
   * parks (link.PARK=1) and, with no data reaching the FIFO, the transmitter
   * falls back to TX_IDLE.
   *
   * Only CLEAN, never INVALIDATE:  dma_link[] shares cache lines with
   * neighbouring members of this same container (callback, apb, nbytes, ...),
   * so discarding those lines would throw away live data.  Cleaning writes
   * dirty lines back and leaves the lines valid, which is sufficient for a
   * TX path where the CPU is the only writer. */

  if (g_i2s_tx_cache_clean)
    {
      uintptr_t dstart = (uintptr_t)bfcontainer->dma_link;
      uintptr_t dend   = dstart + sizeof(bfcontainer->dma_link);
      uintptr_t bstart = (uintptr_t)bfcontainer->buf;
      uintptr_t bend   = bstart + bfcontainer->nbytes;

      up_clean_dcache(dstart, dend);

      if (bfcontainer->buf != NULL && bfcontainer->nbytes > 0)
        {
          up_clean_dcache(bstart, bend);
        }

      
    }

  /* [V356] *** Queue the transfer as ACTIVE before it is armed. ***
   *
   * This used to happen only at the very end, after the GDMA had already
   * been armed and started.  Any event the GDMA raised in between found
   * priv->tx.act empty, so i2s_tx_schedule() counted it as g_sched_noact
   * and threw the completion away -- the caller then blocked for the full
   * 1.5 s timeout even though the samples had really gone out.
   * Registering the buffer ahead of esp32s3_dma_load()/enable() removes
   * that window entirely; nothing between here and the end of this
   * function can fail, so no container can leak into tx.act. */

  sq_addlast((sq_entry_t *)bfcontainer, &priv->tx.act);

  esp32s3_dma_load(bfcontainer->dma_link, priv->dma_channel, I2S_TX);
  esp32s3_dma_enable(priv->dma_channel, I2S_TX);

  /* [V214] Snapshot OUTLINK immediately after the START bit is written: if the
   * hardware clears it before i2s_send() can look, the outlink never ran. */
  if (g_txdump_n < 3)
    {
      g_dbg_link_after_enable =
        GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG, priv->dma_channel);
    }

  /* [V353] *** Start in ESP-IDF's order: update -> wait -> START LAST. ***
   *
   * The V343 build wrote tx_start and only THEN tx_update, which is the exact
   * reverse of Espressif's i2s_ll_tx_start():
   *
   *     hw->tx_conf.tx_update = 1;
   *     while (hw->tx_conf.tx_update);    // wait for the copy to complete
   *     hw->tx_conf.tx_start = 1;         // start is the LAST step
   *
   * ("Have to update registers before start".)  tx_conf lives in the APB
   * domain until a tx_update copies it into the I2S TX clock domain, and that
   * copy is only defined while the transmitter is stopped.  Writing tx_update
   * *after* tx_start re-latches the whole tx_conf while the TX FSM is already
   * supposed to be running -- which is precisely the measured symptom: STATE
   * reads 0 on the instruction after the start write and has fallen back to
   * TX_IDLE on the next one, the TX FIFO stays empty and the GDMA outlink
   * parks.  V350's re-arm loop repeated the same ordering, so nothing further
   * downstream could recover it.
   *
   * A CPU-polling fallback is NOT available on this part.  Espressif's own
   * soc/esp32s3/include/soc/i2s_struct.h opens the I2S block with
   *     uint32_t reserved_0;  // 0x00
   *     uint32_t reserved_4;  // 0x04
   *     uint32_t reserved_8;  // 0x08
   *     union { ... } int_raw;// 0x0C
   * i.e. ESP32-S3 has no CPU-accessible TX FIFO port; the only "fifo" members
   * of tx_conf are the tx_fifo_reset / rx_fifo_reset control bits.  The TX
   * FIFO can only be filled by the GDMA, so the outlink has to be made to
   * work.  The I2S_TX_FIFO_REG(i) = base + 0x00 macro above therefore does not
   * map to anything real and must not be used. */

  if (!light)
  {
    uint32_t port = priv->config->port;
    int      stop_spins = 0;

    g_tx_state_pre = getreg32(I2S_STATE_REG(port));

    /* 1. i2s_ll_tx_stop(): drive tx_start low and hand that to the clock
     *    domain.  Without this the write in step 2 is not an edge at all,
     *    which is the failure mode V347 already documented. */

    modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_START, 0);
    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
    while (stop_spins < I2S_UPDATE_SPIN_MAX &&
           (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE))
      {
        stop_spins++;
      }

    /* [V668] *** Latch the channel mode while the transmitter is STOPPED. ***
     *
     * (Lines immediately above wrote TX_START = 0 and pushed a TX_UPDATE
     * through; TX_START is only driven high again a few lines below, and the
     * comment there says outright that no further update is issued after
     * that write.)
     *
     * Why this is the right moment.  TX_CONF is shadowed into the TX clock
     * domain and only takes effect when an update is handed over while
     * tx_start is low.  Everything the caller-side workaround (v664/v665/v666)
     * did wrote MONO / CHAN_EQUAL while the transmitter was already running,
     * so the APB copy read back correct -- "[V666] forced mono:
     * conf=0x00081264 mono=1 eq=1" -- while the datapath kept clocking at
     * 26104 samples/s instead of 16000 (measured, v667).
     *
     * What is being restored.  The V343 block earlier in this function clears
     * I2S_TX_MONO | I2S_TX_CHAN_EQUAL | I2S_TX_LEFT_ALIGN on every single
     * buffer, and the I2S_TX_RESET after it clears them again.  That block
     * exists to make a transmitter that used to deadlock actually run, and it
     * works -- but it leaves the transmitter in stereo mode permanently.
     *
     * Consequence of stereo mode: I2S_TX_TDM_TOT_CHAN_NUM is 1 here (bit field
     * [19:16]; measured tdm = 0x00010001, i.e. two slots with CHAN0 enabled),
     * so a frame carries two 16-bit slots and, with MONO / CHAN_EQUAL clear,
     * the transmitter lifts TWO samples out of the FIFO per LRCK period.  The
     * mono PCM that MiMo synthesises is then played at double rate: pitch up
     * an octave, unintelligibly fast speech -- the reported symptom.  Every
     * measurement agrees: registers all correct (MCLK 4.096 MHz, BCK_DIV 7 ->
     * BCLK 512 kHz, BITS_MOD 15 -> 16 bits/slot, BCLK/LRCK = 32) yet the clip
     * finishes early.
     *
     * I2S_TX_LEFT_ALIGN rides along because i2s_configure() sets it for
     * I2S_TDM_PHILIPS (V236) and this reset drops it too. */

    {
      uint32_t mode = 0;
      int      latch_spins = 0;

      if (g_i2s_tx_mono)
        {
          mode |= I2S_TX_MONO | I2S_TX_CHAN_EQUAL;
        }

      if (priv->config->audio_std_mode == I2S_TDM_PHILIPS)
        {
          mode |= I2S_TX_LEFT_ALIGN;
        }

      modifyreg32(I2S_TX_CONF_REG(port),
                  I2S_TX_MONO_M | I2S_TX_CHAN_EQUAL_M | I2S_TX_LEFT_ALIGN_M,
                  mode);

      modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
      while (latch_spins < I2S_UPDATE_SPIN_MAX &&
             (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE))
        {
          latch_spins++;
        }

    }

    g_tx_stop_spins = stop_spins;
    g_tx_state_post = getreg32(I2S_STATE_REG(port));

    /* 2. i2s_ll_tx_start(): tx_start is the LAST thing written, and no
     *    tx_update is issued afterwards. */

    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_START);
  }

  /* [V671] The module is configured and the transmitter is running, so from
   * the next transfer on the light re-arm path is allowed.  Reached from both
   * paths (the light one skips the block above and lands here unchanged). */

  g_v671_cfg_valid = 1;

  /* [V353] Read I2S_STATE three times straight after the start write.  The
   * previous builds could only infer the "STATE 0 -> 1 one instruction later"
   * fall-back from separate syslog lines; three back-to-back reads make it
   * visible in one record. */

  {
    uint32_t s0 = getreg32(I2S_STATE_REG(priv->config->port));
    uint32_t s1 = getreg32(I2S_STATE_REG(priv->config->port));
    uint32_t s2 = getreg32(I2S_STATE_REG(priv->config->port));

    
  }

  /* [V347] Post-start verify: the whole point of this version is that this
   * reads STATE=0x00000000 (TX running) on EVERY transfer, not only the first,
   * and that I2S_TX_CONF.START is 1. */

  {
    uint32_t st = getreg32(I2S_STATE_REG(priv->config->port));
    uint32_t cf = getreg32(I2S_TX_CONF_REG(priv->config->port));

    g_tx_state_post = st;
    
  }


#if I2S_V350_REARM_PROBE
  /* [V350] *** Bounded re-arm / restart retry. ***
   *
   * The v349 trace pins the failure down to the GDMA side, not the I2S
   * configuration: on the 2nd+ transfer every register write is correct
   * (START=0 during the module reset, a clean stop->start edge, STATE reads
   * 0x00000000 immediately after the start handshake) yet STATE falls back to
   * TX_IDLE one instruction later, the TX FIFO stays EMPTY (OUTFIFO_STATUS
   * OUTFIFO_EMPTY=1) and the GDMA outlink parks (OUTLINK_PARK=1) with the
   * descriptor OWNER never cleared.  In other words the transmitter starts,
   * sees no data, and idles because the GDMA never serviced its request.
   *
   * Rather than guess at which single ordering the GDMA wants, re-arm the
   * outlink and re-assert tx_start a few times until the transmitter actually
   * stays running (or we run out of tries, which the log will show).  Each
   * iteration re-loads the descriptor address, re-enables the channel (which
   * pulses DMA_OUT_RST and sets OUTLINK_START), and drives tx_start 0 -> 1 with
   * a tx_update in between. */

  {
    uint32_t port = priv->config->port;
    uint32_t ch   = priv->dma_channel;
    int      tries;

    for (tries = 0; tries < 6; tries++)
      {
        uint32_t st   = getreg32(I2S_STATE_REG(port));
        uint32_t fifo = GET_GDMA_CH_REG(DMA_OUTFIFO_STATUS_CH0_REG, ch);
        uint32_t link = GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG, ch);

        if ((st & 1u) == 0)
          {
            
            break;
          }

        esp32s3_dma_load(bfcontainer->dma_link, ch, I2S_TX);
        esp32s3_dma_enable(ch, I2S_TX);

        modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_START, 0);
        modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_START);
        modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
        {
          int u = 0;
          while (u < 200000 && (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE))
            {
              u++;
            }
        }

        
      }
  }

#endif


  return OK;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rxdma_start
 *
 * Description:
 *   Initiate the next RX DMA transfer. Assuming the DMA inlink is already
 *   bound, it's safe to start the next DMA transfer in an interrupt context.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure
 *
 * Assumptions:
 *   Interrupts are disabled
 *
 ****************************************************************************/

/* [V388] rx_update spin counts around rx_start, and how many times the
 * snapshot has been logged.  Declared here (not next to the other V380
 * globals, which sit below i2s_rxdma_start()) so they are in scope.
 */

/* [V390] A/B switch: 1 = run the receiver as I2S master (own divider),
 * 0 = keep it as a slave of the TX generator (the current behaviour).
 */
volatile int g_v390_rxmaster = 1;
volatile int g_v388_pre_spins;
volatile int g_v388_post_spins;
volatile int g_v388_logs;

#ifdef I2S_HAVE_RX
static int i2s_rxdma_start(struct esp32s3_i2s_s *priv)
{
  g_v94_start++;
  struct esp32s3_buffer_s *bfcontainer;
  size_t eof_nbytes;
  irqstate_t flags;            /* [V395] arm+enqueue critical section */

  /* If there is already an active transmission in progress, then bail
   * returning success.
   */

  if (!sq_empty(&priv->rx.act))
    {
      /* [V682] An entry in rx.act means "a transfer is in flight", so this
       * bail-out is normally correct.  But if the channel is not running at
       * the same time nothing will ever complete that entry, and every later
       * arm returns here - which is the second-PTT symptom measured on v682a:
       * rxconf = 0x00089600 (rx_start = 0), "capture EIO, retry 1/5..5/5",
       * 0 chunks, only a reboot recovers.
       *
       * Count it, and re-assert RX_START so the descriptor that is still
       * pending gets a chance to complete instead of hanging forever.
       */

      if (!(getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_START))
        {
          g_v682_act_stale++;

          modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_START);
          modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_UPDATE);
        }

      return OK;
    }

  /* If there are no pending transfer, then bail returning success */

  if (sq_empty(&priv->rx.pend))
    {
      return OK;
    }

  bfcontainer = (struct esp32s3_buffer_s *)sq_remfirst(&priv->rx.pend);

  /* If there isn't already an active transmission in progress,
   * then start it.
   */

  /* [V385] The register counts in BITS, not bytes:
   *   trigger = (I2S_RX_BITS_MOD + 1) * (REG_RX_EOF_NUM + 1)
   * so the old MIN(nbytes, DMA_BUFLEN_MAX) = 4095 asked for 8192 bytes
   * while the inlink only covers 4096 - the link ran off its NULL tail
   * before EOF, no SUC_EOF, no interrupt, capture EIO.  Start with a
   * deliberately small threshold so the interrupt has to fire if it is
   * merely mis-programmed.  See patch_v385.py.
   */

  eof_nbytes = MIN(512, bfcontainer->nbytes);

  /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO, "[V385] rx eof_num=%u (nbytes=%u)\n",
         (unsigned)eof_nbytes, (unsigned)bfcontainer->nbytes);

  modifyreg32(I2S_RXEOF_NUM_REG(priv->config->port), I2S_RX_EOF_NUM_M,
              FIELD_TO_VALUE(I2S_RX_EOF_NUM, eof_nbytes));

  /* [V682] Both of the things that decide whether this buffer can ever
   * complete are (re-)asserted here, ahead of the DMA arm and ahead of
   * I2S_RX_START:
   *
   *   1) who drives BCLK/WS on the pads - the receiver is the master
   *      (rxconf bit3 = 0), so it must own them;
   *   2) RX TDM in single-channel mode (CHAN0 only, 2-slot frame), which the
   *      rx_update below hands to the RX clock domain.
   *
   * Order matters: once the receiver is started it clocks on whatever was
   * wired at that instant, and re-routing a pad afterwards does not re-open
   * the frame sync (the v680 lesson).  Doing it per buffer also makes it
   * independent of the call order of es8311_setsamplerate() /
   * es8311_setbitspersample(), which reconfigure both directions in turn and
   * leave the pads on the TX generator.
   *
   * Register writes only - no I/O, no locks - because i2s_rx_schedule() also
   * enters here on the interrupt stack.  The console report lives in
   * i2s_receive(), which is always task context.
   */

  if (priv->config->role == I2S_ROLE_MASTER &&
      !(getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_SLAVE_MOD))
    {
      i2s_route_bclk_ws(priv, true);
    }

  i2s_rx_tdm_mono(priv);

  /* [V392] Enqueue BEFORE arming, not after.
   *
   * This is the RX twin of the completion-loss bug V356 fixed on the TX
   * side.  The GDMA is armed by esp32s3_dma_load() below; any completion
   * raised before sq_addlast(&priv->rx.act) runs finds rx.act empty and
   * is dropped, so the reader waits forever on a buffer that has in fact
   * already been filled.  With the enqueue first there is no window.
   */

  /* [V395] Arm and enqueue atomically - see the note above. */

  flags = enter_critical_section();

  esp32s3_dma_load(bfcontainer->dma_link, priv->dma_channel, I2S_RX);
  esp32s3_dma_enable(priv->dma_channel, I2S_RX);

  sq_addlast((sq_entry_t *)bfcontainer, &priv->rx.act);

  leave_critical_section(flags);

  /* [V388] ESP-IDF i2s_ll_rx_start() hands the APB configuration to the
   * RX clock domain with rx_update *immediately before* raising rx_start,
   * after every configuration write that has to reach the receiver.
   * i2s_rx_channel_start() issues its rx_update before this function
   * programs I2S_RXEOF_NUM_REG and arms the GDMA, so the threshold - and
   * rx_start itself - go out with no handshake behind them.  Do it here,
   * on both sides of rx_start, and time how long it takes to self-clear.
   * If it never clears the RX module is not accepting configuration at
   * all, which is exactly what "clocks on the pads, data on DIN, and
   * still zero bytes" looks like.
   */

  {
    int spins;

    modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_UPDATE);

    for (spins = 0;
         spins < I2S_UPDATE_SPIN_MAX &&
         (getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_UPDATE);
         spins++)
      {
      }

    g_v388_pre_spins = spins;
  }

  modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_START);

  {
    int spins;

    modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_UPDATE);

    for (spins = 0;
         spins < I2S_UPDATE_SPIN_MAX &&
         (getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_UPDATE);
         spins++)
      {
      }

    g_v388_post_spins = spins;

    /* [V682] The DMA is armed and RX_START has been raised twice; verify that
     * the bit actually stuck, and re-assert it once if it did not.  The
     * v682a field run saw later PTT sessions come up with
     * rxconf = 0x00089600 (bit2 = 0) and 0 chunks, so "START was written once
     * and lost" is a real failure mode on this part.  The readback is kept for
     * the i2s_receive() report.
     */

    g_v682_rxstart =
      (getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_START) ? 1 : 0;

    if (!g_v682_rxstart)
      {
        modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_START);
        modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_UPDATE);

        g_v682_rxstart =
          (getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_START) ?
          1 : 0;

        if (!g_v682_rxstart)
          {
            g_v682_start_lost++;
          }
      }
  }

  if (g_v388_logs < 4)
    {
      g_v388_logs++;
      /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
             "[V388] rx_update spins pre=%d post=%d rxconf=0x%08x "
             "eofnum=0x%08x fifo=0x%08x intena=0x%08x\n",
             g_v388_pre_spins, g_v388_post_spins,
             (unsigned)getreg32(I2S_RX_CONF_REG(priv->config->port)),
             (unsigned)getreg32(I2S_RXEOF_NUM_REG(priv->config->port)),
             (unsigned)getreg32(I2S_RX_CLKM_CONF_REG(priv->config->port)),
             (unsigned)getreg32(I2S_INT_ENA_REG(priv->config->port)));

      /* [V389] I2S_INT_ST_REG is gated by INT_ENA, which reads 0, so it
       * can never show anything.  I2S_INT_RAW_REG is not gated:
       *   bit0 RX_DONE, bit1 TX_DONE, bit2 RX_HUNG, bit3 TX_HUNG
       * and rx_tdm_ctrl is the register V389 now programs.
       */
      /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
             "[V389] intraw=0x%08x rxtdmctrl=0x%08x rxconf1=0x%08x\n",
             (unsigned)getreg32(I2S_INT_RAW_REG(priv->config->port)),
             (unsigned)getreg32(I2S_RX_TDM_CTRL_REG(priv->config->port)),
             (unsigned)getreg32(I2S_RX_CONF1_REG(priv->config->port)));
    }

  /* [V393] V387's post-arm dump is disabled: i2s_rxdma_start() runs in
   * interrupt context when it re-arms the next buffer, and its 200000
   * spin loop plus syslog() overflowed the 2 KB IRQ stack
   * (v392run.log: sp 0x3fccbeb0, base 0x3fccb800, size 2048).
   * It has served its purpose - it is what proved the receiver was
   * never requesting a transfer. */
#if 0
  /* [V387] Give the channel a moment, then read what it actually did. */

  {
    static int s_v387_n;

    if (s_v387_n < 4)
      {
        volatile uint32_t sink;
        uint32_t k;

        s_v387_n++;

        for (k = 0; k < 200000u; k++)
          {
            sink = GET_GDMA_CH_REG(DMA_IN_INT_RAW_CH0_REG,
                                   priv->dma_channel);
            if (sink & (DMA_IN_SUC_EOF_CH0_INT_RAW |
                        DMA_IN_DONE_CH0_INT_RAW |
                        DMA_IN_ERR_EOF_CH0_INT_RAW |
                        DMA_IN_DSCR_ERR_CH0_INT_RAW |
                        DMA_IN_DSCR_EMPTY_CH0_INT_RAW |
                        DMA_INFIFO_OVF_L3_CH0_INT_RAW |
                        DMA_INFIFO_UDF_L1_CH0_INT_RAW))
              {
                break;
              }
          }

        

        
      }
  }

#endif

  /* [RX-DIAG] Dump RX state on every DMA arm to diagnose 0-data capture */
  {
    static unsigned int arm_cnt = 0;
    arm_cnt++;
    /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
           "[RXDUMP] arm#%u rxconf=0x%08x eof_nbytes=0x%08x i2sintst=0x%08x"
           " gdmainst=0x%08x isr=%u eof=%u\n",
           arm_cnt,
           (unsigned)getreg32(I2S_RX_CONF_REG(priv->config->port)),
           (unsigned)getreg32(I2S_RXEOF_NUM_REG(priv->config->port)),
           (unsigned)getreg32(I2S_INT_ST_REG(priv->config->port)),
           (unsigned)GET_GDMA_CH_REG(DMA_IN_INT_ST_CH0_REG,
                                      priv->dma_channel),
           g_v94_isr, g_v94_eof);
  }

  return OK;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_txdma_setup
 *
 * Description:
 *   Setup the next TX DMA transfer
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *   bfcontainer - The buffer container to be set up
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure
 *
 * Assumptions:
 *   Interrupts are disabled
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static IRAM_ATTR int i2s_txdma_setup(struct esp32s3_i2s_s *priv,
                                     struct esp32s3_buffer_s *bfcontainer)
{
  int ret = OK;
  size_t carry_size;
  uint32_t bytes_queued;
  uint32_t data_copied;
  struct ap_buffer_s *apb;
  struct esp32s3_dmadesc_s *outlink;
  apb_samp_t samp_size;
  irqstate_t flags;
  uint8_t *buf;
  uint8_t padding;
  uint8_t *samp;

  DEBUGASSERT(bfcontainer && bfcontainer->apb);

  apb = bfcontainer->apb;
  outlink = bfcontainer->dma_link;

  /* Get the transfer information, accounting for any data offset */

  const apb_samp_t bytes_per_sample = priv->data_width / 8;
  samp = &apb->samp[apb->curbyte];
  samp_size = (apb->nbytes - apb->curbyte) + priv->tx.carry.bytes;
  carry_size = samp_size % bytes_per_sample;

  /* Allocate the current audio buffer considering the remaining bytes
   * carried from the last upper half audio buffer.
   */

  /* [V211] Must be internal SRAM: see i2s_dmabuf_alloc(). */
  bfcontainer->buf = i2s_dmabuf_alloc(bfcontainer->nbytes);
  if (bfcontainer->buf == NULL)
    {
      i2serr("Failed to allocate the DMA internal buffer "
             "[%" PRIu32 " bytes]", bfcontainer->nbytes);
      return -ENOMEM;
    }

  data_copied = 0;
  buf = bfcontainer->buf;

  /* Copy the remaining bytes from the last audio buffer to the current
   * audio buffer. The remaining bytes are part of a sample that was split
   * between the last and the current audio buffer. Also, copy the bytes
   * from that split sample that are on the current buffer to the internal
   * buffer.
   */

  if (priv->tx.carry.bytes)
    {
      memcpy(buf, &priv->tx.carry.value, priv->tx.carry.bytes);
      buf += priv->tx.carry.bytes;
      data_copied += priv->tx.carry.bytes;
      memcpy(buf, samp, (bytes_per_sample - priv->tx.carry.bytes));
      buf += (bytes_per_sample - priv->tx.carry.bytes);
      samp += (bytes_per_sample - priv->tx.carry.bytes);
      data_copied += (bytes_per_sample - priv->tx.carry.bytes);
    }

  /* Copy the upper half buffer to the internal buffer considering that
   * the current upper half buffer may not contain a complete sample at
   * the end of the buffer (and those bytes needs to be carried to the
   * next audio buffer).
   */

  memcpy(buf, samp, samp_size - (data_copied + carry_size));
  buf += samp_size - (data_copied + carry_size);
  samp += samp_size - (data_copied + carry_size);
  data_copied += samp_size - (data_copied + carry_size);

  /* If the audio buffer's size is not a multiple of the sample size,
   * it's necessary to carry the remaining bytes that are part of what
   * would be the last sample on this buffer. These bytes will then be
   * saved and inserted at the beginning of the next DMA buffer to
   * rebuild the sample correctly.
   */

  priv->tx.carry.bytes = carry_size;

  if (priv->tx.carry.bytes)
    {
      memcpy((uint8_t *)&priv->tx.carry.value, samp, priv->tx.carry.bytes);
    }

  /* Release our reference on the audio buffer. This may very likely
   * cause the audio buffer to be freed.
   */

  apb_free(bfcontainer->apb);

  /* Configure DMA stream */

  bytes_queued = esp32s3_dma_setup(outlink, I2S_DMADESC_NUM,
                                   bfcontainer->buf,
                                   bfcontainer->nbytes, I2S_TX,
                                   priv->dma_channel);

  if (bytes_queued != bfcontainer->nbytes)
    {
      i2serr("Failed to enqueue I2S buffer "
             "(%" PRIu32 " bytes of %" PRIu32 ")\n",
             bytes_queued, bfcontainer->nbytes);
      i2s_dmabuf_release(bfcontainer->buf);   /* [V211] return pool slot */
      bfcontainer->buf = NULL;
      return -bytes_queued;
    }

  flags = spin_lock_irqsave(&priv->slock);

  /* Add the buffer container to the end of the TX pending queue */

  sq_addlast((sq_entry_t *)bfcontainer, &priv->tx.pend);

  /* Trigger DMA transfer if no transmission is in progress */

  /* [V362] attribute any bail from this call to site 1. */
  g_tx_callsite = 1;

  /* [V364] A buffer is about to be armed: re-enable the TX completion
   * interrupt sources that the idle bail may have gated.  This is the
   * only place a buffer enters priv->tx.pend, so it is the only place the
   * unmask can be needed -- and doing it here guarantees we can never
   * leave the channel permanently muted.
   */

  i2s_tx_irq_unmask(priv);

  ret = i2s_txdma_start(priv);

  spin_unlock_irqrestore(&priv->slock, flags);

  return ret;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rxdma_setup
 *
 * Description:
 *   Setup the next RX DMA transfer
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *   bfcontainer - The buffer container to be set up
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure
 *
 * Assumptions:
 *   Interrupts are disabled
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static int i2s_rxdma_setup(struct esp32s3_i2s_s *priv,
                           struct esp32s3_buffer_s *bfcontainer)
{
  int ret = OK;
  struct esp32s3_dmadesc_s *inlink;
  uint32_t bytes_queued;
  irqstate_t flags;

  DEBUGASSERT(bfcontainer && bfcontainer->apb);

  inlink = bfcontainer->dma_link;

  /* [V307][RXBOUNCE] The capture apb payload (apb->samp) comes from the
   * common heap, which on this board is PSRAM
   * (CONFIG_ESP32S3_SPIRAM_COMMON_HEAP=y).  The I2S GDMA IN channel never
   * completes a single descriptor when the target buffer is in PSRAM:
   * i2s_rxdma_start() arms fine ([RXDUMP] xfer ret=0) yet neither the I2S
   * nor the GDMA interrupt status ever moves (i2sintst=0 gdmainst=0
   * isr=0), the SUC_EOF interrupt never fires and the capture starves --
   * reported by audio_capture_native as "capture EIO".  Mirror the TX V211
   * strategy: DMA into an internal-SRAM bounce slot, then i2s_rx_worker()
   * copies the received bytes back into the apb and returns the slot. */

  
  bfcontainer->buf = NULL;
  uint8_t *rx_target = bfcontainer->apb->samp;
  if (esp32s3_ptr_extram(rx_target))
    {
      bfcontainer->buf = i2s_dmabuf_alloc(bfcontainer->nbytes);
      if (bfcontainer->buf == NULL)
        {
          i2serr("[RXBOUNCE] no internal-SRAM slot for %u bytes\n",
                 (unsigned)bfcontainer->nbytes);
          return -ENOMEM;
        }

      /* [V659] Rate-limited: this is one line per received buffer, so a
       * capture session alone produced ~190 of them and helped push the real
       * diagnostics out of the 32 KB kmsg ring. */

      {
        static unsigned s_v659_bounce;

        if (s_v659_bounce < 4u || (s_v659_bounce % 256u) == 0u)
          {
            syslog(LOG_INFO, "[RXBOUNCE] apb samp %p in PSRAM -> bounce %p\n",
                   (void *)bfcontainer->apb->samp, (void *)bfcontainer->buf);
          }

        s_v659_bounce++;
      }
      rx_target = bfcontainer->buf;
    }

  

  /* Configure DMA stream */

  bytes_queued = esp32s3_dma_setup(inlink, I2S_DMADESC_NUM,
                                   rx_target,
                                   bfcontainer->nbytes, I2S_RX,
                                   priv->dma_channel);

  

  

  if (bytes_queued != bfcontainer->nbytes)
    {
      i2serr("Failed to enqueue I2S buffer "
             "(%" PRIu32 " bytes of %" PRIu32 ")\n",
             bytes_queued, bfcontainer->nbytes);
      i2s_dmabuf_release(bfcontainer->buf);   /* [V307] no-op if NULL */
      bfcontainer->buf = NULL;
      return -bytes_queued;
    }

  flags = spin_lock_irqsave(&priv->slock);

  

  /* Add the buffer container to the end of the RX pending queue */

  sq_addlast((sq_entry_t *)bfcontainer, &priv->rx.pend);

  /* Trigger DMA transfer if no transmission is in progress */

  

  ret = i2s_rxdma_start(priv);

  

  spin_unlock_irqrestore(&priv->slock, flags);

  

  return ret;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_tx_schedule
 *
 * Description:
 *   An TX DMA completion has occurred.  Schedule processing on
 *   the working thread.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *   outlink - DMA outlink descriptor that triggered the interrupt.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   - Interrupts are disabled
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static void IRAM_ATTR i2s_tx_schedule(struct esp32s3_i2s_s *priv,
                                      struct esp32s3_dmadesc_s *outlink)
{
  struct esp32s3_buffer_s *bfcontainer;
  struct esp32s3_dmadesc_s *bfdesc;
  int ret;

  /* [V345] Record every entry and whether there was an active buffer. */

  g_sched_n++;

  if (sq_empty(&priv->tx.act))
    {
      g_sched_noact++;
    }

  /* Upon entry, the transfer(s) that just completed are the ones in the
   * priv->tx.act queue.
   */

  /* Move all entries from the tx.act queue to the tx.done queue */

  if (!sq_empty(&priv->tx.act))
    {
      /* Remove the next buffer container from the tx.act list */

      bfcontainer = (struct esp32s3_buffer_s *)sq_peek(&priv->tx.act);

      /* Check if the DMA descriptor that generated an EOF interrupt is the
       * last descriptor of the current buffer container's DMA outlink.
       * REVISIT: what to do if we miss syncronization and the descriptor
       * that generated the interrupt is different from the expected (the
       * oldest of the list containing active transmissions)?
       */

      /* Find the last descriptor of the current buffer container */

      bfdesc = bfcontainer->dma_link;
      while (!(bfdesc->ctrl & ESP32S3_DMA_CTRL_EOF))
        {
          DEBUGASSERT(bfdesc->next);
          bfdesc = bfdesc->next;
        }

      if (bfdesc == outlink)
        {
          g_sched_match++;   /* [V345] */

          sq_remfirst(&priv->tx.act);

          /* Report the result of the transfer */

          bfcontainer->result = OK;

          /* Add the completed buffer container to the tail of the tx.done
           * queue
           */

          sq_addlast((sq_entry_t *)bfcontainer, &priv->tx.done);

          /* Check if the DMA is IDLE */

          if (sq_empty(&priv->tx.act))
            {
              /* Then start the next DMA. */

              /* [V362] attribute any bail from this call to site 2. */
              g_tx_callsite = 2;
              i2s_txdma_start(priv);
            }
        }

      /* If the worker has completed running, then reschedule the working
       * thread.
       */

      if (work_available(&priv->tx.work))
        {
          /* Schedule the TX DMA done processing to occur on the worker
           * thread.
           */

          ret = work_queue(HPWORK, &priv->tx.work, i2s_tx_worker, priv, 0);
          if (ret != 0)
            {
              i2serr("ERROR: Failed to queue TX work: %d\n", ret);
            }
        }
    }
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rx_schedule
 *
 * Description:
 *   An RX DMA completion has occurred.  Schedule processing on
 *   the working thread.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *   inlink - DMA inlink descriptor that triggered the interrupt.
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   - Interrupts are disabled
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static void i2s_rx_schedule(struct esp32s3_i2s_s *priv,
                            struct esp32s3_dmadesc_s *inlink)
{
  struct esp32s3_buffer_s *bfcontainer;
  struct esp32s3_dmadesc_s *bfdesc;
  int ret;

  /* Upon entry, the transfer(s) that just completed are the ones in the
   * priv->rx.act queue.
   */

  /* Move all entries from the rx.act queue to the rx.done queue */

  if (!sq_empty(&priv->rx.act))
    {
      /* Remove the next buffer container from the rx.act list */

      bfcontainer = (struct esp32s3_buffer_s *)sq_peek(&priv->rx.act);

      /* Find the last descriptor of the current buffer container */

      bfdesc = bfcontainer->dma_link;

      while (bfdesc->next != NULL &&
             (bfdesc->next->ctrl & ESP32S3_DMA_CTRL_EOF))
        {
          bfdesc = bfdesc->next;
        }

      if (bfdesc == inlink)
        {
          sq_remfirst(&priv->rx.act);

          /* Report the result of the transfer */

          bfcontainer->result = OK;

          /* Add the completed buffer container to the tail of the rx.done
           * queue
           */

          sq_addlast((sq_entry_t *)bfcontainer, &priv->rx.done);

          /* Check if the DMA is IDLE */

          if (sq_empty(&priv->rx.act))
            {
              /* Then start the next DMA. */

              i2s_rxdma_start(priv);
            }
        }

      /* If the worker has completed running, then reschedule the working
       * thread.
       */

      if (work_available(&priv->rx.work))
        {
          /* Schedule the RX DMA done processing to occur on the worker
           * thread.
           */

          ret = work_queue(HPWORK, &priv->rx.work, i2s_rx_worker, priv, 0);
          if (ret != 0)
            {
              i2serr("ERROR: Failed to queue RX work: %d\n", ret);
            }
        }
    }
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_tx_worker
 *
 * Description:
 *   TX transfer done worker
 *
 * Input Parameters:
 *   arg - the I2S device instance cast to void*
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static void i2s_tx_worker(void *arg)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)arg;
  struct esp32s3_buffer_s *bfcontainer;
  irqstate_t flags;

  DEBUGASSERT(priv);

  /* When the transfer was started, the active buffer containers were removed
   * from the tx.pend queue and saved in the tx.act queue.  We get here when
   * the DMA is finished.
   *
   * In any case, the buffer containers in tx.act will be moved to the end
   * of the tx.done queue and tx.act will be emptied before this worker is
   * started.
   *
   */

  g_worker_n++;   /* [V345] */

  

  /* Process each buffer in the tx.done queue */

  while (sq_peek(&priv->tx.done) != NULL)
    {
      /* Remove the buffer container from the tx.done queue.  NOTE that
       * interrupts must be disabled to do this because the tx.done queue is
       * also modified from the interrupt level.
       */

      flags = spin_lock_irqsave(&priv->slock);
      bfcontainer = (struct esp32s3_buffer_s *)sq_remfirst(&priv->tx.done);
      spin_unlock_irqrestore(&priv->slock, flags);

      /* Perform the TX transfer done callback */

      g_worker_cb++;   /* [V345] */
      DEBUGASSERT(bfcontainer && bfcontainer->callback);
      bfcontainer->callback(&priv->dev, bfcontainer->apb,
                            bfcontainer->arg, bfcontainer->result);

      /* Release the internal buffer used by the DMA outlink */

      i2s_dmabuf_release(bfcontainer->buf);   /* [V211] internal-SRAM pool */
      bfcontainer->buf = NULL;                /* [V307] avoid double free */

      /* And release the buffer container */

      i2s_buf_free(priv, bfcontainer);
    }
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rx_worker
 *
 * Description:
 *   RX transfer done worker
 *
 * Input Parameters:
 *   arg - the I2S device instance cast to void*
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static void i2s_rx_worker(void *arg)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)arg;
  struct esp32s3_buffer_s *bfcontainer;
  struct esp32s3_dmadesc_s *dmadesc;
  irqstate_t flags;

  DEBUGASSERT(priv);

  /* When the transfer was started, the active buffer containers were removed
   * from the rx.pend queue and saved in the rx.act queue. We get here when
   * the DMA is finished.
   *
   * In any case, the buffer containers in rx.act will be moved to the end
   * of the rx.done queue and rx.act will be emptied before this worker is
   * started.
   *
   */

  i2sinfo("rx.act.head=%p rx.done.head=%p\n",
          priv->rx.act.head, priv->rx.done.head);

  /* Process each buffer in the rx.done queue */

  while (sq_peek(&priv->rx.done) != NULL)
    {
      /* Remove the buffer container from the rx.done queue.  NOTE that
       * interrupts must be disabled to do this because the rx.done queue is
       * also modified from the interrupt level.
       */

      flags = spin_lock_irqsave(&priv->slock);
      bfcontainer = (struct esp32s3_buffer_s *)sq_remfirst(&priv->rx.done);
      spin_unlock_irqrestore(&priv->slock, flags);

      dmadesc = bfcontainer->dma_link;

      

      bfcontainer->apb->nbytes = 0;

      while (dmadesc != NULL && (dmadesc->ctrl & ESP32S3_DMA_CTRL_EOF))
        {
          bfcontainer->apb->nbytes +=
              (dmadesc->ctrl >> ESP32S3_DMA_CTRL_DATALEN_S) &
              ESP32S3_DMA_CTRL_DATALEN_V;
          dmadesc = dmadesc->next;
        }

      /* [V307][RXBOUNCE] DMA landed in the internal-SRAM bounce slot;
       * deliver the received bytes to the real (PSRAM) apb payload and
       * return the slot to the pool. */

      if (bfcontainer->buf != NULL)
        {
          uint32_t ncopy = bfcontainer->apb->nbytes;

          if (ncopy > bfcontainer->apb->nmaxbytes)
            {
              ncopy = bfcontainer->apb->nmaxbytes;
            }

          
          memcpy(bfcontainer->apb->samp, bfcontainer->buf, ncopy);
          bfcontainer->apb->nbytes = ncopy;
          i2s_dmabuf_release(bfcontainer->buf);
          bfcontainer->buf = NULL;
        }

      /* Perform the RX transfer done callback */

      DEBUGASSERT(bfcontainer && bfcontainer->callback);

      if (priv->streaming == false)
        {
          bfcontainer->apb->flags |= AUDIO_APB_FINAL;
        }

      bfcontainer->callback(&priv->dev, bfcontainer->apb,
                            bfcontainer->arg, bfcontainer->result);

      /* Release our reference on the audio buffer. This may very likely
       * cause the audio buffer to be freed.
       */

      apb_free(bfcontainer->apb);

      /* And release the buffer container */

      i2s_buf_free(priv, bfcontainer);
    }
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_configure
 *
 * Description:
 *   Configure I2S
 *
 * Input Parameters:
 *   priv - Partially initialized I2S device structure.  This function
 *          will complete the I2S specific portions of the initialization
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

/* [V93] Time-division direction override.
 *
 * See the file header: with both engines enabled the receiver is a slave to
 * the transmitter's clock, and the transmitter is silent during capture, so
 * capture never sees a bit.  While a capture is in flight we make
 * i2s_configure() believe tx_en = 0, which selects the "rx + master" branch.
 */

static int g_i2s_rxonly_port = -1;

/* [V380] Diagnostic direction overrides.  g_v380_loopback is switched by
 * the capture layer between phase 1 (internal loopback) and phase 2
 * (TX clocks the real codec); g_v380_lb_applied remembers the value that
 * i2s_configure() has already written to the hardware so flipping the
 * flag actually takes effect on the next call.
 */

volatile int g_v380_loopback = 0;
volatile int g_v380_txmaster = 1;
static int   g_v380_lb_applied = -1;

static bool i2s_tx_active(FAR struct esp32s3_i2s_s *priv)
{
  /* [V380] Both phases need the transmitter to own the port: the
   * receiver stays a slave to the TX BCK/WS generator and the pins keep
   * carrying the TX clock output signals.  The rxonly override is what
   * used to flip this back and forth between the two es8311 workers.
   */

  if (g_v380_txmaster || g_v380_loopback)
    {
      return true;
    }

  if (g_i2s_rxonly_port >= 0 &&
      (int)priv->config->port == g_i2s_rxonly_port)
    {
      return false;
    }

  return priv->config->tx_en;
}

static bool i2s_rx_active(FAR struct esp32s3_i2s_s *priv)
{
  return priv->config->rx_en;
}



static void i2s_configure(struct esp32s3_i2s_s *priv)
{
  uint32_t tx_conf  = 0;
  uint32_t tx_conf1 = 0;
  uint32_t rx_conf  = 0;
  uint32_t rx_conf1 = 0;

  /* Set peripheral clock and clear reset */

  if (priv->config->port == ESP32S3_I2S0)
    {
      modifyreg32(SYSTEM_PERIP_CLK_EN0_REG, 0, SYSTEM_I2S0_CLK_EN);
      modifyreg32(SYSTEM_PERIP_RST_EN0_REG, 0, SYSTEM_I2S0_RST);
      modifyreg32(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S0_RST, 0);
    }
  else
    {
      modifyreg32(SYSTEM_PERIP_CLK_EN0_REG, 0, SYSTEM_I2S1_CLK_EN);
      modifyreg32(SYSTEM_PERIP_RST_EN0_REG, 0, SYSTEM_I2S1_RST);
      modifyreg32(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_I2S1_RST, 0);
    }

  /* [V340] GDMA 模块初始化：这个调用原先根本不存在。
   *
   * esp32s3_dma_init() 使能 GDMA 外设时钟、解除其复位，
   * 并给 GDMA 的『描述符 RAM』上电 + 打开时钟门控 (DMA_PD_CONF)。
   * 不做这三件事，outlink FSM 一起来就停车：START 写了被清、PARK=1、
   * OWNER 位永不被取走、中断永不来 —— 也就是我们喇叭的症状。
   * 该函数在全树里只有定义、没有任何调用者（V226 写了修复却没接线）。 */

  esp32s3_dma_init();

  if (!(getreg32(I2S_TX_CLKM_CONF_REG(priv->config->port)) & I2S_CLK_EN))
    {
      i2sinfo("Enabling I2S clock...\n");
      modifyreg32(I2S_TX_CLKM_CONF_REG(priv->config->port), 0, I2S_CLK_EN);
    }

  /* Configure multiplexed pins as connected on the board */

  /* TODO: check for loopback mode */

  /* Enable TX channel */

  if (priv->config->dout_pin != I2S_GPIO_UNUSED)
    {
      /* If TX channel is used, enable the clock source */

      if (!(getreg32(I2S_TX_CLKM_CONF_REG(priv->config->port)) &
            I2S_TX_CLK_ACTIVE))
        {
          i2sinfo("Activating I2S TX clock...\n");

          /* [V341] The source clock is selected in exactly ONE place now:
           * i2s_clk_src_select(), driven by i2s_clock_source_bringup() at the
           * end of this function.  Only the clock gate is opened here. */

          modifyreg32(I2S_TX_CLKM_CONF_REG(priv->config->port), 0,
                      I2S_TX_CLK_ACTIVE);
        }

      esp32s3_gpiowrite(priv->config->dout_pin, 1);
      esp32s3_configgpio(priv->config->dout_pin, OUTPUT_FUNCTION_2);
      esp32s3_gpio_matrix_out(priv->config->dout_pin,
                              priv->config->dout_outsig, 0, 0);
    }

  /* Enable RX channel */

  if (priv->config->din_pin != I2S_GPIO_UNUSED)
    {
      /* If RX channel is used, enable the clock source */

      if (!(getreg32(I2S_RX_CLKM_CONF_REG(priv->config->port)) &
            I2S_RX_CLK_ACTIVE))
        {
          i2sinfo("Activating I2S RX clock...\n");

          /* [V341] Source selected centrally (see i2s_clk_src_select()). */

          modifyreg32(I2S_RX_CLKM_CONF_REG(priv->config->port), 0,
                      I2S_RX_CLK_ACTIVE);
        }

      esp32s3_configgpio(priv->config->din_pin, INPUT_FUNCTION_2);
      esp32s3_gpio_matrix_in(priv->config->din_pin,
                             priv->config->din_insig, 0);
    }

  if (priv->config->role == I2S_ROLE_SLAVE)
    {
      if (i2s_tx_active(priv) && !i2s_rx_active(priv))
        {
          /* For "tx + slave" mode, select TX signal index for ws and bck */

          esp32s3_gpiowrite(priv->config->ws_pin, 1);
          esp32s3_configgpio(priv->config->ws_pin, INPUT_FUNCTION_2);
          esp32s3_gpio_matrix_in(priv->config->ws_pin,
                                 priv->config->ws_out_insig, 0);

          esp32s3_gpiowrite(priv->config->bclk_pin, 1);
          esp32s3_configgpio(priv->config->bclk_pin, INPUT_FUNCTION_2);
          esp32s3_gpio_matrix_in(priv->config->bclk_pin,
                                 priv->config->bclk_out_insig, 0);
        }
      else
        {
          /* For "tx + rx + slave" or "rx + slave" mode, select RX signal
           * index for ws and bck.
           */

          esp32s3_gpiowrite(priv->config->ws_pin, 1);
          esp32s3_configgpio(priv->config->ws_pin, INPUT_FUNCTION_2);
          esp32s3_gpio_matrix_in(priv->config->ws_pin,
                                 priv->config->ws_in_insig, 0);

          esp32s3_gpiowrite(priv->config->bclk_pin, 1);
          esp32s3_configgpio(priv->config->bclk_pin, INPUT_FUNCTION_2);
          esp32s3_gpio_matrix_in(priv->config->bclk_pin,
                                 priv->config->bclk_in_insig, 0);
        }
    }
  else
    {
      /* Considering master role for the I2S port */

      /* Set MCLK pin */

      if (priv->config->mclk_pin != I2S_GPIO_UNUSED)
        {
          i2sinfo("Configuring GPIO%" PRIu8 " to output master clock\n",
                  priv->config->mclk_pin);

          esp32s3_gpiowrite(priv->config->mclk_pin, 1);
          esp32s3_configgpio(priv->config->mclk_pin, OUTPUT_FUNCTION_2);
          esp32s3_gpio_matrix_out(priv->config->mclk_pin,
                                  priv->config->mclk_out_sig, 0, 0);
        }

      /* [V682] The receiver owns the clock whenever it is the master
       * (g_v390_rxmaster is the compile-time default here), so it must own
       * the pads too.  Without the added term this condition is dead code:
       * i2s_tx_active() returns true unconditionally (g_v380_txmaster = 1),
       * so the pads stayed bound to the transmitter's generators, which are
       * idle during capture - measured v681: BCLK(10) = 0, WS(8) stuck high,
       * rxconf = 0x00089604 (rx_start = 1, rx_slave_mod = 0).
       *
       * Certainty: 已证实 (v682a real-device experiment: BCLK 0 -> 1000,
       * WS stuck high -> 1858, mic peak 6 -> 32768).
       */

      if (i2s_rx_active(priv) &&
          (g_v390_rxmaster || !i2s_tx_active(priv)))
        {
          /* For "rx + master" mode, select RX signal index for ws and bck */

          g_v682_pad_owner = 1;

          esp32s3_gpiowrite(priv->config->ws_pin, 1);
          esp32s3_configgpio(priv->config->ws_pin, OUTPUT_FUNCTION_2);
          esp32s3_gpio_matrix_out(priv->config->ws_pin,
                                  priv->config->ws_in_outsig, 0, 0);

          esp32s3_gpiowrite(priv->config->bclk_pin, 1);
          esp32s3_configgpio(priv->config->bclk_pin, OUTPUT_FUNCTION_2);
          esp32s3_gpio_matrix_out(priv->config->bclk_pin,
                                  priv->config->bclk_in_outsig, 0, 0);
        }
      else
        {
          /* For "tx + rx + master" or "tx + master" mode, select TX signal
           * index for ws and bck.
           */

          g_v682_pad_owner = 0;

          esp32s3_gpiowrite(priv->config->ws_pin, 1);
          esp32s3_configgpio(priv->config->ws_pin, OUTPUT_FUNCTION_2);
          esp32s3_gpio_matrix_out(priv->config->ws_pin,
                                  priv->config->ws_out_outsig, 0, 0);

          esp32s3_gpiowrite(priv->config->bclk_pin, 1);
          esp32s3_configgpio(priv->config->bclk_pin, OUTPUT_FUNCTION_2);
          esp32s3_gpio_matrix_out(priv->config->bclk_pin,
                                  priv->config->bclk_out_outsig, 0, 0);
        }
    }

  /* [V386] Feed the pads back into the receiver's clock inputs.
   *
   * Both branches above leave BCK/WS configured as outputs only.  When
   * the receiver is a slave (i2s_configure() sets I2S_RX_SLAVE_MOD
   * whenever the transmitter owns the port) it expects its clock on the
   * I2S0I_BCK_IN / I2S0I_WS_IN peripheral inputs, and those
   * gpio_matrix_in() connections are only made in the SLAVE branch of
   * this function.  In this master configuration nothing is attached to
   * them, so the receiver never sees a bit clock: it shifts nothing,
   * never reaches I2S_RXEOF_NUM, never raises a DMA request, and the
   * capture starves (v384/v385: i2sintst=0 gdmainst=0 isr=0 eof=0 with
   * the pads measured toggling).  A pad can drive an output and feed a
   * peripheral input at the same time, so this is free.
   */

  if (priv->config->rx_en)
    {
      esp32s3_gpio_matrix_in(priv->config->ws_pin,
                             priv->config->ws_in_insig, 0);
      esp32s3_gpio_matrix_in(priv->config->bclk_pin,
                             priv->config->bclk_in_insig, 0);

      /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
             "[V386] rx clock input routed: ws(gpio%d)<-sig%u "
             "bclk(gpio%d)<-sig%u\n",
             (int)priv->config->ws_pin,
             (unsigned)priv->config->ws_in_insig,
             (int)priv->config->bclk_pin,
             (unsigned)priv->config->bclk_in_insig);
    }

  /* Share BCLK and WS if in full-duplex mode */

  /* [V250] Keep the TX on its OWN BCK/WS -- never on the RX's.
   *
   * NuttX sets I2S_SIG_LOOPBACK whenever both directions are active, i.e. in
   * exactly the full-duplex configuration this board runs (ES8311 with mic
   * and speaker).  With that bit set the TX module takes its BCK/WS from the
   * RX side instead of its own generator, and the RX frame generator only runs
   * once the RX channel is actually started.  A TX without a shift clock never
   * drains its FIFO, and a TX that never drains its FIFO never asserts the
   * peripheral DMA request -- the GDMA outlink therefore reports PARK=0 (its
   * FSM has been kicked) yet OWNER in the first descriptor stays set forever
   * and no SUC_EOF interrupt is ever raised, which is precisely what the
   * register dump shows:
   *
   *   LINK=0x000b9db8 (START=0 PARK=0) OWNER=1 INTST=0x00000000 txirqN=0
   *
   * The BCLK/WS pins on this board are routed to the TX generator outputs
   * (bclk_out_outsig / ws_out_outsig), so the TX's own divider is the one the
   * codec actually sees.  The reference firmware that does produce sound on
   * this board (ESP-IDF i2s_std + esp_codec_dev) never sets this bit either.
   * Leave it cleared and let the TX clock itself. */

  if (g_v380_loopback)
    {
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_SIG_LOOPBACK);
    }
  else
    {
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_SIG_LOOPBACK, 0);
    }

  /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO, "[RAW] sig_loopback %s (tx_active=%d rx_active=%d)\n",
         g_v380_loopback ? "SET" : "cleared",
         (int)i2s_tx_active(priv), (int)i2s_rx_active(priv));

  /* Configure the TX module */

  if (i2s_tx_active(priv))
    {
      /* Reset I2S TX module */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_RESET);
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_RESET, 0);

      g_v671_cfg_valid = 0;   /* [V671] TX module reset */

      /* Enable/disable I2S TX slave mode */

      if (priv->config->role == I2S_ROLE_SLAVE)
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      0, I2S_TX_SLAVE_MOD);
        }
      else
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_SLAVE_MOD, 0);
        }

      /* Configure TX chan bit, audio data bit and mono mode.
       * On ESP32-S3, sample_bit should equals to data_bit.
       */

      /* Set TX data width */

      priv->data_width = priv->config->data_width;
      i2s_set_datawidth(priv);

      /* Set channel mode control */

      if (priv->config->audio_std_mode <= I2S_TDM_PCM)
        {
          /* [V228] TDM mode must stay ENABLED.
           *
           * Clearing tx_tdm_en (to match ESP-IDF's i2s_std, which never sets
           * it) was tried and measured: WS(8) stopped toggling entirely
           * (hi=2000/2000, stuck high, versus 1002/2000 with TDM enabled),
           * i.e. this driver only generates a frame clock in TDM mode.
           * The setting is load-bearing for the way this driver programs
           * tx_conf1 / tx_tdm_ctrl, so it is left alone. */
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_TDM_EN_M,
                      I2S_TX_TDM_EN);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_PDM_EN, 0);

          /* [V229] *** THE MISSING PIECE ***
           *
           * ESP-IDF's i2s_ll_tx_enable_tdm() -- which is exactly what
           * i2s_ll_tx_enable_std() calls for standard I2S -- is:
           *     tx_conf.tx_pdm_en        = false;
           *     tx_conf.tx_tdm_en        = true;
           *     tx_pcm2pdm_conf.pcm2pdm_conv_en = false;   <-- this one
           * This driver set the first two but NEVER wrote
           * I2S_TX_PCM2PDM_CONF_REG at all (verified: zero writes in the whole
           * file).  With the PCM->PDM converter enabled the TX data path is
           * routed into the PDM filter instead of onto the DAC/DMA path, which
           * matches every measurement: the bit clock and WS keep running, the
           * configuration registers all read back correct, and yet the I2S
           * never requests data from the GDMA so the outlink parks without
           * fetching a single descriptor.
           *
           * Standard I2S uses none of the PDM/PCM2PDM block, so clear the
           * whole register -- this mirrors ESP-IDF setting conv_en = false. */
          putreg32(0, I2S_TX_PCM2PDM_CONF_REG(priv->config->port));

          if (priv->config->audio_std_mode == I2S_TDM_PHILIPS)
            {
              modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                          I2S_TX_MSB_SHIFT_M, I2S_TX_MSB_SHIFT);

          /* [V236] Match ESP-IDF: I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG sets
           * left_align = true and i2s_hal_std_set_tx_slot() programs
           * i2s_ll_tx_enable_left_align(); this driver never touched
           * tx_left_align. */
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0,
                      I2S_TX_LEFT_ALIGN);
            }
          else
            {
              modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                          I2S_TX_MSB_SHIFT, 0);
            }
        }
      else
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_PDM_EN_M,
                      I2S_TX_PDM_EN);
        }

      /* Set the default level of WS */

      if (priv->config->ws_pol == 1)
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0,
                      I2S_TX_WS_IDLE_POL);
        }
      else
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_WS_IDLE_POL, 0);
        }

      /* The default value for the master clock frequency (MCLK frequency)
       * can be set from the sample rate multiplied by a fixed value, known
       * as MCLK multiplier. This multiplier, however, should be divisible
       * by the number of bytes from a sample, i.e, for 24 bits, the
       * multiplier should be divisible by 3. NOTE: the MCLK frequency can
       * be adjusted on runtime, so this value remains valid only if the
       * upper half does not implement the `i2s_setmclkfrequency` method.
       */

      if (priv->config->data_width == I2S_DATA_BIT_WIDTH_24BIT)
        {
          priv->mclk_multiple = I2S_MCLK_MULTIPLE_384;
        }
      else
        {
          priv->mclk_multiple = I2S_MCLK_MULTIPLE_256;
        }

      i2s_setmclkfrequency((struct i2s_dev_s *)priv, (priv->config->rate *
                           priv->mclk_multiple));

      priv->rate = priv->config->rate;

      /* [V341] Dividers + source clock are brought up once, at the end of
       * this function (i2s_clock_source_bringup()). */
    }

  /* Configure the RX module */

  if (i2s_rx_active(priv))
    {
      /* Reset I2S RX module */

      modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_RESET);
      modifyreg32(I2S_RX_CONF_REG(priv->config->port), I2S_RX_RESET, 0);

      /* Enable/disable I2S RX slave mode */

      if (priv->config->role == I2S_ROLE_SLAVE)
        {
          modifyreg32(I2S_RX_CONF_REG(priv->config->port),
                      0, I2S_RX_SLAVE_MOD);
        }
      else
        {
          /* Since BCLK and WS are shared, only TX or RX can be master. In
           * this case, force RX as slave to avoid conflict of clock signal.
           */

          /* [V391] V390 showed this decision is what actually sticks:
           * i2s_configure() runs after i2s_rx_channel_start() and
           * re-asserts slave mode, so flipping the bit there was
           * overwritten before the DMA was ever armed.  With
           * g_v390_rxmaster set, run the receiver from its own divider
           * instead of from I2S0I_BCK_IN / I2S0I_WS_IN.
           */

          if (i2s_tx_active(priv) && !g_v390_rxmaster)
            {
              modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0,
                          I2S_RX_SLAVE_MOD);
            }
          else
            {
              modifyreg32(I2S_RX_CONF_REG(priv->config->port),
                          I2S_RX_SLAVE_MOD, 0);
            }

          /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
                 "[V391] rx role=%s rxconf=0x%08x\n",
                 g_v390_rxmaster ? "MASTER" : "SLAVE",
                 (unsigned)getreg32(
                   I2S_RX_CONF_REG(priv->config->port)));
        }

      /* Congfigure RX chan bit, audio data bit and mono mode.
       * On ESP32, sample_bit should equals to data_bit.
       */

      /* Set RX data width */

      priv->data_width = priv->config->data_width;
      i2s_set_datawidth(priv);

      /* Set channel mode control */

      if (priv->config->audio_std_mode <= I2S_TDM_PCM)
        {
          /* [V228] See the TX comment: rx_tdm_en must stay enabled. */
          modifyreg32(I2S_RX_CONF_REG(priv->config->port), I2S_RX_TDM_EN_M,
                      I2S_RX_TDM_EN);

          /* [V389] *** the register this driver has never written ***
           *
           * hal/esp32s3/include/hal/i2s_ll.h:602, in
           * i2s_ll_rx_select_std_slot() -- which every standard-I2S
           * receive path runs:
           *
           *     hw->rx_tdm_ctrl.rx_tdm_tot_chan_num = 1;  (2 slots - 1)
           *     hw->rx_tdm_ctrl.val = chan_mask;          (slot enable)
           *
           * I2S_RX_TDM_CTRL_REG (0x50) is absent from this file entirely,
           * so rx_tdm_tot_chan_num has been sitting at its reset default
           * of 0 while RX_TDM_EN is set and BCLK/WS/DIN are all measured
           * toggling.  The receiver then never produces a DMA request:
           * descriptor OWNER stays 1, no GDMA interrupt, zero bytes --
           * including with I2S_SIG_LOOPBACK set, which proves the fault
           * is inside the RX channel, not on the pins or in the codec.
           *
           * Program it exactly the way ESP-IDF does: 2 slots, both
           * enabled (the reset value already has every chanN_en bit set,
           * but write them so the intent survives a reset default
           * change).
           */
          modifyreg32(I2S_RX_TDM_CTRL_REG(priv->config->port),
                      I2S_RX_TDM_TOT_CHAN_NUM_M,
                      FIELD_TO_VALUE(I2S_RX_TDM_TOT_CHAN_NUM, 1));

          /* [V680] Mono capture must read ONE sample per frame.
           *
           * The RX branch used to arm CHAN0_EN and CHAN1_EN at the same bit
           * positions as the TX bug fixed in v669: two slots are armed, so
           * the RX FIFO hands the DMA two samples per frame and every
           * consumer of the captured stream (the peak meter, ASR) sees
           * interleaved garbage -- which is why the board logs "[V64] mic
           * peak=0" and the assistant only ever heard noise.  The panel
           * captures mono (nchannels == 1), so only slot 0 may be enabled.
           *
           * Done here, on every configure, so no later ordering can undo
           * it -- the same reason the TX fix lives in i2s_txdma_start().
           */
          modifyreg32(I2S_RX_TDM_CTRL_REG(priv->config->port),
                      I2S_RX_TDM_PDM_CHAN0_EN_M |
                      I2S_RX_TDM_PDM_CHAN1_EN_M,
                      g_i2s_rx_mono ? I2S_RX_TDM_PDM_CHAN0_EN
                                    : (I2S_RX_TDM_PDM_CHAN0_EN |
                                       I2S_RX_TDM_PDM_CHAN1_EN));
          {
            static int s_v680_logged;
            if (s_v680_logged < 3)
              {
                s_v680_logged++;
                syslog(LOG_INFO, "[V680] rx tdm ctrl=0x%08x mono=%d\n",
                       (unsigned)getreg32(I2S_RX_TDM_CTRL_REG(priv->config->port)),
                       g_i2s_rx_mono);
              }
          }

          /* [V229] See the TX comment: ESP-IDF's i2s_ll_rx_enable_tdm() also
           * clears rx_pdm2pcm_en, and this driver never wrote the register. */
          modifyreg32(I2S_RX_CONF_REG(priv->config->port),
                      I2S_RX_PDM2PCM_EN, 0);

          if (priv->config->audio_std_mode == I2S_TDM_PHILIPS)
            {
              modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                          I2S_RX_MSB_SHIFT_M, I2S_RX_MSB_SHIFT);
            }
          else
            {
              modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                          I2S_RX_MSB_SHIFT, 0);
            }
        }
      else
        {
          modifyreg32(I2S_RX_CONF_REG(priv->config->port), I2S_RX_PDM_EN_M,
                      I2S_RX_PDM_EN);
        }

      /* Set I2S RX right channel first */

      if (priv->config->ws_pol == 1)
        {
          modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0,
                      I2S_RX_BIG_ENDIAN);
        }
      else
        {
          modifyreg32(I2S_RX_CONF_REG(priv->config->port),
                      I2S_RX_BIG_ENDIAN, 0);
        }

      /* The default value for the master clock frequency (MCLK frequency)
       * can be set from the sample rate multiplied by a fixed value, known
       * as MCLK multiplier. This multiplier, however, should be divisible
       * by the number of bytes from a sample, i.e, for 24 bits, the
       * multiplier should be divisible by 3. NOTE: the MCLK frequency can
       * be adjusted on runtime, so this value remains valid only if the
       * upper half does not implement the `i2s_setmclkfrequency` method.
       */

      if (priv->config->data_width == I2S_DATA_BIT_WIDTH_24BIT)
        {
          priv->mclk_multiple = I2S_MCLK_MULTIPLE_384;
        }
      else
        {
          priv->mclk_multiple = I2S_MCLK_MULTIPLE_256;
        }

      i2s_setmclkfrequency((struct i2s_dev_s *)priv, (priv->config->rate *
                           priv->mclk_multiple));

      priv->rate = priv->config->rate;
    }

  /* [V341] Bring up the source clock exactly once, after the TX and/or RX
   * blocks above have programmed priv->rate / priv->mclk_freq / data_width.
   * This both selects the source and verifies that it actually advances the
   * TX clock domain (the tx_update handshake), falling back to XTAL if the
   * 160 MHz source is not running. */

  if (i2s_tx_active(priv) || i2s_rx_active(priv))
    {
      (void)i2s_clock_source_bringup(priv);
    }
}

/* [V93] Give the active direction sole ownership of the port.
 *
 * Called from the ES8311 worker immediately before an I2S_SEND or an
 * I2S_RECEIVE.  Idempotent: reconfiguration happens only when the direction
 * actually changes, so a stream of capture buffers does not restart the DMA.
 */

/* [V660] One-shot capture-path health dump.
 *
 * Called from the application's capture thread immediately after
 * AUDIOIOC_START returns, i.e. from ordinary task context.  The same
 * measurement used to live in i2s_rx_channel_start() and had to be silenced
 * by [V394] because syslog() on the interrupt stack overflowed it (all of
 * [V382]/[V383]/[V390]/[RXCLK] are wrapped in `if (0)` for that reason).
 *
 * Why it matters: every PTT chunk in the v658 field run reported peak=6, and
 * the [V380] calibration says a healthy loopback shows peak == 8 while real
 * microphone audio shows "a much larger peak".  A constant 6 is an idle
 * serial stream, so the first question is whether the clocks are moving:
 *
 *   MCLK(11)/BCLK(10) hi ~= 1000/2000 -> clock running
 *   WS(8)             hi ~=  500/2000 -> 50% duty frame sync
 *   DIN(9)            hi ~= 1000/2000 -> the codec is shifting data
 *   any line stuck at 0 or 2000       -> that line is dead
 */

void esp32s3_i2s_rx_health(void)
{
  static int s_v660_n;
  unsigned int mhi = 0;
  unsigned int bhi = 0;
  unsigned int whi = 0;
  unsigned int dhi = 0;
  unsigned int k;

  if (s_v660_n >= 3)
    {
      return;
    }

  s_v660_n++;

  modifyreg32(IO_MUX_GPIO8_REG,  0, (1u << 9));   /* WS   */
  modifyreg32(IO_MUX_GPIO9_REG,  0, (1u << 9));   /* DIN  */
  modifyreg32(IO_MUX_GPIO10_REG, 0, (1u << 9));   /* BCLK */
  modifyreg32(IO_MUX_GPIO11_REG, 0, (1u << 9));   /* MCLK */

  for (k = 0; k < 2000; k++)
    {
      if (esp32s3_gpioread(11)) { mhi++; }
      if (esp32s3_gpioread(10)) { bhi++; }
      if (esp32s3_gpioread(8))  { whi++; }
      if (esp32s3_gpioread(9))  { dhi++; }
    }

  syslog(LOG_INFO,
         "[V660] pins MCLK(11)=%u BCLK(10)=%u WS(8)=%u ASDOUT-IN(9)=%u "
         "/2000 | "
         "rxconf=0x%08x txconf=0x%08x rxstart=%d txstart=%d rxonly=%d "
         "loopback=%d txmaster=%d\n",
         mhi, bhi, whi, dhi,
         (unsigned)getreg32(I2S_RX_CONF_REG(0)),
         (unsigned)getreg32(I2S_TX_CONF_REG(0)),
         (getreg32(I2S_RX_CONF_REG(0)) & I2S_RX_START) ? 1 : 0,
         (getreg32(I2S_TX_CONF_REG(0)) & I2S_TX_START) ? 1 : 0,
         g_i2s_rxonly_port, g_v380_loopback, g_v380_txmaster);
}

void esp32s3_i2s_set_rxonly(FAR struct i2s_dev_s *dev, bool on)
{
  FAR struct esp32s3_i2s_s *priv = (FAR struct esp32s3_i2s_s *)dev;
  int port = (int)priv->config->port;
  /* [V380] While the transmitter owns the port the capture worker must
   * not be able to take it away -- handing it back and forth is what
   * restarted the DMA underneath the other worker.  The loopback flag is
   * still honoured: a change there has to reach i2s_configure().
   */

  int want = (on && !g_v380_txmaster) ? port : -1;

  if (want == g_i2s_rxonly_port && g_v380_loopback == g_v380_lb_applied)
    {
      return;
    }

  /* [V644] PROBE: mid-stream port reconfiguration (the [V89] hazard). */
  

  /* Stop both engines before rewiring the clock. */

      /* [V341] The clock source and its dividers are now handled in exactly
       * one place: i2s_configure() -> i2s_clock_source_bringup().  The old
       * V241 block that re-wrote g_i2s_sclk / I2S_TX_CLK_SEL here (and thus
       * fought i2s_configure()) has been removed.  i2s_configure() is invoked
       * at the bottom of this function and will (re)select the source and
       * verify that it actually advances the TX clock domain. */

  modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_START, 0);
  modifyreg32(I2S_RX_CONF_REG(port), I2S_RX_START, 0);

  g_i2s_rxonly_port = want;
  g_v380_lb_applied = g_v380_loopback;

  i2s_configure(priv);
}

/****************************************************************************
 * Name: i2s_check_mclkfrequency
 *
 * Description:
 *   Check if MCLK frequency is compatible with the current data width and
 *   bits/sample set. Master clock should be multiple of the sample rate and
 *   bclk at the same time.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   Returns the current master clock or a negated errno value on failure.
 *
 ****************************************************************************/

static int32_t i2s_check_mclkfrequency(struct esp32s3_i2s_s *priv)
{
  uint32_t mclk_freq;
  uint32_t mclk_multiple = priv->mclk_multiple;
  uint32_t bclk = priv->rate * priv->config->total_slot * priv->data_width;
  int i;

  /* If the master clock is divisible by both the sample rate and the bit
   * clock, everything is as expected and we can return the current master
   * clock frequency.
   */

  if (priv->mclk_freq % priv->rate == 0 && priv->mclk_freq % bclk == 0)
    {
      priv->mclk_multiple = priv->mclk_freq / priv->rate;
      return priv->mclk_freq;
    }

  /* Select the lowest multiplier for setting the master clock */

  for (mclk_multiple = I2S_MCLK_MULTIPLE_128;
       mclk_multiple <= I2S_MCLK_MULTIPLE_512;
       mclk_multiple += I2S_MCLK_MULTIPLE_128)
    {
      mclk_freq = priv->rate * mclk_multiple;
      if (mclk_freq % priv->rate == 0 && mclk_freq % bclk == 0)
        {
          priv->mclk_multiple = mclk_multiple;
          i2s_setmclkfrequency((struct i2s_dev_s *)priv, mclk_freq);
          return priv->mclk_freq;
        }
    }

  return -EINVAL;
}

/****************************************************************************
 * Name: i2s_set_datawidth
 *
 * Description:
 *   Set the I2S TX/RX data width.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   Returns the resulting data width
 *
 ****************************************************************************/

static uint32_t i2s_set_datawidth(struct esp32s3_i2s_s *priv)
{
#ifdef I2S_HAVE_TX
  if (priv->config->tx_en)
    {
      modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                  I2S_TX_BITS_MOD_M, FIELD_TO_VALUE(I2S_TX_BITS_MOD,
                  priv->data_width - 1));

      modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                  I2S_TX_TDM_CHAN_BITS_M,
                  FIELD_TO_VALUE(I2S_TX_TDM_CHAN_BITS,
                  priv->data_width - 1));

      modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                  I2S_TX_HALF_SAMPLE_BITS_M,
                  FIELD_TO_VALUE(I2S_TX_HALF_SAMPLE_BITS,
                  priv->data_width - 1));

      if (priv->config->audio_std_mode == I2S_TDM_PCM)
        {
          modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                      I2S_TX_TDM_WS_WIDTH_M,
                      FIELD_TO_VALUE(I2S_TX_TDM_WS_WIDTH, 1));
        }
      else
        {
          modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
                      I2S_TX_TDM_WS_WIDTH_M,
                      FIELD_TO_VALUE(I2S_TX_TDM_WS_WIDTH,
                      priv->data_width - 1));
        }

      /* Set I2S_TX_UPDATE bit to update configs. Cleared automatically */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                  0, I2S_TX_UPDATE);
    }
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
  if (priv->config->rx_en)
    {
      modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                  I2S_RX_BITS_MOD_M, FIELD_TO_VALUE(I2S_RX_BITS_MOD,
                  priv->data_width - 1));

      modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                  I2S_RX_TDM_CHAN_BITS_M,
                  FIELD_TO_VALUE(I2S_RX_TDM_CHAN_BITS,
                  priv->data_width - 1));

      modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                  I2S_RX_HALF_SAMPLE_BITS_M,
                  FIELD_TO_VALUE(I2S_RX_HALF_SAMPLE_BITS,
                  priv->data_width - 1));

      if (priv->config->audio_std_mode == I2S_TDM_PCM)
        {
          modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                      I2S_RX_TDM_WS_WIDTH_M,
                      FIELD_TO_VALUE(I2S_RX_TDM_WS_WIDTH,
                      priv->data_width - 1));
        }
      else
        {
          modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
                      I2S_RX_TDM_WS_WIDTH_M,
                      FIELD_TO_VALUE(I2S_RX_TDM_WS_WIDTH, 1));
        }
    }
#endif /* I2S_HAVE_RX */

  return priv->data_width;
}

/****************************************************************************
 * Name: i2s_set_clock
 *
 * Description:
 *   Set the I2S TX sample rate by adjusting I2S clock.
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   Returns the resulting bitrate
 *
 ****************************************************************************/

static uint32_t i2s_set_clock(struct esp32s3_i2s_s *priv)
{
  uint32_t rate;
  uint32_t bclk;
  uint32_t mclk;
  uint32_t sclk;
  uint32_t mclk_div;
  int denominator;
  int numerator;
  uint32_t regval;
  uint32_t freq_diff;
  uint16_t bclk_div;

  /* TODO: provide other clock support modes */

  /* I2S module will using PLL_D2_CLK(160M) as source clock. */

  sclk = g_i2s_sclk;

  /* fmclk = bck_div * fbclk = fsclk / (mclk_div + b / a)
   * mclk_div is the I2S clock divider's integral value
   * b is the fraction clock divider's numerator value
   * a is the fraction clock divider's denominator value
   */

  if (priv->config->role == I2S_ROLE_MASTER)
    {
      bclk = priv->rate * priv->config->total_slot * priv->data_width;
      mclk = priv->mclk_freq;
      bclk_div = mclk / bclk;
    }
  else
    {
      /* For slave mode, mclk >= bclk * 8, so fix bclk_div to 2 first */

      bclk_div = 8;
      bclk = priv->rate * priv->config->total_slot * priv->data_width;
      mclk = bclk * bclk_div;
    }

  /* Calculate the nearest integer value of the I2S clock divider */

  mclk_div = sclk / mclk;

  i2sinfo("Clock division info: [sclk]%" PRIu32 " Hz [mdiv] %d "
          "[mclk] %" PRIu32 " Hz [bdiv] %d [bclk] %" PRIu32 " Hz\n",
          sclk, mclk_div, mclk, bclk_div, bclk);

  freq_diff = abs((int)sclk - (int)(mclk * mclk_div));

  denominator = 1;
  numerator = 0;

  if (freq_diff)
    {
      float decimal = freq_diff / (float)mclk;

      /* Carry bit if the decimal is greater than
       * 1.0 - 1.0 / (63.0 * 2) = 125.0 / 126.0
       */

      if (decimal > 125.0f / 126.0f)
        {
          mclk_div++;
        }
      else
        {
          uint32_t min = UINT32_MAX;

          for (int a = 2; a <= I2S_LL_MCLK_DIVIDER_MAX; a++)
            {
              int b = (int)(a * (freq_diff / (double)mclk) + 0.5);
              int ma = freq_diff * a;
              int mb = mclk * b;
              if (ma == mb)
                {
                  denominator = a;
                  numerator = b;
                  break;
                }

              if (abs((mb - ma)) < min)
                {
                  denominator = a;
                  numerator = b;
                  min = abs(mb - ma);
                }
            }
        }
    }

  i2sinfo("Clock register: [mclk] %" PRIu32 " Hz [numerator] %d "
          "[denominator] %d\n", mclk, numerator, denominator);

  /* Setup TX Clock */

  regval = getreg32(I2S_TX_CLKM_CONF_REG(priv->config->port));
  regval &= ~I2S_TX_CLKM_DIV_NUM_M;
  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_NUM, mclk_div);
  putreg32(regval, I2S_TX_CLKM_CONF_REG(priv->config->port));

  regval = getreg32(I2S_TX_CLKM_DIV_CONF_REG(priv->config->port));
  regval &= ~(I2S_TX_CLKM_DIV_Z_M | I2S_TX_CLKM_DIV_Y_M |
              I2S_TX_CLKM_DIV_X_M | I2S_TX_CLKM_DIV_YN1_M);
  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_Z,
                           numerator <= (denominator / 2) ?
                           numerator : (denominator - numerator));

  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_Y,
                           numerator <= (denominator / 2) ?
                           (denominator % numerator) :
                           (denominator % (denominator - numerator)));

  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_X,
                           numerator <= (denominator / 2) ?
                           (int)(floor(denominator / numerator) - 1) :
                           (int)(floor(denominator /
                                       (denominator - numerator))-1));

  regval |= FIELD_TO_VALUE(I2S_TX_CLKM_DIV_YN1,
                           numerator <= (denominator / 2) ?
                           0 : 1);
  putreg32(regval, I2S_TX_CLKM_DIV_CONF_REG(priv->config->port));

  /* Setup RX Clock */

  regval = getreg32(I2S_RX_CLKM_CONF_REG(priv->config->port));
  regval &= ~I2S_RX_CLKM_DIV_NUM_M;
  regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_NUM, mclk_div);
  putreg32(regval, I2S_RX_CLKM_CONF_REG(priv->config->port));

  regval = getreg32(I2S_RX_CLKM_DIV_CONF_REG(priv->config->port));
  regval &= ~(I2S_RX_CLKM_DIV_Z_M | I2S_RX_CLKM_DIV_Y_M |
              I2S_RX_CLKM_DIV_X_M | I2S_RX_CLKM_DIV_YN1_M);
  regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_Z,
                           numerator <= (denominator / 2) ?
                           numerator : (denominator - numerator));

  regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_Y,
                           numerator <= (denominator / 2) ?
                           (denominator % numerator) :
                           (denominator % (denominator - numerator)));

  regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_X,
                           numerator <= (denominator / 2) ?
                           (int)(floor(denominator / numerator) - 1) :
                           (int)(floor(denominator /
                                       (denominator - numerator))-1));

  regval |= FIELD_TO_VALUE(I2S_RX_CLKM_DIV_YN1,
                           numerator <= (denominator / 2) ?
                           0 : 1);
  putreg32(regval, I2S_RX_CLKM_DIV_CONF_REG(priv->config->port));

  /* Set I2S TX bck div num
   * Note: I2S_TX/RX_BCK_DIV_NUM cannot be 1
   */

#ifdef I2S_HAVE_TX
  modifyreg32(I2S_TX_CONF1_REG(priv->config->port),
              I2S_TX_BCK_DIV_NUM_M,
              FIELD_TO_VALUE(I2S_TX_BCK_DIV_NUM, bclk_div - 1));

  /* Set I2S_TX_UPDATE bit to update the configs.
   * This bit is automatically cleared.
   */

  modifyreg32(I2S_TX_CONF_REG(priv->config->port),
              0, I2S_TX_UPDATE);
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
  modifyreg32(I2S_RX_CONF1_REG(priv->config->port),
              I2S_RX_BCK_DIV_NUM_M,
              FIELD_TO_VALUE(I2S_RX_BCK_DIV_NUM, bclk_div - 1));

  /* Set I2S_RX_UPDATE bit to update the configs.
   * This bit is automatically cleared.
   */

  modifyreg32(I2S_RX_CONF_REG(priv->config->port),
              0, I2S_RX_UPDATE);
#endif /* I2S_HAVE_RX */

  /* Returns the actual sample rate */

  bclk = sclk / (float)((mclk_div + numerator / (float)denominator) *
                        bclk_div);
  rate = bclk / (float)(priv->config->total_slot * priv->data_width);

  return rate;
}

/****************************************************************************
 * Name: i2s_tx_channel_start
 *
 * Description:
 *   Start TX channel for the I2S port
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
/* [V337] 无论 tx_started 与否都先应用 GDMA strategy。
 *
 * 实测 `[I2SREG] #1 ch=1 txirq=77 tx_started=1` —— 本函数一进来 tx_started
 * 就已是 1，所以下面的早退会把它后面所有的 GDMA 配置（含 V251 的 strategy
 * 与 V336 追加的那次）全部跳过，DMA_OUT_CONF0 因此恒为复位值 0x00000008
 * （无 auto_wrback / 无 burst / 无 owner_check / PRI=0）。
 *
 * 而 V251 注释已写明：out_auto_wrback=0 时控制器发完不清 OWNER 位、
 * outlink FSM 无法前进 —— 正是 DSCR 恒 0、FIFO 不排空的成因。
 *
 * 因此把 strategy 提到最前面：先保证它一定生效，再走原有的早退逻辑。 */
/**
 * Name: i2s_tx_rate_tick
 *
 * Description:
 *   [V362] Emit one aggregate summary line every 5 seconds describing
 *   how often i2s_txdma_start() was entered and how many of those
 *   entries were the *idle* kind (nothing active, nothing queued).
 *
 *   A healthy stream shows starts >= 1 and bail_pend == 0.
 *   The failure this instrumentation was written to prove shows
 *   bail_pend in the thousands with starts == 0, and the s1/s2 split
 *   then names the caller that is spinning.
 *
 *   Cost control: clock_gettime() is only reached once every 64 calls;
 *   every other call returns after a counter increment, so a kHz-rate
 *   idle spin cannot turn this into a second CPU hog.
 */

/**
 * Name: i2s_tx_irq_mask / i2s_tx_irq_unmask
 *
 * Description:
 *   [V364] Gate the three GDMA TX completion interrupt sources
 *   (OUT_DONE, OUT_EOF, OUT_TOTAL_EOF) instead of letting them fire into
 *   an idle driver.
 *
 *   Rationale: V362 proved from the code structure that every idle entry
 *   into i2s_txdma_start() comes from i2s_tx_schedule(), which the GDMA
 *   interrupt handler calls as soon as the outlink raises an EOF.  When
 *   the last buffer has already been reported the queues are empty and
 *   the interrupt has nothing left to report, yet nothing stopped it
 *   from being raised again.  V362 made each entry cheap; this stops the
 *   entries.
 *
 *   OUT_DSCR_ERR is deliberately left enabled in both directions -- it is
 *   an error source, not a completion source, and we want to see it.
 *
 *   Both helpers drop the corresponding INT_ST/INT_RAW bits first.  The
 *   descriptor pool is recycled, so a stale RAW bit that survived the
 *   gate could otherwise fire the instant we re-enable and, because the
 *   address matches, be accepted by i2s_tx_schedule() as the completion
 *   of the buffer that was just queued.
 */

static void i2s_tx_irq_mask(struct esp32s3_i2s_s *priv)
{
  SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel,
                  DMA_OUT_TOTAL_EOF_CH0_INT_ST |
                  DMA_OUT_EOF_CH0_INT_ST |
                  DMA_OUT_DONE_CH0_INT_ST);

  CLR_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_OUT_TOTAL_EOF_CH0_INT_ENA |
                   DMA_OUT_EOF_CH0_INT_ENA |
                   DMA_OUT_DONE_CH0_INT_ENA);

  g_tx_irq_mask_n++;

  if (g_tx_irq_mask_n <= 3)
    {
      
    }
}

static void i2s_tx_irq_unmask(struct esp32s3_i2s_s *priv)
{
  SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel,
                  DMA_OUT_TOTAL_EOF_CH0_INT_ST |
                  DMA_OUT_EOF_CH0_INT_ST |
                  DMA_OUT_DONE_CH0_INT_ST);

  SET_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                   DMA_OUT_TOTAL_EOF_CH0_INT_ENA |
                   DMA_OUT_EOF_CH0_INT_ENA |
                   DMA_OUT_DONE_CH0_INT_ENA);

  g_tx_irq_unmask_n++;

  if (g_tx_irq_unmask_n <= 3)
    {
      
    }
}

static void i2s_tx_rate_tick(void)
{
  struct timespec ts;
  int64_t now_ms;

  if ((++g_tx_rate_skip & 0x3f) != 0)
    {
      return;
    }

  ts.tv_sec = 0;
  ts.tv_nsec = 0;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return;
    }

  now_ms = ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);

  if (g_tx_rate_next_ms == 0)
    {
      g_tx_rate_next_ms = now_ms + 5000;
      return;
    }

  if (now_ms < g_tx_rate_next_ms)
    {
      return;
    }

  g_tx_rate_next_ms = now_ms + 5000;

  
}

static void i2s_apply_tx_gdma_strategy(struct esp32s3_i2s_s *priv)
{
  SET_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                   DMA_OUT_DATA_BURST_EN_CH0_M |
                   DMA_OUTDSCR_BURST_EN_CH0_M |
                   DMA_OUT_AUTO_WRBACK_CH0_M);
  CLR_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                   DMA_OUT_EOF_MODE_CH0);
  SET_GDMA_CH_BITS(DMA_OUT_CONF1_CH0_REG, priv->dma_channel,
                   DMA_OUT_CHECK_OWNER_CH0_M);
  SET_GDMA_CH_REG(DMA_OUT_PRI_CH0_REG, priv->dma_channel, 1);

  
}

static void i2s_tx_channel_start(struct esp32s3_i2s_s *priv)
{
  if (priv->config->tx_en)
    {
      /* [V337] 早退之前先落 strategy（原来它在早退之后，永远执行不到） */

      i2s_apply_tx_gdma_strategy(priv);

      if (priv->tx_started)
        {
          /* [V347] Team-lead hypothesis probe.  If this ever fires *per
           * transfer* it would explain a missing I2S_TX_START; keep a counter
           * and a log so the trace says so unambiguously.  (Note: this
           * function is only entered from i2s_start()/i2s_stop()/ioctl and the
           * RAW one-shot path -- the DMA start path that actually writes
           * I2S_TX_START is i2s_txdma_start(), instrumented separately.) */

          g_tx_channel_start_er++;
          

          i2swarn("TX channel of port %d was previously started\n",
                  priv->config->port);
          return;
        }

      /* Reset the DMA operation */

      SET_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                       DMA_OUT_RST_CH0_M);
      CLR_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                       DMA_OUT_RST_CH0_M);
      CLR_GDMA_CH_BITS(DMA_OUT_CONF1_CH0_REG, priv->dma_channel,
                       DMA_OUT_CHECK_OWNER_CH0);

      /* [V251] Mirror ESP-IDF's AHB-GDMA TX channel configuration exactly.
       *
       * ESP-IDF builds every AHB channel through gdma_hal_ahb_v1.c, which
       * calls gdma_ll_tx_enable_data_burst(), gdma_ll_tx_enable_descriptor_burst(),
       * gdma_ll_tx_enable_owner_check() and gdma_ll_tx_set_priority() before
       * it ever sets the descriptor address.  NuttX leaves all four at their
       * reset values:
       *
       *   DMA_OUT_CONF0 (0x60) bit4 OUTDSCR_BURST_EN  default 0
       *   DMA_OUT_CONF0 (0x60) bit5 OUT_DATA_BURST_EN default 0
       *   DMA_OUT_CONF1 (0x64) bit12 OUT_CHECK_OWNER  default 0
       *   DMA_OUT_PRI   (0xa4) OUT_PRI                default 0
       *
       * Measured before this change: DMA_OUT_CONF0 = 0x00000008, i.e. only
       * out_eof_mode (bit3, a reset default) -- no burst of either kind and
       * the lowest possible priority.  With the outlink FSM confirmed running
       * (PARK=0) the channel still will not pull the first descriptor, which
       * is what an unburstable, zero-priority AHB master looks like.
       *
       * out_check_owner makes the GDMA verify descriptor ownership before use;
       * desc0.ctrl reads 0xc0fe0fe0, whose bit31 OWNER is 1, so the check
       * passes.
       *
       * [V252] *** OUT_AUTO_WRBACK and OUT_EOF_MODE -- the last two ***
       *
       * ESP-IDF's gdma_ahb_hal_set_strategy() writes THREE things on the TX
       * path:
       *
       *   gdma_ll_tx_enable_owner_check(dev, chan, en_owner_check);
       *   gdma_ll_tx_enable_auto_write_back(dev, chan, en_desc_write_back);
       *   gdma_ll_tx_set_eof_mode(dev, chan, eof_till_popped);
       *
       * and the I2S driver reaches it with GDMA_STRATEGY_FORWARD_DSCR and
       * no_auto_update = false, i.e. owner_check = TRUE, desc_write_back =
       * TRUE, eof_till_popped = FALSE.  NuttX, which drives the channel by
       * hand, sets none of the three.  out_auto_wrback (conf0 bit 2) and
       * out_eof_mode (conf0 bit 3) therefore sit at their reset values 0 and 1.
       *
       * out_auto_wrback is the one that matters: with it clear the controller
       * never writes the descriptor back after transmission.  It does not
       * clear the OWNER bit, does not store the transferred length, and the
       * outlink FSM consequently cannot advance past the first entry.  That is
       * exactly what the hardware reports:
       *
       *   FSM: DSCR=0x00000000 OUTFIFO=0x00408415 STATE=0x002f9db8
       *        CONF0=0x00000038 CONF1=0x00001000 PRI=0x00000001
       *
       * STATE holds the outlink descriptor address (low 18 bits 0x39db8 ==
       * 0x3fcb9db8) but DMA_OUT_DSCR, the descriptor the FSM is actually
       * working on, is still zero; the first descriptor's OWNER bit stays 1
       * across the whole write; no SUC_EOF is ever raised and txirqN stays 0.
       *
       * Set both to ESP-IDF's values. */

      SET_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                       DMA_OUT_DATA_BURST_EN_CH0_M |
                       DMA_OUTDSCR_BURST_EN_CH0_M |
                       DMA_OUT_AUTO_WRBACK_CH0_M);
      CLR_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                       DMA_OUT_EOF_MODE_CH0);
      SET_GDMA_CH_BITS(DMA_OUT_CONF1_CH0_REG, priv->dma_channel,
                       DMA_OUT_CHECK_OWNER_CH0_M);
      SET_GDMA_CH_REG(DMA_OUT_PRI_CH0_REG, priv->dma_channel, 1);

      /* Reset the TX channel */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_RESET);
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_RESET, 0);

      /* Reset TX FIFO */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_FIFO_RESET);
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_FIFO_RESET, 0);

      g_v671_cfg_valid = 0;   /* [V671] TX channel (re)started */

      /* Set I2S_RX_UPDATE bit to update the configs.
       * This bit is automatically cleared.
       */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_UPDATE);

      /* [V336] ★ 在 I2S 复位之后**重新应用** GDMA strategy。
       *
       * 实测 CONF0 一直是 0x00000008（只有 out_eof_mode），说明上面那次
       * V251 写入被紧随其后的 I2S_TX_RESET/FIFO_RESET 冲掉了 —— 而
       * out_auto_wrback 为 0 会让控制器发完不清 OWNER 位、outlink FSM
       * 无法前进（DSCR 恒为 0、FIFO 不排空、完成中断不来），
       * 这就是喇叭"TX DMA 从不排空"的根因。
       *
       * 顺序修正：复位完成后重新写一遍，确保它留得住。 */

      SET_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                       DMA_OUT_DATA_BURST_EN_CH0_M |
                       DMA_OUTDSCR_BURST_EN_CH0_M |
                       DMA_OUT_AUTO_WRBACK_CH0_M);
      CLR_GDMA_CH_BITS(DMA_OUT_CONF0_CH0_REG, priv->dma_channel,
                       DMA_OUT_EOF_MODE_CH0);
      SET_GDMA_CH_BITS(DMA_OUT_CONF1_CH0_REG, priv->dma_channel,
                       DMA_OUT_CHECK_OWNER_CH0_M);
      SET_GDMA_CH_REG(DMA_OUT_PRI_CH0_REG, priv->dma_channel, 1);

      


      /* [V221] Wait for tx_update to self-clear, exactly as ESP-IDF's
       * i2s_ll_tx_start() does.  tx_update hands the APB-domain configuration
       * to the I2S TX clock domain and clears itself on completion.  If it
       * never clears the configuration never reaches the clock domain, so
       * tx_start has no effect there, the I2S never raises its DMA request and
       * the GDMA outlink parks without fetching a single descriptor.  The
       * measurement was inconsistent across boots (0 spins vs a full timeout),
       * which points at the source clock rather than the configuration. */
      {
        int spins = 0;

        while (spins < 100000 &&
               (getreg32(I2S_TX_CONF_REG(priv->config->port)) & I2S_TX_UPDATE))
          {
            spins++;
          }

        g_tx_update_spins = spins;
      }


      /* [V217] Keep BCK/WS running when the TX FIFO is empty.
       *
       * tx_stop_en (tx_conf bit 13) gates the bit clock off as soon as the TX
       * FIFO drains.  With the FIFO empty that also stops the I2S from
       * asserting its DMA request, so the GDMA outlink sits parked with the
       * very first descriptor untouched -- which is precisely what we
       * measured (START written then cleared, PARK toggling, OWNER never
       * cleared, INT_ST and the interrupt count permanently zero).  Clearing
       * it lets the clock run continuously so the FIFO can drain and the DMA
       * request can be raised. */
#ifdef I2S_TX_STOP_EN
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_STOP_EN, 0);
#endif
      modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_UPDATE);



      /* Enable DMA interrupt */

      up_enable_irq(priv->tx_irq);

      /* [V342] Enable every TX completion event, not just OUT_TOTAL_EOF.
       * OUT_EOF (SUC_EOF) is the event that actually populates
       * DMA_OUT_EOF_DES_ADDR and is what the RX side of this driver uses;
       * OUT_DONE covers the per-descriptor case.  Leaving them disabled meant
       * the only enabled events were OUT_TOTAL_EOF and OUT_DSCR_ERR, so a
       * transfer that ended in SUC_EOF never produced the completion
       * callback -- exactly the "[audio_direct] I2S TX completion TIMEOUT". */

      SET_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                       DMA_OUT_TOTAL_EOF_CH0_INT_ENA |
                       DMA_OUT_EOF_CH0_INT_ENA |
                       DMA_OUT_DONE_CH0_INT_ENA |
                       DMA_OUT_DSCR_ERR_CH0_INT_ENA);

      /* REVISIT: enable I2S interrupt and set the FIFO hung counter togit
       * enable the `I2S_TX_HUNG_INT` and check for data timeout.
       */

      /* [V346] A fresh channel activation must re-run the one-time
       * configure/reset/start sequence on its first transfer. */

      g_tx_cfg_done = 0;

      priv->tx_started = true;

      i2sinfo("Started TX channel of port %d\n", priv->config->port);
    }
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rx_channel_start
 *
 * Description:
 *   Start RX channel for the I2S port
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static void i2s_rx_channel_start(struct esp32s3_i2s_s *priv)
{
  if (priv->config->rx_en)
    {
      if (priv->rx_started)
        {
          i2swarn("RX channel of port %d was previously started\n",
                  priv->config->port);
          return;
        }

      /* Reset the DMA operation */

      SET_GDMA_CH_BITS(DMA_IN_CONF0_CH0_REG, priv->dma_channel,
                       DMA_IN_RST_CH0_M);
      CLR_GDMA_CH_BITS(DMA_IN_CONF0_CH0_REG, priv->dma_channel,
                       DMA_IN_RST_CH0_M);

      /* [RX-FIX-2] Program the GDMA IN channel the way the TX side already
       * is (see the V251/V252 strategy sequence in i2s_tx_channel_start).
       * NuttX left it at its reset value: no burst of either kind, no owner
       * check, priority 0.  The symptom here is the same one that sequence
       * fixed for TX -- the RX DMA reports "armed" ([AUDIO-R42] xfer ret=0)
       * yet never raises SUC_EOF, so the native capture path starves and
       * audio_capture_native gives up with -EIO after six empty waits. */
      SET_GDMA_CH_BITS(DMA_IN_CONF0_CH0_REG, priv->dma_channel,
                       DMA_IN_DATA_BURST_EN_CH0_M |
                       DMA_INDSCR_BURST_EN_CH0_M);
      SET_GDMA_CH_BITS(DMA_IN_CONF1_CH0_REG, priv->dma_channel,
                       DMA_IN_CHECK_OWNER_CH0_M);
      SET_GDMA_CH_REG(DMA_IN_PRI_CH0_REG, priv->dma_channel, 1);

      /* Reset the RX channel */

      modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_RESET);
      modifyreg32(I2S_RX_CONF_REG(priv->config->port), I2S_RX_RESET, 0);

      /* [V390] A/B: choose where the receiver takes its bit clock from.
       * i2s_configure() forces I2S_RX_SLAVE_MOD whenever the transmitter
       * owns the port, which is the case here, so the RX module runs off
       * I2S0I_BCK_IN / I2S0I_WS_IN.  Flip it to master to test whether
       * that input path is the reason the receiver never asks the GDMA
       * for anything.  The pads keep the TX generator's clock either way,
       * so the codec stays clocked in both arms of the test.
       */

      if (g_v390_rxmaster)
        {
          modifyreg32(I2S_RX_CONF_REG(priv->config->port),
                      I2S_RX_SLAVE_MOD, 0);
        }
      else
        {
          modifyreg32(I2S_RX_CONF_REG(priv->config->port),
                      0, I2S_RX_SLAVE_MOD);
        }

      /* Reset RX FIFO */

      modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_FIFO_RESET);
      modifyreg32(I2S_RX_CONF_REG(priv->config->port), I2S_RX_FIFO_RESET, 0);

      /* Set I2S_RX_UPDATE bit to update the configs.
       * This bit is automatically cleared.
       */

      modifyreg32(I2S_RX_CONF_REG(priv->config->port), 0, I2S_RX_UPDATE);

      /* [RX-FIX-1] Wait for rx_update to self-clear, exactly as the TX path
       * (V221) and ESP-IDF's i2s_ll_rx_start() do.  rx_update hands the
       * APB-domain RX configuration to the I2S RX clock domain and clears
       * itself on completion.  If it never clears, the configuration never
       * reaches the clock domain, the RX never starts, the I2S never raises
       * its DMA request and the capture path starves -- exactly what
       * audio_capture_native reports as "capture EIO" after 6 s of empty
       * waits.  The RX path previously had no such handshake at all. */
      {
        int spins = 0;

        while (spins < I2S_UPDATE_SPIN_MAX &&
               (getreg32(I2S_RX_CONF_REG(priv->config->port)) & I2S_RX_UPDATE))
          {
            spins++;
          }

        g_rx_update_spins = spins;
        /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
               "[V390] rx role=%s conf=0x%08x\n",
               g_v390_rxmaster ? "MASTER" : "SLAVE",
               (unsigned)getreg32(I2S_RX_CONF_REG(priv->config->port)));
        /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO,
               "[RXCLK] rx_update spins=%d conf=0x%08x rate=%u\n",
               spins,
               (unsigned)getreg32(I2S_RX_CONF_REG(priv->config->port)),
               (unsigned)priv->rate);
      }

      /* Enable DMA interrupt */

      up_enable_irq(priv->rx_irq);

      SET_GDMA_CH_BITS(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel,
                       DMA_IN_SUC_EOF_CH0_INT_ENA);

      /* REVISIT: enable I2S interrupt and set the FIFO hung counter to
       * enable the `I2S_RX_HUNG_INT` and check for data timeout.
       */

      /* [V382] Run the TX clock generator while a capture streams.
       *
       * The receiver on this port is a slave to the transmitter's
       * BCK/WS, and the transmitter only shifts while it has something
       * to shift.  With TX stopped there is no frame clock on the pins,
       * the ES8311 never puts a bit on DIN and the RX GDMA waits for
       * ever -- logged as [RXDUMP] ... isr=0 eof=0 and reported by
       * audio_capture_native as "capture EIO".
       *
       * Feeding the transmitter from a second es8311 instance
       * (the V373/V378/V381 clock keeper) does produce a clock but
       * panics the board the moment its TX DMA starts next to a running
       * capture.  So start the generator on its own instead: with the
       * TX DMA idle the transmitter underruns and shifts zeros, which
       * is inaudible but keeps BCLK and WS alive.  One instance, no
       * second thread, no DMA restart, nothing to deadlock.
       */

      if (!(getreg32(I2S_TX_CONF_REG(priv->config->port)) & I2S_TX_START))
        {
          int spins;

          modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0,
                      I2S_TX_START);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0,
                      I2S_TX_UPDATE);

          for (spins = 0;
               spins < I2S_UPDATE_SPIN_MAX &&
               (getreg32(I2S_TX_CONF_REG(priv->config->port)) &
                I2S_TX_UPDATE);
               spins++)
            {
            }

          /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO, "[V382] tx clock-only start spins=%d "
                 "txconf=0x%08x\n",
                 spins,
                 (unsigned)getreg32(I2S_TX_CONF_REG(priv->config->port)));
        }

      /* [V383] Are the clocks and the codec's data actually on the pins?
       *
       * The RX register dump keeps showing i2sintst=0 / gdmainst=0, i.e.
       * not a single frame reached the receiver.  That is equally
       * consistent with "no BCLK" and with "BCLK is fine but the ES8311
       * is silent".  The pins are driven through the GPIO matrix, so
       * enabling only the pad input buffer makes the level readable
       * (same trick the [V244] block in esp32s3_i2s_tx_write_raw uses).
       *
       *   BCLK(10) hi ~= 2000/4000  -> the frame clock is running
       *   DIN(9)   hi ~= 2000/4000  -> the codec is shifting data at us
       *   either extreme (0 or 4000) -> that line is stuck
       */

      {
        static int s_v383_n;

        if (s_v383_n < 8)
          {
            unsigned int bhi = 0;
            unsigned int whi = 0;
            unsigned int dhi = 0;
            unsigned int k;

            s_v383_n++;

            modifyreg32(IO_MUX_GPIO8_REG,  0, (1u << 9));   /* WS   */
            modifyreg32(IO_MUX_GPIO9_REG,  0, (1u << 9));   /* DIN  */
            modifyreg32(IO_MUX_GPIO10_REG, 0, (1u << 9));   /* BCLK */

            for (k = 0; k < 4000; k++)
              {
                if (esp32s3_gpioread(10))
                  {
                    bhi++;
                  }

                if (esp32s3_gpioread(8))
                  {
                    whi++;
                  }

                if (esp32s3_gpioread(9))
                  {
                    dhi++;
                  }
              }

            /* [V394] IRQ-stack: silenced */ if (0) syslog(LOG_INFO, "[V383] pins BCLK(10) hi=%u WS(8) hi=%u "
                   "DIN(9) hi=%u /4000\n", bhi, whi, dhi);
          }
      }

      priv->rx_started = true;

      i2sinfo("Started RX channel of port %d\n", priv->config->port);
    }
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_tx_channel_stop
 *
 * Description:
 *   Stop TX channel for the I2S port
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static void i2s_tx_channel_stop(struct esp32s3_i2s_s *priv)
{
  if (priv->config->tx_en)
    {
      if (!priv->tx_started)
        {
          i2swarn("TX channel of port %d was previously stopped\n",
                  priv->config->port);
          return;
        }

      /* Stop TX channel */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port), I2S_TX_START, 0);

      g_v671_cfg_valid = 0;   /* [V671] transmitter stopped */

      /* Stop outlink */

      SET_GDMA_CH_BITS(DMA_OUT_LINK_CH0_REG, priv->dma_channel,
                       DMA_OUTLINK_STOP_CH0_M);

      /* Disable DMA interrupt */

      /* [V342] Disable the same set that i2s_tx_channel_start() enables. */

      CLR_GDMA_CH_BITS(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel,
                       DMA_OUT_TOTAL_EOF_CH0_INT_ENA |
                       DMA_OUT_EOF_CH0_INT_ENA |
                       DMA_OUT_DONE_CH0_INT_ENA |
                       DMA_OUT_DSCR_ERR_CH0_INT_ENA);

      /* Disable DMA operation mode */

      up_disable_irq(priv->tx_irq);

      priv->tx_started = false;
      g_tx_cfg_done    = 0;   /* [V346] force reconfig on next activation */

      
    }
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rx_channel_stop
 *
 * Description:
 *   Stop RX channel for the I2S port
 *
 * Input Parameters:
 *   priv - Initialized I2S device structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static void i2s_rx_channel_stop(struct esp32s3_i2s_s *priv)
{
  if (priv->config->rx_en)
    {
      if (!priv->rx_started)
        {
          i2swarn("RX channel of port %d was previously stopped\n",
                  priv->config->port);
          return;
        }

      /* Stop RX channel */

      modifyreg32(I2S_RX_CONF_REG(priv->config->port), I2S_RX_START, 0);

      /* Stop outlink */

      SET_GDMA_CH_BITS(DMA_IN_LINK_CH0_REG, priv->dma_channel,
                       DMA_INLINK_STOP_CH0_M);

      /* Disable DMA interrupt */

      CLR_GDMA_CH_BITS(DMA_IN_INT_ENA_CH0_REG, priv->dma_channel,
                       DMA_IN_SUC_EOF_CH0_INT_ENA);

      /* Disable DMA operation mode */

      up_disable_irq(priv->rx_irq);

      priv->rx_started = false;

      i2sinfo("Stopped RX channel of port %d\n", priv->config->port);
    }
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_tx_interrupt
 *
 * Description:
 *   Common I2S DMA interrupt handler
 *
 * Input Parameters:
 *   irq     - Number of the IRQ that generated the interrupt
 *   context - Interrupt register state save info
 *   arg     - I2S controller private data
 *
 * Returned Value:
 *   Standard interrupt return value.
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
/* [V209] The TX interrupt counter lives with the other diagnostics at the top
 * of this file, because i2s_txdma_start() (below) also needs g_txdump_n. */

static int IRAM_ATTR i2s_tx_interrupt(int irq, void *context, void *arg)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)arg;
  struct esp32s3_dmadesc_s *cur = NULL;
  uint32_t status;

  /* [V342] Latch the RAW status word BEFORE clearing it.  The previous code
   * cleared the status first and then looked at the same register, so by the
   * time any diagnostic ran the cause was gone.  This is the authoritative
   * record of which GDMA event actually woke the CPU. */

  status = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG, priv->dma_channel);

  g_txirq_n++;

  if (status != 0)
    {
      if (g_tx_isr_status == 0)
        {
          g_tx_isr_status = status;
        }

      if (status & DMA_OUT_DONE_CH0_INT_ST)
        {
          g_tx_isr_n[0]++;
        }

      if (status & DMA_OUT_EOF_CH0_INT_ST)
        {
          g_tx_isr_n[1]++;
        }

      if (status & DMA_OUT_DSCR_ERR_CH0_INT_ST)
        {
          g_tx_isr_n[2]++;
          g_tx_isr_err++;
        }

      if (status & DMA_OUT_TOTAL_EOF_CH0_INT_ST)
        {
          g_tx_isr_n[3]++;
        }
    }

  SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel, status);

  if (priv->config->tx_en)
    {
      /* [V342] Complete on OUT_DONE, OUT_EOF (SUC_EOF) *or* OUT_TOTAL_EOF.
       *
       * Which of these the controller raises depends on out_eof_mode and on
       * how the outlink terminates, so acting on OUT_TOTAL_EOF alone made the
       * completion callback order-dependent: the RX path in this very driver
       * already completes on SUC_EOF, and here DMA_OUT_EOF_DES_ADDR is the
       * register the hardware fills on the SUC_EOF event.  i2s_tx_schedule()
       * itself de-duplicates: it only reports a buffer whose last descriptor
       * address matches the one it is handed, so a spurious extra call is a
       * harmless no-op. */

      if (status & (DMA_OUT_TOTAL_EOF_CH0_INT_ST |
                    DMA_OUT_EOF_CH0_INT_ST |
                    DMA_OUT_DONE_CH0_INT_ST))
        {
          cur = (struct esp32s3_dmadesc_s *)
                GET_GDMA_CH_REG(DMA_OUT_EOF_DES_ADDR_CH0_REG,
                                priv->dma_channel);

          /* Schedule completion of the transfer on the worker thread */

          i2s_tx_schedule(priv, cur);
        }
    }

  return 0;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rx_interrupt
 *
 * Description:
 *   Common I2S DMA interrupt handler
 *
 * Input Parameters:
 *   irq     - Number of the IRQ that generated the interrupt
 *   context - Interrupt register state save info
 *   arg     - I2S controller private data
 *
 * Returned Value:
 *   Standard interrupt return value.
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static int i2s_rx_interrupt(int irq, void *context, void *arg)
{
  g_v94_isr++;
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)arg;
  struct esp32s3_dmadesc_s *cur = NULL;

  uint32_t status = GET_GDMA_CH_REG(DMA_IN_INT_ST_CH0_REG,
                                    priv->dma_channel);

  SET_GDMA_CH_REG(DMA_IN_INT_CLR_CH0_REG, priv->dma_channel,
                  UINT32_MAX);

  if (priv->config->rx_en)
    {
      if (status & DMA_IN_SUC_EOF_CH0_INT_ST)
        {
          /* [RX-DIAG] SUC_EOF seen -- proof RX data is flowing */
          {
            static int first_eof = 1;
            if (first_eof) {
              first_eof = 0;
              syslog(LOG_INFO,
                     "[RXEOF] first SUC_EOF! isr=%u status=0x%08x\n",
                     g_v94_isr, (unsigned)status);
            }
          }
          g_v94_eof++;
          cur = (struct esp32s3_dmadesc_s *)
                GET_GDMA_CH_REG(DMA_IN_SUC_EOF_DES_ADDR_CH0_REG,
                                priv->dma_channel);

          /* Schedule completion of the transfer on the worker thread */

          i2s_rx_schedule(priv, cur);
        }
    }

  return 0;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_getmclkfrequency
 *
 * Description:
 *   Get the current master clock frequency.
 *
 * Input Parameters:
 *   dev        - Device-specific state data
 *
 * Returned Value:
 *   Returns the current master clock.
 *
 ****************************************************************************/

static uint32_t i2s_getmclkfrequency(struct i2s_dev_s *dev)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  return priv->mclk_freq;
}

/****************************************************************************
 * Name: i2s_setmclkfrequency
 *
 * Description:
 *   Set the master clock frequency. Usually, the MCLK is a multiple of the
 *   sample rate. Most of the audio codecs require setting specific MCLK
 *   frequency according to the sample rate.
 *
 * Input Parameters:
 *   dev        - Device-specific state data
 *   frequency  - The I2S master clock's frequency
 *
 * Returned Value:
 *   Returns the resulting master clock or a negated errno value on failure.
 *
 ****************************************************************************/

static uint32_t i2s_setmclkfrequency(struct i2s_dev_s *dev,
                                     uint32_t frequency)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  /* Check if the master clock frequency is beyond the highest possible
   * value and return an error.
   */

  if (frequency >= (g_i2s_sclk / 2))
    {
      return -EINVAL;
    }

  priv->mclk_freq = frequency;

  return frequency;
}

/****************************************************************************
 * Name: i2s_txchannels
 *
 * Description:
 *   Set the I2S TX number of channels.
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   channels - The I2S numbers of channels
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static int i2s_txchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;
  uint32_t channels_mask;

  if (priv->config->tx_en)
    {
      if (channels != 1 && channels != 2)
        {
          return -EINVAL;
        }

      i2s_tx_channel_stop(priv);

      priv->channels = channels;

      /* [V668] Remember it.  priv->channels itself is shared with the RX half
       * (i2s_rxchannels() writes the same field), so it cannot be used later
       * as the authority on how the transmitter should be configured. */
      g_i2s_tx_mono = (channels == 1) ? 1 : 0;

      /* Always consider two channels. For mono (1-channel), we set the
       * I2S_TX_TDM_CHAN1_EN to 0 and I2S_TX_CHAN_EQUAL to 1 to send out
       * the data of the previous channel.
       */

      /* I2S_TX_TDM_TOT_CHAN_NUM = channels - 1 */

      modifyreg32(I2S_TX_TDM_CTRL_REG(priv->config->port),
                  I2S_TX_TDM_TOT_CHAN_NUM_M,
                  FIELD_TO_VALUE(I2S_TX_TDM_TOT_CHAN_NUM, 1));

      channels_mask = getreg32(I2S_TX_TDM_CTRL_REG(priv->config->port));
      channels_mask &= 0xffff0000;

      if (priv->channels == 1)
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_MONO_M, I2S_TX_MONO);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_CHAN_EQUAL_M, I2S_TX_CHAN_EQUAL);
          channels_mask |= I2S_TX_TDM_CHAN0_EN;
        }
      else
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_MONO_M, 0);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_CHAN_EQUAL_M, 0);
          channels_mask |= I2S_TX_TDM_CHAN0_EN | I2S_TX_TDM_CHAN1_EN;
        }

      putreg32(channels_mask, I2S_TX_TDM_CTRL_REG(priv->config->port));

      /* Set I2S_TX_UPDATE bit to update the configs.
       * This bit is automatically cleared.
       */

      modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                  0, I2S_TX_UPDATE);

      i2s_tx_channel_start(priv);

      return OK;
    }

  return -ENOTTY;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rxchannels
 *
 * Description:
 *   Set the I2S RX number of channels.
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   channels - The I2S numbers of channels
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static int i2s_rxchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->rx_en)
    {
      if (channels != 1 && channels != 2)
        {
          return -EINVAL;
        }

      priv->channels = channels;
      /* [V680] Remember the capture width for i2s_configure()'s RX TDM
       * programming -- see the comment at the slot-mask write. */
      g_i2s_rx_mono = (channels == 1) ? 1 : 0;
      return OK;
    }

  return -ENOTTY;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_txsamplerate
 *
 * Description:
 *   Set the I2S TX sample rate.  NOTE:  This will have no effect if (1) the
 *   driver does not support an I2S transmitter or if (2) the sample rate is
 *   driven by the I2S frame clock.  This may also have unexpected side-
 *   effects of the TX sample is coupled with the RX sample rate.
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   rate - The I2S sample rate in samples (not bits) per second
 *
 * Returned Value:
 *   Returns the resulting bitrate
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static uint32_t i2s_txsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->tx_en)
    {
      i2s_tx_channel_stop(priv);

      priv->rate = rate;

      if (i2s_check_mclkfrequency(priv) < OK)
        {
          return 0;
        }

      /* [V341] The source clock and every divider are handled centrally now.
       *
       * This used to re-write I2S_TX_CLK_SEL here (the V221 / V249 blocks),
       * duplicating -- and often contradicting -- the choice i2s_configure()
       * makes.  i2s_clock_source_bringup() selects the source (PLL_F160M
       * first, XTAL as a guaranteed-running fallback), programs all dividers
       * for it via i2s_set_clock(), and verifies the tx_update handshake so a
       * stalled clock domain can never again be selected silently.
       * g_i2s_sclk and g_i2s_clk_sel are updated by that function. */

      rate = i2s_clock_source_bringup(priv);

      i2s_tx_channel_start(priv);

      /* [V664] *** Restore the channel mode that the reset just dropped. ***
       *
       * i2s_clock_source_bringup() -> i2s_clock_try_source() pulses
       * I2S_TX_RESET, and that clears I2S_TX_MONO / I2S_TX_CHAN_EQUAL out
       * of I2S_TX_CONF as well.  The comment inside i2s_clock_try_source()
       * claims tx_reset leaves the configuration registers alone -- it does
       * not.  Measured [V663] with TX already running:
       *
       *     I2S_TXCHANNELS(1) then I2S_TXSAMPLERATE(16000)
       *     TX_CONF = 0x00081204   -> bit5 (MONO) = 0, bit6 (CHAN_EQUAL) = 0
       *     TX_TDM_CTRL = 0x00010001 -> TOT_CHAN_NUM = 1, CHAN0_EN = 1
       *
       * The TDM half proves i2s_txchannels() did run, so the two CONF bits
       * were written and then wiped here.
       *
       * Consequences of running stereo with mono PCM: the hardware takes
       * TWO 16-bit samples out of the FIFO per LRCK frame (one per slot),
       * so 30720 samples are consumed in 15360 frames instead of 30720 and
       * the clip plays at exactly 2x -- pitch up an octave, speech at double
       * speed, which is the field report.  (30720/2/16000 = 0.960 s,
       * matching the 1.005 s v663 measured for the whole 61440 B.)
       *
       * i2s_txchannels() has already stored priv->channels, so put the two
       * bits back here -- after every reset, before anything can be queued.
       * channels == 0 means nobody ever asked, so leave the default alone. */

      if (priv->channels == 1)
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_MONO_M, I2S_TX_MONO);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_CHAN_EQUAL_M, I2S_TX_CHAN_EQUAL);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_UPDATE);

        }
      else if (priv->channels == 2)
        {
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_MONO_M, 0);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port),
                      I2S_TX_CHAN_EQUAL_M, 0);
          modifyreg32(I2S_TX_CONF_REG(priv->config->port), 0, I2S_TX_UPDATE);
        }

      /* [V662] *** Playback-rate forensics: what is LRCK really? ***
       *
       * The field report is "TTS speaks much too fast, unintelligible".
       * Every candidate cause collapses onto one quantity: the real LRCK
       * frequency of I2S0 TX.  What i2s_txsamplerate() returns is only
       * the nominal value i2s_set_clock() recomputes from its own
       * assumptions (sclk, total_slot, data_width, bclk_div) -- if any of
       * those disagrees with the hardware, it still cheerfully reports
       * 16000.  So print three mutually independent groups of facts:
       *
       *   1) driver state        rate / mclk_freq / data_width / channels /
       *                          total_slot / role / g_i2s_sclk / clk_sel
       *   2) TX hardware regs    conf (BITS_MOD) / conf1 (BCK_DIV_NUM) /
       *                          clkm_conf / clkm_div_conf / tdm_ctrl
       *   3) measured pin edges  WS(GPIO8) and BCLK(GPIO10), CCOUNT timebase
       *
       * The TX clock keeps running with no data queued, so this can be
       * sampled right here, before the first ap_buffer is ever sent.
       *
       * Interpretation: 16000 -> I2S is innocent, look at the data path.
       * ~32000 -> one level forgot a factor of two (bits_mod / bck_div).
       * ~24000 -> sclk is really the 240 MHz PLL, not 160 MHz.
       */
      return rate;
    }

  return 0;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rxsamplerate
 *
 * Description:
 *   Set the I2S RX sample rate.
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   rate - The I2S sample rate in samples (not bits) per second
 *
 * Returned Value:
 *   Returns the resulting bitrate
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static uint32_t i2s_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->rx_en)
    {
      i2s_rx_channel_stop(priv);

      priv->rate = rate;

      if (i2s_check_mclkfrequency(priv) < OK)
        {
          return 0;
        }

      rate = i2s_set_clock(priv);

      i2s_rx_channel_start(priv);

      return rate;
    }

  return 0;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_txdatawidth
 *
 * Description:
 *   Set the I2S TX data width.  The TX bitrate is determined by
 *   sample_rate * data_width.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *   width - The I2S data with in bits.
 *
 * Returned Value:
 *   Returns the resulting data width
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static uint32_t i2s_txdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->tx_en)
    {
      i2s_tx_channel_stop(priv);

      priv->data_width = bits;

      i2s_set_datawidth(priv);

      i2s_tx_channel_start(priv);

      return bits;
    }

  return 0;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_rxdatawidth
 *
 * Description:
 *   Set the I2S RX data width.  The RX bitrate is determined by
 *   sample_rate * data_width.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *   width - The I2S data with in bits.
 *
 * Returned Value:
 *   Returns the resulting data width
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static uint32_t i2s_rxdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->rx_en)
    {
      i2s_rx_channel_stop(priv);

      priv->data_width = bits;

      i2s_set_datawidth(priv);

      i2s_rx_channel_start(priv);

      return bits;
    }

  return 0;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_send
 *
 * Description:
 *   Send a block of data on I2S.
 *
 * Input Parameters:
 *   dev      - Device-specific state data
 *   apb      - A pointer to the audio buffer from which to send data
 *   callback - A user provided callback function that will be called at
 *              the completion of the transfer.
 *   arg      - An opaque argument that will be provided to the callback
 *              when the transfer complete
 *   timeout  - The timeout value to use.  The transfer will be cancelled
 *              and an ETIMEDOUT error will be reported if this timeout
 *              elapsed without completion of the DMA transfer.  Units
 *              are system clock ticks.  Zero means no timeout.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef I2S_HAVE_TX
static int i2s_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                    i2s_callback_t callback, void *arg, uint32_t timeout)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->tx_en)
    {
      struct esp32s3_buffer_s *bfcontainer;
      int ret = OK;
      uint32_t nbytes;
      uint32_t nsamp;

      /* Check audio buffer data size from the upper half. If the buffer
       * size is not a multiple of the data width, the remaining bytes
       * must be sent along with the next audio buffer.
       */

      nbytes = (apb->nbytes - apb->curbyte) + priv->tx.carry.bytes;

      nbytes -= (nbytes % (priv->data_width / 8));

      if (nbytes > (ESP32S3_DMA_BUFLEN_MAX * I2S_DMADESC_NUM))
        {
          i2serr("Required buffer size can't fit into DMA outlink "
                 "(exceeds in %" PRIu32 " bytes). Try to increase the "
                 "number of the DMA descriptors (CONFIG_I2S_DMADESC_NUM).",
                 nbytes - (ESP32S3_DMA_BUFLEN_MAX * I2S_DMADESC_NUM));
          return -EFBIG;
        }

      /* Allocate a buffer container in advance */

      bfcontainer = i2s_buf_allocate(priv);
      if (bfcontainer == NULL)
        {
          i2serr("Failed to allocate the buffer container");
          return -ENOMEM;
        }

      /* Get exclusive access to the I2S driver data */

      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          goto errout_with_buf;
        }

      /* Add a reference to the audio buffer */

      apb_reference(apb);

      /* Initialize the buffer container structure */

      bfcontainer->callback = callback;
      bfcontainer->timeout  = timeout;
      bfcontainer->arg      = arg;
      bfcontainer->apb      = apb;
      bfcontainer->nbytes   = nbytes;
      bfcontainer->result   = -EBUSY;

      ret = i2s_txdma_setup(priv, bfcontainer);

      if (ret != OK)
        {
          goto errout_with_buf;
        }

      i2sinfo("Queued %d bytes into DMA buffers\n", apb->nbytes);
      i2s_dump_buffer("Audio pipeline buffer:", &apb->samp[apb->curbyte],
                      apb->nbytes - apb->curbyte);

      nxmutex_unlock(&priv->lock);

      /* [V209] One-shot TX register dump.  A dead I2S TX clock and a dead
       * GDMA outlink both present as "the completion callback never fires",
       * which is indistinguishable from twenty other faults; these four
       * registers separate them in a single boot. */
      if (I2S_I2SREG_DUMP && g_txdump_n < 3)
        {
          uint32_t conf  = getreg32(I2S_TX_CONF_REG(priv->config->port));
          uint32_t clkm  = getreg32(I2S_TX_CLKM_CONF_REG(priv->config->port));
          uint32_t conf1 = getreg32(I2S_TX_CONF1_REG(priv->config->port));
          uint32_t lnk   = GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG,
                                           priv->dma_channel);
          uint32_t c0    = GET_GDMA_CH_REG(DMA_OUT_CONF0_CH0_REG,
                                           priv->dma_channel);

          g_txdump_n++;
          syslog(LOG_INFO,
                 "[I2SREG] #%d ch=%d txirq=%d tx_started=%d txirqN=%d\n",
                 g_txdump_n, priv->dma_channel, priv->tx_irq,
                 (int)priv->tx_started, g_txirq_n);
          syslog(LOG_INFO,
                 "[I2SREG] TXCONF=0x%08x START=%d RESET=%d FIFORST=%d "
                 "SIGLOOP=%d\n", (unsigned)conf,
                 !!(conf & I2S_TX_START), !!(conf & I2S_TX_RESET),
                 !!(conf & I2S_TX_FIFO_RESET), !!(conf & I2S_SIG_LOOPBACK));
          syslog(LOG_INFO,
                 "[I2SREG] CLKM=0x%08x CLK_EN=%d CLK_SEL=%u CLK_ACTIVE=%d\n",
                 (unsigned)clkm, !!(clkm & I2S_CLK_EN),
                 (unsigned)((clkm >> I2S_TX_CLK_SEL_S) & 0x3),
                 !!(clkm & I2S_TX_CLK_ACTIVE));
          uint32_t ist  = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG,
                                          priv->dma_channel);
          uint32_t ien  = GET_GDMA_CH_REG(DMA_OUT_INT_ENA_CH0_REG,
                                          priv->dma_channel);
          uint32_t eofd = GET_GDMA_CH_REG(DMA_OUT_EOF_DES_ADDR_CH0_REG,
                                          priv->dma_channel);
          uint32_t d0   = *(volatile uint32_t *)bfcontainer->dma_link;

          syslog(LOG_INFO,
                 "[I2SREG] TXCONF1=0x%08x DMA_OUTLINK=0x%08x "
                 "DMA_OUT_CONF0=0x%08x\n", (unsigned)conf1,
                 (unsigned)lnk, (unsigned)c0);
          syslog(LOG_INFO,
                 "[I2SREG] INT_ST=0x%08x INT_ENA=0x%08x EOF_DES=0x%08x "
                 "LINKSTART=%d\n", (unsigned)ist, (unsigned)ien,
                 (unsigned)eofd, !!(lnk & DMA_OUTLINK_START_CH0_M));

          /* [V342] Authoritative record of which GDMA TX event actually
           * fired (latched by the ISR before it clears the status), plus the
           * per-event counters.  g_tx_isr_status == 0 with txirqN > 0 would
           * mean the interrupt came from somewhere other than OUT_INT_ST. */

          

          /* [V345] Completion-path accounting: how many ISR schedules ran,
           * how many found an empty tx.act, how many matched the descriptor,
           * and how many actually reached the user callback. */

          
          syslog(LOG_INFO,
                 "[I2SREG] desc0.ctrl=0x%08x OWNER(bit31)=%d EOF(bit30)=%d "
                 "len=%u dma_link=%p buf=%p\n", (unsigned)d0,
                 !!(d0 & 0x80000000u), !!(d0 & 0x40000000u),
                 (unsigned)(d0 & 0xfff),
                 (void *)bfcontainer->dma_link, (void *)bfcontainer->buf);
          {
            /* [V214] The three remaining suspects: is the GDMA actually bound
             * to I2S0, has the I2S TX FIFO reported a hang (i.e. the FIFO
             * filled and BCLK never advanced), and was OUTLINK_START ever
             * really set? */
            uint32_t psel = GET_GDMA_CH_REG(DMA_OUT_PERI_SEL_CH0_REG,
                                            priv->dma_channel);
            uint32_t ists = getreg32(I2S_INT_ST_REG(priv->config->port));
            uint32_t hung = getreg32(I2S_LC_HUNG_CONF_REG(priv->config->port));

            syslog(LOG_INFO,
                   "[I2SREG] PERI_SEL=0x%08x (want 3=I2S0)  I2S_INT_ST=0x%08x "
                   "TX_HUNG=%d\n", (unsigned)psel, (unsigned)ists,
                   !!(ists & I2S_TX_HUNG_INT_ST));
            syslog(LOG_INFO,
                   "[I2SREG] PCM2PDM_CONF=0x%08x  TXCONF=0x%08x "
                   "(TXPDM=%d TXTDM=%d)\n",
                   (unsigned)getreg32(I2S_TX_PCM2PDM_CONF_REG(
                                      priv->config->port)),
                   (unsigned)getreg32(I2S_TX_CONF_REG(priv->config->port)),
                   !!(getreg32(I2S_TX_CONF_REG(priv->config->port)) &
                      I2S_TX_PDM_EN),
                   !!(getreg32(I2S_TX_CONF_REG(priv->config->port)) &
                      I2S_TX_TDM_EN));
            syslog(LOG_INFO,
                   "[I2SREG] tx_update_spins=%d (max %d) fallback=%d "
                   "after_fallback=%d\n", g_tx_update_spins,
                   I2S_UPDATE_SPIN_MAX, g_sclk_fallback, g_tx_update_spins2);
            syslog(LOG_INFO,
                   "[I2SREG] LC_HUNG=0x%08x  LINK_after_enable=0x%08x "
                   "START_was=%d\n", (unsigned)hung,
                   (unsigned)g_dbg_link_after_enable,
                   !!(g_dbg_link_after_enable & DMA_OUTLINK_START_CH0_M));

            /* [V215] Is the GDMA outlink FSM alive at all?  Sample link +
             * INT_ST now, again 20 ms later, then try an explicit retry start
             * and sample a third time.  If the retry finally consumes the
             * descriptor (OWNER clears) the first start is being lost; if
             * OWNER never clears and INT_ST stays 0, the engine is not
             * fetching the descriptor at all. */
            uint32_t l1 = GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG,
                                          priv->dma_channel);
            uint32_t i1 = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG,
                                          priv->dma_channel);

            up_mdelay(20);

            uint32_t l2 = GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG,
                                          priv->dma_channel);
            uint32_t i2 = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG,
                                          priv->dma_channel);
            uint32_t dd = *(volatile uint32_t *)bfcontainer->dma_link;

            syslog(LOG_INFO,
                   "[I2SREG] t0: LINK=0x%08x INTST=0x%08x | "
                   "+20ms: LINK=0x%08x OWNER=%d INTST=0x%08x\n",
                   (unsigned)l1, (unsigned)i1, (unsigned)l2,
                   !!(dd & 0x80000000u), (unsigned)i2);

            /* [V344] The explicit re-arm that used to live here (a second
             * OUTLINK_START) has been removed.  It re-issued the start against
             * a descriptor the FSM had already consumed -- its OWNER was
             * already written back to 0 -- which the GDMA with
             * out_check_owner = 1 rejects with OUT_DSCR_ERR.  It also ran while
             * the real transfer was still in flight and stole its completion.
             * The read-only re-sample below is kept. */

            up_mdelay(20);

            {
              uint32_t l3 = GET_GDMA_CH_REG(DMA_OUT_LINK_CH0_REG,
                                            priv->dma_channel);
              uint32_t i3 = GET_GDMA_CH_REG(DMA_OUT_INT_ST_CH0_REG,
                                            priv->dma_channel);
              uint32_t d3 = *(volatile uint32_t *)bfcontainer->dma_link;

              syslog(LOG_INFO,
                     "[I2SREG] after RETRY+20ms: LINK=0x%08x "
                     "(START=%d PARK=%d) OWNER=%d INTST=0x%08x txirqN=%d\n",
                     (unsigned)l3, !!(l3 & DMA_OUTLINK_START_CH0_M),
                     !!(l3 & 0x00800000u), !!(d3 & 0x80000000u),
                     (unsigned)i3, g_txirq_n);

  /* [V251] The three registers that say WHY the outlink will not move:
   *   DMA_OUT_DSCR_CH0           current descriptor address held by the FSM
   *   DMA_OUTFIFO_STATUS_CH0     level/full/empty of the GDMA's own OUT FIFO
   *   DMA_OUT_STATE_CH0          the FSM state codes + outlink dscr address
   * If DSCR still reads 0 the FSM never loaded a descriptor; if OUTFIFO is
   * full the peripheral is not accepting data; if OUTFIFO is empty the
   * peripheral never asked for any. */

  

  /* [V344] The V260 TDM-slot / mono sweep that used to run here has been
   * removed.  It was a one-shot experiment, but it did lasting damage:
   *
   *   - case 7 set I2S_SIG_LOOPBACK and the restore block never cleared it,
   *     so every later transfer ran with the TX output looped back internally
   *     instead of driving DOUT (the register dump confirms SIGLOOP=1);
   *   - its closing restore wrote tx_mono=1 / tx_chan_equal=1, contradicting
   *     the V343 mono=0 / chan_equal=0 configuration;
   *   - its eight re-arms issued OUTLINK_START against descriptors the FSM had
   *     already consumed (OWNER written back to 0), which is what produced the
   *     OUT_DSCR_ERR storm seen in the ISR counters;
   *   - it reconfigured TX_TDM_CTRL and restarted the module while the real
   *     transfer was in flight.
   *
   * All of the information it collected is already captured by the read-only
   * [I2SREG] dump above and by the [V342] ISR forensics, so nothing is lost. */


            }
          }
        }

      return OK;

errout_with_buf:
      nxmutex_unlock(&priv->lock);
      i2s_buf_free(priv, bfcontainer);
      return ret;
    }

  return -ENOTTY;
}
#endif /* I2S_HAVE_TX */

/****************************************************************************
 * Name: i2s_receive
 *
 * Description:
 *   Receive a block of data on I2S.
 *
 * Input Parameters:
 *   dev      - Device-specific state data
 *   apb      - A pointer to the audio buffer in which to receive data
 *   callback - A user provided callback function that will be called at
 *              the completion of the transfer.
 *   arg      - An opaque argument that will be provided to the callback
 *              when the transfer complete
 *   timeout  - The timeout value to use.  The transfer will be cancelled
 *              and an ETIMEDOUT error will be reported if this timeout
 *              elapsed without completion of the DMA transfer.  Units
 *              are system clock ticks.  Zero means no timeout.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef I2S_HAVE_RX
static int i2s_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                       i2s_callback_t callback, void *arg, uint32_t timeout)
{
  g_v94_recv++;
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;

  if (priv->config->rx_en)
    {
      struct esp32s3_buffer_s *bfcontainer;
      int ret = OK;
      uint32_t nbytes;
      uint32_t nsamp;

      /* Check max audio buffer data size from the upper half and align the
       * receiving buffer according to the data width.
       */

      nbytes = apb->nmaxbytes;

      nbytes -= (nbytes % (priv->data_width / 8));

      nbytes = MIN(nbytes, ESP32S3_DMA_BUFLEN_MAX);

      /* Allocate a buffer container in advance */

      bfcontainer = i2s_buf_allocate(priv);
      if (bfcontainer == NULL)
        {
          i2serr("Failed to allocate the buffer container");
          return -ENOMEM;
        }

      /* Get exclusive access to the I2S driver data */

      ret = nxmutex_lock(&priv->lock);
      if (ret < 0)
        {
          goto errout_with_buf;
        }

      /* Add a reference to the audio buffer */

      apb_reference(apb);

      /* Initialize the buffer container structure */

      bfcontainer->callback = callback;
      bfcontainer->timeout  = timeout;
      bfcontainer->arg      = arg;
      bfcontainer->apb      = apb;
      bfcontainer->nbytes   = nbytes;
      bfcontainer->result   = -EBUSY;

      ret = i2s_rxdma_setup(priv, bfcontainer);

      if (ret != OK)
        {
          goto errout_with_buf;
        }

      /* [V682] Console report of the state this buffer was armed with.
       *
       * Deliberately here and not in i2s_rxdma_start(): that function is also
       * called from i2s_rx_schedule() on the interrupt stack, and the 2 KB IRQ
       * stack overflowed on exactly this kind of syslog() in v392.  i2s_receive()
       * always runs in the caller's task (the es8311 worker).
       *
       * Read it as:
       *   bclk(gpioN)<-sigM : which generator drives the pad
       *                       (RX = I2S0I_BCK_OUT_IDX, TX = I2S0O_BCK_OUT_IDX)
       *   rxstart / verify  : RX_START now, and the value read back after the
       *                       post-arm re-assert
       *   lost / stale      : >0 means RX_START never stuck, or rx.act was
       *                       left pending without a running channel (the
       *                       second-PTT failure mode)
       *   tdm=0x00010001    : CHAN0 only, two-slot frame (mono)
       *
       * Certainty: 已证实 as an observation tool.
       */

      if (g_v682_pad_logs < 3 || g_v682_start_lost || g_v682_act_stale)
        {
          uint32_t v682_rxconf =
            getreg32(I2S_RX_CONF_REG(priv->config->port));
          uint32_t v682_txconf =
            getreg32(I2S_TX_CONF_REG(priv->config->port));

          g_v682_pad_logs++;

          syslog(LOG_INFO,
                 "[V682] rx armed: role=%s bclk(gpio%d)<-sig%" PRIu32
                 " ws(gpio%d)<-sig%" PRIu32 " rxstart=%d verify=%d"
                 " txstart=%d lost=%d stale=%d tdm=0x%08" PRIx32
                 " rxconf=0x%08" PRIx32 " rxconf1=0x%08" PRIx32
                 " din(gpio%d) dout(gpio%d)\n",
                 g_v390_rxmaster ? "RXMASTER" : "RXSLAVE",
                 (int)priv->config->bclk_pin,
                 (uint32_t)(g_v682_pad_owner == 1 ?
                            priv->config->bclk_in_outsig :
                            priv->config->bclk_out_outsig),
                 (int)priv->config->ws_pin,
                 (uint32_t)(g_v682_pad_owner == 1 ?
                            priv->config->ws_in_outsig :
                            priv->config->ws_out_outsig),
                 (v682_rxconf & I2S_RX_START) ? 1 : 0,
                 g_v682_rxstart,
                 (v682_txconf & I2S_TX_START) ? 1 : 0,
                 g_v682_start_lost, g_v682_act_stale,
                 (uint32_t)getreg32(
                   I2S_RX_TDM_CTRL_REG(priv->config->port)),
                 v682_rxconf,
                 (uint32_t)getreg32(I2S_RX_CONF1_REG(priv->config->port)),
                 (int)priv->config->din_pin,
                 (int)priv->config->dout_pin);
        }

      i2sinfo("Prepared %d bytes to receive DMA buffers\n", apb->nmaxbytes);
      i2s_dump_buffer("Audio pipeline buffer:", &apb->samp[apb->curbyte],
                      apb->nbytes - apb->curbyte);

      nxmutex_unlock(&priv->lock);

      return OK;

errout_with_buf:
      nxmutex_unlock(&priv->lock);
      i2s_buf_free(priv, bfcontainer);
      return ret;
    }

  return -ENOTTY;
}
#endif /* I2S_HAVE_RX */

/****************************************************************************
 * Name: i2s_ioctl
 *
 * Description:
 *   Implement the lower-half logic ioctl commands
 *
 * Input parameters:
 *   dev - A reference to the lower-half I2S driver device
 *   cmd - The ioctl command
 *   arg - The argument accompanying the ioctl command
 *
 * Returned Value:
 *   OK on success; A negated errno value on failure.
 *
 ****************************************************************************/

/* [V671] Export the transfer-path counters so the playback layer can print
 * one line per utterance.  "full" counts transfers that had to reconfigure and
 * reset the transmitter; "light" counts the ones that only re-armed the DMA.
 * A healthy session is full == 1 and light == (transfers - 1).
 */

void esp32s3_i2s_v671_stats(uint32_t *full, uint32_t *light)
{
  if (full != NULL)
    {
      *full = (uint32_t)g_v671_full_n;
    }

  if (light != NULL)
    {
      *light = (uint32_t)g_v671_light_n;
    }
}

static int i2s_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;
  struct audio_buf_desc_s  *bufdesc;
  int ret = -ENOTTY;

  switch (cmd)
    {
      /* AUDIOIOC_START - Start the audio stream.
       *
       *   ioctl argument:  Audio session
       */

      case AUDIOIOC_START:
        {
          i2sinfo("AUDIOIOC_START\n");

          priv->streaming = true;

          ret = OK;
        }
        break;

      /* AUDIOIOC_STOP - Stop the audio stream.
       *
       *   ioctl argument:  Audio session
       */

#ifndef CONFIG_AUDIO_EXCLUDE_STOP
      case AUDIOIOC_STOP:
        {
          i2sinfo("AUDIOIOC_STOP\n");

          priv->streaming = false;

          ret = OK;
        }
        break;
#endif /* CONFIG_AUDIO_EXCLUDE_STOP */

      /* AUDIOIOC_ALLOCBUFFER - Allocate an audio buffer
       *
       *   ioctl argument:  pointer to an audio_buf_desc_s structure
       */

      case AUDIOIOC_ALLOCBUFFER:
        {
          i2sinfo("AUDIOIOC_ALLOCBUFFER\n");

          bufdesc = (struct audio_buf_desc_s *) arg;
          ret = apb_alloc(bufdesc);
        }
        break;

      /* AUDIOIOC_FREEBUFFER - Free an audio buffer
       *
       *   ioctl argument:  pointer to an audio_buf_desc_s structure
       */

      case AUDIOIOC_FREEBUFFER:
        {
          i2sinfo("AUDIOIOC_FREEBUFFER\n");

          bufdesc = (struct audio_buf_desc_s *) arg;
          DEBUGASSERT(bufdesc->u.buffer != NULL);
          apb_free(bufdesc->u.buffer);
          ret = sizeof(struct audio_buf_desc_s);
        }
        break;

      default:
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: i2s_dma_setup
 *
 * Description:
 *   Configure the DMA for the I2S peripheral
 *
 * Input Parameters:
 *   priv - Partially initialized I2S device structure. This function
 *          will complete the I2S specific portions of the initialization
 *          regarding the DMA operation.
 *
 * Returned Value:
 *   OK on success; A negated errno value on failure.
 *
 ****************************************************************************/

static int i2s_dma_setup(struct esp32s3_i2s_s *priv)
{
  int ret;
  int i2s_dma_dev;

  if (priv->config->port == 0)
    {
      i2s_dma_dev = ESP32S3_DMA_PERIPH_I2S0;
    }
  else
    {
      i2s_dma_dev = ESP32S3_DMA_PERIPH_I2S1;
    }

  /* Request a GDMA channel for the I2S peripheral */

  priv->dma_channel = esp32s3_dma_request(i2s_dma_dev, 1, 1, false);
  if (priv->dma_channel < 0)
    {
      i2serr("Failed to allocate GDMA channel\n");
      return ERROR;
    }

  /* [V268] *** Only ONE GDMA channel may serve I2S0 ***
   *
   * Measured: a second I2S instance runs during bring-up and takes channel 0,
   * binding DMA_PERI_OUT_SEL = 3 on both directions, i.e. to I2S0 -- the same
   * peripheral this instance serves from channel 1.  Two channels with the
   * same peripheral id means the I2S's DMA request fans out to both and the
   * data path is no longer deterministic, which fits every measurement so
   * far: the engine pulls the descriptor into its own FIFO (FIFO reads FULL)
   * yet nothing is ever handed to the I2S, so the transmitter drains nothing,
   * goes idle and the transfer times out.
   *
   * If the other channel is bound to THIS peripheral, unbind it and let one
   * channel own I2S0.  A channel bound to a different peripheral is left
   * strictly alone. */

  {
    uint32_t c0o = getreg32(DMA_OUT_PERI_SEL_CH0_REG);
    uint32_t c0i = getreg32(DMA_IN_PERI_SEL_CH0_REG);

    

    if (priv->dma_channel != 0 && c0o == 3u && c0i == 3u)
      {
        SET_GDMA_CH_REG(DMA_OUT_PERI_SEL_CH0_REG, 0, 0x3fu);
        SET_GDMA_CH_REG(DMA_IN_PERI_SEL_CH0_REG, 0, 0x3fu);
        
      }
    else if (priv->dma_channel != 0 && c0o == 0x3fu && c0i == 0x3fu)
      {
        priv->dma_channel = 0;
        SET_GDMA_CH_REG(DMA_OUT_PERI_SEL_CH0_REG, 0, 3u);
        SET_GDMA_CH_REG(DMA_IN_PERI_SEL_CH0_REG, 0, 0x3fu);
        
      }
  }

  /* Set up to receive GDMA interrupts on the current CPU. Each TX/RX channel
   * will be assigned to a different CPU interrupt.
   */

  priv->cpu = this_cpu();

#ifdef I2S_HAVE_TX
  if (priv->config->tx_en)
    {
      int periph = ESP32S3_PERIPH_DMA_OUT_CH0 + priv->dma_channel;
      int cpuint = esp32s3_setup_irq(priv->cpu, periph, 1,
                                     ESP32S3_CPUINT_LEVEL);
      if (cpuint < 0)
        {
          i2serr("Failed to allocate a CPU interrupt.\n");
          return ERROR;
        }

      priv->tx_irq = ESP32S3_PERIPH2IRQ(periph);
      ret = irq_attach(priv->tx_irq, i2s_tx_interrupt, priv);
      if (ret != OK)
        {
          i2serr("Couldn't attach IRQ to handler.\n");
          esp32s3_teardown_irq(priv->cpu, periph, cpuint);
          return ret;
        }
    }
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
  if (priv->config->rx_en)
    {
      int periph = ESP32S3_PERIPH_DMA_IN_CH0 + priv->dma_channel;
      int cpuint = esp32s3_setup_irq(priv->cpu, periph, 1,
                                     ESP32S3_CPUINT_LEVEL);
      if (cpuint < 0)
        {
          i2serr("Failed to allocate a CPU interrupt.\n");
          return ERROR;
        }

      priv->rx_irq = ESP32S3_PERIPH2IRQ(periph);
      ret = irq_attach(priv->rx_irq, i2s_rx_interrupt, priv);
      if (ret != OK)
        {
          i2serr("Couldn't attach IRQ to handler.\n");
          esp32s3_teardown_irq(priv->cpu, periph, cpuint);
          return ret;
        }
    }
#endif /* I2S_HAVE_RX */

  return OK;
}

/****************************************************************************
 * Name: esp32s3_i2sbus_initialize
 *
 * Description:
 *   Initialize the selected I2S port
 *
 * Input Parameters:
 *   Port number (for hardware that has multiple I2S interfaces)
 *
 * Returned Value:
 *   Valid I2S device structure reference on success; a NULL on failure
 *
 ****************************************************************************/

struct i2s_dev_s *esp32s3_i2sbus_initialize(int port)
{
  int ret;
  struct esp32s3_i2s_s *priv = NULL;
  irqstate_t flags;

  i2sinfo("port: %d\n", port);

  /* Statically allocated I2S' device strucuture */

  switch (port)
    {
#ifdef CONFIG_ESP32S3_I2S0
      case ESP32S3_I2S0:
        priv = &esp32s3_i2s0_priv;
        break;
#endif
#ifdef CONFIG_ESP32S3_I2S1
      case ESP32S3_I2S1:
        priv = &esp32s3_i2s1_priv;
        break;
#endif
      default:
        return NULL;
    }

  /* Allocate buffer containers */

  ret = i2s_buf_initialize(priv);
  if (ret < 0)
    {
      goto err;
    }

  flags = spin_lock_irqsave(&priv->slock);

  i2s_configure(priv);

  ret = i2s_dma_setup(priv);
  if (ret < 0)
    {
      goto err;
    }

#ifdef I2S_HAVE_TX
  /* Start TX channel */

  if (priv->config->tx_en)
    {
      priv->tx_started = false;
      g_tx_cfg_done    = 0;   /* [V346] force reconfig on next activation */
      i2s_tx_channel_start(priv);
    }
#endif /* I2S_HAVE_TX */

#ifdef I2S_HAVE_RX
  /* Start RX channel */

  if (priv->config->rx_en)
    {
      priv->rx_started = false;
      i2s_rx_channel_start(priv);
    }
#endif /* I2S_HAVE_RX */

  spin_unlock_irqrestore(&priv->slock, flags);

  /* Success exit */

  i2sinfo("I2S%d was successfully initialized\n", priv->config->port);

  return &priv->dev;

  /* Failure exit */

err:
  spin_unlock_irqrestore(&priv->slock, flags);
  return NULL;
}

#endif /* CONFIG_ESP32S3_I2S */


/****************************************************************************
 * [V222] Self-contained I2S0 TX one-shot write.
 *
 * Written directly against the authoritative ESP32-S3 register definitions
 * (soc/gdma_struct.h, soc/i2s_struct.h) and following ESP-IDF's ordering,
 * instead of the driver's hand-rolled txdma helpers.  Every call re-derives
 * the whole chain from scratch so no stale state can survive, and completion
 * is POLLED rather than taken from the GDMA interrupt.
 *
 * Returns the number of bytes written, or a negated errno.
 ****************************************************************************/

static struct esp32s3_dmadesc_s g_raw_desc __attribute__((aligned(4)));
static int g_raw_log = 4;

int esp32s3_i2s_tx_write_raw(FAR struct i2s_dev_s *dev,
                             FAR const uint8_t *buf, size_t len)
{
  struct esp32s3_i2s_s *priv = (struct esp32s3_i2s_s *)dev;
  const int16_t *samp;
  size_t total;                 /* number of 16-bit mono samples */
  size_t i = 0;
  int port;
  struct timespec t0;
  struct timespec now;
  uint32_t spin;
  uint32_t k;

  if (dev == NULL || buf == NULL || len < 2 || (len & 1) != 0)
    {
      return -EINVAL;
    }

  port = priv->config->port;
  samp  = (const int16_t *)buf;
  total = len / 2;

  /* 1. Bring the TX side up exactly as before: reset, push the configuration
   *    into the I2S clock domain, then start.  The DMA engine is deliberately
   *    not touched -- it cannot fetch descriptors on this board (measured at
   *    register level: outlink parks, no fetch, no error flag, with every
   *    clock and power domain verified enabled). */

  if (!g_tx_streaming)
      /* [V246] *** THE ROOT CAUSE ***
       *
       * es8311.c:1465 calls
       *     esp32s3_i2s_set_rxonly(priv->i2s,
       *                             priv->audio_mode != ES_MODULE_DAC);
       * This board runs the codec as ES_MODULE_ADC_DAC, so the argument is
       * true and g_i2s_rxonly_port becomes 0.  i2s_tx_active() then returns
       * false for port 0, so the "configure the TX module" block inside
       * i2s_configure() (TX reset, slave mode, data width, TDM/WS setup,
       * clock) is SKIPPED ENTIRELY.  The TX module is therefore never
       * brought up: it neither shifts data onto DOUT (measured stuck at 0
       * while BCLK/WS/MCLK ran) nor raises a DMA request, so the GDMA
       * outlink stays parked.  Nothing ever calls it with on=false.
       *
       * Take the port back for TX.  set_rxonly(false) clears the override
       * and re-runs i2s_configure(), which restores the whole TX block. */

      syslog(LOG_INFO, "[RAW] rxonly_port=%d -> taking TX ownership\n",
             g_i2s_rxonly_port);

      esp32s3_i2s_set_rxonly(dev, false);

      syslog(LOG_INFO, "[RAW] rxonly_port=%d after TX bring-up\n",
             g_i2s_rxonly_port);

      /* [V240] The XTAL source experiment (V238) is REVERTED.
       *
       * Forcing I2S_TX_CLK_SEL=0 and recomputing the dividers from 40 MHz made
       * the speaker output disappear entirely (user report), while the previous
       * CLK160 selection produced audible output.  Whatever the MCLK path does
       * differently in the XTAL case, it is not worth chasing: the clock source
       * is back to i2s_configure()'s choice. */

    {
    modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_START, 0);
    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_RESET);
    modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_RESET, 0);
    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_FIFO_RESET);
    modifyreg32(I2S_TX_CONF_REG(port), I2S_TX_FIFO_RESET, 0);

    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
    for (spin = 0;
         spin < I2S_UPDATE_SPIN_MAX &&
         (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE);
         spin++)
      {
      }

    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
    for (spin = 0;
         spin < I2S_UPDATE_SPIN_MAX &&
         (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE);
         spin++)
      {
      }
    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_START);

      /* [V236] tx_conf lives in the APB domain; only tx_update pushes it
       * into the I2S TX clock domain.  Set start first, then update once
       * more so the clock domain actually sees tx_start -- otherwise the
       * serialiser never reads the FIFO and every write is dropped. */
      modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
      for (spin = 0;
           spin < I2S_UPDATE_SPIN_MAX &&
           (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE);
           spin++)
        {
        }

      clock_gettime(CLOCK_MONOTONIC, &t0);
      g_tx_start_ns = (long long)t0.tv_sec * 1000000000LL + t0.tv_nsec;
      g_tx_delivered = 0;
      g_tx_streaming = 1;
    }

  /* 2. Push PCM straight into the 32-bit TX FIFO, paced in real time.
   *
   *    One FIFO word is one frame (left+right slot), so a mono sample is
   *    duplicated into both halves.  That is correct whether the hardware
   *    reads one 32-bit frame or two 16-bit slots per FIFO entry.
   *
   *    Pacing: 16 kHz mono => one frame every 62.5 us.  The FIFO holds 64
   *    frames (4 ms); pushing 48 frames (3 ms) per burst keeps it between
   *    16 and 64 entries, so neither an underrun nor an overflow can occur
   *    even with the +-1 ms granularity of usleep().  Long waits yield the
   *    CPU so Wi-Fi and the rest of the system keep running; only the last
   *    millisecond is busy-waited for accuracy. */

  /* [V244] Prove the DATA pin is actually carrying bits.
   *
   * BCLK/WS/MCLK were verified toggling earlier, but the serial data output
   * was never checked.  If DOUT (GPIO7) is stuck the codec receives clocks and
   * no data -- exactly the "amplifier hiss, no tone" symptom.  The pins are
   * driven through the GPIO matrix, so enabling only the pad input buffer makes
   * the level readable. */
  if (g_raw_log > 0)
    {
      unsigned int dhi = 0;
      unsigned int chi = 0;
      unsigned int k;

      modifyreg32(IO_MUX_GPIO7_REG, 0, (1u << 9));    /* FUN_IE */
      modifyreg32(IO_MUX_GPIO10_REG, 0, (1u << 9));

      for (k = 0; k < 4000; k++)
        {
          if (esp32s3_gpioread(7))
            {
              dhi++;
            }

          if (esp32s3_gpioread(10))
            {
              chi++;
            }
        }

      syslog(LOG_INFO,
             "[RAW] pins: DOUT(7) hi=%u/4000  BCLK(10) hi=%u/4000\n",
             dhi, chi);
    }

  /* [V245] Feed the I2S through the GDMA using ESP-IDF's LL sequence.
   *
   * ESP32-S3 has no CPU-accessible I2S FIFO port -- 0x00/0x04/0x08 are
   * reserved in the SoC definition (the first real I2S register is int_raw at
   * 0x0C) -- so the earlier polled-FIFO writer was pushing samples into
   * reserved space.  That is why DOUT (GPIO7) measured stuck at 0 while BCLK
   * ran, and why the codec produced only its noise floor.  The I2S FIFO can
   * only be fed by the GDMA, so the outlink has to work.
   *
   * The sequence mirrors ESP-IDF's gdma_ll_tx_* helpers one for one, and
   * samples out.dscr (the descriptor the FSM has pre-fetched) plus link.park
   * (FSM idle) so the outlink state is observable instead of inferred. */

  {
    uint32_t base = DMA_OUT_CONF0_CH0_REG + priv->dma_channel *
                                             GDMA_REG_OFFSET;
    uint32_t link = DMA_OUT_LINK_CH0_REG + priv->dma_channel *
                                             GDMA_REG_OFFSET;

    g_raw_desc.ctrl = (uint32_t)(len & ESP32S3_DMA_CTRL_BUFLEN_V) |
                      ((uint32_t)(len & ESP32S3_DMA_CTRL_DATALEN_V)
                       << ESP32S3_DMA_CTRL_DATALEN_S) |
                      ESP32S3_DMA_CTRL_EOF | ESP32S3_DMA_CTRL_OWN;
    g_raw_desc.pbuf = buf;
    g_raw_desc.next = NULL;

    modifyreg32(base, 0, 1u << 0);          /* out_rst = 1            */
    modifyreg32(base, 1u << 0, 0);          /* out_rst = 0            */

    SET_GDMA_CH_REG(DMA_OUT_PERI_SEL_CH0_REG, priv->dma_channel, 3);

    CLR_GDMA_CH_BITS(DMA_OUT_CONF1_CH0_REG, priv->dma_channel,
                     DMA_OUT_CHECK_OWNER_CH0);

    modifyreg32(base, 0, (1u << 4));        /* outdscr_burst_en       */
    modifyreg32(base, 1u << 5, 0);          /* out_data_burst_en off  */

    SET_GDMA_CH_REG(DMA_OUT_INT_CLR_CH0_REG, priv->dma_channel, UINT32_MAX);
    SET_GDMA_CH_REG(DMA_OUT_INT_ENA_CH0_REG, priv->dma_channel, 0);

    putreg32((uint32_t)&g_raw_desc, link);

    syslog(LOG_INFO, "[RAW] gdma: link=0x%08x base=0x%08x desc=0x%08x\n",
           (unsigned)getreg32(link), (unsigned)getreg32(base),
           (unsigned)((uint32_t)&g_raw_desc));

    SET_GDMA_CH_BITS(DMA_OUT_LINK_CH0_REG, priv->dma_channel,
                     DMA_OUTLINK_START_CH0_M);

    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_UPDATE);
    for (spin = 0;
         spin < I2S_UPDATE_SPIN_MAX &&
         (getreg32(I2S_TX_CONF_REG(port)) & I2S_TX_UPDATE);
         spin++)
      {
      }
    modifyreg32(I2S_TX_CONF_REG(port), 0, I2S_TX_START);

    for (k = 0; k < 200000; k++)
      {
        uint32_t lnk = getreg32(link);
        uint32_t dsc = GET_GDMA_CH_REG(DMA_OUT_DSCR_CH0_REG,
                                       priv->dma_channel);

        if (k == 0 || k == 50000)
          {
            syslog(LOG_INFO,
                   "[RAW] fsm k=%u park=%d start=%d dscr=0x%08x owner=%d\n",
                   (unsigned)k, !!(lnk & 0x00800000u), !!(lnk & 0x00200000u),
                   (unsigned)dsc,
                   !!(g_raw_desc.ctrl & ESP32S3_DMA_CTRL_OWN));
          }

        if ((g_raw_desc.ctrl & ESP32S3_DMA_CTRL_OWN) == 0)
          {
            break;
          }

        up_udelay(10);
      }

    syslog(LOG_INFO, "[RAW] gdma done: owner=%d waited=%u dscr=0x%08x\n",
           !!(g_raw_desc.ctrl & ESP32S3_DMA_CTRL_OWN), (unsigned)k,
           (unsigned)GET_GDMA_CH_REG(DMA_OUT_DSCR_CH0_REG,
                                     priv->dma_channel));
  }


  if (g_raw_log > 0)
    {
      g_raw_log--;
      clock_gettime(CLOCK_MONOTONIC, &now);
      syslog(LOG_INFO,
             "[RAW] FIFO wrote %u samples (%u bytes) in %lld us; "
             "txconf=0x%08x\\n",
             (unsigned)total, (unsigned)len,
             (long long)((now.tv_sec - t0.tv_sec) * 1000000LL +
                         (now.tv_nsec - t0.tv_nsec) / 1000LL),
             (unsigned)getreg32(I2S_TX_CONF_REG(port)));
    }

  return (int)len;
}
