#include "sdcard.h"

// SD is fully disabled.
// Reason: SD_MMC instability (0x109 / read_blocks failed) interfered with modem/GNSS timing.

bool sdcardInit() { return false; }
bool sdcardIsMounted() { return false; }
void sdcardDisable() {}
