/****************************************************************************
 * apps/examples/desktop_companion/config.h
 *
 * Compile-time configuration for the AI-VOX3 desktop companion application.
 *
 * SECURITY: The LLM API key is NEVER hard-coded. It is referenced ONLY through
 * the build-time symbol CONFIG_AIVOX3_LLM_API_KEY (set in menuconfig / injected
 * via NVS). Do not add a plaintext secret to this file or any other.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_CONFIG_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_CONFIG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * LLM (OpenAI-compatible) endpoint
 ****************************************************************************/

/* Base URL and chat path for the mimo-v2.5-pro endpoint (confirmed). */
#define AIVOX3_LLM_BASE_URL    "https://token-plan-cn.xiaomimimo.com/v1"
#define AIVOX3_LLM_CHAT_PATH   "/chat/completions"
#ifndef CONFIG_AIVOX3_LLM_MODEL
#define AIVOX3_LLM_MODEL       "mimo-v2.5-pro"
#else
#define AIVOX3_LLM_MODEL       CONFIG_AIVOX3_LLM_MODEL
#endif

/* API key: ONLY via build config. Never inline a secret here. If empty, the
 * app logs a warning and skips actual LLM calls (still runs the UI loop). */
#ifndef CONFIG_AIVOX3_LLM_API_KEY
#define CONFIG_AIVOX3_LLM_API_KEY ""
#endif
#define AIVOX3_LLM_API_KEY     CONFIG_AIVOX3_LLM_API_KEY

/* Generation limits. */
#define AIVOX3_LLM_MAX_TOKENS  256
#define AIVOX3_LLM_TEMPERATURE 0.8f

/****************************************************************************
 * Weather (Open-Meteo, no API key required)
 ****************************************************************************/

#define AIVOX3_WEATHER_URL     "https://api.open-meteo.com/v1/forecast"
#define AIVOX3_WEATHER_LAT     39.9042   /* Beijing by default; TODO: geo IP */
#define AIVOX3_WEATHER_LON     116.4074

/****************************************************************************
 * PC voice bridge (off-board ASR + LLM + TTS)
 *
 * The board has neither an ASR nor a TTS engine, so a full turn is relayed
 * to a PC running toolchain/voice_bridge/bridge_chat.py (Vosk + mimo +
 * edge-tts). The bridge listens on 0.0.0.0:8765 and answers POST /api/turn.
 *
 * HOME ROUTERS HAND OUT DHCP LEASES: the PC's address WILL change, so this
 * address is deliberately NOT a hard-coded literal. It comes from Kconfig
 * (menuconfig) for the flashed default and can be overridden at runtime
 * over the USB-CDC console with:   bridge 192.168.1.23 8765
 ****************************************************************************/

#ifndef CONFIG_AIVOX3_BRIDGE_HOST
#define AIVOX3_BRIDGE_HOST_DEFAULT "192.168.1.23"
#else
#define AIVOX3_BRIDGE_HOST_DEFAULT CONFIG_AIVOX3_BRIDGE_HOST
#endif

#ifndef CONFIG_AIVOX3_BRIDGE_PORT
#define AIVOX3_BRIDGE_PORT_DEFAULT 8765
#else
#define AIVOX3_BRIDGE_PORT_DEFAULT CONFIG_AIVOX3_BRIDGE_PORT
#endif

#ifndef CONFIG_AIVOX3_BRIDGE_TIMEOUT
#define AIVOX3_BRIDGE_TIMEOUT_DEFAULT 60
#else
#define AIVOX3_BRIDGE_TIMEOUT_DEFAULT CONFIG_AIVOX3_BRIDGE_TIMEOUT
#endif

/* Request path of one conversation turn (see bridge_chat.py do_POST). */
#define AIVOX3_BRIDGE_PATH          "/api/turn"

/* Socket inactivity timeout, seconds. A turn costs: Vosk/mimo ASR (1-4 s) +
 * a streaming LLM (3-10 s) + parallel sentence TTS (2-6 s), so the old
 * HTTP_TIMEOUT_SEC of 15 s is far too tight. Every successful recv()
 * restarts this window, so it only fires on a genuinely dead peer.
 */
#define AIVOX3_BRIDGE_TIMEOUT_SEC   AIVOX3_BRIDGE_TIMEOUT_DEFAULT

/****************************************************************************
 * Voice turn tuning
 ****************************************************************************/

/* Main-loop tick. Everything (USB-CDC poll, VAD poll, UI) is driven from
 * this cadence, so no state ever blocks the loop for long except SPEAK. */
#define AIVOX3_LOOP_TICK_MS         50

/* How long a prompt stays open waiting for the user to start talking
 * (button-triggered turns only). A pure voice trigger has no deadline: it
 * simply keeps listening in IDLE. */
#define AIVOX3_LISTEN_TIMEOUT_MS    12000

/* Must wait for the speaker's mechanical ring-down before re-opening the
 * microphone, otherwise the tail of the answer re-triggers the VAD and the
 * device starts talking to itself (the codec is half duplex, so the capture
 * simply must not be live while playback drains). */
#define AIVOX3_POST_PLAY_GUARD_MS   600

/* Upper bound on the TTS payload we accept from the bridge. 12 s of speech
 * at 16 kHz / 16-bit / mono. Anything beyond is dropped (not fatal) so one
 * chatty answer can never eat the whole heap. */
#define AIVOX3_TTS_PCM_MAX_BYTES    (768u * 1024u)

/****************************************************************************
 * LCD geometry (mirror of the BSP; app uses its own copy)
 ****************************************************************************/

#define AIVOX3_LCD_WIDTH       240
#define AIVOX3_LCD_HEIGHT      240

/****************************************************************************
 * Servo / face-follow mapping (application view; BSP owns the GPIOs)
 ****************************************************************************/

#define AIVOX3_HEAD_PAN_SERVO_ID   0   /* IO42 — head left/right */
#define AIVOX3_HEAD_TILT_SERVO_ID  1   /* IO43 — head up/down */

/****************************************************************************
 * Audio format
 ****************************************************************************/

#define AIVOX3_AUDIO_RATE      16000
#define AIVOX3_AUDIO_BITS      16
#define AIVOX3_AUDIO_CHANNELS  1

/****************************************************************************
 * Top-level application state machine
 ****************************************************************************/

typedef enum
{
  AIVOX3_STATE_IDLE = 0,    /* standby carousel + MIC ALWAYS CAPTURING     */
  AIVOX3_STATE_WAKE,        /* ack (tone + face); mic is re-armed cleanly  */
  AIVOX3_STATE_LISTEN,      /* waiting for a finished VAD utterance        */
  AIVOX3_STATE_TALK,        /* upload the utterance to the PC voice bridge */
  AIVOX3_STATE_SPEAK,       /* play the returned TTS PCM, show the reply   */
  AIVOX3_STATE_SLEEP        /* low-power / dimmed: capture stopped         */
} aivox3_state_t;

#define AIVOX3_STATE_COUNT     6

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_CONFIG_H */
