#include "logging.h"
#include "config.h"
#include "time_manager.h"
#include <SD_MMC.h>

static bool   sdOk    = false;
static String logPath = "/system.log";

void loggingInit() {
  // SD_MMC ska redan vara mountat av sdcardInit()
  // Verifiera att kort finns
  uint8_t t = SD_MMC.cardType();
  if (t == CARD_NONE) {
    Serial.println("LOG: SD_MMC not mounted / no card");
    sdOk = false;
    return;
  }

  sdOk = true;
  Serial.println("LOG: SD OK (already mounted), path=" + logPath);

  File f = SD_MMC.open(logPath, FILE_APPEND);
  if (f) f.close();
  else {
    Serial.println("LOG: open(/system.log) FAILED");
    sdOk = false;
  }
}

static String logPrefix() {
  uint32_t up = millis() / 1000;
  if (timeIsValid()) {
    return timeDateLocal() + " " + timeClockLocal() + " | " + String(up) + "s | ";
  }
  return String("--no-time-- | ") + String(up) + "s | ";
}

void logSystem(const String &msg) {
  String line = logPrefix() + msg;
  Serial.println(line);

  if (!sdOk) return;

  File f = SD_MMC.open(logPath, FILE_APPEND);
  if (!f) {
    Serial.println("LOG: open(/system.log) FAILED");
    sdOk = false;               // stoppa spam: markera SD trasig
    return;
  }
  f.println(line);
  f.close();
}
