#include "sdcard.h"
#include "config.h"

#include <Arduino.h>
#include <SD_MMC.h>

static bool s_mounted = false;

bool sdcardInit()
{
  if (s_mounted)
    return true;

  Serial.println("SD: init SD_MMC...");

  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);

  for (int attempt = 1; attempt <= 3; attempt++)
  {
    delay(300 * attempt); // enkel backoff: 300ms, 600ms, 900ms

    if (!SD_MMC.begin("/sdcard", true))
    {
      Serial.printf("SD: SD_MMC.begin FAILED (attempt %d/3)\n", attempt);
      SD_MMC.end();
      continue;
    }

    if (SD_MMC.cardType() == CARD_NONE)
    {
      Serial.printf("SD: No card attached (attempt %d/3)\n", attempt);
      SD_MMC.end();
      continue;
    }

    s_mounted = true;
    uint64_t sizeMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("SD: mounted OK, size=%lluMB\n", sizeMB);
    return true;
  }

  Serial.println("SD: mount failed after 3 attempts");
  s_mounted = false;
  return false;
}
