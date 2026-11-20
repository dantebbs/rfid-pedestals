#include "TagMap.h"
#include "Log.h"

static const char KEYS[MAX_READERS][2 * MAP_TAGS + 1] = {
    "qwertyuiop",
    "asdfghjkl;",
    "zxcvbnm,./",
    "QWERTYUIOP",
    "ASDFGHJKL:",
};

static const uint32_t KEY_WINDOW_MS = 5000;  // two key presentations within this erase the map
static const uint8_t HEX_LEN = 2 * MAX_UID_LEN + 1;

// Kept in the two pages below the settings page (1023) in flash bank 1, so
// saving settings, learning tags and learning key tags never overwrite each
// other.
static const uint32_t MAP_PAGE = 1022;
static const uint32_t KEY_PAGE = 1021;
static const uint32_t MAP_MAGIC = 0x544D4150;  // "TMAP"
static const uint32_t KEY_MAGIC = 0x544B4559;  // "TKEY"
static const uint16_t STORE_VERSION = 2;

struct MapStore {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  char tag[MAP_TAGS][HEX_LEN];   // empty: free column pair
  uint32_t crc;                  // must stay last
};
struct KeyStore {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  char key[MAP_KEYS][HEX_LEN];   // empty: no key tag in this slot
  uint32_t crc;                  // must stay last
};
static_assert(sizeof(MapStore) <= 256, "MapStore must fit one flash page");
static_assert(sizeof(KeyStore) <= 256, "KeyStore must fit one flash page");

static MapStore store;
static KeyStore keys;
static int8_t learningKey = -1;  // key slot waiting for a tag, -1: none
static int8_t lastKey = -1;      // key slot presented last, for the double-present erase
static uint32_t lastKeyMs = 0;

static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  while (n--) {
    crc ^= *d++;
    for (uint8_t b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

// Both stores start with magic/version/size and end with a CRC of the rest.
struct StoreHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
};

static void saveBlob(uint32_t page, uint32_t magic, void* blob, size_t size) {
  StoreHeader* h = (StoreHeader*)blob;
  h->magic = magic;
  h->version = STORE_VERSION;
  h->size = size;
  uint32_t crc = crc32((const uint8_t*)blob, size - 4);
  memcpy((uint8_t*)blob + size - 4, &crc, 4);
  if (!flashWritePage(page, blob, size)) logf(LOG_EVENT, "tag map: saving to flash FAILED");
}

static void loadBlob(uint32_t page, uint32_t magic, void* blob, size_t size) {
  memcpy(blob, (const void*)flashPageAddr(page), size);
  const StoreHeader* h = (const StoreHeader*)blob;
  uint32_t crc;
  memcpy(&crc, (const uint8_t*)blob + size - 4, 4);
  if (h->magic != magic || h->version != STORE_VERSION || h->size != size ||
      crc != crc32((const uint8_t*)blob, size - 4)) {
    memset(blob, 0, size);
  }
}

static void save() { saveBlob(MAP_PAGE, MAP_MAGIC, &store, sizeof(store)); }
static void saveKeys() { saveBlob(KEY_PAGE, KEY_MAGIC, &keys, sizeof(keys)); }

void tagMapBegin() {
  static_assert(offsetof(MapStore, crc) == sizeof(MapStore) - 4, "crc must be last");
  static_assert(offsetof(KeyStore, crc) == sizeof(KeyStore) - 4, "crc must be last");
  loadBlob(MAP_PAGE, MAP_MAGIC, &store, sizeof(store));
  loadBlob(KEY_PAGE, KEY_MAGIC, &keys, sizeof(keys));
  // Defensive: make sure every string is terminated.
  for (auto& t : store.tag) t[HEX_LEN - 1] = 0;
  for (auto& k : keys.key) k[HEX_LEN - 1] = 0;
}

static int findKey(const char* uid) {
  for (uint8_t i = 0; i < MAP_KEYS; i++)
    if (keys.key[i][0] && !strcmp(keys.key[i], uid)) return i;
  return -1;
}

static int findTag(const char* uid) {
  for (uint8_t i = 0; i < MAP_TAGS; i++)
    if (store.tag[i][0] && !strcmp(store.tag[i], uid)) return i;
  return -1;
}

static int assignTag(const char* uid) {
  for (uint8_t i = 0; i < MAP_TAGS; i++) {
    if (store.tag[i][0]) continue;
    strncpy(store.tag[i], uid, HEX_LEN - 1);
    save();
    logf(LOG_EVENT, "tag map: learned %s as tag %u", uid, i + 1);
    return i;
  }
  return -1;
}

void tagMapClear() {
  for (auto& t : store.tag) memset(t, 0, HEX_LEN);
  save();
  logf(LOG_EVENT, "tag map erased, the next 5 new tags will be learned");
}

void tagMapLearnKey(uint8_t slot) {
  if (slot >= MAP_KEYS) return;
  learningKey = slot;
  logf(LOG_EVENT, "present key tag %u at any reader", slot);
}

void tagMapForgetKey(uint8_t slot) {
  if (slot >= MAP_KEYS) return;
  if (learningKey == slot) learningKey = -1;
  if (lastKey == slot) lastKey = -1;
  memset(keys.key[slot], 0, HEX_LEN);
  saveKeys();
}

// The same key tag presented twice within KEY_WINDOW_MS erases the map.
static void handleKey(int slot, uint32_t now) {
  if (lastKey == slot && now - lastKeyMs <= KEY_WINDOW_MS) {
    lastKey = -1;
    tagMapClear();
    return;
  }
  lastKey = slot;
  lastKeyMs = now;
  logf(LOG_EVENT, "key tag %d presented; present it again within 5 s to erase the map", slot);
}

char tagMapEvent(bool appeared, uint8_t reader, const char* uid) {
  if (reader >= MAX_READERS) return 0;

  if (learningKey >= 0 && appeared) {
    uint8_t slot = learningKey;
    learningKey = -1;
    int i = findTag(uid);
    if (i >= 0) {  // a key tag can't also be a mapped tag
      memset(store.tag[i], 0, HEX_LEN);
      save();
    }
    int k = findKey(uid);
    if (k >= 0) memset(keys.key[k], 0, HEX_LEN);  // one tag, one key slot
    strncpy(keys.key[slot], uid, HEX_LEN - 1);
    saveKeys();
    lastKey = -1;
    logf(LOG_EVENT, "key tag %u set to %s", slot, uid);
    return 0;
  }

  int k = findKey(uid);
  if (k >= 0) {
    if (appeared) handleKey(k, millis());
    return 0;  // key tags never type anything
  }

  int i = findTag(uid);
  if (i < 0) {
    if (!appeared) return 0;  // left before it was learned (map was full)
    i = assignTag(uid);
    if (i < 0) {
      logf(LOG_EVENT, "tag map full, %s ignored (present a key tag twice to erase)", uid);
      return 0;
    }
  }
  return KEYS[reader][2 * i + (appeared ? 0 : 1)];
}

void tagMapPrint(Print& out) {
  char line[80];
  for (uint8_t k = 0; k < MAP_KEYS; k++) {
    snprintf(line, sizeof(line), "key %u: %s", k, keys.key[k][0] ? keys.key[k] : "(none)");
    out.println(line);
  }
  for (uint8_t i = 0; i < MAP_TAGS; i++) {
    snprintf(line, sizeof(line), "tag %u: %s", i + 1, store.tag[i][0] ? store.tag[i] : "(free)");
    out.println(line);
  }
  out.println("          tag1 tag2 tag3 tag4 tag5   (+ -)");
  for (uint8_t r = 0; r < MAX_READERS; r++) {
    char* p = line + snprintf(line, sizeof(line), "reader %u ", r + 1);
    for (uint8_t i = 0; i < MAP_TAGS; i++) p += sprintf(p, "  %c %c", KEYS[r][2 * i], KEYS[r][2 * i + 1]);
    out.println(line);
  }
}
