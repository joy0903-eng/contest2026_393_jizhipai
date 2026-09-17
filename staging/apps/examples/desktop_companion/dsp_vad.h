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
 * Concurrency and lifecycle invariants
 *
 * These are contractual, not incidental.  They were established together
 * with the owner of the audio capture thread; breaking one of them does not
 * produce a crash, it produces occasional mis-recognition, which is much
 * harder to diagnose.
 *
 *   INV-1  Single consumer.  dsp_vad_trim(), dsp_mfcc_compute() and every
 *          kws_* entry point use module-static scratch areas (no heap
 *          allocation on the hot path), so at most one thread at a time may
 *          execute them.
 *
 *   INV-2  dsp_vad_feed() must not run concurrently with any of the above.
 *          feed() reaches dsp_vad_trim() through
 *          feed -> vad_process_frame -> vad_finish_capture -> dsp_vad_trim
 *          and therefore shares the same scratch.  A producer that keeps
 *          feeding while a consumer is classifying is racy.
 *
 *   INV-3  Utterances are delivered already trimmed.  dsp_vad_trim() is
 *          applied inside vad_finish_capture() before the ready flag is
 *          set, and the ready flag is set in exactly that one place.  Do
 *          NOT trim again on the consumer side: the second pass re-applies
 *          the same 18% threshold and eats the deliberate 3-frame (60 ms)
 *          margin, shortening the first/last syllable and inflating the DTW
 *          distance.
 *
 *   INV-4  Dereferencing is fail-closed across deinit.  dsp_vad_deinit()
 *          clears the endpointing state before releasing "ready", so after
 *          deinit every accessor (dsp_vad_word(), dsp_vad_take_word())
 *          returns NULL / 0 and never hands out the freed buffer.
 *
 *   INV-5  The learned noise floor survives dsp_vad_reset() (only
 *          dsp_vad_init() re-seeds it).  Stopping and restarting capture,
 *          e.g. around half-duplex playback, therefore does not reset the
 *          ambient adaptation.
 ****************************************************************************/

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
 *   All endpointing state is invalidated before the module publishes
 *   "not ready" (see INV-4 above), so any consumer that races this call
 *   observes dsp_vad_word() == NULL / dsp_vad_take_word() == 0 instead of
 *   a pointer to freed memory.  Safe to call on an uninitialised module.
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
 *   Zero-copy accessor: returns a pointer to the pending utterance and, if
 *   out_samples is non-NULL, its length in samples. NULL when nothing is
 *   pending.
 *
 *   Ownership stays with this module. The contents are only stable while
 *   the producer keeps dsp_vad_feed() suspended (see INV-2); they become
 *   undefined after the next dsp_vad_feed(), dsp_vad_reset() or
 *   dsp_vad_deinit(). The pointer must never be dereferenced after the
 *   module has been deinitialised (INV-4).
 *
 *   The caller may modify the samples in place, but note that the
 *   utterance has already been trimmed (INV-3).
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
