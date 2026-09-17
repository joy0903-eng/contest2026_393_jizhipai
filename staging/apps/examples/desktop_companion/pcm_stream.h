/****************************************************************************
 * apps/examples/desktop_companion/pcm_stream.h
 *
 * Thin, blocking wrapper around the NuttX "audio pipeline buffer" (apb)
 * streaming model, for the ES8311 codec on the AI-VOX3 board.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The ES8311 lower-half driver (drivers/audio/es8311.c) leaves the
 * audio_ops_s::read and audio_ops_s::write callbacks NULL.  The NuttX audio
 * upper half silently returns 0 bytes for read()/write() when those pointers
 * are NULL -- no -ENOSYS, no crash.  Consequences for the previous
 * audio_pipeline.c:
 *
 *   - audio_capture_read()   always returned 0 bytes (ASR got silence).
 *   - audio_play_pcm()       did `off += n` with n == 0 -> INFINITE LOOP,
 *                            the whole system hung.
 *
 * The only working data path is the apb pipeline:
 *
 *   AUDIOIOC_RESERVE -> AUDIOIOC_CONFIGURE -> mq_open + AUDIOIOC_REGISTERMQ
 *   -> AUDIOIOC_ALLOCBUFFER -> fill -> AUDIOIOC_ENQUEUEBUFFER
 *   -> AUDIOIOC_START
 *   -> loop on the message queue:
 *        AUDIO_MSG_DEQUEUE  : consume apb->samp, reset, re-enqueue
 *        AUDIO_MSG_COMPLETE : stream finished
 *   -> AUDIOIOC_STOP -> drain -> AUDIOIOC_FREEBUFFER -> AUDIOIOC_UNREGISTERMQ
 *      -> AUDIOIOC_RELEASE
 *
 * HALF-DUPLEX CONSTRAINT
 * ----------------------
 * The ES8311 driver keeps a single `audio_mode` field and
 * es8311_processbegin() picks I2S_SEND *or* I2S_RECEIVE from it.  Capture and
 * playback can therefore never run at the same time.  Callers MUST open, use
 * and close one direction before touching the other (audio_pipeline.c
 * enforces this with a mutex).
 *
 * NOTES
 *  - struct pcm_stream is fully declared here so callers can allocate it on
 *    the stack; it must be zeroed or passed to pcm_stream_open() (which
 *    memsets it) before any other call.
 *  - C99 only, no C++.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_PCM_STREAM_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_PCM_STREAM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <mqueue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/audio/audio.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of pipeline buffers.  CONFIG_ES8311_INFLIGHT defaults to 2, so 4
 * gives the driver one spare to chew on while we refill.
 */

#define PCM_STREAM_NBUFFERS   4

/* Bytes per pipeline buffer: 2048 B = 64 ms @ 16 kHz / 16-bit / mono. */

#define PCM_STREAM_BUFSIZE    2048

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Capture callback.  `samples` points into the driver-owned apb -- it is
 * only valid for the duration of the call, so COPY IT (or hand it off) and
 * return quickly.  Never block in this callback.
 */

typedef void (*pcm_data_cb_t)(const int16_t *samples, int nsamples,
                              void *arg);

/* Stream context.  Caller-allocated (stack is fine). */

struct pcm_stream
{
  int                    fd;        /* /dev/audio/pcm0, -1 when closed      */
  int                    type;      /* AUDIO_TYPE_INPUT / AUDIO_TYPE_OUTPUT */
  bool                   opened;    /* open() + reserve + configure done    */
  bool                   started;   /* AUDIOIOC_START issued, not stopped   */

  mqd_t                  mq;        /* (mqd_t)-1 when not open             */
  char                   mqname[24];

  struct ap_buffer_s    *apb[PCM_STREAM_NBUFFERS];
  int                    nallocated;

  /* Capture-only state */

  pthread_t              thread;
  bool                   thread_started;
  volatile bool          stop_req;   /* capture thread must exit           */
  pcm_data_cb_t          cb;
  void                  *cb_arg;
  int                    stop_result;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: pcm_stream_open
 *
 * Description:
 *   Open /dev/audio/pcm0, reserve it, configure it for `type`
 *   (AUDIO_TYPE_INPUT or AUDIO_TYPE_OUTPUT) at AIVOX3_AUDIO_RATE / 16-bit /
 *   mono, register a message queue and allocate the pipeline buffers.
 *   Does NOT start streaming.
 *
 * Input Parameters:
 *   s    - Stream context; fully initialised (memset) by this call.
 *   type - AUDIO_TYPE_INPUT or AUDIO_TYPE_OUTPUT.
 *
 * Returned Value:
 *   OK (0) on success, negated errno on failure.  On failure the context is
 *   left in a state that is safe to pass to pcm_stream_close().
 ****************************************************************************/

int pcm_stream_open(struct pcm_stream *s, int type);

/****************************************************************************
 * Name: pcm_stream_close
 *
 * Description:
 *   Stop (if running), free every pipeline buffer, unregister and delete the
 *   message queue, release and close the device.  Never fails; safe to call
 *   on a partially opened or already closed context.
 ****************************************************************************/

void pcm_stream_close(struct pcm_stream *s);

/****************************************************************************
 * Name: pcm_stream_start
 *
 * Description:
 *   Issue AUDIOIOC_START.  Buffers must already have been enqueued (done by
 *   pcm_stream_start_capture() / pcm_stream_play()).
 *
 * Returned Value:
 *   OK on success, negated errno on failure.
 ****************************************************************************/

int pcm_stream_start(struct pcm_stream *s);

/****************************************************************************
 * Name: pcm_stream_stop
 *
 * Description:
 *   Issue AUDIOIOC_STOP and drain the message queue (reaping, never
 *   re-enqueueing) until AUDIO_MSG_COMPLETE arrives or a timeout expires.
 *   If a capture thread is running it is asked to exit and joined; the
 *   thread performs the stop/drain itself.
 *
 * Returned Value:
 *   OK on success, negated errno on failure.
 ****************************************************************************/

int pcm_stream_stop(struct pcm_stream *s);

/****************************************************************************
 * Name: pcm_stream_start_capture
 *
 * Description:
 *   Prime every buffer, start the stream and spawn the capture pump thread.
 *   Each AUDIO_MSG_DEQUEUE invokes `cb` and then re-enqueues the buffer.
 *   Requires an AUDIO_TYPE_INPUT stream.
 *
 * Input Parameters:
 *   s   - Stream context (must be opened with AUDIO_TYPE_INPUT).
 *   cb  - Data callback, invoked from the capture thread.  Must not block.
 *   arg - Opaque argument passed to `cb`.
 *
 * Returned Value:
 *   OK on success, negated errno on failure.
 ****************************************************************************/

int pcm_stream_start_capture(struct pcm_stream *s, pcm_data_cb_t cb,
                             void *arg);

/****************************************************************************
 * Name: pcm_stream_play
 *
 * Description:
 *   Blocking playback of `len` bytes of raw PCM.  Primes up to
 *   PCM_STREAM_NBUFFERS buffers, starts the stream, then refills each
 *   dequeued buffer until the whole payload has been handed to the driver
 *   and every buffer has come back.  Stops the stream with AUDIOIOC_STOP
 *   (never with AUDIO_APB_FINAL -- es8311_returnbuffers() DEBUGASSERTs on
 *   that path).  Requires an AUDIO_TYPE_OUTPUT stream.
 *
 * Returned Value:
 *   OK on success, negated errno on failure.
 ****************************************************************************/

int pcm_stream_play(struct pcm_stream *s, const uint8_t *pcm, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_PCM_STREAM_H */
