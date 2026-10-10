/****************************************************************************
 * apps/examples/desktop_companion/novel_reader.c
 *
 * Audiobook feature -- pure text loading + segmentation logic.
 *
 * See novel_reader.h for the splitting/contract description. The module owns a
 * single process-wide reader instance (like voice_bridge.c owns the endpoint),
 * so main.c just calls the flat API without passing a handle around.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "novel_reader.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Initial segment-table capacity; grown by doubling as needed. */
#define SEG_INIT           16

/* Characters that end a speakable segment. */
#define IS_TERM(c)                                                       \
  ((c) == '\0' || (c) == '\n' || (c) == '\r' || (c) == '。' ||          \
   (c) == '！' || (c) == '？' || (c) == '!'  || (c) == '?'  ||          \
   (c) == '…'  || (c) == '；' || (c) == ';')

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct novel_reader_s
{
  char    *text;       /* heap copy of the whole novel (mutated by splitting) */
  size_t   text_len;
  char   **seg;        /* segment start pointers into text[]                   */
  int     *chap;       /* chapter index per segment (1-based)                 */
  int     *line;       /* line-within-chapter per segment (1-based)           */
  int      nseg;
  int      cur;        /* current segment index (0-based)                    */
  bool     owns_text;  /* whether text[] must be freed on close              */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct novel_reader_s g_novel;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: is_chapter_heading
 *
 * Description:
 *   Returns true when `s` looks like a chapter heading: optional leading
 *   spaces, then " 第 " + one or more digits / Chinese numerals + one of
 *   章/回/卷/部.
 ****************************************************************************/

static bool is_chapter_heading(const char *s)
{
  static const char *nums = "0123456789零一二三四五六七八九十百千";
  const char *p = s;

  while (*p != '\0' && isspace((unsigned char)*p))
    {
      p++;
    }

  if (p[0] != '第')
    {
      return false;
    }

  p++;
  bool has_num = false;
  while (*p != '\0' && strchr(nums, *p) != NULL)
    {
      has_num = true;
      p++;
    }

  if (!has_num)
    {
      return false;
    }

  return (p[0] == '章' || p[0] == '回' || p[0] == '卷' || p[0] == '部');
}

/****************************************************************************
 * Name: reader_reset
 ****************************************************************************/

static void reader_reset(struct novel_reader_s *n)
{
  if (n->owns_text && n->text != NULL)
    {
      free(n->text);
    }

  if (n->seg != NULL)
    {
      free(n->seg);
    }

  if (n->chap != NULL)
    {
      free(n->chap);
    }

  if (n->line != NULL)
    {
      free(n->line);
    }

  memset(n, 0, sizeof(*n));
  n->cur = 0;
}

/****************************************************************************
 * Name: reader_build
 *
 * Description:
 *   Take ownership of `text` (heap, mutable) and split it into segments.
 *   Returns OK or -ENOENT when no segment could be extracted.
 ****************************************************************************/

static int reader_build(struct novel_reader_s *n, char *text, size_t len,
                        bool owns)
{
  char  *seg_start = NULL;
  int    cur_chap  = 1;
  int    line_chap = 0;
  int    cap       = SEG_INIT;
  size_t i;

  reader_reset(n);

  n->text      = text;
  n->text_len  = len;
  n->owns_text = owns;
  n->cur       = 0;

  n->seg  = (char **)malloc((size_t)cap * sizeof(char *));
  n->chap = (int *)malloc((size_t)cap * sizeof(int));
  n->line = (int *)malloc((size_t)cap * sizeof(int));
  if (n->seg == NULL || n->chap == NULL || n->line == NULL)
    {
      reader_reset(n);
      return -ENOMEM;
    }

  for (i = 0; i <= len; i++)
    {
      char c = (i < len) ? text[i] : '\0';

      if (seg_start == NULL)
        {
          if (c != '\0' && !IS_TERM(c) && !isspace((unsigned char)c))
            {
              seg_start = &text[i];
            }

          continue;
        }

      if (!IS_TERM(c))
        {
          continue;
        }

      /* End of a segment at index i. */
      text[i] = '\0';

      /* Trim trailing whitespace (keeps the NUL we just wrote). */
      char *t = &text[i];
      while (t > seg_start && isspace((unsigned char)t[-1]))
        {
          t--;
        }

      *t = '\0';

      if (seg_start[0] == '\0')
        {
          seg_start = NULL;   /* whitespace-only segment, discard */
          continue;
        }

      if (is_chapter_heading(seg_start))
        {
          cur_chap++;
          line_chap = 0;
        }

      line_chap++;

      if (n->nseg >= cap)
        {
          int    ncap = cap * 2;
          char **ns  = (char **)realloc(n->seg, (size_t)ncap * sizeof(char *));
          int   *nc  = (int *)realloc(n->chap, (size_t)ncap * sizeof(int));
          int   *nl  = (int *)realloc(n->line, (size_t)ncap * sizeof(int));

          if (ns == NULL || nc == NULL || nl == NULL)
            {
              reader_reset(n);
              return -ENOMEM;
            }

          n->seg  = ns;
          n->chap = nc;
          n->line = nl;
          cap     = ncap;
        }

      n->seg[n->nseg]  = seg_start;
      n->chap[n->nseg] = cur_chap;
      n->line[n->nseg] = line_chap;
      n->nseg++;

      seg_start = NULL;
    }

  if (n->nseg == 0)
    {
      reader_reset(n);
      return -ENOENT;
    }

  return OK;
}

/****************************************************************************
 * Name: reader_read_file
 *
 * Description:
 *   Open `path` and read up to AIVOX3_READ_NOVEL_MAX_BYTES into a freshly
 *   allocated, NUL-terminated heap buffer. Returns the buffer (caller owns) or
 *   NULL on any failure (errno set appropriately by open/read).
 ****************************************************************************/

static char *reader_read_file(const char *path, size_t *out_len)
{
  int   fd;
  char *buf;
  size_t got = 0;
  size_t cap = (size_t)AIVOX3_READ_NOVEL_MAX_BYTES;

  *out_len = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return NULL;
    }

  buf = (char *)malloc(cap + 1);
  if (buf == NULL)
    {
      close(fd);
      return NULL;
    }

  while (got < cap)
    {
      ssize_t r = read(fd, buf + got, cap - got);
      if (r < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          break;
        }

      if (r == 0)
        {
          break;                 /* EOF */
        }

      got += (size_t)r;
    }

  close(fd);

  if (got == 0)
    {
      free(buf);
      return NULL;
    }

  buf[got] = '\0';
  *out_len = got;
  return buf;
}

/****************************************************************************
 * Name: reader_write_file
 *
 * Description:
 *   Best-effort write of `text` (len bytes) to `path` (O_WRONLY|O_CREAT|
 *   O_TRUNC). Returns OK on a complete write, negated errno otherwise.
 ****************************************************************************/

static int reader_write_file(const char *path, const char *text, size_t len)
{
  int   fd;
  size_t done = 0;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      return -errno;
    }

  while (done < len)
    {
      ssize_t w = write(fd, text + done, len - done);
      if (w < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          close(fd);
          return -errno;
        }

      if (w == 0)
        {
          break;
        }

      done += (size_t)w;
    }

  close(fd);
  return (done == len) ? OK : -EIO;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: novel_reader_open
 ****************************************************************************/

int novel_reader_open(const char *path)
{
  const char *use = (path != NULL && path[0] != '\0') ?
                     path : AIVOX3_READ_NOVEL_PATH;
  char  *src;
  size_t srclen;
  int    ret;

  reader_reset(&g_novel);

  /* 1) Try the on-board file first. reader_build() TAKES OWNERSHIP of the
   * buffer it is given and frees it itself on every exit path (success or
   * failure), so the caller must never free(src) afterwards. */
  src = reader_read_file(use, &srclen);
  if (src != NULL && srclen > 0)
    {
      ret = reader_build(&g_novel, src, srclen, true);
      if (ret == OK)
        {
          return OK;
        }

      /* reader_build already released src; fall through to the seed path. */
    }

  /* 2) No file yet: seed the built-in sample onto FFat, then read it back so
   *    the user can later edit/replace it in place. */
  {
    size_t slen = strlen(AIVOX3_BUILTIN_NOVEL);
    if (reader_write_file(use, AIVOX3_BUILTIN_NOVEL, slen) == OK)
      {
        src = reader_read_file(use, &srclen);
        if (src != NULL && srclen > 0)
          {
            ret = reader_build(&g_novel, src, srclen, true);
            if (ret == OK)
              {
                return OK;
              }

            /* reader_build already released src. */
          }
      }
  }

  /* 3) Last resort: keep the built-in sample in memory. We still copy it to a
   *    heap buffer because the splitter mutates the text in place. */
  {
    size_t slen = strlen(AIVOX3_BUILTIN_NOVEL);
    char  *copy = (char *)malloc(slen + 1);
    if (copy == NULL)
      {
        return -ENOMEM;
      }

    memcpy(copy, AIVOX3_BUILTIN_NOVEL, slen);
    copy[slen] = '\0';
    ret = reader_build(&g_novel, copy, slen, true);
    if (ret == OK)
      {
        return OK;
      }

    /* reader_build already released copy. */
  }

  return -ENOENT;
}

/****************************************************************************
 * Name: novel_reader_close
 ****************************************************************************/

int novel_reader_close(void)
{
  reader_reset(&g_novel);
  return OK;
}

/****************************************************************************
 * Name: novel_reader_current
 ****************************************************************************/

int novel_reader_current(char *out, size_t cap, size_t *out_len)
{
  size_t l;

  if (g_novel.cur < 0 || g_novel.cur >= g_novel.nseg)
    {
      if (out_len != NULL)
        {
          *out_len = 0;
        }

      return -ENOENT;
    }

  l = strlen(g_novel.seg[g_novel.cur]);
  if (out != NULL && cap > 0)
    {
      strncpy(out, g_novel.seg[g_novel.cur], cap - 1);
      out[cap - 1] = '\0';
    }

  if (out_len != NULL)
    {
      *out_len = l;
    }

  return OK;
}

/****************************************************************************
 * Name: novel_reader_next
 ****************************************************************************/

int novel_reader_next(void)
{
  if (g_novel.nseg == 0 || g_novel.cur + 1 >= g_novel.nseg)
    {
      return -ENOENT;
    }

  g_novel.cur++;
  return OK;
}

/****************************************************************************
 * Name: novel_reader_prev
 ****************************************************************************/

int novel_reader_prev(void)
{
  if (g_novel.nseg == 0 || g_novel.cur <= 0)
    {
      return -ENOENT;
    }

  g_novel.cur--;
  return OK;
}

/****************************************************************************
 * Name: novel_reader_chapter
 ****************************************************************************/

int novel_reader_chapter(void)
{
  if (g_novel.cur < 0 || g_novel.cur >= g_novel.nseg)
    {
      return 1;
    }

  return g_novel.chap[g_novel.cur];
}

/****************************************************************************
 * Name: novel_reader_line
 ****************************************************************************/

int novel_reader_line(void)
{
  if (g_novel.cur < 0 || g_novel.cur >= g_novel.nseg)
    {
      return 0;
    }

  return g_novel.line[g_novel.cur];
}

/****************************************************************************
 * Name: novel_reader_total
 ****************************************************************************/

int novel_reader_total(void)
{
  return g_novel.nseg;
}

/****************************************************************************
 * Name: novel_reader_progress
 ****************************************************************************/

int novel_reader_progress(void)
{
  if (g_novel.nseg <= 0)
    {
      return 0;
    }

  return (g_novel.cur * 100) / g_novel.nseg;
}
