#include "Config.h"

// The Due has no EEPROM, so the config lives in the last 256-byte page of
// flash bank 1. The sketch runs from bank 0, so programming this page never
// stalls code fetch. Uploading a new sketch erases the whole flash, which
// puts the config back to defaults.
static const uint32_t CFG_MAGIC = 0x52464944;  // "RFID"
static const uint16_t CFG_VERSION = 1;
static const uint32_t FLASH_BANK1_ADDR = 0x000C0000;
static const uint32_t FLASH_BANK1_SIZE = 0x00040000;
static const uint32_t FLASH_PAGE_SIZE = 256;
static const uint32_t CFG_PAGE = FLASH_BANK1_SIZE / FLASH_PAGE_SIZE - 1;
static const uint32_t CFG_ADDR = FLASH_BANK1_ADDR + CFG_PAGE * FLASH_PAGE_SIZE;

static const uint32_t FCMD_EWP = 0x03;  // erase page and write page
static const uint32_t FCMD_CLB = 0x09;  // clear lock bit

static_assert(sizeof(Config) <= FLASH_PAGE_SIZE, "Config must fit one flash page");

Config cfg;

void configDefaults(Config& c) {
  memset(&c, 0, sizeof(c));
  c.magic = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.size = sizeof(Config);

  // Readers 1-4 share Wire at the four addresses the PN7160 offers. Reader 5
  // can't fit there, so it defaults to Wire1 (which has no on-board pull-ups).
  static const ReaderConfig defaults[MAX_READERS] = {
      {1, 0, 0x28, 22, 23},
      {1, 0, 0x29, 24, 25},
      {1, 0, 0x2A, 26, 27},
      {1, 0, 0x2B, 28, 29},
      {1, 1, 0x28, 30, 31},
  };
  memcpy(c.reader, defaults, sizeof(defaults));

  c.removeMs = 500;
  c.pollPeriodMs = 100;
  c.rfOffMs = 20;
  c.techMask = TECH_A | TECH_B | TECH_V;
  c.hidMode = HID_KEYBOARD;
  c.outputMode = OUT_MAP;
  c.hidEnter = 1;
  c.kbDelayMs = 2;
  c.logLevel = LOG_INFO;
  c.i2cHz = 100000;
}

static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  while (n--) {
    crc ^= *d++;
    for (uint8_t b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

static uint32_t configCrc(const Config& c) {
  return crc32((const uint8_t*)&c, offsetof(Config, crc));
}

bool configLoad() {
  Config stored;
  memcpy(&stored, (const void*)CFG_ADDR, sizeof(stored));
  if (stored.magic != CFG_MAGIC || stored.version != CFG_VERSION ||
      stored.size != sizeof(Config) || stored.crc != configCrc(stored)) {
    configDefaults(cfg);
    return false;
  }
  cfg = stored;
  return true;
}

static bool flashCommand(uint32_t cmd, uint32_t page) {
  while (!(EFC1->EEFC_FSR & EEFC_FSR_FRDY)) {
  }
  EFC1->EEFC_FCR = EEFC_FCR_FKEY(0x5A) | EEFC_FCR_FARG(page) | EEFC_FCR_FCMD(cmd);
  uint32_t status;
  while (!((status = EFC1->EEFC_FSR) & EEFC_FSR_FRDY)) {
  }
  return !(status & (EEFC_FSR_FCMDE | EEFC_FSR_FLOCKE));
}

uint32_t flashPageAddr(uint32_t page) { return FLASH_BANK1_ADDR + page * FLASH_PAGE_SIZE; }

bool flashWritePage(uint32_t pageNum, const void* data, size_t len) {
  if (len > FLASH_PAGE_SIZE || pageNum >= FLASH_BANK1_SIZE / FLASH_PAGE_SIZE) return false;
  uint32_t page[FLASH_PAGE_SIZE / 4];
  memset(page, 0xFF, sizeof(page));
  memcpy(page, data, len);

  if (!flashCommand(FCMD_CLB, pageNum)) return false;
  // Writing anywhere in the page fills the EFC latch buffer; EWP then
  // erases the page and programs it from that buffer.
  volatile uint32_t* dst = (volatile uint32_t*)flashPageAddr(pageNum);
  for (uint32_t i = 0; i < FLASH_PAGE_SIZE / 4; i++) dst[i] = page[i];
  if (!flashCommand(FCMD_EWP, pageNum)) return false;

  return memcmp((const void*)flashPageAddr(pageNum), data, len) == 0;
}

bool configSave() {
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.size = sizeof(Config);
  cfg.crc = configCrc(cfg);
  return flashWritePage(CFG_PAGE, &cfg, sizeof(cfg));
}
