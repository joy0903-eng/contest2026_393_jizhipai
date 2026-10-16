/****************************************************************************
 * apps/examples/desktop_companion/main.c
 *
 * AI-VOX3 "desktop companion" main entry point and state machine.
 *
 * THIS IS A VOICE APPLICATION
 * ---------------------------
 * The microphone is open and running in IDLE, permanently. Every captured
 * frame is pushed through the offline VAD inside the audio pipeline, so the
 * board is listening all the time. A finished utterance is uploaded to a PC
 * running toolchain/voice_bridge/bridge_chat.py, which returns the recognised
 * text plus synthesised speech; the answer is played back over the speaker.
 *
 *   IDLE   mic open, VAD listening, standby carousel
 *     |  -- VAD says "utterance finished" (optionally gated by local KWS)
 *     |  -- or button A / `wake` on the console
 *     v
 *   WAKE   acknowledgement tone, mic re-armed cleanly
 *     v
 *   LISTEN wait for the utterance to arrive
 *     v
 *   TALK   mic closed -> POST PCM to the bridge -> transcript + reply + TTS
 *     v
 *   SPEAK  play the TTS PCM, show the reply -> back to IDLE
 *
 * (BOOT button toggles SLEEP, where the microphone is deliberately stopped.)
 *
 * Everything except SPEAK runs on the 50 ms main-loop tick, so the UI keeps
 * ticking and button presses stay responsive during network I/O.
 *
 * SERIAL CONSOLE (USB-CDC)
 *   bridge [<host> [<port>]]   query / override the bridge address
 *   wake                       same as button A
 *   clean                      clear the in-RAM todo list
 *   <anything else>            treated as a typed utterance: answered with
 *                              speech exactly like a spoken one
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>

#include <arch/board/board.h>

#include "config.h"
#include "ui_lvgl.h"
#include "net_services.h"
#include "audio_pipeline.h"
#include "dsp_vad.h"
#include "kws_engine.h"
#include "voice_bridge.h"
#include "face_follow.h"
#include "smart_home.h"
#include "novel_reader.h"

extern int ets_printf(const char *fmt, ...);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Cadence of the standby carousel refresh. */

#define WEATHER_INTERVAL_LOOP   200   /* 200 * 50 ms ~= 10 s */

/* Longest FIXED recording used when dsp_vad is unavailable (see
 * record_fixed_window()). 2.5 s at 16 kHz.
 */

#define FIXED_REC_MS            2500

/* Longest status line pushed to LVGL. The 240 px label cannot show much
 * more, and LV_FONT_DEFAULT has no CJK glyphs, so status text stays ASCII.
 */

#define UI_STATUS_CLIP          110

/* Console line accumulator size. */

#define SERIAL_LINE_MAX         256

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What the next TALK should send to the bridge. */

enum turn_source_e
{
  TURN_NONE = 0,
  TURN_PCM,        /* g_utt holds the utterance to upload                */
  TURN_TEXT        /* g_typed holds the text the user typed on the console */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static aivox3_state_t      g_state = AIVOX3_STATE_IDLE;
static bool                g_wake_req  = false;
static bool                g_sleep_req = false;

/* Utterance staging buffer. 44800 samples = 87.5 KB, which would blow a
 * NuttX thread stack instantly -- allocated once from the heap (PSRAM).
 */

static int16_t            *g_utt    = NULL;
static int                 g_utt_nsamples = 0;
static enum turn_source_e  g_turn_src = TURN_NONE;

/* Result of the last bridge turn (its PCM is heap owned). */

static bridge_turn_result_t g_result;

/* Console line accumulator + the text typed by the user. */

static char   g_serial_buf[SERIAL_LINE_MAX];
static size_t g_serial_len = 0;

static char   g_typed[SERIAL_LINE_MAX];
static bool   g_have_typed = false;

/* LISTEN state deadline, counted in main-loop ticks. */

static int    g_listen_ticks = 0;
static int    g_listen_limit = AIVOX3_LISTEN_TIMEOUT_MS / AIVOX3_LOOP_TICK_MS;

/* Diagnostic latches: report each degraded condition once, not 20x a
 * second, but re-report it if it later clears and comes back.
 */

static bool   g_net_warned  = false;
static bool   g_vad_warned  = false;
static bool   g_kws_warned  = false;
static bool   g_mic_warned  = false;

/* --- Audiobook / novel-reading mode -----------------------------------------
 * The reading feature REUSES the chat TTS playback path: it sends one text
 * segment to the bridge /tts endpoint and plays the returned PCM through
 * audio_play_pcm(). It never touches the chat loop's core logic; it only adds
 * two new top-level states (MODE_READ / READING) entered from IDLE/SLEEP.
 */
static bool     g_read_req     = false;   /* serial `read` or LVGL start  */
static bool     g_read_paused  = false;   /* pause/resume latch           */
static bool     g_read_stop    = false;   /* leave reading mode           */
static bool     g_read_next    = false;   /* skip to next segment         */
static bool     g_read_prev    = false;   /* back one segment             */
static char     g_read_file[128];         /* optional `read <file>` arg   */
static uint8_t *g_read_pcm     = NULL;    /* heap PCM for current segment */
static size_t   g_read_pcm_len = 0;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: btn_callback
 *
 * Description:
 *   Button press handler (registered with the BSP buttons driver).
 ****************************************************************************/

static void btn_callback(int btn_id)
{
  /* Both the A button and the BOOT button start a talk turn.  The user's
   * reflex is to press BOOT ("按住说话"), so it must NOT only toggle sleep.
   */
  if (btn_id == AI_VOX3_BTN_A || btn_id == AI_VOX3_BTN_BOOT)
    {
      g_wake_req = true;
    }
}

/****************************************************************************
 * Name: read_ui_button
 *
 * Description:
 *   LVGL reading-button callback (mirrors btn_callback for hardware buttons).
 *   Each press only latches a request flag; the main-loop state machine acts on
 *   it when it is safe to do so (never mid-chat).
 ****************************************************************************/

static void read_ui_button(int id)
{
  switch (id)
    {
      case UI_BTN_READ_START:
        g_read_req = true;
        break;

      case UI_BTN_READ_PAUSE:
        g_read_paused = true;
        break;

      case UI_BTN_READ_RESUME:
        g_read_paused = false;
        break;

      case UI_BTN_READ_NEXT:
        g_read_next = true;
        break;

      case UI_BTN_READ_STOP:
        g_read_stop = true;
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: ensure_utterance_buffer
 *
 * Description:
 *   Allocate the utterance staging buffer once. Must be done before any
 *   state tries to collect a word.
 *
 * Returned Value:
 *   true when g_utt is usable.
 ****************************************************************************/

static bool ensure_utterance_buffer(void)
{
  if (g_utt != NULL)
    {
      return true;
    }

  g_utt = (int16_t *)malloc((size_t)VAD_MAX_WORD_SAMPLES * sizeof(int16_t));
  if (g_utt == NULL)
    {
      syslog(LOG_ERR, "ERROR: cannot allocate the %lu-byte utterance "
             "buffer; voice input disabled\n",
             (unsigned long)((size_t)VAD_MAX_WORD_SAMPLES * sizeof(int16_t)));
      return false;
    }

  syslog(LOG_INFO, "voice: utterance buffer ok (%lu bytes)\n",
         (unsigned long)((size_t)VAD_MAX_WORD_SAMPLES * sizeof(int16_t)));
  return true;
}

/****************************************************************************
 * Name: set_status
 *
 * Description:
 *   ui_set_status() wrapper that also mirrors the line to the console and
 *   clips it to something the 240 px label can actually render.
 ****************************************************************************/

static void set_status(const char *text)
{
  static char last[UI_STATUS_CLIP + 8];
  char        clipped[UI_STATUS_CLIP + 8];

  if (text == NULL)
    {
      text = "";
    }

  strncpy(clipped, text, UI_STATUS_CLIP);
  clipped[UI_STATUS_CLIP] = '\0';

  if (strcmp(clipped, last) != 0)
    {
      strncpy(last, clipped, sizeof(last) - 1);
      last[sizeof(last) - 1] = '\0';
      ui_set_status(clipped);
      printf("[ui] %s\n", clipped);
      fflush(stdout);
    }
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
      ui_show_date(&t);
    }
}

/****************************************************************************
 * Name: serial_poll_line
 *
 * Description:
 *   NON-BLOCKING line reader for the USB-CDC console. Partial lines are
 *   accumulated across calls so the state machine is never blocked waiting
 *   for a human.
 *
 * Returned Value:
 *   true when a complete line was placed in `line`.
 ****************************************************************************/

static bool serial_poll_line(char *line, size_t cap)
{
  struct pollfd pfd;
  int n;

  pfd.fd      = 0;      /* stdin */
  pfd.events  = POLLIN;
  pfd.revents = 0;

  n = poll(&pfd, 1, 0);
  if (n <= 0)
    {
      return false;
    }

  while (g_serial_len + 1u < sizeof(g_serial_buf))
    {
      char    c;
      ssize_t r = read(0, &c, 1);

      if (r <= 0)
        {
          break;
        }

      if (c == '\n' || c == '\r')
        {
          if (g_serial_len == 0u)
            {
              continue;             /* swallow a bare line terminator */
            }

          memcpy(line, g_serial_buf, g_serial_len);
          line[g_serial_len] = '\0';
          g_serial_len       = 0u;
          return true;
        }

      if (c == '\b' || c == 0x7f)
        {
          if (g_serial_len > 0u)
            {
              g_serial_len--;
            }

          continue;
        }

      if (c < 0x20)
        {
          continue;                 /* control characters */
        }

      g_serial_buf[g_serial_len++] = c;
    }

  if (g_serial_len + 1u >= sizeof(g_serial_buf))
    {
      g_serial_len = 0u;            /* overflow: drop the junk line */
    }

  return false;
}

/****************************************************************************
 * Name: handle_serial_line
 *
 * Description:
 *   Dispatch one console line: an endpoint command, a wake word, or a typed
 *   utterance to be answered out loud.
 ****************************************************************************/

static void handle_serial_line(const char *line)
{
  if (strncmp(line, "bridge", 6) == 0 &&
      (line[6] == '\0' || line[6] == ' '))
    {
      const char *p = line + 6;
      char        host[BRIDGE_HOST_MAX];
      int         port = (int)bridge_port();

      while (*p == ' ')
        {
          p++;
        }

      if (*p == '\0')
        {
          printf("bridge = %s:%u\n", bridge_host(), (unsigned)bridge_port());
          fflush(stdout);
          return;
        }

      /* sscanf tolerates "host" and "host port" alike; port keeps the
       * previous value in the first form because it was pre-seeded.
       */
      host[0] = '\0';
      if (sscanf(p, "%63s %d", host, &port) >= 1 && host[0] != '\0')
        {
          bridge_set_endpoint(host,
                              (uint16_t)(port > 0 && port < 65536 ?
                                         port : 8765));
          printf("bridge -> %s:%u (RAM only, lost on reboot)\n",
                 bridge_host(), (unsigned)bridge_port());
        }
      else
        {
          printf("usage: bridge [<host> [<port>]]\n");
        }

      fflush(stdout);
      return;
    }

  if (strcmp(line, "wake") == 0)
    {
      g_wake_req = true;
      return;
    }

  if (strcmp(line, "clean") == 0)
    {
      todo_clear();
      set_status("Todo list cleared");
      return;
    }

  /* Audiobook (novel reading) controls. Accepted at any time; the state machine
   * only acts on them from a safe state (IDLE/SLEEP), so a chat in progress is
   * never disturbed. */
  if (strncmp(line, "read", 4) == 0 &&
      (line[4] == '\0' || line[4] == ' '))
    {
      const char *p = line + 4;
      while (*p == ' ')
        {
          p++;
        }

      strncpy(g_read_file, p, sizeof(g_read_file) - 1);
      g_read_file[sizeof(g_read_file) - 1] = '\0';
      g_read_req = true;
      printf("read: queued (start on next idle)\n");
      fflush(stdout);
      return;
    }

  if (strcmp(line, "pause") == 0)
    {
      g_read_paused = true;
      printf("read: paused\n");
      fflush(stdout);
      return;
    }

  if (strcmp(line, "resume") == 0)
    {
      g_read_paused = false;
      printf("read: resumed\n");
      fflush(stdout);
      return;
    }

  if (strcmp(line, "next") == 0)
    {
      g_read_next = true;
      return;
    }

  if (strcmp(line, "prev") == 0)
    {
      g_read_prev = true;
      return;
    }

  if (strcmp(line, "stop") == 0)
    {
      g_read_stop = true;
      return;
    }

  if (line[0] == '\0')
    {
      return;
    }

  /* Anything else is treated exactly like something the user said. */
  strncpy(g_typed, line, sizeof(g_typed) - 1);
  g_typed[sizeof(g_typed) - 1] = '\0';
  g_have_typed = true;
}

/****************************************************************************
 * Name: rearm_mic
 *
 * Description:
 *   Make sure the microphone is open and the VAD is armed from a clean
 *   state. Cheap and idempotent, so it can be called from any state.
 ****************************************************************************/

static void rearm_mic(void)
{
  if (audio_capture_start() == OK)
    {
      g_mic_warned = false;
      return;
    }

  if (!g_mic_warned)
    {
      g_mic_warned = true;
      syslog(LOG_ERR, "ERROR: cannot start microphone capture "
             "(voice trigger unavailable; button still works)\n");
    }
}

/****************************************************************************
 * Name: collect_utterance
 *
 * Description:
 *   Try to collect one finished utterance into g_utt.
 *
 * Returned Value:
 *   > 0 : sample count now valid in g_utt.
 *     0 : nothing finished yet.
 *    < 0 : negated errno (-EAGAIN = capture or VAD not available).
 ****************************************************************************/

static int collect_utterance(void)
{
  float score = 0.0f;
  int   ret;

  if (g_utt == NULL)
    {
      return -EINVAL;
    }

  ret = audio_capture_take_word(g_utt, VAD_MAX_WORD_SAMPLES);
  if (ret <= 0)
    {
      return ret;
    }

  /* Local keyword filter, when a template bank actually exists. With no
   * templates there is deliberately NO gate: the utterance goes to the PC,
   * where Vosk recognises arbitrary speech (and the bridge itself filters
   * out ambient noise). Waking WITHOUT the wake word is intentional here --
   * it is how the demo works on a freshly flashed board.
   */

  if (kws_is_ready())
    {
      if (!g_kws_warned)
        {
          syslog(LOG_INFO, "voice: local KWS active (threshold %.0f)\n",
                 (double)kws_get_threshold());
          g_kws_warned = true;
        }

      /* MFCC + DTW run HERE, on the consumer thread, never in the audio
       * callback (which would stall the apb pipeline and underrun RX).
       */
      if (kws_recognize(g_utt, ret, &score) < 0)
        {
          syslog(LOG_DEBUG, "kws: rejected (score %.0f)\n", (double)score);
          return 0;
        }

      syslog(LOG_INFO, "kws: accepted (score %.0f)\n", (double)score);
    }

  return ret;
}

/****************************************************************************
 * Name: record_fixed_window
 *
 * Description:
 *   Fallback recorder for when the VAD could not be initialised: drain the
 *   capture ring straight into g_utt for FIXED_REC_MS worth of audio, with
 *   no end-pointing. Used only by a button-triggered turn.
 *
 * Returned Value:
 *   Sample count in g_utt (> 0 on success), or a negated errno.
 ****************************************************************************/

static int record_fixed_window(void)
{
  size_t want = ((size_t)AIVOX3_AUDIO_RATE * FIXED_REC_MS / 1000) *
                sizeof(int16_t);
  size_t have = 0;
  size_t spins = 0;
  size_t max_spins = ((size_t)FIXED_REC_MS * 2u) / 50u + 40u;

  if (g_utt == NULL || want > (size_t)VAD_MAX_WORD_SAMPLES * sizeof(int16_t))
    {
      return -EINVAL;
    }

  while (have < want && spins < max_spins)
    {
      int n = audio_capture_read_timeout(
                ((uint8_t *)g_utt) + have, want - have, 20);

      if (n > 0)
        {
          have += (size_t)n;
        }
      else if (n < 0)
        {
          return n;
        }

      spins++;
    }

  return (int)(have / sizeof(int16_t));
}

/****************************************************************************
 * Name: do_bridge_turn
 *
 * Description:
 *   TALK state body: close the microphone and run one bridge turn.
 *
 * Returned Value:
 *   OK when something came back; negated errno otherwise.
 ****************************************************************************/

static int do_bridge_turn(void)
{
  int ret;

  if (!net_is_connected())
    {
      if (!g_net_warned)
        {
          g_net_warned = true;
          syslog(LOG_ERR, "ERROR: wlan0 has no IPv4 address; "
                 "check the Wi-Fi association and then retarget the bridge "
                 "with: bridge <pc-ip>\n");
        }

      return -ENETDOWN;
    }

  if (g_net_warned)
    {
      g_net_warned = false;
      syslog(LOG_INFO, "network: wlan0 is up again\n");
    }

  /* The codec is half duplex and the answer is about to be played back
   * through the speaker, so giving up the microphone here is not optional:
   * leaving capture up would let the VAD overhear its own answer and start
   * another turn immediately.
   */

  (void)audio_capture_stop();

  ui_set_expression(UI_EXPR_THINK);
  set_status("Thinking...");

  if (g_turn_src == TURN_TEXT)
    {
      ret = bridge_turn_text(g_typed, &g_result);
    }
  else
    {
      ret = bridge_turn_pcm(g_utt, g_utt_nsamples, &g_result);
    }

  if (ret < 0)
    {
      return ret;
    }

  if (g_result.reply[0] != '\0')
    {
      printf("You> %s\nAI>  %s\n",
             g_result.transcript[0] != '\0' ? g_result.transcript
                                            : g_typed,
             g_result.reply);
    }
  else
    {
      printf("You> %s\nAI>  (filtered: nothing to say)\n",
             g_result.transcript[0] != '\0' ? g_result.transcript
                                            : g_typed);
    }

  fflush(stdout);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main (desktop_companion)
 ****************************************************************************/

int main(int argc, char *argv[])
{
  char line[SERIAL_LINE_MAX];
  char weather_desc[32];
  char todo[64];
  int temp = 0;
  int loop = 0;
  int ret;

  (void)argc;
  (void)argv;

  ets_printf("[APP] desktop_companion_main entered\n");
  syslog(LOG_INFO, "AI-VOX3 desktop_companion starting (voice loop)\n");

  /* --- BSP bring-up -------------------------------------------------------
   * 2026-09-17: with CONFIG_INIT_ENTRYPOINT="desktop_companion_main" the
   * NSH startup chain never runs, so BOARDIOC_INIT ->
   * board_app_initialize() (which registers /dev/lcd0, audio, servo,
   * buttons via esp_board_initialize) would NEVER be called and ui_init()
   * would find no LCD device.  Run it here ourselves.  Note: if the init
   * entry were ever switched back to nsh_main, NSH would call this too --
   * double init is loud in syslog but non-fatal.
   */
  ret = board_app_initialize(0);
  ets_printf("[UI] board_app_initialize ret=%d\n", ret);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_app_initialize failed: %d\n", ret);
    }

  /* --- Bring up subsystems --- */
  /* 2026-09-17 real-device forensics: this call used to be a bare
   * `ui_init();` with the return value thrown away.  ui_lvgl.c:74 returns
   * -ENODEV when no LVGL display is registered, and every later
   * ui_set_status()/ui_set_expression()/ui_refresh() then hits
   * `if (!g_ui_ready) return;` and does NOTHING.  Net effect: a permanently
   * black screen with zero error output anywhere -- the user reported it as
   * "the board is dead".  Never let this fail silently again.
   */
  ets_printf("[UI] calling ui_init\n");
  ret = ui_init();
  ets_printf("[UI] ui_init ret=%d\n", ret);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ui_init failed: %d -- UI will be BLANK\n", ret);
    }

  /* Wire the LVGL reading buttons (start / pause / resume / next / stop) to the
   * same request-latch pattern used by the hardware buttons. */
  ui_set_button_callback(read_ui_button);
  ets_printf("[MAIN] ui button cb set\n");

  net_init();
  ets_printf("[MAIN] net_init done\n");
  audio_init();
  ets_printf("[MAIN] audio_init done\n");
  face_follow_enable(true);
  ets_printf("[MAIN] face_follow done\n");
  ai_vox3_buttons_register_callback(btn_callback);
  ets_printf("[MAIN] board btn cb done\n");

  /* Try to bring up the network + time if possible (best effort). */
  if (net_wait_linked(2000))
    {
      net_ntp_sync();
    }
  else
    {
      syslog(LOG_WARNING, "WARNING: no IPv4 on wlan0 after 2 s; the voice "
             "bridge will be unreachable until Wi-Fi comes up\n");
    }
  ets_printf("[MAIN] net link check done\n");

  /* Offline keyword spotting is a bonus, never a blocker: with no loaded
   * template bank every detected utterance simply goes to the PC.
   */
  kws_init();
  ret = kws_load(NULL);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "kws_load: %d (no wake-word templates; every "
             "utterance goes straight to the PC bridge)\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "kws: template bank loaded\n");
    }
  ets_printf("[MAIN] kws done\n");

  bridge_turn_result_init(&g_result);
  ensure_utterance_buffer();
  rearm_mic();
  ets_printf("[MAIN] rearm_mic done\n");

  ui_set_expression(UI_EXPR_IDLE);
  set_status("Standby -- talk to me");
  ets_printf("[MAIN] entering main loop\n");

  /* --- Main loop --- */
  while (1)
    {
      /* Console commands are serviced in every state (never blocking). */
      if (serial_poll_line(line, sizeof(line)))
        {
          handle_serial_line(line);
        }

      /* BOOT button toggles sleep. */
      if (g_sleep_req)
        {
          g_sleep_req = false;

          if (g_state == AIVOX3_STATE_SLEEP)
            {
              g_state = AIVOX3_STATE_IDLE;
              ui_set_expression(UI_EXPR_IDLE);
              rearm_mic();
              set_status("Standby -- talk to me");
            }
          else
            {
              g_state = AIVOX3_STATE_SLEEP;
              (void)audio_capture_stop();
              bridge_turn_result_release(&g_result);
              ui_set_expression(UI_EXPR_SLEEP);
              set_status("Sleeping (BOOT to wake)");
            }
        }

      /* Reading mode request (serial `read` or LVGL start button). Only honoured
       * from IDLE/SLEEP so an in-flight chat is never interrupted. */
      if (g_read_req &&
          (g_state == AIVOX3_STATE_IDLE ||
           g_state == AIVOX3_STATE_SLEEP))
        {
          g_read_req = false;
          g_state    = AIVOX3_STATE_MODE_READ;
        }

      switch (g_state)
        {
          case AIVOX3_STATE_IDLE:
            {
              show_clock();

              /* The microphone must be live in standby, otherwise nothing
               * can ever be heard. Cheap when already running.
               */
              rearm_mic();

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

              /* Path 1: the user typed something on the console. */
              if (g_have_typed)
                {
                  g_have_typed = false;
                  g_turn_src   = TURN_TEXT;
                  g_state      = AIVOX3_STATE_TALK;
                  break;
                }

              /* Path 2: button A or the `wake` command. */
              if (g_wake_req)
                {
                  g_wake_req     = false;
                  g_turn_src     = TURN_PCM;
                  g_utt_nsamples = 0;      /* must record a fresh utterance */
                  g_state        = AIVOX3_STATE_WAKE;
                  break;
                }

              /* Path 3: the VAD finished an utterance while we were
               * listening in standby. This is the real voice trigger.
               */
              ret = collect_utterance();
              if (ret > 0)
                {
                  g_utt_nsamples = ret;
                  g_turn_src     = TURN_PCM;
                  g_state        = AIVOX3_STATE_WAKE;
                }
              else if (ret == -EAGAIN && !g_vad_warned)
                {
                  g_vad_warned = true;
                  syslog(LOG_WARNING, "voice: microphone/VAD not running; "
                         "only the button and the console can start a "
                         "turn\n");
                }

              break;
            }

          case AIVOX3_STATE_WAKE:
            {
              /* Acknowledge, then re-arm the microphone from a clean slate.
               * The tone itself is played by the half-duplex pipeline, which
               * closes capture behind our back -- so drop whatever the mic
               * picked up during playback before listening for real,
               * otherwise the prompt echo becomes the user's utterance.
               */
              ui_set_expression(UI_EXPR_THINK);
              (void)audio_play_tone(1);

              (void)audio_capture_stop();
              usleep(150000);
              rearm_mic();

              g_listen_ticks = 0;

              /* A voice trigger already carries its utterance. */
              if (g_utt_nsamples > 0)
                {
                  g_state = AIVOX3_STATE_TALK;
                }
              else
                {
                  set_status("Listening...");
                  g_state = AIVOX3_STATE_LISTEN;
                }

              break;
            }

          case AIVOX3_STATE_LISTEN:
            {
              set_status("Listening...");

              ret = collect_utterance();
              if (ret > 0)
                {
                  g_utt_nsamples = ret;
                  g_state        = AIVOX3_STATE_TALK;
                  break;
                }

              /* No VAD available? Fall back to a fixed-length recording so
               * the demo still works end to end.
               */
              if (ret == -EAGAIN)
                {
                  int nsamples = record_fixed_window();

                  if (nsamples > 0)
                    {
                      g_utt_nsamples = nsamples;
                      g_state        = AIVOX3_STATE_TALK;
                      break;
                    }
                }

              g_listen_ticks++;
              if (g_listen_ticks >= g_listen_limit)
                {
                  ui_set_expression(UI_EXPR_IDLE);
                  set_status("Standby -- talk to me");
                  g_state = AIVOX3_STATE_IDLE;
                }

              break;
            }

          case AIVOX3_STATE_TALK:
            {
              ret = do_bridge_turn();

              if (ret == OK && g_result.audio_len > 0)
                {
                  g_state = AIVOX3_STATE_SPEAK;
                }
              else
                {
                  /* Either the transport failed, or the bridge decided there
                   * was nothing worth saying (ambient noise). Both simply
                   * return to standby.
                   */
                  bridge_turn_result_release(&g_result);

                  if (ret == OK)
                    {
                      ui_set_expression(UI_EXPR_HAPPY);
                      set_status("Standby -- talk to me");
                    }
                  else
                    {
                      ui_set_expression(UI_EXPR_IDLE);
                      set_status(ret == -ENETDOWN ? "No Wi-Fi" :
                                 ret == -ETIMEDOUT ? "Bridge timeout" :
                                 "Bridge unreachable");
                    }

                  g_turn_src     = TURN_NONE;
                  g_utt_nsamples = 0;
                  rearm_mic();
                  g_state = AIVOX3_STATE_IDLE;
                }

              break;
            }

          case AIVOX3_STATE_SPEAK:
            {
              ui_set_expression(UI_EXPR_SPEAK);
              face_follow_trigger_speak();

              /* The status line deliberately stays ASCII: LV_FONT_DEFAULT
               * carries no CJK glyphs, so a Chinese reply renders as boxes.
               * It goes to the console in full instead; switch these two
               * lines once a CJK font is registered in ui_lvgl.c.
               */
              set_status("Speaking...");

              /* Blocks for the whole answer. Capture is already closed, so
               * nothing can be recorded while (or just after) we play.
               */
              (void)audio_play_pcm(g_result.audio, g_result.audio_len);

              bridge_turn_result_release(&g_result);

              /* Let the speaker ring down before opening the microphone
               * again, or the tail of the answer retriggers the VAD and the
               * device interviews itself in a loop.
               */
              usleep(AIVOX3_POST_PLAY_GUARD_MS * 1000);
              rearm_mic();

              ui_set_expression(UI_EXPR_HAPPY);
              g_turn_src     = TURN_NONE;
              g_utt_nsamples = 0;
              set_status("Standby -- talk to me");
              g_state = AIVOX3_STATE_IDLE;
              break;
            }

          case AIVOX3_STATE_MODE_READ:
            {
              const char *rpath = g_read_file[0] != '\0' ?
                                  g_read_file : AIVOX3_READ_NOVEL_PATH;
              int         rr;

              set_status("Reading mode...");
              ui_set_reading_mode(true);

              /* The microphone must stay off while reading: the codec is
               * half-duplex and we must not let the VAD eavesdrop on the book
               * (it would start a chat turn mid-chapter). */
              (void)audio_capture_stop();

              /* If the local novel file is missing, try to pull the one the user
               * staged on the PC bridge via /upload_novel and persist it onto
               * FFat. Falls back to the built-in sample if the pull fails or
               * there is no network. */
              if (net_is_connected())
                {
                  int fd = open(rpath, O_RDONLY);
                  if (fd < 0)
                    {
                      uint8_t *nb = (uint8_t *)malloc(
                                      (size_t)AIVOX3_READ_NOVEL_MAX_BYTES);
                      size_t   nlen = 0;

                      if (nb != NULL &&
                          bridge_novel_download(
                            nb, (size_t)AIVOX3_READ_NOVEL_MAX_BYTES,
                            &nlen) == OK && nlen > 0)
                        {
                          int wfd = open(rpath,
                                         O_WRONLY | O_CREAT | O_TRUNC, 0666);
                          if (wfd >= 0)
                            {
                              (void)write(wfd, nb, nlen);
                              close(wfd);
                            }
                        }

                      if (nb != NULL)
                        {
                          free(nb);
                        }
                    }
                  else
                    {
                      close(fd);
                    }
                }

              rr = novel_reader_open(rpath);
              if (rr < 0)
                {
                  syslog(LOG_ERR, "ERROR: cannot open novel: %d\n", rr);
                  set_status("No novel available");
                  ui_set_reading_mode(false);
                  rearm_mic();
                  g_state = AIVOX3_STATE_IDLE;
                  break;
                }

              g_read_pcm      = NULL;
              g_read_pcm_len  = 0;
              g_read_paused   = false;
              g_read_stop     = false;
              g_read_next     = false;
              g_read_prev     = false;
              g_state = AIVOX3_STATE_READING;
              break;
            }

          case AIVOX3_STATE_READING:
            {
              int   ch;
              int   ln;
              int   tot;
              int   pct;
              char  seg[1024];
              size_t seglen = 0;

              /* Control: leave reading mode. */
              if (g_read_stop)
                {
                  g_read_stop = false;
                  novel_reader_close();
                  if (g_read_pcm != NULL)
                    {
                      free(g_read_pcm);
                      g_read_pcm = NULL;
                    }

                  g_read_pcm_len = 0;
                  ui_set_reading_mode(false);
                  set_status("Standby -- talk to me");
                  ui_set_expression(UI_EXPR_IDLE);
                  rearm_mic();
                  g_state = AIVOX3_STATE_IDLE;
                  break;
                }

              /* Control: skip backward / forward one segment. */
              if (g_read_prev)
                {
                  g_read_prev = false;
                  if (g_read_pcm != NULL)
                    {
                      free(g_read_pcm);
                      g_read_pcm = NULL;
                    }

                  g_read_pcm_len = 0;
                  if (novel_reader_prev() < 0)
                    {
                      set_status("Already at start");
                    }

                  g_read_paused = false;
                }

              if (g_read_next)
                {
                  g_read_next = false;
                  if (g_read_pcm != NULL)
                    {
                      free(g_read_pcm);
                      g_read_pcm = NULL;
                    }

                  g_read_pcm_len = 0;
                  if (novel_reader_next() < 0)
                    {
                      novel_reader_close();
                      ui_set_reading_mode(false);
                      set_status("Book finished");
                      ui_set_expression(UI_EXPR_HAPPY);
                      rearm_mic();
                      g_state = AIVOX3_STATE_IDLE;
                      break;
                    }

                  g_read_paused = false;
                }

              if (g_read_paused)
                {
                  set_status("Paused");
                  break;
                }

              ch  = novel_reader_chapter();
              ln  = novel_reader_line();
              tot = novel_reader_total();
              pct = novel_reader_progress();

              /* Need TTS for the current segment? */
              if (g_read_pcm == NULL)
                {
                  int sr = novel_reader_current(seg, sizeof(seg), &seglen);
                  if (sr < 0)
                    {
                      /* Nothing left to read. */
                      novel_reader_close();
                      ui_set_reading_mode(false);
                      set_status("Book finished");
                      ui_set_expression(UI_EXPR_HAPPY);
                      rearm_mic();
                      g_state = AIVOX3_STATE_IDLE;
                      break;
                    }

                  ui_set_expression(UI_EXPR_THINK);
                  set_status("Synthesizing...");

                  int tr = bridge_tts_synthesize(seg, &g_read_pcm,
                                                &g_read_pcm_len);
                  if (tr < 0 || g_read_pcm_len == 0)
                    {
                      syslog(LOG_WARNING,
                             "WARNING: TTS failed (%d); skip segment\n", tr);
                      if (g_read_pcm != NULL)
                        {
                          free(g_read_pcm);
                          g_read_pcm = NULL;
                        }

                      g_read_pcm_len = 0;
                      novel_reader_next();
                      break;
                    }
                }

              ui_set_expression(UI_EXPR_SPEAK);
              ui_show_reading(ch, ln, tot, pct);
              set_status("Reading...");

              /* Reuses the exact chat TTS playback path. Blocking, like SPEAK. */
              (void)audio_play_pcm(g_read_pcm, g_read_pcm_len);
              free(g_read_pcm);
              g_read_pcm = NULL;
              g_read_pcm_len = 0;

              /* Advance to the next segment (end-of-book ends the mode). */
              if (novel_reader_next() < 0)
                {
                  novel_reader_close();
                  ui_set_reading_mode(false);
                  set_status("Book finished");
                  ui_set_expression(UI_EXPR_HAPPY);
                  rearm_mic();
                  g_state = AIVOX3_STATE_IDLE;
                  break;
                }

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
      if ((loop % 40) == 0)
        ets_printf("[LOOP] iter=%d state=%d -> ui_refresh\n", loop, g_state);
      ui_refresh();

      usleep(AIVOX3_LOOP_TICK_MS * 1000);
      loop++;
    }

  return 0;
}
