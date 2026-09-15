/****************************************************************************
 * apps/examples/desktop_companion/ui_lvgl.h
 *
 * LVGL-based UI for the AI-VOX3 desktop companion.
 *
 * Provides 5 facial expressions (idle / think / happy / speak / sleep) and
 * desktop info cards (time / weather / todo) plus a status line.
 *
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKTOP_COMPANION_UI_LVGL_H
#define __APPS_EXAMPLES_DESKTOP_COMPANION_UI_LVGL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef enum
{
  UI_EXPR_IDLE = 0,
  UI_EXPR_THINK,
  UI_EXPR_HAPPY,
  UI_EXPR_SPEAK,
  UI_EXPR_SLEEP,
  UI_EXPR_COUNT
} ui_expression_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Initialize the LVGL display and build the default screen. Returns OK. */
int ui_init(void);

/* Switch the facial expression. */
void ui_set_expression(ui_expression_t expr);

/* Update the on-screen time card from a broken-down time. */
void ui_show_time(const struct tm *tp);

/* Update the weather card. desc may be NULL (shows "—"). */
void ui_show_weather(const char *desc, int temp_c);

/* Update the todo card text. */
void ui_show_todo(const char *text);

/* Update the bottom status line. */
void ui_set_status(const char *text);

/* Drive the LVGL task handler (call periodically from the main loop). */
void ui_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_EXAMPLES_DESKTOP_COMPANION_UI_LVGL_H */
