#include "sdlog.h"
#include <Arduino.h>
#include <SD_MMC.h>

SdLogger SDLOG;

bool SdLogger::begin(const SdLogConfig& cfg) {
  _cfg = cfg;
  _ready = true;

  _head = _tail = _count = 0;
  _lastFlushMs = millis();

  if (_cfg.alsoSerial) Serial.println("SDLOG: begin()");
  return true;
}

void SdLogger::setLogFile(const char* path) {
  if (!path || !path[0]) return;
  _logFile = String(path);
}

void SdLogger::setState(const char* state) {
  if (!state || !state[0]) return;
  _state = state;
}

void SdLogger::pushToBuffer(const String& line) {
  // Ringbuffer: skriv på head, vid full -> droppa äldsta (tail++)
  _buf[_head] = line;
  _head = (uint16_t)((_head + 1) % BUF_CAP);

  if (_count < BUF_CAP) {
    _count++;
  } else {
    _tail = (uint16_t)((_tail + 1) % BUF_CAP);
  }
}

void SdLogger::logLine(const String& line) {
  if (!_ready) return;

  if (_cfg.alsoSerial) {
    Serial.println(line);
  }

  pushToBuffer(line);
}

void SdLogger::appendBatch(uint16_t n) {
  if (n == 0) return;

  File f = SD_MMC.open(_logFile, FILE_APPEND);
  if (!f) {
    // Viktigt: vi slår inte av _ready, men vi kan signalera på Serial om du vill
    if (_cfg.alsoSerial) Serial.println("SDLOG: open FAILED");
    return;
  }

  for (uint16_t i = 0; i < n && _count > 0; i++) {
    f.println(_buf[_tail]);
    _tail = (uint16_t)((_tail + 1) % BUF_CAP);
    _count--;
  }

  f.flush();
  f.close();
}

void SdLogger::flushNow(uint16_t maxLines) {
  if (!_ready) return;
  if (_count == 0) return;

  uint16_t batch = _cfg.flushBatchLines;
  if (maxLines > 0 && maxLines < batch) batch = maxLines;

  uint16_t n = (_count < batch) ? _count : batch;
  appendBatch(n);

  _lastFlushMs = millis();
}

void SdLogger::flushAndSync() {
  if (!_ready) return;
  while (_count > 0) {
    flushNow(80);
    delay(5);
  }
}

void SdLogger::loop() {
  if (!_ready) return;

  const uint32_t now = millis();
  if (now - _lastFlushMs >= _cfg.flushEveryMs) {
    flushNow();
  }
}
