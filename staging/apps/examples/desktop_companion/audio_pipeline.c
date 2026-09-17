/****************************************************************************
 * apps/examples/desktop_companion/audio_pipeline.c
 *
 * ES8311 audio pipeline: capture (RX) + playback (TX) + prompt tones.
 *
 * NOTE on the ioctl to use for format setup.  An earlier revision of this
 * comment said "AUDIOIOC_SETAUDIOINFO".  That token DOES NOT EXIST in
 * include/nuttx/audio/audio.h -- and in this codebase a wrong symbol is not a
 * compile error you notice, it is a silent feature loss.  The tokens that do
 * exist upstream are:
 *
 *   AUDIOIOC_CONFIGURE       _AUDIOIOC(4)   configure device for a mode
 *   AUDIOIOC_GETCAPS         _AUDIOIOC(1)   query capabilities
 *   AUDIOIOC_START           _AUDIOIOC(6)   start streaming
 *   AUDIOIOC_STOP            _AUDIOIOC(7)   stop streaming
 *   AUDIOIOC_GETBUFFERINFO   _AUDIOIOC(10)  buffer geometry
 *   AUDIOIOC_SETBUFFERINFO   _AUDIOIOC(17)  buffer geometry
 *   AUDIOIOC_SETPARAMTER     _AUDIOIOC(18)  set stream params
 *                            ^^ note the upstream spelling: "SETPARAMTER",
 *                            missing the E.  Not our typo -- do not "fix" it,
 *                            grepping for AUDIOIOC_SETPARAMETER finds nothing.
 *   AUDIOIOC_GETAUDIOINFO    _AUDIOIOC(22)  read back the active audio info
 *
 * The device node is /dev/audio/pcm0 and it is created by
 * audio_register(BOARD_AUDIO_DEV_NAME, ...) in the board's
 * ai_vox3_audio.c -- which until 2026-09-17 passed a literal 0 as the name
 * and therefore never registered anything at all.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>

#include "audio_pipeline.h"
#include "config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_DEV            "/dev/audio/pcm0"
#define TONE_DURATION_MS     150
#define TONE_BUF_SAMPLES     (AIVOX3_AUDIO_RATE * TONE_DURATION_MS / 1000)
#define TONE_BUF_BYTES       (TONE_BUF_SAMPLES * sizeof(int16_t))

/* Tone frequencies (Hz) by tone_id. */
static const int g_tone_freq[4] =
{
  880,   /* 0: short ack */
  660,   /* 1: think */
  1040,  /* 2: happy */
  440    /* 3: sleep */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_audio_fd = -1;
static bool g_capturing = false;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: audio_init
 ****************************************************************************/

int audio_init(void)
{
  if (g_audio_fd >= 0)
    {
      return OK;
    }

  g_audio_fd = open(AUDIO_DEV, O_RDWR);
  if (g_audio_fd < 0)
    {
      syslog(LOG_ERR, "ERROR: open %s failed: %d\n", AUDIO_DEV, errno);
      return -errno;
    }

  /* TODO(real-device): configure the ES8311 stream format before read/write.
   *
   * Use AUDIOIOC_CONFIGURE (token _AUDIOIOC(4)) with a struct audio_caps_s
   * carrying AIVOX3_AUDIO_RATE / 16-bit / mono, and AUDIOIOC_START to begin
   * streaming.  Do NOT use AUDIOIOC_SETAUDIOINFO -- that token does not exist
   * (see the file header).  Left unwritten on purpose: the codec's own
   * defaults are already 16 kHz/16-bit, and we want the first real-device
   * run to tell us whether format setup is actually needed before adding it.
   */
  return OK;
}

/****************************************************************************
 * Name: audio_capture_start
 ****************************************************************************/

int audio_capture_start(void)
{
  if (g_audio_fd < 0)
    {
      return -EAGAIN;
    }

  g_capturing = true;
  syslog(LOG_INFO, "audio: capture started (RX only; no ASR yet)\n");
  return OK;
}

/****************************************************************************
 * Name: audio_capture_stop
 ****************************************************************************/

int audio_capture_stop(void)
{
  g_capturing = false;
  return OK;
}

/****************************************************************************
 * Name: audio_capture_read
 ****************************************************************************/

int audio_capture_read(uint8_t *buf, size_t len)
{
  int n;

  if (!g_capturing || g_audio_fd < 0 || buf == NULL)
    {
      return -EAGAIN;
    }

  n = read(g_audio_fd, buf, len);
  return n < 0 ? -errno : n;
}

/****************************************************************************
 * Name: audio_play_tone
 ****************************************************************************/

int audio_play_tone(int tone_id)
{
  static int16_t tone[TONE_BUF_SAMPLES];
  int freq;
  int i;
  int16_t sample;

  if (g_audio_fd < 0)
    {
      return -EAGAIN;
    }

  if (tone_id < 0 || tone_id > 3)
    {
      tone_id = 0;
    }

  freq = g_tone_freq[tone_id];

  /* Generate a square-ish tone with a simple linear fade-in/out envelope to
   * reduce clicks. (No floating point / math lib dependency.) */
  for (i = 0; i < TONE_BUF_SAMPLES; i++)
    {
      int phase = (i * freq * 360) / AIVOX3_AUDIO_RATE;  /* degrees */
      int sgn = (phase % 360) < 180 ? 1 : -1;
      int env = 1;

      /* Envelope: ramp first/last 10%. */
      if (i < TONE_BUF_SAMPLES / 10)
        {
          env = i * 10 / (TONE_BUF_SAMPLES / 10);
        }
      else if (i > TONE_BUF_SAMPLES - TONE_BUF_SAMPLES / 10)
        {
          env = (TONE_BUF_SAMPLES - i) * 10 / (TONE_BUF_SAMPLES / 10);
        }

      sample = (int16_t)(sgn * 8000 * env / 10);
      tone[i] = sample;
    }

  return audio_play_pcm((const uint8_t *)tone, TONE_BUF_BYTES);
}

/****************************************************************************
 * Name: audio_play_pcm
 ****************************************************************************/

int audio_play_pcm(const uint8_t *pcm, size_t len)
{
  int n;
  size_t off = 0;

  if (g_audio_fd < 0 || pcm == NULL)
    {
      return -EAGAIN;
    }

  while (off < len)
    {
      n = write(g_audio_fd, pcm + off, len - off);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          syslog(LOG_ERR, "ERROR: audio write failed: %d\n", errno);
          return -errno;
        }

      off += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Name: asr_transcribe  (stub)
 ****************************************************************************/

int asr_transcribe(const uint8_t *pcm, size_t len,
                   char *text, size_t text_len)
{
  (void)pcm;
  (void)len;
  if (text != NULL && text_len > 0)
    {
      text[0] = '\0';
    }

  syslog(LOG_INFO, "asr_transcribe: not implemented (no ASR backend)\n");
  return -ENOSYS;
}

/****************************************************************************
 * Name: tts_synthesize  (stub)
 ****************************************************************************/

int tts_synthesize(const char *text, uint8_t *pcm, size_t *len)
{
  (void)text;
  (void)pcm;
  if (len != NULL)
    {
      *len = 0;
    }

  syslog(LOG_INFO, "tts_synthesize: not implemented (no TTS backend)\n");
  return -ENOSYS;
}
