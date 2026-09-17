/****************************************************************************
 * apps/examples/desktop_companion/audio_pipeline.h
 *
 * Audio capture / playback / prompt-tone handling for the AI-VOX3 (ES8311).
 *
 * The data path is the NuttX apb pipeline (see pcm_stream.h).  read()/write()
 * on the audio device node are NOT used anywhere: the ES8311 lower half has
 * NULL read/write callbacks, so the upper half returns 0 bytes for both and
 * a write() based loop would spin forever.
 *
 * The ES8311 is strictly half duplex.  Capture and playback each own their
 * own short-lived pcm_stream; audio_play_pcm() stops and closes capture
 * before opening the output direction, and restarts it afterwards.
 *
 * asr_transcribe() / tts_synthesize() remain -ENOSYS stubs until an ASR/TTS
 * backend is wired up.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_AUDIO_PIPELINE_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_AUDIO_PIPELINE_H

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
 * Public Function Prototypes
 ****************************************************************************/

/* Probe the audio device and prepare the pipeline.  Does NOT keep the device
 * open: the ES8311 is half duplex, so each direction is opened on demand.
 * Returns OK or a negated errno. */
int audio_init(void);

/* Stop capture (if running) and release pipeline state. */
int audio_deinit(void);

/* Begin microphone capture.  Captured samples accumulate in an internal ring
 * buffer until audio_capture_stop() or audio_capture_read*() drains them.
 * Returns OK or a negated errno. */
int audio_capture_start(void);

/* Stop microphone capture and close the capture stream. */
int audio_capture_stop(void);

/* Non-blocking drain of the capture ring buffer.  Returns the number of
 * bytes copied (0 if nothing is available yet), or a negated errno
 * (-EAGAIN when capture is not running).  NEVER blocks. */
int audio_capture_read(uint8_t *buf, size_t len);

/* Blocking variant: waits up to timeout_ms for data.  Returns the number of
 * bytes copied (0 on timeout), or a negated errno.  Always returns within
 * timeout_ms + a few ms. */
int audio_capture_read_timeout(void *buf, size_t len, int timeout_ms);

/* --- Offline voice front-end tap (dsp_vad) ------------------------------- *
 *
 * While capture is running every captured frame is also pushed through
 * dsp_vad_feed() (see dsp_vad.h).  The VAD is fed from the capture thread,
 * so dsp_vad_feed() MUST stay cheap; keyword recognition (MFCC + DTW) must
 * therefore not run there.  Instead:
 *
 *   1. the VAD signals a finished utterance,
 *   2. feeding is suspended so the utterance buffer is stable,
 *   3. the application calls audio_capture_take_word() to collect it and
 *      runs kws_recognize() on its own thread.
 *
 * The capture ring buffer is completely independent of this path: dropping
 * or delaying a word never stalls or blocks microphone capture.
 *
 * INVARIANT - DO NOT BREAK
 * ------------------------
 * dsp_vad / dsp_mfcc / kws_engine keep their working areas in module-static
 * arrays, so they are not reentrant:
 *
 *   - dsp_vad_feed() may only run on the capture thread (this module),
 *   - dsp_vad_trim() / dsp_mfcc_compute() / kws_*() may only run on one
 *     consumer at a time,
 *   - dsp_vad_feed() must NOT run concurrently with the above: feed() itself
 *     reaches dsp_vad_trim() through
 *     feed -> vad_process_frame -> vad_finish_capture -> dsp_vad_trim,
 *     which uses the same static workspace.
 *
 * The last point is what "while an utterance is pending, stop feeding"
 * actually buys us.  It is a load-bearing invariant: removing it would not
 * crash, it would produce occasional silent mis-recognition, which is much
 * worse to debug.  Keep the gate.
 * ------------------------------------------------------------------------- */

/* 1 while an utterance is being captured RIGHT NOW, 0 otherwise.
 *
 * Precise semantics: it reports the VAD's capturing state, which is cleared
 * the moment an utterance is published.  So it also reads 0
 *   - during playback (the ES8311 is half duplex: nothing is captured), and
 *   - while a finished utterance is pending recognition.
 * Use it for a "listening" indicator.  Do NOT use it for "recognizing":
 * that state is what audio_capture_process_word() reports.
 */
int audio_capture_is_speech(void);

/* Zero-copy processing of a finished utterance - PREFERRED.
 *
 * When an utterance is pending, `fn` is called with a pointer to the VAD's
 * internal buffer (mono int16, AIVOX3_AUDIO_RATE, up to VAD_MAX_WORD_SAMPLES
 * samples).  This costs no extra memory: the alternative
 * (audio_capture_take_word) needs a second 87 KB buffer in the caller.
 *
 * The utterance is ALREADY silence-trimmed: vad_finish_capture() calls
 * dsp_vad_trim() before publishing it (dsp_vad.c:178-182).  Do NOT trim it
 * again - a second pass with the same 18% threshold eats the ~60 ms margin
 * that the first pass deliberately keeps, shortening the first syllables and
 * pushing the DTW distance up.
 *
 * `fn` runs without the internal lock held, so it may take a few tens of ms
 * (kws_recognize) without blocking microphone capture.  It MUST NOT block
 * indefinitely, and must not use the pointer after it returns.
 *
 * Note on the result: kws_recognize() returns -1 both for "rejected" and for
 * "engine not ready / utterance too short", so an unloaded template bank
 * looks exactly like an over-strict threshold.  Check kws_is_ready() first
 * when tuning.
 *
 * Returns 0 when no utterance is pending, the value returned by `fn`
 * otherwise, or -EAGAIN / -EINVAL.
 */
typedef int (*audio_word_fn_t)(const int16_t *pcm, int nsamples, void *arg);
int audio_capture_process_word(audio_word_fn_t fn, void *arg);

/* Copy-out variant of the above.  `out` must hold at least
 * VAD_MAX_WORD_SAMPLES (44800) samples, i.e. ~87 KB.  Prefer
 * audio_capture_process_word() unless you really need to keep the PCM.
 * The utterance is already trimmed (see above) - do not trim again.
 *
 * Returns the number of samples copied, 0 when nothing is pending, or a
 * negated errno (-EAGAIN when the VAD is not running, -ENOSPC when the
 * utterance does not fit).  Calling this also resumes feeding.
 */
int audio_capture_take_word(int16_t *out, int capacity);

/* Play a short prompt tone (tone_id selects pitch/pattern). Returns OK. */
int audio_play_tone(int tone_id);

/* Play raw PCM (16-bit, mono, AIVOX3_AUDIO_RATE).  Blocks until the whole
 * payload has been handed to the driver.  Returns OK or a negated errno. */
int audio_play_pcm(const uint8_t *pcm, size_t len);

/* --- Stubs (plan §7.2): no ASR/TTS backend in this build --- */

/* Transcribe PCM to text. Stub: returns -ENOSYS. */
int asr_transcribe(const uint8_t *pcm, size_t len,
                   char *text, size_t text_len);

/* Synthesize text to PCM. Stub: returns -ENOSYS. */
int tts_synthesize(const char *text, uint8_t *pcm, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_AUDIO_PIPELINE_H */
