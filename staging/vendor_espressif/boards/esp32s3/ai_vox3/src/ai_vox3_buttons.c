/****************************************************************************
 * src/ai_vox3_buttons.c
 *
 * On-board button driver for the emakefun AI-VOX3.
 *
 * Buttons (external pull-up, active LOW):
 *   A    = IO46
 *   B    = IO45
 *   BOOT = IO0
 *
 * Uses the official ESP32-S3 GPIO IRQ pattern from the upstream board
 * code (boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_buttons.c):
 *   esp32s3_configgpio(pin, INPUT | PULLUP)
 *   irq = ESP32S3_PIN2IRQ(pin)
 *   irq_attach(irq, isr, arg)
 *   esp32s3_gpioirqenable(irq, FALLING)
 *
 * NOTE: no up_enable_irq() call -- second-level GPIO interrupts are
 * enabled by esp32s3_gpioirqenable() itself (official boards do not
 * call up_enable_irq for these).
 *
 * A single application callback receives the button id (AI_VOX3_BTN_*).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>

#include <arch/board/board.h>

#include "esp32s3_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Button id -> GPIO pin. */
#define BTN_GPIO(id)                                            \
  ((id) == AI_VOX3_BTN_A     ? BOARD_BTN_A_GPIO     :           \
   (id) == AI_VOX3_BTN_B     ? BOARD_BTN_B_GPIO     :           \
   (id) == AI_VOX3_BTN_BOOT  ? BOARD_BTN_BOOT_GPIO : -1)

/* Input pin config: input with pull-up (active LOW press). */
#define BTN_PINCFG             (INPUT | PULLUP)

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef void (*button_callback_t)(int btn_id);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static button_callback_t g_btn_callback = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: button_isr
 *
 * Description:
 *   GPIO interrupt handler (falling edge = press).  Dispatches the button
 *   id to the application callback.  The callback runs in interrupt
 *   context and must be short.
 *
 ****************************************************************************/

static int button_isr(int irq, FAR void *context, FAR void *arg)
{
  int btn_id = (int)(uintptr_t)arg;

  if (g_btn_callback != NULL)
    {
      g_btn_callback(btn_id);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_buttons_initialize
 *
 * Description:
 *   Configure each button pin as input with pull-up and attach a
 *   falling-edge interrupt.  Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_buttons_initialize(void)
{
  int id;
  int pin;
  int irq;
  int ret;

  for (id = 0; id < AI_VOX3_BTN_COUNT; id++)
    {
      pin = BTN_GPIO(id);
      if (pin < 0)
        {
          continue;
        }

      esp32s3_configgpio(pin, BTN_PINCFG);

#ifdef CONFIG_ESP32S3_GPIO_IRQ
      irq = ESP32S3_PIN2IRQ(pin);
      ret = irq_attach(irq, button_isr, (FAR void *)(uintptr_t)id);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: button %d irq_attach failed: %d\n",
                 id, ret);
          return ret;
        }

      /* Enable the pin interrupt on the falling edge (press). */
      esp32s3_gpioirqenable(irq, FALLING);
#endif
    }

  syslog(LOG_INFO, "Buttons x%d initialized\n", AI_VOX3_BTN_COUNT);
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_buttons_register_callback
 *
 * Description:
 *   Register the application callback invoked on a button press.  Only
 *   one callback is supported (last registration wins).  Returns OK.
 *
 ****************************************************************************/

int ai_vox3_buttons_register_callback(ai_vox3_button_callback_t cb)
{
  g_btn_callback = cb;
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_buttons_read
 *
 * Description:
 *   Polled read of a button.  Returns 1 if pressed (active LOW), 0
 *   otherwise.  Returns -EINVAL for a bad id.
 *
 ****************************************************************************/

int ai_vox3_buttons_read(int btn_id)
{
  int pin = BTN_GPIO(btn_id);

  if (pin < 0)
    {
      return -EINVAL;
    }

  /* Active LOW: reading 0 means pressed. */
  return esp32s3_gpioread(pin) == 0 ? 1 : 0;
}
