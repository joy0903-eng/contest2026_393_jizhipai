/****************************************************************************
 * apps/examples/desktop_companion/dsp_vad.h
 *
 * Adaptive energy voice-activity detector with slow noise-floor tracking,
 * a pre-roll ring buffer and utterance capture/end-pointing.
 *
 * Ported 1:1 (numerically) from the Arduino reference firmware
 * (voice_chat_src/audio_frontend.h, fe_poll()/fe_trim()).
 *
 * The module is device agnostic: the caller feeds raw PCM in
 * (dsp_vad_feed) and pulls finished utterances out (dsp_vad_take_word).
 * Time advances on the audio clock (exactly VAD_FRAME_MS per processed
 * frame) so no platform timer API is required.
 *
 * Porting notes (vs. Arduino):
 *   - millis() -> internal audio-clock counter s_clock_ms (equivalent
 *     because frames arrive in real time from the audio device).
 *   - the trigger gate is uniformly "max(noise * 1.3, noise + offset)";
 *     the reference firmware's diagnostic fe_current_gate() used 1.5 by
 *     mistake while fe_poll() gated on 1.3. 1.3 is the real behaviour.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_DSP_VAD_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_DSP_VAD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "dsp_mfcc.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * VAD parameters (mirrors Arduino config.h)
 ****************************************************************************/

#define VAD_FRAME_MS        20     /* one analysis hop                      */
#define VAD_SILENCE_MS      700    /* trailing silence that ends a word     */
#define VAD_MAX_WORD_MS     2800   /* hard utterance length limit           */
#define VAD_MIN_WORD_MS     280    /* shorter utterances are discarded      */
#define VAD_PRE_ROLL_MS     200    /* audio kept before the trigger         */
#define VAD_COOLDOWN_MS     500    /* dead time after a finished word       */

#define VAD_NOISE_INIT      600.0f /* initial noise-floor RMS               */
#define VAD_NOISE_ALPHA     0.02f  /* noise tracker step (0.98 / 0.02)      */
#define VAD_NOISE_GUARD     2.0f   /* do not track above noise * guard      */
#define VAD_GATE_RATIO      1.3f   /* relative part of the trigger gate     */
#define VAD_THR_OFFSET_DEF  100    /* absolute part of the trigger gate     */

#define VAD_THR_OFFSET_MIN  0
#define VAD_THR_OFFSET_MAX  3000

#define VAD_PRE_ROLL_SAMPLES  (VAD_PRE_ROLL_MS * DSP_SAMPLE_RATE / 1000) /*  3200 */
#define VAD_MAX_WORD_SAMPLES  (VAD_MAX_WORD_MS * DSP_SAMPLE_RATE / 1000) /* 44800 */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: dsp_vad_init
 *
 * Description:
 *   Allocate the utterance buffer and reset all state. Must be called
 *   before any dsp_vad_feed().
 *
 * Returned Value:
 *   OK (0) on success; -ENOMEM if the utterance buffer cannot be allocated.
 *
 ****************************************************************************/

int dsp_vad_init(void);

/****************************************************************************
 * Name: dsp_vad_deinit
 *
 * Description:
 *   Release the utterance buffer and stop the detector.
 *
 ****************************************************************************/

void dsp_vad_deinit(void);

/****************************************************************************
 * Name: dsp_vad_reset
 *
 * Description:
 *   Clear the capture state (keeps the learned noise floor) without
 *   releasing memory. Returns OK.
 *
 ****************************************************************************/

int dsp_vad_reset(void);

/****************************************************************************
 * Name: dsp_vad_feed
 *
 * Description:
 *   Push PCM samples through the detector. Any number of samples may be
 *   fed per call; a partial frame is buffered until it is complete.
 *
 * Input Parameters:
 *   pcm      - mono int16 samples at DSP_SAMPLE_RATE.
 *   nsamples - number of samples (>= 0).
 *
 * Returned Value:
 *   > 0 : number of samples of a finished utterance waiting in the
 *         internal word buffer (retrieve it with dsp_vad_take_word()).
 *     0 : nothing finished yet.
 *   < 0 : negated errno (-EINVAL / -EPERM / -ENOSPC).
 *
 *   The internal word buffer holds one utterance at a time: if the caller
 *   does not take it before the next one completes, it is overwritten.
 *
 ****************************************************************************/

int dsp_vad_feed(const int16_t *pcm, int nsamples);

/****************************************************************************
 * Name: dsp_vad_is_speech
 *
 * Description:
 *   Returns 1 while an utterance is being captured (CAPTURING state),
 *   0 while listening for a trigger.
 *
 ****************************************************************************/

int dsp_vad_is_speech(void);

/****************************************************************************
 * Name: dsp_vad_word_ready
 *
 * Description:
 *   Returns 1 when a finished (and silence-trimmed) utterance is pending.
 *
 ****************************************************************************/

int dsp_vad_word_ready(void);

/****************************************************************************
 * Name: dsp_vad_take_word
 *
 * Description:
 *   Copy the pending utterance out and clear the pending flag.
 *
 * Input Parameters:
 *   out      - destination buffer (may be NULL to just query/clear).
 *   capacity - capacity of out, in samples.
 *
 * Returned Value:
 *   Number of samples copied (0 if nothing pending), or -ENOSPC if the
 *   utterance does not fit in out (nothing is copied in that case).
 *
 ****************************************************************************/

int dsp_vad_take_word(int16_t *out, int capacity);

/****************************************************************************
 * Name: dsp_vad_word
 *
 * Description:
 *   Zero-copy accessor: returns a pointer to the pending utterance (valid
 *   until the next dsp_vad_feed()) and, if out_samples is non-NULL, its
 *   length in samples. NULL when nothing is pending.
 *
 ****************************************************************************/

const int16_t *dsp_vad_word(int *out_samples);

/****************************************************************************
 * Name: dsp_vad_trim
 *
 * Description:
 *   In-place head/tail silence trim of one utterance using a relative
 *   energy threshold (18% of the peak frame energy) with a 3-frame margin.
 *   Prevents long noisy recordings from becoming geometrically
 *   incomparable with short templates under a banded DTW.
 *
 * Input Parameters:
 *   pcm      - utterance samples (modified in place).
 *   nsamples - length of pcm, in samples.
 *
 * Returned Value:
 *   Trimmed length in samples (<= nsamples).
 *
 ****************************************************************************/

int dsp_vad_trim(int16_t *pcm, int nsamples);

/****************************************************************************
 * Name: dsp_vad_noise / dsp_vad_gate
 *
 * Description:
 *   Current tracked noise-floor RMS and the derived trigger gate
 *   max(noise * VAD_GATE_RATIO, noise + thr_offset).
 *
 ****************************************************************************/

float dsp_vad_noise(void);
float dsp_vad_gate(void);

/****************************************************************************
 * Name: dsp_vad_set_thr_offset / dsp_vad_get_thr_offset
 *
 * Description:
 *   Absolute margin added to the noise floor when forming the gate.
 *   Smaller = more sensitive. Clamped to [0, 3000].
 *
 ****************************************************************************/

void dsp_vad_set_thr_offset(int offset);
int  dsp_vad_get_thr_offset(void);

/****************************************************************************
 * Name: dsp_vad_frames / dsp_vad_last_energy / dsp_vad_peak
 *
 * Description:
 *   Diagnostic counters: processed frame count, last frame mean |x|, and
 *   the peak frame energy since the last dsp_vad_peak() call (reading it
 *   clears it).
 *
 ****************************************************************************/

uint32_t dsp_vad_frames(void);
int dsp_vad_last_energy(void);
int dsp_vad_peak(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_DSP_VAD_H */
