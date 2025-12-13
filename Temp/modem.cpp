#include "modem.h"
#include "config.h"
#include "logging.h"

#include <TinyGsmClient.h>
#include <esp_task_wdt.h>

#define SerialAT Serial1

TinyGsm modem(SerialAT);

void modemSetupPins() {
  // Vi sätter bara pinmodes, men rör inte PWRKEY aktivt
  pinMode(PIN_MODEM_PWRKEY, OUTPUT);
  digitalWrite(PIN_MODEM_PWRKEY, LOW);

  if (PIN_MODEM_RESET >= 0) {
    pinMode(PIN_MODEM_RESET, OUTPUT);
    digitalWrite(PIN_MODEM_RESET, HIGH);  // inaktiv
  }
}

void modemPowerOn() {
  logSystem("MODEM: init serial & restart");

  // Starta UART mot modemet
  SerialAT.begin(115200, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);
  delay(300);

  uint32_t start = millis();
  bool ok = false;

  // Först ett snabbt test – ibland är modemet redan igång
  if (modem.testAT()) {
    ok = true;
    logSystem("MODEM: AT OK direkt utan restart()");
  } else {
    logSystem("MODEM: no AT, calling modem.restart()");
    modem.restart();

    const int maxTries = 10;
    for (int i = 1; i <= maxTries; i++) {
      esp_task_wdt_reset();
      delay(500);
      logSystem("MODEM: AT try " + String(i) + "/" + String(maxTries));

      if (modem.testAT()) {
        ok = true;
        break;
      }
    }
  }

  uint32_t elapsed = millis() - start;
  if (ok) {
    logSystem("MODEM: AT OK after " + String(elapsed) + " ms");
  } else {
    logSystem("MODEM: AT FAILED after " + String(elapsed) + " ms");
  }
}

bool modemConnectNetwork(uint32_t timeoutMs, uint32_t &elapsedMs, String &err) {
  uint32_t start = millis();
  err = "";

  logSystem("MODEM: waiting for network, timeout=" + String(timeoutMs) + " ms");

  uint32_t lastLog = 0;

  while ((millis() - start) < timeoutMs) {
    esp_task_wdt_reset();

    if (modem.isNetworkConnected()) {
      break;
    }

    modem.waitForNetwork(1);  // 1 s steg

    uint32_t elapsed = millis() - start;
    if (elapsed / 3000 != lastLog / 3000) {
      logSystem("MODEM: still waiting for network, t=" + String(elapsed) + " ms");
      lastLog = elapsed;
    }
  }

  if (!modem.isNetworkConnected()) {
    elapsedMs = millis() - start;
    err = "network attach timeout";
    logSystem("MODEM: network attach TIMEOUT efter " + String(elapsedMs) + " ms");
    return false;
  }

  logSystem("MODEM: network registered, opening PDP, APN=" + String(APN));

  // Enkelt PDP-upplägg med kort timeout
  uint32_t gprsStart = millis();
  while (!modem.isGprsConnected()) {
    esp_task_wdt_reset();

    if (modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
      break;
    }

    uint32_t elapsed = millis() - gprsStart;
    if (elapsed > 15000) {
      elapsedMs = millis() - start;
      err = "gprsConnect failed";
      logSystem("MODEM: gprsConnect FAILED efter " + String(elapsedMs) + " ms");
      return false;
    }

    logSystem("MODEM: gprsConnect retry...");
    delay(1000);
  }

  if (!modem.isGprsConnected()) {
    elapsedMs = millis() - start;
    err = "gprs not connected after gprsConnect";
    logSystem("MODEM: GPRS inte ansluten efter gprsConnect, t=" + String(elapsedMs) + " ms");
    return false;
  }

  elapsedMs = millis() - start;
  logSystem("MODEM: PDP up, IP=" + modem.localIP());
  return true;
}

void modemDisconnectNetwork() {
  if (modem.isGprsConnected()) {
    logSystem("MODEM: gprsDisconnect()");
    modem.gprsDisconnect();
  }
  logSystem("MODEM: radioOff()");
  modem.radioOff();
}

int modemGetSignal() {
  return modem.getSignalQuality();
}
