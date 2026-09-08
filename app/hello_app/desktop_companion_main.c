/****************************************************************************
 * Contest 2026 team 393 - AI-VOX3 desktop companion demo
 *
 * A runnable, interactive demo of the AI-VOX3 hardware:
 *   - pan/tilt servos on /dev/pwm0 (PAN, GPIO42) and /dev/pwm1 (TILT, GPIO43)
 *   - ST7789 LCD face on /dev/lcd0
 *   - onboard buttons via board_buttons()
 *
 * Run from the NSH console:  nsh> desktop_companion
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <nuttx/timers/pwm.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/lcd_dev.h>
#include <arch/board/board.h>

#define LCD_DEV   "/dev/lcd0"
#define PWM_PAN   "/dev/pwm0"
#define PWM_TILT  "/dev/pwm1"

#define LCD_W 240
#define LCD_H 240

#ifndef CONFIG_PWM_DUTY_MAX
#  define PWM_DUTY_FULL 65535
#else
#  define PWM_DUTY_FULL CONFIG_PWM_DUTY_MAX
#endif

#define RGB565(r, g, b) \
  ((uint16_t)(((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | (((b) & 0xf8) >> 3))))

static int g_lcdfd = -1;

/****************************************************************************
 * LCD helpers
 ****************************************************************************/

static int lcd_getdev(FAR struct lcd_dev_s **dev)
{
  if (g_lcdfd < 0)
    {
      return -ENODEV;
    }

  if (ioctl(g_lcdfd, LCDDEV_IOCTL_GETDEV, (unsigned long)dev) < 0 || !*dev)
    {
      return -ENODEV;
    }

  return OK;
}

static void lcd_init(void)
{
  g_lcdfd = open(LCD_DEV, O_WRONLY);
  if (g_lcdfd < 0)
    {
      printf("LCD: %s not available (%d)\n", LCD_DEV, errno);
      return;
    }

  FAR struct lcd_dev_s *dev = NULL;
  if (lcd_getdev(&dev) < 0 || !dev)
    {
      printf("LCD: GETDEV failed\n");
      close(g_lcdfd);
      g_lcdfd = -1;
      return;
    }

  dev->setpower(dev, 1);
  printf("LCD: ready (%dx%d)\n", LCD_W, LCD_H);
}

static int lcd_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
  FAR struct lcd_dev_s *dev = NULL;
  if (lcd_getdev(&dev) < 0 || !dev)
    {
      return -ENODEV;
    }

  int w = x1 - x0 + 1;
  int h = y1 - y0 + 1;
  if (w <= 0 || h <= 0)
    {
      return -EINVAL;
    }

  FAR uint16_t *buf = (FAR uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
  if (!buf)
    {
      return -ENOMEM;
    }

  for (int i = 0; i < w * h; i++)
    {
      buf[i] = color;
    }

  struct lcd_area_s area;
  area.row_start = y0;
  area.row_end   = y1;
  area.col_start = x0;
  area.col_end   = x1;
  area.data      = buf;

  int r = dev->putarea(dev, &area);
  free(buf);
  return r;
}

static void draw_face(int mood)
{
  if (g_lcdfd < 0)
    {
      printf("(no LCD)\n");
      return;
    }

  /* background */
  lcd_fill_rect(0, 0, LCD_W - 1, LCD_H - 1, RGB565(20, 60, 160));
  /* eyes */
  lcd_fill_rect(60, 70, 100, 110, RGB565(255, 255, 255));
  lcd_fill_rect(140, 70, 180, 110, RGB565(255, 255, 255));
  /* mouth */
  if (mood > 0)
    {
      lcd_fill_rect(80, 150, 160, 170, RGB565(255, 80, 80));   /* smile */
    }
  else if (mood < 0)
    {
      lcd_fill_rect(80, 165, 160, 185, RGB565(255, 80, 80));   /* frown */
    }
  else
    {
      lcd_fill_rect(80, 160, 160, 170, RGB565(255, 80, 80));   /* neutral */
    }

  printf("face drawn (mood=%d)\n", mood);
}

/****************************************************************************
 * Servo helper
 ****************************************************************************/

static void servo_set(const char *devpath, int deg)
{
  int fd = open(devpath, O_WRONLY);
  if (fd < 0)
    {
      printf("%s: open failed (%d)\n", devpath, errno);
      return;
    }

  if (deg < 0)
    {
      deg = 0;
    }
  if (deg > 180)
    {
      deg = 180;
    }

  /* 50 Hz servo: 0deg -> 2.5% duty, 180deg -> 12.5% duty */
  double frac = 0.025 + ((double)deg / 180.0) * 0.10;
  uint32_t duty = (uint32_t)(frac * PWM_DUTY_FULL);

  struct pwm_info_s info;
  memset(&info, 0, sizeof(info));
  info.frequency        = 50;
  info.chars[0].duty    = duty;

  if (ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) < 0)
    {
      printf("%s: set failed (%d)\n", devpath, errno);
    }
  else
    {
      printf("%s -> %d deg (duty=%u)\n", devpath, deg, duty);
    }

  close(fd);
}

/****************************************************************************
 * Interactive loop
 ****************************************************************************/

static void print_help(void)
{
  printf("Commands:\n"
         "  servo <pan> <tilt>   set pan/tilt servos (0-180)\n"
         "  pan <deg> | tilt <deg>\n"
         "  face [mood]          draw face on LCD (-1/0/1)\n"
         "  buttons              read onboard buttons\n"
         "  demo                 run a short demo sequence\n"
         "  help                 this text\n"
         "  quit                 exit\n");
}

int main(int argc, char *argv[])
{
  printf("\n=== AI-VOX3 Desktop Companion ===\n");
  lcd_init();
  print_help();

  char line[128];
  while (1)
    {
      printf("aivox3> ");
      fflush(stdout);

      if (!fgets(line, sizeof(line), stdin))
        {
          break;
        }

      line[strcspn(line, "\r\n")] = 0;
      if (!*line)
        {
          continue;
        }

      if (!strcmp(line, "quit") || !strcmp(line, "exit"))
        {
          break;
        }
      else if (!strcmp(line, "help"))
        {
          print_help();
        }
      else if (!strcmp(line, "face"))
        {
          draw_face(0);
        }
      else if (!strcmp(line, "buttons"))
        {
          uint32_t b = board_buttons();
          printf("buttons raw=0x%02X (1=BTN1 2=BTN2 4=BTN3)\n", b);
        }
      else if (!strncmp(line, "servo ", 6))
        {
          int pan = 90;
          int tilt = 90;
          sscanf(line + 6, "%d %d", &pan, &tilt);
          servo_set(PWM_PAN, pan);
          servo_set(PWM_TILT, tilt);
          draw_face(0);
        }
      else if (!strncmp(line, "pan ", 4))
        {
          int d = 90;
          sscanf(line + 4, "%d", &d);
          servo_set(PWM_PAN, d);
        }
      else if (!strncmp(line, "tilt ", 5))
        {
          int d = 90;
          sscanf(line + 5, "%d", &d);
          servo_set(PWM_TILT, d);
        }
      else if (!strcmp(line, "demo"))
        {
          for (int a = 0; a <= 180; a += 45)
            {
              servo_set(PWM_PAN, a);
              servo_set(PWM_TILT, 90);
              usleep(200000);
            }

          draw_face(1);
          uint32_t b = board_buttons();
          printf("demo done. buttons=0x%02X\n", b);
        }
      else
        {
          printf("unknown: '%s' (try 'help')\n", line);
        }
    }

  if (g_lcdfd >= 0)
    {
      close(g_lcdfd);
    }

  printf("bye.\n");
  return 0;
}
