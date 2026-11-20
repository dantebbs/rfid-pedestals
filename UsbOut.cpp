#include "UsbOut.h"
#include <HID.h>
#include "Log.h"

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

// Hands one packet to IN endpoint `ep`, or returns false and sends nothing.
// The check and the send happen with interrupts off. The USB interrupt can
// switch an endpoint off at any moment (bus reset, or the core stalling a
// request it doesn't support); checking first and then calling the core's
// USBD_Send() left a gap where that happened in between, and the core's send
// then waits for a free bank forever. With the watchdog running that ends in
// a restart; before it, the sketch hung there with USB dead.
bool usbSendPacket(uint32_t ep, const uint8_t* data, size_t len) {
  if (ep == 0 || ep > 9 || len > 64) return false;
  bool sent = false;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (USBDevice.configured() && !(UOTGHS->UOTGHS_DEVISR & UOTGHS_DEVISR_EORST) &&
      (UOTGHS->UOTGHS_DEVEPT & (UOTGHS_DEVEPT_EPEN0 << ep)) &&
      (UOTGHS->UOTGHS_DEVEPTISR[ep] & UOTGHS_DEVEPTISR_TXINI)) {
    volatile uint8_t* fifo = (volatile uint8_t*)&udd_get_endpoint_fifo_access8(ep);
    for (size_t i = 0; i < len; i++) fifo[i] = data[i];
    UOTGHS->UOTGHS_DEVEPTICR[ep] = UOTGHS_DEVEPTICR_TXINIC;   // packet ready
    UOTGHS->UOTGHS_DEVEPTIDR[ep] = UOTGHS_DEVEPTIDR_FIFOCONC; // hand bank to USB
    sent = true;
  }
  __set_PRIMASK(primask);
  return sent;
}

uint32_t usbRepairs = 0;

// The core's endpoint table (USBCore.cpp), zero-terminated, index = endpoint.
extern uint32_t EndPoints[];

// Frame number when the endpoints were first seen switched off, or
// NO_FRAME while they are fine.
static const uint32_t NO_FRAME = 0xFFFFFFFF;
static uint32_t missingAtFrame = NO_FRAME;

enum : uint8_t { EV_RESET = 1, EV_SETUP, EV_SUSPEND, EV_RESUME, EV_REPAIR };  // USB trace, below
static void tracePoll();
static void traceAdd(uint8_t kind, uint8_t stalled, const volatile uint8_t* setup);

void usbService() {
  tracePoll();
  if (!USBDevice.configured()) {
    missingAtFrame = NO_FRAME;
    return;
  }
  uint32_t want = 0;
  for (uint32_t ep = 1; ep < 10 && EndPoints[ep]; ep++) want |= UOTGHS_DEVEPT_EPEN0 << ep;
  uint32_t have = UOTGHS->UOTGHS_DEVEPT;
  if ((have & want) == want) {
    missingAtFrame = NO_FRAME;
    return;
  }

  // A USB bus reset also switches the endpoints off, in hardware, the moment
  // it starts; the core only notices (and drops the configuration) when the
  // reset ends some 10 ms later. Putting the endpoints back inside that
  // window fought the hardware's reset: the endpoints came out of the reset
  // already enabled, with old banks and data toggles, before the PC had
  // configured the board. The PC then configures endpoints that are
  // already allocated; if the core's UDD_InitEndpoints() sees that fail it
  // spins forever inside the USB interrupt, otherwise the first transfers
  // after the reset fail and the PC gives up on the device, leaving only
  // the Programming port.
  // The PC sends no frames during a reset, so only repair once the frame
  // number has moved on since the endpoints were found off: then this was a
  // stall, not a reset.
  uint32_t fnum = UOTGHS->UOTGHS_DEVFNUM & 0x3FFF;
  if (missingAtFrame == NO_FRAME) {
    missingAtFrame = fnum;
    return;
  }
  if (fnum == missingAtFrame) return;

  // Same steps as the core's SET_CONFIGURATION handler.
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  bool fixed = false;
  if (USBDevice.configured() && !(UOTGHS->UOTGHS_DEVISR & (UOTGHS_DEVISR_EORST | UOTGHS_DEVISR_SUSP))) {
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
    fixed = true;
    traceAdd(EV_REPAIR, 0, nullptr);
  }
  __set_PRIMASK(primask);
  missingAtFrame = NO_FRAME;
  if (fixed) usbRepairs++;
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
    if (!usbSendPacket(ep, data + sent, n)) break;  // endpoint went away: drop the rest
    sent += n;
  }
  dropped += len - sent;
  return len;
}

// ---- USB event trace ------------------------------------------------------
// Records, from inside the USB interrupt, every bus reset and every control
// request the PC sends (the 8-byte setup packet, and whether the core
// rejected it), plus suspend/resume seen by the loop. usbTraceLog() prints
// what hasn't been printed yet; the link watch calls it when the link drops,
// so the Programming port log shows what the PC did just before.

extern "C" void (*gpf_isr)(void);  // libsam uotghs.c: the USB interrupt hook

struct UsbEvent {
  uint32_t ms;
  uint8_t kind;
  uint8_t stalled;
  uint8_t setup[8];
};
static const uint32_t TRACE_N = 32;
static UsbEvent trace[TRACE_N];
static volatile uint32_t traceCount = 0;  // events ever recorded
static uint32_t tracePrinted = 0;
static void (*coreIsr)(void) = nullptr;
static bool wasSuspended = false;

// Interrupts must be off, or this must run in the USB interrupt.
static void traceAdd(uint8_t kind, uint8_t stalled, const volatile uint8_t* setup) {
  UsbEvent& e = trace[traceCount % TRACE_N];
  e.ms = millis();
  e.kind = kind;
  e.stalled = stalled;
  for (int i = 0; i < 8; i++) e.setup[i] = setup ? setup[i] : 0;
  traceCount = traceCount + 1;
}

// The setup packet itself is NOT read here: the endpoint FIFO is read
// through a hardware pointer, so peeking at it consumed the packet before
// the core could read it, and the PC could no longer set the board up. Only
// what's visible in registers is recorded: that a request came in, its byte
// count, and whether the core rejected (stalled) it.
static void tracedIsr() {
  if (UOTGHS->UOTGHS_DEVISR & UOTGHS_DEVISR_EORST) traceAdd(EV_RESET, 0, nullptr);
  bool setup = UOTGHS->UOTGHS_DEVEPTISR[0] & UOTGHS_DEVEPTISR_RXSTPI;
  coreIsr();
  if (setup) traceAdd(EV_SETUP, (UOTGHS->UOTGHS_DEVEPTIMR[0] & UOTGHS_DEVEPTIMR_STALLRQ) ? 1 : 0, nullptr);
}

void usbTraceBegin() {
  __disable_irq();
  if (gpf_isr && gpf_isr != tracedIsr) {
    coreIsr = gpf_isr;
    gpf_isr = tracedIsr;
  }
  __enable_irq();
}

static void tracePoll() {
  bool susp = UOTGHS->UOTGHS_DEVISR & UOTGHS_DEVISR_SUSP;
  if (susp == wasSuspended) return;
  wasSuspended = susp;
  __disable_irq();
  traceAdd(susp ? EV_SUSPEND : EV_RESUME, 0, nullptr);
  __enable_irq();
}

void usbTraceLog() {
  uint32_t count = traceCount;
  if (count - tracePrinted > TRACE_N) tracePrinted = count - TRACE_N;
  if (tracePrinted == count) return;
  logf(LOG_EVENT, "USB trace, last %lu events:", (unsigned long)(count - tracePrinted));
  for (; tracePrinted < count; tracePrinted++) {
    __disable_irq();
    UsbEvent e = trace[tracePrinted % TRACE_N];
    __enable_irq();
    char buf[96];
    unsigned long sec = e.ms / 1000, ms = e.ms % 1000;
    switch (e.kind) {
      case EV_RESET: snprintf(buf, sizeof(buf), "  %6lu.%03lu bus reset by PC", sec, ms); break;
      case EV_SUSPEND: snprintf(buf, sizeof(buf), "  %6lu.%03lu bus suspended", sec, ms); break;
      case EV_RESUME: snprintf(buf, sizeof(buf), "  %6lu.%03lu bus resumed", sec, ms); break;
      case EV_REPAIR: snprintf(buf, sizeof(buf), "  %6lu.%03lu endpoints re-enabled", sec, ms); break;
      default:
        snprintf(buf, sizeof(buf), "  %6lu.%03lu request from PC%s", sec, ms,
                 e.stalled ? ", REJECTED by core" : "");
    }
    logf(LOG_EVENT, "%s", buf);
  }
}
