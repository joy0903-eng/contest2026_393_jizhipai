/****************************************************************************
 * apps/examples/desktop_companion/main.c
 *
 * AI-VOX3 "desktop companion" main entry point and state machine.
 *
 * State flow:  IDLE -> WAKE -> TALK -> SPEAK -> IDLE
 *              (BOOT button toggles SLEEP)
 *
 * Per plan §7.2 (no ASR/TTS): input is taken from the button (wake) + a
 * serial text line (the "user message"); the LLM reply is shown on the LCD
 * and echoed to the serial console. Microphone capture is started but not
 * recognized (asr_transcribe() is a stub).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>

#include <arch/board/board.h>

#include "config.h"
#include "ui_lvgl.h"
#include "net_services.h"
#include "llm_client.h"
#include "audio_pipeline.h"
#include "face_follow.h"
#include "smart_home.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static aivox3_state_t g_state = AIVOX3_STATE_IDLE;
static bool g_wake_req  = false;
static bool g_sleep_req = false;

/* Periodic refresh / weather cadence. */
#define IDLE_REFRESH_MS      50
#define WEATHER_INTERVAL_LOOP 200   /* ~10 s at 50 ms loop */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: btn_callback
 *
 * Description:
 *   Button press handler (registered with the BSP buttons driver).
 *
 ****************************************************************************/

static void btn_callback(int btn_id)
{
  if (btn_id == AI_VOX3_BTN_A)
    {
      g_wake_req = true;
    }
  else if (btn_id == AI_VOX3_BTN_BOOT)
    {
      g_sleep_req = true;
    }
}

/****************************************************************************
 * Name: read_line_timeout
 *
 * Description:
 *   Non-blocking read of one line from stdin (USB-CDC console) with a timeout.
 *   Returns the number of characters read (excluding NUL), or 0 on timeout.
 *
 ****************************************************************************/

static int read_line_timeout(char *buf, size_t len, int timeout_ms)
{
  struct pollfd pfd;
  int n;
  int got = 0;
  char c;

  pfd.fd      = 0;   /* stdin */
  pfd.events  = POLLIN;

  n = poll(&pfd, 1, timeout_ms);
  if (n <= 0)
    {
      return 0;
    }

  while (got + 1 < (int)len)
    {
      n = (int)read(0, &c, 1);
      if (n <= 0)
        {
          break;
        }

      if (c == '\n' || c == '\r')
        {
          break;
        }

      if (c == '\b' && got > 0)
        {
          got--;          /* backspace */
          continue;
        }

      buf[got++] = c;
    }

  buf[got] = '\0';
  return got;
}

/****************************************************************************
 * Name: show_clock
 ****************************************************************************/

static void show_clock(void)
{
  time_t now = time(NULL);
  struct tm t;

  if (localtime_r(&now, &t) != NULL)
    {
      ui_show_time(&t);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main (desktop_companion)
 ****************************************************************************/

int main(int argc, char *argv[])
{
  char line[256];
  char reply[1024];
  char weather_desc[32];
  char todo[64];
  int temp = 0;
  int loop = 0;
  int ret;

  (void)argc;
  (void)argv;

  syslog(LOG_INFO, "AI-VOX3 desktop_companion starting\n");

  /* --- Bring up subsystems --- */
  ui_init();
  net_init();
  audio_init();
  face_follow_enable(true);
  ai_vox3_buttons_register_callback(btn_callback);

  /* Try to bring up the network + time if possible (best effort). */
  if (net_wait_linked(2000))
    {
      net_ntp_sync();
    }

  ui_set_status("AI-VOX3 ready — press A to talk");

  /* --- Main loop --- */
  while (1)
    {
      /* BOOT button toggles sleep. */
      if (g_sleep_req)
        {
          g_sleep_req = false;
          if (g_state == AIVOX3_STATE_SLEEP)
            {
              g_state = AIVOX3_STATE_IDLE;
              ui_set_expression(UI_EXPR_IDLE);
              ui_set_status("Awake");
            }
          else
            {
              g_state = AIVOX3_STATE_SLEEP;
              ui_set_expression(UI_EXPR_SLEEP);
              ui_set_status("Sleeping (BOOT to wake)");
            }
        }

      switch (g_state)
        {
          case AIVOX3_STATE_IDLE:
            {
              show_clock();

              /* Periodic weather refresh. */
              if (loop % WEATHER_INTERVAL_LOOP == 0 && net_is_connected())
                {
                  if (net_get_weather(weather_desc, sizeof(weather_desc),
                                      &temp) == OK)
                    {
                      ui_show_weather(weather_desc, temp);
                    }
                }

              /* Show first todo. */
              if (loop % WEATHER_INTERVAL_LOOP == 0)
                {
                  if (todo_load_first(todo, sizeof(todo)) == OK)
                    {
                      ui_show_todo(todo);
                    }
                  else
                    {
                      ui_show_todo("--");
                    }
                }

              /* Button A (or serial "wake") starts a conversation. */
              if (g_wake_req)
                {
                  g_wake_req = false;
                  g_state = AIVOX3_STATE_WAKE;
                  ui_set_expression(UI_EXPR_THINK);
                  ui_set_status("Listening... type a message");
                  audio_play_tone(1);
                }
              break;
            }

          case AIVOX3_STATE_WAKE:
            {
              /* Gather the user's text. No ASR yet -> read a serial line. */
              printf("You> ");
              fflush(stdout);

              ret = read_line_timeout(line, sizeof(line), 10000);
              if (ret <= 0)
                {
                  strncpy(line, "你好，今天过得怎么样？", sizeof(line) - 1);
                  line[sizeof(line) - 1] = '\0';
                }

              g_state = AIVOX3_STATE_TALK;
              break;
            }

          case AIVOX3_STATE_TALK:
            {
              /* (Optional) start mic capture for future ASR; not recognized. */
              audio_capture_start();

              syslog(LOG_INFO, "LLM request: %s\n", line);
              ui_set_status("Thinking...");

              ret = llm_chat(line, reply, sizeof(reply));
              audio_capture_stop();

              if (ret == OK && reply[0] != '\0')
                {
                  printf("AI> %s\n", reply);
                  fflush(stdout);
                  ui_set_expression(UI_EXPR_HAPPY);
                  ui_set_expression(UI_EXPR_SPEAK);
                  face_follow_trigger_speak();
                  audio_play_tone(2);
                  ui_set_status(reply);
                }
              else
                {
                  ui_set_expression(UI_EXPR_IDLE);
                  ui_set_status(ret == -ENOKEY ? "No API key" : "LLM error");
                }

              g_state = AIVOX3_STATE_SPEAK;
              break;
            }

          case AIVOX3_STATE_SPEAK:
            {
              /* Hold the reply briefly, then return to standby. */
              usleep(2000000);
              ui_set_expression(UI_EXPR_IDLE);
              ui_set_status("AI-VOX3 ready — press A to talk");
              g_state = AIVOX3_STATE_IDLE;
              break;
            }

          case AIVOX3_STATE_SLEEP:
          default:
            {
              /* In sleep we only wait for the BOOT wake (handled above). */
              break;
            }
        }

      /* Drive face-follow demo + LVGL. */
      face_follow_demo_tick();
      ui_refresh();

      usleep(IDLE_REFRESH_MS * 1000);
      loop++;
    }

  return 0;
}
