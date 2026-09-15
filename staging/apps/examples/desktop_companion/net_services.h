/****************************************************************************
 * apps/examples/desktop_companion/net_services.h
 *
 * Networking, time, weather and local todo storage for the AI-VOX3 desktop
 * companion.
 *
 *  - Wi-Fi connection (via wapi / saved params)
 *  - NTP time synchronization (clock_synchronize)
 *  - Weather fetch from Open-Meteo (no API key)
 *  - Todo persistence via NVS (non-volatile storage)
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_NET_SERVICES_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_NET_SERVICES_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Initialize networking subsystem (no-op placeholder; real link comes from
 * net_wifi_connect / NSH netinit). Returns OK. */
int net_init(void);

/* Connect to a Wi-Fi AP. ssid/pass may be NULL to use saved params.
 * Returns OK on success, negated errno otherwise. */
int net_wifi_connect(const char *ssid, const char *pass);

/* Poll until the wlan0 interface has an IPv4 address, or timeout (ms).
 * Returns true when linked. */
bool net_wait_linked(int timeout_ms);

/* True if an IPv4 address is configured on wlan0. */
bool net_is_connected(void);

/* Synchronize system clock via NTP (CONFIG_NETUTILS_NTPCLIENT). Returns OK. */
int net_ntp_sync(void);

/* Fetch current weather. On success fills desc_out (weather text) and
 * temp_out (Celsius). Returns OK or negated errno. */
int net_get_weather(char *desc_out, size_t desc_len, int *temp_out);

/* --- Todo (NVS-backed) --- */

/* Load the first stored todo item into buf. Returns OK / -ENOENT. */
int todo_load_first(char *buf, size_t len);

/* Append a todo item (stored as a small numbered list in NVS). Returns OK. */
int todo_add(const char *text);

/* Clear all stored todos. Returns OK. */
int todo_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_NET_SERVICES_H */
