// main.cpp (STEP 2) - COMM scheduler + GPS scheduler (GNSS <-> LTE time-mux)

#include <Arduino.h>
#include "config.h"
#include "power.h"
#include "modem.h"
#include "mqtt.h"
#include "gps.h"
#include "profiles.h"
#include "time_manager.h"
#include "logging.h"
#include "esp_log.h"

// ================= State machine =================
enum class SystemState
{
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
static uint32_t stateEnterMs = 0;

// ================= Reason codes =================
enum class ReasonCode
{
  NONE,
  BOOT_START,
  TIMER_COMM_DUE,
  NET_ATTACH_OK,
  NET_ATTACH_FAIL,
  MQTT_CONN_OK,
  MQTT_CONN_FAIL,
  MQTT_LOST_RECONNECTING,
  MQTT_RECONNECT_OK,
  MQTT_RECONNECT_FAIL,
  COMM_PUBLISH_OK,
  COMM_PUBLISH_FAIL,
  COMM_WINDOW_MIN_HOLD_DONE,
  COMM_WINDOW_MAX_EXPIRED,
  BACKOFF_SET
};

static const char *reasonName(ReasonCode r)
{
  switch (r)
  {
  case ReasonCode::NONE:
    return "NONE";
  case ReasonCode::BOOT_START:
    return "BOOT_START";
  case ReasonCode::TIMER_COMM_DUE:
    return "TIMER_COMM_DUE";
  case ReasonCode::NET_ATTACH_OK:
    return "NET_ATTACH_OK";
  case ReasonCode::NET_ATTACH_FAIL:
    return "NET_ATTACH_FAIL";
  case ReasonCode::MQTT_CONN_OK:
    return "MQTT_CONN_OK";
  case ReasonCode::MQTT_CONN_FAIL:
    return "MQTT_CONN_FAIL";
  case ReasonCode::MQTT_LOST_RECONNECTING:
    return "MQTT_LOST_RECONNECTING";
  case ReasonCode::MQTT_RECONNECT_OK:
    return "MQTT_RECONNECT_OK";
  case ReasonCode::MQTT_RECONNECT_FAIL:
    return "MQTT_RECONNECT_FAIL";
  case ReasonCode::COMM_PUBLISH_OK:
    return "COMM_PUBLISH_OK";
  case ReasonCode::COMM_PUBLISH_FAIL:
    return "COMM_PUBLISH_FAIL";
  case ReasonCode::COMM_WINDOW_MIN_HOLD_DONE:
    return "COMM_WINDOW_MIN_HOLD_DONE";
  case ReasonCode::COMM_WINDOW_MAX_EXPIRED:
    return "COMM_WINDOW_MAX_EXPIRED";
  case ReasonCode::BACKOFF_SET:
    return "BACKOFF_SET";
  default:
    return "UNKNOWN";
  }
}

static const char *stateName(SystemState s)
{
  switch (s)
  {
  case SystemState::BOOT:
    return "BOOT";
  case SystemState::INIT_HW:
    return "INIT_HW";
  case SystemState::IDLE:
    return "IDLE";
  case SystemState::NET_CONNECT:
    return "NET_CONNECT";
  case SystemState::MQTT_CONNECT:
    return "MQTT_CONNECT";
  case SystemState::MQTT_ONLINE:
    return "MQTT_ONLINE";
  case SystemState::MQTT_DISCONNECT:
    return "MQTT_DISCONNECT";
  case SystemState::NET_DISCONNECT:
    return "NET_DISCONNECT";
  default:
    return "UNKNOWN";
  }
}

// ================= Logging wrapper =================
static uint32_t logSeq = 0;

static inline void LOG(const String &s)
{
  String line;
  line.reserve(200);
  line += String(++logSeq);
  line += " | ";
  line += stateName(currentState);
  line += " | ";
  line += s;
  logSystem(line);
}

static void changeState(SystemState newState, ReasonCode reason = ReasonCode::NONE, const String &extra = "")
{
  String msg = "STATE ";
  msg += stateName(currentState);
  msg += " -> ";
  msg += stateName(newState);
  msg += " reason=";
  msg += reasonName(reason);
  if (extra.length() > 0)
  {
    msg += " ";
    msg += extra;
  }
  LOG(msg);

  currentState = newState;
  stateEnterMs = millis();
}

// ================= Schedulers (COMM + GPS) =================
static uint32_t commDueAtMs = 0; // next comm window
static uint32_t gpsDueAtMs = 0;  // next GPS sample time

// MQTT publish retry backoff (only used inside a comm window)
static uint32_t retryBackoffMs = 0;
static const uint32_t RETRY_MIN_MS = 15000UL;
static const uint32_t RETRY_MAX_MS = 120000UL;

static void bumpBackoff()
{
  if (retryBackoffMs == 0)
    retryBackoffMs = RETRY_MIN_MS;
  else
    retryBackoffMs = min<uint32_t>(retryBackoffMs * 2, RETRY_MAX_MS);
}

// NET attach backoff
static uint8_t netFailCount = 0;
static uint32_t netNextAttemptAtMs = 0;

static const uint8_t NET_FAILS_BEFORE_POWERCYCLE = 3;
static const uint32_t NET_BACKOFF_MIN_MS = 60000UL;  // 1 min
static const uint32_t NET_BACKOFF_MAX_MS = 900000UL; // 15 min
static uint32_t netBackoffMs = 0;

static void bumpNetBackoff()
{
  if (netBackoffMs == 0)
    netBackoffMs = NET_BACKOFF_MIN_MS;
  else
    netBackoffMs = min<uint32_t>(netBackoffMs * 2, NET_BACKOFF_MAX_MS);
}

static void clearNetBackoff()
{
  netBackoffMs = 0;
  netFailCount = 0;
  netNextAttemptAtMs = 0;
}

// Profile periods
static uint32_t commPeriodFor(ProfileId id)
{
  switch (id)
  {
  case ProfileId::TRAVEL:
    return 300000UL; // 5 min
  case ProfileId::STOLEN:
    return 120000UL; // 2 min
  case ProfileId::ALARM:
    return 300000UL; // 5 min
  case ProfileId::PARKED:
    return 300000UL; // 5 min
  default:
    return 300000UL;
  }
}

static uint32_t gpsPeriodFor(ProfileId id)
{
  switch (id)
  {
  case ProfileId::TRAVEL:
    return 10000UL; // 10 s
  case ProfileId::STOLEN:
    return 120000UL; // 1 fix per uplink period (du bad om det)
  case ProfileId::ALARM:
    return 300000UL; // 5 min
  case ProfileId::PARKED:
    return 300000UL; // 5 min
  default:
    return 60000UL;
  }
}

// Max wait for a good fix (you requested 30s)
static const uint32_t GPS_FIX_WAIT_MS = 30000UL;

static void scheduleNextComm(uint32_t nowMs)
{
  uint32_t period = commPeriodFor(currentProfile().id);
  // jitter +/- 10s to avoid "same minute each hour" coupling
  int32_t jitter = random(-10000, 10000);
  int64_t next = (int64_t)nowMs + (int64_t)period + (int64_t)jitter;
  if (next < (int64_t)nowMs + 1000)
    next = (int64_t)nowMs + 1000; // never schedule in the past
  commDueAtMs = (uint32_t)next;
}

static void scheduleNextGps(uint32_t nowMs)
{
  gpsDueAtMs = nowMs + gpsPeriodFor(currentProfile().id);
}

// ================= Comm window timing =================
static uint32_t mqttOnlineSince = 0;
static uint32_t commWindowEndsAtMs = 0;

static const uint32_t MQTT_MIN_ONLINE_MS = 15000UL; // keep online for downlink window
static const uint32_t COMM_WINDOW_MAX_MS = 60000UL; // hard cap per attempt

// publish-once flag per comm window
static bool commPublishedThisWindow = false;

// ================= MQTT quick reconnect policy =================
static const uint8_t MQTT_RECONNECT_TRIES = 5;
static const uint32_t MQTT_RECONNECT_DELAY_MS = 2000;

static bool mqttQuickReconnect(uint32_t nowMs)
{
  for (uint8_t i = 1; i <= MQTT_RECONNECT_TRIES; i++)
  {
    if (nowMs >= commWindowEndsAtMs)
    {
      LOG("MQTT: quickReconnect abort (comm window expired)");
      return false;
    }

    LOG("MQTT: quickReconnect attempt " + String(i) + "/" + String(MQTT_RECONNECT_TRIES));

    if (mqttConnect())
    {
      LOG("MQTT: quickReconnect OK");
      return true;
    }

    delay(MQTT_RECONNECT_DELAY_MS);
    nowMs = millis();
  }
  return false;
}

// ================= setup/loop =================
void setup()
{
  Serial.begin(115200);

  // stop sdmmc spam in serial
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
  esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);

  delay(2000);
  Serial.println();
  Serial.println("=== Vanlarm V2 – STEP 2 (COMM + GPS time-mux) ===");

  timeInit();
  profilesInit(ProfileId::ALARM);

  currentState = SystemState::BOOT;
  stateEnterMs = millis();

  // Seed random for jitter
  randomSeed(esp_random());

  // initial schedules
  commDueAtMs = millis() + 2000;
  gpsDueAtMs = millis() + 5000;
}

void loop()
{
  const uint32_t now = millis();

  switch (currentState)
  {

  case SystemState::BOOT:
  {
    changeState(SystemState::INIT_HW, ReasonCode::BOOT_START);
    break;
  }

  case SystemState::INIT_HW:
  {
    LOG("INIT_HW: powerInit()");
    if (!powerInit())
    {
      LOG("FATAL: PMU init failed");
      while (true)
        delay(1000);
    }

    LOG("INIT_HW: loggingInit() (SD_MMC mount + /system.log)");
    loggingInit();

    LOG(String("PROFILE: initial = ") + profileName(currentProfile().id));

    LOG("INIT_HW: modemInitUartAndPins()");
    modemInitUartAndPins();

    mqttSetup();

    // ensure we start with RF off (we want GNSS between comm windows)
    modemRfOff();
    gpsPowerOff(); // start clean; gpsGetFixWait will power on as needed

    changeState(SystemState::IDLE);
    break;
  }

  case SystemState::IDLE:
  {
    // If comm is due soon/now, prioritize comm (we will power off GNSS anyway)
    const bool commIsDue = ((int32_t)(now - commDueAtMs) >= 0);

    // 1) GPS sampling (only when comm is NOT due)
    if (!commIsDue && (int32_t)(now - gpsDueAtMs) >= 0)
    {
      modemRfOff(); // ensure RF is OFF while using GNSS
      GpsFix fx;
      bool ok = gpsGetFixWait(fx, GPS_FIX_WAIT_MS);
      LOG(String("GPS: sample ") + (ok ? "OK" : "FAIL") +
          " valid=" + String(fx.valid ? "true" : "false") +
          (fx.valid ? (String(" lat=") + String(fx.lat, 6) + " lon=" + String(fx.lon, 6)) : ""));

      // TRAVEL can keep GNSS running for faster fixes; others can power off
      if (currentProfile().id != ProfileId::TRAVEL)
      {
        gpsPowerOff();
      }

      scheduleNextGps(now);
    }

    // 2) COMM window
    if (commIsDue)
    {

      // NET backoff gate
      if (netNextAttemptAtMs != 0 && (int32_t)(now - netNextAttemptAtMs) < 0)
      {
        uint32_t waitMs = netNextAttemptAtMs - now;
        LOG("IDLE: comm due but NET backoff active, wait " + String(waitMs / 1000) + "s");
        break;
      }

      int32_t overdue = (int32_t)(now - commDueAtMs);
      LOG("IDLE: comm due -> start comm window overdueMs=" + String(overdue));
      changeState(SystemState::NET_CONNECT, ReasonCode::TIMER_COMM_DUE,
                  String("commOverdueMs=") + String(overdue));
    }

    break;
  }

  case SystemState::NET_CONNECT:
  {
    LOG("NET_CONNECT: starting network attach");

    // Stop GNSS before LTE
    gpsPowerOff();

    // Turn RF on and attach
    modemRfOn();

    NetResult net;
    bool ok = modemConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS, net);

    if (ok)
    {
      clearNetBackoff();
      timeSyncFromModem();
      timeSyncFromNtp(8000);

      commPublishedThisWindow = false;
      retryBackoffMs = 0;

      changeState(SystemState::MQTT_CONNECT, ReasonCode::NET_ATTACH_OK);
    }
    else
    {
      netFailCount++;
      bumpNetBackoff();

      // next attempt with jitter 0..20s
      netNextAttemptAtMs = millis() + netBackoffMs + (uint32_t)random(0, 20000);

      if (netFailCount >= NET_FAILS_BEFORE_POWERCYCLE)
      {
        LOG("NET_CONNECT: hard recovery (power cycle modem), fails=" + String(netFailCount));
        modemPowerCycle();
        netFailCount = 0;
        // after powercycle, try again soon (still respecting backoff)
      }

      commDueAtMs = netNextAttemptAtMs;

      changeState(SystemState::NET_DISCONNECT, ReasonCode::NET_ATTACH_FAIL,
                  String("backoffMs=") + String(netBackoffMs));
    }
    break;
  }

  case SystemState::MQTT_CONNECT:
  {
    LOG("MQTT_CONNECT: connecting to broker");

    // Hard timeout for mqttConnect (avoid dead-hang)
    const uint32_t t0 = millis();
    bool ok = false;
    while (millis() - t0 < 15000UL)
    { // 15s total
      if (mqttConnect())
      {
        ok = true;
        break;
      }
      delay(500);
    }

    if (ok)
    {
      mqttOnlineSince = millis();
      commWindowEndsAtMs = mqttOnlineSince + COMM_WINDOW_MAX_MS;
      changeState(SystemState::MQTT_ONLINE, ReasonCode::MQTT_CONN_OK);
    }
    else
    {
      bumpBackoff();
      commDueAtMs = millis() + retryBackoffMs;
      changeState(SystemState::MQTT_DISCONNECT, ReasonCode::MQTT_CONN_FAIL,
                  String("backoffMs=") + String(retryBackoffMs));
    }
    break;
  }

  case SystemState::MQTT_ONLINE:
  {
    mqttLoop();

    const uint32_t now2 = millis();
    const uint32_t elapsed = now2 - mqttOnlineSince;
    const bool minHoldDone = (elapsed >= MQTT_MIN_ONLINE_MS);
    const bool windowExpired = (now2 >= commWindowEndsAtMs);

    // If MQTT dropped inside window, try reconnect
    if (!mqttIsConnected())
    {
      LOG("MQTT: lost connection -> quick reconnect in same window");
      bool recOk = mqttQuickReconnect(now2);
      if (!recOk)
      {
        bumpBackoff();
        commDueAtMs = millis() + retryBackoffMs;
        changeState(SystemState::MQTT_DISCONNECT, ReasonCode::MQTT_RECONNECT_FAIL,
                    String("backoffMs=") + String(retryBackoffMs));
        break;
      }
    }

    // Publish once per comm window (your "alive/status", ideally includes latest GPS fix)
    if (!commPublishedThisWindow)
    {
      if (mqttPublishAlive())
      {
        commPublishedThisWindow = true;
        scheduleNextComm(now2);
        LOG("COMM: publish OK, next comm scheduled");
      }
      else
      {
        LOG("COMM: publish failed -> try quick reconnect then retry");
        bool recOk = mqttQuickReconnect(now2);
        if (recOk && mqttPublishAlive())
        {
          commPublishedThisWindow = true;
          scheduleNextComm(millis());
          LOG("COMM: publish OK after reconnect, next comm scheduled");
        }
        else
        {
          bumpBackoff();
          commDueAtMs = millis() + retryBackoffMs;
          LOG("COMM: still failing, backoffMs=" + String(retryBackoffMs));
          changeState(SystemState::MQTT_DISCONNECT, ReasonCode::COMM_PUBLISH_FAIL,
                      String("backoffMs=") + String(retryBackoffMs));
          break;
        }
      }
    }

    // status log every 5s
    static uint32_t lastStat = 0;
    if (now2 - lastStat > 5000)
    {
      uint32_t remMin = (elapsed < MQTT_MIN_ONLINE_MS) ? (MQTT_MIN_ONLINE_MS - elapsed) : 0;
      uint32_t remMax = (now2 < commWindowEndsAtMs) ? (commWindowEndsAtMs - now2) : 0;
      LOG("MQTT_ONLINE: minHoldRem=" + String(remMin / 1000) + "s, windowRem=" + String(remMax / 1000) + "s");
      lastStat = now2;
    }

    if (windowExpired)
    {
      changeState(SystemState::MQTT_DISCONNECT, ReasonCode::COMM_WINDOW_MAX_EXPIRED);
      break;
    }

    if (!minHoldDone)
      break;

    changeState(SystemState::MQTT_DISCONNECT, ReasonCode::COMM_WINDOW_MIN_HOLD_DONE);
    break;
  }

  case SystemState::MQTT_DISCONNECT:
  {
    mqttDisconnect();
    changeState(SystemState::NET_DISCONNECT);
    break;
  }

  case SystemState::NET_DISCONNECT:
  {
    LOG("NET_DISCONNECT: end of comm window");

    // Always turn RF off between windows (we want GNSS time)
    modemRfOff();

    if (retryBackoffMs > 0)
    {
      LOG("BACKOFF: mqtt backoffMs=" + String(retryBackoffMs));
    }
    if (netBackoffMs > 0 && netNextAttemptAtMs != 0)
    {
      uint32_t waitMs = (netNextAttemptAtMs > millis()) ? (netNextAttemptAtMs - millis()) : 0;
      LOG("BACKOFF: net backoffMs=" + String(netBackoffMs) + " next in " + String(waitMs / 1000) + "s");
    }

    changeState(SystemState::IDLE);
    break;
  }
  }

  delay(10);
}
