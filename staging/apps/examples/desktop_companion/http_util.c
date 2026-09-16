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
