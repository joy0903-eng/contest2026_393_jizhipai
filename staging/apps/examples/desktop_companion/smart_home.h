/****************************************************************************
 * apps/examples/desktop_companion/smart_home.h
 *
 * Smart-home intent interface (stub). See plan §7.3.
 *
 * Parses a simple intent JSON and, depending on the selected backend
 * (none / mqtt / http), acts or returns "unconfigured". The default backend
 * is "none": intent is logged and "unconfigured" is returned. A real backend
 * adapter (e.g. MQTT -> Home Assistant) can be dropped in later without
 * changing the caller.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_SMART_HOME_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_SMART_HOME_H

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

/* Select the smart-home backend ("none" | "mqtt" | "http"). Returns OK. */
int smart_home_set_backend(const char *backend);

/* Handle an intent (JSON string with at least "device" and "action").
 * On return, msg_out (if non-NULL) holds a human-readable status.
 * Returns OK if handled, -EOPNOTSUPP if the backend is not configured. */
int smart_home_control(const char *intent_json, char *msg_out, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_SMART_HOME_H */
