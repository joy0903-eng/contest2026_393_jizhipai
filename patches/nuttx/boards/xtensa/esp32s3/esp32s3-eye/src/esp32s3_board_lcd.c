/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_board_lcd.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <string.h>       /* [V627] strchr() for the /tmp/net.txt probe */
#include <debug.h>
#include <errno.h>
#include <syslog.h>
#include <fcntl.h>       /* [V554] open() for the /dev/fb0 probe */
#include <sys/ioctl.h>   /* [V554] FBIOGET_* */
#include <sys/mman.h>    /* [V554] mmap() the framebuffer to inspect it */
#include <sys/stat.h>    /* [V554] */

#include <nuttx/arch.h>
#include <nuttx/kthread.h>
#include <nuttx/sched.h>
#include <nuttx/board.h>
#include <nuttx/signal.h>
#include <unistd.h>      /* [V326] usleep */
#include <nuttx/clock.h> /* [V548] up_mdelay */
#include "esp32s3_ledc.h"   /* [V481] LEDC hardware PWM */
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7789.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>   /* [V554] struct fb_videoinfo_s / fb_planeinfo_s */

#include <arch/board/board.h>

#include "esp32s3_gpio.h"
#include "esp32s3_spi.h"
#include "hardware/esp32s3_gpio_sigmap.h"

/* [V546] LEDC backlight helper implemented in arch/xtensa/src/esp32s3/
 * esp32s3_ledc.c.  Declared in esp32s3_ledc.h but that header pulls in
 * arch-private registers, so re-declare it here.
 * [V548] No longer called (the backlight is plain GPIO), but keep the
 * declaration so the LEDC path stays available if we ever revisit it.
 */
extern int esp32s3_ledc_raw_backlight(uint32_t freq, uint32_t duty16)
  __attribute__((unused));


#include "esp32s3-eye.h"

/* [LCDDIAG] Board-level LCD / backlight / SPI pin tracing (black-screen hunt).
 * Prints the physical pin numbers plus their read-back levels so we can tell
 * whether the backlight (GPIO16) is really driven high, whether the shared
 * DC/MISO pin (GPIO14) ended up as a plain GPIO output, and whether the SPI3
 * bus pins were (re)configured successfully.
 */

#define LCDDIAG(fmt, ...) \
  syslog(LOG_ERR, "[LCDDIAG] board: " fmt "\n", ##__VA_ARGS__)

/* [V535] GPIO16 full output chain -- authoritative addresses from headers:
 *
 *   IO_MUX GPIO16           = 0x60009000 + 0x04 + 16*4 = 0x60009044
 *     (PERIPHS_IO_MUX_GPIO0_U = REG_IO_MUX_BASE + 0x04, so configgpio's
 *      (pin+1)*4 formula IS correct.  MCU_SEL=1 = GPIO function.)
 *   GPIO_FUNC16_OUT_SEL_CFG = DR_REG_GPIO_BASE + 0x594 = 0x60004594
 *     (GPIO matrix: WHICH signal drives the pad output.  Reset = 256 =
 *      SIG_GPIO_OUT_IDX so the pad follows GPIO_OUT bit16.  If a peripheral
 *      driver reroutes this, GPIO_OUT toggles but the pin does not!)
 *   GPIO_PIN16_REG          = DR_REG_GPIO_BASE + 0xB4 = 0x600040B4
 *     (bit2 PAD_DRIVER: 1 = open-drain -> HIGH cannot drive the line.)
 *   GPIO_PIN20_REG          = DR_REG_GPIO_BASE + 0xC4 = 0x600040C4
 *     (v534 mistakenly wrote 0x1A00 here believing it was the IO_MUX;
 *      restore it to 0.)
 *
 * The pad only outputs the backlight level when ALL of these agree:
 * MCU_SEL=1 AND OUTSEL=256 AND PAD_DRIVER=0 AND GPIO_OUT.b16 = level. */
static void ai_vox3_bl_dump16(const char *tag)
{
  volatile uint32_t *iomux  = (volatile uint32_t *)0x60009044u;
  volatile uint32_t *outsel = (volatile uint32_t *)0x60004594u;
  volatile uint32_t *pinreg = (volatile uint32_t *)0x600040B4u;
  volatile uint32_t *pin20  = (volatile uint32_t *)0x600040C4u;
  /* [V536] RTC-domain suspects: GPIO16 == XTAL_32K_N pad.
   *   RTCIO_XTAL_32N_PAD_REG  = 0x60008400 + 0xc4 = 0x600084C4
   *     bit19 X32N_MUX_SEL: 1 = pad claimed by the RTC 32K oscillator
   *     (analog domain, overrides the digital IO_MUX entirely).
   *   RTC_CNTL_RTC_CLK_CONF_REG = 0x60008000 + 0x74 = 0x60008074
   *     bit8 DIG_XTAL32K_EN: digital 32K oscillator enable.
   * [V537] Pad HOLD latches live in the RTC power domain and SURVIVE a
   *   USB reset -- a hold bit set by any earlier firmware freezes the pad
   *   forever no matter what every other register says.
   *   RTC_CNTL_RTC_PAD_HOLD_REG = 0x600080D8  (RTC pads = GPIO0..21, bit16)
   *   RTC_CNTL_DIG_PAD_HOLD_REG = 0x600080DC  (digital pads) */
  volatile uint32_t *x32n    = (volatile uint32_t *)0x600084C4u;
  volatile uint32_t *clkcf   = (volatile uint32_t *)0x60008074u;
  volatile uint32_t *rtchold = (volatile uint32_t *)0x600080D8u;
  volatile uint32_t *dighold = (volatile uint32_t *)0x600080DCu;

  LCDDIAG("%s: MUX=0x%08x OUTSEL=0x%08x PIN16=0x%08x X32N=0x%08x "
          "RTCHOLD=0x%08x DIGHOLD=0x%08x",
          tag, (unsigned)*iomux, (unsigned)*outsel, (unsigned)*pinreg,
          (unsigned)*x32n, (unsigned)*rtchold, (unsigned)*dighold);
}

/* [V546] Retained for diagnosis but no longer called: the backlight is now
 * driven by LEDC, and this function's whole purpose was to force GPIO16 to be
 * a plain GPIO output -- which would detach it from the LEDC matrix. */
static void ai_vox3_bl_force_gpio(void) __attribute__((unused));
static void ai_vox3_bl_force_gpio(void)
{
  volatile uint32_t *iomux    = (volatile uint32_t *)0x60009044u;
  volatile uint32_t *outsel   = (volatile uint32_t *)0x60004594u;
  volatile uint32_t *pinreg   = (volatile uint32_t *)0x600040B4u;
  volatile uint32_t *pin20    = (volatile uint32_t *)0x600040C4u;
  volatile uint32_t *x32n     = (volatile uint32_t *)0x600084C4u;
  volatile uint32_t *clkcf    = (volatile uint32_t *)0x60008074u;
  volatile uint32_t *rtchold  = (volatile uint32_t *)0x600080D8u;
  volatile uint32_t *dighold  = (volatile uint32_t *)0x600080DCu;

  /* RTC domain: give the pad back to the digital IO_MUX and stop the 32K
   * oscillator.  Read-modify-write: these registers hold other fields. */
  *x32n     &= ~(1u << 19);   /* X32N_MUX_SEL = 0: pad follows digital IO_MUX */
  *clkcf    &= ~(1u << 8);    /* DIG_XTAL32K_EN = 0 */
  *rtchold  &= ~(1u << 16);   /* [V537] release GPIO16 RTC-pad hold latch */
  *dighold  &= ~(1u << 16);   /* [V537] same, in case the mapping differs */

  *pin20  = 0;             /* undo v534 accidental write */
  *outsel = 256u;          /* SIG_GPIO_OUT_IDX: pad out <- GPIO_OUT bit16 */
  *pinreg = 0;             /* push-pull, no interrupt */
  *iomux  = (uint32_t)((1u << 12) | (1u << 9) | (2u << 10));
}

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct spi_dev_s *g_spidev;
static struct lcd_dev_s *g_lcd;

/****************************************************************************
 * Public Functions
 ****************************************************************************/


/****************************************************************************
 * Name:  lcd_selftest
 *
 * Description:
 *   AI-VOX3 boot self-test (v530).  st7789_lcdinitialize() already issues
 *   SWRESET+SLPOUT+DISPON and fills the panel with 0xffff (white), so a
 *   black screen after boot means the BACKLIGHT is OFF, i.e. the polarity
 *   assumption is wrong.  This thread paints a solid color block via putarea
 *   and BLINKS the backlight (LOW/HIGH each second) so the user can tell us
 *   which backlight state lights the panel -- that disambiguates
 *   active-low vs active-high with a single flash.
 *
 *   [V530] Backlight on AI-VOX3 is ACTIVE-LOW: GPIO16 LOW = on, HIGH = off.
 *   The original openvela board file wrote HIGH, which actually switched the
 *   backlight OFF -- that is why every build looked black even though
 *   st7789_lcdinitialize() returned a valid device and filled white.
 *
 ****************************************************************************/

#define AI_VOX3_LCD_SELFTEST 0   /* [V580] keep the bounded 16 s sweep so we
                                  * still get a hardware proof on every boot,
                                  * but it now EXITS and hands /dev/fb0 to
                                  * info_screen instead of fighting it for
                                  * ever.  ★ Tested with #if below -- never
                                  *   #ifdef (`#define X 0` still counts as
                                  *   defined, which once kept the thread
                                  *   alive after I thought I had removed it). */

/* [V625] Backlight bisection gate.
 *
 * A controlled A/B on real hardware (6 boots each, every run verified by its
 * own ESP-ROM banner) finally separated the screen subsystem from the UI:
 *
 *   v619  screen subsystem OFF              -> 6/6 associations succeeded
 *   v624  panel + fb + SPI3 up, LVGL UI not  -> 0/6 associations succeeded
 *         started (the launcher waited for an address that never came)
 *
 * So the LVGL UI is NOT the breaker - the panel bring-up is.  V625 keeps
 * everything of v624 (SPI3 routed, ST7789 initialised, /dev/fb0 registered)
 * and removes only the two things that touch GPIO16: the [V548] toggle test
 * and the "leave it HIGH" drive, plus the 2 s blkeeper thread.
 *
 * GPIO16 is XTAL_32K_N on the ESP32-S3, and esp32s3_wifi_adapter.c reads
 * RTC_SLOW_CLK_CAL_REG (the slow-clock calibration) when it needs a clock
 * estimate, so a loaded or clamped 32 kHz pad is a plausible mechanism.
 *
 *   guard on  -> backlight never driven, panel stays dark (expect 6/6)
 *   guard off -> v624 behaviour (expect 0/6)
 *
 * Measured, 6 real boots each:
 *   v619  no screen subsystem at all         -> 6/6, no disconnects
 *   v624  panel + fb + SPI3, GPIO16 driven   -> 0/6, reason=2 every time
 *   v625  panel + fb + SPI3, GPIO16 untouched-> 3/6, failures are reason=39
 *
 * Driving GPIO16 is what turns the failure into reason=2 (AUTH_EXPIRE).
 *
 * [V626] THE FIX ATTEMPT: give the pad back to the digital IO_MUX and stop
 * the 32 kHz oscillator BEFORE driving it.  ai_vox3_bl_force_gpio() was
 * written for exactly this and then left uncalled when the backlight moved to
 * plain GPIO - so every build since has driven a pad that the RTC domain
 * still owned.  Re-enable the drive and run the handoff first.
 */

#define V625_NO_BACKLIGHT_DRIVE 0
/* [V681] RE-ENABLED.  v626 backed this off after a 0/6 WiFi association
 * failure, but that measurement predates two fixes that change its
 * premise, and the failure mode it was chasing cannot be the pad itself:
 *
 *   - The v626 build drove the pad and STILL left it dark, because the
 *     RTC 32 kHz domain still owned it: with X32N_MUX_SEL set, whatever
 *     the digital side writes is discarded.  The board log proves this
 *     directly (X32N=0x40000000 on every boot, i.e. the RTC side keeps
 *     the pad while the panel sits dark).
 *   - WiFi now runs on the RC_SLOW slow clock (V628_FORCE_RC_SLOW=1) and
 *     the V636 dual-heap split fixed the wlan_sta_initialize -12
 *     allocation failure, which is what a 0/6 association run really
 *     looked like back then.
 *
 * So the handoff is not the thing that broke WiFi -- an undriven pad is
 * what kept the backlight dark.  Hand the pad back to the digital IO_MUX
 * before driving it.  If association does regress, the counter-test is to
 * set this back to 0; the backlight will go dark again but WiFi returns.
 */
#define V626_XTAL32K_HANDOFF    1   /* [V681] re-enabled: pad is never drivable otherwise */
#define V627_DEFER_BACKLIGHT    0   /* [V627] did not help -> see [V628]   */
#define V628_LOG_SLOWCLK        1   /* print the RTC slow-clock source     */
#define V628_FORCE_RC_SLOW      1   /* park the slow clock on RC_SLOW      */
#define V627_WAIT_S             75  /* max s to wait for /tmp/net.txt      */

/* [V548] Backlight pin mode that allows a TRUE level read-back.
 *
 * esp32s3_configgpio(OUTPUT) alone never sets FUN_IE, so the input buffer is
 * off and esp32s3_gpioread() is hard-wired to 0 for that pad -- every single
 * "BCKL=0" line in the v530..v547 logs was an artifact of that, NOT evidence
 * that the pin was low.  Passing INPUT|OUTPUT sets FUN_IE and keeps the
 * output driver enabled, so gpioread() finally reflects the real pad level.
 */
#define AI_VOX3_BCKL_MODE  (INPUT | OUTPUT)

#if AI_VOX3_LCD_SELFTEST
static void lcd_selftest(void)
{
  struct lcd_planeinfo_s pinfo;
  const int W = 240;
  const int H = 240;
  uint16_t *fb = (uint16_t *)kmm_malloc((size_t)W * H * sizeof(uint16_t));
  const uint16_t colors[4] = { 0xf800, 0x07e0, 0x001f, 0xffff };
  const char *names[4] = { "RED", "GREEN", "BLUE", "WHITE" };
  int phase = 0;
  int i;
  int round;

  if (g_lcd->getplaneinfo(g_lcd, 0, &pinfo) != OK)
    {
      LCDDIAG("selftest: getplaneinfo FAILED");
      return;
    }

  if (fb == NULL)
    {
      LCDDIAG("selftest: kmm_malloc FAILED");
      return;
    }

  /* [V548] Plain-GPIO backlight, ACTIVE-HIGH, held on; panel cycles R/G/B/W
   * exactly like the verified hardware test
   *   C:\Users\xxzx\Desktop\VOX3\backlight_test\backlight_test.ino
   * which does pinMode(BL, OUTPUT); digitalWrite(BL, HIGH); then
   * SWRESET/SLPOUT/COLMOD(0x55)/MADCTL(0x00)/INVON/NORON/DISPON and finally
   * a 4-colour full-screen loop at 2 s per colour.
   *
   * The pin mode here additionally sets INPUT so gpioread() is meaningful. */
  esp32s3_configgpio(ESP32S3_EYE_DISPLAY_BCKL, AI_VOX3_BCKL_MODE);
  esp32s3_gpiowrite(ESP32S3_EYE_DISPLAY_BCKL, 1);   /* ACTIVE-HIGH = on */

  

  for (round = 0; round < 2; round++)
    {
      for (phase = 0; phase < 4; phase++)
        {
          for (i = 0; i < W * H; i++)
            {
              fb[i] = colors[phase];
            }

          pinfo.putarea(g_lcd, 0, H - 1, 0, W - 1,
                        (FAR const uint8_t *)fb,
                        (fb_coord_t)(W * sizeof(uint16_t)));

          /* Re-assert, then read back for real. */
          esp32s3_configgpio(ESP32S3_EYE_DISPLAY_BCKL, AI_VOX3_BCKL_MODE);
          esp32s3_gpiowrite(ESP32S3_EYE_DISPLAY_BCKL, 1);

          

          sleep(2);
        }
    }

  /* [V553] ★ Bounded self-test: 2 rounds x 4 colours x 2 s = 16 s, then EXIT.
   * The old for(;;) loop never returned and fought info_screen (pid=11) for
   * /dev/fb0 forever -- info_screen cleared the panel to black and drew its
   * text once a second while the selftest repainted a solid colour every 3 s,
   * which is exactly the "亮了一下又灭了" flicker the user kept reporting.
   * Now the panel shows a clean R/G/B/W sweep for the first 16 s and then
   * info_screen owns /dev/fb0 exclusively. */

  free(fb);
  
}
#endif

/****************************************************************************
 * Name:  lcd_fb_probe
 *
 * Description:
 *   [V554] Ground-truth probe for the "self-test is perfect but info_screen
 *   shows nothing" question.  The panel proves the SPI3 + software-CS path
 *   works whenever *we* call pinfo.putarea(); what is unproven is whether the
 *   framebuffer device (/dev/fb0, i.e. drivers/lcd/lcd_framebuffer.c) ever
 *   receives the pixels info_screen writes into its mmap.
 *
 *   The self-test writes straight to the LCD, so it can be perfectly correct
 *   while /dev/fb0 is a black hole.  This thread opens /dev/fb0 exactly like
 *   info_screen does, mmaps it, and every 5 s counts how many pixels differ
 *   from the background colour.  Three outcomes are distinguishable:
 *     - fb=<n> px!=bg  with n>0  -> info_screen IS painting; the LCD-side push
 *                                   is what is broken (FBIO_UPDATE path).
 *     - fb=<n> px!=bg  with n==0 -> info_screen never wrote anything (its
 *                                   fb_init failed, or it is stuck).
 *     - open/mmap/ioctl error    -> the framebuffer node itself is the
 *                                   problem (wrong path, not registered).
 *   Runs for 50 s and then stops so it does not spam the log forever.
 *
 ****************************************************************************/

static void lcd_fb_probe(void) __attribute__((unused));
static void lcd_fb_probe(void)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  uint16_t *mem;
  int fd;
  int i;

  /* [V554] ★ CRITICAL: this thread is started from board_lcd_initialize(),
   * but /dev/fb0 is only created later, by fb_register() ->
   * fb_register_device() -> register_driver("/dev/fb%d").  Opening it too
   * early fails with ENOENT (errno=2) -- which is exactly what the first
   * run of this probe reported.  Wait until well after bringup has
   * registered the node AND after info_screen's own 20 s hold-off, so the
   * first sample reflects what the panel task has actually painted.
   *
   * [V557] Report through syslog() only.  syslog reaches the serial console
   * during boot (proven by every [LCDDIAG] line), whereas the FILE* mirror
   * tried in v556 was never observed and only added risk. */
  sleep(45);

  fd = open("/dev/fb0", O_RDWR);
  if (fd < 0)
    {
      
      return;
    }

  if (ioctl(fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0)
    {
      
      close(fd);
      return;
    }

  if (ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&pinfo) < 0)
    {
      
      close(fd);
      return;
    }

  mem = (uint16_t *)mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FILE, fd, 0);
  if (mem == MAP_FAILED)
    {
      
      close(fd);
      return;
    }

  

  for (i = 0; i < 10; i++)   /* 10 x 5 s = 50 s */
    {
      int n = 0;
      unsigned int total;
      unsigned int k;

      sleep(5);

      /* Count pixels that are neither the dark-navy background (0xFFE7) nor
       * pure black -- a non-zero count means somebody drew something. */
      total = (pinfo.stride / 2) * vinfo.yres;
      for (k = 0; k < total; k++)
        {
          uint16_t p = mem[k];
          if (p != 0xFFE7 && p != 0x0000)
            {
              n++;
            }
        }

      
    }

  munmap(mem, pinfo.fblen);
  close(fd);
  
}


/****************************************************************************
 * Name:  board_lcd_initialize
 *
 * Description:
 *   Initialize the LCD video hardware.  The initial state of the LCD is
 *   fully initialized, display memory cleared, and the LCD ready to use, but
 *   with the power setting at 0 (full off).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

/* [V460] 原版的 board_lcd_bckl_keeper 已被删除。
 *
 * 事实：.bak_bl（原始板级文件）里没有 keeper、没有 PWM 脉冲、
 * 没有低->高跳变，GPIO16 一直保持低电平，屏幕是亮的。
 * 所以"保活脉冲"整套东西是多余的，而且是这几轮黑屏的祸根
 * （把它改高 = 关背光）。
 * 真正点亮面板靠 st7789_lcdinitialize() 里的 DISPON(0x29)。
 */

/* [V440] 背光脚是否已经配置过。只用于避免重复 configgpio。 */
static bool s_cfg __attribute__((unused));

void board_lcd_backlight_on(void)
{
  /* [V525] 本函数**故意什么都不做**。
   *
   * 它被 vox3_net_keepalive() 周期性调用。如果在这里 configgpio(GPIO16)
   * 就会和 keeper 线程抢同一个 pad —— v523/v524 的屏幕闪烁正是这种
   * 「多处写同一个引脚、谁最后写谁赢」的竞态造成的。
   *
   * 背光现在由 board_lcd_bl_breath() 独占管理：单点、每2 秒无条件重钉
   * HIGH，不做任何读回判断（gpioread 对 output-only 引脚恒返回 0）。
   */
}

static void board_lcd_bl_breath(void)
{
  /* [V524] 背光 = GPIO16 普通高电平常亮 + 每秒重新钉一次。
   *
   * [V530 更正] 之前写「拉高→亮、拉低→灭（active-high）」是错的。真相：
   * st7789_lcdinitialize() 已经 SWRESET+SLPOUT+DISPON+fill(0xffff)（白屏），
   * 但每版都黑 → 背光其实是 **低电平点亮（active-low）**，写 HIGH=灭。
   * 所以这里改成每 2 秒无条件重钉 **LOW**。
   *
   * ★ 为什么不继续用 LEDC：v481~v522 一直认定必须用 LEDC 硬件 PWM，理由是
   *   "背光 LED 是大电流负载，普通 GPIO 带不动"。v522 把引脚按 plain GPIO
   *   HIGH 驱动时面板正常点亮，实测推翻了该前提——普通 GPIO 完全带得动。
   *
   * ★ LEDC 侧为什么会失败：寄存器读回全部正确，逐一核对无误——
   *     CONF0=0x7  → bit2 = LEDC_SIG_OUT_EN_CH0，输出已使能
   *     CONF1=0x80000000 → LEDC_DUTY_START_CH0
   *     DUTY=0x6000 → 0x600<<4 = 1536/2048 = 75%
   *     func_out_sel=77 = LEDC_LS_SIG_OUT4_IDX → ch4 → GPIO16
   *   路由、使能、占空比、频率全部对，但波形就是没落到面板上。既然 GPIO
   *   能点亮，就用它；不再为一个解释不通的现象继续在 LEDC 上兜圈。
   *
   * ★★★ v525：v524 的「读回判断」是个bug，keeper 一直误判被抢。
   *
   * v524 里keeper 写的是 if (esp32s3_gpioread(bl) != 1) 才重新钉，
   * 结果日志里每秒都打「backlight re-asserted (pin was not HIGH)」。
   * 当时误判成「有别的代码在抢 pad」。真实原因在 esp32s3_configgpio() 里：
   *
   *     if ((attr & INPUT)  != 0) { ...; func |= FUN_IE; }    // 只在 INPUT 分支
   *     if ((attr & OUTPUT) != 0) { ... GPIO_ENABLE_W1TS ... } // 只开出使能
   *
   * 我们只传了 OUTPUT，FUN_IE（输入缓冲使能）从来没被置上。而
   * esp32s3_gpioread() 读的正是 GPIO_IN_REG —— **输入通路**。
   * 输入缓冲关着，gpioread 对这个引脚永远返回 0，跟 pad 实际电平无关。
   *
   * 所以：既不存在抢占者（grep 全树确认 esp32s3_ledc_raw_backlight()
   * **只有定义、没有任何调用者**，LEDC 根本没被启动过），也读不出真实
   * 电平。唯一的闪烁源就是 v523 那次性动作退出后没人再管这个pad。
   *
   * 修法：不再读回判断，**无条件重钉**。每 2 秒两步写一次
   * （configgpio(OUTPUT) + gpiowrite(1)），开销可忽略，且不管未来谁碰
   * 这个 pad 都能恢复。
   *
   * ★★★ v528 关键修正：绝对**不要**调 esp32s3_gpio_matrix_out(bl, 0, 0, 0)。
   *   信号索引 0 在 ESP32-S3 上不是"解绑"，而是把 pad 输出源接到信号 0，
   *   等于把 GPIO_OUT_REG 和引脚断开，gpiowrite 失效。原始 v460 板级代码
   *   只做 configgpio+gpiowrite 就点亮了，从没碰过矩阵输出选择器。
   *   之前那句"先解绑再写电平"的"铁律"是建立在错误前提（LEDC 抢 pin）
   * 上的，纯属多此一举还帮倒忙，已删除。
   */
  const int bl = ESP32S3_EYE_DISPLAY_BCKL;
  int beat = 0;

  

  for (;;)
    {
      /* [V681] Plain GPIO, ACTIVE-LOW: the backlight net is driven LOW to
       * light the panel and HIGH to turn it off.  Re-asserted with
       * INPUT|OUTPUT so the read-back is real (see AI_VOX3_BCKL_MODE).
       * This thread touches ONLY the backlight pad -- never the panel data
       * -- so /dev/fb0 is the sole owner of the SPI3 bus.
       *
       * It also re-runs the RTC handoff on every pass.  The pad HOLD
       * latches live in the RTC power domain and survive a USB reset, so
       * anything that ran before this thread (or any future early boot
       * path) can leave GPIO16 owned by the 32 kHz oscillator again -- and
       * then every write below is silently dropped and the screen goes
       * black after a few seconds of uptime. */
      if (V626_XTAL32K_HANDOFF)
        {
          ai_vox3_bl_force_gpio();
        }

      esp32s3_configgpio(bl, AI_VOX3_BCKL_MODE);
      esp32s3_gpiowrite(bl, 0);   /* ACTIVE-LOW: LOW = lit */

      /* [V678] Read the pad back for real (the mode carries INPUT since
       * V550, so gpioread is meaningful).  A lit backlight reads back LOW;
       * anything else means the pad was taken away from us again. */
      if (esp32s3_gpioread(bl) != 0)
        {
          syslog(LOG_ERR, "[V681] bl pin reads HIGH, expected LOW "
                 "(pad lost again?)\n");
          /* [V679] Evidence snapshot: registers as they are RIGHT after our
           * re-pin + write.  If it still reads 1 the pad is being pulled
           * or held by something else entirely. */
          ai_vox3_bl_dump16("high16");
        }
      else if (++beat >= 15)
        {
          beat = 0;
          syslog(LOG_INFO, "[V679] bl pin HIGH, keeper alive\n");
          /* [V679] Register snapshot on every heartbeat.  Anything that
           * reprograms GPIO16 through direct register writes (bypassing
           * esp32s3_configgpio, e.g. RTC / WiFi-domain code) is invisible
           * to the gpio.c audit -- this dump is not. */
          ai_vox3_bl_dump16("beat16");
        }

      /* Heartbeat so we can tell from the log that the keeper is alive and
       * that the pad still reads back HIGH. */
      

      sleep(2);
    }
}

/* [V627] Is /tmp/net.txt carrying a real address yet?
 *
 * netinit_publish_net_state() is the sole writer; line 1 is either the IPv4
 * address or the literal "NO NET", so a '.' in line 1 is a reliable
 * "DHCP finished" signal - the same contract the V624 UI launcher uses.
 */

static bool v627_net_has_ip(void)
{
  char buf[48];
  bool ok = false;
  FILE *f = fopen("/tmp/net.txt", "r");

  if (f != NULL)
    {
      if (fgets(buf, sizeof(buf), f) != NULL)
        {
          ok = (strchr(buf, '.') != NULL);
        }

      fclose(f);
    }

  return ok;
}

/* [V627] Deferred backlight bring-up.  THE EXPERIMENT.
 *
 * v619 (no screen subsystem) associated 6/6; v624/v626 (panel up AND GPIO16
 * driven during bring-up) associated 0/6 with reason=2; v625 (panel up,
 * GPIO16 never driven) managed 3/6.  The pad being driven is what kills the
 * first association - and on this board the first association is the only
 * one that counts.
 *
 * This thread leaves GPIO16 in its reset state until the link is already up,
 * then drives it HIGH and falls through into the 2 s keeper.  If association
 * returns to 6/6 the breaker is the TIMING of the drive (the pad is clamped
 * while auth / the 4-way handshake runs) rather than the pad state itself.
 */

static void v627_bckl_waiter(void)
{
  const int bl = ESP32S3_EYE_DISPLAY_BCKL;
  int waited = 0;

  

  while (waited < V627_WAIT_S && !v627_net_has_ip())
    {
      sleep(1);
      waited++;
    }

  

  /* [V681] Hand the pad back from the RTC 32 kHz domain before the first
   * drive, otherwise every write below is discarded and the panel stays
   * dark. */
  if (V626_XTAL32K_HANDOFF)
    {
      ai_vox3_bl_force_gpio();
    }

  esp32s3_configgpio(bl, AI_VOX3_BCKL_MODE);
  esp32s3_gpiowrite(bl, 0);   /* ACTIVE-LOW: LOW = lit */

  ai_vox3_bl_dump16("v627post");

  board_lcd_bl_breath();       /* never returns; keeps the pad lit */
}



/* [V628] The RTC slow clock: who is feeding it?
 *
 *   RTC_CNTL_RTC_CLK_CONF_REG = DR_REG_RTCCNTL_BASE + 0x74 = 0x60008074
 *     bits[31:30] ANA_CLK_RTC_SEL : 0 = RC_SLOW (internal ~136 kHz),
 *                                   1 = XTAL32K (external 32.768 kHz),
 *                                   2 = 8 MHz / 256
 *     bit[8]      DIG_XTAL32K_EN  : digital 32 kHz oscillator enable
 *
 * The xtensa ESP32-S3 port in openvela NEVER programs this field (only the
 * risc-v ESP32-C3 legacy port has rtc_clk_slow_freq_set()), so whatever the
 * ROM bootloader selected stays in force for the whole session.
 *
 * Why it matters here: the panel shares BOTH 32 kHz crystal pads --
 *   XTAL_32K_P = GPIO15  (SPI3 chip-select)
 *   XTAL_32K_N = GPIO16  (backlight)
 * If the ROM parked the slow clock on that crystal, then the SPI3 CS edges
 * on GPIO15 and the parked-HIGH backlight on GPIO16 both corrupt the WiFi
 * blob's slow-clock estimate: r39 while only CS toggles, r2 once the
 * backlight pad is held HIGH.
 *
 * V628 reads the field, then hands the slow clock to RC_SLOW *before* any
 * board code touches GPIO15 or GPIO16.
 */

static unsigned v628_rtc_clk_conf(void)
{
  volatile uint32_t *clkcf = (volatile uint32_t *)0x60008074u;
  return (unsigned)*clkcf;
}

static unsigned v628_slow_sel(void)
{
  return (v628_rtc_clk_conf() >> 30) & 0x3u;
}

int board_lcd_initialize(void)
{
  int i;

  /* [V550] ★★★ IDEMPOTENCY GUARD -- this is the fix for "亮了一下又灭了".
   *
   * board_lcd_initialize() is reached through TWO independent paths:
   *   1) esp32s3_bringup.c  -> board_lcd_initialize()
   *   2) fb_register()      -> drivers/lcd/lcd_framebuffer.c:689
   *                           -> board_lcd_initialize()   (framework call)
   * Both run on every boot, so the whole body executed twice.  Each run
   * created its own "lcdselftest" kthread (pid 9 AND pid 10), and both
   * threads then wrote full 240x240 frames to the SAME SPI3 bus while
   * re-asserting the backlight -- two writers on one bus = the panel
   * visibly lights up, blanks, lights up, blanks.  The second run also
   * re-issued SWRESET(0x01) to an already-initialised panel.
   *
   * Make the function idempotent: the first call does the real work, any
   * later call returns OK immediately. */
  if (g_lcd != NULL)
    {
      
      return OK;
    }

  /* Initialize non-SPI GPIOs */

  /* [V628] RTC slow-clock audit + fix.  MUST run before the first touch of
   * GPIO15 (SPI3 CS) or GPIO16 (backlight) - both are 32 kHz crystal pads. */
#if V628_LOG_SLOWCLK
  LCDDIAG("[V628] RTC_CLK_CONF=0x%08x ANA_CLK_RTC_SEL=%u "
          "(0=RC_SLOW 1=XTAL32K 2=8M/256) DIG_XTAL32K_EN=%u",
          v628_rtc_clk_conf(), v628_slow_sel(),
          (unsigned)((v628_rtc_clk_conf() >> 8) & 0x1u));
#endif

#if V628_FORCE_RC_SLOW
  {
    volatile uint32_t *clkcf = (volatile uint32_t *)0x60008074u;
    unsigned prev = v628_slow_sel();

    if (prev != 0u)
      {
        /* ANA_CLK_RTC_SEL = 0 (RC_SLOW) and stop the digital 32 kHz
         * oscillator, exactly as IDF's rtc_clk_slow_freq_set() +
         * rtc_clk_xtal32k_disable() pair does. */
        *clkcf = (*clkcf & ~(0x3u << 30)) & ~(1u << 8);
        
      }
    else
      {
        
      }
  }
#endif

  /* [V548] Print the REAL display-bus pins (SPI3, per the vendor example).
   * The old line printed SPI3 too but those values were still the Kconfig
   * defaults (cs=10 clk=6 mosi=7 miso=2) because the defconfig never set
   * them.  Now they really are cs=15 clk=17 mosi=21 miso=-1. */
  LCDDIAG("pins: DC=%d BCKL=%d SPI=%d | SPI3 cs=%d clk=%d mosi=%d miso=%d",
          ESP32S3_EYE_DISPLAY_DC, ESP32S3_EYE_DISPLAY_BCKL,
          ESP32S3_EYE_DISPLAY_SPI,
          CONFIG_ESP32S3_SPI3_CSPIN, CONFIG_ESP32S3_SPI3_CLKPIN,
          CONFIG_ESP32S3_SPI3_MOSIPIN, CONFIG_ESP32S3_SPI3_MISOPIN);

  esp32s3_configgpio(ESP32S3_EYE_DISPLAY_DC, OUTPUT);

  /* [V546] Snapshot the GPIO16 chain BEFORE any board code touches it --
   * this reveals which peripheral driver (if any) has rerouted the pad. */
  ai_vox3_bl_dump16("pre16");

  /* [V548] BACKLIGHT: plain GPIO output, driven HIGH, then a slow 3x toggle
   * with a REAL read-back at each step.
   *
   * AUTHORITATIVE EVIDENCE -- the one build verified on real hardware:
   *   C:\Users\xxzx\Desktop\VOX3\backlight_test\backlight_test.ino
   *       pinMode(PIN_LCD_BL, OUTPUT);
   *       digitalWrite(PIN_LCD_BL, HIGH);      // plain GPIO, HIGH = ON
   *
   * The pin mode used here is INPUT|OUTPUT rather than bare OUTPUT: only the
   * INPUT flag makes esp32s3_configgpio() set FUN_IE, and without FUN_IE
   * esp32s3_gpioread() always returns 0 regardless of the actual pad level.
   * That is why every previous "BCKL=0" log line was meaningless.
   *
   * The toggle loop below is the decisive test: if the read-back follows the
   * write (1,0,1,0,...), the pad is genuinely driving the backlight line.
   */
  if (!V625_NO_BACKLIGHT_DRIVE && !V627_DEFER_BACKLIGHT)
    {
      if (V626_XTAL32K_HANDOFF)
        {
          /* [V626] Take GPIO16 out of the RTC 32 kHz domain and shut the
           * oscillator down before the pad is driven as a push-pull output.
           * XTAL_32K_N is GPIO16 on the ESP32-S3, and the WiFi port reads the
           * RTC slow-clock calibration (esp32s3_wifi_adapter.c,
           * RTC_SLOW_CLK_CAL_REG), so a loaded crystal pad shows up as failed
           * authentication timing. */
          ai_vox3_bl_force_gpio();
          ai_vox3_bl_dump16("v626post-force");
        }

      
      for (i = 0; i < 3; i++)
        {
          esp32s3_configgpio(ESP32S3_EYE_DISPLAY_BCKL, AI_VOX3_BCKL_MODE);
          /* [V681] ACTIVE-LOW: HIGH is off, LOW is on. */
          esp32s3_gpiowrite(ESP32S3_EYE_DISPLAY_BCKL, 0);
          up_mdelay(50);

          esp32s3_gpiowrite(ESP32S3_EYE_DISPLAY_BCKL, 1);
          up_mdelay(50);

        }

      /* [V681] Leave it ON.  AI-VOX3's backlight net is ACTIVE-LOW (the LED
       * cathode is on GPIO16), so ON is a LOW level.  The old comment here
       * said ACTIVE-HIGH and wrote 1, which is why the panel stayed dark
       * even with the pad correctly configured. */
      esp32s3_configgpio(ESP32S3_EYE_DISPLAY_BCKL, AI_VOX3_BCKL_MODE);
      esp32s3_gpiowrite(ESP32S3_EYE_DISPLAY_BCKL, 0);

      ai_vox3_bl_dump16("post16");
    }
  else if (V627_DEFER_BACKLIGHT)
    {
      /* [V627] GPIO16 stays in its reset state for now; the [V548] drive is
       * handed to v627_bckl_waiter(), which asserts the backlight only once
       * /tmp/net.txt carries a real address - i.e. after association and
       * DHCP have both finished. */
      
    }
  else
    {
      /* [V625] GPIO16 is left completely untouched (reset state = input, no
       * drive).  The panel is initialised but dark. */
      
    }

  LCDDIAG("DC(%d) level=%d, BCKL(%d) level=%d",
          ESP32S3_EYE_DISPLAY_DC,
          (int)esp32s3_gpioread(ESP32S3_EYE_DISPLAY_DC),
          ESP32S3_EYE_DISPLAY_BCKL,
          (int)esp32s3_gpioread(ESP32S3_EYE_DISPLAY_BCKL));

  g_spidev = esp32s3_spibus_initialize(ESP32S3_EYE_DISPLAY_SPI);
  if (!g_spidev)
    {
      lcderr("ERROR: Failed to initialize SPI port %d\n",
             ESP32S3_EYE_DISPLAY_SPI);
      return -ENODEV;
    }

  LCDDIAG("spibus_initialize(%d)=%p", ESP32S3_EYE_DISPLAY_SPI,
          (FAR void *)g_spidev);

  /* Re-claim DC: SPI bus init reconfigures GPIO14 as MISO (LCD needs DC). */
  esp32s3_configgpio(ESP32S3_EYE_DISPLAY_DC, OUTPUT);

  LCDDIAG("after re-claim DC: DC(%d) level=%d", ESP32S3_EYE_DISPLAY_DC,
          (int)esp32s3_gpioread(ESP32S3_EYE_DISPLAY_DC));

  /* [V552] ★★★ SOFTWARE CS -- the real reason the panel stayed blank even
   * with the backlight proven on.
   *
   * esp32s3_spi_select() body is:
   *     #if SPI_HAVE_SWCS
   *       if (cs_pin >= 0) esp32s3_gpiowrite(cs_pin, !selected);
   *     #endif
   * and SPI_HAVE_SWCS is 1 only when CONFIG_ESP32S3_SPI_SWCS is set.  It was
   * NOT set, so the whole function compiled to a no-op: the ST7789 driver's
   * SPI_SELECT() calls did nothing, CS stayed under the SPI3 hardware's
   * automatic control (which releases CS after each byte group), and every
   * command/data pair was mis-framed.  Backlight on, panel blank -- exactly
   * the symptom.
   *
   * With CONFIG_ESP32S3_SPI_SWCS=y the driver owns CS as a plain GPIO:
   *   esp32s3_spi_init() does configgpio(cs, OUTPUT_FUNCTION_1) +
   *   gpio_matrix_out(cs, SIG_GPIO_OUT_IDX, 0, 0), and
   *   esp32s3_spi_select() then simply gpiowrite()s it.
   *
   * Therefore we must NOT attach SPI3_CS0_OUT_IDX to the CS pin here -- that
   * would steal it back from the GPIO path and break software CS.  Only the
   * CLK and MOSI lines stay on the SPI3 hardware signals.
   *
   * SPI3 signal indices (hardware/esp32s3_gpio_sigmap.h):
   *   SPI3_CLK_OUT_IDX = 66, SPI3_D_OUT_IDX = 68.
   */
  esp32s3_configgpio(CONFIG_ESP32S3_SPI3_CLKPIN, OUTPUT);
  esp32s3_configgpio(CONFIG_ESP32S3_SPI3_MOSIPIN, OUTPUT);
  esp32s3_gpio_matrix_out(CONFIG_ESP32S3_SPI3_CLKPIN, SPI3_CLK_OUT_IDX, 0, 0);
  esp32s3_gpio_matrix_out(CONFIG_ESP32S3_SPI3_MOSIPIN, SPI3_D_OUT_IDX, 0, 0);

  /* CS: driven by software (SPI_SELECT -> gpiowrite).  A plain GPIO output,
   * idle HIGH (deselected).  The SPI driver already routed it; re-assert the
   * GPIO function and the idle level so the first transfer starts clean. */
  esp32s3_configgpio(CONFIG_ESP32S3_SPI3_CSPIN, OUTPUT);
  esp32s3_gpio_matrix_out(CONFIG_ESP32S3_SPI3_CSPIN, SIG_GPIO_OUT_IDX, 0, 0);
  esp32s3_gpiowrite(CONFIG_ESP32S3_SPI3_CSPIN, 1);

  esp32s3_configgpio(ESP32S3_EYE_DISPLAY_DC, OUTPUT);

  

  g_lcd = st7789_lcdinitialize(g_spidev);
  if (!g_lcd)
    {
      lcderr("ERROR: st7789_lcdinitialize() failed\n");
      return -ENODEV;
    }

  LCDDIAG("st7789_lcdinitialize()=%p", (FAR void *)g_lcd);

  

#if AI_VOX3_LCD_SELFTEST
  {
    int spid = kthread_create("lcdselftest", 120, 4096,
                              (main_t)lcd_selftest, NULL);
    
  }
#else
  if (V627_DEFER_BACKLIGHT)
    {
      int dpid;

      /* [V627] The deferred-backlight waiter replaces the keeper: it waits
       * for an address, drives GPIO16, then falls through into
       * board_lcd_bl_breath() and keeps the pad pinned. */
      dpid = kthread_create("bcklwait", 100, 3072,
                            (main_t)v627_bckl_waiter, NULL);
      
    }
  else if (!V625_NO_BACKLIGHT_DRIVE)
    {
      int bpid;

      /* [V550] Backlight keeper: plain GPIO, ACTIVE-HIGH, re-asserted every
       * 2 s.  It does NOT touch the panel data, so /dev/fb0 owns the display
       * alone.  [V625] not started while the gate is on. */
      bpid = kthread_create("blkeeper", 120, 2048,
                            (main_t)board_lcd_bl_breath, NULL);
      syslog(LOG_INFO, "[V678] blkeeper create pid=%d\n", bpid);
      
    }
#endif

  return OK;
}

/****************************************************************************
 * Name:  board_lcd_getdev
 *
 * Description:
 *   Return a a reference to the LCD object for the specified LCD.  This
 *   allows support for multiple LCD devices.
 *
 * Input Parameters:
 *   devno - LCD device nmber
 *
 * Returned Value:
 *   LCD device pointer if success or NULL if failed.
 *
 ****************************************************************************/

struct lcd_dev_s *board_lcd_getdev(int devno)
{
  if (!g_lcd)
    {
      lcderr("ERROR: Failed to bind SPI port %d to LCD %d\n",
             ESP32S3_EYE_DISPLAY_SPI, devno);
    }
  else
    {
      lcdinfo("SPI port %d bound to LCD %d\n",
              ESP32S3_EYE_DISPLAY_SPI,
              devno);
      return g_lcd;
    }

  return NULL;
}

/****************************************************************************
 * Name:  board_lcd_uninitialize
 *
 * Description:
 *   Uninitialize the LCD support
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
  /* Turn the display off */

  g_lcd->setpower(g_lcd, 0);
}
