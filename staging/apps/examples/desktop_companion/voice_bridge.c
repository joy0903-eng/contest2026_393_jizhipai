/****************************************************************************
 * apps/examples/desktop_companion/voice_bridge.c
 *
 * Incremental client for the PC conversational voice bridge.
 *
 * Protocol (read off bridge_chat.py, do_POST at :527, process_turn at :410):
 *
 *   request
 *     POST /api/turn
 *     Content-Type: application/octet-stream   (binary path)
 *       body = raw int16 LE PCM, 16000 Hz, mono
 *     Content-Type: application/json           (text path)
 *       body = {"text": "<user utterance>"}
 *
 *   response 200, Content-Type: application/json; charset=utf-8
 *     {"transcript": "...", "reply": "...", "audio": "<base64 PCM>"}
 *
 *   Notes that shaped this implementation:
 *     - Python's json.dumps default separators are (", ", ": ") so there IS
 *       a space after the colon. Any parser must therefore skip whitespace
 *       after ':' rather than match `"audio":"`.
 *     - ensure_ascii=False means Chinese arrives as raw UTF-8 bytes, but
 *       json.dumps still escapes control characters, '"' and '\', so \uXXXX
 *       sequences must be decoded.
 *     - `audio` may legitimately be "" when the bridge decided the input was
 *       ambient noise; that is a successful turn with nothing to play.
 *     - `reply` may legitimately be "" too (same filtering).
 *
 * Everything below is written so that the response is consumed in one pass
 * straight off the socket: no full-body buffer exists anywhere.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "config.h"
#include "http_util.h"
#include "voice_bridge.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PCM sink growth policy. Starts small, grows in step, hard-stops at the
 * configured ceiling so one verbose answer can never exhaust the heap.
 */

#define SINK_INIT_BYTES    (128u * 1024u)
#define SINK_STEP_BYTES    (128u * 1024u)

#define OCTET_STREAM_HDR   "Content-Type: application/octet-stream\r\n"
#define JSON_HDR           "Content-Type: application/json\r\n"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Growable byte sink used for the decoded TTS PCM. */

struct pcm_sink_s
{
  uint8_t *buf;
  size_t   len;
  size_t   cap;
  bool     stopped;   /* ceiling reached or OOM: silently drop the rest */
};

/* Base64 stream decoder state. */

struct b64_ctx_s
{
  uint32_t     acc;
  unsigned int nbits;
};

enum bridge_field_e
{
  FIELD_NONE = 0,
  FIELD_TRANSCRIPT,
  FIELD_REPLY,
  FIELD_AUDIO
};

/* Tiny streaming JSON reader: recognises {"k": "v", ...} of strings only,
 * which is exactly what the bridge emits. Nested objects/arrays are skipped
 * wholesale rather than parsed, so a future non-string member cannot derail
 * the caller.
 */

enum json_state_e
{
  JS_IDLE = 0,        /* between tokens                                   */
  JS_KEY,             /* reading a member name                            */
  JS_AFTER_KEY,       /* after the name, waiting for ':'                  */
  JS_PRE_VALUE,       /* after ':', waiting for the value's first char    */
  JS_VALUE_STRING,    /* reading a string value                           */
  JS_UNICODE          /* inside a \uXXXX escape                           */
};

struct turn_parser_s
{
  int                   state;
  bool                  escape;
  int                   field;
  char                  key[24];
  size_t                key_len;

  /* \uXXXX accumulation (with UTF-16 surrogate pair merging). */
  unsigned int          uni_n;
  uint32_t              uni_cp;
  uint32_t              uni_high;

  struct b64_ctx_s      b64;
  uint8_t               tri[3];
  unsigned int          tri_n;

  struct pcm_sink_s     sink;
  bridge_turn_result_t *res;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char     g_host[BRIDGE_HOST_MAX] = AIVOX3_BRIDGE_HOST_DEFAULT;
static uint16_t g_port                  = (uint16_t)AIVOX3_BRIDGE_PORT_DEFAULT;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sink_init / sink_release
 ****************************************************************************/

static void sink_init(struct pcm_sink_s *s)
{
  s->buf     = NULL;
  s->len     = 0;
  s->cap     = 0;
  s->stopped = false;
}

/****************************************************************************
 * Name: sink_grow
 *
 * Description:
 *   Add one growth step, up to AIVOX3_TTS_PCM_MAX_BYTES.
 *
 * Returned Value:
 *   OK, or -ENOMEM when the ceiling was already reached or the allocator
 *   refused (in both cases the sink is marked stopped).
 ****************************************************************************/

static int sink_grow(struct pcm_sink_s *s)
{
  size_t   want;
  uint8_t *nbuf;

  if (s->stopped)
    {
      return -ENOMEM;
    }

  want = (s->cap == 0) ? (size_t)SINK_INIT_BYTES : s->cap + SINK_STEP_BYTES;
  if (want > (size_t)AIVOX3_TTS_PCM_MAX_BYTES)
    {
      want = (size_t)AIVOX3_TTS_PCM_MAX_BYTES;
    }

  if (want == s->cap)
    {
      s->stopped = true;
      return -ENOMEM;
    }

  nbuf = (uint8_t *)realloc(s->buf, want);
  if (nbuf == NULL)
    {
      s->stopped = true;
      return -ENOMEM;
    }

  s->buf = nbuf;
  s->cap = want;
  return OK;
}

/****************************************************************************
 * Name: sink_put
 *
 * Description:
 *   Append `n` bytes, growing as needed. Bytes that would overflow the
 *   configured ceiling are dropped (and remembered) rather than failing
 *   the turn -- a truncated answer is better than no answer.
 ****************************************************************************/

static void sink_put(struct pcm_sink_s *s, const uint8_t *data, size_t n)
{
  while (n > 0)
    {
      size_t room  = (s->cap > s->len) ? (s->cap - s->len) : 0;
      size_t chunk = (n < room) ? n : room;

      if (chunk > 0)
        {
          memcpy(s->buf + s->len, data, chunk);
          s->len += chunk;
          data   += chunk;
          n      -= chunk;
        }

      if (n == 0)
        {
          break;
        }

      /* Still short: either grow, or give up on the remainder. */

      if (s->cap >= (size_t)AIVOX3_TTS_PCM_MAX_BYTES || sink_grow(s) != OK)
        {
          s->stopped = true;
          return;
        }
    }
}

/****************************************************************************
 * Name: b64_reset / b64_digit / b64_push
 ****************************************************************************/

static void b64_reset(struct b64_ctx_s *b)
{
  b->acc   = 0;
  b->nbits = 0;
}

static int b64_digit(char c)
{
  if (c >= 'A' && c <= 'Z')
    {
      return c - 'A';
    }

  if (c >= 'a' && c <= 'z')
    {
      return c - 'a' + 26;
    }

  if (c >= '0' && c <= '9')
    {
      return c - '0' + 52;
    }

  if (c == '+')
    {
      return 62;
    }

  if (c == '/')
    {
      return 63;
    }

  return -1;            /* '=', whitespace, anything unexpected */
}

/* Feed one 6-bit symbol; emit a full byte into `out` every 8 accumulated
 * bits. Returned true means a byte was written. */

static bool b64_push(struct b64_ctx_s *b, int v, uint8_t *out)
{
  b->acc   = (b->acc << 6) | (uint32_t)v;
  b->nbits = (unsigned int)(b->nbits + 6);

  if (b->nbits >= 8)
    {
      b->nbits = (unsigned int)(b->nbits - 8);
      *out     = (uint8_t)((b->acc >> b->nbits) & 0xffu);
      return true;
    }

  return false;
}

/****************************************************************************
 * Name: utf8_put
 *
 * Description:
 *   Encode one code point as UTF-8 into out[]. Returns the byte count.
 ****************************************************************************/

static size_t utf8_put(uint32_t cp, uint8_t *out)
{
  if (cp < 0x80u)
    {
      out[0] = (uint8_t)cp;
      return 1;
    }

  if (cp < 0x800u)
    {
      out[0] = (uint8_t)(0xc0u | (cp >> 6));
      out[1] = (uint8_t)(0x80u | (cp & 0x3fu));
      return 2;
    }

  if (cp < 0x10000u)
    {
      out[0] = (uint8_t)(0xe0u | (cp >> 12));
      out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
      out[2] = (uint8_t)(0x80u | (cp & 0x3fu));
      return 3;
    }

  out[0] = (uint8_t)(0xf0u | (cp >> 18));
  out[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
  out[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
  out[3] = (uint8_t)(0x80u | (cp & 0x3fu));
  return 4;
}

/****************************************************************************
 * Name: text_append
 *
 * Description:
 *   Append one byte to whichever bounded text field is currently open.
 *   Silently drops bytes past the field's capacity.
 ****************************************************************************/

static void text_append(struct turn_parser_s *p, char c)
{
  size_t len;

  if (p->field == FIELD_REPLY)
    {
      len = strlen(p->res->reply);
      if (len + 1 < BRIDGE_REPLY_MAX)
        {
          p->res->reply[len]     = c;
          p->res->reply[len + 1] = '\0';
        }

      return;
    }

  if (p->field == FIELD_TRANSCRIPT)
    {
      len = strlen(p->res->transcript);
      if (len + 1 < BRIDGE_TRANSCRIPT_MAX)
        {
          p->res->transcript[len]     = c;
          p->res->transcript[len + 1] = '\0';
        }
    }
}

/****************************************************************************
 * Name: emit_codepoint
 *
 * Description:
 *   Flush a decoded \uXXXX code point, merging UTF-16 surrogate pairs.
 ****************************************************************************/

static void emit_codepoint(struct turn_parser_s *p, uint32_t cp)
{
  uint8_t tmp[4];
  size_t  n;
  size_t  i;

  if (cp >= 0xd800u && cp <= 0xdbffu)
    {
      p->uni_high = cp;         /* wait for the low surrogate */
      return;
    }

  if (cp >= 0xdc00u && cp <= 0xdfffu && p->uni_high != 0u)
    {
      cp = 0x10000u + ((p->uni_high - 0xd800u) << 10) + (cp - 0xdc00u);
    }

  p->uni_high = 0u;
  n = utf8_put(cp, tmp);

  for (i = 0; i < n; i++)
    {
      text_append(p, (char)tmp[i]);
    }
}

/****************************************************************************
 * Name: audio_flush
 *
 * Description:
 *   Push (and clear) the partial base64 group held by the parser.
 ****************************************************************************/

static void audio_flush(struct turn_parser_s *p)
{
  if (p->tri_n > 0u)
    {
      sink_put(&p->sink, p->tri, (size_t)p->tri_n);
      p->tri_n = 0u;
    }
}

/****************************************************************************
 * Name: audio_char
 *
 * Description:
 *   Consume one character of the base64 "audio" member.
 ****************************************************************************/

static void audio_char(struct turn_parser_s *p, char c)
{
  int      v;
  uint8_t  byte;

  /* '=' padding is skipped here (b64_digit() rejects it) rather than used to
   * end the group: the 1-2 bytes of the FINAL group are already sitting in
   * tri[] and must not be thrown away, or every payload whose length is not
   * a multiple of 3 loses its tail. audio_flush() emits them when the
   * closing quote arrives.
   */

  v = b64_digit(c);
  if (v < 0)
    {
      return;                       /* '=', whitespace, anything unexpected */
    }

  if (b64_push(&p->b64, v, &byte))
    {
      p->tri[p->tri_n] = byte;
      p->tri_n++;

      if (p->tri_n == 3u)
        {
          sink_put(&p->sink, p->tri, 3u);
          p->tri_n = 0u;
        }
    }
}

/****************************************************************************
 * Name: field_from_key
 ****************************************************************************/

static int field_from_key(const char *key)
{
  if (strcmp(key, "transcript") == 0)
    {
      return FIELD_TRANSCRIPT;
    }

  if (strcmp(key, "reply") == 0)
    {
      return FIELD_REPLY;
    }

  if (strcmp(key, "audio") == 0)
    {
      return FIELD_AUDIO;
    }

  return FIELD_NONE;
}

/****************************************************************************
 * Name: json_feed
 *
 * Description:
 *   Incrementally consume one byte of the JSON response body.
 ****************************************************************************/

static void json_feed(struct turn_parser_s *p, char c)
{
  switch (p->state)
    {
      case JS_KEY:
        if (p->escape)
          {
            p->escape = false;
            break;
          }

        if (c == '\\')
          {
            p->escape = true;
            break;
          }

        if (c == '"')
          {
            p->key[p->key_len] = '\0';
            p->state = JS_AFTER_KEY;
            break;
          }

        if (p->key_len + 1u < sizeof(p->key))
          {
            p->key[p->key_len++] = c;
          }

        break;

      case JS_AFTER_KEY:
        if (c == ':')
          {
            p->state = JS_PRE_VALUE;
          }
        else if (c == ',' || c == '}')
          {
            p->state = JS_IDLE;
          }

        break;

      case JS_PRE_VALUE:
        /* json.dumps writes ": " with a trailing space, so whitespace MUST
         * be tolerated here -- a literal `"audio":"` search would fail.
         */
        if (c == '"')
          {
            p->field  = field_from_key(p->key);
            p->escape = false;
            p->state  = JS_VALUE_STRING;

            if (p->field == FIELD_AUDIO)
              {
                b64_reset(&p->b64);
                p->tri_n = 0u;
              }
          }
        else if (c == '{' || c == '[')
          {
            /* Unsupported nesting: fall back to token scanning. */
            p->field = FIELD_NONE;
            p->state = JS_IDLE;
          }
        else if (c == ',' || c == '}')
          {
            p->state = JS_IDLE;
          }

        break;

      case JS_VALUE_STRING:
        if (p->escape)
          {
            p->escape = false;

            if (p->field == FIELD_AUDIO)
              {
                break;                    /* cannot legitimately occur */
              }

            switch (c)
              {
                case 'n': text_append(p, '\n'); break;
                case 'r': text_append(p, '\r'); break;
                case 't': text_append(p, '\t'); break;
                case 'b': text_append(p, '\b'); break;
                case 'f': text_append(p, '\f'); break;
                case 'u':
                  p->state  = JS_UNICODE;
                  p->uni_n  = 0u;
                  p->uni_cp = 0u;
                  break;
                default:  text_append(p, c);    break;   /* " \ / */
              }

            break;
          }

        if (c == '\\')
          {
            p->escape = true;
            break;
          }

        if (c == '"')
          {
            audio_flush(p);
            p->field = FIELD_NONE;
            p->state = JS_IDLE;
            break;
          }

        if (p->field == FIELD_AUDIO)
          {
            audio_char(p, c);
          }
        else
          {
            text_append(p, c);
          }

        break;

      case JS_UNICODE:
        {
          int hv = -1;

          if (c >= '0' && c <= '9')
            {
              hv = c - '0';
            }
          else if (c >= 'a' && c <= 'f')
            {
              hv = c - 'a' + 10;
            }
          else if (c >= 'A' && c <= 'F')
            {
              hv = c - 'A' + 10;
            }

          if (hv >= 0)
            {
              p->uni_cp = (p->uni_cp << 4) | (uint32_t)hv;
              p->uni_n++;

              if (p->uni_n == 4u)
                {
                  emit_codepoint(p, p->uni_cp);
                  p->state = JS_VALUE_STRING;
                }
            }
          else
            {
              /* Malformed escape: surface it literally and move on. */
              text_append(p, 'u');
              p->state = JS_VALUE_STRING;
            }

          break;
        }

      case JS_IDLE:
      default:
        if (c == '"')
          {
            p->key_len = 0;
            p->escape  = false;
            p->state   = JS_KEY;
          }

        break;
    }
}

/****************************************************************************
 * Name: turn_body_cb
 *
 * Description:
 *   http_request_stream() sink: run each received chunk through the scanner.
 ****************************************************************************/

static int turn_body_cb(const uint8_t *chunk, size_t len, void *arg)
{
  struct turn_parser_s *p = (struct turn_parser_s *)arg;
  size_t i;

  for (i = 0; i < len; i++)
    {
      json_feed(p, (char)chunk[i]);
    }

  return OK;
}

/****************************************************************************
 * Name: parser_init / parser_finish
 ****************************************************************************/

static void parser_init(struct turn_parser_s *p, bridge_turn_result_t *res)
{
  memset(p, 0, sizeof(*p));
  p->state = JS_IDLE;
  p->field = FIELD_NONE;
  p->res   = res;
  sink_init(&p->sink);
  b64_reset(&p->b64);
}

static void parser_finish(struct turn_parser_s *p)
{
  bool truncated;

  /* Terminated string values already flushed; a truncated stream may leave
   * an open group behind -- close it so no decoded sample is lost.
   */

  audio_flush(p);

  truncated = p->sink.stopped;

  p->res->audio     = p->sink.buf;
  p->res->audio_len = p->sink.len;
  p->res->truncated = truncated;
  p->res->ms        = p->sink.len / ((size_t)AIVOX3_AUDIO_RATE *
                                     (size_t)(AIVOX3_AUDIO_BITS / 8));

  /* Ownership of the buffer moved into the result; clearing the sink here
   * documents that it must no longer be touched (bridge_turn_result_release
   * is now responsible for freeing it). `truncated` is preserved so the
   * caller can still report that the tail was dropped.
   */
  sink_init(&p->sink);
}

/****************************************************************************
 * Name: parser_truncated
 *
 * Description:
 *   Accessor for the flag parser_finish() had to preserve across the sink
 *   reset (the sink is emptied so nobody frees the result's buffer twice).
 ****************************************************************************/

static bool parser_truncated(bridge_turn_result_t *res)
{
  return res->truncated;
}

/****************************************************************************
 * Name: json_escape
 *
 * Description:
 *   Copy src into dst applying minimal JSON string escaping. Returns the
 *   number of bytes written (always NUL-terminated).
 ****************************************************************************/

static size_t json_escape(char *dst, size_t dst_cap, const char *src)
{
  size_t j = 0;
  char   c;

  while ((c = *src++) != '\0' && j + 2u < dst_cap)
    {
      switch (c)
        {
          case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
          case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
          case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
          case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
          case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
          default:   dst[j++] = c;                     break;
        }
    }

  dst[j] = '\0';
  return j;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bridge_turn_result_init
 ****************************************************************************/

void bridge_turn_result_init(bridge_turn_result_t *res)
{
  if (res == NULL)
    {
      return;
    }

  res->transcript[0] = '\0';
  res->reply[0]      = '\0';
  res->audio         = NULL;
  res->audio_len     = 0;
  res->ms            = 0;
  res->truncated     = false;
}

/****************************************************************************
 * Name: bridge_turn_result_release
 ****************************************************************************/

void bridge_turn_result_release(bridge_turn_result_t *res)
{
  if (res == NULL)
    {
      return;
    }

  if (res->audio != NULL)
    {
      free(res->audio);
      res->audio = NULL;
    }

  bridge_turn_result_init(res);
}

/****************************************************************************
 * Name: bridge_turn_pcm
 ****************************************************************************/

int bridge_turn_pcm(const int16_t *pcm, int nsamples,
                    bridge_turn_result_t *res)
{
  struct turn_parser_s parser;
  char                 hdr[192];
  size_t               body_len;
  int                  status = 0;
  int                  ret;

  if (pcm == NULL || nsamples <= 0 || res == NULL)
    {
      return -EINVAL;
    }

  bridge_turn_result_release(res);

  body_len = (size_t)nsamples * (size_t)sizeof(int16_t);

  /* A deliberately NON-JSON content type: bridge_chat.py's do_POST switches
   * to `process_turn(pcm_bytes=raw)` whenever "application/json" is absent
   * (:532/:537), so declaring octet-stream routes this into the Vosk ASR
   * path. The extra header is advisory and ignored by the bridge.
   */

  snprintf(hdr, sizeof(hdr), "%sX-Aivox3-Sample-Rate: %d\r\n"
                             "X-Aivox3-Samples: %d\r\n",
           OCTET_STREAM_HDR, AIVOX3_AUDIO_RATE, nsamples);

  parser_init(&parser, res);

  syslog(LOG_INFO, "bridge: POST %s (%s:%u), %lu bytes PCM\n",
         AIVOX3_BRIDGE_PATH, g_host, (unsigned)g_port,
         (unsigned long)body_len);

  ret = http_request_stream(g_host, g_port, "POST", AIVOX3_BRIDGE_PATH,
                            hdr, (const uint8_t *)pcm, body_len,
                            turn_body_cb, &parser,
                            AIVOX3_BRIDGE_TIMEOUT_SEC, &status);

  parser_finish(&parser);

  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bridge turn failed: %d\n", ret);
      bridge_turn_result_release(res);
      return ret;
    }

  if (status >= 400)
    {
      syslog(LOG_ERR, "ERROR: bridge HTTP %d\n", status);
      bridge_turn_result_release(res);
      return -EIO;
    }

  if (status != 200 && status != 0)
    {
      syslog(LOG_WARNING, "WARNING: bridge HTTP %d\n", status);
    }

  if (parser_truncated(res))
    {
      syslog(LOG_WARNING, "WARNING: TTS payload exceeded %lu bytes, "
             "answer truncated to %lu ms\n",
             (unsigned long)AIVOX3_TTS_PCM_MAX_BYTES,
             (unsigned long)res->ms);
    }

  syslog(LOG_INFO, "bridge: in=%s | out=%s | %lu ms audio\n",
         res->transcript[0] != '\0' ? res->transcript : "(none)",
         res->reply[0] != '\0' ? res->reply : "(filtered)",
         (unsigned long)res->ms);
  return OK;
}

/****************************************************************************
 * Name: bridge_turn_text
 ****************************************************************************/

int bridge_turn_text(const char *text, bridge_turn_result_t *res)
{
  struct turn_parser_s parser;
  char                 esc[BRIDGE_REPLY_MAX];
  char                 body[BRIDGE_REPLY_MAX + 32];
  int                  status = 0;
  int                  ret;

  if (text == NULL || text[0] == '\0' || res == NULL)
    {
      return -EINVAL;
    }

  bridge_turn_result_release(res);

  json_escape(esc, sizeof(esc), text);
  snprintf(body, sizeof(body), "{\"text\":\"%s\"}", esc);

  parser_init(&parser, res);

  ret = http_request_stream(g_host, g_port, "POST", AIVOX3_BRIDGE_PATH,
                            JSON_HDR, (const uint8_t *)body, strlen(body),
                            turn_body_cb, &parser,
                            AIVOX3_BRIDGE_TIMEOUT_SEC, &status);

  parser_finish(&parser);

  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bridge text turn failed: %d\n", ret);
      bridge_turn_result_release(res);
      return ret;
    }

  if (status >= 400)
    {
      syslog(LOG_ERR, "ERROR: bridge HTTP %d\n", status);
      bridge_turn_result_release(res);
      return -EIO;
    }

  syslog(LOG_INFO, "bridge: text=%s | out=%s | %lu ms audio\n",
         text, res->reply[0] != '\0' ? res->reply : "(filtered)",
         (unsigned long)res->ms);
  return OK;
}

/****************************************************************************
 * Name: bridge_set_endpoint
 ****************************************************************************/

void bridge_set_endpoint(const char *host, uint16_t port)
{
  if (host != NULL && host[0] != '\0')
    {
      strncpy(g_host, host, BRIDGE_HOST_MAX - 1);
      g_host[BRIDGE_HOST_MAX - 1] = '\0';
    }

  if (port != 0u)
    {
      g_port = port;
    }
}

/****************************************************************************
 * Name: bridge_host
 ****************************************************************************/

const char *bridge_host(void)
{
  return g_host;
}

/****************************************************************************
 * Name: bridge_port
 ****************************************************************************/

uint16_t bridge_port(void)
{
  return g_port;
}
