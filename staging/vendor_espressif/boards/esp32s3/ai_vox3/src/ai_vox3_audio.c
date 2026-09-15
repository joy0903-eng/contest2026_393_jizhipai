/****************************************************************************
 * src/ai_vox3_audio.c
 *
 * ES8311 audio codec + ESP32-S3 I2S bring-up for the emakefun AI-VOX3.
 *
 *  - ES8311 is configured over I2C (SCL=12, SDA=13, addr 0x18).
 *  - Audio data moves over I2S (MCLK=11, BCLK=10, WS/DOUT/DIN = ambiguous).
 *
 * The three I2S data pins (WS, DOUT, DIN) are documented inconsistently by
 * the vendor and are centralized in board.h as BOARD_ES8311_I2S_*. They must
 * be oscilloscope-verified on the real board (see TODO markers below).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8311.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>
#include <esp32s3_gpio.h>
#include <esp32s3_i2c.h>
#include <esp32s3_i2s.h>

#include "board.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C0 (I2CEXT0) output signal routing. */
#define AI_VOX3_I2C0_SCL_OUT   I2CEXT0_SCL_OUT
#define AI_VOX3_I2C0_SDA_OUT   I2CEXT0_SDA_OUT

/* I2S0 output signal routing.
 * TODO(real-device): confirm exact I2S0 signal enum names in the current
 * tree (esp32s3_gpio_sigmap.h). The MCLK/BCLK mapping is unambiguous;
 * WS / DO(ESP->codec) / DI(ESP<-codec) mapping must match the verified pins.
 */
#define AI_VOX3_I2S0_MCLK_OUT  I2S0_MCLK_OUT
#define AI_VOX3_I2S0_BCLK_OUT  I2S0_BCLK_OUT
#define AI_VOX3_I2S0_WS_OUT    I2S0_WS_OUT
#define AI_VOX3_I2S0_DO_OUT    I2S0_DO_OUT   /* ESP data out  -> codec DIN */
#define AI_VOX3_I2S0_DI_OUT    I2S0_DI_OUT   /* ESP data in   <- codec DOUT */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct audio_lowerhalf_s *g_audio_codec = NULL;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_audio_initialize
 *
 * Description:
 *   Initialize I2C + I2S, route pins, bind the ES8311 codec and register the
 *   audio device as /dev/audio/pcm0 (or similar). Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_audio_initialize(void)
{
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  /* --- I2C: configure ES8311 control bus --- */
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2C_SCL, AI_VOX3_I2C0_SCL_OUT, false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2C_SDA, AI_VOX3_I2C0_SDA_OUT, false, false);

  i2c = esp32s3_i2c_initialize(BOARD_ES8311_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to init I2C%d for ES8311\n",
             BOARD_ES8311_I2C_BUS);
      return -ENODEV;
    }

  /* --- I2S: route the (partially ambiguous) data pins --- */
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_MCLK, AI_VOX3_I2S0_MCLK_OUT, false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_BCLK, AI_VOX3_I2S0_BCLK_OUT, false, false);
  /* TODO(real-device): WS/LRCK, DOUT(8), DIN(7) — verify on real HW.
   * Current assumption: WS=9, ESP-DO->codec DIN=7, ESP-DI<-codec DOUT=8. */
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_WS,   AI_VOX3_I2S0_WS_OUT,  false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_DIN,  AI_VOX3_I2S0_DO_OUT, false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_DOUT, AI_VOX3_I2S0_DI_OUT, false, false);

  i2s = esp32s3_i2s_initialize(BOARD_ES8311_I2S_PORT);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to init I2S%d\n", BOARD_ES8311_I2S_PORT);
      return -ENODEV;
    }

  /* --- Bind the ES8311 codec driver ---
   * The exact argument order of es8311_initialize() depends on the tree
   * version; all required handles (i2c, i2s, addr) are passed here. */
  g_audio_codec = es8311_initialize(i2c, i2s, BOARD_ES8311_I2C_ADDR);
  if (g_audio_codec == NULL)
    {
      syslog(LOG_ERR, "ERROR: es8311_initialize failed\n");
      return -ENODEV;
    }

  /* Register the audio device so the framework can open it. */
  ret = audio_register(0, g_audio_codec);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio_register failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "Audio (ES8311 + I2S) initialized\n");
  return OK;
}
