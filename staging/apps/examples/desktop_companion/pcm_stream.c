/****************************************************************************
 * apps/examples/desktop_companion/pcm_stream.c
 *
 * NuttX apb ("audio pipeline buffer") streaming for the ES8311 on AI-VOX3.
 * See pcm_stream.h for the rationale and the required ioctl sequence.
 *
 * Every ioctl return value is checked and logged with its errno.  We have no
 * JTAG and no simulator here: syslog output is the only diagnostic channel
 * available on the real device, so a silent failure is unacceptable.
 *
 * Buffer ownership (do not "optimise" this away):
 *   es8311_enqueuebuffer()   -> apb_reference(apb)   (crefs 1 -> 2)
 *   es8311_returnbuffers()   -> apb_free(apb)        (crefs 2 -> 1)
 * The application owns exactly one reference (the one from
 * AUDIOIOC_ALLOCBUFFER) and must release it with exactly one
 * AUDIOIOC_FREEBUFFER per buffer, after the buffer has been dequeued back.
 * Never call apb_free() on a buffer that has been enqueued.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

#include "config.h"
#include "pcm_stream.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Device node registered by ai_vox3_audio.c via audio_register("pcm0", ..). */

#define PCM_STREAM_DEV              "/dev/audio/pcm0"

/* Depth of our message queue.  We only ever have PCM_STREAM_NBUFFERS buffers
 * in flight, but the driver also posts COMPLETE/STOP, so leave headroom.
 */

#define PCM_STREAM_MQ_MAXMSG        16

/* How long a single mq wait may take (ms).  Bounded waits everywhere: there
 * must be no code path that can block forever.
 */

#define PCM_STREAM_RECV_TIMEOUT_MS  500
#define PCM_STREAM_DRAIN_TIMEOUT_MS 1000

/* Capture pump thread stack.  The callback only memcpy()s into a ring
 * buffer, so this is generous.
 */

#define PCM_STREAM_CAPTURE_STACKSIZE 4096

/* Maximum consecutive EINTR retries before we give up on a wait. */

#define PCM_STREAM_MAX_EINTR        8

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pcm_ioc
 *
 * Description:
 *   ioctl() wrapper that logs the command name, the return value and errno
 *   on failure.  Returns OK (0) or a negated errno.
 ****************************************************************************/

static int pcm_ioc(struct pcm_stream *s, int cmd, unsigned long arg,
                   const char *cmdname)
{
  int ret;

  ret = ioctl(s->fd, cmd, arg);
  if (ret < 0)
    {
      int errcode = errno;

      syslog(LOG_ERR, "pcm_stream: %s failed: ret=%d errno=%d\n",
             cmdname, ret, errcode);
      return -errcode;
    }

  return OK;
}

/****************************************************************************
 * Name: pcm_mq_recv
 *
 * Description:
 *   Receive one struct audio_msg_s, with a hard deadline.
 *
 * Returned Value:
 *   The message id (>= 0) on success, negated errno on failure
 *   (-ETIMEDOUT when nothing arrived in time).
 ****************************************************************************/

static int pcm_mq_recv(struct pcm_stream *s, struct audio_msg_s *msg,
                       int timeout_ms)
{
  struct timespec abstime;
  unsigned int prio = 0;
  ssize_t n;

  if (clock_gettime(CLOCK_REALTIME, &abstime) != 0)
    {
      return -errno;
    }

  abstime.tv_sec  += (time_t)(timeout_ms / 1000);
  abstime.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
  if (abstime.tv_nsec >= 1000000000L)
    {
      abstime.tv_sec  += 1;
      abstime.tv_nsec -= 1000000000L;
    }

  n = mq_timedreceive(s->mq, (char *)msg, sizeof(struct audio_msg_s),
                      &prio, &abstime);
  if (n < (ssize_t)sizeof(struct audio_msg_s))
    {
      int errcode = errno;

      if (n < 0 && errcode != 0)
        {
          return -errcode;
        }

      /* Short read or no errno set: treat as a protocol error. */

      return -EIO;
    }

  return (int)msg->msg_id;
}

/****************************************************************************
 * Name: pcm_enqueue
 *
 * Description:
 *   Hand one filled (input: empty) pipeline buffer to the driver.
 ****************************************************************************/

static int pcm_enqueue(struct pcm_stream *s, struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s bufdesc;

  if (apb == NULL)
    {
      return -EINVAL;
    }

  memset(&bufdesc, 0, sizeof(bufdesc));
  bufdesc.numbytes = apb->nmaxbytes;
  bufdesc.u.buffer = apb;

  return pcm_ioc(s, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&bufdesc,
                 "AUDIOIOC_ENQUEUEBUFFER");
}

/****************************************************************************
 * Name: pcm_do_stop
 *
 * Description:
 *   AUDIOIOC_STOP plus a bounded drain.  Buffers coming back during the
 *   drain are reaped but NOT re-enqueued.
 *
 *   Guarded by s->started: es8311_stop() does pthread_join() and then clears
 *   its thread id, so calling it twice would be undefined.  Receiving
 *   AUDIO_MSG_COMPLETE clears s->started too.
 ****************************************************************************/

static int pcm_do_stop(struct pcm_stream *s)
{
  struct audio_msg_s msg;
  int ret;
  int r;
  int i;
  int eintr;

  if (!s->started)
    {
      return OK;
    }

  ret = pcm_ioc(s, AUDIOIOC_STOP, 0UL, "AUDIOIOC_STOP");
  s->started = false;

  eintr = 0;
  for (i = 0; i < PCM_STREAM_NBUFFERS + 4; i++)
    {
      r = pcm_mq_recv(s, &msg, PCM_STREAM_DRAIN_TIMEOUT_MS);
      if (r < 0)
        {
          if (r == -EINTR && eintr < PCM_STREAM_MAX_EINTR)
            {
              eintr++;
              continue;
            }

          if (r != -ETIMEDOUT)
            {
              syslog(LOG_ERR, "pcm_stream: drain: mq recv: %d\n", r);
            }

          break;
        }

      if (r == AUDIO_MSG_COMPLETE)
        {
          break;
        }

      /* AUDIO_MSG_DEQUEUE and anything else: reap and keep draining. */
    }

  return ret;
}

/****************************************************************************
 * Name: pcm_capture_thread
 *
 * Description:
 *   Capture pump.  Waits for DEQUEUE, hands the samples to the callback and
 *   re-enqueues the buffer.  Exits on stop request, error or COMPLETE, and
 *   always finishes with pcm_do_stop().
 ****************************************************************************/

static void *pcm_capture_thread(pthread_addr_t arg)
{
  struct pcm_stream *s = (struct pcm_stream *)arg;
  struct audio_msg_s msg;
  struct ap_buffer_s *apb;
  int r;
  int eintr = 0;

  while (!s->stop_req)
    {
      r = pcm_mq_recv(s, &msg, PCM_STREAM_RECV_TIMEOUT_MS);
      if (r < 0)
        {
          if (r == -EINTR && eintr < PCM_STREAM_MAX_EINTR)
            {
              eintr++;
              continue;
            }

          if (r == -EINTR)
            {
              break;
            }

          if (r != -ETIMEDOUT)
            {
              syslog(LOG_ERR, "pcm_stream: capture: mq recv: %d\n", r);
              break;
            }

          /* Timed out: normal while the codec is idle; re-check stop_req. */

          continue;
        }

      if (r == AUDIO_MSG_COMPLETE)
        {
          s->started = false;
          break;
        }

      if (r != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      apb = (struct ap_buffer_s *)msg.u.ptr;
      if (apb != NULL)
        {
          int nbytes = (int)apb->nbytes - (int)apb->curbyte;

          if (nbytes > 0 && s->cb != NULL)
            {
              s->cb((const int16_t *)(apb->samp + apb->curbyte),
                    nbytes / (int)sizeof(int16_t), s->cb_arg);
            }

          if (s->stop_req)
            {
              break;
            }

          /* Re-arm: the driver uses nbytes as the DMA length for RX, so it
           * must be reset to the full capacity.  Never set AUDIO_APB_FINAL.
           */

          apb->curbyte = 0;
          apb->nbytes  = apb->nmaxbytes;
          apb->flags   = 0;

          if (pcm_enqueue(s, apb) < 0)
            {
              syslog(LOG_ERR, "pcm_stream: capture: re-enqueue failed\n");
              break;
            }
        }
    }

  s->stop_result = pcm_do_stop(s);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: pcm_stream_open
 ****************************************************************************/

int pcm_stream_open(struct pcm_stream *s, int type)
{
  static unsigned int g_mq_seq = 0;

  struct audio_caps_desc_s capsdesc;
  struct audio_buf_desc_s bufdesc;
  struct mq_attr attr;
  unsigned int seq;
  int ret;
  int i;
  int errcode;

  if (s == NULL)
    {
      return -EINVAL;
    }

  if (type != AUDIO_TYPE_INPUT && type != AUDIO_TYPE_OUTPUT)
    {
      return -EINVAL;
    }

  memset(s, 0, sizeof(*s));
  s->fd  = -1;
  s->mq  = (mqd_t)-1;
  s->type = type;

  s->fd = open(PCM_STREAM_DEV, O_RDWR | O_CLOEXEC);
  if (s->fd < 0)
    {
      errcode = errno;
      syslog(LOG_ERR, "pcm_stream: open %s failed: errno=%d\n",
             PCM_STREAM_DEV, errcode);
      return -errcode;
    }

  /* MULTI_SESSION is not enabled, so RESERVE/RELEASE take no argument. */

  ret = pcm_ioc(s, AUDIOIOC_RESERVE, 0UL, "AUDIOIOC_RESERVE");
  if (ret < 0)
    {
      goto errout_close;
    }

  /* AUDIOIOC_CONFIGURE takes a struct audio_caps_desc_s, i.e. an
   * audio_caps_s wrapped one level deeper.
   *
   * Field placement confirmed against drivers/audio/es8311.c:
   *   sample rate  -> ac_controls.hw[0]   (es8311.c:1042 / :1089)
   *   bits/sample  -> ac_controls.b[2]    (es8311.c:1043 / :1090)
   *   ac_channels  -> 1 or 2              (es8311.c:1021 / :1068)
   *   ac_len       -> sizeof(audio_caps_s) (DEBUGASSERT es8311.c:805)
   */

  memset(&capsdesc, 0, sizeof(capsdesc));
  capsdesc.caps.ac_len      = (uint8_t)sizeof(struct audio_caps_s);
  capsdesc.caps.ac_type     = (uint8_t)type;
  capsdesc.caps.ac_subtype  = AUDIO_SUBFMT_PCM_S16_LE;
  capsdesc.caps.ac_channels = (uint8_t)AIVOX3_AUDIO_CHANNELS;
  capsdesc.caps.ac_chmap    = 0;
  capsdesc.caps.reserved    = 0;
  capsdesc.caps.ac_format.hw = 0;
  capsdesc.caps.ac_controls.w = 0;
  capsdesc.caps.ac_controls.hw[0] = (uint16_t)AIVOX3_AUDIO_RATE;
  capsdesc.caps.ac_controls.b[2]  = (uint8_t)AIVOX3_AUDIO_BITS;

  ret = pcm_ioc(s, AUDIOIOC_CONFIGURE, (unsigned long)&capsdesc,
                "AUDIOIOC_CONFIGURE");
  if (ret < 0)
    {
      goto errout_release;
    }

  /* Message queue used by the upper half to report DEQUEUE / COMPLETE. */

  seq = g_mq_seq++;
  snprintf(s->mqname, sizeof(s->mqname), "/pcmstream%u", seq);

  attr.mq_maxmsg  = PCM_STREAM_MQ_MAXMSG;
  attr.mq_msgsize = (long)sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = 0;

  s->mq = mq_open(s->mqname, O_RDWR | O_CREAT | O_EXCL, 0644, &attr);
  if (s->mq == (mqd_t)-1)
    {
      errcode = errno;
      if (errcode == EEXIST)
        {
          mq_unlink(s->mqname);
          s->mq = mq_open(s->mqname, O_RDWR | O_CREAT, 0644, &attr);
        }

      if (s->mq == (mqd_t)-1)
        {
          errcode = errno;
          syslog(LOG_ERR, "pcm_stream: mq_open %s failed: errno=%d\n",
                 s->mqname, errcode);
          ret = -errcode;
          goto errout_release;
        }
    }

  ret = pcm_ioc(s, AUDIOIOC_REGISTERMQ, (unsigned long)s->mq,
                "AUDIOIOC_REGISTERMQ");
  if (ret < 0)
    {
      goto errout_mq;
    }

  for (i = 0; i < PCM_STREAM_NBUFFERS; i++)
    {
      memset(&bufdesc, 0, sizeof(bufdesc));
      bufdesc.numbytes   = PCM_STREAM_BUFSIZE;
      bufdesc.u.pbuffer  = &s->apb[i];

      ret = pcm_ioc(s, AUDIOIOC_ALLOCBUFFER, (unsigned long)&bufdesc,
                    "AUDIOIOC_ALLOCBUFFER");
      if (ret < 0)
        {
          goto errout_buffers;
        }

      if (s->apb[i] == NULL)
        {
          syslog(LOG_ERR, "pcm_stream: ALLOCBUFFER returned no buffer\n");
          ret = -ENOMEM;
          goto errout_buffers;
        }

      s->apb[i]->curbyte = 0;
      s->apb[i]->nbytes  = s->apb[i]->nmaxbytes;
      s->apb[i]->flags   = 0;
      s->nallocated++;
    }

  s->opened = true;

  syslog(LOG_INFO,
         "pcm_stream: open %s type=%d rate=%d bits=%d ch=%d bufs=%dx%d\n",
         PCM_STREAM_DEV, type, AIVOX3_AUDIO_RATE, AIVOX3_AUDIO_BITS,
         AIVOX3_AUDIO_CHANNELS, s->nallocated, PCM_STREAM_BUFSIZE);

  return OK;

errout_buffers:
  for (i = 0; i < s->nallocated; i++)
    {
      memset(&bufdesc, 0, sizeof(bufdesc));
      bufdesc.numbytes  = 0;
      bufdesc.u.buffer  = s->apb[i];
      (void)pcm_ioc(s, AUDIOIOC_FREEBUFFER, (unsigned long)&bufdesc,
                    "AUDIOIOC_FREEBUFFER");
      s->apb[i] = NULL;
    }

  s->nallocated = 0;
  (void)pcm_ioc(s, AUDIOIOC_UNREGISTERMQ, (unsigned long)s->mq,
                "AUDIOIOC_UNREGISTERMQ");

errout_mq:
  mq_close(s->mq);
  mq_unlink(s->mqname);
  s->mq = (mqd_t)-1;

errout_release:
  (void)pcm_ioc(s, AUDIOIOC_RELEASE, 0UL, "AUDIOIOC_RELEASE");

errout_close:
  close(s->fd);
  s->fd = -1;
  s->opened = false;
  return ret;
}

/****************************************************************************
 * Name: pcm_stream_close
 ****************************************************************************/

void pcm_stream_close(struct pcm_stream *s)
{
  struct audio_buf_desc_s bufdesc;
  int i;

  if (s == NULL)
    {
      return;
    }

  /* Join the capture pump first: it owns the message queue while running. */

  if (s->thread_started)
    {
      s->stop_req = true;
      (void)pthread_join(s->thread, NULL);
      s->thread_started = false;
    }

  (void)pcm_do_stop(s);

  if (s->fd >= 0)
    {
      for (i = 0; i < s->nallocated; i++)
        {
          if (s->apb[i] == NULL)
            {
              continue;
            }

          memset(&bufdesc, 0, sizeof(bufdesc));
          bufdesc.numbytes = 0;
          bufdesc.u.buffer = s->apb[i];
          (void)pcm_ioc(s, AUDIOIOC_FREEBUFFER, (unsigned long)&bufdesc,
                        "AUDIOIOC_FREEBUFFER");
          s->apb[i] = NULL;
        }

      s->nallocated = 0;

      if (s->mq != (mqd_t)-1)
        {
          (void)pcm_ioc(s, AUDIOIOC_UNREGISTERMQ, (unsigned long)s->mq,
                        "AUDIOIOC_UNREGISTERMQ");
        }

      (void)pcm_ioc(s, AUDIOIOC_RELEASE, 0UL, "AUDIOIOC_RELEASE");
    }

  if (s->mq != (mqd_t)-1)
    {
      mq_close(s->mq);
      mq_unlink(s->mqname);
      s->mq = (mqd_t)-1;
    }

  if (s->fd >= 0)
    {
      close(s->fd);
      s->fd = -1;
    }

  s->opened  = false;
  s->started = false;
}

/****************************************************************************
 * Name: pcm_stream_start
 ****************************************************************************/

int pcm_stream_start(struct pcm_stream *s)
{
  int ret;

  if (s == NULL || !s->opened)
    {
      return -EINVAL;
    }

  if (s->started)
    {
      return OK;
    }

  ret = pcm_ioc(s, AUDIOIOC_START, 0UL, "AUDIOIOC_START");
  if (ret < 0)
    {
      return ret;
    }

  s->started = true;
  return OK;
}

/****************************************************************************
 * Name: pcm_stream_stop
 ****************************************************************************/

int pcm_stream_stop(struct pcm_stream *s)
{
  if (s == NULL || !s->opened)
    {
      return -EINVAL;
    }

  if (s->thread_started)
    {
      s->stop_req = true;
      (void)pthread_join(s->thread, NULL);
      s->thread_started = false;
      return s->stop_result;
    }

  return pcm_do_stop(s);
}

/****************************************************************************
 * Name: pcm_stream_start_capture
 ****************************************************************************/

int pcm_stream_start_capture(struct pcm_stream *s, pcm_data_cb_t cb,
                             void *arg)
{
  pthread_attr_t tattr;
  int ret;
  int i;

  if (s == NULL || !s->opened)
    {
      return -EINVAL;
    }

  if (s->type != AUDIO_TYPE_INPUT || cb == NULL)
    {
      return -EINVAL;
    }

  if (s->thread_started || s->started)
    {
      return -EBUSY;
    }

  s->cb          = cb;
  s->cb_arg      = arg;
  s->stop_req    = false;
  s->stop_result = OK;

  /* Prime: for RX the driver uses nbytes as the DMA length, so it must be
   * set to the full capacity and curbyte cleared.  flags must be 0 -- a
   * stray AUDIO_APB_FINAL would terminate the stream immediately.
   */

  for (i = 0; i < s->nallocated; i++)
    {
      s->apb[i]->curbyte = 0;
      s->apb[i]->nbytes  = s->apb[i]->nmaxbytes;
      s->apb[i]->flags   = 0;

      ret = pcm_enqueue(s, s->apb[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = pcm_stream_start(s);
  if (ret < 0)
    {
      return ret;
    }

  pthread_attr_init(&tattr);
  pthread_attr_setstacksize(&tattr, PCM_STREAM_CAPTURE_STACKSIZE);

  ret = pthread_create(&s->thread, &tattr, pcm_capture_thread,
                       (pthread_addr_t)s);
  pthread_attr_destroy(&tattr);

  if (ret != OK)
    {
      syslog(LOG_ERR, "pcm_stream: pthread_create failed: %d\n", ret);
      (void)pcm_do_stop(s);
      return -ret;
    }

  s->thread_started = true;
  return OK;
}

/****************************************************************************
 * Name: pcm_stream_play
 ****************************************************************************/

int pcm_stream_play(struct pcm_stream *s, const uint8_t *pcm, size_t len)
{
  struct audio_msg_s msg;
  struct ap_buffer_s *apb;
  size_t off = 0;
  int inflight = 0;
  int ret = OK;
  int r;
  int i;
  int eintr;

  if (s == NULL || !s->opened)
    {
      return -EINVAL;
    }

  if (s->type != AUDIO_TYPE_OUTPUT)
    {
      return -EINVAL;
    }

  if (pcm == NULL && len > 0)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return OK;
    }

  if (s->started || s->thread_started)
    {
      return -EBUSY;
    }

  /* Prime: fill and enqueue as many buffers as we have. */

  for (i = 0; i < s->nallocated && off < len; i++)
    {
      size_t chunk = len - off;

      if (chunk > (size_t)s->apb[i]->nmaxbytes)
        {
          chunk = (size_t)s->apb[i]->nmaxbytes;
        }

      memcpy(s->apb[i]->samp, pcm + off, chunk);
      s->apb[i]->curbyte = 0;
      s->apb[i]->nbytes  = (apb_samp_t)chunk;
      s->apb[i]->flags   = 0;

      ret = pcm_enqueue(s, s->apb[i]);
      if (ret < 0)
        {
          (void)pcm_do_stop(s);
          return ret;
        }

      off      += chunk;
      inflight++;
    }

  ret = pcm_stream_start(s);
  if (ret < 0)
    {
      (void)pcm_do_stop(s);
      return ret;
    }

  /* Feed / reap loop.  Bounded by construction: every iteration either
   * consumes a buffer or fails.  off >= len && inflight == 0 ends the loop.
   */

  eintr = 0;
  while (inflight > 0)
    {
      r = pcm_mq_recv(s, &msg, PCM_STREAM_RECV_TIMEOUT_MS);
      if (r < 0)
        {
          if (r == -EINTR && eintr < PCM_STREAM_MAX_EINTR)
            {
              eintr++;
              continue;
            }

          syslog(LOG_ERR, "pcm_stream: play: mq recv: %d (inflight=%d)\n",
                 r, inflight);
          ret = r;
          break;
        }

      if (r == AUDIO_MSG_COMPLETE)
        {
          s->started = false;
          break;
        }

      if (r != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      apb = (struct ap_buffer_s *)msg.u.ptr;
      inflight--;

      if (apb == NULL)
        {
          continue;
        }

      if (off < len)
        {
          size_t chunk = len - off;

          if (chunk > (size_t)apb->nmaxbytes)
            {
              chunk = (size_t)apb->nmaxbytes;
            }

          memcpy(apb->samp, pcm + off, chunk);
          apb->curbyte = 0;
          apb->nbytes  = (apb_samp_t)chunk;
          apb->flags   = 0;

          if (pcm_enqueue(s, apb) < 0)
            {
              syslog(LOG_ERR, "pcm_stream: play: re-enqueue failed\n");
              ret = -EIO;
              break;
            }

          off      += chunk;
          inflight++;
        }
    }

  /* All payload handed over and every buffer back -> stop the stream.
   * pcm_do_stop() is a no-op if COMPLETE already ended it.
   */

  (void)pcm_do_stop(s);
  return ret;
}
