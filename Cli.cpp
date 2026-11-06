#include "Cli.h"
#include "Config.h"
#include "HidOut.h"
#include "I2CBus.h"
#include "Log.h"
#include "TagSensor.h"
#include "TagTracker.h"
#include "TagMap.h"

#define CLI usbOut  // output; input is read from SerialUSB directly

static const char HELP[] =
    "Commands (settings take effect at once; 'save' keeps them across power cycles):\n"
    "  status                      readers, present tags and settings\n"
    "  tags                        tags currently present\n"
    "  scan                        list I2C devices on both buses\n"
    "  reset <1-5|all>             power-cycle and re-initialise reader(s)\n"
    "  reader <1-5> on|off\n"
    "  reader <1-5> bus <0|1>      0 = SDA/SCL (pins 20/21), 1 = SDA1/SCL1\n"
    "  reader <1-5> addr <hex>     0x28..0x2B, set by the HIF1/HIF2 pins\n"
    "  reader <1-5> irq <pin>\n"
    "  reader <1-5> ven <pin|none>\n"
    "  set remove <ms>             unseen this long -> '-' event (default 500)\n"
    "  set poll <ms>               PN7160 polling period (default 100)\n"
    "  set rfoff <ms>              field-off gap between presence checks (default 20)\n"
    "  set tech <letters>          any of A B F V (default ABV; F = FeliCa, old firmware only)\n"
    "  map                         show the tag map and the learned tags\n"
    "  map clear                   forget the 5 tags (key tag stays)\n"
    "  key learn                   next tag presented at any reader becomes the key tag\n"
    "  key clear                   forget the key tag\n"
    "  set output <map|id>         HID sends a map key (default) or '<reader>+<UID>'\n"
    "  set hid <kb|raw|both|off>   keyboard typing and/or vendor reports (default kb)\n"
    "  set enter <on|off>          id output: press Enter after each line (default on)\n"
    "  set kbdelay <ms>            keyboard: gap between key reports (default 2)\n"
    "  set log <0-4>               0 off, 1 events, 2 info, 3 debug, 4 NCI trace\n"
    "  set i2c <hz>                I2C clock, 100000 or 400000\n"
    "  save | load | defaults\n"
    "  hidtest <text>              send text over HID as if it were an event\n"
    "  nci <1-5> <hex bytes>       send a raw NCI packet (debug; see 'set log 4')\n";

static char line[160];
static uint8_t lineLen = 0;

static bool parseOnOff(const char* s, uint8_t& out) {
  if (!s) return false;
  if (!strcasecmp(s, "on") || !strcmp(s, "1")) { out = 1; return true; }
  if (!strcasecmp(s, "off") || !strcmp(s, "0")) { out = 0; return true; }
  return false;
}

static bool parseNum(const char* s, long lo, long hi, long& out) {
  if (!s) return false;
  char* end;
  out = strtol(s, &end, 0);
  return *end == 0 && end != s && out >= lo && out <= hi;
}

static int readerIndex(const char* s) {
  long n;
  return parseNum(s, 1, MAX_READERS, n) ? (int)n - 1 : -1;
}

static void restartAll() {
  for (uint8_t i = 0; i < MAX_READERS; i++) sensors[i]->restart();
}

static const char* hidModeName(uint8_t m) {
  switch (m) {
    case HID_KEYBOARD: return "kb";
    case HID_RAW: return "raw";
    case HID_BOTH: return "both";
  }
  return "off";
}

static void printSettings() {
  char buf[160];
  char tech[5];
  uint8_t n = 0;
  if (cfg.techMask & TECH_A) tech[n++] = 'A';
  if (cfg.techMask & TECH_B) tech[n++] = 'B';
  if (cfg.techMask & TECH_F) tech[n++] = 'F';
  if (cfg.techMask & TECH_V) tech[n++] = 'V';
  tech[n] = 0;
  snprintf(buf, sizeof(buf),
           "remove %u ms, poll %u ms, rfoff %u ms, tech %s, output %s, hid %s, enter %s, kbdelay %u ms, "
           "log %u, i2c %lu Hz",
           cfg.removeMs, cfg.pollPeriodMs, cfg.rfOffMs, tech, cfg.outputMode == OUT_ID ? "id" : "map", hidModeName(cfg.hidMode),
           cfg.hidEnter ? "on" : "off", cfg.kbDelayMs, cfg.logLevel, (unsigned long)cfg.i2cHz);
  CLI.println(buf);
}

static void cmdStatus() {
  for (uint8_t i = 0; i < MAX_READERS; i++) sensors[i]->printStatus(CLI);
  CLI.println("Tags:");
  trackerPrint(CLI);
  CLI.print("Settings: ");
  printSettings();
  char buf[140];
  snprintf(buf, sizeof(buf),
           "I2C errors: bus0 %lu, bus1 %lu; USB %s; serial bytes dropped %lu; USB endpoint repairs %lu",
           (unsigned long)i2cBus(0).errors, (unsigned long)i2cBus(1).errors,
           USBDevice.configured() ? "configured" : "not configured", (unsigned long)usbOut.dropped,
           (unsigned long)usbRepairs);
  CLI.println(buf);
}

static void cmdScan() {
  // Readers that are failing sit in power-down (VEN low) between retries and
  // wouldn't answer, so power every wired reader up for the scan. Afterwards
  // all readers start again from scratch.
  for (uint8_t i = 0; i < MAX_READERS; i++) {
    int8_t ven = cfg.reader[i].venPin;
    if (ven < 0) continue;
    pinMode(ven, OUTPUT);
    digitalWrite(ven, HIGH);
  }
  delay(20);  // datasheet boot time is 2.5 ms max
  for (uint8_t b = 0; b < 2; b++) {
    I2CBus& bus = i2cBus(b);
    if (!bus.started()) bus.begin(cfg.i2cHz);
    CLI.print("bus ");
    CLI.print(b);
    CLI.print(":");
    uint8_t found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
      if (bus.probe(a)) {
        char buf[8];
        snprintf(buf, sizeof(buf), " 0x%02X", a);
        CLI.print(buf);
        found++;
      }
    }
    CLI.println(found ? "" : " nothing");
  }
  restartAll();
  CLI.println("(readers re-initialising)");
}

static void cmdReader(char* args[], int argc) {
  int r = readerIndex(args[1]);
  if (r < 0 || argc < 3) {
    CLI.println("usage: reader <1-5> on|off|bus|addr|irq|ven ...");
    return;
  }
  ReaderConfig& rc = cfg.reader[r];
  const char* what = args[2];
  const char* val = argc > 3 ? args[3] : nullptr;
  long n;
  uint8_t b;
  if (parseOnOff(what, b)) {
    rc.enabled = b;
  } else if (!strcasecmp(what, "bus") && parseNum(val, 0, 1, n)) {
    rc.bus = n;
  } else if (!strcasecmp(what, "addr") && parseNum(val, 0x08, 0x77, n)) {
    if (n < 0x28 || n > 0x2B) CLI.println("note: the PN7160 only answers at 0x28..0x2B");
    rc.addr = n;
  } else if (!strcasecmp(what, "irq") && parseNum(val, 0, 78, n)) {
    rc.irqPin = n;
  } else if (!strcasecmp(what, "ven") && val && !strcasecmp(val, "none")) {
    rc.venPin = -1;
  } else if (!strcasecmp(what, "ven") && parseNum(val, 0, 78, n)) {
    rc.venPin = n;
  } else {
    CLI.println("bad value");
    return;
  }
  for (uint8_t i = 0; i < MAX_READERS; i++) {
    const ReaderConfig& o = cfg.reader[i];
    if (i != r && o.enabled && rc.enabled && o.bus == rc.bus && o.addr == rc.addr) {
      CLI.print("warning: reader ");
      CLI.print(i + 1);
      CLI.println(" uses the same bus and address");
    }
  }
  sensors[r]->restart();
  sensors[r]->printStatus(CLI);
}

static void cmdSet(char* args[], int argc) {
  const char* what = argc > 1 ? args[1] : "";
  const char* val = argc > 2 ? args[2] : nullptr;
  long n;
  uint8_t b;
  if (!strcasecmp(what, "remove") && parseNum(val, 50, 60000, n)) {
    cfg.removeMs = n;
  } else if (!strcasecmp(what, "poll") && parseNum(val, 10, 5000, n)) {
    cfg.pollPeriodMs = n;
    restartAll();
  } else if (!strcasecmp(what, "rfoff") && parseNum(val, 0, 1000, n)) {
    cfg.rfOffMs = n;
  } else if (!strcasecmp(what, "tech") && val) {
    uint8_t mask = 0;
    for (const char* c = val; *c; c++) {
      switch (toupper(*c)) {
        case 'A': mask |= TECH_A; break;
        case 'B': mask |= TECH_B; break;
        case 'F': mask |= TECH_F; break;
        case 'V': mask |= TECH_V; break;
        case ',': break;
        default: CLI.println("tech letters are A, B, F, V"); return;
      }
    }
    if (!mask) { CLI.println("need at least one technology"); return; }
    cfg.techMask = mask;
    restartAll();
  } else if (!strcasecmp(what, "hid") && val) {
    if (!strcasecmp(val, "kb")) cfg.hidMode = HID_KEYBOARD;
    else if (!strcasecmp(val, "raw")) cfg.hidMode = HID_RAW;
    else if (!strcasecmp(val, "both")) cfg.hidMode = HID_BOTH;
    else if (!strcasecmp(val, "off")) cfg.hidMode = HID_OFF;
    else { CLI.println("hid modes: kb raw both off"); return; }
  } else if (!strcasecmp(what, "output") && val && !strcasecmp(val, "map")) {
    cfg.outputMode = OUT_MAP;
  } else if (!strcasecmp(what, "output") && val && !strcasecmp(val, "id")) {
    cfg.outputMode = OUT_ID;
  } else if (!strcasecmp(what, "enter") && parseOnOff(val, b)) {
    cfg.hidEnter = b;
  } else if (!strcasecmp(what, "kbdelay") && parseNum(val, 1, 100, n)) {
    cfg.kbDelayMs = n;
  } else if (!strcasecmp(what, "log") && parseNum(val, 0, 4, n)) {
    cfg.logLevel = n;
  } else if (!strcasecmp(what, "i2c") && parseNum(val, 10000, 400000, n)) {
    cfg.i2cHz = n;
    for (uint8_t i = 0; i < 2; i++) {
      if (i2cBus(i).started()) i2cBus(i).begin(cfg.i2cHz);
    }
  } else {
    CLI.println("usage: set <remove|poll|rfoff|tech|output|hid|enter|kbdelay|log|i2c> <value>");
    return;
  }
  printSettings();
}

static void cmdNci(char* args[], int argc) {
  int r = readerIndex(argc > 1 ? args[1] : nullptr);
  if (r < 0 || argc < 5) {
    CLI.println("usage: nci <1-5> <hex bytes>, e.g. nci 1 20 00 01 00");
    return;
  }
  uint8_t pkt[64];
  uint8_t n = 0;
  for (int i = 2; i < argc && n < sizeof(pkt); i++) {
    char* end;
    long v = strtol(args[i], &end, 16);
    if (*end || v < 0 || v > 255) { CLI.println("bytes are hex, 00..FF"); return; }
    pkt[n++] = v;
  }
  if (!sensors[r]->sendRaw(pkt, n)) CLI.println("reader not ready");
}

static void execute(char* text) {
  char* args[24];
  int argc = 0;
  for (char* tok = strtok(text, " \t"); tok && argc < 24; tok = strtok(nullptr, " \t")) args[argc++] = tok;
  if (!argc) return;
  const char* cmd = args[0];

  if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
    CLI.print(HELP);
  } else if (!strcasecmp(cmd, "status")) {
    cmdStatus();
  } else if (!strcasecmp(cmd, "tags")) {
    trackerPrint(CLI);
  } else if (!strcasecmp(cmd, "scan")) {
    cmdScan();
  } else if (!strcasecmp(cmd, "reset")) {
    if (argc > 1 && !strcasecmp(args[1], "all")) {
      restartAll();
    } else {
      int r = readerIndex(argc > 1 ? args[1] : nullptr);
      if (r < 0) { CLI.println("usage: reset <1-5|all>"); return; }
      sensors[r]->restart();
    }
    CLI.println("ok");
  } else if (!strcasecmp(cmd, "reader")) {
    cmdReader(args, argc);
  } else if (!strcasecmp(cmd, "set")) {
    cmdSet(args, argc);
  } else if (!strcasecmp(cmd, "save")) {
    CLI.println(configSave() ? "saved" : "save FAILED");
  } else if (!strcasecmp(cmd, "load")) {
    CLI.println(configLoad() ? "loaded" : "no saved config, using defaults");
    restartAll();
  } else if (!strcasecmp(cmd, "defaults")) {
    trackerClear();
    configDefaults(cfg);
    restartAll();
    CLI.println("defaults restored (not saved)");
  } else if (!strcasecmp(cmd, "hidtest")) {
    // Re-join the rest of the line so spaces survive.
    char msg[64] = "";
    for (int i = 1; i < argc; i++) {
      if (i > 1) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
      strncat(msg, args[i], sizeof(msg) - strlen(msg) - 1);
    }
    hidSendLine(msg);
    CLI.println("sent");
  } else if (!strcasecmp(cmd, "map")) {
    if (argc > 1 && !strcasecmp(args[1], "clear")) tagMapClear();
    else tagMapPrint(CLI);
  } else if (!strcasecmp(cmd, "key")) {
    if (argc > 1 && !strcasecmp(args[1], "learn")) {
      tagMapLearnKey();
      CLI.println("present the key tag at any reader");
    } else if (argc > 1 && !strcasecmp(args[1], "clear")) {
      tagMapForgetKey();
      CLI.println("key tag forgotten");
    } else {
      CLI.println("usage: key learn | key clear");
    }
  } else if (!strcasecmp(cmd, "nci")) {
    cmdNci(args, argc);
  } else {
    CLI.println("unknown command, try 'help'");
  }
}

void cliService() {
  static char prev = 0;
  static bool wasConfigured = false;

  // A USB reset or unplug starts a new session: hold output until the
  // terminal speaks again.
  bool configured = USBDevice.configured();
  if (!configured && wasConfigured) usbOut.disarm();
  wasConfigured = configured;

  while (SerialUSB.available()) {
    char c = SerialUSB.read();
    if (!usbOut.armed()) {
      usbOut.arm();
      lineLen = 0;
      CLI.println();
      CLI.println("RFID Reader ready. Type 'help' for commands.");
    }
    bool crlf = (c == '\n' && prev == '\r');
    prev = c;
    if (c == '\r' || c == '\n') {
      if (crlf) continue;  // the \n of \r\n
      if (!lineLen) {      // bare Enter: answer with a prompt, so it's easy to
        CLI.print("> ");   // see the CLI is alive without echo
        continue;
      }
      line[lineLen] = 0;
      lineLen = 0;
      execute(line);
      CLI.print("> ");
    } else if (c == 0x08 || c == 0x7F) {
      if (lineLen) lineLen--;
    } else if (c >= 0x20 && lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;  // no echo: the terminal shows what was typed
    }
  }
}
