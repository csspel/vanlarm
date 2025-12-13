#include "mqtt.h"
#include "config.h"
#include "logging.h"
#include "modem.h"
#include "profiles.h"
#include <PubSubClient.h>

static Client*       netClient   = nullptr;
static PubSubClient* mqttClient  = nullptr;
static uint32_t msgCounter = 0;

void mqttSetup() {
  if (!netClient) {
    netClient = &modemGetClient();
  }
  if (!mqttClient) {
    mqttClient = new PubSubClient(*netClient);
    mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  }
}

bool mqttConnect() {
  if (!mqttClient) mqttSetup();

  logSystem("MQTT: connecting to broker");

  mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);

  bool ok;
  if (strlen(MQTT_USERNAME) > 0) {
    ok = mqttClient->connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    ok = mqttClient->connect(MQTT_CLIENT_ID);
  }

  if (!ok) {
    logSystem("MQTT: connect FAILED, rc=" + String(mqttClient->state()));
    return false;
  }

  logSystem("MQTT: connected OK");
  return true;
}

bool mqttPublishAlive() {
  if (!mqttClient || !mqttClient->connected()) {
    logSystem("MQTT: cannot publish alive, not connected");
    return false;
  }

  uint32_t upSeconds = millis() / 1000;
  const ProfileConfig& p = currentProfile();

  // msg_id som enkel räknare – förbättras senare (t.ex. inkludera boot-id)
  msgCounter++;
  String msgId = String(msgCounter);

  // timestamp i UTC (ISO-ish)
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  char tsBuf[25];
  // YYYY-MM-DDTHH:MM:SSZ
  snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);

  // Bygg JSON enligt din dataspec (light-version)
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"msg_id\":\""   + msgId + "\",";
  payload += "\"type\":\"ALIVE\",";
  payload += "\"timestamp\":\"" + String(tsBuf) + "\",";
  payload += "\"profile\":\"" + String(p.name) + "\",";
  payload += "\"uptime_s\":" + String(upSeconds);
  payload += "}";

  logSystem("MQTT: publishing alive to " + String(MQTT_TOPIC_ALIVE) +
            " payload=" + payload);

  bool ok = mqttClient->publish(MQTT_TOPIC_ALIVE, payload.c_str());
  if (!ok) {
    logSystem("MQTT: publish FAILED");
    return false;
  }

  logSystem("MQTT: alive published OK, msg_id=" + msgId);
  return true;
}

void mqttLoop() {
  if (mqttClient && mqttClient->connected()) {
    mqttClient->loop();
  }
}

void mqttDisconnect() {
  if (mqttClient && mqttClient->connected()) {
    logSystem("MQTT: disconnect");
    mqttClient->disconnect();
  }
}
