#include <Arduino.h>
#include "config.h"
#include "logging.h"
#include "power.h"
#include "modem.h"
#include "mqtt.h"
#include "profiles.h"
#include "esp_log.h"
#include "time_manager.h"

// ====== state machine för STEP 2 ======

enum class SystemState {
  BOOT,
  INIT_HW,
  IDLE,
  NET_CONNECT,
  MQTT_CONNECT,
  MQTT_ONLINE,
  MQTT_DISCONNECT,
  NET_DISCONNECT
};

static SystemState currentState   = SystemState::BOOT;
static unsigned long stateEnterMs = 0;

enum class CommReason {
  NONE,
  ALIVE
};
static CommReason currentReason = CommReason::NONE;

// ===== MQTT online-minfönster =====
static uint32_t mqttOnlineSince = 0;
static const uint32_t MQTT_MIN_ONLINE_MS = 15000;  // 15 sek
static bool aliveSentThisWindow = false;

// ===== Alive scheduler + retry backoff =====
static uint32_t nextAliveAtMs   = 0;
static uint32_t retryBackoffMs  = 0;

static const uint32_t ALIVE_PERIOD_MS = 120000;   // 2 min
static const uint32_t RETRY_MIN_MS    = 15000;    // 15 s
static const uint32_t RETRY_MAX_MS    = 120000;   // 2 min (max)

// Om nät-attach failar innan vi ens kommer till MQTT:
static const unsigned long NET_RETRY_INTERVAL_MS = 60000UL;

// ---- helpers ----
static const char *stateName(SystemState s) {
  switch (s) {
    case SystemState::BOOT:            return "BOOT";
    case SystemState::INIT_HW:         return "INIT_HW";
    case SystemState::IDLE:            return "IDLE";
    case SystemState::NET_CONNECT:     return "NET_CONNECT";
    case SystemState::MQTT_CONNECT:    return "MQTT_CONNECT";
    case SystemState::MQTT_ONLINE:     return "MQTT_ONLINE";
    case SystemState::MQTT_DISCONNECT: return "MQTT_DISCONNECT";
    case SystemState::NET_DISCONNECT:  return "NET_DISCONNECT";
    default:                           return "UNKNOWN";
  }
}

static void changeState(SystemState newState) {
  String msg = "STATE ";
  msg += stateName(currentState);
  msg += " -> ";
  msg += stateName(newState);
  logSystem(msg);

  currentState = newState;
  stateEnterMs = millis();
}

// Schemalägg nästa försök:
// - om retryBackoffMs>0: kör backoff
// - annars: normal period
static void scheduleNextAttempt(uint32_t nowMs) {
  uint32_t delayMs = (retryBackoffMs > 0) ? retryBackoffMs : ALIVE_PERIOD_MS;
  nextAliveAtMs = nowMs + delayMs;
}

static void bumpBackoff() {
  if (retryBackoffMs == 0) retryBackoffMs = RETRY_MIN_MS;
  else retryBackoffMs = min(retryBackoffMs * 2, RETRY_MAX_MS);
}

void setup() {
  Serial.begin(115200);

  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_req",    ESP_LOG_NONE);
  esp_log_level_set("sdmmc_cmd",    ESP_LOG_NONE);
  esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);

  delay(2000);

  Serial.println();
  Serial.println("=== Vanlarm V2 – STEP 2 (MQTT + alive) ===");

  loggingInit();
  timeInit();
  logSystem("BOOT: Vanlarm V2 STEP 2 starting");

  profilesInit(ProfileId::ALARM);
  logSystem(String("PROFILE: initial = ") + profileName(currentProfile().id));

  currentState = SystemState::BOOT;
  stateEnterMs = millis();
}

void loop() {
  const uint32_t now = millis();

  switch (currentState) {

    case SystemState::BOOT: {
      changeState(SystemState::INIT_HW);
      break;
    }

    case SystemState::INIT_HW: {
      logSystem("INIT_HW: powerInit()");
      if (!powerInit()) {
        logSystem("FATAL: PMU init failed");
        while (true) delay(1000);
      }

      logSystem("INIT_HW: modemInitUartAndPins()");
      modemInitUartAndPins();

      // Init MQTT-klientobjekt (skapar interna objekt)
      mqttSetup();

      // Första alive efter 2 sek
      nextAliveAtMs  = now + 2000;
      retryBackoffMs = 0;
      currentReason  = CommReason::NONE;

      changeState(SystemState::IDLE);
      break;
    }

    case SystemState::IDLE: {
      if ((int32_t)(now - nextAliveAtMs) >= 0) {
        logSystem("IDLE: alive timer due → start comm window");

        currentReason = CommReason::ALIVE;

        // Schemalägg nästa försök DIREKT så vi inte kan loopa
        scheduleNextAttempt(now);

        changeState(SystemState::NET_CONNECT);
      }
      break;
    }

    case SystemState::NET_CONNECT: {
      logSystem("NET_CONNECT: starting network attach");

      NetResult net;
      bool ok = modemConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS, net);

      if (ok) {
        //logSystem("NET_CONNECT: SUCCESS, T_net=" + String(now - stateEnterMs) +
        //          " ms, IP=" + net.ip + ", CSQ=" + String(net.csq));

        // Tidssync: modem först, sen NTP för validering/finjustering
        timeSyncFromModem();
        timeSyncFromNtp(8000);

        changeState(SystemState::MQTT_CONNECT);
      } else {
        logSystem("NET_CONNECT: FAIL, T_net=" + String(now - stateEnterMs) + " ms");

        // Om nätet failar: kör en lite snällare retry (1 minut), men låt även backoff gälla om den är aktiv
        if (retryBackoffMs == 0) {
          nextAliveAtMs = now + NET_RETRY_INTERVAL_MS;
        } else {
          nextAliveAtMs = now + retryBackoffMs;
        }

        changeState(SystemState::NET_DISCONNECT);
      }

      break;
    }

    case SystemState::MQTT_CONNECT: {
      logSystem("MQTT_CONNECT: connecting to broker");

      if (mqttConnect()) {
        mqttOnlineSince = now;
        aliveSentThisWindow = false;

        logSystem("MQTT_ONLINE: enter, holding window 15s");
        changeState(SystemState::MQTT_ONLINE);
      } else {
        bumpBackoff();
        logSystem("MQTT_CONNECT: FAILED, backoff_ms=" + String(retryBackoffMs));

        // Försök igen enligt backoff (överskriv tidigare schema)
        nextAliveAtMs = now + retryBackoffMs;

        changeState(SystemState::MQTT_DISCONNECT);
      }

      break;
    }

    case SystemState::MQTT_ONLINE: {
      // Måste loopa ofta för att hinna ta emot downlink
      mqttLoop();

      const uint32_t elapsed   = now - mqttOnlineSince;
      const uint32_t remaining = (elapsed < MQTT_MIN_ONLINE_MS) ? (MQTT_MIN_ONLINE_MS - elapsed) : 0;

      // Skicka ALIVE en gång per fönster
      if (!aliveSentThisWindow && currentReason == CommReason::ALIVE) {
        if (mqttPublishAlive()) {
          aliveSentThisWindow = true;

          // ✅ Framgång: nollställ backoff och återställ 2-minuters-cadence
          retryBackoffMs = 0;
          nextAliveAtMs  = now + ALIVE_PERIOD_MS;

          currentReason = CommReason::NONE;
          logSystem("ALIVE: sent OK, nextAliveAtMs set to +2min");
        } else {
          // Publish fail: börja/öka backoff, men håll kvar fönstret tills 15s passerat
          bumpBackoff();
          nextAliveAtMs = now + retryBackoffMs;
          logSystem("ALIVE: publish failed, backoff_ms=" + String(retryBackoffMs));
        }
      }

      // Logga var 5s (valfritt)
      static uint32_t lastLog = 0;
      if (now - lastLog > 5000) {
        logSystem("MQTT_ONLINE: window remaining " + String(remaining / 1000) + "s");
        lastLog = now;
      }

      // Håll online minst 15 sek
      if (elapsed < MQTT_MIN_ONLINE_MS) {
        break;
      }

      logSystem("MQTT_ONLINE: window elapsed, proceeding to disconnect");
      changeState(SystemState::MQTT_DISCONNECT);
      break;
    }

    case SystemState::MQTT_DISCONNECT: {
      mqttDisconnect();
      changeState(SystemState::NET_DISCONNECT);
      break;
    }

    case SystemState::NET_DISCONNECT: {
      logSystem("NET_DISCONNECT: end of comm window (no-op in STEP 2)");
      changeState(SystemState::IDLE);
      break;
    }
  }

  delay(10);
}
