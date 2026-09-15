/****************************************************************************
 * src/ai_vox3_servo.c
 *
 * 4-channel servo driver (LEDC PWM @ 50 Hz) for the emakefun AI-VOX3.
 *
 * Servo GPIOs (board.h): IO42 / IO43 / IO44 / IO48  (servo id 0..3).
 * Servo positioning uses the standard 50 Hz PWM convention:
 *   0 deg   ~ 500 us pulse
 *   180 deg ~ 2500 us pulse
 * with a 20 ms (50 Hz) period.
 *
 * The LEDC resolution is assumed 16-bit (max duty 65535). If the LEDC timer
 * is configured with a different bit-depth on the build machine, adjust the
 * duty math in servo_set_angle().
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fixedmath.h>
#include <syslog.h>

#include <esp32s3_ledc.h>

#include "board.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* LEDC timer used for all four servos. */
#define SERVO_LEDC_TIMER       0

/* Assumed LEDC resolution (bits). 16-bit -> max duty 65535. */
#define SERVO_LEDC_RES_BITS    16
#define SERVO_LEDC_MAX_DUTY    ((1 << SERVO_LEDC_RES_BITS) - 1)

/* 50 Hz servo timing (microseconds). */
#define SERVO_PERIOD_US        20000
#define SERVO_PULSE_MIN_US     500
#define SERVO_PULSE_MAX_US     2500

/* Map servo id -> GPIO. */
#define SERVO_GPIO(id)                                  \
  ((id) == 0 ? BOARD_SERVO0_GPIO :                     \
   (id) == 1 ? BOARD_SERVO1_GPIO :                     \
   (id) == 2 ? BOARD_SERVO2_GPIO : BOARD_SERVO3_GPIO)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_servo_inited = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: servo_angle_to_duty
 *
 * Description:
 *   Convert a servo angle (0..180 deg) to a 16-bit LEDC duty value.
 *
 ****************************************************************************/

static uint32_t servo_angle_to_duty(int angle_deg)
{
  long pulse_us;

  if (angle_deg < 0)
    {
      angle_deg = 0;
    }
  else if (angle_deg > 180)
    {
      angle_deg = 180;
    }

  /* Linear map: 0deg -> 500us, 180deg -> 2500us. */
  pulse_us = SERVO_PULSE_MIN_US +
             ((long)angle_deg * (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US)) / 180;

  /* duty = pulse_us / period_us * max_duty */
  return (uint32_t)((pulse_us * SERVO_LEDC_MAX_DUTY) / SERVO_PERIOD_US);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_servo_initialize
 *
 * Description:
 *   Configure the LEDC timer for 50 Hz and bind each servo GPIO to its
 *   LEDC channel. Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_servo_initialize(void)
{
  int id;
  int ret;

  if (g_servo_inited)
    {
      return OK;
    }

  /* Configure the LEDC timer to 50 Hz. The exact timer-setup API depends on
   * the tree version (esp32s3_ledc.h). Adjust if your tree uses a struct
   * ledc_timer_config_t instead of a simple frequency setter. */
  ret = esp32s3_ledc_set_timer(SERVO_LEDC_TIMER, BOARD_SERVO_FREQ_HZ,
                               SERVO_LEDC_RES_BITS);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: servo LEDC timer setup failed: %d\n", ret);
      return ret;
    }

  for (id = 0; id < BOARD_SERVO_COUNT; id++)
    {
      ret = esp32s3_ledc_gpio(id, SERVO_GPIO(id));
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: servo %d gpio(%d) bind failed: %d\n",
                 id, SERVO_GPIO(id), ret);
          return ret;
        }

      /* Park at center (90 deg) by default. */
      ret = ai_vox3_servo_set_angle(id, 90);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: servo %d park failed: %d\n", id, ret);
          return ret;
        }
    }

  g_servo_inited = true;
  syslog(LOG_INFO, "Servo x%d (LEDC 50Hz) initialized\n", BOARD_SERVO_COUNT);
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_servo_set_angle
 *
 * Description:
 *   Set a servo (0..3) to the given angle (0..180 deg). Returns OK on
 *   success, -EINVAL for a bad id.
 *
 ****************************************************************************/

int ai_vox3_servo_set_angle(int servo_id, int angle_deg)
{
  uint32_t duty;

  if (servo_id < 0 || servo_id >= BOARD_SERVO_COUNT)
    {
      return -EINVAL;
    }

  if (!g_servo_inited)
    {
      return -EAGAIN;
    }

  duty = servo_angle_to_duty(angle_deg);
  return esp32s3_ledc_duty_set(servo_id, (int)duty);
}
