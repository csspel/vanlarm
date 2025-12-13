#include "mqtt.h"
#include "config.h"
#include "logging.h"
#include "modem.h"
#include "profiles.h"
#include <PubSubClient.h>
#include "time_manager.h"

static Client*       netClient   = nullptr;
static PubSubClient* mqttClient  = nullptr;
static uint32_t msgCounter = 0;

static String lastDownlinkRaw;
static uint32_t lastAckMsgId = 0;

static String jsonGetString(const String& json, const char* key) {
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k);
  if (i < 0) return "";
  i += k.length();

  // hoppa whitespace
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;

  // måste börja med "
  if (i >= (int)json.length() || json[i] != '"') return "";
  i++;
  int j = json.indexOf('"', i);
  if (j < 0) return "";
  return json.substring(i, j);
}

static uint32_t jsonGetUInt(const String& json, const char* key) {
  String k = String("\"") + key + "\":";
  int i = json.indexOf(k);
  if (i < 0) return 0;
  i += k.length();
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;

  // tillåt "123" eller 123
  if (i < (int)json.length() && json[i] == '"') i++;

  uint32_t val = 0;
  while (i < (int)json.length() && isDigit(json[i])) {
    val = val * 10 + (json[i] - '0');
    i++;
  }
  return val;
}

static void mqttPublishAck(uint32_t ackMsgId, const char* status, const char* detail = "") {
  // {"device_id":"...","type":"ACK","ack_msg_id":1234,"status":"OK","detail":"...","profile":"ALARM"}
  const ProfileConfig& p = currentProfile();

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"type\":\"ACK\",";
  payload += "\"ack_msg_id\":" + String(ackMsgId) + ",";
  payload += "\"status\":\"" + String(status) + "\",";
  payload += "\"detail\":\"" + String(detail) + "\",";
  payload += "\"profile\":\"" + String(p.name) + "\"";
  payload += "}";

  mqttClient->publish(MQTT_TOPIC_ACK, payload.c_str());
  logSystem("MQTT: ACK published payload=" + payload);
}

static void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  String t(topic);
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  // --- FIX: ignore retained clear (empty payload) ---
  msg.trim();
  if (msg.length() == 0) {
    return;  // HA cleared retained cmd/downlink with empty payload
  }

  logSystem("MQTT: RX topic=" + t + " payload=" + msg);
  lastDownlinkRaw = msg;

  if (t != MQTT_TOPIC_DOWNLINK) return;

  uint32_t ackId = jsonGetUInt(msg, "ack_msg_id");
  String desired = jsonGetString(msg, "desired_profile");

  if (ackId == 0) {
    // vi kräver msg_id för spårbarhet
    mqttPublishAck(0, "ERROR", "missing_ack_msg_id");
    return;
  }

  if (desired.length() > 0) {
    ProfileId pid;
    if (profileFromString(desired, pid)) {
      setProfile(pid);
      mqttPublishAck(ackId, "OK", "profile_set");
      mqttPublishAlive();   // eller mqttPublishTelemetry() om du senare byter namn
    } else {
      mqttPublishAck(ackId, "ERROR", "unknown_profile");
    }
  } else {
    mqttPublishAck(ackId, "OK", "no_profile_change");
  }
}

void mqttSetup() {
  if (!netClient) {
    netClient = &modemGetClient();
  }
  if (!mqttClient) {
    mqttClient = new PubSubClient(*netClient);
    mqttClient->setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqttClient->setCallback(mqttCallback);

    // ✅ FIX: större buffer för längre JSON-payload
    mqttClient->setBufferSize(512);   // testa 512 först, annars 1024
    mqttClient->setKeepAlive(30);     // valfritt, men bra över LTE
    mqttClient->setSocketTimeout(10); // valfritt
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
  mqttClient->subscribe(MQTT_TOPIC_DOWNLINK);
  logSystem("MQTT: subscribed " + String(MQTT_TOPIC_DOWNLINK));
  return true;
}

bool mqttPublishAlive() {
  if (!mqttClient || !mqttClient->connected()) {
    logSystem("MQTT: cannot publish alive, not connected");
    return false;
  }

  uint32_t upSeconds = millis() / 1000;
  const ProfileConfig& p = currentProfile();

  msgCounter++;
  String msgId = String(msgCounter);

  const bool   timeValid = timeIsValid();
  const String isoUtc    = timeIsoUtc();

  const char* src = "NONE";
  if (timeGetSource() == TimeSource::MODEM) src = "MODEM";
  else if (timeGetSource() == TimeSource::NTP) src = "NTP";

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  payload += "\"msg_id\":\""   + msgId + "\",";
  payload += "\"type\":\"ALIVE\",";
  payload += "\"timestamp\":\"" + isoUtc + "\",";
  payload += "\"epoch_utc\":" + String(timeEpochUtc()) + ",";
  payload += "\"time_valid\":" + String(timeValid ? "true" : "false") + ",";
  payload += "\"time_source\":\"" + String(src) + "\",";
  payload += "\"date_local\":\"" + timeDateLocal() + "\",";
  payload += "\"time_local\":\"" + timeClockLocal() + "\",";
  payload += "\"profile\":\"" + String(p.name) + "\",";
  payload += "\"uptime_s\":" + String(upSeconds);
  payload += "}";

  logSystem("MQTT: publishing alive to " + String(MQTT_TOPIC_ALIVE) +
            " payload=" + payload);
  logSystem("MQTT: alive payload bytes=" + String(payload.length()));

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
bool mqttIsConnected() {
  return mqttClient && mqttClient->connected();
}

void mqttLoopFor(uint32_t durationMs) {
  if (!mqttClient || !mqttClient->connected()) return;
  uint32_t start = millis();
  while (millis() - start < durationMs) {
    mqttClient->loop();
    delay(10);
  }
}
