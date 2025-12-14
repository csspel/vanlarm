#pragma once
#include <Arduino.h>
#include <FS.h>  

struct SdLogConfig {
  const char* mountPoint = "/sd";
  //uint8_t csPin = 10;              // ÄNDRA till din SD-CS
  uint32_t flushEveryMs = 2000;     // flush minst var 2s (om det finns data)
  uint16_t flushBatchLines = 40;    // skriv max 40 rader per flush (så vi inte blockerar för länge)
  bool alsoSerial = true;          // spegla logg till Serial
  bool addSeq = true;              // prefixa med seq
  bool addMillis = true;           // prefixa med uptime/seconds
};

class SdLogger {
public:
  bool begin(const SdLogConfig& cfg);
  void setLogFile(const String& path);     // ex: "/sd/van_2025-12-14.log"
  void setState(const char* state);        // valfritt: "IDLE", "NET_CONNECT"...
  void logf(const char* fmt, ...);
  void logLine(const String& line);

  // Kör ofta i loop() – sköter tidsstyrd flush
  void loop();

  // Kalla dessa på strategiska ställen
  void flushNow(uint16_t maxLines = 0);    // skriv batch direkt (0 = cfg.flushBatchLines)
  void flushAndSync();                     // skriv allt + flush

  // Stats
  uint32_t getSeq() const { return _seq; }
  uint32_t getDropped() const { return _dropped; }
  uint32_t getSdWriteFails() const { return _sdWriteFails; }
  bool isReady() const { return _ready; }

private:
  bool openFileIfNeeded();
  void closeFile();
  String makePrefix();
  void pushToBuffer(const String& s);
  fs::FS* _fs = nullptr;


  // ringbuffer
  static constexpr uint16_t CAP = 300;     // antal rader i RAM
  String _buf[CAP];
  uint16_t _head = 0; // nästa skrivposition
  uint16_t _tail = 0; // nästa läsposition
  uint16_t _count = 0;

  SdLogConfig _cfg;
  bool _ready = false;
  String _filePath;
  String _state = "NA";

  // stats
  uint32_t _seq = 0;
  uint32_t _dropped = 0;
  uint32_t _sdWriteFails = 0;

  // timing
  uint32_t _lastFlushMs = 0;

  // SD handle (Arduino SD / SdFat kan skilja; här kör vi SD)
  File _file;
};

extern SdLogger SDLOG;
