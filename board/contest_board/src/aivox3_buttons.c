/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 onboard buttons
 *
 * Three buttons, active-low:
 *   BTN1 = GPIO46, BTN2 = GPIO45, BTN3 = GPIO0
 *
 * Implements the standard NuttX board_button_initialize() / board_buttons()
 * API so the generic buttons driver can expose /dev/buttons.
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <stdbool.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/irq.h>

#include "esp32s3_gpio.h"
#include "hardware/esp32s3_gpio_sigmap.h"

#include "aivox3.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint32_t g_btn_gpio[] =
{
  AIVOX3_BTN_1_GPIO,  /* BTN1 */
  AIVOX3_BTN_2_GPIO,  /* BTN2 */
  AIVOX3_BTN_3_GPIO   /* BTN3 */
};

#define NBTN (sizeof(g_btn_gpio) / sizeof(g_btn_gpio[0]))

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_button_initialize
 ****************************************************************************/

uint32_t board_button_initialize(void)
{
  int i;

  for (i = 0; i < NBTN; i++)
    {
      esp32s3_configgpio(g_btn_gpio[i], INPUT_FUNCTION_2 | PULLUP);
    }

  return NBTN;
}

/****************************************************************************
 * Name: board_buttons
 ****************************************************************************/

uint32_t board_buttons(void)
{
  uint32_t ret = 0;
  int i;

  for (i = 0; i < NBTN; i++)
    {
      /* Active-low: a pressed button reads 0 */
      if (!esp32s3_gpioread(g_btn_gpio[i]))
        {
          ret |= (1u << i);
        }
    }

  return ret;
}
