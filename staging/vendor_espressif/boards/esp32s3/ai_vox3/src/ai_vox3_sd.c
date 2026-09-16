/****************************************************************************
 * src/ai_vox3_sd.c
 *
 * TF/SD card (MMC, 1-bit mode) bring-up for the emakefun AI-VOX3.
 *
 * Pin mapping (board.h): CMD=38, CLK=39, DAT0=40  (FAT32, <= 32 GB)
 *
 * The SD host controller is initialized and registered as /dev/mmcsd0;
 * the FAT filesystem is mounted by NSH / the application as needed.
 *
 * TODO(real-device): confirm the 1-bit SDIO signal routing names in the
 * current tree. The pins are also typically set via Kconfig
 * (CONFIG_ESP32S3_SDIO_*_PIN); both paths are shown for clarity.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <syslog.h>

#include <nuttx/mmcsd.h>
#include <nuttx/sdio.h>
#include <nuttx/fs/fs.h>

#include <arch/board/board.h>
#include <esp32s3_gpio.h>
#include <esp32s3_sdio.h>


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SDIO output signal routing (slot 0). */
#define AI_VOX3_SDIO_CMD_OUT    SDIO_CMD_OUT
#define AI_VOX3_SDIO_CLK_OUT    SDIO_CLK_OUT
#define AI_VOX3_SDIO_DAT0_OUT   SDIO_DATA0_OUT

/* Mount point used by the application / NVS todo store. */
#define SD_MOUNT_POINT          "/mnt/sd0"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ai_vox3_sd_initialize
 *
 * Description:
 *   Initialize the SDIO host in 1-bit mode and register the MMC/SD block
 *   device. Returns OK on success.
 *
 ****************************************************************************/

int ai_vox3_sd_initialize(void)
{
  struct sdio_dev_s *sdio;
  struct mmcsd_config_s config;
  int ret;

  /* Route SDIO signals to the board pins. */
  esp32s3_gpio_matrix_out(BOARD_SD_CMD_GPIO,  AI_VOX3_SDIO_CMD_OUT,  false, false);
  esp32s3_gpio_matrix_out(BOARD_SD_CLK_GPIO,  AI_VOX3_SDIO_CLK_OUT,  false, false);
  esp32s3_gpio_matrix_out(BOARD_SD_DAT0_GPIO, AI_VOX3_SDIO_DAT0_OUT, false, false);

  /* Initialize the SDIO host controller (slot 0). */
  sdio = esp32s3_sdio_initialize(BOARD_SD_SLOT);
  if (sdio == NULL)
    {
      syslog(LOG_ERR, "ERROR: esp32s3_sdio_initialize(slot %d) failed\n",
             BOARD_SD_SLOT);
      return -ENODEV;
    }

  /* 1-bit slot configuration. */
  memset(&config, 0, sizeof(config));
  config.slotno  = BOARD_SD_SLOT;
  config.type    = MMCSD_TYPE_SD;     /* SD card (not eMMC) */
  config.width   = 1;                 /* 1-bit data bus */

  ret = mmcsd_slotconfig(BOARD_SD_SLOT, &config);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mmcsd_slotconfig failed: %d\n", ret);
      return ret;
    }

  /* Register the MMC/SD block driver as /dev/mmcsd0. */
  ret = mmcsd_initialize(0, sdio, &config);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mmcsd_initialize failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "SD/MMC (1-bit) initialized as /dev/mmcsd0\n");
  return OK;
}
