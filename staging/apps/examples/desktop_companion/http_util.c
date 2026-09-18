/****************************************************************************
 * apps/examples/desktop_companion/http_util.c
 *
 * Minimal plain-HTTP client on the NuttX POSIX socket API. See http_util.h
 * for the design rationale.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "http_util.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HTTP_TIMEOUT_SEC  15
#define HTTP_REQ_BUF_LEN  2048

/* http_request_stream() working-set sizes.  Both live comfortably inside a
 * NuttX thread stack, which is why the tens/hundreds of kilobytes handled by
 * that function never touch the stack at all.
 */

#define HTTP_HEAD_BUF_LEN   640   /* request head (no body!)             */
#define HTTP_RX_CHUNK_LEN   1536  /* recv() granularity                  */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: http_status_from_line
 *
 * Description:
 *   Parse the numeric status code out of a status line such as
 *   "HTTP/1.1 200 OK" (the first line of the response, NUL-terminated).
 *
 ****************************************************************************/

static int http_status_from_line(const char *line)
{
  const char *p;

  if (strncmp(line, "HTTP/", 5) != 0)
    {
      return 0;
    }

  /* Skip "HTTP/1.x " to the status code. */
  p = line + 5;
  while (*p != ' ' && *p != '\0')
    {
      p++;
    }

  if (*p == '\0')
    {
      return 0;
    }

  return atoi(p + 1);
}

/****************************************************************************
 * Name: http_set_timeouts
 *
 * Description:
 *   Apply send/receive inactivity timeouts to a fresh socket.
 ****************************************************************************/

static void http_set_timeouts(int s, int timeout_sec)
{
  struct timeval tv;

  tv.tv_sec  = (timeout_sec > 0) ? (time_t)timeout_sec
                                 : (time_t)HTTP_TIMEOUT_SEC;
  tv.tv_usec = 0;

  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/****************************************************************************
 * Name: http_send_all
 *
 * Description:
 *   Push `len` bytes, absorbing short writes. EINTR is retried.
 *
 * Returned Value:
 *   OK (0), or a negated errno.
 ****************************************************************************/

static int http_send_all(int s, const uint8_t *buf, size_t len)
{
  size_t sent = 0;

  while (sent < len)
    {
      ssize_t n = send(s, buf + sent, len - sent, 0);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          return -EIO;
        }

      sent += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: http_request
 ****************************************************************************/

int http_request(const char *host, uint16_t port, const char *method,
                 const char *path, const char *extra_headers,
                 const char *body, size_t body_len,
                 char *resp_buf, size_t resp_cap, size_t *resp_len_out,
                 int *http_status_out)
{
  struct hostent *he;
  struct sockaddr_in srv;
  struct timeval tv;
  char req[HTTP_REQ_BUF_LEN];
  char *p;
  size_t total;
  size_t body_pos = 0;
  int s;
  int status = 0;
  int len;
  int ret;

  if (host == NULL || method == NULL || path == NULL ||
      resp_buf == NULL || resp_cap < 1)
    {
      return -EINVAL;
    }

  if (resp_len_out != NULL)
    {
      *resp_len_out = 0;
    }

  if (http_status_out != NULL)
    {
      *http_status_out = 0;
    }

  resp_buf[0] = '\0';

  /* Resolve the hostname (DNS via CONFIG_NETDB_DNSCLIENT, or a dotted IP). */
  he = gethostbyname(host);
  if (he == NULL || he->h_addr_list[0] == NULL)
    {
      syslog(LOG_ERR, "http: DNS lookup failed for %s\n", host);
      return -EHOSTUNREACH;
    }

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "http: socket failed: %d\n", ret);
      return ret;
    }

  tv.tv_sec  = HTTP_TIMEOUT_SEC;
  tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_port   = htons(port);
  memcpy(&srv.sin_addr.s_addr, he->h_addr_list[0],
         sizeof(srv.sin_addr.s_addr));

  ret = connect(s, (struct sockaddr *)&srv, sizeof(srv));
  if (ret < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "http: connect %s:%u failed: %d\n", host, port, ret);
      close(s);
      return ret;
    }

  /* Build the request. HTTP/1.0 semantics: the server must not use chunked
   * encoding and closes the connection when the body ends — which is what
   * the recv-to-EOF loop below relies on. */
  len = snprintf(req, sizeof(req),
                 "%s %s HTTP/1.0\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n"
                 "User-Agent: aivox3-companion/1.0\r\n"
                 "%s"
                 "Content-Length: %lu\r\n"
                 "\r\n",
                 method, path, host,
                 extra_headers != NULL ? extra_headers : "",
                 (unsigned long)(body != NULL ? body_len : 0));
  if (len < 0 || len >= (int)sizeof(req) ||
      body_len + 1 > sizeof(req) - (size_t)len)
    {
      syslog(LOG_ERR, "http: request too large (%d + %lu)\n",
             len, (unsigned long)body_len);
      close(s);
      return -EINVAL;
    }

  if (body != NULL && body_len > 0)
    {
      memcpy(req + len, body, body_len);
      len += (int)body_len;
    }

  /* Send everything (handles short writes). */
  total = 0;
  while (total < (size_t)len)
    {
      ssize_t n = send(s, req + total, (size_t)len - total, 0);
      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          ret = (n < 0) ? -errno : -EIO;
          syslog(LOG_ERR, "http: send failed: %d\n", ret);
          close(s);
          return ret;
        }

      total += (size_t)n;
    }

  /* Receive to EOF (or buffer full), then terminate the string. */
  total = 0;
  while (total < resp_cap - 1)
    {
      ssize_t n = recv(s, resp_buf + total, resp_cap - 1 - total, 0);
      if (n <= 0)
        {
          if (n < 0 && errno == EINTR)
            {
              continue;
            }

          break;   /* EOF, timeout or error: use what we have */
        }

      total += (size_t)n;
    }

  close(s);
  resp_buf[total] = '\0';

  /* Split headers from the body and pull out the status code. */
  p = strstr(resp_buf, "\r\n\r\n");
  if (p != NULL)
    {
      size_t hdr_len = (size_t)(p - resp_buf);

      status = http_status_from_line(resp_buf);
      p += 4;
      body_pos = (size_t)(p - resp_buf);
      memmove(resp_buf, p, total - body_pos + 1);
      total -= body_pos;
      syslog(LOG_DEBUG, "http: %s %s -> %d (%lu bytes)\n",
             method, path, status, (unsigned long)hdr_len);
    }
  else
    {
      syslog(LOG_WARNING, "http: no header terminator in response\n");
    }

  if (resp_len_out != NULL)
    {
      *resp_len_out = total;
    }

  if (http_status_out != NULL)
    {
      *http_status_out = status;
    }

  return OK;
}

/****************************************************************************
 * Name: http_request_stream
 *
 * Description:
 *   See http_util.h.  Difference from http_request(): neither the request
 *   nor the response is materialised in a fixed buffer.
 ****************************************************************************/

int http_request_stream(const char *host, uint16_t port, const char *method,
                        const char *path, const char *extra_headers,
                        const uint8_t *body, size_t body_len,
                        http_sink_cb_t body_cb, void *cb_arg,
                        int timeout_sec, int *http_status_out)
{
  struct hostent *he;
  struct sockaddr_in srv;
  uint8_t rx[HTTP_RX_CHUNK_LEN];
  char head[HTTP_HEAD_BUF_LEN];
  char status_line[64];
  size_t status_len = 0;
  size_t body_sent  = 0;
  bool   status_done  = false;
  bool   headers_done = false;
  unsigned int end_match = 0;
  int s = -1;
  int status = 0;
  int ret;
  int len;

  /* "\r\n\r\n" is the header/body delimiter we scan for in the byte stream. */
  static const char hdr_end[4] = { '\r', '\n', '\r', '\n' };

  if (host == NULL || method == NULL || path == NULL)
    {
      return -EINVAL;
    }

  if (http_status_out != NULL)
    {
      *http_status_out = 0;
    }

  status_line[0] = '\0';

  /* Resolve the hostname (DNS via CONFIG_NETDB_DNSCLIENT, or a dotted IP). */

  he = gethostbyname(host);
  if (he == NULL || he->h_addr_list[0] == NULL)
    {
      syslog(LOG_ERR, "http-stream: DNS lookup failed for %s\n", host);
      return -EHOSTUNREACH;
    }

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "http-stream: socket failed: %d\n", ret);
      return ret;
    }

  http_set_timeouts(s, timeout_sec);

  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_port   = htons(port);
  memcpy(&srv.sin_addr.s_addr, he->h_addr_list[0],
         sizeof(srv.sin_addr.s_addr));

  ret = connect(s, (struct sockaddr *)&srv, sizeof(srv));
  if (ret < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "http-stream: connect %s:%u failed: %d\n",
             host, port, ret);
      close(s);
      return ret;
    }

  /* Build ONLY the head. The body is never copied anywhere: it is streamed
   * out of the caller's own buffer right after this header, which is what
   * lets us upload ~90 KB of PCM without a 90 KB stack/heap request buffer.
   */

  len = snprintf(head, sizeof(head),
                 "%s %s HTTP/1.0\r\n"
                 "Host: %s\r\n"
                 "Connection: close\r\n"
                 "User-Agent: aivox3-companion/1.0\r\n"
                 "%s"
                 "Content-Length: %lu\r\n"
                 "\r\n",
                 method, path, host,
                 extra_headers != NULL ? extra_headers : "",
                 (unsigned long)body_len);
  if (len < 0 || len >= (int)sizeof(head))
    {
      syslog(LOG_ERR, "http-stream: request head too large (%d)\n", len);
      close(s);
      return -EINVAL;
    }

  ret = http_send_all(s, (const uint8_t *)head, (size_t)len);
  if (ret < 0)
    {
      syslog(LOG_ERR, "http-stream: send head failed: %d\n", ret);
      close(s);
      return ret;
    }

  if (body != NULL && body_len > 0)
    {
      ret = http_send_all(s, body, body_len);
      if (ret < 0)
        {
          syslog(LOG_ERR, "http-stream: send body failed: %d "
                 "(sent %lu/%lu)\n", ret,
                 (unsigned long)body_sent, (unsigned long)body_len);
          close(s);
          return ret;
        }

      body_sent = body_len;
    }

  /* Receive until EOF, streaming every post-header byte to the consumer.
   *
   * HTTP/1.0 + "Connection: close" means no chunked transfer-encoding and a
   * server-side close at the end of the body, which is exactly what this
   * recv-to-EOF loop expects.
   */

  ret = OK;
  while (true)
    {
      ssize_t n = recv(s, rx, sizeof(rx), 0);
      size_t   i;
      size_t   body_start = 0;

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
              ret = -errno;
              syslog(LOG_WARNING, "http-stream: recv failed: %d\n", ret);
            }
          else
            {
              syslog(LOG_WARNING, "http-stream: recv timed out\n");
              ret = -ETIMEDOUT;
            }

          break;
        }

      if (n == 0)
        {
          break;                       /* EOF: server closed, body complete */
        }

      if (!headers_done)
        {
          bool found = false;

          for (i = 0; i < (size_t)n; i++)
            {
              char c = (char)rx[i];

              /* Keep the very first line around: it carries the status. */
              if (!status_done)
                {
                  if (c == '\r')
                    {
                      status_done = true;
                    }
                  else if (status_len + 1 < sizeof(status_line))
                    {
                      status_line[status_len++] = c;
                    }

                  status_line[status_len] = '\0';
                }

              if (c == hdr_end[end_match])
                {
                  end_match++;
                  if (end_match == 4)
                    {
                      found       = true;
                      end_match   = 0;
                      headers_done = true;
                      i++;
                      break;
                    }
                }
              else
                {
                  /* Partial match restart: the current byte may itself be
                   * the start of a valid delimiter, so re-seed on '\r'.
                   */
                  end_match = (unsigned int)(c == '\r' ? 1 : 0);
                }
            }

          status = http_status_from_line(status_line);

          if (found)
            {
              body_start = i;
            }
          else
            {
              continue;            /* header block spans more chunks */
            }
        }

      if (body_cb != NULL && (size_t)n > body_start)
        {
          int cbret = body_cb(&rx[body_start], (size_t)n - body_start,
                              cb_arg);
          if (cbret != OK)
            {
              ret = cbret;
              break;
            }
        }
    }

  close(s);

  if (http_status_out != NULL)
    {
      *http_status_out = status;
    }

  syslog(LOG_DEBUG, "http-stream: %s %s -> %d, %lu bytes body sent\n",
         method, path, status, (unsigned long)body_sent);
  return ret;
}
