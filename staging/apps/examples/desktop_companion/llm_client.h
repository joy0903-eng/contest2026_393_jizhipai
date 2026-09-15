/****************************************************************************
 * apps/examples/desktop_companion/llm_client.h
 *
 * Minimal OpenAI-compatible LLM client for the AI-VOX3 desktop companion.
 *
 * Talks to mimo-v2.5-pro at /v1/chat/completions. The API key is taken from
 * CONFIG_AIVOX3_LLM_API_KEY (build config / NVS) — never from source.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_LLM_CLIENT_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_LLM_CLIENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Send a single user message to the LLM and copy the assistant reply text
 * into out_buf (NUL-terminated). Returns OK on success, negated errno on
 * failure (including -ENOKEY if no API key is configured). */
int llm_chat(const char *user_text, char *out_buf, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_LLM_CLIENT_H */
