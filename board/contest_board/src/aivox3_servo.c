/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 pan/tilt servo (LEDC PWM)
 *
 * PAN  = GPIO42  (LEDC timer 0, channel 0)
 * TILT = GPIO43  (LEDC timer 1, channel 1)
 *
 * Registers two PWM character devices:
 *   /dev/pwm0  -> PAN
 *   /dev/pwm1  -> TILT
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <syslog.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/timers/pwm.h>

#include "esp32s3_ledc.h"
#include "aivox3.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: aivox3_servo_initialize
 ****************************************************************************/

int aivox3_servo_initialize(void)
{
  struct pwm_lowerhalf_s *pwm;
  int ret;

  /* PAN */
  pwm = esp32s3_ledc_init(AIVOX3_LEDC_TIMER_PAN);
  if (!pwm)
    {
      syslog(LOG_ERR, "ERROR: LEDC timer %d (PAN) init failed\n",
             AIVOX3_LEDC_TIMER_PAN);
      return -ENODEV;
    }

  ret = pwm_register("/dev/pwm0", pwm, 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: pwm_register(/dev/pwm0) failed: %d\n", ret);
      return ret;
    }

  /* TILT */
  pwm = esp32s3_ledc_init(AIVOX3_LEDC_TIMER_TILT);
  if (!pwm)
    {
      syslog(LOG_ERR, "ERROR: LEDC timer %d (TILT) init failed\n",
             AIVOX3_LEDC_TIMER_TILT);
      return -ENODEV;
    }

  ret = pwm_register("/dev/pwm1", pwm, 1, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: pwm_register(/dev/pwm1) failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "AI-VOX3 servos ready at /dev/pwm0 (PAN), /dev/pwm1 (TILT)\n");
  return OK;
}
