// Debug telemetry on the native USB CDC port (SerialUSB).
#pragma once
#include <Arduino.h>
#include "Config.h"
#include "UsbOut.h"

#define LOG_PORT usbOut

inline bool logOn(uint8_t level) { return level <= cfg.logLevel; }

void logf(uint8_t level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logHex(uint8_t level, const char* prefix, const uint8_t* data, uint16_t len);

// Writes `len` bytes as uppercase hex into `out` (needs 2*len+1 bytes).
void toHex(const uint8_t* data, uint8_t len, char* out);
