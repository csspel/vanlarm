#include <Arduino.h>

#include "gps.h"
#include "logging.h"
#include "config.h"

// We reuse the same UART that TinyGSM uses.
// SerialAT is defined in modem.cpp with external linkage.
extern HardwareSerial SerialAT;

static bool g_gpsOn = false;
static bool g_hasFix = false;
static GpsFix g_lastFix;
static uint32_t g_lastFixAtMs = 0;

// Track what start mode we requested (for logging)
static const char *g_lastStartCmd = "AT+CGNSCOLD";

// If not defined in config.h, use sane defaults
#ifndef GPS_HOT_MAX_AGE_MS
static constexpr uint32_t GPS_HOT_MAX_AGE_MS = 2UL * 60UL * 60UL * 1000UL; // 2h
#endif
#ifndef GPS_WARM_MAX_AGE_MS
static constexpr uint32_t GPS_WARM_MAX_AGE_MS = 24UL * 60UL * 60UL * 1000UL; // 24h
#endif

// --- minimal AT helper ----------------------------------------------------
static void atFlush()
{
  while (SerialAT.available())
    (void)SerialAT.read();
}

static bool atWaitOk(uint32_t timeoutMs)
{
  uint32_t start = millis();
  String line;
  while (millis() - start < timeoutMs)
  {
    while (SerialAT.available())
    {
      char c = (char)SerialAT.read();
      if (c == '\r')
        continue;
      if (c == '\n')
      {
        line.trim();
        if (line.length())
        {
          if (line == "OK")
            return true;
          if (line == "ERROR")
            return false;
        }
        line = "";
      }
      else
      {
        line += c;
      }
    }
    delay(5);
  }
  return false;
}

static bool atCmdOk(const String &cmd, uint32_t timeoutMs = 2000)
{
  atFlush();
  SerialAT.println(cmd);
  return atWaitOk(timeoutMs);
}

static bool atCmdGetLine(const String &cmd, const String &prefix, String &outLine, uint32_t timeoutMs = 2000)
{
  atFlush();
  SerialAT.println(cmd);
  uint32_t start = millis();
  String line;
  bool got = false;
  while (millis() - start < timeoutMs)
  {
    while (SerialAT.available())
    {
      char c = (char)SerialAT.read();
      if (c == '\r')
        continue;
      if (c == '\n')
      {
        line.trim();
        if (line.length())
        {
          if (line.startsWith(prefix))
          {
            outLine = line;
            got = true;
          }
          if (line == "OK")
            return got;
          if (line == "ERROR")
            return false;
        }
        line = "";
      }
      else
      {
        line += c;
      }
    }
    delay(5);
  }
  return got;
}

// --- CGNSINF parse --------------------------------------------------------
// +CGNSINF: <run>,<fix>,<utc>,<lat>,<lon>,<alt>,<speed>,<course>,<fixmode>,...
static bool parseCgnsinf(const String &line, GpsFix &out)
{
  int colon = line.indexOf(':');
  if (colon < 0)
    return false;

  String csv = line.substring(colon + 1);
  csv.trim();

  // split (keep empty fields)
  const int MAXF = 20;
  String f[MAXF];
  int n = 0;
  int start = 0;
  for (int i = 0; i <= (int)csv.length(); i++)
  {
    if (i == (int)csv.length() || csv[i] == ',')
    {
      if (n < MAXF)
        f[n++] = csv.substring(start, i);
      start = i + 1;
    }
  }
  if (n < 9)
    return false;

  int run = f[0].toInt();
  int fix = f[1].toInt();

  out.utc = f[2];
  out.utc.trim();

  out.lat = f[3].toDouble();
  out.lon = f[4].toDouble();
  out.alt_m = f[5].toDouble();

  // SIMCom brukar ge km/h här (men vi behåller som "speed_kmh" som du redan gör)
  out.speed_kmh = f[6].toDouble();
  out.course_deg = f[7].toDouble();
  out.fix_mode = (uint8_t)f[8].toInt();

  out.valid = (run == 1) && (fix == 1) && (out.utc.length() >= 8) &&
              (abs(out.lat) > 0.0001 || abs(out.lon) > 0.0001);

  out.fix_age_ms = 0;
  return true;
}

static const char *pickStartCmd()
{
  // Ingen tidigare fix i RAM → cold
  if (!g_hasFix)
    return "AT+CGNSCOLD";

  uint32_t age = millis() - g_lastFixAtMs;
  if (age <= GPS_HOT_MAX_AGE_MS)
    return "AT+CGNSHOT";
  if (age <= GPS_WARM_MAX_AGE_MS)
    return "AT+CGNSWARM";
  return "AT+CGNSCOLD";
}

static const char *cmdToMode(const char *cmd)
{
  if (!cmd)
    return "UNKNOWN";
  if (strcmp(cmd, "AT+CGNSHOT") == 0)
    return "HOT";
  if (strcmp(cmd, "AT+CGNSWARM") == 0)
    return "WARM";
  if (strcmp(cmd, "AT+CGNSCOLD") == 0)
    return "COLD";
  return "UNKNOWN";
}

// -------------------------------------------------------------------------

void gpsInit()
{
  // UART is initialized elsewhere (modem.cpp)
}

bool gpsPowerOn()
{
  if (g_gpsOn)
    return true;

  // Configure output format before power on (harmless if module ignores it)
  atCmdOk("AT+CGNSCFG=0", 2000);

  if (!atCmdOk("AT+CGNSPWR=1", 5000))
  {
    logSystem("GPS: CGNSPWR=1 failed");
    return false;
  }

  // Select start mode (HOT/WARM/COLD) to reduce TTFF when we have recent fixes
  const char *startCmd = pickStartCmd();
  g_lastStartCmd = startCmd;

  bool startOk = atCmdOk(String(startCmd), 2000);
  logSystem(String("GPS: start=") + cmdToMode(startCmd) + " cmd_ok=" + (startOk ? "1" : "0"));

  // Request RMC output when available; tolerated if unsupported
  atCmdOk("AT+CGNSSEQ=RMC", 2000);

  g_gpsOn = true;
  logSystem("GPS: ON");
  return true;
}

bool gpsPowerOff()
{
  if (!g_gpsOn)
    return true;

  if (!atCmdOk("AT+CGNSPWR=0", 5000))
  {
    logSystem("GPS: CGNSPWR=0 failed");
    // mark off anyway to avoid stuck state
  }
  g_gpsOn = false;
  logSystem("GPS: OFF");
  return true;
}

bool gpsIsOn()
{
  return g_gpsOn;
}

bool gpsPollOnce(GpsFix &out)
{
  out = GpsFix{};

  if (!g_gpsOn)
  {
    if (!gpsPowerOn())
      return false;
  }

  String line;
  bool ok = atCmdGetLine("AT+CGNSINF", "+CGNSINF:", line, 2000);
  if (!ok)
    return false;

  if (!parseCgnsinf(line, out))
    return false;

  if (out.valid)
  {
    g_hasFix = true;
    g_lastFix = out;
    g_lastFixAtMs = millis();
  }
  return true;
}

bool gpsGetFixWait(GpsFix &out, uint32_t maxWaitMs)
{
  uint32_t startMs = millis();
  uint32_t attempt = 0;

  if (!gpsPowerOn())
    return false;

  while (millis() - startMs < maxWaitMs)
  {
    attempt++;

    GpsFix tmp;
    bool ok = gpsPollOnce(tmp);

    if (ok && tmp.valid)
    {
      tmp.fix_age_ms = 0;
      out = tmp;

      uint32_t ttff_s = (millis() - startMs) / 1000UL;
      logSystem(String("GPS: FIX OK lat=") + String(tmp.lat, 6) +
                " lon=" + String(tmp.lon, 6) +
                " spd=" + String(tmp.speed_kmh, 1) +
                " ttff_s=" + String(ttff_s) +
                " start=" + cmdToMode(g_lastStartCmd));
      return true;
    }

    // If we have an old fix already, keep its age updated for caller
    if (g_hasFix)
    {
      out = g_lastFix;
      out.fix_age_ms = millis() - g_lastFixAtMs;
    }

    if (attempt == 1)
    {
      logSystem(String("GPS: waiting for fix... start=") + cmdToMode(g_lastStartCmd));
    }
    delay(1000);
  }

  // timeout: return last known fix (valid or not) for logging
  if (g_hasFix)
  {
    out = g_lastFix;
    out.fix_age_ms = millis() - g_lastFixAtMs;
  }

  logSystem(String("GPS: FIX TIMEOUT after ") + String(maxWaitMs / 1000) +
            "s start=" + cmdToMode(g_lastStartCmd));
  return false;
}

bool gpsHasLastFix()
{
  return g_hasFix;
}

GpsFix gpsLastFix()
{
  GpsFix o = g_lastFix;
  if (g_hasFix)
    o.fix_age_ms = millis() - g_lastFixAtMs;
  return o;
}

uint32_t gpsLastFixAtMs()
{
  return g_lastFixAtMs;
}
