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

/* Base URL and chat path for the mimo-v2.6-pro endpoint (user-specified). */
#define AIVOX3_LLM_BASE_URL    "https://token-plan-cn.xiaomimimo.com/v1"
#define AIVOX3_LLM_CHAT_PATH   "/chat/completions"
#ifndef CONFIG_AIVOX3_LLM_MODEL
#define AIVOX3_LLM_MODEL       "mimo-v2.6-pro"
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

/* Standalone TTS + novel-staging endpoints added for the audiobook feature
 * (see bridge_chat.py do_POST/do_GET). /tts returns raw PCM (same format as
 * the chat TTS audio); /upload_novel lets a PC upload a novel; /novel lets the
 * board pull the staged novel back onto its FFat. */
#ifndef CONFIG_AIVOX3_TTS_PATH
#define AIVOX3_TTS_PATH             "/tts"
#else
#define AIVOX3_TTS_PATH             CONFIG_AIVOX3_TTS_PATH
#endif

#ifndef CONFIG_AIVOX3_NOVEL_UPLOAD_PATH
#define AIVOX3_NOVEL_UPLOAD_PATH    "/upload_novel"
#else
#define AIVOX3_NOVEL_UPLOAD_PATH    CONFIG_AIVOX3_NOVEL_UPLOAD_PATH
#endif

#ifndef CONFIG_AIVOX3_NOVEL_GET_PATH
#define AIVOX3_NOVEL_GET_PATH       "/novel"
#else
#define AIVOX3_NOVEL_GET_PATH       CONFIG_AIVOX3_NOVEL_GET_PATH
#endif

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

/* LEGACY: deadline of the old single-shot LISTEN window. That state was
 * superseded by AIVOX3_STATE_CHAT (continuous conversation), whose deadline
 * is AIVOX3_CHAT_IDLE_TIMEOUT_MS below; the symbol is kept so existing
 * defconfigs still parse. */
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
 * Wake word ("你好" / "ni hao") + continuous conversation
 *
 * Two-tier design (see main.c wake_gate()):
 *   tier A - offline KWS (kws_engine.c) when a template bank is loaded:
 *            zero network cost, instant, but needs recorded templates.
 *   tier B - the PC bridge's wake re-check (PUT /api/turn, bridge_chat.py
 *            do_PUT at :524): the board uploads the utterance, the bridge
 *            runs Vosk ASR and answers {"wake": bool, "text": "..."}.
 *            ASR only -- no LLM, no TTS -- so it is cheap and it needs no
 *            on-board templates, which is why it is the tier that actually
 *            works on a freshly flashed board.
 ****************************************************************************/

/* Idle gap that ends a conversation. While a session is open every finished
 * utterance is answered WITHOUT a second wake word; after this much silence
 * the board drops back to standby and waits for "你好" again.
 * 12 s is a compromise: long enough to think between sentences, short enough
 * that the device stops eavesdropping soon after you walk away.
 */
#ifndef CONFIG_AIVOX3_CHAT_IDLE_TIMEOUT
#define AIVOX3_CHAT_IDLE_TIMEOUT_MS 12000
#else
#define AIVOX3_CHAT_IDLE_TIMEOUT_MS CONFIG_AIVOX3_CHAT_IDLE_TIMEOUT
#endif

/* Shortest utterance worth uploading for a wake check, milliseconds. Clicks,
 * door slams and other sub-0.4 s blips are dropped on the board instead of
 * costing a round trip to the PC. "你好" is ~0.6-1.0 s, so this is safe.
 */
#ifndef CONFIG_AIVOX3_WAKE_MIN_MS
#define AIVOX3_WAKE_MIN_MS          400
#else
#define AIVOX3_WAKE_MIN_MS          CONFIG_AIVOX3_WAKE_MIN_MS
#endif

/* One-shot support: "你好，讲个笑话" is answered straight away instead of
 * being consumed as a bare wake word. The bridge already returns the ASR
 * text of the wake utterance, so the leftover sentence is free.
 * 1 = enabled (default), 0 = wake word is always consumed on its own.
 */
#ifndef CONFIG_AIVOX3_WAKE_ONESHOT
#define AIVOX3_WAKE_ONESHOT         1
#else
#define AIVOX3_WAKE_ONESHOT         CONFIG_AIVOX3_WAKE_ONESHOT
#endif

/* How many characters (UTF-8 code points, not bytes) the wake transcript
 * must contain before it is treated as "wake word + a real question" rather
 * than a plain greeting. "你好" is 2, "你好。" is 3, "你好，讲个笑话" is 7.
 */
#ifndef CONFIG_AIVOX3_WAKE_ONESHOT_MIN_CHARS
#define AIVOX3_WAKE_ONESHOT_MIN_CHARS 4
#else
#define AIVOX3_WAKE_ONESHOT_MIN_CHARS CONFIG_AIVOX3_WAKE_ONESHOT_MIN_CHARS
#endif

/****************************************************************************
 * LCD geometry (mirror of the BSP; app uses its own copy)
 ****************************************************************************/

#define AIVOX3_LCD_WIDTH       240
#define AIVOX3_LCD_HEIGHT      320

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
 * Audiobook (novel reading) mode
 ****************************************************************************/

/* On-board FFat path the novel text is read from / written to. The board
 * config currently mounts no writable filesystem by default, so this path only
 * resolves once FFat is mounted there; until then the reader transparently
 * falls back to an in-memory built-in sample (see novel_reader.c). */
#ifndef CONFIG_AIVOX3_READ_NOVEL_PATH
#define AIVOX3_READ_NOVEL_PATH     "/mnt/ffat/novel.txt"
#else
#define AIVOX3_READ_NOVEL_PATH     CONFIG_AIVOX3_READ_NOVEL_PATH
#endif

/* Hard ceiling on the novel size we will load into RAM. A whole novel is far
 * larger, but the reading feature is meant for short stories / chapters; the
 * bridge /upload_novel path enforces the same ceiling on the PC side. */
#ifndef CONFIG_AIVOX3_READ_NOVEL_MAX_BYTES
#define AIVOX3_READ_NOVEL_MAX_BYTES  (256u * 1024u)
#else
#define AIVOX3_READ_NOVEL_MAX_BYTES  CONFIG_AIVOX3_READ_NOVEL_MAX_BYTES
#endif

/* Built-in sample novel used when no file is present on FFat yet. It is
 * written to AIVOX3_READ_NOVEL_PATH on first boot (best effort) so the
 * reading demo always has content, and demonstrates chapter detection
 * (第N章 / 第N回) plus multi-line segmentation. */
#define AIVOX3_BUILTIN_NOVEL                                               \
  "第一章 初入江湖\n"                                                      \
  "风起云涌，少年持剑走出了大山。\n"                                      \
  "他望着眼前的城池，心中既忐忑又充满期待。\n"                            \
  "第二章 偶遇\n"                                                          \
  "酒馆之中，一位白衣老者向他招了招手。\n"                                \
  "年轻人，你的路才刚刚开始。老者笑着说道。\n"                            \
  "第三章 试炼\n"                                                          \
  "山道崎岖，云雾缭绕，隐约传来阵阵钟声。\n"                            \
  "他深吸一口气，迈开脚步向山顶走去。\n"

/****************************************************************************
 * Top-level application state machine
 ****************************************************************************/

typedef enum
{
  AIVOX3_STATE_IDLE = 0,    /* STANDBY: mic capturing, wake-word gate only */
  AIVOX3_STATE_WAKE,        /* ack (tone + face); opens a chat session     */
  AIVOX3_STATE_LISTEN,      /* superseded by CHAT; kept for log stability  */
  AIVOX3_STATE_CHAT,        /* CONVERSATION: every utterance is answered   */
  AIVOX3_STATE_TALK,        /* upload the utterance to the PC voice bridge */
  AIVOX3_STATE_SPEAK,       /* play the returned TTS PCM, show the reply   */
  AIVOX3_STATE_SLEEP,       /* low-power / dimmed: capture stopped         */
  AIVOX3_STATE_MODE_READ,   /* audiobook: loading the novel, mic is off   */
  AIVOX3_STATE_READING      /* audiobook: synthesizing + playing a segment */
} aivox3_state_t;

#define AIVOX3_STATE_COUNT     9

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_CONFIG_H */
