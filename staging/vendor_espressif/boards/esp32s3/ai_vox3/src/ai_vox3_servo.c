/****************************************************************************
 * src/ai_vox3_servo.c
 *
 * 4-channel servo driver (LEDC PWM @ 50 Hz) for the emakefun AI-VOX3.
 *
 * Servo GPIOs (board.h): IO42 / IO43 / IO44 / IO48  (servo id 0..3).
 *
 * Uses the official ESP32-S3 LEDC lower-half API exactly as the upstream
 * board code does (boards/xtensa/esp32s3/common/src/esp32s3_board_ledc.c):
 *   esp32s3_ledc_init(timer) -> struct pwm_lowerhalf_s *
 *   dev->ops->setup(dev)
 *   dev->ops->start(dev, &info)
 *
 * One LEDC timer per servo (CONFIG_ESP32S3_LEDC_TIM0..3 with
 * _CHANNELS=1), the channel GPIOs are routed by the chip driver from
 * CONFIG_ESP32S3_LEDC_CHANNEL0..3_PIN (42/43/44/48) -- servo id i maps to
 * timer i / channel i.
 *
 * Servo positioning uses the standard 50 Hz PWM convention:
 *   0 deg   ~ 500 us pulse
 *   180 deg ~ 2500 us pulse
 * with a 20 ms (50 Hz) period.  Duty follows the NuttX pwm_info_s
 * convention: ub16_t, 65535 = ~100% of the period.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/timers/pwm.h>

#include <arch/board/board.h>

#include "esp32s3_ledc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 50 Hz servo timing (microseconds). */
#define SERVO_PERIOD_US        20000
#define SERVO_PULSE_MIN_US     500
#define SERVO_PULSE_MAX_US     2500

/* Full-scale duty for the ub16_t duty field. */
#define SERVO_DUTY_FULL        65536

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_servo_inited = false;
static struct pwm_lowerhalf_s *g_servo_lower[BOARD_SERVO_COUNT];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: servo_angle_to_duty
 *
 * Description:
 *   Convert a servo angle (0..180 deg) to a ub16_t PWM duty value.
 *   0 deg -> 500us, 180 deg -> 2500us over a 20 ms period.
 *
 ****************************************************************************/

static uint32_t servo_angle_to_duty(int angle_deg)
{
  long pulse_us;
  long duty;

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

  /* duty = pulse_us / period_us * 65536, clamped to [1, 65535]
   * (duty 0 means "output held low" and is not a valid pulse). */
  duty = (pulse_us * SERVO_DUTY_FULL) / SERVO_PERIOD_US;

  if (duty < 1)
    {
      duty = 1;
    }
  else if (duty > 65535)
    {
      duty = 65535;
    }

  return (uint32_t)duty;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_servo_initialize
 *
 * Description:
 *   Initialize one LEDC lower-half per servo (timer i for servo i),
 *   run the driver setup and expose each timer as /dev/pwmN following the
 *   official esp32s3_board_ledc.c pattern, then park all servos at 90 deg.
 *
 ****************************************************************************/

int ai_vox3_servo_initialize(void)
{
  struct pwm_info_s info;
  char devpath[16];
  int id;
  int ret;

  if (g_servo_inited)
    {
      return OK;
    }

  memset(g_servo_lower, 0, sizeof(g_servo_lower));

  for (id = 0; id < BOARD_SERVO_COUNT; id++)
    {
      /* Get the LEDC lower-half for timer <id> (one channel each). */
      g_servo_lower[id] = esp32s3_ledc_init(id);
      if (g_servo_lower[id] == NULL)
        {
          syslog(LOG_ERR, "ERROR: servo %d ledc_init failed\n", id);
          return -ENODEV;
        }

      ret = g_servo_lower[id]->ops->setup(g_servo_lower[id]);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: servo %d ledc setup failed: %d\n",
                 id, ret);
          return ret;
        }

      /* Expose the standard PWM character device (official pattern;
       * handy for NSH-based bring-up debugging). */
      snprintf(devpath, sizeof(devpath), "/dev/pwm%d", id);
      ret = pwm_register(devpath, g_servo_lower[id]);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: servo %d pwm_register failed: %d\n",
                 id, ret);
          return ret;
        }
    }

  /* Park all servos at center (90 deg). */
  for (id = 0; id < BOARD_SERVO_COUNT; id++)
    {
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
 *   Set a servo (0..3) to the given angle (0..180 deg).  Starts the PWM
 *   output on the servo's LEDC timer.  Returns OK on success, -EINVAL for
 *   a bad id and -EAGAIN when called before ai_vox3_servo_initialize().
 *
 ****************************************************************************/

int ai_vox3_servo_set_angle(int servo_id, int angle_deg)
{
  struct pwm_info_s info;
  struct pwm_lowerhalf_s *dev;
  uint32_t duty;

  if (servo_id < 0 || servo_id >= BOARD_SERVO_COUNT)
    {
      return -EINVAL;
    }

  if (!g_servo_inited || g_servo_lower[servo_id] == NULL)
    {
      return -EAGAIN;
    }

  duty = servo_angle_to_duty(angle_deg);

  memset(&info, 0, sizeof(info));
  info.frequency = BOARD_SERVO_FREQ_HZ;

#ifdef CONFIG_PWM_MULTICHAN
  /* This lower half owns exactly one channel (TIMx_CHANNELS=1), so the
   * first entry of the per-channel array carries the duty. */
  info.channels[0].duty = (ub16_t)duty;
#else
  info.duty = (ub16_t)duty;
#endif

  dev = g_servo_lower[servo_id];
  return dev->ops->start(dev, &info);
}
