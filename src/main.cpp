// main.cpp (STEP 2) - SD-only via logging.cpp
// Features:
//  - Catch-up alive (if overdue, send immediately when online)
//  - Quick MQTT reconnect inside the same comm window
//  - Reason codes on state transitions
//  - RF policy: keep RF ON in TRAVEL + STOLEN

#include <Arduino.h>

#include "config.h"
#include "power.h"
#include "modem.h"
#include "mqtt.h"
#include "profiles.h"
#include "time_manager.h"
#include "logging.h"
#include "sdcard.h"
#include "esp_log.h"

// ================= RF-policy =================
static bool shouldKeepRfOn(ProfileId p) {
  switch (p) {
    case ProfileId::TRAVEL:
    case ProfileId::STOLEN:
      return true;   // stabilitet + snabb återhämtning
    default:
      return false;  // ALARM / PARKED kan spara ström
  }
}

// ================= State machine =================
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

static SystemState currentState = SystemState::BOOT;
static uint32_t    stateEnterMs = 0;

// ================= Reason codes =================
enum class ReasonCode {
  NONE,
  BOOT_START,
  TIMER_ALIVE_DUE,
  NET_ATTACH_OK,
  NET_ATTACH_FAIL,
  MQTT_CONN_OK,
  MQTT_CONN_FAIL,
  MQTT_LOST_RECONNECTING,
  MQTT_RECONNECT_OK,
  MQTT_RECONNECT_FAIL,
  ALIVE_SENT,
  ALIVE_PUBLISH_FAIL,
  COMM_WINDOW_MIN_HOLD_DONE,
  COMM_WINDOW_MAX_EXPIRED,
  BACKOFF_SET
};

static const char* reasonName(ReasonCode r) {
  switch (r) {
    case ReasonCode::NONE:                   return "NONE";
    case ReasonCode::BOOT_START:             return "BOOT_START";
    case ReasonCode::TIMER_ALIVE_DUE:        return "TIMER_ALIVE_DUE";
    case ReasonCode::NET_ATTACH_OK:          return "NET_ATTACH_OK";
    case ReasonCode::NET_ATTACH_FAIL:        return "NET_ATTACH_FAIL";
    case ReasonCode::MQTT_CONN_OK:           return "MQTT_CONN_OK";
    case ReasonCode::MQTT_CONN_FAIL:         return "MQTT_CONN_FAIL";
    case ReasonCode::MQTT_LOST_RECONNECTING: return "MQTT_LOST_RECONNECTING";
    case ReasonCode::MQTT_RECONNECT_OK:      return "MQTT_RECONNECT_OK";
    case ReasonCode::MQTT_RECONNECT_FAIL:    return "MQTT_RECONNECT_FAIL";
    case ReasonCode::ALIVE_SENT:             return "ALIVE_SENT";
    case ReasonCode::ALIVE_PUBLISH_FAIL:     return "ALIVE_PUBLISH_FAIL";
    case ReasonCode::COMM_WINDOW_MIN_HOLD_DONE: return "COMM_WINDOW_MIN_HOLD_DONE";
    case ReasonCode::COMM_WINDOW_MAX_EXPIRED:   return "COMM_WINDOW_MAX_EXPIRED";
    case ReasonCode::BACKOFF_SET:            return "BACKOFF_SET";
    default:                                 return "UNKNOWN";
  }
}

static const char* stateName(SystemState s) {
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

// ================= Logging helpers =================
// sekvensräknare så du ser om rader tappas
static uint32_t logSeq = 0;

static inline void LOG(const String& s) {
  // logSystem() prefixar redan datum/tid + uptime.
  // Vi lägger på "SEQ | STATE |" i själva msg:et.
  String line;
  line.reserve(180);
  line += String(++logSeq);
  line += " | ";
  line += stateName(currentState);
  line += " | ";
  line += s;
  logSystem(line);
}

static void changeState(SystemState newState, ReasonCode reason = ReasonCode::NONE, const String& extra = "") {
  String msg;
  msg.reserve(160);
  msg += "STATE ";
  msg += stateName(currentState);
  msg += " -> ";
  msg += stateName(newState);
  msg += " reason=";
  msg += reasonName(reason);
  if (extra.length() > 0) {
    msg += " ";
    msg += extra;
  }

  LOG(msg);
  currentState = newState;
  stateEnterMs = millis();
}

// ================= Alive scheduler + backoff =================
static uint32_t aliveDueAtMs   = 0;  // deadline, catch-up om overdue
static uint32_t retryBackoffMs = 0;

static const uint32_t ALIVE_PERIOD_MS = 120000UL; // 2 min
static const uint32_t RETRY_MIN_MS    = 15000UL;  // 15 s
static const uint32_t RETRY_MAX_MS    = 120000UL; // 2 min
static const uint32_t NET_RETRY_INTERVAL_MS = 60000UL;

static void bumpBackoff() {
  if (retryBackoffMs == 0) retryBackoffMs = RETRY_MIN_MS;
  else retryBackoffMs = min<uint32_t>(retryBackoffMs * 2, RETRY_MAX_MS);
}

// ================= Comm window =================
static uint32_t mqttOnlineSince    = 0;
static uint32_t commWindowEndsAtMs = 0;

static const uint32_t MQTT_MIN_ONLINE_MS = 15000UL; // 15 s downlink-fönster
static const uint32_t COMM_WINDOW_MAX_MS = 60000UL; // max tid per försök

// ================= MQTT quick reconnect policy =================
static const uint8_t  MQTT_RECONNECT_TRIES    = 5;
static const uint32_t MQTT_RECONNECT_DELAY_MS = 2000UL;

static bool mqttQuickReconnect(uint32_t nowMs) {
  for (uint8_t i = 1; i <= MQTT_RECONNECT_TRIES; i++) {
    if (nowMs >= commWindowEndsAtMs) {
      LOG("MQTT: quickReconnect abort (comm window expired)");
      return false;
    }

    LOG("MQTT: quickReconnect attempt " + String(i) + "/" + String(MQTT_RECONNECT_TRIES));

    if (mqttConnect()) {
      LOG("MQTT: quickReconnect OK");
      return true;
    }

    delay(MQTT_RECONNECT_DELAY_MS);
    nowMs = millis();
  }
  return false;
}

// ================= setup/loop =================
void setup() {
  Serial.begin(115200);

  // Stäng av ESP-IDF sdmmc spam i terminaln
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_req",    ESP_LOG_NONE);
  esp_log_level_set("sdmmc_cmd",    ESP_LOG_NONE);
  esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);

  delay(2000);
  Serial.println();
  Serial.println("=== Vanlarm V2 – STEP 2 (MQTT + alive) ===");

  timeInit();
  profilesInit(ProfileId::ALARM);

  currentState = SystemState::BOOT;
  stateEnterMs = millis();

  // första alive efter 2s
  aliveDueAtMs   = millis() + 2000;
  retryBackoffMs = 0;
}

void loop() {
  const uint32_t now = millis();

  switch (currentState) {

    case SystemState::BOOT: {
      changeState(SystemState::INIT_HW, ReasonCode::BOOT_START);
      break;
    }

    case SystemState::INIT_HW: {
      LOG("INIT_HW: powerInit()");
      if (!powerInit()) {
        LOG("FATAL: PMU init failed");
        while (true) delay(1000);
      }
      if (!sdcardInit()) {
       LOG("FATAL: SD init failed");
       while (true) delay(1000);
      }

      // SD-only: mount + create /system.log (logging.cpp)
      LOG("INIT_HW: loggingInit() (SD_MMC mount + /system.log)");
      loggingInit();

      LOG(String("PROFILE: initial = ") + profileName(currentProfile().id));

      LOG("INIT_HW: modemInitUartAndPins()");
      modemInitUartAndPins();

      mqttSetup();

      changeState(SystemState::IDLE);
      break;
    }

    case SystemState::IDLE: {
      // Trigger comm window när alive är due (eller overdue)
      if ((int32_t)(now - aliveDueAtMs) >= 0) {
        int32_t overdue = (int32_t)(now - aliveDueAtMs);
        LOG("IDLE: alive due -> start comm window overdueMs=" + String(overdue));
        changeState(SystemState::NET_CONNECT, ReasonCode::TIMER_ALIVE_DUE,
                    String("aliveOverdueMs=") + String(overdue));
      }
      break;
    }

    case SystemState::NET_CONNECT: {
      LOG("NET_CONNECT: starting network attach");

      // RF ON inför attach (idempotent)
      modemRfOn();

      NetResult net;
      bool ok = modemConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS, net);

      if (ok) {
        timeSyncFromModem();
        timeSyncFromNtp(8000);

        changeState(SystemState::MQTT_CONNECT, ReasonCode::NET_ATTACH_OK);
      } else {
        LOG("NET_CONNECT: FAIL, T_net=" + String(now - stateEnterMs) + " ms");

        if (retryBackoffMs == 0) aliveDueAtMs = now + NET_RETRY_INTERVAL_MS;
        else aliveDueAtMs = now + retryBackoffMs;

        changeState(SystemState::NET_DISCONNECT, ReasonCode::NET_ATTACH_FAIL,
                    String("nextAttemptMs=") + String((uint32_t)(aliveDueAtMs - now)));
      }
      break;
    }

    case SystemState::MQTT_CONNECT: {
      LOG("MQTT_CONNECT: connecting to broker");

      if (mqttConnect()) {
        mqttOnlineSince    = now;
        commWindowEndsAtMs = now + COMM_WINDOW_MAX_MS;

        changeState(SystemState::MQTT_ONLINE, ReasonCode::MQTT_CONN_OK);
      } else {
        bumpBackoff();
        aliveDueAtMs = now + retryBackoffMs;

        changeState(SystemState::MQTT_DISCONNECT, ReasonCode::MQTT_CONN_FAIL,
                    String("backoffMs=") + String(retryBackoffMs));
      }
      break;
    }

    case SystemState::MQTT_ONLINE: {
      // pumpa callbacks (downlink)
      mqttLoop();

      const uint32_t elapsedMin = now - mqttOnlineSince;
      const bool minHoldDone    = (elapsedMin >= MQTT_MIN_ONLINE_MS);
      const bool windowExpired  = (now >= commWindowEndsAtMs);

      // C) Om MQTT tappat: reconnect i samma comm window
      if (!mqttIsConnected()) {
        LOG("MQTT: lost connection -> quick reconnect in same window");
        changeState(SystemState::MQTT_ONLINE, ReasonCode::MQTT_LOST_RECONNECTING);

        bool recOk = mqttQuickReconnect(millis());
        if (recOk) {
          // Efter reconnect: publicera version igen (mqttConnect gör det redan)
          // och fortsätt i ONLINE
          // (Vi byter inte state här för att slippa “STATE ONLINE -> ONLINE”-spam,
          //  men reason codes finns i loggen ovan.)
          LOG("MQTT: reconnect OK");
        } else {
          bumpBackoff();
          aliveDueAtMs = millis() + retryBackoffMs;
          changeState(SystemState::MQTT_DISCONNECT, ReasonCode::MQTT_RECONNECT_FAIL,
                      String("backoffMs=") + String(retryBackoffMs));
          break;
        }
      }

      // B) Catch-up alive: om overdue och online -> skicka direkt
      if ((int32_t)(now - aliveDueAtMs) >= 0) {
        if (mqttPublishAlive()) {
          retryBackoffMs = 0;
          aliveDueAtMs   = now + ALIVE_PERIOD_MS;
          LOG("ALIVE: sent OK (catch-up), next alive in 2min");
        } else {
          LOG("ALIVE: publish failed -> try quick reconnect then retry publish");

          bool recOk = mqttQuickReconnect(millis());
          if (recOk && mqttPublishAlive()) {
            retryBackoffMs = 0;
            aliveDueAtMs   = millis() + ALIVE_PERIOD_MS;
            LOG("ALIVE: sent OK after reconnect, next alive in 2min");
          } else {
            bumpBackoff();
            aliveDueAtMs = millis() + retryBackoffMs;
            LOG("ALIVE: still failing, backoffMs=" + String(retryBackoffMs));
          }
        }
      }

      // statuslogg var 5s
      static uint32_t lastStat = 0;
      if (now - lastStat > 5000) {
        uint32_t remMin = (elapsedMin < MQTT_MIN_ONLINE_MS) ? (MQTT_MIN_ONLINE_MS - elapsedMin) : 0;
        uint32_t remMax = (now < commWindowEndsAtMs) ? (commWindowEndsAtMs - now) : 0;
        LOG("MQTT_ONLINE: minHoldRem=" + String(remMin / 1000) + "s, windowRem=" + String(remMax / 1000) + "s");
        lastStat = now;
      }

      // max fönster: bryt oavsett
      if (windowExpired) {
        changeState(SystemState::MQTT_DISCONNECT, ReasonCode::COMM_WINDOW_MAX_EXPIRED);
        break;
      }

      // håll minst 15s
      if (!minHoldDone) break;

      // min hold klar -> vi kan gå ur fönstret
      changeState(SystemState::MQTT_DISCONNECT, ReasonCode::COMM_WINDOW_MIN_HOLD_DONE);
      break;
    }

    case SystemState::MQTT_DISCONNECT: {
      mqttDisconnect();
      changeState(SystemState::NET_DISCONNECT);
      break;
    }

    case SystemState::NET_DISCONNECT: {
      LOG("NET_DISCONNECT: end of comm window");

      // RF-policy per profil
      ProfileId p = currentProfile().id;
      if (shouldKeepRfOn(p)) {
        LOG(String("MODEM: keep RF ON in profile=") + profileName(p));
        // ingen modemRfOff()
      } else {
        modemRfOff();
      }

      if (retryBackoffMs > 0) {
        LOG("BACKOFF: active backoffMs=" + String(retryBackoffMs));
      }

      changeState(SystemState::IDLE);
      break;
    }
  }

  delay(10);
}
