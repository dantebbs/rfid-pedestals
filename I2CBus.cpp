#include "I2CBus.h"

// Per-byte limit. The PN7160 stretches SCL while it wakes from standby, so
// this is generous; it only matters when a bus is stuck.
static const uint32_t BYTE_TIMEOUT_US = 20000;

static const int WAIT_NACK = -1;
static const int WAIT_TIMEOUT = -2;

I2CBus::I2CBus(uint8_t index) : index_(index), twi_(index == 0 ? WIRE_INTERFACE : WIRE1_INTERFACE) {}

TwoWire& I2CBus::wire() { return index_ == 0 ? Wire : Wire1; }

void I2CBus::begin(uint32_t hz) {
  hz_ = hz;
  wire().begin();  // pin mux, peripheral clock, TWI reset into master mode
  wire().setClock(hz_);
  started_ = true;
}

// Returns 0 once a status bit in `mask` is set.
int I2CBus::waitFor(uint32_t mask) {
  uint32_t t0 = micros();
  for (;;) {
    uint32_t sr = twi_->TWI_SR;
    if (sr & TWI_SR_NACK) return WAIT_NACK;
    if (sr & mask) return 0;
    if (micros() - t0 > BYTE_TIMEOUT_US) return WAIT_TIMEOUT;
  }
}

bool I2CBus::fail(int why) {
  errors++;
  if (why == WAIT_TIMEOUT) {
    // A stuck transfer leaves the TWI mid-frame; reset it so the next
    // transfer starts clean.
    twi_->TWI_CR = TWI_CR_STOP;
    begin(hz_);
  }
  return false;
}

bool I2CBus::write(uint8_t addr, const uint8_t* data, uint16_t len) {
  if (!started_ || len == 0) return false;
  int r;
  TWI_StartWrite(twi_, addr, 0, 0, data[0]);
  if ((r = waitFor(TWI_SR_TXRDY))) return fail(r);
  for (uint16_t i = 1; i < len; i++) {
    TWI_WriteByte(twi_, data[i]);
    if ((r = waitFor(TWI_SR_TXRDY))) return fail(r);
  }
  TWI_Stop(twi_);
  if ((r = waitFor(TWI_SR_TXCOMP))) return fail(r);
  return true;
}

bool I2CBus::read(uint8_t addr, uint8_t* data, uint16_t len) {
  if (!started_ || len == 0) return false;
  int r;
  TWI_StartRead(twi_, addr, 0, 0);
  for (uint16_t i = 0; i < len; i++) {
    // STOP has to be requested while the last byte is being received.
    if (i + 1 == len) TWI_SendSTOPCondition(twi_);
    if ((r = waitFor(TWI_SR_RXRDY))) return fail(r);
    data[i] = TWI_ReadByte(twi_);
  }
  if ((r = waitFor(TWI_SR_TXCOMP))) return fail(r);
  return true;
}

#ifndef TWI_CR_QUICK
#define TWI_CR_QUICK (0x1u << 6)
#endif

// Address-only write (an SMBus "quick command"), the same check i2cdetect
// uses: nothing reaches the device, and unlike a read it doesn't depend on
// the device having data to send. Tried twice because a PN7160 waking from
// standby NACKs the first attempt.
bool I2CBus::probe(uint8_t addr) {
  if (!started_) return false;
  uint32_t before = errors;
  // Long enough for START, 9 address bits and STOP, so TXCOMP below can't be
  // the "idle" TXCOMP from before the transfer began.
  uint32_t frameUs = 12 * 1000000UL / hz_ + 20;
  bool ok = false;
  for (uint8_t attempt = 0; attempt < 2 && !ok; attempt++) {
    if (attempt) delay(2);
    twi_->TWI_MMR = 0;
    twi_->TWI_MMR = TWI_MMR_DADR(addr);  // write direction
    twi_->TWI_IADR = 0;
    twi_->TWI_CR = TWI_CR_QUICK;
    delayMicroseconds(frameUs);
    int r = waitFor(TWI_SR_TXCOMP);
    if (r == WAIT_TIMEOUT) fail(r);
    ok = (r == 0);
  }
  errors = before;
  return ok;
}

I2CBus& i2cBus(uint8_t index) {
  static I2CBus buses[2] = {I2CBus(0), I2CBus(1)};
  return buses[index ? 1 : 0];
}
