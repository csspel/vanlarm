#include "sdlog.h"
#include <SD_MMC.h>
#include <LittleFS.h>
#include <stdarg.h>

SdLogger SDLOG;

bool SdLogger::begin(const SdLogConfig& cfg) {
  _cfg = cfg;

  if (_cfg.alsoSerial) Serial.println("SDLOG: begin()");

  _ready = true;
  _fs = nullptr;

  // 1) Försök SD-kort via SD_MMC
  if (SD_MMC.begin("/sdcard", true) && SD_MMC.cardType() != CARD_NONE) {
    _fs = &SD_MMC;
    _ready = true;
    if (_cfg.alsoSerial) {
      Serial.println("SDLOG: using SD_MMC (/sdcard, 1-bit)");
      Serial.printf("SDLOG: size=%llu MB\n", SD_MMC.cardSize() / (1024 * 1024));
    }
  } else {
    // 2) Fallback: LittleFS (intern flash)
    if (LittleFS.begin(true)) {
      _fs = &LittleFS;
      _ready = true;
      if (_cfg.alsoSerial) Serial.println("SDLOG: using LittleFS (flash) fallback");
    } else {
      if (_cfg.alsoSerial) Serial.println("SDLOG: LittleFS.begin FAILED");
      _ready = false;
      return false;
    }
  }

  _lastFlushMs = millis();
  return true;
}

void SdLogger::setLogFile(const String& path) {
  _filePath = path;
  closeFile();
}

void SdLogger::setState(const char* state) {
  if (state && *state) _state = state;
}

String SdLogger::makePrefix() {
  String p;
  if (_cfg.addSeq) { p += String(++_seq); p += " | "; }
  if (_cfg.addMillis) { p += String(millis() / 1000); p += "s | "; }
  p += _state; p += " | ";
  return p;
}

void SdLogger::pushToBuffer(const String& s) {
  if (_count >= CAP) {
    _tail = (_tail + 1) % CAP;
    _count--;
    _dropped++;
  }
  _buf[_head] = s;
  _head = (_head + 1) % CAP;
  _count++;
}

void SdLogger::logLine(const String& line) {
  String full = makePrefix() + line;
  pushToBuffer(full);
  if (_cfg.alsoSerial) Serial.println(full);
}

void SdLogger::logf(const char* fmt, ...) {
  char tmp[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, args);
  va_end(args);
  logLine(String(tmp));
}

bool SdLogger::openFileIfNeeded() {
  if (!_ready) return false;
  if (_file) return true;
  if (_filePath.length() == 0) return false;

  _file = SD_MMC.open(_filePath.c_str(), FILE_APPEND);
  if (!_file) {
    _sdWriteFails++;
    if (_cfg.alsoSerial) Serial.println("SDLOG: open file FAILED");
    return false;
  }
  return true;
}

void SdLogger::closeFile() {
  if (_file) {
    _file.flush();
    _file.close();
  }
}

void SdLogger::flushNow(uint16_t maxLines) {
  if (_count == 0) return;
  if (!_ready || !_fs) return;
  if (_filePath.length() == 0) return;
  if (!openFileIfNeeded()) return;

  uint16_t toWrite = (maxLines == 0) ? _cfg.flushBatchLines : maxLines;
  uint16_t written = 0;

  while (_count > 0 && written < toWrite) {
    String& s = _buf[_tail];
    size_t n = _file.println(s);
    if (n == 0) {
      _sdWriteFails++;
      closeFile();
      break;
    }
    _tail = (_tail + 1) % CAP;
    _count--;
    written++;
  }

  _file.flush();
  _lastFlushMs = millis();
}

void SdLogger::flushAndSync() {
  while (_count > 0) {
    flushNow(_cfg.flushBatchLines);
    delay(2);
    if (!_file) break;
  }
  if (_file) _file.flush();
}

void SdLogger::loop() {
  if (_count == 0) return;
  uint32_t now = millis();
  if (now - _lastFlushMs >= _cfg.flushEveryMs) flushNow();
}
