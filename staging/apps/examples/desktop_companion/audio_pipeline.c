/****************************************************************************
 * apps/examples/desktop_companion/audio_pipeline.c
 *
 * ES8311 audio pipeline on top of pcm_stream (NuttX apb pipeline).
 *
 * HISTORY / WHY THIS FILE WAS REWRITTEN
 * -------------------------------------
 * The previous revision talked to /dev/audio/pcm0 with read() and write().
 * That can never work on this board:
 *
 *   - drivers/audio/es8311.c declares g_audioops with NULL for both the
 *     `read` and `write` members (es8311.c:176-177).
 *   - The NuttX audio upper half returns 0 (not -ENOSYS, not an error) when
 *     lower->ops->read / ->write is NULL.
 *
 * Therefore:
 *   - the old audio_capture_read() always returned 0 bytes -> ASR starved,
 *     silently.
 *   - the old audio_play_pcm() advanced its offset by the return value of
 *     write(), i.e. by 0, forever -> the whole firmware hung in that loop.
 *
 * The rewrite routes everything through pcm_stream (ALLOCBUFFER / ENQUEUE /
 * message-queue DEQUEUE / FREEBUFFER).  Neither read() nor write() is called
 * on the audio descriptor any more.
 *
 * HALF-DUPLEX
 * -----------
 * es8311_processbegin() picks I2S_SEND or I2S_RECEIVE from the single
 * priv->audio_mode field, and the upper half only forwards AUDIOIOC_CONFIGURE
 * to the driver while the session state is AUDIO_STATE_OPEN.  So we never try
 * to re-configure a live stream: each direction gets a fresh open()/close()
 * cycle, and g_audio_lock makes sure only one of them is ever live.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "audio_pipeline.h"
#include "config.h"
#include "dsp_vad.h"
#include "pcm_stream.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_DEV            "/dev/audio/pcm0"
#define TONE_DURATION_MS     150
#define TONE_BUF_SAMPLES     (AIVOX3_AUDIO_RATE * TONE_DURATION_MS / 1000)
#define TONE_BUF_BYTES       (TONE_BUF_SAMPLES * (int)sizeof(int16_t))

/* Capture ring buffer: 8 KiB = 256 ms @ 16 kHz / 16-bit / mono.  Oldest
 * bytes are dropped on overflow, so the capture callback never blocks.
 */

#define CAP_RING_BYTES       8192

/* Granularity of the wait in audio_capture_read_timeout(). */

#define CAP_POLL_MS          5

/* Frames we are willing to skip while a finished utterance is uncollected.
 * After this many frames the utterance is dropped and feeding resumes, so a
 * consumer that stops calling audio_capture_take_word() can never wedge the
 * VAD.  64 frames * 20 ms = 1.28 s.
 */

#define DSP_DROP_AFTER_FRAMES  64

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Single-producer / single-consumer byte ring with an explicit count,
 * protected by a mutex (ESP32-S3 is dual core: volatile alone is not
 * enough).
 */

struct cap_ring_s
{
  uint8_t       buf[CAP_RING_BYTES];
  unsigned int  head;   /* write index  (capture thread) */
  unsigned int  tail;   /* read index   (application)    */
  unsigned int  count;  /* bytes currently buffered      */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Tone frequencies (Hz) by tone_id. */
static const int g_tone_freq[4] =
{
  880,   /* 0: short ack */
  660,   /* 1: think */
  1040,  /* 2: happy */
  440    /* 3: sleep */
};

static bool               g_ready = false;
static pthread_mutex_t    g_audio_lock = PTHREAD_MUTEX_INITIALIZER;

static struct pcm_stream  g_cap;
static bool               g_capturing = false;

static struct cap_ring_s  g_ring;
static pthread_mutex_t    g_ring_lock = PTHREAD_MUTEX_INITIALIZER;

/* Offline voice front-end tap.
 *
 * g_dsp_on      - dsp_vad_init() succeeded, feeding is enabled.
 * g_word_pending- a finished utterance is waiting for audio_capture_take_word().
 *                 While it is set, capture_cb() stops feeding, which keeps the
 *                 VAD's utterance buffer stable for the consumer without
 *                 needing a second (89 KB) copy of it.
 * g_dsp_lock    - serialises dsp_vad_feed() against dsp_vad_take_word().
 *                 Held only for the duration of those two calls; kws_recognize()
 *                 is deliberately run by the caller OUTSIDE this lock.
 */

static bool               g_dsp_on       = false;
static bool               g_word_pending = false;
static unsigned int       g_dsp_skipped  = 0;
static pthread_mutex_t    g_dsp_lock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ring_put
 *
 * Description:
 *   Append `n` bytes from the capture callback.  Drops the oldest bytes when
 *   the ring is full so that the audio callback can never block.
 ****************************************************************************/

static void ring_put(const uint8_t *src, size_t n)
{
  size_t done = 0;

  pthread_mutex_lock(&g_ring_lock);

  while (done < n)
    {
      size_t space = (size_t)CAP_RING_BYTES - (size_t)g_ring.head;
      size_t chunk = n - done;

      if (chunk > space)
        {
          chunk = space;
        }

      memcpy(&g_ring.buf[g_ring.head], &src[done], chunk);
      g_ring.head = (g_ring.head + (unsigned int)chunk) % CAP_RING_BYTES;
      done += chunk;

      if ((size_t)g_ring.count + chunk > (size_t)CAP_RING_BYTES)
        {
          unsigned int over =
            (unsigned int)((size_t)g_ring.count + chunk -
                           (size_t)CAP_RING_BYTES);

          g_ring.tail  = (g_ring.tail + over) % CAP_RING_BYTES;
          g_ring.count = CAP_RING_BYTES;
        }
      else
        {
          g_ring.count += (unsigned int)chunk;
        }
    }

  pthread_mutex_unlock(&g_ring_lock);
}

/****************************************************************************
 * Name: ring_get
 *
 * Description:
 *   Remove up to `n` bytes from the ring.  Returns the byte count actually
 *   copied (0 when the ring is empty).  Never blocks.
 ****************************************************************************/

static size_t ring_get(uint8_t *dst, size_t n)
{
  size_t done = 0;

  if (dst == NULL || n == 0)
    {
      return 0;
    }

  pthread_mutex_lock(&g_ring_lock);

  while (done < n && g_ring.count > 0)
    {
      size_t avail = (size_t)CAP_RING_BYTES - (size_t)g_ring.tail;
      size_t chunk = (size_t)g_ring.count;

      if (chunk > n - done)
        {
          chunk = n - done;
        }

      if (chunk > avail)
        {
          chunk = avail;
        }

      memcpy(&dst[done], &g_ring.buf[g_ring.tail], chunk);
      g_ring.tail = (g_ring.tail + (unsigned int)chunk) % CAP_RING_BYTES;
      g_ring.count -= (unsigned int)chunk;
      done += chunk;
    }

  pthread_mutex_unlock(&g_ring_lock);
  return done;
}

/****************************************************************************
 * Name: capture_cb
 *
 * Description:
 *   pcm_stream capture callback, runs on the capture thread.  Copies into
 *   the ring buffer, feeds the offline VAD (when enabled) and returns.
 ****************************************************************************/

static void capture_cb(const int16_t *samples, int nsamples, void *arg)
{
  (void)arg;

  if (samples == NULL || nsamples <= 0)
    {
      return;
    }

  ring_put((const uint8_t *)samples, (size_t)nsamples * sizeof(int16_t));

  /* Offline voice front-end tap.  This runs on the capture thread, so it
   * must stay cheap: dsp_vad_feed() is O(nsamples) with no allocation and
   * no I/O.  Keyword recognition (MFCC + DTW) is orders of magnitude more
   * expensive and is deliberately NOT done here - it runs on the consumer
   * thread, see audio_capture_take_word().
   */

  if (!g_dsp_on)
    {
      return;
    }

  pthread_mutex_lock(&g_dsp_lock);

  if (!g_word_pending)
    {
      g_dsp_skipped = 0;
      if (dsp_vad_feed(samples, nsamples) > 0)
        {
          /* Utterance complete.  Stop feeding so the VAD's utterance
           * buffer is stable while the consumer collects it.  This is
           * what makes a zero-copy hand-off safe without a second 87 KB
           * buffer.
           */

          g_word_pending = true;
        }
    }
  else if (++g_dsp_skipped >= DSP_DROP_AFTER_FRAMES)
    {
      /* Nobody came to collect it.  Drop the utterance and resume so a
       * missing/stuck consumer can never wedge the VAD forever.
       */

      (void)dsp_vad_take_word(NULL, 0);
      g_word_pending = false;
      g_dsp_skipped  = 0;
    }

  pthread_mutex_unlock(&g_dsp_lock);
}

/****************************************************************************
 * Name: capture_restart
 *
 * Description:
 *   Re-open and restart the capture direction after a playback slot.
 *   Called with g_audio_lock held.
 ****************************************************************************/

static void capture_restart(void)
{
  int ret;

  ret = pcm_stream_open(&g_cap, AUDIO_TYPE_INPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "audio: capture restart: open failed: %d\n", ret);
      return;
    }

  ret = pcm_stream_start_capture(&g_cap, capture_cb, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "audio: capture restart: start failed: %d\n", ret);
      pcm_stream_close(&g_cap);
      return;
    }

  /* Playback stopped the codec, so any utterance in flight lost audio. */

  if (g_dsp_on)
    {
      (void)dsp_vad_reset();
      g_word_pending = false;
      g_dsp_skipped  = 0;
    }

  g_capturing = true;
}

/****************************************************************************
 * Name: play_locked
 *
 * Description:
 *   Half-duplex playback helper.  Caller must hold g_audio_lock.
 ****************************************************************************/

static int play_locked(const uint8_t *pcm, size_t len)
{
  struct pcm_stream out;
  bool restart_capture;
  int ret;

  if (pcm == NULL)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return OK;
    }

  /* The ES8311 cannot do both directions: give up the input path first. */

  restart_capture = g_capturing;
  if (g_capturing)
    {
      (void)pcm_stream_stop(&g_cap);
      pcm_stream_close(&g_cap);
      g_capturing = false;
    }

  ret = pcm_stream_open(&out, AUDIO_TYPE_OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "audio: play: open output failed: %d\n", ret);
      goto out_restart;
    }

  ret = pcm_stream_play(&out, pcm, len);
  pcm_stream_close(&out);

  if (ret < 0)
    {
      syslog(LOG_ERR, "audio: play: pcm_stream_play failed: %d\n", ret);
    }

out_restart:
  if (restart_capture)
    {
      capture_restart();
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: audio_init
 ****************************************************************************/

int audio_init(void)
{
  int fd;
  int errcode;

  if (g_ready)
    {
      return OK;
    }

  /* Probe only: the half-duplex codec is opened per use. */

  fd = open(AUDIO_DEV, O_RDWR);
  if (fd < 0)
    {
      errcode = errno;
      syslog(LOG_ERR, "ERROR: open %s failed: %d\n", AUDIO_DEV, errcode);
      return -errcode;
    }

  close(fd);

  memset(&g_cap, 0, sizeof(g_cap));
  memset(&g_ring, 0, sizeof(g_ring));
  g_word_pending = false;
  g_dsp_skipped  = 0;

  /* Offline voice front-end, best effort.  dsp_vad_init() allocates the
   * 44800-sample utterance buffer (~87 KB); if that fails the audio
   * pipeline must still work, keyword spotting is simply unavailable.
   * A DSP failure therefore never fails audio_init().
   */

  if (dsp_vad_init() == OK)
    {
      g_dsp_on = true;
      syslog(LOG_INFO, "audio: dsp_vad tap enabled\n");
    }
  else
    {
      g_dsp_on = false;
      syslog(LOG_WARNING, "audio: dsp_vad_init failed, KWS disabled\n");
    }

  g_ready = true;
  syslog(LOG_INFO, "audio: %s present (apb pipeline mode)\n", AUDIO_DEV);
  return OK;
}

/****************************************************************************
 * Name: audio_deinit
 ****************************************************************************/

int audio_deinit(void)
{
  pthread_mutex_lock(&g_audio_lock);

  if (g_capturing)
    {
      (void)pcm_stream_stop(&g_cap);
      pcm_stream_close(&g_cap);
      g_capturing = false;
    }

  memset(&g_ring, 0, sizeof(g_ring));

  if (g_dsp_on)
    {
      dsp_vad_deinit();
      g_dsp_on       = false;
      g_word_pending = false;
      g_dsp_skipped  = 0;
    }

  g_ready = false;

  pthread_mutex_unlock(&g_audio_lock);
  return OK;
}

/****************************************************************************
 * Name: audio_capture_start
 ****************************************************************************/

int audio_capture_start(void)
{
  int ret;

  pthread_mutex_lock(&g_audio_lock);

  if (!g_ready)
    {
      pthread_mutex_unlock(&g_audio_lock);
      return -EAGAIN;
    }

  if (g_capturing)
    {
      pthread_mutex_unlock(&g_audio_lock);
      return OK;
    }

  ret = pcm_stream_open(&g_cap, AUDIO_TYPE_INPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "audio: capture: open failed: %d\n", ret);
      pthread_mutex_unlock(&g_audio_lock);
      return ret;
    }

  ret = pcm_stream_start_capture(&g_cap, capture_cb, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "audio: capture: start failed: %d\n", ret);
      pcm_stream_close(&g_cap);
      pthread_mutex_unlock(&g_audio_lock);
      return ret;
    }

  /* Drop any half-captured utterance left over from a previous session;
   * its audio is gone, so it could never be a valid keyword.
   */

  if (g_dsp_on)
    {
      (void)dsp_vad_reset();
      g_word_pending = false;
      g_dsp_skipped  = 0;
    }

  g_capturing = true;
  pthread_mutex_unlock(&g_audio_lock);

  syslog(LOG_INFO, "audio: capture started (RX apb pipeline)\n");
  return OK;
}
/****************************************************************************
 * Name: audio_capture_stop
 ****************************************************************************/

int audio_capture_stop(void)
{
  int ret = OK;

  pthread_mutex_lock(&g_audio_lock);

  if (g_capturing)
    {
      ret = pcm_stream_stop(&g_cap);
      pcm_stream_close(&g_cap);
      g_capturing = false;

      if (ret < 0)
        {
          syslog(LOG_ERR, "audio: capture: stop failed: %d\n", ret);
        }
    }

  pthread_mutex_unlock(&g_audio_lock);
  return ret;
}

/****************************************************************************
 * Name: audio_capture_read
 ****************************************************************************/

int audio_capture_read(uint8_t *buf, size_t len)
{
  return audio_capture_read_timeout((void *)buf, len, 0);
}

/****************************************************************************
 * Name: audio_capture_read_timeout
 ****************************************************************************/

int audio_capture_read_timeout(void *buf, size_t len, int timeout_ms)
{
  size_t got = 0;
  int waited = 0;

  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (!g_capturing)
    {
      return -EAGAIN;
    }

  while (got < len)
    {
      got += ring_get((uint8_t *)buf + got, len - got);
      if (got >= len)
        {
          break;
        }

      if (waited >= timeout_ms)
        {
          break;
        }

      usleep(CAP_POLL_MS * 1000);
      waited += CAP_POLL_MS;
    }

  return (int)got;
}

/****************************************************************************
 * Name: audio_capture_take_word
 ****************************************************************************/

int audio_capture_take_word(int16_t *out, int capacity)
{
  int ret;

  if (out == NULL || capacity <= 0)
    {
      return -EINVAL;
    }

  if (!g_dsp_on)
    {
      return -EAGAIN;
    }

  pthread_mutex_lock(&g_dsp_lock);

  if (!g_word_pending)
    {
      pthread_mutex_unlock(&g_dsp_lock);
      return 0;
    }

  ret = dsp_vad_take_word(out, capacity);

  /* -ENOSPC leaves the utterance in place so the caller can retry with a
   * larger buffer.  Anything else (including success) clears the gate and
   * lets capture_cb() resume feeding.
   */

  if (ret != -ENOSPC)
    {
      g_word_pending = false;
      g_dsp_skipped  = 0;
    }

  pthread_mutex_unlock(&g_dsp_lock);
  return ret;
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
  int ret;

  if (tone_id < 0 || tone_id > 3)
    {
      tone_id = 0;
    }

  freq = g_tone_freq[tone_id];

  /* Square-ish tone with a linear fade-in/out envelope to reduce clicks.
   * (No floating point / math library dependency.)
   */

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

  pthread_mutex_lock(&g_audio_lock);
  ret = play_locked((const uint8_t *)tone, (size_t)TONE_BUF_BYTES);
  pthread_mutex_unlock(&g_audio_lock);

  return ret;
}

/****************************************************************************
 * Name: audio_play_pcm
 ****************************************************************************/

int audio_play_pcm(const uint8_t *pcm, size_t len)
{
  int ret;

  if (!g_ready)
    {
      return -EAGAIN;
    }

  pthread_mutex_lock(&g_audio_lock);
  ret = play_locked(pcm, len);
  pthread_mutex_unlock(&g_audio_lock);

  return ret;
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
