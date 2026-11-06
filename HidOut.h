// The HID half of the composite USB device.
//
// One HID interface carries two top-level collections:
//   report 2: a boot-style keyboard that types each event line, e.g.
//             "1+04A1B2C3D4E5F6" then Enter (US keyboard layout assumed);
//   report 3: a vendor-defined 32-byte input report (usage page 0xFF00)
//             holding the same line as ASCII, zero padded, one per event.
// Config::hidMode picks which of them is used.
#pragma once
#include <Arduino.h>

void hidSendLine(const char* line);                // presses Enter after it if 'set enter on'
void hidSendText(const char* text, bool enter);    // Enter only if asked
void hidService(uint32_t nowMs);  // types queued keystrokes, one report per call
bool hidBusy();
