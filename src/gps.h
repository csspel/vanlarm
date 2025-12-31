#pragma once
#include <Arduino.h>

struct GpsFix
{
  bool valid = false;
  String utc; // YYYYMMDDHHMMSS.sss (from CGNSINF)
  double lat = 0.0;
  double lon = 0.0;
  double alt_m = 0.0;
  double speed_kmh = 0.0;
  double course_deg = 0.0;
  uint32_t fix_age_ms = 0; // age since fix was read
  uint8_t fix_mode = 0;    // 1=2D,2=3D, etc (module dependent)

  // Diagnostik / TTFF
  uint16_t ttff_s = 0;    // time-to-first-fix (sek) för senaste wait
  uint8_t start_mode = 0; // 1=COLD, 2=WARM, 3=HOT
};

void gpsInit();
bool gpsPowerOn();
bool gpsPowerOff();
bool gpsIsOn();

bool gpsGetFixWait(GpsFix &out, uint32_t maxWaitMs = 30000);
bool gpsPollOnce(GpsFix &out);

bool gpsHasLastFix();
GpsFix gpsLastFix();
uint32_t gpsLastFixAtMs();
