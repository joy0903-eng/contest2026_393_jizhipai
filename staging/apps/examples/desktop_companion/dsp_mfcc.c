/****************************************************************************
 * apps/examples/desktop_companion/dsp_mfcc.c
 *
 * Offline speech front-end implementation. See dsp_mfcc.h.
 *
 * Numerically identical to the Arduino reference implementation:
 *   pre-emphasis (0.97) -> Hann(400) -> zero-pad to 512 -> radix-2 FFT
 *   -> power spectrum -> 24 triangular Mel filters -> natural log
 *   -> keep bands 0,2,...,22 (12 dims) -> CMN -> *100 -> int16.
 *
 * Only the transcendental functions differ in flavour: tables and Mel logs
 * are evaluated in double precision (cos/sin/log/log10/pow) instead of the
 * float variants, because <math.h> float entry points are not guaranteed on
 * every NuttX libm configuration. The stored values are float, so the
 * difference is < 1e-7 - far below the 0.01 quantum of the int16 feature.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include "dsp_mfcc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#define MFCC_MEL_BINS   (FE_FFT_SIZE / 2 + 1)   /* 257 spectral bins       */
#define MFCC_FFT_BITS   9                       /* 512 = 2^9               */

#ifndef OK
#  define OK 0
#endif

/****************************************************************************
 * Private Data
 *
 * Total BSS footprint: ~34 KB (dominated by the 24 x 257 Mel matrix).
 ****************************************************************************/

static float    s_hann[FE_FRAME_LEN];                    /* 1.6 KB  */
static float    s_fft_re[FE_FFT_SIZE];                   /* 2.0 KB  */
static float    s_fft_im[FE_FFT_SIZE];                   /* 2.0 KB  */
static float    s_tw_re[FE_FFT_SIZE / 2];                /* 1.0 KB  */
static float    s_tw_im[FE_FFT_SIZE / 2];                /* 1.0 KB  */
static uint16_t s_bitrev[FE_FFT_SIZE];                   /* 1.0 KB  */
static float    s_mel_w[FE_MEL_BANDS][MFCC_MEL_BINS];    /* 24.7 KB */
static uint16_t s_mel_start[FE_MEL_BANDS];
static uint16_t s_mel_end[FE_MEL_BANDS];

/* Float log-Mel workspace: the reference implementation keeps full float
 * precision until CMN and only quantizes at the very end, so the intermediate
 * matrix MUST NOT be the int16 output buffer. 140 * 12 * 4 = 6.7 KB. */

static float    s_lmel[KWS_MAX_FRAMES][FEAT_DIM];
static float    s_pw[MFCC_MEL_BINS];
static int      s_mfcc_ready = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mfcc_hz_to_mel / mfcc_mel_to_hz
 *
 * Description:
 *   Standard Slaney Mel scale: mel(f) = 2595 * log10(1 + f / 700).
 *
 ****************************************************************************/

static double mfcc_hz_to_mel(double hz)
{
  return 2595.0 * log10(1.0 + hz / 700.0);
}

static double mfcc_mel_to_hz(double mel)
{
  return 700.0 * (pow(10.0, mel / 2595.0) - 1.0);
}

/****************************************************************************
 * Name: mfcc_tables_init
 *
 * Description:
 *   Build the Hann window, the bit-reversal and twiddle tables and the
 *   triangular Mel filter bank (with the per-filter [start, end] bin range
 *   used to keep the filter application cheap).
 *
 ****************************************************************************/

static void mfcc_tables_init(void)
{
  double pts[FE_MEL_BANDS + 2];
  double mlo;
  double mhi;
  int    i;
  int    b;
  int    k;

  /* Hann window over the 400-sample analysis frame. */

  for (i = 0; i < FE_FRAME_LEN; i++)
    {
      s_hann[i] = (float)(0.5 * (1.0 - cos(2.0 * M_PI * (double)i /
                                           (double)(FE_FRAME_LEN - 1))));
    }

  /* Bit-reversal permutation for the 512-point FFT. */

  for (i = 0; i < FE_FFT_SIZE; i++)
    {
      int r = 0;
      int v = i;
      int bit;

      for (bit = 0; bit < MFCC_FFT_BITS; bit++)
        {
          r = (r << 1) | (v & 1);
          v >>= 1;
        }

      s_bitrev[i] = (uint16_t)r;
    }

  /* Twiddle factors W_N^k, k = 0 .. N/2 - 1. */

  for (k = 0; k < FE_FFT_SIZE / 2; k++)
    {
      double a = -2.0 * M_PI * (double)k / (double)FE_FFT_SIZE;

      s_tw_re[k] = (float)cos(a);
      s_tw_im[k] = (float)sin(a);
    }

  /* Mel filter bank: FE_MEL_BANDS + 2 equally spaced points on the Mel
   * axis, mapped back to Hz. */

  mlo = mfcc_hz_to_mel((double)FE_MIN_FREQ_HZ);
  mhi = mfcc_hz_to_mel((double)FE_MAX_FREQ_HZ);

  for (b = 0; b < FE_MEL_BANDS + 2; b++)
    {
      pts[b] = mfcc_mel_to_hz(mlo + (mhi - mlo) * (double)b /
                              (double)(FE_MEL_BANDS + 1));
    }

  for (b = 0; b < FE_MEL_BANDS; b++)
    {
      s_mel_start[b] = (uint16_t)MFCC_MEL_BINS;
      s_mel_end[b]   = 0;

      for (k = 0; k < MFCC_MEL_BINS; k++)
        {
          double f = (double)k * (double)DSP_SAMPLE_RATE / (double)FE_FFT_SIZE;
          float  w = 0.0f;

          if (f >= pts[b] && f <= pts[b + 2])
            {
              if (f <= pts[b + 1])
                {
                  w = (float)((f - pts[b]) / (pts[b + 1] - pts[b] + 1e-9));
                }
              else
                {
                  w = (float)((pts[b + 2] - f) /
                              (pts[b + 2] - pts[b + 1] + 1e-9));
                }
            }

          s_mel_w[b][k] = w;

          if (w > 0.0f)
            {
              if (k < (int)s_mel_start[b])
                {
                  s_mel_start[b] = (uint16_t)k;
                }

              if (k > (int)s_mel_end[b])
                {
                  s_mel_end[b] = (uint16_t)k;
                }
            }
        }
    }
}

/****************************************************************************
 * Name: mfcc_fft
 *
 * Description:
 *   In-place radix-2 iterative FFT of s_fft_re / s_fft_im (FE_FFT_SIZE
 *   points). Decimation-in-time with the pre-computed bit-reversal table.
 *
 ****************************************************************************/

static void mfcc_fft(void)
{
  int i;
  int len;

  for (i = 0; i < FE_FFT_SIZE; i++)
    {
      int j = (int)s_bitrev[i];

      if (i < j)
        {
          float t = s_fft_re[i];

          s_fft_re[i] = s_fft_re[j];
          s_fft_re[j] = t;
          t           = s_fft_im[i];
          s_fft_im[i] = s_fft_im[j];
          s_fft_im[j] = t;
        }
    }

  for (len = 2; len <= FE_FFT_SIZE; len <<= 1)
    {
      int half = len >> 1;
      int step = FE_FFT_SIZE / len;
      int blk;

      for (blk = 0; blk < FE_FFT_SIZE; blk += len)
        {
          int j;

          for (j = 0; j < half; j++)
            {
              int   w  = j * step;
              int   ia = blk + j;
              int   ib = ia + half;
              float xr = s_fft_re[ib] * s_tw_re[w] - s_fft_im[ib] * s_tw_im[w];
              float xi = s_fft_re[ib] * s_tw_im[w] + s_fft_im[ib] * s_tw_re[w];

              s_fft_re[ib] = s_fft_re[ia] - xr;
              s_fft_im[ib] = s_fft_im[ia] - xi;
              s_fft_re[ia] += xr;
              s_fft_im[ia] += xi;
            }
        }
    }
}

/****************************************************************************
 * Name: mfcc_quantize
 *
 * Description:
 *   Clamp and round a float feature to int16 (lroundf replacement: the
 *   float lround family is not guaranteed on every NuttX libm).
 *
 ****************************************************************************/

static int16_t mfcc_quantize(float v)
{
  if (v > 32767.0f)
    {
      v = 32767.0f;
    }
  else if (v < -32768.0f)
    {
      v = -32768.0f;
    }

  if (v >= 0.0f)
    {
      return (int16_t)(v + 0.5f);
    }

  return (int16_t)(v - 0.5f);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: dsp_mfcc_init
 ****************************************************************************/

int dsp_mfcc_init(void)
{
  if (s_mfcc_ready)
    {
      return OK;
    }

  mfcc_tables_init();
  s_mfcc_ready = 1;

  syslog(LOG_INFO,
         "dsp_mfcc: tables built (fft=%d frame=%d hop=%d mel=%d dim=%d)\n",
         FE_FFT_SIZE, FE_FRAME_LEN, FE_HOP, FE_MEL_BANDS, FEAT_DIM);
  return OK;
}

/****************************************************************************
 * Name: dsp_mfcc_deinit
 ****************************************************************************/

void dsp_mfcc_deinit(void)
{
  s_mfcc_ready = 0;
}

/****************************************************************************
 * Name: dsp_mfcc_frame_count
 ****************************************************************************/

int dsp_mfcc_frame_count(int nsamples)
{
  int frames;

  if (nsamples < FE_FRAME_LEN)
    {
      return 0;
    }

  frames = (nsamples - FE_FRAME_LEN) / FE_HOP + 1;

  if (frames > KWS_MAX_FRAMES)
    {
      frames = KWS_MAX_FRAMES;
    }

  return frames;
}

/****************************************************************************
 * Name: dsp_mfcc_compute
 ****************************************************************************/

int dsp_mfcc_compute(const int16_t *pcm, int nsamples,
                     int16_t *feat, int max_frames, int *frames_out)
{
  double       mean[FEAT_DIM];
  int          frames;
  int          f;
  int          d;

  if (frames_out != NULL)
    {
      *frames_out = 0;
    }

  if (pcm == NULL || feat == NULL || max_frames <= 0)
    {
      return -EINVAL;
    }

  if (s_mfcc_ready == 0)
    {
      dsp_mfcc_init();
    }

  if (nsamples < FE_FRAME_LEN)
    {
      return -EINVAL;
    }

  frames = dsp_mfcc_frame_count(nsamples);

  if (frames > max_frames)
    {
      frames = max_frames;
    }

  if (frames < 1)
    {
      return -EINVAL;
    }

  /* ---- per-frame: pre-emphasis + Hann + FFT + power spectrum + Mel ---- */

  for (f = 0; f < frames; f++)
    {
      int   base = f * FE_HOP;
      float prev;
      int   i;
      int   b;
      int   k;

      prev = (base > 0) ? (float)pcm[base - 1] : (float)pcm[base];

      for (i = 0; i < FE_FRAME_LEN; i++)
        {
          float x = (float)pcm[base + i];
          float e = x - FE_PREEMPH * prev;

          prev        = x;
          s_fft_re[i] = e * s_hann[i];
          s_fft_im[i] = 0.0f;
        }

      for (i = FE_FRAME_LEN; i < FE_FFT_SIZE; i++)
        {
          s_fft_re[i] = 0.0f;
          s_fft_im[i] = 0.0f;
        }

      mfcc_fft();

      for (k = 0; k < MFCC_MEL_BINS; k++)
        {
          s_pw[k] = s_fft_re[k] * s_fft_re[k] + s_fft_im[k] * s_fft_im[k];
        }

      for (b = 0; b < FE_MEL_BANDS; b++)
        {
          float sum = 0.0f;

          for (k = (int)s_mel_start[b]; k <= (int)s_mel_end[b]; k++)
            {
              sum += s_pw[k] * s_mel_w[b][k];
            }

          if ((b & 1) == 0)
            {
              /* Keep bands 0,2,...,22 -> FEAT_DIM = 12 dims spanning the
               * full 80 Hz .. 3700 Hz range. */
              s_lmel[f][b >> 1] = (float)log((double)sum + 1e-6);
            }
        }
    }

  /* ---- CMN: subtract the per-dimension utterance mean ---- */

  for (d = 0; d < FEAT_DIM; d++)
    {
      mean[d] = 0.0;
    }

  for (f = 0; f < frames; f++)
    {
      for (d = 0; d < FEAT_DIM; d++)
        {
          mean[d] += (double)s_lmel[f][d];
        }
    }

  for (d = 0; d < FEAT_DIM; d++)
    {
      mean[d] /= (double)frames;
    }

  for (f = 0; f < frames; f++)
    {
      for (d = 0; d < FEAT_DIM; d++)
        {
          float v = (s_lmel[f][d] - (float)mean[d]) * FEAT_SCALE;

          feat[f * FEAT_DIM + d] = mfcc_quantize(v);
        }
    }

  if (frames_out != NULL)
    {
      *frames_out = frames;
    }

  return OK;
}
