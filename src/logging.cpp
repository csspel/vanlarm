#include "logging.h"
#include "config.h"

#include <SD_MMC.h>

static bool   sdOk      = false;
static String logPath   = "/system.log";

void loggingInit() {
    Serial.println("LOG: init SD_MMC...");

    // Sätt pinnar för T-SIM7080G-S3
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);

    // 1-bit-läge, ingen autoformat
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("LOG: SD_MMC init FAILED");
        sdOk = false;
        return;
    }

    sdOk = true;
    Serial.println("LOG: SD_MMC init OK");

    // Skriv en enkel BOOT-marker i loggen
    File f = SD_MMC.open(logPath, FILE_APPEND);
    if (f) {
        f.println();
        f.println("===== BOOT =====");
        f.close();
    }
}

void logSystem(const String &msg) {
    // Lägg till uptime i sekunder i början av raden
    unsigned long sec = millis() / 1000;
    String line = String(sec) + "s " + msg;

    // Alltid till Serial
    Serial.println(line);

    if (!sdOk) {
        return;
    }

    File f = SD_MMC.open(logPath, FILE_APPEND);
    if (!f) {
        Serial.println("LOG: open(system.log) FAILED");
        return;
    }
    f.println(line);
    f.close();
}
