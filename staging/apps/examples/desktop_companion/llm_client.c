/****************************************************************************
 * apps/examples/desktop_companion/llm_client.c
 *
 * OpenAI-compatible chat client. Posts a single user turn to
 * /v1/chat/completions and extracts the assistant message text.
 *
 * HTTP transport: local http_util (POSIX sockets, no TLS). The canonical
 * endpoint in config.h is https://; until TLS is wired up we target the
 * same host over plain HTTP. If the server rejects plain HTTP the call
 * fails gracefully and the UI keeps working.
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
#include <string.h>
#include <syslog.h>

#include "http_util.h"
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

/* Plain-HTTP mirror of AIVOX3_LLM_BASE_URL + AIVOX3_LLM_CHAT_PATH.
 * Keep in sync with config.h until TLS support lands in http_util. */
#define LLM_HTTP_HOST  "token-plan-cn.xiaomimimo.com"
#define LLM_HTTP_PORT  80
#define LLM_HTTP_PATH  "/v1/chat/completions"

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
  char req[REQ_BUF_LEN];
  char esc[512];
  char resp[RESP_BUF_LEN];
  char hdr[192];
  size_t resp_len = 0;
  int status = 0;
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

  /* Extra headers: bearer auth + JSON content type (each ends with CRLF). */
  snprintf(hdr, sizeof(hdr),
           "Authorization: Bearer %s\r\n"
           "Content-Type: application/json\r\n",
           AIVOX3_LLM_API_KEY);

  ret = http_request(LLM_HTTP_HOST, LLM_HTTP_PORT, "POST", LLM_HTTP_PATH,
                     hdr, req, strlen(req),
                     resp, sizeof(resp), &resp_len, &status);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: LLM HTTP POST failed: %d\n", ret);
      return ret;
    }

  if (status != 200)
    {
      syslog(LOG_ERR, "ERROR: LLM HTTP status %d\n", status);
    }

  extract_content(resp, out_buf, out_len);
  return OK;
}
