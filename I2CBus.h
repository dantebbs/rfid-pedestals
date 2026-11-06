// Register-level I2C master for the SAM3X TWI peripherals.
//
// The Due's Wire library buffers at most 32 bytes per transfer, but NCI
// packets carry up to 255 payload bytes, so reads and writes go straight to
// the TWI registers. Wire is only used to set up the pins and clock.
#pragma once
#include <Arduino.h>
#include <Wire.h>

class I2CBus {
 public:
  explicit I2CBus(uint8_t index);

  void begin(uint32_t hz);  // safe to call again, e.g. to change speed
  bool started() const { return started_; }

  bool write(uint8_t addr, const uint8_t* data, uint16_t len);
  bool read(uint8_t addr, uint8_t* data, uint16_t len);
  bool probe(uint8_t addr);

  uint8_t index() const { return index_; }
  uint32_t errors = 0;

 private:
  int waitFor(uint32_t mask);
  bool fail(int why);
  TwoWire& wire();

  uint8_t index_;
  Twi* twi_;
  uint32_t hz_ = 100000;
  bool started_ = false;
};

I2CBus& i2cBus(uint8_t index);  // 0 = Wire (pins 20/21), 1 = Wire1 (SDA1/SCL1)
