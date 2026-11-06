#include "Log.h"
#include <stdarg.h>

static void stamp() {
  uint32_t ms = millis();
  char buf[16];
  snprintf(buf, sizeof(buf), "[%6lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  LOG_PORT.print(buf);
}

// usbOut drops output when no terminal is reading, so logging never blocks
// the readers.
void logf(uint8_t level, const char* fmt, ...) {
  if (!logOn(level)) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  stamp();
  LOG_PORT.println(buf);
}

void logHex(uint8_t level, const char* prefix, const uint8_t* data, uint16_t len) {
  if (!logOn(level)) return;
  stamp();
  LOG_PORT.print(prefix);
  char b[4];
  for (uint16_t i = 0; i < len; i++) {
    snprintf(b, sizeof(b), " %02X", data[i]);
    LOG_PORT.print(b);
  }
  LOG_PORT.println();
}

void toHex(const uint8_t* data, uint8_t len, char* out) {
  static const char digits[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < len; i++) {
    *out++ = digits[data[i] >> 4];
    *out++ = digits[data[i] & 0x0F];
  }
  *out = 0;
}
