#pragma once
#include <Arduino.h>

struct GpsFix {
  bool   valid = false;
  String utc;          // YYYYMMDDHHMMSS.sss (from CGNSINF)
  double lat  = 0.0;
  double lon  = 0.0;
  double alt_m = 0.0;
  double speed_kmh = 0.0;
  double course_deg = 0.0;
  uint32_t fix_age_ms = 0;   // age since fix was read
  uint8_t fix_mode = 0;      // 1=2D,2=3D, etc (module dependent)
};

void gpsInit();
bool gpsPowerOn();
bool gpsPowerOff();
bool gpsIsOn();

// Wait up to maxWaitMs for a *valid* fix. Returns true if fix.valid==true.
bool gpsGetFixWait(GpsFix &out, uint32_t maxWaitMs = 30000);

// Non-blocking single poll (still uses a short AT timeout internally)
bool gpsPollOnce(GpsFix &out);

// Access last fix cached in gps.cpp (thread-safe enough for loop usage)
bool gpsHasLastFix();
GpsFix gpsLastFix();
uint32_t gpsLastFixAtMs();
