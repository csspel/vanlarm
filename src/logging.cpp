#include "logging.h"
#include "config.h"
#include "time_manager.h"

#include <SD_MMC.h>

static bool   sdOk    = false;
static String logPath = "/system.log";

void loggingInit() {
  Serial.println("LOG: init SD_MMC...");

  // Sätt pinnar för T-SIM7080G-S3
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);

  // 1-bit-läge, ingen autoformat
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("LOG: SD_MMC.begin FAILED (no SD?)");
    sdOk = false;
    return;
  }

  sdOk = true;
  Serial.println("LOG: SD OK, path=" + logPath);

  // Skapa fil om den inte finns
  File f = SD_MMC.open(logPath, FILE_APPEND);
  if (f) f.close();
  else Serial.println("LOG: open(system.log) FAILED");
}

static String logPrefix() {
  uint32_t up = millis() / 1000;

  if (timeIsValid()) {
    // Lokal tid för läsbarhet (Stockholm), uptime för felsökning
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
    Serial.println("LOG: open(system.log) FAILED");
    return;
  }
  f.println(line);
  f.close();
}
