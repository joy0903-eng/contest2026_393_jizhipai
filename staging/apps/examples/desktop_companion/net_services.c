/****************************************************************************
 * apps/examples/desktop_companion/net_services.c
 *
 * Implementation of Wi-Fi / NTP / weather / todo(NVS) services.
 *
 * Weather uses Open-Meteo (https://api.open-meteo.com) which needs NO API key.
 * Wi-Fi credentials are NOT stored in source — they are passed in at runtime
 * (or read from saved params).
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
#include <time.h>
#include <syslog.h>

#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <netutils/netlib.h>
#include <nuttx/clock.h>
#include <nuttx/net/webclient.h>
#include <wireless/wapi.h>

#include <nvs.h>

#include "net_services.h"
#include "config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WLAN_IFNAME            "wlan0"
#define NVS_NAMESPACE          "aivox3"
#define NVS_TODO_COUNT_KEY     "todocount"
#define NVS_TODO_PREFIX        "todo"

/* Weather code -> short text (subset of WMO codes). */
#define WEATHER_CODE_MAX       12

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
 * Name: fetch_callback
 *
 * Description:
 *   webclient body accumulator. Drops the HTTP header (up to \r\n\r\n).
 *
 ****************************************************************************/

struct fetch_buf_s
{
  char *buf;
  size_t cap;
  size_t len;
  bool headers_done;
};

static int fetch_callback(FAR struct webclient_session *s,
                          FAR const char *buf, size_t len, FAR void *arg)
{
  struct fetch_buf_s *fb = (struct fetch_buf_s *)arg;
  size_t i = 0;

  if (!fb->headers_done)
    {
      /* Search for the end of the header section. */
      for (i = 0; i + 3 < len; i++)
        {
          if (buf[i] == '\r' && buf[i + 1] == '\n' &&
              buf[i + 2] == '\r' && buf[i + 3] == '\n')
            {
              i += 4;
              fb->headers_done = true;
              break;
            }
        }

      if (!fb->headers_done)
        {
          return 0;   /* still in headers */
        }
    }

  /* Append body bytes. */
  if (fb->len + (len - i) < fb->cap)
    {
      memcpy(fb->buf + fb->len, buf + i, len - i);
      fb->len += (len - i);
    }

  return 0;
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
  int ret;

  /* wapi associates the STA interface with the given SSID/PSK. If ssid is
   * NULL, the saved params (esp32s3_wifi_save_param) are used. */
  ret = wapi_set_sta_connect(WLAN_IFNAME, ssid, pass);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: wifi connect failed: %d\n", ret);
      return ret;
    }

  return OK;
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
 ****************************************************************************/

int net_ntp_sync(void)
{
  /* clock_synchronize() performs an NTP request using the configured server
   * (CONFIG_NETUTILS_NTPCLIENT + CONFIG_NETUTILS_NTPCLIENT_SERVER). It blocks
   * until the clock is updated or it fails. */
  int ret = clock_synchronize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: NTP sync failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: net_get_weather
 ****************************************************************************/

int net_get_weather(char *desc_out, size_t desc_len, int *temp_out)
{
  char url[160];
  char body[256];
  struct fetch_buf_s fb;
  struct webclient_context ctx;
  char *p;
  double temp = 0.0;
  int code = -1;
  int ret;

  snprintf(url, sizeof(url),
           "%s?latitude=%f&longitude=%f&current_weather=true",
           AIVOX3_WEATHER_URL, AIVOX3_WEATHER_LAT, AIVOX3_WEATHER_LON);

  memset(&fb, 0, sizeof(fb));
  fb.buf = body;
  fb.cap = sizeof(body);

  memset(&ctx, 0, sizeof(ctx));
  ctx.url     = url;
  ctx.method  = "GET";
  ctx.callback = fetch_callback;
  ctx.cbarg   = &fb;

  ret = webclient_perform(&ctx);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: weather HTTP failed: %d\n", ret);
      return ret;
    }

  body[fb.len] = '\0';

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
 ****************************************************************************/

int todo_load_first(char *buf, size_t len)
{
  nvs_handle_t h;
  size_t rlen = len;
  int ret;

  ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
  if (ret != OK)
    {
      return ret;
    }

  ret = nvs_get_str(h, NVS_TODO_PREFIX "0", buf, &rlen);
  nvs_close(h);
  return ret;
}

/****************************************************************************
 * Name: todo_add
 ****************************************************************************/

int todo_add(const char *text)
{
  nvs_handle_t h;
  int count = 0;
  size_t rlen = sizeof(count);
  char key[16];
  int ret;

  if (text == NULL || *text == '\0')
    {
      return -EINVAL;
    }

  ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (ret != OK)
    {
      return ret;
    }

  /* Read current count (default 0). */
  if (nvs_get_i32(h, NVS_TODO_COUNT_KEY, (int32_t *)&count) != OK)
    {
      count = 0;
    }

  /* Store next item and bump the counter. */
  snprintf(key, sizeof(key), NVS_TODO_PREFIX "%d", count);
  ret = nvs_set_str(h, key, text);
  if (ret == OK)
    {
      count++;
      nvs_set_i32(h, NVS_TODO_COUNT_KEY, count);
      nvs_commit(h);
    }

  nvs_close(h);
  return ret;
}

/****************************************************************************
 * Name: todo_clear
 ****************************************************************************/

int todo_clear(void)
{
  nvs_handle_t h;
  int count = 0;
  char key[16];
  int i;
  int ret;

  ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (ret != OK)
    {
      return ret;
    }

  if (nvs_get_i32(h, NVS_TODO_COUNT_KEY, (int32_t *)&count) == OK)
    {
      for (i = 0; i < count; i++)
        {
          snprintf(key, sizeof(key), NVS_TODO_PREFIX "%d", i);
          nvs_erase_key(h, key);
        }

      nvs_set_i32(h, NVS_TODO_COUNT_KEY, 0);
      nvs_commit(h);
    }

  nvs_close(h);
  return OK;
}
