// Debug telemetry, sent to both the native USB CDC port (SerialUSB) and the
// Programming port (Serial, 115200 baud). The Programming port copy keeps
// working when the native USB link is lost, so a terminal left open on it
// records what happened.
#pragma once
#include <Arduino.h>
#include "Config.h"
#include "UsbOut.h"

class LogTee : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t len) override;
  using Print::write;
};
extern LogTee logTee;
#define LOG_PORT logTee

inline bool logOn(uint8_t level) { return level <= cfg.logLevel; }

void logf(uint8_t level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logHex(uint8_t level, const char* prefix, const uint8_t* data, uint16_t len);

// Writes `len` bytes as uppercase hex into `out` (needs 2*len+1 bytes).
void toHex(const uint8_t* data, uint8_t len, char* out);
