// The boundary between the rest of the firmware and the RFID chip.
//
// A sensor only has to report "I can see tag X" repeatedly while the tag is
// in its field. TagTracker turns those sightings into appear/disappear
// events, so swapping the PN7160 for another reader means writing one new
// class with this interface and changing the declaration in RFID_Reader.ino.
#pragma once
#include <Arduino.h>
#include "Config.h"

typedef void (*TagSeenFn)(uint8_t reader, const uint8_t* uid, uint8_t uidLen);

class TagSensor {
 public:
  virtual void begin(uint8_t index, ReaderConfig* rc, TagSeenFn onSeen) = 0;
  virtual void service(uint32_t nowMs) = 0;  // called every loop()
  virtual void restart() = 0;                // re-apply config and re-initialise
  virtual bool online() const = 0;
  virtual void printStatus(Print& out) = 0;
  virtual bool sendRaw(const uint8_t* pkt, uint8_t len) { return false; }  // debug, optional
};

extern TagSensor* sensors[MAX_READERS];  // defined in RFID_Reader.ino
