#include "HidOut.h"
#include <HID.h>
#include "Config.h"
#include "Log.h"
#include "UsbOut.h"

static const uint8_t REPORT_KEYBOARD = 2;
static const uint8_t REPORT_RAW = 3;
static const uint8_t RAW_SIZE = 32;

static const uint8_t hidDescriptor[] = {
    // Keyboard
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_KEYBOARD,
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute): modifiers
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Constant): reserved
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x73,        //   Logical Maximum (115)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x73,        //   Usage Maximum (115)
    0x81, 0x00,        //   Input (Data, Array): key codes
    0xC0,              // End Collection

    // Vendor-defined tag events
    0x06, 0x00, 0xFF,  // Usage Page (Vendor 0xFF00)
    0x09, 0x01,        // Usage (1)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_RAW,
    0x09, 0x01,        //   Usage (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, RAW_SIZE,    //   Report Count (32)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection
};

// The descriptor has to be registered before USB enumeration, which the Due
// core starts before setup(), so it's done from a static constructor (the
// same way Arduino's Keyboard library does it).
static struct HidRegistrar {
  HidRegistrar() {
    static HIDSubDescriptor node(hidDescriptor, sizeof(hidDescriptor));
    HID().AppendDescriptor(&node);
  }
} hidRegistrar;

// ---- Keyboard typing -----------------------------------------------------

static const uint8_t MOD_LSHIFT = 0x02;

// US layout. Covers hex digits, '+', '-', Enter and every map key (TagMap.cpp).
static bool asciiToKey(char c, uint8_t& mod, uint8_t& key) {
  mod = 0;
  if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { key = 0x04 + (c - 'A'); mod = MOD_LSHIFT; return true; }
  if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); return true; }
  switch (c) {
    case '0': key = 0x27; return true;
    case '\n': key = 0x28; return true;
    case ' ': key = 0x2C; return true;
    case '-': key = 0x2D; return true;
    case '_': key = 0x2D; mod = MOD_LSHIFT; return true;
    case '=': key = 0x2E; return true;
    case '+': key = 0x2E; mod = MOD_LSHIFT; return true;
    case ';': key = 0x33; return true;
    case ':': key = 0x33; mod = MOD_LSHIFT; return true;
    case ',': key = 0x36; return true;
    case '.': key = 0x37; return true;
    case '/': key = 0x38; return true;
  }
  return false;
}

static char queue[512];
static uint16_t qHead = 0, qTail = 0;  // head = next to type
static bool keyDown = false;
static uint32_t lastReportMs = 0;

static uint16_t queueFree() { return sizeof(queue) - 1 - (uint16_t)((qTail - qHead + sizeof(queue)) % sizeof(queue)); }

static void queueLine(const char* line, bool enter) {
  size_t n = strlen(line) + (enter ? 1 : 0);
  if (n > queueFree()) {
    logf(LOG_INFO, "keyboard queue full, dropped \"%s\"", line);
    return;
  }
  for (const char* c = line; *c; c++) {
    queue[qTail] = *c;
    qTail = (qTail + 1) % sizeof(queue);
  }
  if (enter) {
    queue[qTail] = '\n';
    qTail = (qTail + 1) % sizeof(queue);
  }
}

// Every HID().SendReport() below is preceded by usbInReady(): the core's
// send blocks until the host takes the report, which froze the sketch.
static bool hidReady() { return usbInReady(usbHidEndpoint()); }

static void sendKey(uint8_t mod, uint8_t key) {
  uint8_t report[8] = {mod, 0, key, 0, 0, 0, 0, 0};
  HID().SendReport(REPORT_KEYBOARD, report, sizeof(report));
}

// Raw reports wait here for a free endpoint instead of blocking.
static const uint8_t RAW_QUEUE = 8;
static char rawQueue[RAW_QUEUE][RAW_SIZE];
static uint8_t rawHead = 0, rawCount = 0;

// Backlog is dropped if the host hasn't taken a report for this long
// (host asleep, HID driver not started).
static const uint32_t HID_STALL_MS = 1000;
static uint32_t lastProgressMs = 0;

bool hidBusy() { return qHead != qTail || keyDown || rawCount; }

static void dropBacklog(const char* why) {
  if (hidBusy()) logf(LOG_INFO, "HID output dropped: %s", why);
  qHead = qTail;
  keyDown = false;
  rawCount = 0;
}

void hidService(uint32_t nowMs) {
  if (!hidBusy()) {
    lastProgressMs = nowMs;
    return;
  }
  if (!USBDevice.configured()) return dropBacklog("no USB host");
  if (!hidReady()) {
    if (nowMs - lastProgressMs >= HID_STALL_MS) dropBacklog("host is not reading the HID device");
    return;
  }
  lastProgressMs = nowMs;

  if (rawCount) {  // raw reports are single packets: send at once
    HID().SendReport(REPORT_RAW, rawQueue[rawHead], RAW_SIZE);
    rawHead = (rawHead + 1) % RAW_QUEUE;
    rawCount--;
    return;
  }

  if (nowMs - lastReportMs < cfg.kbDelayMs) return;
  lastReportMs = nowMs;
  if (keyDown) {  // every key is released before the next is pressed
    sendKey(0, 0);
    keyDown = false;
    return;
  }
  if (qHead == qTail) return;
  char c = queue[qHead];
  qHead = (qHead + 1) % sizeof(queue);
  uint8_t mod, key;
  if (asciiToKey(c, mod, key)) {
    sendKey(mod, key);
    keyDown = true;
  }
}

// ---- Raw reports ---------------------------------------------------------

static void queueRaw(const char* line) {
  if (!USBDevice.configured()) return;
  if (rawCount == RAW_QUEUE) {
    logf(LOG_INFO, "raw HID queue full, dropped \"%s\"", line);
    return;
  }
  char* slot = rawQueue[(rawHead + rawCount) % RAW_QUEUE];
  memset(slot, 0, RAW_SIZE);
  strncpy(slot, line, RAW_SIZE);
  rawCount++;
}

void hidSendLine(const char* line) { hidSendText(line, cfg.hidEnter); }

void hidSendText(const char* line, bool enter) {
  if (cfg.hidMode == HID_KEYBOARD || cfg.hidMode == HID_BOTH) queueLine(line, enter);
  if (cfg.hidMode == HID_RAW || cfg.hidMode == HID_BOTH) queueRaw(line);
}
