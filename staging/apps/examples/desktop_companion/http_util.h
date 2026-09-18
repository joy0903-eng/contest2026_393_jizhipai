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

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Name: http_sink_cb_t
 *
 * Description:
 *   Consumer invoked for every chunk of response body received by
 *   http_request_stream().  `chunk` is NOT NUL-terminated and is only valid
 *   for the duration of the call -- copy whatever must be kept.
 *
 * Returned Value:
 *   OK (0) to keep receiving; any negated errno to abort the transfer (that
 *   value is returned by http_request_stream()).
 *
 ****************************************************************************/

typedef int (*http_sink_cb_t)(const uint8_t *chunk, size_t len, void *arg);

/****************************************************************************
 * Name: http_request_stream
 *
 * Description:
 *   Streaming variant of http_request(): identical request semantics (same
 *   HTTP/1.0, no-TLS, no-keepalive rules) but WITHOUT the two fixed-size
 *   buffers that make http_request() unusable for voice traffic:
 *
 *     - the request *head* is built in a small stack buffer; the body is
 *       streamed straight out of the caller's buffer in send() chunks, so a
 *       multi-dozen-kilobyte PCM upload needs no HTTP_REQ_BUF_LEN copy at
 *       all;
 *     - the response body is never accumulated: each received chunk is
 *       handed to `body_cb` (after the header block has been skipped), so
 *       the caller can decode it incrementally instead of buffering hundreds
 *       of kilobytes.
 *
 * Input Parameters:
 *   host          - hostname or dotted IP (no scheme, no port).
 *   port          - TCP port.
 *   method        - "GET" / "POST" / "PUT".
 *   path          - request path incl. query string.
 *   extra_headers - optional CRLF-terminated extra header lines (NULL ok).
 *   body          - request body (may be NULL when body_len == 0).
 *   body_len      - request body length in bytes (Content-Length).
 *   body_cb       - response body consumer. NULL discards the body.
 *   cb_arg        - opaque argument for body_cb.
 *   timeout_sec   - per-socket send/recv timeout (<= 0 -> HTTP_TIMEOUT_SEC).
 *                   This is an *inactivity* timeout: each successful recv()
 *                   restarts it, so a slow-but-streaming server is fine.
 *   http_status_out - optional; receives the numeric HTTP status code.
 *
 * Returned Value:
 *   OK (0) when the transfer completed; a negated errno otherwise
 *   (-EHOSTUNREACH DNS, -EINVAL oversized request head, -ETIMEDOUT socket
 *   timeout, or whatever body_cb returned to abort).
 *
 ****************************************************************************/

int http_request_stream(const char *host, uint16_t port, const char *method,
                        const char *path, const char *extra_headers,
                        const uint8_t *body, size_t body_len,
                        http_sink_cb_t body_cb, void *cb_arg,
                        int timeout_sec, int *http_status_out);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_HTTP_UTIL_H */
