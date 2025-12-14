#include "logging.h"
#include "time_manager.h"
#include "sdlog.h"

static String logPrefix() {
  uint32_t up = millis() / 1000;

  if (timeIsValid()) {
    // Lokal tid för läsbarhet (Stockholm), uptime för felsökning
    return timeDateLocal() + " " + timeClockLocal() + " | " + String(up) + "s | ";
  }
  return String("--no-time-- | ") + String(up) + "s | ";
}

// Behåll funktionen så annan kod inte går sönder,
// men den ska INTE mounta SD eller skriva filer själv längre.
void loggingInit() {
  // valfritt
  // Serial.println("LOG: loggingInit() (SD handled by SDLOG)");
}

void logSystem(const String &msg) {
  String line = logPrefix() + msg;

  // Serial är bra under utveckling
  Serial.println(line);

  // Skriv samma rad till SDLOG (som i sin tur skriver till SD-kortet)
  SDLOG.logLine(line);
}
