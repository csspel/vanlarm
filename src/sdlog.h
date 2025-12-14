#pragma once
#include <Arduino.h>

struct SdLogConfig {
  uint32_t flushEveryMs     = 2000;
  uint16_t flushBatchLines  = 40;
  bool     alsoSerial       = false;
};

class SdLogger {
public:
  bool begin(const SdLogConfig& cfg);

  void setLogFile(const char* path);
  void setState(const char* state);

  void logLine(const String& line);

  void loop();
  void flushNow(uint16_t maxLines = 0);
  void flushAndSync();

  bool isReady() const { return _ready; }

private:
  void pushToBuffer(const String& line);
  void appendBatch(uint16_t n);

  SdLogConfig _cfg;
  bool        _ready       = false;

  String      _logFile     = "/system.log";
  const char* _state       = "NA";

  static constexpr uint16_t BUF_CAP = 300;
  String   _buf[BUF_CAP];
  uint16_t _head = 0;
  uint16_t _tail = 0;
  uint16_t _count = 0;

  uint32_t _lastFlushMs = 0;
};

extern SdLogger SDLOG;
