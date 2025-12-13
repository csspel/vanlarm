#include "logging.h"
#include "config.h"

#include <SD_MMC.h>

static bool sdOk = false;

void loggingInit() {
  Serial.println("LOG: init SD_MMC...");

  // Sätt SDMMC-pinnar för T-SIM7080G-S3
  // (definierade i config.h)
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);

  // 1-bit-läge, mountpoint /sdcard
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("LOG: SD_MMC init FAILED");
    sdOk = false;
    return;
  }

  sdOk = true;

  // Öppna/skap loggfil
  File f = SD_MMC.open("/system.log", FILE_APPEND);
  if (f) {
    f.println();
    f.println("===== BOOT =====");
    f.close();
  }

  Serial.println("LOG: SD_MMC init OK");
}

void logSystem(const String &line) {
  uint32_t ms = millis();
  String out = String(ms / 1000) + "s " + line;

  // Alltid till Serial
  Serial.println(out);

  if (!sdOk) return;

  File f = SD_MMC.open("/system.log", FILE_APPEND);
  if (!f) return;

  f.println(out);
  f.close();
}
