/****************************************************************************
 * apps/examples/desktop_companion/face_follow.c
 *
 * Servo-based face-follow DEMO (no camera on AI-VOX3). See face_follow.h.
 *
 * Uses the BSP servo helper ai_vox3_servo_set_angle() (board.h).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <syslog.h>

#include <arch/board/board.h>

#include "face_follow.h"
#include "config.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Demo animation length (ticks). */
#define DEMO_TICKS           40
/* Peak swing amplitude (degrees) around center (90 deg). */
#define DEMO_SWING_DEG       30

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_enabled     = false;
static bool g_demo_active = false;
static int  g_demo_tick   = 0;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: face_follow_enable
 ****************************************************************************/

int face_follow_enable(bool enable)
{
  int ret;

  g_enabled = enable;
  if (enable)
    {
      /* Park head at center when enabling. */
      ret = ai_vox3_servo_set_angle(AIVOX3_HEAD_PAN_SERVO_ID, 90);
      if (ret == OK)
        {
          ai_vox3_servo_set_angle(AIVOX3_HEAD_TILT_SERVO_ID, 90);
        }
    }

  syslog(LOG_INFO, "face_follow: %s\n", enable ? "enabled (demo)" : "disabled");
  return OK;
}

/****************************************************************************
 * Name: face_follow_update
 ****************************************************************************/

int face_follow_update(int x, int y)
{
  int pan;
  int tilt;

  /* No camera on this board: this is the hook a future detector would call.
   * When coordinates are provided, map them to head angles so the same
   * interface also serves the real (OV2640 + ESP-DL) path. */
  if (x < -100 || x > 100 || y < -100 || y > 100)
    {
      return -EINVAL;
    }

  pan  = 90 + (x * DEMO_SWING_DEG) / 100;
  tilt = 90 - (y * DEMO_SWING_DEG) / 100;

  ai_vox3_servo_set_angle(AIVOX3_HEAD_PAN_SERVO_ID, pan);
  ai_vox3_servo_set_angle(AIVOX3_HEAD_TILT_SERVO_ID, tilt);
  return OK;
}

/****************************************************************************
 * Name: face_follow_trigger_speak
 ****************************************************************************/

int face_follow_trigger_speak(void)
{
  if (!g_enabled)
    {
      return OK;   /* disabled: no animation */
    }

  g_demo_active = true;
  g_demo_tick   = 0;
  return OK;
}

/****************************************************************************
 * Name: face_follow_demo_tick
 ****************************************************************************/

int face_follow_demo_tick(void)
{
  double t;
  int pan;
  int tilt;

  if (!g_demo_active)
    {
      return OK;
    }

  t = (double)g_demo_tick / (double)DEMO_TICKS;   /* 0..1 */

  /* Gentle left-right swing (sin) plus a small nod (cos), returning to center
   * at the end (envelope fades to 0). */
  pan  = 90 + (int)(sin(t * 2.0 * M_PI) * DEMO_SWING_DEG * (1.0 - t));
  tilt = 90 + (int)(cos(t * 2.0 * M_PI) * (DEMO_SWING_DEG / 2) * (1.0 - t));

  ai_vox3_servo_set_angle(AIVOX3_HEAD_PAN_SERVO_ID, pan);
  ai_vox3_servo_set_angle(AIVOX3_HEAD_TILT_SERVO_ID, tilt);

  g_demo_tick++;
  if (g_demo_tick >= DEMO_TICKS)
    {
      /* Return to center and stop. */
      ai_vox3_servo_set_angle(AIVOX3_HEAD_PAN_SERVO_ID, 90);
      ai_vox3_servo_set_angle(AIVOX3_HEAD_TILT_SERVO_ID, 90);
      g_demo_active = false;
    }

  return OK;
}
