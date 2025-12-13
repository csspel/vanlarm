#include "power.h"
#include "config.h"
#include "logging.h"

#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static XPowersPMU PMU;

bool powerInit() {
  logSystem("PMU: init...");

  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, BOARD_I2C_SDA, BOARD_I2C_SCL)) {
      logSystem("PMU: FAILED to initialize");
      return false;
  }

  // DC3 = modemets huvudmatning
  PMU.setDC3Voltage(3000);
  PMU.enableDC3();

  // BLDO2 = GPS-antenna power
  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();

  // TS-pin utan NTC -> stäng av mätning
  PMU.disableTSPinMeasure();

  logSystem("PMU: modem power rails ON");
  return true;
}
