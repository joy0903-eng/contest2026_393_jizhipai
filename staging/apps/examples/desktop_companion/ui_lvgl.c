/****************************************************************************
 * apps/examples/desktop_companion/ui_lvgl.c
 *
 * LVGL UI implementation for the AI-VOX3 desktop companion.
 *
 * Layout (240x240):
 *   +-------------------------+
 *   |   [face expression]     |   (top ~ 55%)
 *   |   time                  |
 *   |   weather               |
 *   |   todo                  |
 *   |   status (bottom)       |
 *   +-------------------------+
 *
 * Expressions are rendered as large glyphs with a per-expression background
 * color. Real font/asset loading is left to the LVGL integration; the glyph
 * strings here are plain text and degrade gracefully if emoji fonts are absent.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include <lvgl/lvgl.h>

#include "ui_lvgl.h"
#include "config.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Expression glyph + background color (RGB565-ish 8-bit components). */
static const struct
{
  const char *glyph;
  lv_color_t  bg;
} g_expr_style[UI_EXPR_COUNT] =
{
  [UI_EXPR_IDLE]   = { ":-)",  LV_COLOR_MAKE(0x10, 0x20, 0x30) },
  [UI_EXPR_THINK]  = { ":-?",  LV_COLOR_MAKE(0x20, 0x20, 0x40) },
  [UI_EXPR_HAPPY]  = { ":-D",  LV_COLOR_MAKE(0x10, 0x40, 0x20) },
  [UI_EXPR_SPEAK]  = { ":-O",  LV_COLOR_MAKE(0x30, 0x30, 0x10) },
  [UI_EXPR_SLEEP]  = { "(-_-)",LV_COLOR_MAKE(0x08, 0x08, 0x10) },
};

static lv_obj_t *g_screen;
static lv_obj_t *g_face;
static lv_obj_t *g_time;
static lv_obj_t *g_weather;
static lv_obj_t *g_todo;
static lv_obj_t *g_status;
static bool      g_ui_ready = false;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ui_init
 ****************************************************************************/

int ui_init(void)
{
  /* The LVGL display + framebuffer are registered by the LVGL integration
   * (LCD driver + CONFIG_GRAPHICS_LVGL). Verify a display exists. */
  if (lv_disp_get_default() == NULL)
    {
      syslog(LOG_ERR, "ERROR: ui_init: no LVGL display registered\n");
      return -ENODEV;
    }

  lv_init();

  g_screen = lv_obj_create(NULL);
  if (g_screen == NULL)
    {
      return -ENOMEM;
    }

  lv_obj_set_size(g_screen, AIVOX3_LCD_WIDTH, AIVOX3_LCD_HEIGHT);
  lv_scr_load(g_screen);

  /* Face expression (top area). */
  g_face = lv_label_create(g_screen);
  lv_obj_set_width(g_face, AIVOX3_LCD_WIDTH);
  lv_label_set_long_mode(g_face, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_face, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_text_font(g_face, LV_FONT_DEFAULT, 0);
  lv_label_set_text(g_face, g_expr_style[UI_EXPR_IDLE].glyph);

  /* Time card. */
  g_time = lv_label_create(g_screen);
  lv_obj_set_width(g_time, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_time, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_time, LV_ALIGN_TOP_MID, 0, 90);
  lv_label_set_text(g_time, "Time: --:--:--");

  /* Weather card. */
  g_weather = lv_label_create(g_screen);
  lv_obj_set_width(g_weather, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_weather, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_weather, LV_ALIGN_TOP_MID, 0, 130);
  lv_label_set_text(g_weather, "Weather: --");

  /* Todo card. */
  g_todo = lv_label_create(g_screen);
  lv_obj_set_width(g_todo, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_todo, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_todo, LV_ALIGN_TOP_MID, 0, 165);
  lv_label_set_text(g_todo, "Todo: --");

  /* Status line (bottom). */
  g_status = lv_label_create(g_screen);
  lv_obj_set_width(g_status, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_status, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_label_set_text(g_status, "AI-VOX3 ready");

  g_ui_ready = true;
  ui_set_expression(UI_EXPR_IDLE);
  return OK;
}

/****************************************************************************
 * Name: ui_set_expression
 ****************************************************************************/

void ui_set_expression(ui_expression_t expr)
{
  if (!g_ui_ready || expr >= UI_EXPR_COUNT)
    {
      return;
    }

  lv_label_set_text(g_face, g_expr_style[expr].glyph);
  lv_obj_set_style_bg_color(g_screen, g_expr_style[expr].bg, 0);
}

/****************************************************************************
 * Name: ui_show_time
 ****************************************************************************/

void ui_show_time(const struct tm *tp)
{
  char buf[32];

  if (!g_ui_ready || tp == NULL)
    {
      return;
    }

  snprintf(buf, sizeof(buf), "Time: %02d:%02d:%02d",
           tp->tm_hour, tp->tm_min, tp->tm_sec);
  lv_label_set_text(g_time, buf);
}

/****************************************************************************
 * Name: ui_show_weather
 ****************************************************************************/

void ui_show_weather(const char *desc, int temp_c)
{
  char buf[48];

  if (!g_ui_ready)
    {
      return;
    }

  if (desc == NULL)
    {
      lv_label_set_text(g_weather, "Weather: --");
      return;
    }

  snprintf(buf, sizeof(buf), "Weather: %s %dC", desc, temp_c);
  lv_label_set_text(g_weather, buf);
}

/****************************************************************************
 * Name: ui_show_todo
 ****************************************************************************/

void ui_show_todo(const char *text)
{
  char buf[64];

  if (!g_ui_ready)
    {
      return;
    }

  snprintf(buf, sizeof(buf), "Todo: %s", text ? text : "--");
  lv_label_set_text(g_todo, buf);
}

/****************************************************************************
 * Name: ui_set_status
 ****************************************************************************/

void ui_set_status(const char *text)
{
  if (!g_ui_ready || text == NULL)
    {
      return;
    }

  lv_label_set_text(g_status, text);
}

/****************************************************************************
 * Name: ui_refresh
 ****************************************************************************/

void ui_refresh(void)
{
  if (g_ui_ready)
    {
      lv_timer_handler();
    }
}
