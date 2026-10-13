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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <lvgl/lvgl.h>

#include <nuttx/lcd/lcd.h>
#include <nuttx/video/fb.h>

extern int ets_printf(const char *fmt, ...);
extern struct lcd_dev_s *ai_vox3_lcd_getdev(int);

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
  [UI_EXPR_IDLE]   = { ":-)",  LV_COLOR_MAKE(0x1b, 0x3e, 0x74) },
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
static lv_obj_t *g_read_start;   /* "READ" button (visible in IDLE)    */
static lv_obj_t *g_read_pause;   /* reading controls (visible in READ)  */
static lv_obj_t *g_read_resume;
static lv_obj_t *g_read_next;
static lv_obj_t *g_read_stop;
static lv_obj_t *g_read_info;    /* chapter / line / progress readout   */
static ui_btn_cb_t g_btn_cb = NULL;
static bool      g_ui_ready = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: read_btn_event
 *
 * Description:
 *   LVGL event callback for the reading buttons. The button id is passed as the
 *   event's user data (the LVGL v8-idiomatic way), set when the callback was
 *   registered in make_read_btn().
 ****************************************************************************/

static void read_btn_event(lv_event_t *e)
{
  if (g_btn_cb == NULL)
    {
      return;
    }

  int id = (int)(intptr_t)lv_event_get_user_data(e);

  g_btn_cb(id);
}

/****************************************************************************
 * Name: make_read_btn
 *
 * Description:
 *   Create one reading button (hidden by default) and wire its click handler.
 ****************************************************************************/

static lv_obj_t *make_read_btn(const char *label, int id)
{
  lv_obj_t *btn = lv_btn_create(g_screen);
  lv_obj_t *lbl = lv_label_create(btn);

  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_add_event_cb(btn, read_btn_event, LV_EVENT_CLICKED,
                      (void *)(intptr_t)id);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
  return btn;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/


/****************************************************************************
 * Name: ui_smoke_test
 *
 * Description:
 *   Diagnostic only.  Paint the whole panel RED via the raw LCD driver
 *   (bypassing LVGL) to prove the SPI/ST7789 path works, and print the video
 *   info + putarea result.  After this, the main loop's LVGL refresh (now that
 *   the tick advances in ui_refresh) should paint BLUE over it.  The final
 *   visible colour thus discriminates:
 *     BLUE  -> hardware + tick both OK
 *     RED   -> hardware OK, but LVGL still not flushing (tick still broken)
 *     WHITE -> even the raw putarea failed (panel asleep / SPI / wiring)
 ****************************************************************************/

/* ---------------------------------------------------------------------------
 * Name: my_lcd_flush
 *
 * Custom LVGL display flush for the AI-VOX3 ST7789 panel.  We deliberately
 * BYPASS the OpenVela LVGL/NuttX LCD port's built-in flush callback (which
 * leaves the panel blank on this board -- the port either does not convert
 * LVGL's internal colour format to the panel's RGB565, or does not call
 * lv_display_flush_ready() under PARTIAL render mode, stalling the render
 * pipeline after the first frame).
 *
 * Our flush reads the LVGL draw buffer (whatever its internal colour depth is)
 * and converts every pixel to RGB565 with lv_color_to16() -- this is correct
 * for ANY LVGL colour depth -- then writes it to the panel via the raw LCD
 * driver's putarea().  lv_display_flush_ready() is always called so LVGL keeps
 * rendering.
 * ------------------------------------------------------------------------- */
/* Gate: while false the callback is a no-op (just releases LVGL's flush
 * semaphore) so the port's mandatory initial flush during
 * lv_display_set_buffers() cannot dead-lock the boot.  ui_init() flips it
 * true once the LCD and LVGL screen are fully set up, after which the real
 * paint path (pinfo.putarea) runs from the main loop's ui_refresh(). */
static bool g_flush_live = false;

static void my_lcd_flush(lv_display_t *disp, const lv_area_t *area,
                         uint8_t *color_p)
{
  if (!g_flush_live)
    {
      lv_display_flush_ready(disp);
      return;
    }

  static bool s_first = true;
  static struct lcd_planeinfo_s s_pinfo;
  static bool s_pinfo_ready = false;
  struct lcd_dev_s *lcd = ai_vox3_lcd_getdev(0);

  if (lcd == NULL)
    {
      lv_display_flush_ready(disp);
      return;
    }

  /* Guard against being invoked before the first render (the OpenVela port may
   * call the flush callback during display setup with a NULL draw buffer).  A
   * NULL deref here would hard-fault the boot, so bail out cleanly. */
  if (color_p == NULL)
    {
      lv_display_flush_ready(disp);
      return;
    }

  /* putarea() lives on the LCD plane info, NOT on lcd_dev_s.  Fetch it once. */
  if (!s_pinfo_ready)
    {
      if (lcd->getplaneinfo(lcd, 0, &s_pinfo) != OK)
        {
          lv_display_flush_ready(disp);
          return;
        }

      s_pinfo_ready = true;
    }

  int w = area->x2 - area->x1 + 1;
  int h = area->y2 - area->y1 + 1;
  if (w <= 0 || h <= 0)
    {
      lv_display_flush_ready(disp);
      return;
    }

  lcd->setpower(lcd, 0xff);   /* ensure the panel stays on */

  /* One contiguous RGB565 buffer for the whole area (PARTIAL mode keeps h
   * small -- a handful of rows -- so this is a few KB).  If it cannot be
   * allocated (e.g. a surprise full-frame flush), fall back to row-by-row
   * with a tiny per-row buffer.  lv_color_to_u16() converts from whatever
   * LVGL's internal colour depth is to RGB565 -- correct for this panel. */
  lv_color_t *src = (lv_color_t *)color_p;
  uint16_t *buf = (uint16_t *)malloc((size_t)w * (size_t)h * 2);

  if (buf != NULL)
    {
      for (int i = 0; i < w * h; i++)
        {
          buf[i] = lv_color_to_u16(src[i]);   /* -> RGB565 */
        }

      s_pinfo.putarea(lcd, area->y1, area->y2, area->x1, area->x2,
                      (uint8_t *)buf, (fb_coord_t)(w * 2));
      free(buf);
    }
  else
    {
      uint16_t *row = (uint16_t *)malloc((size_t)w * 2);
      if (row == NULL)
        {
          lv_display_flush_ready(disp);
          return;
        }

      for (int y = 0; y < h; y++)
        {
          for (int x = 0; x < w; x++)
            {
              row[x] = lv_color_to_u16(src[y * w + x]);
            }

          s_pinfo.putarea(lcd, area->y1 + y, area->y1 + y, area->x1,
                          area->x2, (uint8_t *)row, (fb_coord_t)(w * 2));
        }

      free(row);
    }

  if (s_first)
    {
      s_first = false;
      ets_printf("[FLUSH] custom flush active; first area %dx%d at (%d,%d)\n",
                 w, h, area->x1, area->y1);
    }

  lv_display_flush_ready(disp);
}

/* ---------------------------------------------------------------------------
 * Name: ui_smoke_test
 *
 * Diagnostic only.  Paint the WHOLE panel RED via the raw LCD driver
 * (bypassing LVGL) so that after the first LVGL refresh the colour key is
 * binary and unambiguous:
 *     BLUE  -> LVGL custom flush works (hardware + format + flush_ready)
 *     RED   -> flush still broken
 *     WHITE -> even the raw putarea failed (panel asleep / SPI / wiring)
 * ------------------------------------------------------------------------- */
static void ui_smoke_test(void)
{
  struct lcd_dev_s      *lcd = ai_vox3_lcd_getdev(0);
  struct fb_videoinfo_s  vinfo;
  struct lcd_planeinfo_s pinfo;
  int ret;

  if (lcd == NULL)
    {
      ets_printf("[SMOKE] lcd dev NULL\n");
      return;
    }

  ret = lcd->getvideoinfo(lcd, &vinfo);
  ets_printf("[SMOKE] getvideoinfo ret=%d xres=%d yres=%d\n",
             ret, (int)vinfo.xres, (int)vinfo.yres);

  ret = lcd->setpower(lcd, 0xff);   /* LCD_FULL_ON */
  ets_printf("[SMOKE] setpower(0xff) ret=%d\n", ret);

  ret = lcd->getplaneinfo(lcd, 0, &pinfo);
  ets_printf("[SMOKE] getplaneinfo ret=%d bpp=%d\n", ret, (int)pinfo.bpp);
  if (ret != OK)
    {
      ets_printf("[SMOKE] getplaneinfo failed; cannot paint\n");
      return;
    }

  int w = (int)(vinfo.xres ? vinfo.xres : AIVOX3_LCD_WIDTH);
  int h = (int)(vinfo.yres ? vinfo.yres : AIVOX3_LCD_HEIGHT);
  int band = 20;            /* rows per putarea; small enough for internal RAM */
  uint16_t *buf = (uint16_t *)malloc((size_t)w * band * 2);

  if (buf == NULL)
    {
      ets_printf("[SMOKE] malloc(%d) failed\n", (int)(w * band * 2));
      return;
    }

  for (int i = 0; i < w * band; i++)
    {
      buf[i] = 0xF800;   /* RGB565 solid red */
    }

  for (int y = 0; y < h; y += band)
    {
      int y2 = y + band - 1;
      if (y2 >= h)
        {
          y2 = h - 1;
        }

      ret = pinfo.putarea(lcd, y, y2, 0, w - 1,
                          (uint8_t *)buf, (fb_coord_t)(w * 2));
    }

  free(buf);
  ets_printf("[SMOKE] whole panel painted RED (raw LCD). "
             "LVGL custom flush should overwrite it with BLUE.\n");
}

/****************************************************************************
 * Name: ui_init
 ****************************************************************************/

int ui_init(void)
{
  ets_printf("[UI] ui_init enter\n");

  lv_init();

  /* Register the LVGL display from the AI-VOX3 ST7789 panel (/dev/lcd0).
   * OpenVela LVGL v9 ships the NuttX integration (lv_nuttx_init) which opens
   * the LCD/framebuffer device and registers an LVGL display.  Nothing else
   * in the BSP does this, so without it lv_disp_get_default() stays NULL and
   * the screen stays black. */
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/lcd0";
  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      ets_printf("[UI] WARN lv_nuttx_init returned NULL disp\n");
      syslog(LOG_ERR, "ERROR: ui_init: lv_nuttx_init failed to register display\n");
      return -ENODEV;
    }

  ets_printf("[UI] disp registered via lv_nuttx_init\n");

  /* Install OUR custom flush callback BEFORE setting the draw buffer.  The
   * OpenVela LVGL port performs a mandatory initial flush while
   * lv_display_set_buffers() runs, and its built-in flush callback is broken on
   * this board (it leaves the panel blank and, during early setup, stalls the
   * render pipeline).  By overriding the callback first we guarantee that
   * initial flush -- and every later one -- goes through my_lcd_flush(), which
   * converts LVGL's internal colour format to the panel's RGB565 and drives
   * putarea() directly, always calling lv_display_flush_ready().  The callback
   * is hardened against being invoked before the first render (NULL colour
   * pointer) so it can never fault the boot. */
  lv_display_set_flush_cb(result.disp, my_lcd_flush);
  ets_printf("[UI] custom flush callback installed\n");

  /* FIX: the NuttX LVGL port allocates its display draw buffer via clib malloc
   * from the kernel heap.  On this board PSRAM is disabled, so the heap is
   * internal SRAM only and a full-frame (240*320*2 ~= 150 KB) buffer malloc
   * fails -> LVGL has no canvas to render into -> the panel stays white even
   * though the display registers and the tick advances.  Give LVGL a small
   * partial buffer (a few rows) that comfortably fits internal RAM and switch
   * to PARTIAL render mode; LVGL then flushes the screen band-by-band over
   * SPI.  (A later cleanup can enable PSRAM for a full-frame buffer.) */
  static lv_color_t g_lv_buf[240 * 20];   /* 240 px wide x 20 rows = 9600 B */
  lv_display_set_buffers(result.disp, g_lv_buf, NULL,
                         sizeof(g_lv_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  ets_printf("[UI] lv draw buffer set (partial, %u bytes)\n",
             (unsigned)sizeof(g_lv_buf));

  g_screen = lv_obj_create(NULL);
  if (g_screen == NULL)
    {
      return -ENOMEM;
    }

  /* The panel is 240x320 (getvideoinfo), even though the original UI layout
   * assumed 240x240.  Size the root screen to the full panel so LVGL paints
   * every row; otherwise rows 240..319 stay at the smoke-test colour. */
  lv_obj_set_size(g_screen, AIVOX3_LCD_WIDTH, 320);
  lv_obj_set_style_bg_color(g_screen, lv_color_make(0x1b, 0x3e, 0x74), 0);
  lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
  lv_scr_load(g_screen);

  /* Face expression (top area). */
  g_face = lv_label_create(g_screen);
  lv_obj_set_width(g_face, AIVOX3_LCD_WIDTH);
  lv_label_set_long_mode(g_face, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_face, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_text_font(g_face, LV_FONT_DEFAULT, 0);
  lv_label_set_text(g_face, g_expr_style[UI_EXPR_IDLE].glyph);
  lv_obj_set_style_text_color(g_face, lv_color_white(), 0);

  /* Time card. */
  g_time = lv_label_create(g_screen);
  lv_obj_set_width(g_time, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_time, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_time, LV_ALIGN_TOP_MID, 0, 90);
  lv_label_set_text(g_time, "Time: --:--:--");
  lv_obj_set_style_text_color(g_time, lv_color_white(), 0);

  /* Weather card. */
  g_weather = lv_label_create(g_screen);
  lv_obj_set_width(g_weather, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_weather, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_weather, LV_ALIGN_TOP_MID, 0, 130);
  lv_label_set_text(g_weather, "Weather: --");
  lv_obj_set_style_text_color(g_weather, lv_color_white(), 0);

  /* Todo card. */
  g_todo = lv_label_create(g_screen);
  lv_obj_set_width(g_todo, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_todo, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_todo, LV_ALIGN_TOP_MID, 0, 165);
  lv_label_set_text(g_todo, "Todo: --");
  lv_obj_set_style_text_color(g_todo, lv_color_white(), 0);

  /* Status line (bottom). */
  g_status = lv_label_create(g_screen);
  lv_obj_set_width(g_status, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_status, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_label_set_text(g_status, "AI-VOX3 ready");
  lv_obj_set_style_text_color(g_status, lv_color_white(), 0);

  /* --- Reading (audiobook) controls --- */
  g_read_info = lv_label_create(g_screen);
  lv_obj_set_width(g_read_info, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_read_info, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_read_info, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_label_set_text(g_read_info, "");
  lv_obj_set_style_text_color(g_read_info, lv_color_white(), 0);
  lv_obj_add_flag(g_read_info, LV_OBJ_FLAG_HIDDEN);

  g_read_start = make_read_btn("READ", UI_BTN_READ_START);
  lv_obj_align(g_read_start, LV_ALIGN_TOP_MID, 0, 60);

  g_read_pause = make_read_btn("II", UI_BTN_READ_PAUSE);
  lv_obj_align(g_read_pause, LV_ALIGN_BOTTOM_LEFT, 4, -6);
  g_read_resume = make_read_btn(">", UI_BTN_READ_RESUME);
  lv_obj_align(g_read_resume, LV_ALIGN_BOTTOM_LEFT, 50, -6);
  g_read_next = make_read_btn(">>", UI_BTN_READ_NEXT);
  lv_obj_align(g_read_next, LV_ALIGN_BOTTOM_MID, 0, -6);
  g_read_stop = make_read_btn("X", UI_BTN_READ_STOP);
  lv_obj_align(g_read_stop, LV_ALIGN_BOTTOM_RIGHT, -4, -6);

  /* IDLE shows only the start button; reading shows the controls. */
  ui_set_reading_mode(false);

  /* Diagnostic: prove the raw SPI/ST7789 path paints, then let the normal
   * LVGL refresh (now tick-driven) overwrite it with the UI.  See the
   * ui_smoke_test() comment for the colour key. */
  ui_smoke_test();

  g_ui_ready = true;
  ui_set_expression(UI_EXPR_IDLE);
  ets_printf("[UI] ui_init done OK\n");
  g_flush_live = true;   /* open the real paint path for the main loop */
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
      /* LVGL has NO built-in tick source unless CONFIG_LV_USE_NUTTX_TICK is
       * set (it is NOT in this defconfig).  Without advancing the tick,
       * lv_timer_handler() computes elapsed time = 0 and never fires the
       * display-refresh timer, so LVGL never flushes a single pixel and the
       * panel stays white.  Feed it the real loop period every iteration. */
      lv_tick_inc((uint32_t)AIVOX3_LOOP_TICK_MS);
      lv_timer_handler();
    }
}

/****************************************************************************
 * Name: ui_set_button_callback
 ****************************************************************************/

void ui_set_button_callback(ui_btn_cb_t cb)
{
  g_btn_cb = cb;
}

/****************************************************************************
 * Name: ui_set_reading_mode
 ****************************************************************************/

void ui_set_reading_mode(bool reading)
{
  if (!g_ui_ready)
    {
      return;
    }

  if (reading)
    {
      lv_obj_add_flag(g_read_start, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_read_pause, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_read_resume, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_read_next, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_read_stop, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(g_read_info, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_clear_flag(g_read_start, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_read_pause, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_read_resume, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_read_next, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_read_stop, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_read_info, LV_OBJ_FLAG_HIDDEN);
    }
}

/****************************************************************************
 * Name: ui_show_reading
 ****************************************************************************/

void ui_show_reading(int chapter, int line, int total, int pct)
{
  char buf[48];

  if (!g_ui_ready)
    {
      return;
    }

  snprintf(buf, sizeof(buf), "Ch%d L%d/%d %d%%", chapter, line, total, pct);
  lv_label_set_text(g_read_info, buf);
}
