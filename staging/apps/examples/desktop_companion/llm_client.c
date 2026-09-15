/****************************************************************************
 * apps/examples/desktop_companion/llm_client.c
 *
 * OpenAI-compatible chat client. Posts a single user turn to
 * /v1/chat/completions and extracts the assistant message text.
 *
 * SECURITY: the API key is taken ONLY from AIVOX3_LLM_API_KEY (which maps to
 * the build-time CONFIG_AIVOX3_LLM_API_KEY). No secret is hard-coded.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alloca.h>
#include <syslog.h>

#include <nuttx/net/webclient.h>

#include "llm_client.h"
#include "config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SYSTEM_PROMPT  \
  "你是桌面小跟班 AI-VOX3，一只可爱、简短、贴心的桌面伙伴。" \
  "用中文回答，语气轻松，控制在三句话以内。"

#define REQ_BUF_LEN   1024
#define RESP_BUF_LEN  2048

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: json_escape
 *
 * Description:
 *   Copy src into dst applying minimal JSON string escaping (" \ \n \r \t).
 *   Returns the number of bytes written (excluding NUL).
 *
 ****************************************************************************/

static size_t json_escape(char *dst, size_t dst_cap, const char *src)
{
  size_t j = 0;
  char c;

  while ((c = *src++) != '\0' && j + 2 < dst_cap)
    {
      switch (c)
        {
          case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
          case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
          case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
          case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
          case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
          default:
            dst[j++] = c;
            break;
        }
    }

  dst[j] = '\0';
  return j;
}

/****************************************************************************
 * Name: resp_callback
 *
 * Description:
 *   Accumulate the HTTP body (skipping headers) into a buffer.
 *
 ****************************************************************************/

struct resp_buf_s
{
  char *buf;
  size_t cap;
  size_t len;
  bool headers_done;
};

static int resp_callback(FAR struct webclient_session *s,
                         FAR const char *buf, size_t len, FAR void *arg)
{
  struct resp_buf_s *rb = (struct resp_buf_s *)arg;
  size_t i = 0;

  if (!rb->headers_done)
    {
      for (i = 0; i + 3 < len; i++)
        {
          if (buf[i] == '\r' && buf[i + 1] == '\n' &&
              buf[i + 2] == '\r' && buf[i + 3] == '\n')
            {
              i += 4;
              rb->headers_done = true;
              break;
            }
        }

      if (!rb->headers_done)
        {
          return 0;
        }
    }

  if (rb->len + (len - i) < rb->cap)
    {
      memcpy(rb->buf + rb->len, buf + i, len - i);
      rb->len += (len - i);
    }

  return 0;
}

/****************************************************************************
 * Name: extract_content
 *
 * Description:
 *   Find the first "content":"..." in the JSON and copy it (unescaping basic
 *   sequences) into out_buf.
 *
 ****************************************************************************/

static void extract_content(const char *body, char *out_buf, size_t out_len)
{
  const char *p = strstr(body, "\"content\":\"");
  const char *q;
  size_t j = 0;

  if (p == NULL)
    {
      out_buf[0] = '\0';
      return;
    }

  p += strlen("\"content\":\"");
  q = p;
  while (*q != '\0' && *q != '"' && j + 1 < out_len)
    {
      if (*q == '\\' && q[1] != '\0')
        {
          q++;
          switch (*q)
            {
              case 'n': out_buf[j++] = '\n'; break;
              case 't': out_buf[j++] = '\t'; break;
              case 'r': out_buf[j++] = '\r'; break;
              case '"': out_buf[j++] = '"';  break;
              case '\\': out_buf[j++] = '\\'; break;
              default:  out_buf[j++] = *q;    break;
            }
        }
      else
        {
          out_buf[j++] = *q;
        }

      q++;
    }

  out_buf[j] = '\0';
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_chat
 ****************************************************************************/

int llm_chat(const char *user_text, char *out_buf, size_t out_len)
{
  char url[160];
  char req[REQ_BUF_LEN];
  char esc[512];
  char resp[RESP_BUF_LEN];
  struct resp_buf_s rb;
  struct webclient_context ctx;
  int ret;

  if (out_buf != NULL && out_len > 0)
    {
      out_buf[0] = '\0';
    }

  /* Security gate: refuse to call the API without a key. */
  if (AIVOX3_LLM_API_KEY == NULL || AIVOX3_LLM_API_KEY[0] == '\0')
    {
      syslog(LOG_WARNING, "WARNING: no LLM API key configured; skipping call\n");
      return -ENOKEY;
    }

  if (user_text == NULL)
    {
      return -EINVAL;
    }

  /* Build the full endpoint URL. */
  snprintf(url, sizeof(url), "%s%s", AIVOX3_LLM_BASE_URL, AIVOX3_LLM_CHAT_PATH);

  /* Build the JSON request body. */
  json_escape(esc, sizeof(esc), user_text);
  snprintf(req, sizeof(req),
           "{\"model\":\"%s\","
           "\"messages\":["
           "{\"role\":\"system\",\"content\":\"%s\"},"
           "{\"role\":\"user\",\"content\":\"%s\"}"
           "],\"max_tokens\":%d,\"temperature\":%.1f}",
           AIVOX3_LLM_MODEL, SYSTEM_PROMPT, esc,
           AIVOX3_LLM_MAX_TOKENS, AIVOX3_LLM_TEMPERATURE);

  /* Accumulate the response body. */
  memset(&rb, 0, sizeof(rb));
  rb.buf = resp;
  rb.cap = sizeof(resp);

  memset(&ctx, 0, sizeof(ctx));
  ctx.url     = url;
  ctx.method  = "POST";
  ctx.post_data    = req;
  ctx.post_datalen = (int)strlen(req);
  ctx.callback = resp_callback;
  ctx.cbarg   = &rb;
  /* Extra headers: bearer auth + JSON content type. The NuttX webclient
   * context exposes `header` for additional request headers.
   * TODO(real-device/build): if your tree names this field differently,
   * adapt (e.g. prepend headers manually or use webclient_set_header()). */
  {
    char *hdr = alloca(160);
    snprintf(hdr, 160,
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\n",
             AIVOX3_LLM_API_KEY);
    ctx.header = hdr;
  }

  ret = webclient_perform(&ctx);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: LLM HTTP POST failed: %d\n", ret);
      return ret;
    }

  resp[rb.len] = '\0';
  extract_content(resp, out_buf, out_len);
  return OK;
}
