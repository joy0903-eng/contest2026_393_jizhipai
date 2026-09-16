/****************************************************************************
 * apps/examples/desktop_companion/http_util.h
 *
 * Minimal plain-HTTP client built directly on the NuttX POSIX socket API.
 *
 * Why not apps/netutils/webclient: the upstream webclient API surface
 * (webclient_context + sink callbacks) differs substantially from what this
 * app was written against, and the old nuttx/net/webclient.h header no
 * longer exists. A ~150-line socket client has no Kconfig or API contract
 * to fight with.
 *
 * NOTE: HTTP/1.0 is used on the wire, so servers never reply with
 * chunked transfer-encoding and close the connection at EOF — the recv
 * loop relies on that. TLS (https) is NOT supported yet: callers must
 * pass a plain-HTTP host/port or the request fails at runtime.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_HTTP_UTIL_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_HTTP_UTIL_H

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

/****************************************************************************
 * Name: http_request
 *
 * Description:
 *   One-shot blocking HTTP request over a TCP socket (no TLS, no keepalive).
 *
 * Input Parameters:
 *   host          - Plain hostname (resolved via gethostbyname/DNS) or
 *                   dotted IP. Do NOT include a scheme or port here.
 *   port          - TCP port (usually 80).
 *   method        - "GET" or "POST".
 *   path          - Request path incl. query string, e.g.
 *                   "/v1/forecast?latitude=39.9&longitude=116.4".
 *   extra_headers - Optional NUL-terminated string of extra request headers,
 *                   each ending with CRLF ("" or NULL for none).
 *   body          - Optional request body (may be NULL when body_len == 0).
 *   body_len      - Length of body in bytes.
 *   resp_buf      - Output buffer. On return it holds ONLY the response
 *                   body (headers are stripped), NUL-terminated.
 *   resp_cap      - Capacity of resp_buf.
 *   resp_len_out  - Optional; receives the body length.
 *   http_status_out - Optional; receives the numeric HTTP status code
 *                   (e.g. 200). 0 if no parseable status line was seen.
 *
 * Returned Value:
 *   OK (0) on network-level success (check *http_status_out for the
 *   application-level result); a negated errno value otherwise
 *   (-EHOSTUNREACH DNS failure, -ENOTSUP unsupported, -ETIMEDOUT, ...).
 *
 ****************************************************************************/

int http_request(const char *host, uint16_t port, const char *method,
                 const char *path, const char *extra_headers,
                 const char *body, size_t body_len,
                 char *resp_buf, size_t resp_cap, size_t *resp_len_out,
                 int *http_status_out);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_HTTP_UTIL_H */
