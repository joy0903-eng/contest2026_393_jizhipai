/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 WS2812 RGB (M2 placeholder)
 *
 * The WS2812 is wired to a plain GPIO (41) with a single-wire protocol.
 * The base tree has no GPIO bit-bang / RMT WS2812 lower half, so this is
 * intentionally a no-op stub. The pin map is kept in board.h
 * (AIVOX3_WS2812_GPIO) so M2 can drop in a real driver.
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>

#include "aivox3.h"

#ifdef CONFIG_AIVOX3_WS2812
int aivox3_ws2812_initialize(void)
{
  syslog(LOG_INFO,
         "AI-VOX3 WS2812 (GPIO%d): deferred to M2 (needs bit-bang/RMT)\n",
         AIVOX3_WS2812_GPIO);
  return OK;
}
#endif
