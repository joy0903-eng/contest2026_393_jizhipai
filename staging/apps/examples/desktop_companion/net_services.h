/****************************************************************************
 * apps/examples/desktop_companion/net_services.h
 *
 * Networking, time, weather and local todo storage for the AI-VOX3 desktop
 * companion.
 *
 *  - Wi-Fi link status via netlib (association is a placeholder for now)
 *  - NTP time synchronization via the apps ntpc daemon (ntpc_start)
 *  - Weather fetch from Open-Meteo over plain HTTP (no API key)
 *  - Todo list kept in RAM during bring-up (volatile across reboots)
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

/* Bring up the station in-process: ifup wlan0, associate (WPA2-PSK), run
 * DHCP and wait for an IPv4 address. Every wait is bounded (~18 s worst
 * case) and main.c ignores the return value, so failures are also logged
 * with syslog(). Returns OK when net_is_connected() is true, otherwise a
 * negated errno (-ETIMEDOUT when the address never appeared). */
int net_init(void);

/* Associate with a WPA2-PSK AP. NULL/empty ssid or pass fall back to the
 * Kconfig strings CONFIG_AIVOX3_WIFI_SSID / CONFIG_AIVOX3_WIFI_PASSWORD.
 * The SSID is logged, the passphrase is never logged.
 * Returns OK once the driver has accepted the request, negated errno
 * otherwise. Note that this only starts association; the link is up when
 * net_is_connected() reports true. */
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
