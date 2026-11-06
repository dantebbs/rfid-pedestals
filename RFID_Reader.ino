// RFID Reader: up to five NXP PN7160 NFC readers on an Arduino Due.
//
// Connect the PC to the Due's NATIVE USB port. It enumerates as a composite
// device:
//   HID - one key per tag event from the reader/tag map (TagMap.h), or with
//         'set output id' "<reader>+<UID>" / "<reader>-<UID>"
//   CDC - debug telemetry and a configuration CLI (any terminal; type "help")
// Upload through the Programming port as usual.
//
// See README.md for wiring and settings.

#include <Wire.h>
#include <HID.h>
#include <Reset.h>
#include "Config.h"
#include "Log.h"
#include "I2CBus.h"
#include "Pn7160Reader.h"
#include "TagTracker.h"
#include "TagMap.h"
#include "HidOut.h"
#include "Cli.h"
#include "UsbOut.h"

// To use a different reader chip, implement TagSensor (TagSensor.h) and
// change this declaration.
static Pn7160Reader readers[MAX_READERS];
TagSensor* sensors[MAX_READERS];

static void onTagSeen(uint8_t reader, const uint8_t* uid, uint8_t uidLen) {
  trackerSeen(reader, uid, uidLen);
}

static void onTagEvent(bool appeared, uint8_t reader, const char* uidHex) {
  char line[40];
  // <reader number><+|-><tag>, e.g. "1+04A1B2C3D4E5F6".
  snprintf(line, sizeof(line), "%u%c%s", reader + 1, appeared ? '+' : '-', uidHex);
  // The map also sees events in id mode, so the key tag works in both.
  char key = tagMapEvent(appeared, reader, uidHex);
  if (cfg.outputMode == OUT_ID) {
    hidSendLine(line);
    logf(LOG_EVENT, "%s", line);
  } else if (key) {
    char text[2] = {key, 0};
    hidSendText(text, false);  // a single key press, no Enter
    logf(LOG_EVENT, "%s  -> key '%c'", line, key);
  } else {
    logf(LOG_EVENT, "%s  (no key)", line);
  }
}

void setup() {
  SerialUSB.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  bool loaded = configLoad();

  tagMapBegin();
  trackerBegin(onTagEvent);
  for (uint8_t i = 0; i < MAX_READERS; i++) {
    sensors[i] = &readers[i];
    sensors[i]->begin(i, &cfg.reader[i], onTagSeen);
  }

  logf(LOG_EVENT, "RFID Reader started, %s config. Type 'help' for commands.", loaded ? "saved" : "default");
}

// The Due core treats "Native port opened at 1200 baud, then closed" as a
// request to upload through that port: it clears the boot-from-flash flag and
// restarts into the SAM-BA bootloader. The flag stays cleared, so from then
// on the board comes up as a bootloader on every power-up until the sketch is
// uploaded again. Uploads here go through the Programming port, so cancel
// that pending reset on every SysTick (the core calls this hook each 1 ms,
// just before it would act on the reset).
extern "C" int sysTickHook(void) {
  cancelReset();
  return 0;  // 0 = let the core's normal SysTick handling continue
}

// The on-board "L" LED blinks once a second while loop() is running. If it
// stops, the sketch is stuck somewhere.
static void heartbeat(uint32_t now) {
  static uint32_t last = 0;
  static bool on = false;
  if (now - last < 500) return;
  last = now;
  on = !on;
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}

void loop() {
  uint32_t now = millis();
  heartbeat(now);
  usbService();
  cliService();
  for (uint8_t i = 0; i < MAX_READERS; i++) sensors[i]->service(now);
  trackerService(now);
  hidService(now);
}
