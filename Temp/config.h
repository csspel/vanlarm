#pragma once
#include <Arduino.h>

// ---------------------------------------------------------
// LilyGO T-SIM7080G-S3 V1.0 – PIN-KONFIG
// (enligt LilyGO:s schema/exempel)
// ---------------------------------------------------------

// ESP32-S3 <-> SIM7080G
static const int PIN_MODEM_TX     = 4;    // ESP32-S3 TX  -> Modem RXD
static const int PIN_MODEM_RX     = 5;    // ESP32-S3 RX  -> Modem TXD
static const int PIN_MODEM_PWRKEY = 41;   // Modem PWR (PWRKEY/PWR)
static const int PIN_MODEM_RESET  = -1;   // ingen separat resetpin, -1 = ej använd

// SD-kort via SD_MMC (CMD/CLK/D0)
static const int PIN_SD_CMD = 39;
static const int PIN_SD_CLK = 38;
static const int PIN_SD_D0  = 40;

// ---------------------------------------------------------
// APN / NÄT – ÄNDRA TILL DITT ABBONNEMANG
// ---------------------------------------------------------
static const char APN[]       = "services.telenor.se"; 
static const char GPRS_USER[] = "";
static const char GPRS_PASS[] = "";
static const char SimPIN[] = "9952";
// ---------------------------------------------------------
// TIMING
// ---------------------------------------------------------

// För utveckling: 1 minut mellan nätförsök
static const uint32_t COMM_WINDOW_INTERVAL_MS = 60UL * 1000UL;

// För utveckling: 30 s räcker mer än väl
static const uint32_t NET_ATTACH_TIMEOUT_MS   = 15000;   // 15 s

// Watchdog längre än vår längsta nät-timeout
static const uint32_t WDT_TIMEOUT_SECONDS     = 30;
