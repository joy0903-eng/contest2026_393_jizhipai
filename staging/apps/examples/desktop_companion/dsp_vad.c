/****************************************************************************
 * apps/examples/desktop_companion/dsp_vad.c
 *
 * Adaptive VAD implementation. See dsp_vad.h.
 *
 * Numerically identical to the Arduino reference fe_poll() state machine:
 *   - 20 ms frames (FE_HOP = 320 samples); a partial frame is accumulated
 *     across calls so a caller may feed any number of samples.
 *   - mean |x| per frame; noise floor tracked with
 *     noise = 0.98 * noise + 0.02 * energy while energy < 2 * noise.
 *   - trigger gate = max(noise * 1.3, noise + thr_offset).
 *   - on trigger, the 200 ms pre-roll ring buffer is flushed into the word
 *     buffer; the word ends after 700 ms of silence or 2800 ms total; a
 *     500 ms cooldown follows.
 *   - head/tail silence trim (18% of peak frame energy, 3-frame margin).
 *
 * Wall-clock replacement: the reference used millis() for the cooldown.
 * Here the audio clock is used instead (s_clock_ms += VAD_FRAME_MS per
 * processed frame), which is equivalent because the frames arrive in real
 * time from the capture device, and keeps the module free of any platform
 * timer API.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "dsp_mfcc.h"
#include "dsp_vad.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef OK
#  define OK 0
#endif

#define VAD_TRIM_PEAK_RATIO  0.18f   /* relative trim threshold            */
#define VAD_TRIM_MARGIN      3       /* frames kept around the speech core  */
#define VAD_TRIM_MIN_FRAMES  8       /* never trim below 8 frames (160 ms)  */
#define VAD_TRIM_MIN_FRAMES_TOTAL 6  /* do not trim such short utterances   */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 200 ms pre-roll ring buffer: 3200 samples = 6.4 KB BSS. */
static int16_t  s_preroll[VAD_PRE_ROLL_SAMPLES];
static int      s_preroll_w = 0;      /* ring write pointer                 */
static int      s_preroll_n = 0;      /* valid samples in the ring         */

/* Utterance buffer: 44800 samples = 89.6 KB, heap allocated in init. */
static int16_t *s_word      = NULL;
static int      s_word_len  = 0;
static int      s_word_ready = 0;

/* Partial-frame accumulator (exactly one hop). */
static int16_t  s_acc[FE_HOP];
static int      s_acc_len = 0;

/* Detector state. */
static int      s_capturing  = 0;
static float    s_noise      = VAD_NOISE_INIT;
static int      s_thr_offset = VAD_THR_OFFSET_DEF;
static int      s_silence_ms = 0;
static int      s_word_ms    = 0;
static int      s_clock_ms   = 0;     /* audio clock (1 frame = 20 ms)      */
static int      s_cooldown_until = 0;

/* Diagnostics. */
static uint32_t s_frames      = 0;
static int      s_last_energy = 0;
static int      s_peak        = 0;

static int      s_vad_ready = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: vad_gate
 *
 * Description:
 *   Current trigger threshold: max(noise * VAD_GATE_RATIO,
 *   noise + thr_offset).
 *
 ****************************************************************************/

static float vad_gate(void)
{
  float rel = s_noise * VAD_GATE_RATIO;
  float abs = s_noise + (float)s_thr_offset;

  return (rel > abs) ? rel : abs;
}

/****************************************************************************
 * Name: vad_preroll_push
 *
 * Description:
 *   Push one sample into the pre-roll ring buffer.
 *
 ****************************************************************************/

static void vad_preroll_push(int16_t sample)
{
  s_preroll[s_preroll_w] = sample;
  s_preroll_w++;

  if (s_preroll_w >= VAD_PRE_ROLL_SAMPLES)
    {
      s_preroll_w = 0;
    }

  if (s_preroll_n < VAD_PRE_ROLL_SAMPLES)
    {
      s_preroll_n++;
    }
}

/****************************************************************************
 * Name: vad_start_capture
 *
 * Description:
 *   Flush the pre-roll ring buffer (oldest -> newest) into the word buffer
 *   and enter the CAPTURING state.
 *
 ****************************************************************************/

static void vad_start_capture(void)
{
  int cap   = VAD_PRE_ROLL_SAMPLES;
  int count = (s_preroll_n < cap) ? s_preroll_n : cap;
  int start;
  int i;

  start = (s_preroll_w + cap - count) % cap;

  s_word_len = 0;

  for (i = 0; i < count; i++)
    {
      s_word[s_word_len++] = s_preroll[(start + i) % cap];
    }

  s_capturing  = 1;
  s_silence_ms = 0;
  s_word_ms    = 0;
}

/****************************************************************************
 * Name: vad_finish_capture
 *
 * Description:
 *   Leave the CAPTURING state, apply the cooldown and - if the utterance is
 *   long enough - trim it and publish it.
 *
 ****************************************************************************/

static void vad_finish_capture(void)
{
  s_capturing      = 0;
  s_cooldown_until = s_clock_ms + VAD_COOLDOWN_MS;
  s_preroll_n      = 0;
  s_preroll_w      = 0;

  if (s_word_ms >= VAD_MIN_WORD_MS)
    {
      s_word_len  = dsp_vad_trim(s_word, s_word_len);
      s_word_ready = 1;
    }
  else
    {
      s_word_len = 0;
    }
}

/****************************************************************************
 * Name: vad_process_frame
 *
 * Description:
 *   Advance the state machine by exactly one 20 ms frame.
 *
 ****************************************************************************/

static void vad_process_frame(const int16_t *fp)
{
  uint32_t acc = 0;
  float    energy;
  float    thr;
  int      i;

  for (i = 0; i < FE_HOP; i++)
    {
      int32_t v = (int32_t)fp[i];

      acc += (uint32_t)((v < 0) ? -v : v);
    }

  energy = (float)acc / (float)FE_HOP;
  thr    = vad_gate();

  s_frames++;
  s_clock_ms   += VAD_FRAME_MS;
  s_last_energy = (int)energy;

  if ((int)energy > s_peak)
    {
      s_peak = (int)energy;
    }

  if (s_capturing == 0)
    {
      /* Idle: track the noise floor and keep the pre-roll warm. */

      if (energy < s_noise * VAD_NOISE_GUARD)
        {
          s_noise = (1.0f - VAD_NOISE_ALPHA) * s_noise +
                    VAD_NOISE_ALPHA * energy;
        }

      for (i = 0; i < FE_HOP; i++)
        {
          vad_preroll_push(fp[i]);
        }

      if (s_clock_ms >= s_cooldown_until && energy > thr)
        {
          vad_start_capture();
        }
    }
  else
    {
      /* Capturing: append the frame, then test the end conditions. */

      if (s_word_len + FE_HOP <= VAD_MAX_WORD_SAMPLES)
        {
          memcpy(&s_word[s_word_len], fp, FE_HOP * sizeof(int16_t));
          s_word_len += FE_HOP;
        }

      s_word_ms += VAD_FRAME_MS;

      if (energy > thr)
        {
          s_silence_ms = 0;
        }
      else
        {
          s_silence_ms += VAD_FRAME_MS;
        }

      if (s_silence_ms >= VAD_SILENCE_MS || s_word_ms >= VAD_MAX_WORD_MS)
        {
          vad_finish_capture();
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: dsp_vad_init
 ****************************************************************************/

int dsp_vad_init(void)
{
  if (s_vad_ready)
    {
      return OK;
    }

  if (s_word == NULL)
    {
      s_word = (int16_t *)malloc(VAD_MAX_WORD_SAMPLES * sizeof(int16_t));

      if (s_word == NULL)
        {
          syslog(LOG_ERR, "dsp_vad: cannot allocate %d samples\n",
                 VAD_MAX_WORD_SAMPLES);
          return -ENOMEM;
        }
    }

  memset(s_word, 0, VAD_MAX_WORD_SAMPLES * sizeof(int16_t));

  s_preroll_w      = 0;
  s_preroll_n      = 0;
  s_word_len       = 0;
  s_word_ready     = 0;
  s_acc_len        = 0;
  s_capturing      = 0;
  s_noise          = VAD_NOISE_INIT;
  s_thr_offset     = VAD_THR_OFFSET_DEF;
  s_silence_ms     = 0;
  s_word_ms        = 0;
  s_clock_ms       = 0;
  s_cooldown_until = 0;
  s_frames         = 0;
  s_last_energy    = 0;
  s_peak           = 0;
  s_vad_ready      = 1;

  syslog(LOG_INFO,
         "dsp_vad: init (frame=%dms silence=%dms max=%dms preroll=%d)\n",
         VAD_FRAME_MS, VAD_SILENCE_MS, VAD_MAX_WORD_MS, VAD_PRE_ROLL_MS);
  return OK;
}

/****************************************************************************
 * Name: dsp_vad_deinit
 ****************************************************************************/

void dsp_vad_deinit(void)
{
  if (s_word != NULL)
    {
      free(s_word);
      s_word = NULL;
    }

  s_vad_ready = 0;
}

/****************************************************************************
 * Name: dsp_vad_reset
 ****************************************************************************/

int dsp_vad_reset(void)
{
  s_preroll_w      = 0;
  s_preroll_n      = 0;
  s_word_len       = 0;
  s_word_ready     = 0;
  s_acc_len        = 0;
  s_capturing      = 0;
  s_silence_ms     = 0;
  s_word_ms        = 0;
  s_clock_ms       = 0;
  s_cooldown_until = 0;
  return OK;
}

/****************************************************************************
 * Name: dsp_vad_feed
 ****************************************************************************/

int dsp_vad_feed(const int16_t *pcm, int nsamples)
{
  int off = 0;

  if (s_vad_ready == 0)
    {
      return -EPERM;
    }

  if (pcm == NULL)
    {
      return -EINVAL;
    }

  if (nsamples <= 0)
    {
      return s_word_ready ? s_word_len : 0;
    }

  while (off < nsamples)
    {
      int space = FE_HOP - s_acc_len;
      int take  = nsamples - off;

      if (take > space)
        {
          take = space;
        }

      memcpy(&s_acc[s_acc_len], &pcm[off], (size_t)take * sizeof(int16_t));
      s_acc_len += take;
      off       += take;

      if (s_acc_len == FE_HOP)
        {
          vad_process_frame(s_acc);
          s_acc_len = 0;
        }
    }

  return s_word_ready ? s_word_len : 0;
}

/****************************************************************************
 * Name: dsp_vad_is_speech
 ****************************************************************************/

int dsp_vad_is_speech(void)
{
  return s_capturing ? 1 : 0;
}

/****************************************************************************
 * Name: dsp_vad_word_ready
 ****************************************************************************/

int dsp_vad_word_ready(void)
{
  return s_word_ready ? 1 : 0;
}

/****************************************************************************
 * Name: dsp_vad_take_word
 ****************************************************************************/

int dsp_vad_take_word(int16_t *out, int capacity)
{
  if (s_word_ready == 0)
    {
      return 0;
    }

  if (out == NULL)
    {
      s_word_ready = 0;
      return s_word_len;
    }

  if (capacity < s_word_len)
    {
      return -ENOSPC;
    }

  memcpy(out, s_word, (size_t)s_word_len * sizeof(int16_t));
  s_word_ready = 0;
  return s_word_len;
}

/****************************************************************************
 * Name: dsp_vad_word
 ****************************************************************************/

const int16_t *dsp_vad_word(int *out_samples)
{
  if (s_word_ready == 0)
    {
      if (out_samples != NULL)
        {
          *out_samples = 0;
        }

      return NULL;
    }

  if (out_samples != NULL)
    {
      *out_samples = s_word_len;
    }

  return s_word;
}

/****************************************************************************
 * Name: dsp_vad_trim
 *
 * Description:
 *   Head/tail silence trim. Port of the reference fe_trim(): frame mean
 *   |x| (FE_HOP samples per frame), threshold = 18% of the peak frame,
 *   3-frame margin on both sides, bail out if the result would be shorter
 *   than 8 frames or not actually shorter.
 *
 ****************************************************************************/

int dsp_vad_trim(int16_t *pcm, int nsamples)
{
  static float s_es[KWS_MAX_FRAMES + 4];

  int   nf;
  int   f;
  int   i;
  float mx  = 0.0f;
  float thr;
  int   a;
  int   b;
  int   na;

  if (pcm == NULL)
    {
      return 0;
    }

  nf = nsamples / FE_HOP;

  if (nf < VAD_TRIM_MIN_FRAMES_TOTAL)
    {
      return nsamples;
    }

  if (nf > KWS_MAX_FRAMES + 4)
    {
      return nsamples;
    }

  for (f = 0; f < nf; f++)
    {
      uint32_t acc = 0;

      for (i = 0; i < FE_HOP; i++)
        {
          int32_t v = (int32_t)pcm[f * FE_HOP + i];

          acc += (uint32_t)((v < 0) ? -v : v);
        }

      s_es[f] = (float)acc / (float)FE_HOP;

      if (s_es[f] > mx)
        {
          mx = s_es[f];
        }
    }

  if (mx < 1.0f)
    {
      return nsamples;
    }

  thr = mx * VAD_TRIM_PEAK_RATIO;
  a   = 0;
  b   = nf - 1;

  while (a < b && s_es[a] < thr)
    {
      a++;
    }

  while (b > a && s_es[b] < thr)
    {
      b--;
    }

  if (a > VAD_TRIM_MARGIN)
    {
      a -= VAD_TRIM_MARGIN;
    }
  else
    {
      a = 0;
    }

  if (b + VAD_TRIM_MARGIN < nf - 1)
    {
      b += VAD_TRIM_MARGIN;
    }
  else
    {
      b = nf - 1;
    }

  na = (b + 1 - a) * FE_HOP;

  if (na < FE_HOP * VAD_TRIM_MIN_FRAMES || na >= nsamples)
    {
      return nsamples;
    }

  if (a > 0)
    {
      memmove(pcm, &pcm[a * FE_HOP], (size_t)na * sizeof(int16_t));
    }

  return na;
}

/****************************************************************************
 * Name: dsp_vad_noise
 ****************************************************************************/

float dsp_vad_noise(void)
{
  return s_noise;
}

/****************************************************************************
 * Name: dsp_vad_gate
 ****************************************************************************/

float dsp_vad_gate(void)
{
  return vad_gate();
}

/****************************************************************************
 * Name: dsp_vad_set_thr_offset
 ****************************************************************************/

void dsp_vad_set_thr_offset(int offset)
{
  if (offset < VAD_THR_OFFSET_MIN)
    {
      offset = VAD_THR_OFFSET_MIN;
    }

  if (offset > VAD_THR_OFFSET_MAX)
    {
      offset = VAD_THR_OFFSET_MAX;
    }

  s_thr_offset = offset;
}

/****************************************************************************
 * Name: dsp_vad_get_thr_offset
 ****************************************************************************/

int dsp_vad_get_thr_offset(void)
{
  return s_thr_offset;
}

/****************************************************************************
 * Name: dsp_vad_frames
 ****************************************************************************/

uint32_t dsp_vad_frames(void)
{
  return s_frames;
}

/****************************************************************************
 * Name: dsp_vad_last_energy
 ****************************************************************************/

int dsp_vad_last_energy(void)
{
  return s_last_energy;
}

/****************************************************************************
 * Name: dsp_vad_peak
 ****************************************************************************/

int dsp_vad_peak(void)
{
  int p = s_peak;

  s_peak = 0;
  return p;
}
