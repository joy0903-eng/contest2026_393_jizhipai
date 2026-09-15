/****************************************************************************
 * apps/examples/desktop_companion/audio_pipeline.h
 *
 * Audio capture / playback / prompt-tone handling for the AI-VOX3 (ES8311).
 *
 * Microphone capture is implemented (I2S RX) but NOT recognized — there is no
 * ASR cloud in this build (see plan §7.2). asr_transcribe() / tts_synthesize()
 * are stubs that return -ENOSYS until an ASR/TTS backend is configured.
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

/* Initialize the audio device (/dev/audio/pcm0). Returns OK. */
int audio_init(void);

/* Begin microphone capture (reserves the RX path). Returns OK. */
int audio_capture_start(void);

/* Stop microphone capture. Returns OK. */
int audio_capture_stop(void);

/* Read up to len bytes of captured 16-bit PCM into buf. Returns bytes read
 * (>=0) or negated errno. */
int audio_capture_read(uint8_t *buf, size_t len);

/* Play a short prompt tone (tone_id selects pitch/pattern). Returns OK. */
int audio_play_tone(int tone_id);

/* Play raw PCM (16-bit, mono, AIVOX3_AUDIO_RATE). Returns OK or negated. */
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
