/****************************************************************************
 * apps/examples/desktop_companion/net_services.c
 *
 * Implementation of Wi-Fi / NTP / weather / todo services.
 *
 * - Weather uses Open-Meteo (api.open-meteo.com) which needs NO API key,
 *   fetched through the local http_util (plain HTTP).
 * - NTP uses the apps netutils NTP client daemon (ntpc_start).
 * - Wi-Fi station association is performed in-process. Because this board
 *   boots straight into desktop_companion_main
 *   (CONFIG_INIT_ENTRYPOINT="desktop_companion_main", defconfig:322), the
 *   NSH netinit chain never runs and CONFIG_NSH_NETINIT /
 *   CONFIG_NSH_NETINIT_WAPI are dead config, so net_init() has to do what
 *   apps/netutils/netinit would otherwise have done:
 *
 *     netlib_ifup()            -> netinit_net_bringup()   (netinit.c)
 *     net_wifi_connect()       -> netinit_associate()     (netinit_associate.c)
 *     netlib_obtain_ipv4addr() -> netinit_net_bringup()   (netinit.c)
 *
 *   Association itself goes through the in-tree WEXT path:
 *   wpa_driver_wext_associate() (apps/wireless/wapi/src/driver_wext.c), the
 *   very call netinit_associate() makes. It opens its own AF_INET socket, so
 *   an application only has to fill in struct wpa_wconfig_s.
 *
 *   The underlying ioctls (SIOCSIWMODE / SIOCSIWAUTH / SIOCSIWENCODEEXT /
 *   SIOCSIWESSID) are dispatched by the esp32s3 station driver in
 *   arch/xtensa/src/common/espressif/esp_wlan.c:wlan_ioctl(); SIOCSIWESSID
 *   with IW_ESSID_ON is what actually triggers ops->connect().
 *
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

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <nuttx/wireless/wireless.h>

#include <netutils/netlib.h>
#include <netutils/ntpclient.h>

#ifdef CONFIG_WIRELESS_WAPI
#  include <wireless/wapi.h>
#endif

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

/* Bring-up timing. Every wait in net_init() is bounded; the worst case is
 * roughly 18 s and it can never block the UI loop forever:
 *
 *   net_wifi_connect()       - ioctls only, returns as soon as the driver
 *                              has accepted the association request (ms).
 *   netlib_obtain_ipv4addr() - blocking, but internally bounded by
 *                              CONFIG_NETUTILS_DHCPC_RETRIES *
 *                              CONFIG_NETUTILS_DHCPC_RECV_TIMEOUT_MS
 *                              (3 * 3000 ms = 9 s with the board defaults,
 *                              see netutils/dhcpc/Kconfig).
 *   the poll loop below      - NET_ADDR_POLL_MS.
 */
#define NET_ADDR_POLL_MS       9000
#define NET_POLL_STEP_MS       250

/* Wi-Fi credentials. Following the config.h secret convention, the values
 * come from Kconfig only; a missing symbol degrades to the empty string and
 * the passphrase is never written to the log. */
#ifndef CONFIG_AIVOX3_WIFI_SSID
#  define CONFIG_AIVOX3_WIFI_SSID ""
#endif

#ifndef CONFIG_AIVOX3_WIFI_PASSWORD
#  define CONFIG_AIVOX3_WIFI_PASSWORD ""
#endif

/* True when DHCP can be started programmatically. Mirrors the guard that
 * apps/include/netutils/netlib.h places around netlib_obtain_ipv4addr(). */
#if defined(CONFIG_NET_IPv4) && defined(CONFIG_NETUTILS_DHCPC)
#  define NET_HAVE_DHCPC       1
#else
#  undef  NET_HAVE_DHCPC
#endif

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
  { 65,  "Heavy rain" }, { 71, "Light snow" },   { 80, "Rain showers" },
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

#ifndef CONFIG_WIRELESS_WAPI
/****************************************************************************
 * Name: wifi_set_ifname
 *
 * Description:
 *   Copy the station interface name into an iwreq, always NUL terminated.
 *
 ****************************************************************************/

static void wifi_set_ifname(FAR struct iwreq *iwr)
{
  memset(iwr, 0, sizeof(*iwr));
  strncpy(iwr->ifr_name, WLAN_IFNAME, IFNAMSIZ - 1);
  iwr->ifr_name[IFNAMSIZ - 1] = '\0';
}

/****************************************************************************
 * Name: wifi_set_auth_param
 *
 * Description:
 *   One SIOCSIWAUTH round trip, matching
 *   wpa_driver_wext_process_auth_param() in
 *   apps/wireless/wapi/src/driver_wext.c.
 *
 ****************************************************************************/

static int wifi_set_auth_param(int sockfd, int idx, uint32_t value)
{
  struct iwreq iwr;
  int ret;

  wifi_set_ifname(&iwr);
  iwr.u.param.flags = (uint16_t)(idx & IW_AUTH_INDEX);
  iwr.u.param.value = (int32_t)value;

  ret = ioctl(sockfd, SIOCSIWAUTH, (unsigned long)&iwr);
  if (ret < 0)
    {
      ret = -errno;
      if (ret != -EOPNOTSUPP)
        {
          syslog(LOG_ERR, "ERROR: SIOCSIWAUTH(%d=0x%08lx) failed: %d\n",
                 idx, (unsigned long)value, ret);
        }
    }
  else
    {
      ret = OK;
    }

  return ret;
}

/****************************************************************************
 * Name: wifi_associate_wext
 *
 * Description:
 *   Fallback used only when the wapi library is not linked in
 *   (CONFIG_WIRELESS_WAPI off). Issues exactly the ioctl sequence that
 *   wpa_driver_wext_associate() would:
 *
 *     SIOCSIWMODE      <- IW_MODE_INFRA
 *     SIOCSIWAUTH      <- IW_AUTH_WPA_VERSION      = WPA2
 *     SIOCSIWAUTH      <- IW_AUTH_CIPHER_PAIRWISE  = CCMP
 *     SIOCSIWENCODEEXT <- alg = IW_ENCODE_ALG_CCMP, key = passphrase
 *     SIOCSIWESSID     <- flags = IW_ESSID_ON (esp_wlan.c then connects)
 *
 ****************************************************************************/

static int wifi_associate_wext(FAR const char *ssid, FAR const char *pass)
{
  struct iw_encode_ext *ext;
  struct iwreq iwr;
  size_t ssidlen;
  size_t passlen;
  int sockfd;
  int ret = OK;

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "ERROR: wifi socket failed: %d\n", ret);
      return ret;
    }

  /* 1. Infrastructure (station) mode. */

  wifi_set_ifname(&iwr);
  iwr.u.mode = IW_MODE_INFRA;
  if (ioctl(sockfd, SIOCSIWMODE, (unsigned long)&iwr) < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "ERROR: SIOCSIWMODE failed: %d\n", ret);
      goto out_close;
    }

  /* 2/3. WPA2-PSK with AES-CCMP. */

  ret = wifi_set_auth_param(sockfd, IW_AUTH_WPA_VERSION,
                            IW_AUTH_WPA_VERSION_WPA2);
  if (ret < 0)
    {
      goto out_close;
    }

  ret = wifi_set_auth_param(sockfd, IW_AUTH_CIPHER_PAIRWISE,
                            IW_AUTH_CIPHER_CCMP);
  if (ret < 0)
    {
      goto out_close;
    }

  /* 4. The passphrase (WPA2-PSK PMK is derived from it by the driver). */

  passlen = (pass != NULL) ? strlen(pass) : 0;
  if (passlen > 0)
    {
      ext = (FAR struct iw_encode_ext *)malloc(sizeof(*ext) + passlen);
      if (ext == NULL)
        {
          ret = -ENOMEM;
          goto out_close;
        }

      wifi_set_ifname(&iwr);
      memset(ext, 0, sizeof(*ext));
      ext->alg     = IW_ENCODE_ALG_CCMP;
      ext->key_len = (uint16_t)passlen;
      memcpy(ext + 1, pass, passlen);

      iwr.u.encoding.pointer = ext;
      iwr.u.encoding.length  = (uint16_t)(sizeof(*ext) + passlen);

      if (ioctl(sockfd, SIOCSIWENCODEEXT, (unsigned long)&iwr) < 0)
        {
          ret = -errno;
          syslog(LOG_ERR, "ERROR: SIOCSIWENCODEEXT failed: %d\n", ret);
          free(ext);
          goto out_close;
        }

      free(ext);
    }

  /* 5. SSID with IW_ESSID_ON; esp_wlan.c then calls ops->connect(). */

  ssidlen = strlen(ssid);
  wifi_set_ifname(&iwr);
  iwr.u.essid.pointer = (FAR void *)ssid;
  iwr.u.essid.length  = (uint16_t)ssidlen;
  iwr.u.essid.flags   = IW_ESSID_ON;

  if (ioctl(sockfd, SIOCSIWESSID, (unsigned long)&iwr) < 0)
    {
      ret = -errno;
      syslog(LOG_ERR, "ERROR: SIOCSIWESSID failed: %d\n", ret);
      goto out_close;
    }

  ret = OK;

out_close:
  close(sockfd);
  return ret;
}
#endif /* !CONFIG_WIRELESS_WAPI */

/****************************************************************************
 * Name: wifi_associate
 *
 * Description:
 *   Program wlan0 for WPA2-PSK (AES-CCMP) and kick off association.
 *
 * Input Parameters:
 *   ssid - NUL-terminated SSID, non-empty.
 *   pass - NUL-terminated WPA2 passphrase, may be empty (open network).
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

static int wifi_associate(FAR const char *ssid, FAR const char *pass)
{
#ifdef CONFIG_WIRELESS_WAPI
  struct wpa_wconfig_s conf;
  size_t ssidlen = strlen(ssid);
  size_t passlen = (pass != NULL) ? strlen(pass) : 0;

  memset(&conf, 0, sizeof(conf));

  conf.ifname      = WLAN_IFNAME;
  conf.sta_mode    = WAPI_MODE_MANAGED;         /* == IW_MODE_INFRA      */
  conf.auth_wpa    = IW_AUTH_WPA_VERSION_WPA2;  /* 0x04                  */
  conf.cipher_mode = IW_AUTH_CIPHER_CCMP;       /* 0x08                  */
  conf.alg         = WPA_ALG_CCMP;
  conf.freq        = 0.0;                       /* let the driver scan   */
  conf.flag        = WAPI_FREQ_AUTO;
  conf.ssidlen     = (uint8_t)ssidlen;
  conf.phraselen   = (uint8_t)passlen;
  conf.ssid        = ssid;
  conf.bssid       = NULL;
  conf.passphrase  = (passlen > 0) ? pass : NULL;

  return wpa_driver_wext_associate(&conf);
#else
  return wifi_associate_wext(ssid, pass);
#endif
}

/****************************************************************************
 * Name: net_log_ipv4
 *
 * Description:
 *   Log the address we ended up with. Never called with a secret.
 *
 ****************************************************************************/

static void net_log_ipv4(void)
{
  struct in_addr addr;
  char buf[INET_ADDRSTRLEN];

  if (netlib_get_ipv4addr(WLAN_IFNAME, &addr) == OK &&
      inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != NULL)
    {
      syslog(LOG_INFO, "net: %s IPv4 %s\n", WLAN_IFNAME, buf);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: net_init
 *
 * Description:
 *   Bring the station up in-process: ifup, associate, DHCP, then wait for
 *   a usable IPv4 address. This replaces the NSH netinit step that this
 *   board never reaches because CONFIG_INIT_ENTRYPOINT is
 *   desktop_companion_main.
 *
 *   Bounded: returns a negated errno instead of blocking. main.c ignores the
 *   return value, so every failure path is also written to syslog.
 *
 * Returned Value:
 *   OK when net_is_connected() is true; a negated errno otherwise.
 *
 ****************************************************************************/

int net_init(void)
{
  int ret;
  int waited;

  if (net_is_connected())
    {
      syslog(LOG_INFO, "net_init: %s already has an IPv4 address\n",
             WLAN_IFNAME);
      net_log_ipv4();
      return OK;
    }

  /* 1. Interface up. Equivalent to netinit_net_bringup() in netinit.c. */

  ret = netlib_ifup(WLAN_IFNAME);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: net_init: ifup %s failed: %d\n",
             WLAN_IFNAME, ret);
      return ret;
    }

  /* 2. Associate. Equivalent to netinit_associate() in
   *    netinit_associate.c. NULL/NULL means "use the Kconfig credentials". */

  ret = net_wifi_connect(NULL, NULL);
  if (ret < 0)
    {
      /* net_wifi_connect() has already logged the reason. */
      return ret;
    }

  /* 3. DHCP. netlib_obtain_ipv4addr() is blocking but self-bounded by the
   *    DHCPC retry/timeout config, so it cannot wedge the caller.
   */

#ifdef NET_HAVE_DHCPC
  ret = netlib_obtain_ipv4addr(WLAN_IFNAME);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: net_init: DHCP on %s failed: %d\n",
             WLAN_IFNAME, ret);
    }
#endif

  /* 4. Wait for the address to become visible on the interface. */

  for (waited = 0; waited < NET_ADDR_POLL_MS; waited += NET_POLL_STEP_MS)
    {
      if (net_is_connected())
        {
          net_log_ipv4();
          return OK;
        }

      usleep(NET_POLL_STEP_MS * 1000);
    }

  syslog(LOG_ERR,
         "ERROR: net_init: no IPv4 on %s after association "
         "(ssid=%s, %d ms). Check CONFIG_NETDEV_WIRELESS_IOCTL, "
         "CONFIG_WIRELESS_WAPI and the AP credentials.\n",
         WLAN_IFNAME, CONFIG_AIVOX3_WIFI_SSID, NET_ADDR_POLL_MS);

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: net_wifi_connect
 *
 * Description:
 *   Associate wlan0 with a WPA2-PSK access point. Empty/NULL arguments fall
 *   back to the Kconfig strings CONFIG_AIVOX3_WIFI_SSID and
 *   CONFIG_AIVOX3_WIFI_PASSWORD.
 *
 *   The SSID is logged, the passphrase NEVER is (same convention as
 *   CONFIG_AIVOX3_LLM_API_KEY in config.h).
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int net_wifi_connect(FAR const char *ssid, FAR const char *pass)
{
  FAR const char *use_ssid = ssid;
  FAR const char *use_pass = pass;
  int ret;

  if (use_ssid == NULL || *use_ssid == '\0')
    {
      use_ssid = CONFIG_AIVOX3_WIFI_SSID;
    }

  if (use_pass == NULL || *use_pass == '\0')
    {
      use_pass = CONFIG_AIVOX3_WIFI_PASSWORD;
    }

  if (*use_ssid == '\0')
    {
      syslog(LOG_ERR,
             "ERROR: net_wifi_connect: no SSID configured. Set "
             "CONFIG_AIVOX3_WIFI_SSID (menuconfig) or pass one in.\n");
      return -EINVAL;
    }

  /* Log the SSID and only the *fact* that a passphrase is present. */
  syslog(LOG_INFO, "net_wifi_connect: ssid=%s, passphrase=%s\n",
         use_ssid, (*use_pass != '\0') ? "set (not logged)" : "none");

  ret = wifi_associate(use_ssid, use_pass);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: net_wifi_connect: association to '%s' "
             "failed: %d\n", use_ssid, ret);
      return ret;
    }

  syslog(LOG_INFO, "net_wifi_connect: association request to '%s' "
         "accepted\n", use_ssid);
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
