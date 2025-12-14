#include "sdcard.h"
#include "config.h"

#include <Arduino.h>
#include <SD_MMC.h>

static bool s_mounted = false;

bool sdcardInit() {
  if (s_mounted) return true;

  Serial.println("SD: init SD_MMC...");

  // T-SIM7080G-S3 pins (1-bit SDMMC)
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);

  // Mountpoint "/sdcard", 1-bit mode=true
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD: SD_MMC.begin FAILED (no SD?)");
    s_mounted = false;
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("SD: No card attached (CARD_NONE)");
    SD_MMC.end();
    s_mounted = false;
    return false;
  }

  s_mounted = true;

  uint64_t sizeMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("SD: mounted OK, size=%lluMB\n", sizeMB);

  return true;
}
