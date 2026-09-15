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
  AIVOX3_STATE_IDLE = 0,    /* standby carousel: time / weather / todo */
  AIVOX3_STATE_WAKE,        /* button/serial wake -> "thinking" */
  AIVOX3_STATE_TALK,        /* mic capture (reserved) + LLM request */
  AIVOX3_STATE_SPEAK,       /* show reply (happy/speak) */
  AIVOX3_STATE_SLEEP        /* low-power / dimmed */
} aivox3_state_t;

#define AIVOX3_STATE_COUNT     5

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_CONFIG_H */
