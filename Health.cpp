#include "Health.h"
#include "Log.h"

// 8 s: long enough for the slowest command (a scan of a bus without pull-ups
// takes about 5 s, and it feeds the watchdog as it goes), short enough that
// the PC sees the board come back quickly.
static const uint32_t WATCHDOG_MS = 8000;

// Backup registers keep their value through every reset except power loss.
static const uint32_t FAULT_MAGIC = 0xFA170000;
static const uint32_t USB_LOST_MAGIC = 0x05B10000;  // | reason
static const uint32_t USB_LOST_MS = 10000;
static const uint32_t GAVE_UP_MS = 1000;
static const uint32_t SOF_GAP_MS = 1000;  // no frame for this long = PC not talking
#define BREADCRUMB GPBR->SYS_GPBR[0]
#define FAULT_MARK GPBR->SYS_GPBR[1]  // FAULT_MAGIC | stage, set by the fault handler
#define FAULT_PC GPBR->SYS_GPBR[2]

// ---- USB link watch -------------------------------------------------------

static bool everConfigured = false;
static bool wasConfigured = false;
static bool hostTalking = false;
static bool linkBad = false;
static uint32_t lastFrameMs = 0, lastFnum = 0xFFFFFFFF, badSinceMs = 0;
static uint32_t unconfigEvents = 0, frameStopEvents = 0;

static char resetText[80];
static bool watchdogOn = false;

// The core calls this before init() (main.cpp); its default disables the
// watchdog. The watchdog's mode register can only be written once.
void watchdogSetup(void) { watchdogEnable(WATCHDOG_MS); }

static const char* stageName(uint32_t s) {
  static char buf[16];
  if (s == STAGE_HEARTBEAT) return "heartbeat";
  if (s == STAGE_USB) return "USB service";
  if (s == STAGE_CLI) return "CLI";
  if (s >= STAGE_READERS && s < STAGE_READERS + 8) {
    snprintf(buf, sizeof(buf), "reader %lu", (unsigned long)(s - STAGE_READERS + 1));
    return buf;
  }
  if (s == STAGE_TRACKER) return "tag tracker";
  if (s == STAGE_HID) return "HID output";
  if (s == STAGE_HEALTH) return "health";
  return "setup";
}

void healthBegin() {
  watchdogOn = !(WDT->WDT_MR & WDT_MR_WDDIS);
  uint32_t type = (RSTC->RSTC_SR & RSTC_SR_RSTTYP_Msk) >> RSTC_SR_RSTTYP_Pos;
  uint32_t crumb = BREADCRUMB;
  bool fault = (FAULT_MARK & 0xFFFF0000) == FAULT_MAGIC;

  bool usbLost = (FAULT_MARK & 0xFFFF0000) == USB_LOST_MAGIC;
  if (usbLost) {
    snprintf(resetText, sizeof(resetText), "USB link lost (%s), restarted to reconnect",
             (FAULT_MARK & 0xFF) == 1 ? "PC stopped sending USB frames" : "PC left the board unconfigured");
  } else if (fault) {
    snprintf(resetText, sizeof(resetText), "crash (CPU fault at PC 0x%08lX, in %s)", (unsigned long)FAULT_PC,
             stageName(FAULT_MARK & 0xFF));
  } else if (type == 2) {
    // The loop stopped feeding the watchdog. If the stage is one that never
    // blocks, the chip was most likely frozen in the USB interrupt.
    snprintf(resetText, sizeof(resetText), "watchdog (the loop froze, last in %s)", stageName(crumb));
  } else if (type == 0) {
    snprintf(resetText, sizeof(resetText), "power-up");
  } else if (type == 4) {
    snprintf(resetText, sizeof(resetText), "reset pin (RESET button or Programming port opened)");
  } else if (type == 3) {
    snprintf(resetText, sizeof(resetText), "software reset");
  } else {
    snprintf(resetText, sizeof(resetText), "reset type %lu", (unsigned long)type);
  }
  FAULT_MARK = 0;
  lastFrameMs = millis();
  BREADCRUMB = 0;
}

void healthStage(uint8_t stage) { BREADCRUMB = stage; }

void healthKick() { watchdogReset(); }

static void watchUsb(uint32_t now) {
  // The frame number advances with every start-of-frame the PC sends (each
  // 1 ms, or 125 us at high speed), so it shows whether the PC is still
  // talking to the board.
  uint32_t fnum = UOTGHS->UOTGHS_DEVFNUM & 0x3FFF;
  if (fnum != lastFnum) {
    lastFnum = fnum;
    lastFrameMs = now;
  }
  bool talking = now - lastFrameMs < SOF_GAP_MS;
  bool configured = USBDevice.configured();

  if (configured != wasConfigured) {
    if (!configured) unconfigEvents++;
    logf(LOG_EVENT, "USB: %s", configured ? "configured by the PC" : "no longer configured");
    if (!configured) usbTraceLog();
    wasConfigured = configured;
  }
  if (talking != hostTalking) {
    if (!talking) frameStopEvents++;
    logf(LOG_EVENT, "USB: PC %s sending frames", talking ? "resumed" : "stopped");
    hostTalking = talking;
  }
  if (configured) everConfigured = true;

  bool ok = configured && talking;
  if (ok || !everConfigured) {
    if (linkBad) logf(LOG_EVENT, "USB: link OK again");
    linkBad = false;
    return;
  }
  if (!linkBad) {
    linkBad = true;
    badSinceMs = now;
    logf(LOG_EVENT, "USB: link lost, restarting in %lu s unless it recovers", (unsigned long)(USB_LOST_MS / 1000));
    return;
  }
  // Unconfigured and no frames: the PC has given up on the board (it
  // disables the port after failing to set it up again), so there is
  // nothing to wait for.
  uint32_t limit = (!configured && !talking) ? GAVE_UP_MS : USB_LOST_MS;
  if (now - badSinceMs < limit) return;

  logf(LOG_EVENT, "USB: link still lost, detaching and restarting");
  usbTraceLog();
  Serial.flush();
  FAULT_MARK = USB_LOST_MAGIC | (talking ? 2 : 1);
  delay(20);  // let the Programming port log drain
  UOTGHS->UOTGHS_DEVCTRL |= UOTGHS_DEVCTRL_DETACH;
  delay(200);  // long enough for the PC to see the board leave
  // Processor and peripheral reset; the board starts again from setup().
  RSTC->RSTC_CR = (0xA5u << 24) | (1u << 2) | (1u << 0);  // KEY | PERRST | PROCRST
  for (;;) {
  }
}

void healthService(uint32_t nowMs) {
  BREADCRUMB = STAGE_HEALTH;
  watchdogReset();
  watchUsb(nowMs);
}

void healthPrintUsb(Print& out) {
  char buf[120];
  snprintf(buf, sizeof(buf), "USB link: %s, PC %s; unconfigured %lu times, frames stopped %lu times",
           wasConfigured ? "configured" : "not configured", hostTalking ? "sending frames" : "silent",
           (unsigned long)unconfigEvents, (unsigned long)frameStopEvents);
  out.println(buf);
}

const char* healthResetText() { return resetText; }
bool healthWatchdogOn() { return watchdogOn; }

// A CPU fault normally parks the chip in an endless loop. Record where it
// happened, then wait for the watchdog to restart the board.
extern "C" void healthFault(uint32_t* frame) {
  FAULT_PC = frame[6];  // stacked PC
  FAULT_MARK = FAULT_MAGIC | (BREADCRUMB & 0xFF);
  for (;;) {
  }
}

extern "C" __attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
      "tst lr, #4      \n"
      "ite eq          \n"
      "mrseq r0, msp   \n"
      "mrsne r0, psp   \n"
      "b healthFault   \n");
}
extern "C" __attribute__((naked)) void BusFault_Handler(void) { __asm volatile("b HardFault_Handler\n"); }
extern "C" __attribute__((naked)) void UsageFault_Handler(void) { __asm volatile("b HardFault_Handler\n"); }
extern "C" __attribute__((naked)) void MemManage_Handler(void) { __asm volatile("b HardFault_Handler\n"); }
