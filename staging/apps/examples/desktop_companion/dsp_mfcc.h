/****************************************************************************
 * apps/examples/desktop_companion/dsp_mfcc.h
 *
 * Offline speech front-end: pre-emphasis + Hann window + 512-point FFT +
 * 24-channel Mel log-spectrum + per-word CMN, quantized to int16.
 *
 * Ported 1:1 (numerically) from the Arduino reference firmware
 * (voice_chat_src/audio_frontend.h) that was validated on real hardware with
 * the wake word "ni hao, openvela".
 *
 * This header is ALSO the shared numeric contract for the whole DSP/KWS
 * stack (frame geometry, feature dimension, DTW limits) so that dsp_vad.h
 * and kws_engine.h do not have to duplicate constants. It intentionally
 * contains no platform-specific include.
 *
 * Design notes:
 *   - Pure feed-in / compute-out API: the caller owns the audio device.
 *   - No Arduino, ESP-IDF or C++ dependency; C99 + <math.h> only.
 *   - All tables live in BSS (~34 KB) and are built once by dsp_mfcc_init().
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_DSP_MFCC_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_DSP_MFCC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Shared DSP parameters (mirrors Arduino config.h - DO NOT CHANGE BLINDLY:
 * the empirically calibrated DTW threshold depends on them)
 ****************************************************************************/

#define DSP_SAMPLE_RATE    16000  /* 16 kHz mono                            */
#define FE_FRAME_LEN       400    /* 25 ms analysis window                  */
#define FE_FFT_SIZE        512    /* zero-padded FFT size                   */
#define FE_HOP             320    /* 20 ms hop -> 50 frames/s               */
#define FE_MEL_BANDS       24     /* triangular Mel filters                 */
#define FE_PREEMPH         0.97f  /* first-order pre-emphasis coefficient   */
#define FE_MIN_FREQ_HZ     80.0f  /* lowest Mel filter edge                 */
#define FE_MAX_FREQ_HZ     3700.0f /* highest Mel filter edge               */
#define FEAT_DIM           12     /* kept bands: 0,2,4,...,22               */
#define FEAT_SCALE         100    /* float log-energy -> int16 scale        */

/* Maximum frames per utterance; bounds every feature buffer and the DTW
 * cost matrix. 140 frames = 2.8 s at a 20 ms hop. */
#define KWS_MAX_FRAMES     140

/* Sakoe-Chiba band for the banded DTW (|i - j| <= KWS_DTW_BAND). */
#define KWS_DTW_BAND       40

/* Bytes required by one utterance feature matrix. */
#define DSP_FEAT_BYTES(max_frames) ((max_frames) * FEAT_DIM * (int)sizeof(int16_t))

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: dsp_mfcc_init
 *
 * Description:
 *   Build the Hann window, FFT twiddle/bit-reversal tables and the Mel
 *   filter bank. Idempotent: repeated calls are free.
 *
 * Returned Value:
 *   OK (0) on success; negated errno on failure (never fails in practice).
 *
 ****************************************************************************/

int dsp_mfcc_init(void);

/****************************************************************************
 * Name: dsp_mfcc_deinit
 *
 * Description:
 *   Drop the built tables so the next dsp_mfcc_init() rebuilds them.
 *
 ****************************************************************************/

void dsp_mfcc_deinit(void);

/****************************************************************************
 * Name: dsp_mfcc_frame_count
 *
 * Description:
 *   Number of whole analysis frames produced by nsamples PCM samples.
 *
 * Returned Value:
 *   Frame count (>= 0), already clamped to KWS_MAX_FRAMES.
 *
 ****************************************************************************/

int dsp_mfcc_frame_count(int nsamples);

/****************************************************************************
 * Name: dsp_mfcc_compute
 *
 * Description:
 *   Extract the CMN-normalized int16 Mel feature matrix of one utterance.
 *
 * Input Parameters:
 *   pcm        - mono 16 kHz int16 samples (at least FE_FRAME_LEN).
 *   nsamples   - number of samples available in pcm.
 *   feat       - output buffer, max_frames * FEAT_DIM int16 entries,
 *                laid out as feat[frame * FEAT_DIM + dim].
 *   max_frames - capacity of feat, in frames.
 *   frames_out - optional; receives the number of frames written.
 *
 * Returned Value:
 *   OK (0) on success; -EINVAL on bad arguments or a too-short utterance.
 *
 ****************************************************************************/

int dsp_mfcc_compute(const int16_t *pcm, int nsamples,
                     int16_t *feat, int max_frames, int *frames_out);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_DSP_MFCC_H */
