#include "sdcard.h"
#include <SD_MMC.h>
#include "power.h"
#include "logging.h"

// utilities.h i LilyGO anger SDMMC_CLK/SDMMC_CMD/SDMMC_DATA.
// Om du inte har den, definiera pins här (vanligt för T-SIM7080G-S3):
#ifndef SDMMC_CLK
#define SDMMC_CLK  38
#define SDMMC_CMD  39
#define SDMMC_DATA 40
#endif

bool sdcardInit() {
  // 1) Se till att SD-kortets ström är på (ALDO3 3.3V)
  // Vi lägger detta i power.cpp/power.h strax (se nästa steg),
  // men om du redan har PMU globalt där: kalla en funktion här.

  // 2) Sätt pins och mounta i 1-bit läge
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);

  if (!SD_MMC.begin("/sdcard", true)) {
    logSystem("SD: Card Mount Failed (SD_MMC.begin)");
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    logSystem("SD: No SD card detected (CARD_NONE)");
    return false;
  }

  uint64_t mb = SD_MMC.cardSize() / (1024 * 1024);
  logSystem("SD: mounted OK, size=" + String((uint32_t)mb) + "MB");
  return true;
}
