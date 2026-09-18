/****************************************************************************
 * apps/examples/desktop_companion/voice_bridge.h
 *
 * Client for the PC-side conversational voice bridge
 * (toolchain/voice_bridge/bridge_chat.py).
 *
 * WHY THIS EXISTS
 * ---------------
 * There is no ASR and no TTS on this board, and there is no TLS stack in
 * http_util yet, so the cloud speech endpoints cannot be reached directly.
 * The bridge does ALL of the heavy lifting on the PC (Vosk/mimo ASR ->
 * mimo-v2.5 LLM -> edge-tts) and exposes one plain-HTTP endpoint:
 *
 *   POST /api/turn
 *     request  - raw little-endian int16 PCM, 16 kHz, mono
 *                (Content-Type: application/octet-stream)
 *                OR  {"text": "..."}  (Content-Type: application/json)
 *     response - {"transcript": "...", "reply": "...",
 *                 "audio": "<base64 PCM 16 kHz / 16-bit / mono>"}
 *
 * See bridge_chat.py:527-540 (do_POST) and :410-449 (process_turn).
 *
 * STREAMING BY DESIGN
 * -------------------
 * One answer is easily a megabyte of base64. Nothing here buffers that:
 * http_request_stream() feeds the response body in ~1.5 KB chunks straight
 * into an incremental JSON scanner that base64-decodes the "audio" member
 * into a heap buffer as it arrives. Peak extra memory for a turn is the
 * decoded PCM alone, never the JSON as well.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_VOICE_BRIDGE_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_VOICE_BRIDGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Text fields are only used for the UI / console log; the audio is what
 * matters, so these stay small. Truncation is silent and harmless.
 */

#define BRIDGE_TRANSCRIPT_MAX   192
#define BRIDGE_REPLY_MAX        512

/* Capacity of the internally held bridge hostname / IP string ("a.b.c.d"
 * plus room for an mDNS-ish name). Also used by callers that parse a
 * `bridge <host> [port]` command line.
 */

#define BRIDGE_HOST_MAX         64

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Result of one bridge turn. `audio` is heap allocated and owned by the
 * caller until bridge_turn_result_release() (or _init()) is called.
 */

struct bridge_turn_result_s
{
  char     transcript[BRIDGE_TRANSCRIPT_MAX];
  char     reply[BRIDGE_REPLY_MAX];
  uint8_t *audio;       /* decoded PCM, 16 kHz / 16-bit / mono, LE */
  size_t   audio_len;   /* bytes in audio (always even)            */
  size_t   ms;          /* audio_len / 32, i.e. milliseconds of speech */
  bool     truncated;   /* true when the TTS payload hit the cap    */
};

typedef struct bridge_turn_result_s bridge_turn_result_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bridge_turn_result_init / bridge_turn_result_release
 *
 * Description:
 *   Reset (and thereby take ownership of) / free one result container.
 *   Both are safe on an already-empty container and on NULL.
 ****************************************************************************/

void bridge_turn_result_init(bridge_turn_result_t *res);
void bridge_turn_result_release(bridge_turn_result_t *res);

/****************************************************************************
 * Name: bridge_turn_pcm
 *
 * Description:
 *   Upload one utterance and collect the transcript/reply/TTS triple.
 *
 * Input Parameters:
 *   pcm      - mono int16 samples at AIVOX3_AUDIO_RATE (little endian).
 *   nsamples - sample count (> 0).
 *   res      - result container; emptied first, then filled.
 *
 * Returned Value:
 *   OK (0) on a completed turn -- including the legitimate case of an empty
 *   reply (the bridge filters ambient noise and returns reply:""), in which
 *   case res->reply[0] == '\0' and res->audio_len == 0.
 *   Negated errno otherwise: -EINVAL bad args, -ENETUNREACH/-EHOSTUNREACH
 *   transport failure, -ETIMEDOUT the bridge never answered, -EIO a >= 400
 *   HTTP status, -ENOMEM the PCM buffer could not be allocated.
 ****************************************************************************/

int bridge_turn_pcm(const int16_t *pcm, int nsamples,
                    bridge_turn_result_t *res);

/****************************************************************************
 * Name: bridge_turn_text
 *
 * Description:
 *   Same turn, but with the user input supplied as text
 *   ({"text": "..."} + Content-Type: application/json). Used for the
 *   USB-CDC console: anything typed that is not a command is answered by
 *   the assistant's voice.
 ****************************************************************************/

int bridge_turn_text(const char *text, bridge_turn_result_t *res);

/****************************************************************************
 * Name: bridge_set_endpoint
 *
 * Description:
 *   Override the bridge address at runtime (the `bridge <host> [port]`
 *   console command). host == NULL leaves the host unchanged; port == 0
 *   leaves the port unchanged. A copy of host is taken.
 ****************************************************************************/

void bridge_set_endpoint(const char *host, uint16_t port);

/****************************************************************************
 * Name: bridge_host / bridge_port
 *
 * Description:
 *   Current endpoint components (never NULL / never 0).
 ****************************************************************************/

const char *bridge_host(void);
uint16_t    bridge_port(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_VOICE_BRIDGE_H */
