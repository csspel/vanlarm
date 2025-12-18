#include "gps.h"
#include "logging.h"

// We reuse the same UART that TinyGSM uses.
// SerialAT is defined in modem.cpp with external linkage.
extern HardwareSerial SerialAT;

static bool g_gpsOn = false;
static bool g_hasFix = false;
static GpsFix g_lastFix;
static uint32_t g_lastFixAtMs = 0;

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
// +CGNSINF: <GNSS run status>,<Fix status>,<UTC date & Time>,<Latitude>,<Longitude>,<MSL Altitude>,<Speed Over Ground>,<Course Over Ground>,<Fix Mode>,...
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
  double sog = f[6].toDouble(); // usually km/h on SIMCom
  out.speed_kmh = sog;
  out.course_deg = f[7].toDouble();
  out.fix_mode = (uint8_t)f[8].toInt();

  out.valid = (run == 1) && (fix == 1) && (out.utc.length() >= 8) && (abs(out.lat) > 0.0001 || abs(out.lon) > 0.0001);
  out.fix_age_ms = 0;
  return true;
}

// void gpsInit() {
//   // do nothing here; UART is initialized by modemInitUartAndPins()
// }

bool gpsPowerOn()
{
  if (g_gpsOn)
    return true;

  // Configure output format before power on (harmless if module ignores it).
  atCmdOk("AT+CGNSCFG=0", 2000);

  if (!atCmdOk("AT+CGNSPWR=1", 5000))
  {
    logSystem("GPS: CGNSPWR=1 failed");
    return false;
  }

  // Request RMC output when available; tolerated if unsupported.
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
    // still mark off to avoid stuck state
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
  uint32_t start = millis();
  uint32_t attempt = 0;

  if (!gpsPowerOn())
    return false;

  while (millis() - start < maxWaitMs)
  {
    attempt++;
    GpsFix tmp;
    bool ok = gpsPollOnce(tmp);
    if (ok && tmp.valid)
    {
      tmp.fix_age_ms = 0;
      out = tmp;
      logSystem("GPS: FIX OK lat=" + String(tmp.lat, 6) + " lon=" + String(tmp.lon, 6) + " spd=" + String(tmp.speed_kmh, 1));
      return true;
    }

    // If we have an old fix already, keep its age updated for caller
    if (g_hasFix)
    {
      out = g_lastFix;
      out.fix_age_ms = millis() - g_lastFixAtMs;
    }

    if (attempt == 1)
      logSystem("GPS: waiting for fix...");
    delay(1000);
  }

  // timeout: return last known fix (valid or not) for logging
  if (g_hasFix)
  {
    out = g_lastFix;
    out.fix_age_ms = millis() - g_lastFixAtMs;
  }
  logSystem("GPS: FIX TIMEOUT after " + String(maxWaitMs / 1000) + "s");
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
