#include <Arduino.h>
#include <math.h>

#include "gps.h"
#include "logging.h"
#include "config.h"
#include "time_manager.h"

// We reuse the same UART that TinyGSM uses.
// SerialAT is defined in modem.cpp with external linkage.
extern HardwareSerial SerialAT;

static bool g_gpsOn = false;
static bool g_hasFix = false;
static GpsFix g_lastFix;
static uint32_t g_lastFixAtMs = 0;

// Track what start mode we requested (for logging)
static const char *g_lastStartCmd = "AT+CGNSCOLD";

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
//
// OBS: På vissa FW kan <fix> vara tomt: "1,,2025...." trots att lat/lon blir riktiga.
// Då måste vi ha en fallbackbedömning baserat på lat/lon + DOP + satelliter.
static bool parseCgnsinf(const String &line, GpsFix &out)
{
  int colon = line.indexOf(':');
  if (colon < 0)
    return false;

  String csv = line.substring(colon + 1);
  csv.trim();

  // split (keep empty fields)
  const int MAXF = 32;
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
  int fix = f[1].toInt(); // kan bli 0 om fältet är tomt
  const bool fixFieldPresent = (f[1].length() > 0);

  out.utc = f[2];
  out.utc.trim();

  out.lat = f[3].toDouble();
  out.lon = f[4].toDouble();
  out.alt_m = f[5].toDouble();

  out.speed_kmh = f[6].toDouble();
  out.course_deg = f[7].toDouble();
  out.fix_mode = (uint8_t)f[8].toInt();

  // I dina loggar verkar DOP ligga här:
  //  index 10 = hdop-ish (0.1 / 500.0 / 41.5 ...)
  //  index 14 = sats used (2 / 4 ...)
  float hdop = 999.0f;
  if (n > 10 && f[10].length() > 0)
    hdop = f[10].toFloat();

  int satsUsed = 0;
  if (n > 14 && f[14].length() > 0)
    satsUsed = f[14].toInt();

  // Placeholder/dummy som du såg: 62,15,-32
  const bool placeholder =
      (fabs(out.lat - 62.0) < 0.05) &&
      (fabs(out.lon - 15.0) < 0.05) &&
      (fabs(out.alt_m + 32.0) < 10.0);

  const bool coordsLookReal = !placeholder && (fabs(out.lat) > 0.0001 || fabs(out.lon) > 0.0001);
  const bool dopLooksOk = (hdop > 0.0f && hdop < 200.0f); // 500 => skit
  const bool satsLooksOk = (satsUsed >= 4);               // 4+ brukar vara OK för fix

  // Valid-regel:
  // - run=1
  // - tid finns
  // - coords ser rimliga ut
  // - antingen fix==1 (normal)
  //   eller (fixfält tomt men sats+hdop ser bra ut)
  out.valid = (run == 1) &&
              (out.utc.length() >= 8) &&
              coordsLookReal &&
              ((fix == 1) || (!fixFieldPresent && satsLooksOk && dopLooksOk));

  out.fix_age_ms = 0;

  // Extra: om vi godkänner via fallback kan det vara bra att veta
  if (out.valid && fix != 1 && !fixFieldPresent)
  {
    logSystem(String("GPS: valid by sats/hdop (fix field empty) sats=") +
              String(satsUsed) + " hdop=" + String(hdop, 1));
  }

  return true;
}

static const char *pickStartCmd()
{
  // ingen tid -> cold (snabbt in -> men ofta sämre TTFF)
  if (!timeIsValid())
    return "AT+CGNSCOLD";

  // tid finns -> warm även om vi inte har fix i RAM
  if (!g_hasFix)
    return "AT+CGNSWARM";

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

  const char *startCmd = pickStartCmd();
  g_lastStartCmd = startCmd;

  bool startOk = atCmdOk(String(startCmd), 2000);
  logSystem(String("GPS: start=") + cmdToMode(startCmd) + " cmd_ok=" + (startOk ? "1" : "0"));

  // Optional
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
    // mark off anyway
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

  // Debug raw (var 10s)
  static uint32_t lastDbg = 0;
  if (millis() - lastDbg > 10000)
  {
    logSystem(String("GPS: CGNSINF raw=") + line);
    lastDbg = millis();
  }

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
