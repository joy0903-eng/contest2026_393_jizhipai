/****************************************************************************
 * apps/examples/desktop_companion/smart_home.c
 *
 * Smart-home intent stub. See smart_home.h.
 *
 * Intent JSON is parsed with minimal string scanning (no full JSON parser):
 * it looks for "device":"..." and "action":"...". The actual control command
 * is only issued when a real backend is configured; otherwise it is logged
 * and "unconfigured" is reported.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>
#include <syslog.h>

#include "smart_home.h"
#include "config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SMART_HOME_BACKEND
#define CONFIG_SMART_HOME_BACKEND "none"
#endif

#define BACKEND_NONE   "none"
#define BACKEND_MQTT   "mqtt"
#define BACKEND_HTTP   "http"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_backend = CONFIG_SMART_HOME_BACKEND;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: extract_field
 *
 * Description:
 *   Find "key":"value" and copy value into out (up to out_len).
 *
 ****************************************************************************/

static void extract_field(const char *json, const char *key,
                          char *out, size_t out_len)
{
  char pat[32];
  const char *p;
  const char *q;
  size_t j = 0;

  out[0] = '\0';
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);

  p = strstr(json, pat);
  if (p == NULL)
    {
      return;
    }

  p += strlen(pat);
  q = p;
  while (*q != '\0' && *q != '"' && j + 1 < out_len)
    {
      out[j++] = *q++;
    }

  out[j] = '\0';
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: smart_home_set_backend
 ****************************************************************************/

int smart_home_set_backend(const char *backend)
{
  if (backend == NULL)
    {
      return -EINVAL;
    }

  g_backend = backend;
  syslog(LOG_INFO, "smart_home: backend = %s\n", g_backend);
  return OK;
}

/****************************************************************************
 * Name: smart_home_control
 ****************************************************************************/

int smart_home_control(const char *intent_json, char *msg_out, size_t msg_len)
{
  char device[32];
  char action[32];
  int ret = -EOPNOTSUPP;

  if (intent_json == NULL)
    {
      return -EINVAL;
    }

  extract_field(intent_json, "device", device, sizeof(device));
  extract_field(intent_json, "action", action, sizeof(action));

  syslog(LOG_INFO, "smart_home: intent device='%s' action='%s' backend=%s\n",
         device, action, g_backend);

  if (strcmp(g_backend, BACKEND_NONE) == 0)
    {
      /* No platform configured: log intent, report unconfigured. */
      if (msg_out != NULL && msg_len > 0)
        {
          snprintf(msg_out, msg_len,
                   "smart-home: no backend configured (intent logged)");
        }

      return -EOPNOTSUPP;
    }

  /* TODO(real-device): implement mqtt/http adapters. For example:
   *   - mqtt: publish "home/<device>/set" with action payload via an MQTT
   *           client (e.g. to Home Assistant).
   *   - http: POST the intent to the platform open API.
   * The intent parsing above is reused as-is. */
  if (msg_out != NULL && msg_len > 0)
    {
      snprintf(msg_out, msg_len, "smart-home: backend '%s' not yet implemented",
               g_backend);
    }

  return ret;
}
