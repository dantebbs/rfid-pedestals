#include "UsbOut.h"
#include <HID.h>

// How long a write may wait for the host before the port counts as stalled.
// A host that has the port open reads every 1 ms frame, so this only runs
// out when nobody is reading.
static const uint32_t STALL_TIMEOUT_US = 20000;
// One byte under the 64-byte packet size: a full-size packet can sit in the
// PC's serial driver until a short packet follows, so every packet is short.
static const size_t PACKET = 63;

UsbSerialOut usbOut;

bool usbInReady(uint32_t ep) {
  if (!USBDevice.configured() || ep == 0 || ep > 9) return false;
  if (!(UOTGHS->UOTGHS_DEVEPT & (UOTGHS_DEVEPT_EPEN0 << ep))) return false;
  return UOTGHS->UOTGHS_DEVEPTISR[ep] & UOTGHS_DEVEPTISR_TXINI;
}

uint32_t usbRepairs = 0;

// The core's endpoint table (USBCore.cpp), zero-terminated, index = endpoint.
extern uint32_t EndPoints[];

void usbService() {
  if (!USBDevice.configured()) return;
  uint32_t want = 0;
  for (uint32_t ep = 1; ep < 10 && EndPoints[ep]; ep++) want |= UOTGHS_DEVEPT_EPEN0 << ep;
  if ((UOTGHS->UOTGHS_DEVEPT & want) == want) return;

  // Same steps as the core's SET_CONFIGURATION handler.
  __disable_irq();
  for (uint32_t ep = 1; ep < 10 && EndPoints[ep]; ep++) {
    if (UOTGHS->UOTGHS_DEVEPT & (UOTGHS_DEVEPT_EPEN0 << ep)) continue;
    UOTGHS->UOTGHS_DEVEPTCFG[ep] = EndPoints[ep];
    UOTGHS->UOTGHS_DEVEPT |= UOTGHS_DEVEPT_EPEN0 << ep;
  }
#ifdef CDC_RX
  const uint32_t rx = CDC_RX;
#else
  const uint32_t rx = 2;  // CDC_ENDPOINT_OUT in the Due core
#endif
  UOTGHS->UOTGHS_DEVEPTIER[rx] = UOTGHS_DEVEPTIER_RXOUTES;
  UOTGHS->UOTGHS_DEVIER = UOTGHS_DEVIER_PEP_0 << rx;
  __enable_irq();

  usbRepairs++;
}

uint32_t usbCdcEndpoint() {
#ifdef CDC_TX
  return CDC_TX;
#else
  return 3;  // CDC_ENDPOINT_IN in the Due core
#endif
}

// The HID module's endpoint is assigned at run time and is a protected member;
// a pointer-to-member formed through a derived class is the legal way to read it.
struct HidEndpointPeek : HID_ {
  static uint32_t get() { return HID().*(&HidEndpointPeek::pluggedEndpoint); }
};

uint32_t usbHidEndpoint() { return HidEndpointPeek::get(); }

size_t UsbSerialOut::write(const uint8_t* data, size_t len) {
  uint32_t ep = usbCdcEndpoint();
  size_t sent = 0;
  while (armed_ && sent < len) {
    if (!usbInReady(ep)) {
      if (stalled_ || !USBDevice.configured()) break;
      // Give a reading host a moment to empty the bank.
      uint32_t t0 = micros();
      while (!usbInReady(ep) && micros() - t0 < STALL_TIMEOUT_US) {
      }
      if (!usbInReady(ep)) {
        stalled_ = true;
        break;
      }
    }
    stalled_ = false;
    size_t n = len - sent < PACKET ? len - sent : PACKET;
    // Straight to the endpoint rather than SerialUSB.write(), which silently
    // discards everything unless the terminal has raised DTR. Several
    // terminals don't, which made the CLI look dead while it was still
    // running commands. If no terminal is reading, the endpoint fills and
    // the stall handling above drops output instead.
    USBD_Send(ep, data + sent, n);
    sent += n;
  }
  dropped += len - sent;
  return len;
}
