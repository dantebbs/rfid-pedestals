// Runtime configuration, editable from the CLI and stored in flash.
#pragma once
#include <Arduino.h>

#define MAX_READERS 5
#define MAX_UID_LEN 10

enum HidMode : uint8_t {
  HID_OFF = 0,
  HID_KEYBOARD = 1,  // types the event line as keystrokes
  HID_RAW = 2,       // one vendor-defined 32-byte input report per event
  HID_BOTH = 3,
};

// Technologies the readers poll for (Config::techMask).
enum TechBits : uint8_t {
  TECH_A = 0x01,  // ISO 14443A: MIFARE, NTAG, T2T, T4T
  TECH_B = 0x02,  // ISO 14443B
  TECH_F = 0x04,  // FeliCa (only PN7160 firmware 12.50.0A and older)
  TECH_V = 0x08,  // ISO 15693 / ICODE
};

// What a tag event sends over HID (Config::outputMode).
enum OutputMode : uint8_t {
  OUT_MAP = 0,  // one character from the reader/tag map (TagMap.h)
  OUT_ID = 1,   // "<reader><+|-><UID>"
};

enum LogLevel : uint8_t {
  LOG_OFF = 0,
  LOG_EVENT = 1,  // tag events and reader online/offline
  LOG_INFO = 2,   // plus errors and recoveries
  LOG_DEBUG = 3,  // plus every sighting and state change
  LOG_TRACE = 4,  // plus every NCI packet in hex
};

struct ReaderConfig {
  uint8_t enabled;
  uint8_t bus;     // 0 = Wire (SDA/SCL, pins 20/21), 1 = Wire1 (SDA1/SCL1)
  uint8_t addr;    // 7-bit I2C address, 0x28..0x2B (HIF1/HIF2 pins)
  int8_t irqPin;   // PN7160 IRQ output, active high
  int8_t venPin;   // PN7160 VEN (reset / power-down) input, -1 if not wired
};

struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  ReaderConfig reader[MAX_READERS];
  uint16_t removeMs;      // tag unseen this long -> "-" event
  uint16_t pollPeriodMs;  // NCI TOTAL_DURATION of the PN7160 polling loop
  uint16_t rfOffMs;       // RF field-off pause between presence checks
  uint8_t techMask;       // TechBits
  uint8_t hidMode;        // HidMode
  uint8_t outputMode;     // OutputMode (this byte held an old, unused setting, always 0)
  uint8_t hidEnter;       // keyboard mode: press Enter after each event
  uint8_t kbDelayMs;      // gap between keyboard reports
  uint8_t logLevel;       // LogLevel
  uint32_t i2cHz;
  uint32_t crc;           // must stay last
};

extern Config cfg;

void configDefaults(Config& c);
bool configLoad();  // false if flash held no valid config (defaults are used)
bool configSave();

// Erases and programs one 256-byte page of flash bank 1 (page 0..1023) with
// `len` bytes of `data` (rest 0xFF), then verifies it. Used by Config and TagMap.
bool flashWritePage(uint32_t page, const void* data, size_t len);
uint32_t flashPageAddr(uint32_t page);
