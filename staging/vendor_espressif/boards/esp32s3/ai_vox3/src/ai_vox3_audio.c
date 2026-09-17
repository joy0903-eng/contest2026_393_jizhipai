/****************************************************************************
 * src/ai_vox3_audio.c
 *
 * ES8311 audio codec + ESP32-S3 I2S bring-up for the emakefun AI-VOX3.
 *
 *  - ES8311 control is over I2C0 (SCL=12, SDA=13, addr 0x18).  The I2C
 *    driver routes its own pins from CONFIG_ESP32S3_I2C0_SCLPIN/SDAPIN,
 *    so no manual GPIO-matrix routing is done here.
 *  - Audio data moves over I2S0 (MCLK=11, BCLK=10, WS=9, ESP-DOUT to
 *    codec DIN=7, codec DOUT to ESP-DIN=8).  I2S has no IOMUX and no
 *    Kconfig pin options on the ESP32-S3, so the GPIO matrix is used
 *    here with the signal indices from hardware/esp32s3_gpio_sigmap.h.
 *
 * TODO(real-device): the WS/DOUT/DIN pin assignment is documented
 * inconsistently by the vendor (see board.h) -- verify on the real board
 * with an oscilloscope before relying on audio capture/playback.
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
#include <hardware/esp32s3_gpio_sigmap.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Persistent board configuration for the ES8311 driver (must outlive the
 * initialize call -- the driver keeps referencing it). */

static const struct es8311_lower_s g_es8311_lower =
{
  .frequency = 100000,                  /* ES8311 control I2C frequency */
  .address   = BOARD_ES8311_I2C_ADDR
};

static struct audio_lowerhalf_s *g_audio_codec = NULL;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_audio_initialize
 *
 * Description:
 *   Initialize I2C + I2S, route the I2S pins through the GPIO matrix, bind
 *   the ES8311 codec and register the audio device.  Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_audio_initialize(void)
{
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  /* --- I2C0: ES8311 control bus (driver routes SCL/SDA itself) --- */
  i2c = esp32s3_i2cbus_initialize(BOARD_ES8311_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to init I2C%d for ES8311\n",
             BOARD_ES8311_I2C_BUS);
      return -ENODEV;
    }

  /* --- I2S0: route clocks + data via the GPIO matrix ---
   * TX is master: MCLK/BCLK/WS are outputs.  Pin BOARD_ES8311_I2S_DIN is
   * the codec's DSDIN, i.e. the controller's data OUTPUT signal; pin
   * BOARD_ES8311_I2S_DOUT is the codec's ASDOUT, feeding the controller's
   * data INPUT signal (matrix_in). */
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_MCLK, I2S0_MCLK_OUT_IDX, false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_BCLK, I2S0O_BCK_OUT_IDX, false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_WS,   I2S0O_WS_OUT_IDX,  false, false);
  esp32s3_gpio_matrix_out(BOARD_ES8311_I2S_DIN,  I2S0O_SD_OUT_IDX,  false, false);
  esp32s3_gpio_matrix_in(BOARD_ES8311_I2S_DOUT,  I2S0I_SD_IN_IDX,   false);

  i2s = esp32s3_i2sbus_initialize(BOARD_ES8311_I2S_PORT);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to init I2S%d\n", BOARD_ES8311_I2S_PORT);
      return -ENODEV;
    }

  /* --- Bind the ES8311 codec driver (i2c, i2s, persistent lower half) --- */
  g_audio_codec = es8311_initialize(i2c, i2s, &g_es8311_lower);
  if (g_audio_codec == NULL)
    {
      syslog(LOG_ERR, "ERROR: es8311_initialize failed\n");
      return -ENODEV;
    }

  /* Register the audio device so the framework can open it.
   *
   * 2026-09-17: this call used to be `audio_register(0, g_audio_codec)`.
   * The first parameter is `FAR const char *name`, NOT an index, so the
   * literal 0 was passed as a NULL pointer and audio_register() bailed out
   * on its own guard:
   *     if (!name || !dev) { auderr("ERROR: Invalid arguments"); return -EINVAL; }
   * Result: -EINVAL on every boot, /dev/audio/pcm0 never created, and all of
   * audio_pipeline.c silently non-functional.  Pass the name string instead;
   * audio_register() prepends "/dev/audio/" itself, yielding /dev/audio/pcm0
   * (matching AUDIO_DEV in the app and the upstream pcm[x] convention where
   * x is the I2S port number -- this board uses I2S0).
   */
  ret = audio_register(BOARD_AUDIO_DEV_NAME, g_audio_codec);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio_register(\"%s\") failed: %d\n",
             BOARD_AUDIO_DEV_NAME, ret);
      return ret;
    }

  syslog(LOG_INFO, "Audio (ES8311 + I2S) initialized, node /dev/audio/%s\n",
         BOARD_AUDIO_DEV_NAME);
  return OK;
}
