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
 * Provides edge-triggered interrupts (falling edge) plus a polled read.
 * A single application callback receives the button id (AI_VOX3_BTN_*).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/irq.h>
#include <arch/board/board.h>
#include <esp32s3_gpio.h>


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Button id -> GPIO pin. */
#define BTN_GPIO(id)                                            \
  ((id) == AI_VOX3_BTN_A     ? BOARD_BTN_A_GPIO     :           \
   (id) == AI_VOX3_BTN_B     ? BOARD_BTN_B_GPIO     :           \
   (id) == AI_VOX3_BTN_BOOT  ? BOARD_BTN_BOOT_GPIO : -1)

/* Simple software debounce (ticks). */
#define BTN_DEBOUNCE_TICKS     3

/* Input pin config: pull-up, interrupt on falling edge (active LOW press). */
#define BTN_PINCFG(pin)                                        \
  (GPIO_INPUT_PINMUX(pin) | GPIO_PULLUP | GPIO_INTR_NEGEDGE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef void (*button_callback_t)(int btn_id);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static button_callback_t g_btn_callback = NULL;
static uint32_t g_btn_last_isr[AI_VOX3_BTN_COUNT];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: button_isr
 *
 * Description:
 *   Shared GPIO interrupt handler. Dispatches the button id to the callback.
 *
 ****************************************************************************/

static int button_isr(int irq, void *context, void *arg)
{
  int btn_id = (int)(uintptr_t)arg;

  if (g_btn_callback != NULL)
    {
      g_btn_callback(btn_id);
    }

  /* Re-enable the pin interrupt for the next edge. */
  esp32s3_gpioirq(BTN_GPIO(btn_id));
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_buttons_initialize
 *
 * Description:
 *   Configure each button pin as an input with pull-up and falling-edge
 *   interrupt. Returns OK on success.
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

      esp32s3_gpio_config(BTN_PINCFG(pin));

      /* Enable the GPIO as an interrupt source and attach the ISR. */
      esp32s3_gpioirq(pin);
      irq = ESP32S3_PIN2IRQ(pin);
      ret = irq_attach(irq, button_isr, (void *)(uintptr_t)id);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: button %d irq_attach failed: %d\n", id, ret);
          return ret;
        }

      up_enable_irq(irq);
    }

  syslog(LOG_INFO, "Buttons x%d initialized\n", AI_VOX3_BTN_COUNT);
  return OK;
}

/****************************************************************************
 * Name: ai_vox3_buttons_register_callback
 *
 * Description:
 *   Register the application callback invoked on a button press. Only one
 *   callback is supported (last registration wins).
 *
 ****************************************************************************/

void ai_vox3_buttons_register_callback(button_callback_t cb)
{
  g_btn_callback = cb;
}

/****************************************************************************
 * Name: ai_vox3_buttons_read
 *
 * Description:
 *   Polled read of a button. Returns 1 if pressed (active LOW), 0 otherwise.
 *   Returns -EINVAL for a bad id.
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
  return esp32s3_gpio_read(pin) == 0 ? 1 : 0;
}
