// Keeps the sketch running for good: a hardware watchdog restarts the board if
// the main loop stops, and the reason for the last restart is remembered so
// `status` can report it.
//
// Why: several waits in the Due core's USB interrupt code (USBCore.cpp,
// uotghs_device.c) have no timeout. If the PC stops answering in the middle
// of a USB request (sleep, USB power saving, a port reset), the chip freezes
// inside that interrupt for good: the loop stops, and the PC drops the native
// USB port (HID + CDC) while the Programming port, a separate chip, stays.
//
// The USB link can also be lost while the loop keeps running (seen after
// ~30 min idle, L LED still blinking). So the link is watched too: once the
// PC has configured the board, if the PC then stops sending USB frames, or
// the board stays unconfigured, for USB_LOST_MS, the board detaches from USB
// and restarts, which makes the PC enumerate it again from scratch.
#pragma once
#include <Arduino.h>

// Where loop() was, kept in a register that survives a reset, so a watchdog
// restart can say which part of the loop never returned.
enum LoopStage : uint8_t {
  STAGE_HEARTBEAT = 1,
  STAGE_USB,
  STAGE_CLI,
  STAGE_READERS,  // + reader index
  STAGE_TRACKER = STAGE_READERS + 8,
  STAGE_HID,
  STAGE_HEALTH,
};

void healthBegin();                  // call first in setup()
void healthStage(uint8_t stage);     // breadcrumb, cheap
void healthService(uint32_t nowMs);  // once per loop pass: feeds the watchdog, watches USB
void healthKick();                   // feeds the watchdog inside long operations
const char* healthResetText();       // why the board last started
bool healthWatchdogOn();
void healthPrintUsb(Print& out);     // USB link counters for `status`
