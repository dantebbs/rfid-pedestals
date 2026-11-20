// USB sends that never block.
//
// The Due core's USBD_Send() spins until the host empties the endpoint, with
// no timeout. If the host stops reading (terminal closed mid-print, port
// re-opened, PC asleep) the whole sketch freezes there: no CLI, no readers,
// no HID. Everything here checks the endpoint first and drops data instead.
#pragma once
#include <Arduino.h>

// True when IN endpoint `ep` has a free bank, so one packet of up to 64
// bytes can be sent now. Only a hint: use usbSendPacket() to send.
bool usbInReady(uint32_t ep);

// Sends one packet (up to 64 bytes) on IN endpoint `ep` if, at that very
// moment, the endpoint is enabled and has a free bank; otherwise sends
// nothing and returns false. Never waits. Use this instead of USBD_Send()
// or HID().SendReport(), which wait forever if the endpoint goes away.
bool usbSendPacket(uint32_t ep, const uint8_t* data, size_t len);

uint32_t usbCdcEndpoint();
uint32_t usbHidEndpoint();

// Works around a bug in the Due core (libsam UDD_Stall, 1.6.12): when the
// core rejects a control request it writes UOTGHS_DEVEPT = EP0 only, which
// switches off every other endpoint, i.e. the serial port and the HID
// device. Requests the core doesn't support (an unknown string descriptor,
// HID GET_IDLE or SET_REPORT, ...) can arrive at any time, typically when a
// program on the PC opens or enumerates the port. Call every loop(): it puts
// the endpoints back the way SET_CONFIGURATION left them.
void usbService();
extern uint32_t usbRepairs;

// USB event trace (bus resets, the PC's control requests, suspend/resume).
// usbTraceBegin() in setup(), after SerialUSB.begin(); usbTraceLog() prints
// the events recorded since it last ran.
void usbTraceBegin();
void usbTraceLog();  // how often the endpoints had to be restored

// Output side of the CDC port (input still comes from SerialUSB). Writes
// short packets straight to the endpoint, whether or not the terminal has
// raised DTR; if the host stops reading it drops output until the host
// catches up.
class UsbSerialOut : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t len) override;
  using Print::write;

  // Output is held back (dropped) until the terminal has sent something.
  // Otherwise the startup log sits in the USB endpoint from power-up until
  // the PC first opens the port, and that stale data broke the first
  // session after plugging in. The CLI arms this on the first byte received
  // and disarms it when the USB connection is reset.
  void arm() { armed_ = true; }
  void disarm() { armed_ = false; }
  bool armed() const { return armed_; }

  uint32_t dropped = 0;  // bytes thrown away while the host wasn't reading

 private:
  bool armed_ = false;
  bool stalled_ = false;
};

extern UsbSerialOut usbOut;
