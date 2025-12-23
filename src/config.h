#pragma once
#include <Arduino.h>

// ====== LilyGO T-SIM7080G-S3 – PIN-KONFIG ======

// Modem / UART
#define BOARD_MODEM_PWR_PIN 41
#define BOARD_MODEM_RXD_PIN 4
#define BOARD_MODEM_TXD_PIN 5
#define BOARD_MODEM_RI_PIN 3
#define BOARD_MODEM_DTR_PIN 42

// I2C till PMU (AXP2101) – byt namn så vi inte krockar med Arduino-kärnan
#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 7

// ==== Storage policy ====
// true  = systemet kräver SD (release-läge)
// false = SD är valfri (pipeline/dev)
constexpr bool REQUIRE_SD = false;

// SD_MMC (1-bit)
static const int PIN_SD_CLK = 38;
static const int PIN_SD_CMD = 39;
static const int PIN_SD_D0 = 40;

// Nätinställningar
static const char APN[] = "services.telenor.se";
static const char SIM_PIN[] = "9952";

static const uint32_t NET_REG_TIMEOUT_MS = 120000UL;
static const uint32_t DATA_ATTACH_TIMEOUT_MS = 60000UL;

// ====== MQTT-inställningar ======
// Anpassa till din broker (HA / Mosquitto etc.)
static const char MQTT_BROKER_HOST[] = "noren.myds.me"; // ÄNDRA till din broker-IP
static const uint16_t MQTT_BROKER_PORT = 1883;

static const char MQTT_CLIENT_ID[] = "campervanlarm";
static const char MQTT_USERNAME[] = "hemautomation"; // om du kör auth, fyll i
static const char MQTT_PASSWORD[] = "hemautomation"; // annars lämna tomt

// Topic för alive
static const char MQTT_TOPIC_ALIVE[] = "van/ellie/tele/alive";

// GPS single uplink
static const char MQTT_TOPIC_GPS_SINGLE[] = "van/ellie/tele/gps";

// Downlink topics (HA -> device)
static const char MQTT_TOPIC_DOWNLINK[] = "van/ellie/cmd/downlink";

// Uplink ack (device -> HA)
static const char MQTT_TOPIC_ACK[] = "van/ellie/ack";

static const char MQTT_TOPIC_VERSION[] = "van/ellie/tele/version";

// ---- PIR ----
// TODO: Sätt rätt pin för din PIR på T-SIM7080G-S3 bygget
// ---- PIR ----
constexpr int PIN_PIR_FRONT = 9; // PIR fram
constexpr int PIN_PIR_BACK = 17; // PIR bak
constexpr bool PIR_RISING_EDGE = true;

// ---- PIR topics ----
static const char MQTT_TOPIC_PIR[] = "van/ellie/tele/pir";
static const char MQTT_TOPIC_CMD_ACK[] = "van/ellie/cmd/ack"; // server -> device ack (PIR_ACK)

// Hur länge vi håller MQTT_ONLINE öppet för SUBs (Steg 3)
constexpr uint32_t MQTT_ONLINE_WINDOW_MS = 8000UL; // 8 sek

static const char DEVICE_ID[] = "van_ellie";

// --- Timers -----------------------------------------------

// Alive ska motsvara STOLEN-test: 120 s
constexpr uint32_t ALIVE_INTERVAL_MS = 120000UL; // 2 min

// ==== Storage policy ====
// true  = systemet kräver SD (release-läge)
// false = SD är valfri (pipeline/dev)
