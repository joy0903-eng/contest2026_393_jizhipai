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

/* 子集中文点阵字体（由 toolchain/fontgen/fontgen.js 从 simhei.ttf 生成，16px，
 * 仅覆盖屏幕上出现的 UI 字符串）。在 ui_init 里挂到每个文本标签上，中文状态
 * 才能正常显示而不是方框。LV_FONT_DECLARE 展开为 extern lv_font_t ui_font_cn。 */
LV_FONT_DECLARE(ui_font_cn);

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
static lv_obj_t *g_date;
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
  lv_obj_set_style_text_font(lbl, &ui_font_cn, 0);
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
  static bool s_flush_logged = false;

  if (!s_flush_logged)
    {
      s_flush_logged = true;
      ets_printf("[FLUSH] enter live=%d cp=%p\n", g_flush_live,
                 (void *)color_p);
    }

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

  /* FIX (stutter + screen-freeze root cause): removed the per-flush
   *   lcd->setpower(lcd, 0xff)
   * that used to run here.  Re-asserting DISPON on EVERY LVGL flush (which
   * fires many times per second, each a partial band) generated needless SPI
   * traffic and triggered "DISPON re-asserted" / "panel rail is not up yet"
   * churn from the ST7789 driver, and could stall the flush (freezing the UI
   * -- the clock stops advancing).  The panel is powered on ONCE at init
   * (see ui_smoke_test / ai_vox3_lcd_initialize); if the system st7789 driver
   * ever auto-sleeps the panel, that is fixed in nuttx/lcd/st7789.c, not here.
   *
   * Staging buffer: allocate ONCE from the app heap and reuse it on every
   * flush -- a per-flush malloc()+free() churns the heap, and on allocation
   * failure the old code fell back to a slow row-by-row path; both could
   * stall the UI thread and starve the audio DMA, causing choppy speech.
   *
   * IMPORTANT: this board is a DUAL-HEAP build (V636).  malloc() from app
   * code lands in the PSRAM app heap (0x3c100000..0x3c900000, 8 MB), while
   * the kernel heap used by Wi-Fi/DMA is the ~200 KB of leftover internal
   * DRAM.  A function-scope `static` array would be placed in .bss, i.e. it
   * would eat the SAME internal DRAM that esp_wifi_init() needs -- measured:
   * two 9.6 KB static buffers shrank the kernel heap from 211412 to 193912
   * bytes and wlan_sta_initialize() started failing with -12 again.
   * So keep only the POINTERS static (4 bytes each in .bss) and let the
   * payload live in PSRAM. */
  static uint16_t *s_stage;   /* 240*20 RGB565 words, in PSRAM */
  static uint16_t *s_row;     /* 240     RGB565 words, in PSRAM */

  if (s_stage == NULL)
    {
      s_stage = (uint16_t *)malloc(240 * 20 * sizeof(uint16_t));
      s_row   = (uint16_t *)malloc(240 * sizeof(uint16_t));
      if (s_stage == NULL || s_row == NULL)
        {
          /* Out of memory: fall through to the row-by-row path below, which
           * needs no staging buffer at all. */
          free(s_stage); s_stage = NULL;
          free(s_row);   s_row   = NULL;
        }
    }

  lv_color_t *src = (lv_color_t *)color_p;
  const int   stage_bytes = (int)(240 * 20 * 2);

  if (s_stage != NULL && w * h * 2 <= stage_bytes)
    {
      uint16_t *buf = s_stage;

      for (int i = 0; i < w * h; i++)
        {
          buf[i] = lv_color_to_u16(src[i]);   /* -> RGB565 */
        }

      s_pinfo.putarea(lcd, area->y1, area->y2, area->x1, area->x2,
                      (uint8_t *)buf, (fb_coord_t)(w * 2));
    }
  else
    {
      /* Unexpectedly large area (not normal PARTIAL mode): row-by-row with a
       * per-row buffer -- no heap allocation, so it can never fail. */
      uint16_t *row = s_row;

      for (int y = 0; y < h; y++)
        {
          for (int x = 0; x < w; x++)
            {
              row[x] = lv_color_to_u16(src[y * w + x]);
            }

          s_pinfo.putarea(lcd, area->y1 + y, area->y1 + y, area->x1,
                          area->x2, (uint8_t *)row, (fb_coord_t)(w * 2));
        }
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

  /* FIX (root cause of the boot hang): OpenVela's lv_nuttx_init() registers the
   * LVGL display but never propagates the LCD panel geometry, so disp->hor_res /
   * disp->ver_res stay at 0.  lv_display_set_buffers() in PARTIAL render mode
   * computes stride = lv_draw_buf_width_to_stride(w, cf) and then h = buf_size /
   * stride.  With w == 0 the stride is also 0, so it divides by zero and the CPU
   * faults -> the call never returns and the panel stays blank.  (lv_init(),
   * called above, DOES install the draw-buffer handlers, so the only missing
   * piece is the resolution.)  Pin the real panel geometry (240x320, per LCD
   * getvideoinfo) and the colour format (RGB888, matching this build's 3-byte
   * lv_color_t) before setting the draw buffer. */
  lv_display_set_resolution(result.disp, AIVOX3_LCD_WIDTH, AIVOX3_LCD_HEIGHT);
  lv_display_set_color_format(result.disp, LV_COLOR_FORMAT_RGB888);
  ets_printf("[UI] disp res w=%d h=%d cf=%d\n",
             (int)lv_display_get_horizontal_resolution(result.disp),
             (int)lv_display_get_vertical_resolution(result.disp),
             (int)lv_display_get_color_format(result.disp));

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
   * from the kernel heap.  Give LVGL a small partial buffer (a few rows) and
   * switch to PARTIAL render mode; LVGL then flushes the screen band-by-band
   * over SPI.
   *
   * The buffer itself must NOT be a function-scope static array.  This is a
   * DUAL-HEAP build (V636): app malloc() lands in the 8 MB PSRAM app heap,
   * while the kernel heap that Wi-Fi/DMA needs is only ~200 KB of internal
   * DRAM.  A static array is placed in .bss, i.e. it consumes exactly that
   * scarce internal DRAM -- measured: static UI buffers shrank the kernel heap
   * from 211412 to 193912 B and wlan_sta_initialize() began failing with -12.
   * (The old "PSRAM is disabled on this board" note below-this was wrong: the
   * boot log prints `[V636] app heap (malloc): 0x3c100000..0x3c900000
   * size=8388608 (PSRAM)`.)
   * So allocate from the app heap (PSRAM) and keep only the pointer static. */
#define AIVOX3_LV_ROWS   20
#define AIVOX3_LV_PIXELS (AIVOX3_LCD_WIDTH * AIVOX3_LV_ROWS)
  static lv_color_t *g_lv_buf;      /* payload in PSRAM, not in .bss */
  const size_t       g_lv_bytes = AIVOX3_LV_PIXELS * sizeof(lv_color_t);

  if (g_lv_buf == NULL)
    {
      g_lv_buf = (lv_color_t *)malloc(g_lv_bytes);
      if (g_lv_buf == NULL)
        {
          ets_printf("[UI] FATAL: draw buffer malloc(%u) failed\n",
                     (unsigned)g_lv_bytes);
          return -ENOMEM;
        }
    }
  ets_printf("[UI] >> set_buffers buf=%p size=%u\n",
             (void *)g_lv_buf, (unsigned)g_lv_bytes);
  lv_display_set_buffers(result.disp, g_lv_buf, NULL,
                         g_lv_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  ets_printf("[UI] << set_buffers returned\n");

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
  lv_obj_align(g_time, LV_ALIGN_TOP_MID, 0, 80);
  lv_label_set_text(g_time, "时间: --:--:--");
  lv_obj_set_style_text_font(g_time, &ui_font_cn, 0);
  lv_obj_set_style_text_color(g_time, lv_color_white(), 0);

  /* Date card. */
  g_date = lv_label_create(g_screen);
  lv_obj_set_width(g_date, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_date, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_date, LV_ALIGN_TOP_MID, 0, 110);
  lv_label_set_text(g_date, "日期: --");
  lv_obj_set_style_text_font(g_date, &ui_font_cn, 0);
  lv_obj_set_style_text_color(g_date, lv_color_white(), 0);

  /* Weather card. */
  g_weather = lv_label_create(g_screen);
  lv_obj_set_width(g_weather, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_weather, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_weather, LV_ALIGN_TOP_MID, 0, 145);
  lv_label_set_text(g_weather, "天气: --");
  lv_obj_set_style_text_font(g_weather, &ui_font_cn, 0);
  lv_obj_set_style_text_color(g_weather, lv_color_white(), 0);

  /* Todo card. */
  g_todo = lv_label_create(g_screen);
  lv_obj_set_width(g_todo, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_todo, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_todo, LV_ALIGN_TOP_MID, 0, 180);
  lv_label_set_text(g_todo, "待办: --");
  lv_obj_set_style_text_font(g_todo, &ui_font_cn, 0);
  lv_obj_set_style_text_color(g_todo, lv_color_white(), 0);

  /* Status line (bottom). */
  g_status = lv_label_create(g_screen);
  lv_obj_set_width(g_status, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_status, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_label_set_text(g_status, "AI-VOX3 已就绪");
  lv_obj_set_style_text_font(g_status, &ui_font_cn, 0);
  lv_obj_set_style_text_color(g_status, lv_color_white(), 0);

  /* --- Reading (audiobook) controls --- */
  g_read_info = lv_label_create(g_screen);
  lv_obj_set_width(g_read_info, AIVOX3_LCD_WIDTH - 16);
  lv_label_set_long_mode(g_read_info, LV_LABEL_LONG_WRAP);
  lv_obj_align(g_read_info, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_label_set_text(g_read_info, "");
  lv_obj_set_style_text_font(g_read_info, &ui_font_cn, 0);
  lv_obj_set_style_text_color(g_read_info, lv_color_white(), 0);
  lv_obj_add_flag(g_read_info, LV_OBJ_FLAG_HIDDEN);

  g_read_start = make_read_btn("朗读", UI_BTN_READ_START);
  lv_obj_align(g_read_start, LV_ALIGN_TOP_MID, 0, 60);

  g_read_pause = make_read_btn("暂停", UI_BTN_READ_PAUSE);
  lv_obj_align(g_read_pause, LV_ALIGN_BOTTOM_LEFT, 4, -6);
  g_read_resume = make_read_btn("继续", UI_BTN_READ_RESUME);
  lv_obj_align(g_read_resume, LV_ALIGN_BOTTOM_LEFT, 50, -6);
  g_read_next = make_read_btn("下一章", UI_BTN_READ_NEXT);
  lv_obj_align(g_read_next, LV_ALIGN_BOTTOM_MID, 0, -6);
  g_read_stop = make_read_btn("停止", UI_BTN_READ_STOP);
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

  /* ---- HARD DIAGNOSTIC + immediate paint ---------------------------------
   * Force one synchronous LVGL render right here, BEFORE returning to main().
   * This partitions the "why is the screen red, not blue" problem:
   *   - If "[FLUSH]" appears below -> the LVGL flush / display plumbing is
   *     correct.  The blue UI will now also visibly paint over the red, and
   *     any remaining fault is the main loop never reaching ui_refresh()
   *     (some setup call after ui_init -- audio_init / rearm_mic / etc --
   *     blocks indefinitely before the while(1) loop starts).
   *   - If "[FLUSH]" does NOT appear -> the display config itself is broken
   *     (no default display / no refresh timer / colour-format mismatch).
   * ---------------------------------------------------------------------- */
  ets_printf("[UI] forcing first lv_timer_handler() (g_flush_live=%d)\n",
             g_flush_live);
  lv_tick_inc((uint32_t)AIVOX3_LOOP_TICK_MS);
  lv_timer_handler();
  ets_printf("[UI] first lv_timer_handler() returned\n");

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

  snprintf(buf, sizeof(buf), "时间: %02d:%02d:%02d",
           tp->tm_hour, tp->tm_min, tp->tm_sec);
  lv_label_set_text(g_time, buf);
}

/****************************************************************************
 * Name: ui_show_date
 ****************************************************************************/

void ui_show_date(const struct tm *tp)
{
  char buf[32];

  if (!g_ui_ready || tp == NULL)
    {
      return;
    }

  snprintf(buf, sizeof(buf), "日期: %04d-%02d-%02d",
           tp->tm_year + 1900, tp->tm_mon + 1, tp->tm_mday);
  lv_label_set_text(g_date, buf);
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
      lv_label_set_text(g_weather, "天气: --");
      return;
    }

  snprintf(buf, sizeof(buf), "天气: %d℃", temp_c);
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

  snprintf(buf, sizeof(buf), "待办: %s", text ? text : "--");
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

  snprintf(buf, sizeof(buf), "第%d章 行 %d/%d %d%%", chapter, line, total, pct);
  lv_label_set_text(g_read_info, buf);
}
