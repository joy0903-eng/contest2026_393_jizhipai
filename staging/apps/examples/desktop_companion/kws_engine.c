/****************************************************************************
 * apps/examples/desktop_companion/kws_engine.c
 *
 * DTW keyword-spotting engine implementation. See kws_engine.h.
 *
 * Numerically identical to the Arduino reference keyword_engine.h:
 *   - per-frame distance = sum of |a - b| over FEAT_DIM (int32 arithmetic);
 *   - banded Sakoe-Chiba DP with |i - j| <= KWS_DTW_BAND;
 *   - normalized score = total / (na + nb);
 *   - adaptive threshold = max intra-class distance * 1.35.
 *
 * The DP rows are module-static (2 * 141 * 4 B = 1.1 KB) instead of
 * function-static: same memory, but visible and free of any stack pressure
 * on small NuttX task stacks.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <unistd.h>

#include "dsp_mfcc.h"
#include "kws_engine.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef OK
#  define OK 0
#endif

#define KWS_MAGIC       0x4B575331u   /* "KWS1"                             */
#define KWS_FMT_V1      1u            /* Arduino layout (no word names)     */
#define KWS_FMT_V2      2u            /* + word names                       */

#define KWS_DP_INF      0x3FFFFFFF

/* The template file is binary. POSIX has no text/binary distinction; some
 * hosted builds (Win32/MSVCRT) default to text mode and would expand 0x0A
 * into 0x0D 0x0A inside the int16 payload. O_BINARY is a no-op where the
 * platform does not define it. */

#ifndef O_BINARY
#  ifdef _O_BINARY
#    define O_BINARY _O_BINARY
#  else
#    define O_BINARY 0
#  endif
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static kws_template_t s_bank[KWS_WORDS_MAX][KWS_TPL_PER_WORD];
static int            s_tpl_count[KWS_WORDS_MAX];
static int            s_word_count = 0;
static char           s_word_name[KWS_WORDS_MAX][KWS_NAME_MAX];
static float          s_threshold = KWS_THRESH_DEFAULT;
static int            s_ready = 0;

/* DP rows and the scratch feature matrix (140 * 12 * 2 B = 3.4 KB). */

static int32_t s_dp_prev[KWS_MAX_FRAMES + 1];
static int32_t s_dp_cur[KWS_MAX_FRAMES + 1];
static int16_t s_scratch[KWS_MAX_FRAMES * FEAT_DIM];

/****************************************************************************
 * Private Functions - DTW
 ****************************************************************************/

/****************************************************************************
 * Name: kws_dtw
 *
 * Description:
 *   Banded DTW normalized distance between two feature matrices.
 *
 * Returned Value:
 *   total / (na + nb), or a very large value when the band makes the pair
 *   unreachable (length ratio outside the Sakoe-Chiba band).
 *
 ****************************************************************************/

static float kws_dtw(const int16_t *a, int na, const int16_t *b, int nb)
{
  int i;
  int j;

  if (na <= 0 || nb <= 0)
    {
      return 1.0e9f;
    }

  for (j = 0; j <= nb; j++)
    {
      s_dp_prev[j] = KWS_DP_INF;
    }

  s_dp_prev[0] = 0;

  for (i = 1; i <= na; i++)
    {
      int lo = (i - KWS_DTW_BAND > 1) ? i - KWS_DTW_BAND : 1;
      int hi = (i + KWS_DTW_BAND < nb) ? i + KWS_DTW_BAND : nb;
      int k;

      if (lo > nb)
        {
          /* This row cannot reach any column: the path is impossible. */
          for (j = 0; j <= nb; j++)
            {
              s_dp_prev[j] = KWS_DP_INF;
            }

          continue;
        }

      for (j = 0; j <= nb; j++)
        {
          s_dp_cur[j] = KWS_DP_INF;
        }

      for (j = lo; j <= hi; j++)
        {
          const int16_t *pa = a + (i - 1) * FEAT_DIM;
          const int16_t *pb = b + (j - 1) * FEAT_DIM;
          int32_t        d  = 0;
          int32_t        m;

          for (k = 0; k < FEAT_DIM; k++)
            {
              int32_t diff = (int32_t)pa[k] - (int32_t)pb[k];

              d += (diff < 0) ? -diff : diff;
            }

          m = s_dp_prev[j - 1];

          if (s_dp_prev[j] < m)
            {
              m = s_dp_prev[j];
            }

          if (s_dp_cur[j - 1] < m)
            {
              m = s_dp_cur[j - 1];
            }

          s_dp_cur[j] = d + m;
        }

      for (j = 0; j <= nb; j++)
        {
          s_dp_prev[j] = s_dp_cur[j];
        }
    }

  if (s_dp_prev[nb] >= KWS_DP_INF)
    {
      return 1.0e9f;
    }

  return (float)s_dp_prev[nb] / (float)(na + nb);
}

/****************************************************************************
 * Private Functions - little-endian serialization
 ****************************************************************************/

static void kws_put_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void kws_put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint16_t kws_get_u16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t kws_get_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void kws_put_float(uint8_t *p, float v)
{
  uint32_t bits;

  memcpy(&bits, &v, sizeof(bits));
  kws_put_u32(p, bits);
}

static float kws_get_float(const uint8_t *p)
{
  uint32_t bits = kws_get_u32(p);
  float    v;

  memcpy(&v, &bits, sizeof(v));
  return v;
}

/****************************************************************************
 * Name: kws_write_all / kws_read_all
 *
 * Description:
 *   POSIX read/write loops that tolerate short transfers.
 *
 ****************************************************************************/

static int kws_write_all(int fd, const void *buf, size_t len)
{
  const uint8_t *p = (const uint8_t *)buf;
  size_t         done = 0;

  while (done < len)
    {
      ssize_t n = write(fd, &p[done], len - done);

      if (n <= 0)
        {
          return -EIO;
        }

      done += (size_t)n;
    }

  return OK;
}

static int kws_read_all(int fd, void *buf, size_t len)
{
  uint8_t *p    = (uint8_t *)buf;
  size_t   done = 0;

  while (done < len)
    {
      ssize_t n = read(fd, &p[done], len - done);

      if (n < 0)
        {
          return -EIO;
        }

      if (n == 0)
        {
          return -EIO;      /* truncated file */
        }

      done += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kws_init
 ****************************************************************************/

int kws_init(void)
{
  int w;
  int t;

  for (w = 0; w < KWS_WORDS_MAX; w++)
    {
      s_tpl_count[w] = 0;
      s_word_name[w][0] = '\0';

      for (t = 0; t < KWS_TPL_PER_WORD; t++)
        {
          s_bank[w][t].data   = NULL;
          s_bank[w][t].frames = 0;
        }
    }

  s_word_count = 0;
  s_threshold  = KWS_THRESH_DEFAULT;
  s_ready      = 0;

  syslog(LOG_INFO, "kws: init (words_max=%d tpl/word=%d band=%d thr=%.1f)\n",
         KWS_WORDS_MAX, KWS_TPL_PER_WORD, KWS_DTW_BAND, (double)s_threshold);
  return OK;
}

/****************************************************************************
 * Name: kws_deinit
 ****************************************************************************/

void kws_deinit(void)
{
  kws_wipe();
}

/****************************************************************************
 * Name: kws_register_word
 ****************************************************************************/

int kws_register_word(const char *name)
{
  int idx;

  if (name == NULL || name[0] == '\0')
    {
      return -EINVAL;
    }

  if (s_word_count >= KWS_WORDS_MAX)
    {
      return -ENOSPC;
    }

  idx = s_word_count;
  strncpy(s_word_name[idx], name, KWS_NAME_MAX - 1);
  s_word_name[idx][KWS_NAME_MAX - 1] = '\0';
  s_tpl_count[idx] = 0;
  s_word_count++;

  syslog(LOG_INFO, "kws: word %d = '%s'\n", idx, s_word_name[idx]);
  return idx;
}

/****************************************************************************
 * Name: kws_word_count
 ****************************************************************************/

int kws_word_count(void)
{
  return s_word_count;
}

/****************************************************************************
 * Name: kws_word_name
 ****************************************************************************/

const char *kws_word_name(int word_idx)
{
  if (word_idx < 0 || word_idx >= s_word_count)
    {
      return "";
    }

  return s_word_name[word_idx];
}

/****************************************************************************
 * Name: kws_add_template
 ****************************************************************************/

int kws_add_template(int word_idx, const int16_t *feat, int frames)
{
  kws_template_t *tpl;

  if (word_idx < 0 || word_idx >= s_word_count)
    {
      return -EINVAL;
    }

  if (feat == NULL)
    {
      return -EINVAL;
    }

  if (frames < KWS_MIN_TPL_FRAMES || frames > KWS_MAX_FRAMES)
    {
      return -EINVAL;
    }

  if (s_tpl_count[word_idx] >= KWS_TPL_PER_WORD)
    {
      return -ENOSPC;
    }

  tpl = &s_bank[word_idx][s_tpl_count[word_idx]];
  tpl->data = (int16_t *)malloc((size_t)frames * FEAT_DIM * sizeof(int16_t));

  if (tpl->data == NULL)
    {
      return -ENOMEM;
    }

  memcpy(tpl->data, feat, (size_t)frames * FEAT_DIM * sizeof(int16_t));
  tpl->frames = frames;
  s_tpl_count[word_idx]++;

  syslog(LOG_INFO, "kws: template stored word=%d take=%d frames=%d\n",
         word_idx, s_tpl_count[word_idx], frames);
  return OK;
}

/****************************************************************************
 * Name: kws_learn
 ****************************************************************************/

int kws_learn(int word_idx, const int16_t *pcm, int nsamples)
{
  int frames = 0;
  int ret;

  if (word_idx < 0 || word_idx >= s_word_count)
    {
      return -EINVAL;
    }

  ret = dsp_mfcc_compute(pcm, nsamples, s_scratch, KWS_MAX_FRAMES, &frames);

  if (ret != OK)
    {
      syslog(LOG_ERR, "kws: learn failed, feature extraction ret=%d\n", ret);
      return ret;
    }

  if (frames < KWS_MIN_TPL_FRAMES)
    {
      syslog(LOG_ERR, "kws: learn failed, only %d frames\n", frames);
      return -EINVAL;
    }

  ret = kws_add_template(word_idx, s_scratch, frames);

  if (ret != OK)
    {
      return ret;
    }

  if (s_tpl_count[word_idx] >= KWS_TPL_PER_WORD)
    {
      /* Last take of this word: calibrate the threshold right away. */
      kws_finalize_training();
    }

  return s_tpl_count[word_idx];
}

/****************************************************************************
 * Name: kws_finalize_training
 ****************************************************************************/

int kws_finalize_training(void)
{
  float intra_max = 0.0f;
  float cross_min = 1.0e9f;
  int   ok        = 1;
  int   w;
  int   w1;
  int   w2;

  syslog(LOG_INFO, "kws: ---- intra-class spread ----\n");

  for (w = 0; w < s_word_count; w++)
    {
      float sum   = 0.0f;
      int   pairs = 0;
      int   i;
      int   j;

      if (s_tpl_count[w] < 2)
        {
          syslog(LOG_WARNING, "kws: word %d ('%s') has %d take(s), "
                 "need >= 2\n", w, s_word_name[w], s_tpl_count[w]);
          ok = 0;
          continue;
        }

      for (i = 0; i < s_tpl_count[w]; i++)
        {
          for (j = i + 1; j < s_tpl_count[w]; j++)
            {
              float d = kws_dtw(s_bank[w][i].data, s_bank[w][i].frames,
                                s_bank[w][j].data, s_bank[w][j].frames);

              sum += d;
              pairs++;

              if (d > intra_max)
                {
                  intra_max = d;
                }
            }
        }

      syslog(LOG_INFO, "kws: %-8s intra avg=%.1f (%d pairs)\n",
             s_word_name[w], (double)(pairs ? sum / (float)pairs : 0.0f),
             pairs);
    }

  /* Inter-class minimum (diagnostic only). */

  for (w1 = 0; w1 < s_word_count; w1++)
    {
      if (s_tpl_count[w1] < 1)
        {
          continue;
        }

      for (w2 = w1 + 1; w2 < s_word_count; w2++)
        {
          float d;

          if (s_tpl_count[w2] < 1)
            {
              continue;
            }

          d = kws_dtw(s_bank[w1][0].data, s_bank[w1][0].frames,
                      s_bank[w2][0].data, s_bank[w2][0].frames);

          if (d < cross_min)
            {
              cross_min = d;
            }
        }
    }

  s_threshold = intra_max * KWS_INTRA_SCALE;

  if (s_threshold < KWS_THRESH_MIN)
    {
      s_threshold = KWS_THRESH_MIN;
    }

  s_ready = ok;

  syslog(LOG_INFO,
         "kws: threshold=%.1f (intra max %.1f x%.2f) cross min=%.1f %s\n",
         (double)s_threshold, (double)intra_max, (double)KWS_INTRA_SCALE,
         (double)cross_min,
         (cross_min > s_threshold) ? "separable" : "!! classes too close");

  if (cross_min <= intra_max * 1.1f)
    {
      ok = 0;
      s_ready = 0;
    }

  return ok ? OK : -ERANGE;
}

/****************************************************************************
 * Name: kws_classify
 ****************************************************************************/

int kws_classify(const int16_t *feat, int frames, float *score_out)
{
  int   best_w = -1;
  float best_d = 1.0e9f;
  int   w;
  int   t;

  if (score_out != NULL)
    {
      *score_out = -1.0f;
    }

  if (s_ready == 0)
    {
      return -1;
    }

  if (feat == NULL || frames < 1 || frames > KWS_MAX_FRAMES)
    {
      return -1;
    }

  for (w = 0; w < s_word_count; w++)
    {
      for (t = 0; t < s_tpl_count[w]; t++)
        {
          float d = kws_dtw(feat, frames,
                            s_bank[w][t].data, s_bank[w][t].frames);

          if (d < best_d)
            {
              best_d = d;
              best_w = w;
            }
        }
    }

  if (score_out != NULL)
    {
      *score_out = best_d;
    }

  if (best_w >= 0 && best_d <= s_threshold)
    {
      return best_w;
    }

  return -1;
}

/****************************************************************************
 * Name: kws_recognize
 ****************************************************************************/

int kws_recognize(const int16_t *pcm, int nsamples, float *score_out)
{
  int frames = 0;
  int ret;

  if (score_out != NULL)
    {
      *score_out = -1.0f;
    }

  if (pcm == NULL)
    {
      return -1;
    }

  ret = dsp_mfcc_compute(pcm, nsamples, s_scratch, KWS_MAX_FRAMES, &frames);

  if (ret != OK)
    {
      return -1;
    }

  return kws_classify(s_scratch, frames, score_out);
}

/****************************************************************************
 * Name: kws_wipe
 ****************************************************************************/

void kws_wipe(void)
{
  int w;
  int t;

  for (w = 0; w < KWS_WORDS_MAX; w++)
    {
      for (t = 0; t < KWS_TPL_PER_WORD; t++)
        {
          if (s_bank[w][t].data != NULL)
            {
              free(s_bank[w][t].data);
              s_bank[w][t].data   = NULL;
              s_bank[w][t].frames = 0;
            }
        }

      s_tpl_count[w]     = 0;
      s_word_name[w][0]  = '\0';
    }

  s_word_count = 0;
  s_threshold  = KWS_THRESH_DEFAULT;
  s_ready      = 0;
}

/****************************************************************************
 * Name: kws_save
 ****************************************************************************/

int kws_save(const char *path)
{
  uint8_t hdr[18];
  int     fd;
  int     ret;
  int     w;
  int     t;

  if (path == NULL)
    {
      path = KWS_STORE_PATH_DEFAULT;
    }

  if (s_word_count <= 0)
    {
      return -EINVAL;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);

  if (fd < 0)
    {
      syslog(LOG_ERR, "kws: save failed, cannot open %s\n", path);
      return -ENOENT;
    }

  kws_put_u32(&hdr[0],  KWS_MAGIC);
  kws_put_u32(&hdr[4],  KWS_FMT_V2);
  kws_put_u16(&hdr[8],  (uint16_t)FEAT_DIM);
  kws_put_u16(&hdr[10], (uint16_t)s_word_count);
  kws_put_u16(&hdr[12], (uint16_t)KWS_TPL_PER_WORD);

  /* Layout: [0..3] magic [4..7] version [8..9] dim [10..11] words
   *         [12..13] per  [14..17] threshold. */

  kws_put_float(&hdr[14], s_threshold);

  ret = kws_write_all(fd, hdr, 18);

  if (ret == OK)
    {
      for (w = 0; w < s_word_count && ret == OK; w++)
        {
          ret = kws_write_all(fd, s_word_name[w], KWS_NAME_MAX);
        }
    }

  for (w = 0; w < s_word_count && ret == OK; w++)
    {
      uint8_t cnt[2];

      kws_put_u16(cnt, (uint16_t)s_tpl_count[w]);
      ret = kws_write_all(fd, cnt, 2);

      for (t = 0; t < s_tpl_count[w] && ret == OK; t++)
        {
          uint8_t fr[2];

          kws_put_u16(fr, (uint16_t)s_bank[w][t].frames);
          ret = kws_write_all(fd, fr, 2);

          if (ret == OK)
            {
              ret = kws_write_all(fd, s_bank[w][t].data,
                                  (size_t)s_bank[w][t].frames * FEAT_DIM *
                                  sizeof(int16_t));
            }
        }
    }

  close(fd);

  if (ret != OK)
    {
      syslog(LOG_ERR, "kws: save failed writing %s\n", path);
      return ret;
    }

  syslog(LOG_INFO, "kws: %d word(s) written to %s (threshold=%.1f)\n",
         s_word_count, path, (double)s_threshold);
  return OK;
}

/****************************************************************************
 * Name: kws_load
 ****************************************************************************/

int kws_load(const char *path)
{
  uint8_t hdr[18];
  uint32_t magic;
  uint32_t version;
  uint16_t dim;
  uint16_t words;
  uint16_t per;
  int      fd;
  int      ret;
  int      w;
  int      ok = 1;

  if (path == NULL)
    {
      path = KWS_STORE_PATH_DEFAULT;
    }

  fd = open(path, O_RDONLY | O_BINARY);

  if (fd < 0)
    {
      return -ENOENT;
    }

  ret = kws_read_all(fd, hdr, 18);

  if (ret != OK)
    {
      close(fd);
      return -EINVAL;
    }

  magic   = kws_get_u32(&hdr[0]);
  version = kws_get_u32(&hdr[4]);
  dim     = kws_get_u16(&hdr[8]);
  words   = kws_get_u16(&hdr[10]);
  per     = kws_get_u16(&hdr[12]);

  if (magic != KWS_MAGIC ||
      (version != KWS_FMT_V1 && version != KWS_FMT_V2) ||
      dim != (uint16_t)FEAT_DIM ||
      words == 0 || words > (uint16_t)KWS_WORDS_MAX ||
      per != (uint16_t)KWS_TPL_PER_WORD)
    {
      close(fd);
      syslog(LOG_ERR,
             "kws: %s unsupported (magic=%08x ver=%u dim=%u words=%u)\n",
             path, (unsigned)magic, (unsigned)version,
             (unsigned)dim, (unsigned)words);
      return -EINVAL;
    }

  /* Replace whatever is in RAM. */

  kws_wipe();

  s_threshold = kws_get_float(&hdr[14]);
  s_word_count = (int)words;

  if (version >= KWS_FMT_V2)
    {
      for (w = 0; w < s_word_count && ret == OK; w++)
        {
          ret = kws_read_all(fd, s_word_name[w], KWS_NAME_MAX);
          s_word_name[w][KWS_NAME_MAX - 1] = '\0';
        }
    }
  else
    {
      /* Arduino files carry no names: rebuild a placeholder. */

      for (w = 0; w < s_word_count; w++)
        {
          snprintf(s_word_name[w], KWS_NAME_MAX, "w%d", w);
        }
    }

  for (w = 0; w < s_word_count && ok && ret == OK; w++)
    {
      uint8_t cnt[2];
      int     count;
      int     t;

      ret = kws_read_all(fd, cnt, 2);

      if (ret != OK)
        {
          break;
        }

      count = (int)kws_get_u16(cnt);

      if (count > KWS_TPL_PER_WORD)
        {
          ok = 0;
          break;
        }

      for (t = 0; t < count && ok && ret == OK; t++)
        {
          uint8_t   fr[2];
          int       frames;
          int16_t  *data;

          ret = kws_read_all(fd, fr, 2);

          if (ret != OK)
            {
              break;
            }

          frames = (int)kws_get_u16(fr);

          if (frames < 1 || frames > KWS_MAX_FRAMES)
            {
              ok = 0;
              break;
            }

          data = (int16_t *)malloc((size_t)frames * FEAT_DIM *
                                   sizeof(int16_t));

          if (data == NULL)
            {
              ok = 0;
              break;
            }

          ret = kws_read_all(fd, data,
                             (size_t)frames * FEAT_DIM * sizeof(int16_t));

          if (ret != OK)
            {
              free(data);
              break;
            }

          s_bank[w][s_tpl_count[w]].data   = data;
          s_bank[w][s_tpl_count[w]].frames = frames;
          s_tpl_count[w]++;
        }
    }

  close(fd);

  if (ret != OK)
    {
      ok = 0;
    }

  for (w = 0; w < s_word_count; w++)
    {
      if (s_tpl_count[w] < 1)
        {
          ok = 0;
        }
    }

  s_ready = (ok != 0 && s_threshold > 0.0f) ? 1 : 0;

  syslog(LOG_INFO, "kws: load %s from %s (threshold=%.1f, %d template(s))\n",
         s_ready ? "ok" : "incomplete", path, (double)s_threshold,
         kws_total_templates());

  return s_ready ? OK : -EINVAL;
}

/****************************************************************************
 * Name: kws_set_threshold
 ****************************************************************************/

void kws_set_threshold(float threshold)
{
  if (threshold > 5.0f)
    {
      s_threshold = threshold;
    }
}

/****************************************************************************
 * Name: kws_get_threshold
 ****************************************************************************/

float kws_get_threshold(void)
{
  return s_threshold;
}

/****************************************************************************
 * Name: kws_is_ready
 ****************************************************************************/

int kws_is_ready(void)
{
  return s_ready ? 1 : 0;
}

/****************************************************************************
 * Name: kws_force_ready
 ****************************************************************************/

void kws_force_ready(float threshold)
{
  if (threshold > 5.0f)
    {
      s_threshold = threshold;
    }

  s_ready = 1;
}

/****************************************************************************
 * Name: kws_total_templates
 ****************************************************************************/

int kws_total_templates(void)
{
  int n = 0;
  int w;

  for (w = 0; w < s_word_count; w++)
    {
      n += s_tpl_count[w];
    }

  return n;
}

/****************************************************************************
 * Name: kws_first_incomplete
 ****************************************************************************/

void kws_first_incomplete(int *word, int *take)
{
  int w;

  for (w = 0; w < s_word_count; w++)
    {
      if (s_tpl_count[w] < KWS_TPL_PER_WORD)
        {
          if (word != NULL)
            {
              *word = w;
            }

          if (take != NULL)
            {
              *take = s_tpl_count[w];
            }

          return;
        }
    }

  if (word != NULL)
    {
      *word = 0;
    }

  if (take != NULL)
    {
      *take = 0;
    }
}
