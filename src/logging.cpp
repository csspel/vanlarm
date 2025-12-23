// logging.cpp - SD-only logging via SD_MMC (single mount via sdcardInit)

#include "logging.h"
#include "sdcard.h"
#include "time_manager.h"
#include "config.h"

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>

static bool s_sdOk = false;
static String s_logPath = "/system.log";
static bool s_sdWriteEnabled = false;

static String makePrefix()
{
  // Format: "YYYY-MM-DD HH:MM:SS | 4s | "
  uint32_t upS = millis() / 1000;

  String ts;
  if (timeIsValid())
  {
    ts = timeDateLocal() + " " + timeClockLocal();
  }
  else
  {
    ts = String("--no-time--");
  }

  String p;
  p.reserve(48);
  p += ts;
  p += " | ";
  p += String(upS);
  p += "s | ";
  return p;
}

void loggingInit()
{
  // Mount SD en gång (single source of truth)
  bool sdOk = sdcardInit();
  s_sdOk = sdOk;

  if (!sdOk)
  {
    logSystem("SD init FAILED – running without SD");

    if (REQUIRE_SD)
    {
      logSystem("SD required -> HALT");
      while (true)
      {
        delay(1000);
      }
    }
  }
  else
  {
    logSystem("SD init OK");
  }

  // Verifiera att vi kan öppna loggfilen
  File f = SD_MMC.open(s_logPath.c_str(), FILE_APPEND);
  if (!f)
  {
    Serial.println("LOG: SD mounted but cannot open /system.log for append.");
    s_sdOk = false; // disable SD logging

    if (REQUIRE_SD)
    {
      Serial.println("LOG: SD required -> HALT.");
      while (true)
        delay(1000);
    }

    // fortsätt utan SD
    return;
  }

  f.println(makePrefix() + "LOG: start, path=" + s_logPath);
  f.flush();
  f.close();

  Serial.println("LOG: SD OK (already mounted), path=" + s_logPath);
  s_sdWriteEnabled = s_sdOk;
}

// bool loggingSdOk() {
//   return s_sdOk;
// }

// const char* loggingPath() {
//   return s_logPath.c_str();
// }

void logSystem(const String &msg)
{
  String line = makePrefix() + msg;

  // Alltid Serial
  Serial.println(line);

  // SD om ok
  if (!s_sdOk)
    return;

  File f = SD_MMC.open(s_logPath.c_str(), FILE_APPEND);
  if (!f)
  {
    // SD-only: markera tydligt i Serial om SD plötsligt slutar fungera
    Serial.println(makePrefix() + "LOG: SD open FAILED for append");
    return;
  }
  f.println(line);
  f.flush();
  f.close();
}
