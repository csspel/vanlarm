#include <Arduino.h>
#include "config.h"
#include "logging.h"
#include "power.h"
#include "modem.h"
#include "mqtt.h"
#include "profiles.h"
#include "esp_log.h"

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

// Alive-scheduler
static unsigned long nextAliveMs  = 0;

// “kommunikationsfönster”: vad triggar det?
enum class CommReason {
  NONE,
  ALIVE
};
static CommReason currentReason = CommReason::NONE;

// hur ofta vi vill testa alive i STEP 2 (använder ALIVE_INTERVAL_MS från config.h)
static const unsigned long NET_RETRY_INTERVAL_MS = 60000UL;  // används om NET_CONNECT failar

const char *stateName(SystemState s) {
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

void changeState(SystemState newState) {
  String msg = "STATE ";
  msg += stateName(currentState);
  msg += " -> ";
  msg += stateName(newState);
  logSystem(msg);

  currentState = newState;
  stateEnterMs = millis();
}

void setup() {
  Serial.begin(115200);

  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_req",    ESP_LOG_NONE);
  esp_log_level_set("sdmmc_cmd",    ESP_LOG_NONE);
  esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);

  // Vänta lite så loggen blir läsbar, men blockera inte om ingen USB sitter i
  delay(2000);

  Serial.println();
  Serial.println("=== Vanlarm V2 – STEP 2 (MQTT + alive) ===");

  // SD-logg
  loggingInit();
  logSystem("BOOT: Vanlarm V2 STEP 2 starting");

  // start i BOOT
  currentState   = SystemState::BOOT;
  stateEnterMs   = millis();
  //nextAliveMs    = millis() + ALIVE_INTERVAL_MS;  // första alive om ALIVE_INTERVAL_MS
  currentReason  = CommReason::NONE;
  profilesInit(ProfileId::ALARM);      // t.ex. PARKED som default när vanen startar
  logSystem(String("PROFILE: initial = ") + profileName(currentProfile().id));
}

void loop() {
  unsigned long now = millis();

  switch (currentState) {

    case SystemState::BOOT: {
      changeState(SystemState::INIT_HW);
      break;
    }

    case SystemState::INIT_HW: {
      logSystem("INIT_HW: powerInit()");
      if (!powerInit()) {
        logSystem("FATAL: PMU init failed");
        while (true) {
          delay(1000);
        }
      }

      logSystem("INIT_HW: modemInitUartAndPins()");
      modemInitUartAndPins();
      nextAliveMs   = millis();          // trigga första direkt
      currentReason = CommReason::ALIVE; // så vi vet varför
      changeState(SystemState::IDLE);


      // För säkerhets skull: init MQTT-klient (bara interna objekt)
      mqttSetup();

      changeState(SystemState::IDLE);
      break;
    }

    case SystemState::IDLE: {
      // Alive-timer
      if ((long)(now - nextAliveMs) >= 0) {
        logSystem("IDLE: alive timer due → start comm window");
        currentReason = CommReason::ALIVE;
        changeState(SystemState::NET_CONNECT);
      }

      // här senare: andra triggers (PIR-larm, profilbyte, etc.)

      break;
    }

    case SystemState::NET_CONNECT: {
      logSystem("NET_CONNECT: starting network attach");

      NetResult net;
      bool ok = modemConnectData(APN,
                                 NET_REG_TIMEOUT_MS,
                                 DATA_ATTACH_TIMEOUT_MS,
                                 net);

      if (ok) {
        logSystem("NET_CONNECT: SUCCESS, T_net=" + String(millis() - stateEnterMs) +
                  " ms, IP=" + net.ip + ", CSQ=" + String(net.csq));
        changeState(SystemState::MQTT_CONNECT);
      } else {
        logSystem("NET_CONNECT: FAIL, T_net=" + String(millis() - stateEnterMs) + " ms");

        // planera om alive om det var det som trigga
        if (currentReason == CommReason::ALIVE) {
          nextAliveMs = now + NET_RETRY_INTERVAL_MS;
          currentReason = CommReason::NONE;
        }

        changeState(SystemState::NET_DISCONNECT);
      }

      break;
    }

    case SystemState::MQTT_CONNECT: {
      logSystem("MQTT_CONNECT: connecting to broker");

      if (mqttConnect()) {
        changeState(SystemState::MQTT_ONLINE);
      } else {
        logSystem("MQTT_CONNECT: FAILED");
        changeState(SystemState::MQTT_DISCONNECT);
      }

      break;
    }

    case SystemState::MQTT_ONLINE: {
      // I STEP 2: vi gör bara alive-skick direkt när vi kommer in här
      if (currentReason == CommReason::ALIVE) {
        if (mqttPublishAlive()) {
          // alive lyckades → planera nästa
          nextAliveMs = now + ALIVE_INTERVAL_MS;
        } else {
          // alive misslyckades → försök igen senare
          nextAliveMs = now + NET_RETRY_INTERVAL_MS;
        }
        currentReason = CommReason::NONE;
      }

      // Här skulle man kunna lyssna på SUB-topics om vi hade några.
      // För STEP 2 gör vi inget mer: avsluta fönstret.
      changeState(SystemState::MQTT_DISCONNECT);

      break;
    }

    case SystemState::MQTT_DISCONNECT: {
      mqttDisconnect();
      changeState(SystemState::NET_DISCONNECT);
      break;
    }

    case SystemState::NET_DISCONNECT: {
      // I STEP 2 gör vi fortfarande inget aktivt (ingen CFUN=0 etc),
      // bara loggar att fönstret är slut.
      logSystem("NET_DISCONNECT: end of comm window (no-op in STEP 2)");

      changeState(SystemState::IDLE);
      break;
    }
  }

  mqttLoop();    // i framtiden: hålla SUB-connection aktiv under window
  delay(10);
}
