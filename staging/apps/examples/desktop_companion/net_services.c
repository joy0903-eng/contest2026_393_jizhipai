/****************************************************************************
 * apps/examples/desktop_companion/net_services.c
 *
 * Implementation of Wi-Fi / NTP / weather / todo services.
 *
 * - Weather uses Open-Meteo (api.open-meteo.com) which needs NO API key,
 *   fetched through the local http_util (plain HTTP).
 * - NTP uses the apps netutils NTP client daemon (ntpc_start).
 * - Wi-Fi association is a placeholder (-ENOSYS) until the esp32s3 wext
 *   passkey path is wired up; net_is_connected works via netlib.
 * - Todos are kept in RAM for bring-up (volatile across reboots); the
 *   previous ESP-IDF nvs.h API is not part of the NuttX app-level ABI.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <netutils/netlib.h>
#include <netutils/ntpclient.h>

#include "net_services.h"
#include "http_util.h"
#include "config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WLAN_IFNAME            "wlan0"

/* Plain-HTTP mirror of AIVOX3_WEATHER_URL (https). Keep in sync with
 * config.h until TLS support lands in http_util. */
#define WEATHER_HTTP_HOST      "api.open-meteo.com"
#define WEATHER_HTTP_PORT      80

/* In-RAM todo store (bring-up; volatile across reboots). */
#define TODO_MAX               8
#define TODO_ITEM_LEN          64

/* Weather code -> short text (subset of WMO codes). */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Minimal WMO weather-code -> text table. */
static const struct
{
  int code;
  const char *text;
} g_wmo[] =
{
  { 0,   "Clear" },     { 1,   "Mainly clear" }, { 2, "Partly cloudy" },
  { 3,   "Overcast" },  { 45,  "Fog" },          { 48, "Rime fog" },
  { 51,  "Light drizzle" }, { 61, "Light rain" }, { 63, "Rain" },
  { 65,  "Heavy rain" }, { 71,  "Light snow" },   { 80, "Rain showers" },
  { 95,  "Thunderstorm" },
};

static char g_todo_items[TODO_MAX][TODO_ITEM_LEN];
static int g_todo_count;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: weather_code_to_text
 ****************************************************************************/

static const char *weather_code_to_text(int code)
{
  int i;
  for (i = 0; i < (int)(sizeof(g_wmo) / sizeof(g_wmo[0])); i++)
    {
      if (g_wmo[i].code == code)
        {
          return g_wmo[i].text;
        }
    }
  return "Unknown";
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: net_init
 ****************************************************************************/

int net_init(void)
{
  return OK;
}

/****************************************************************************
 * Name: net_wifi_connect
 ****************************************************************************/

int net_wifi_connect(const char *ssid, const char *pass)
{
  /* TODO(real-device): esp32s3 STA association. The generic wapi API only
   * exposes wapi_set_essid()/wapi_set_freq(); the WPA passkey path goes
   * through the platform wext extension (SIOCSIWESSID with the extra
   * payload) or wpa_driver_wext_associate(). Wire this up on the board,
   * or rely on CONFIG_ESP32S3_WIFI_SAVE_PARAM auto-join for now. */
  syslog(LOG_WARNING, "net_wifi_connect: not wired yet (ssid=%s)\n",
         ssid != NULL ? ssid : "?");
  return -ENOSYS;
}

/****************************************************************************
 * Name: net_is_connected
 ****************************************************************************/

bool net_is_connected(void)
{
  struct in_addr addr;

  if (netlib_get_ipv4addr(WLAN_IFNAME, &addr) == OK &&
      addr.s_addr != 0 && addr.s_addr != INADDR_NONE)
    {
      return true;
    }

  return false;
}

/****************************************************************************
 * Name: net_wait_linked
 ****************************************************************************/

bool net_wait_linked(int timeout_ms)
{
  int waited = 0;

  while (waited < timeout_ms)
    {
      if (net_is_connected())
        {
          return true;
        }

      usleep(100000);   /* 100 ms */
      waited += 100;
    }

  return false;
}

/****************************************************************************
 * Name: net_ntp_sync
 *
 * Description:
 *   Start the NTP client daemon. It synchronizes the clock in the
 *   background using CONFIG_NETUTILS_NTPCLIENT_SERVER. Later calls are
 *   no-ops while the daemon runs.
 *
 ****************************************************************************/

int net_ntp_sync(void)
{
  int ret = ntpc_start();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: NTP daemon start failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "NTP daemon started (task %d)\n", ret);
  return OK;
}

/****************************************************************************
 * Name: net_get_weather
 ****************************************************************************/

int net_get_weather(char *desc_out, size_t desc_len, int *temp_out)
{
  char path[160];
  char body[512];
  size_t resp_len = 0;
  char *p;
  double temp = 0.0;
  int code = -1;
  int status = 0;
  int ret;

  snprintf(path, sizeof(path),
           "/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true",
           AIVOX3_WEATHER_LAT, AIVOX3_WEATHER_LON);

  ret = http_request(WEATHER_HTTP_HOST, WEATHER_HTTP_PORT, "GET", path,
                     NULL, NULL, 0,
                     body, sizeof(body), &resp_len, &status);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: weather HTTP failed: %d\n", ret);
      return ret;
    }

  if (status != 200)
    {
      syslog(LOG_WARNING, "WARNING: weather HTTP status %d\n", status);
    }

  /* Parse current_weather.temperature and .weathercode (minimal scan). */
  p = strstr(body, "\"temperature\":");
  if (p != NULL)
    {
      temp = atof(p + strlen("\"temperature\":"));
    }

  p = strstr(body, "\"weathercode\":");
  if (p != NULL)
    {
      code = atoi(p + strlen("\"weathercode\":"));
    }

  if (temp_out != NULL)
    {
      *temp_out = (int)(temp + 0.5);
    }

  if (desc_out != NULL && desc_len > 0)
    {
      strncpy(desc_out, weather_code_to_text(code), desc_len - 1);
      desc_out[desc_len - 1] = '\0';
    }

  return OK;
}

/****************************************************************************
 * Name: todo_load_first
 *
 * Description:
 *   Copy the first in-RAM todo item into buf. Returns OK when one exists.
 *
 ****************************************************************************/

int todo_load_first(char *buf, size_t len)
{
  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (g_todo_count <= 0)
    {
      return -ENOENT;
    }

  strncpy(buf, g_todo_items[0], len - 1);
  buf[len - 1] = '\0';
  return OK;
}

/****************************************************************************
 * Name: todo_add
 ****************************************************************************/

int todo_add(const char *text)
{
  if (text == NULL || *text == '\0')
    {
      return -EINVAL;
    }

  if (g_todo_count >= TODO_MAX)
    {
      syslog(LOG_WARNING, "todo list full (%d)\n", TODO_MAX);
      return -ENOSPC;
    }

  strncpy(g_todo_items[g_todo_count], text, TODO_ITEM_LEN - 1);
  g_todo_items[g_todo_count][TODO_ITEM_LEN - 1] = '\0';
  g_todo_count++;
  return OK;
}

/****************************************************************************
 * Name: todo_clear
 ****************************************************************************/

int todo_clear(void)
{
  memset(g_todo_items, 0, sizeof(g_todo_items));
  g_todo_count = 0;
  return OK;
}
