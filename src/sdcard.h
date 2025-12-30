#pragma once
#include <Arduino.h>

// SD is fully disabled in this build.
// Keep these symbols so the rest of the firmware compiles unchanged.

bool sdcardInit();      // always false
bool sdcardIsMounted(); // always false
void sdcardDisable();   // no-op
