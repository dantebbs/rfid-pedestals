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

// Kept in the page just below the settings page in flash bank 1, so saving
// settings and learning tags never overwrite each other.
static const uint32_t MAP_PAGE = 1022;
static const uint32_t MAP_MAGIC = 0x544D4150;  // "TMAP"
static const uint16_t MAP_VERSION = 1;

struct MapStore {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  char key[HEX_LEN];             // empty: no key tag
  char tag[MAP_TAGS][HEX_LEN];   // empty: free column pair
  uint32_t crc;                  // must stay last
};
static_assert(sizeof(MapStore) <= 256, "MapStore must fit one flash page");

static MapStore store;
static bool learningKey = false;
static uint32_t lastKeyMs = 0;
static bool keySeenOnce = false;

static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t crc = 0xFFFFFFFF;
  while (n--) {
    crc ^= *d++;
    for (uint8_t b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

static uint32_t storeCrc(const MapStore& s) { return crc32((const uint8_t*)&s, offsetof(MapStore, crc)); }

static void save() {
  store.magic = MAP_MAGIC;
  store.version = MAP_VERSION;
  store.size = sizeof(MapStore);
  store.crc = storeCrc(store);
  if (!flashWritePage(MAP_PAGE, &store, sizeof(store))) logf(LOG_EVENT, "tag map: saving to flash FAILED");
}

void tagMapBegin() {
  memcpy(&store, (const void*)flashPageAddr(MAP_PAGE), sizeof(store));
  if (store.magic != MAP_MAGIC || store.version != MAP_VERSION || store.size != sizeof(MapStore) ||
      store.crc != storeCrc(store)) {
    memset(&store, 0, sizeof(store));
  }
  // Defensive: make sure every string is terminated.
  store.key[HEX_LEN - 1] = 0;
  for (auto& t : store.tag) t[HEX_LEN - 1] = 0;
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

void tagMapLearnKey() {
  learningKey = true;
  logf(LOG_EVENT, "present the key tag at any reader");
}

void tagMapForgetKey() {
  learningKey = false;
  memset(store.key, 0, HEX_LEN);
  save();
}

static void handleKey(uint32_t now) {
  if (keySeenOnce && now - lastKeyMs <= KEY_WINDOW_MS) {
    keySeenOnce = false;
    tagMapClear();
    return;
  }
  keySeenOnce = true;
  lastKeyMs = now;
  logf(LOG_EVENT, "key tag presented; present it again within 5 s to erase the map");
}

char tagMapEvent(bool appeared, uint8_t reader, const char* uid) {
  if (reader >= MAX_READERS) return 0;

  if (learningKey && appeared) {
    learningKey = false;
    int i = findTag(uid);
    if (i >= 0) memset(store.tag[i], 0, HEX_LEN);  // a key tag can't also be a mapped tag
    strncpy(store.key, uid, HEX_LEN - 1);
    save();
    keySeenOnce = false;
    logf(LOG_EVENT, "key tag set to %s", uid);
    return 0;
  }

  if (store.key[0] && !strcmp(store.key, uid)) {
    if (appeared) handleKey(millis());
    return 0;  // the key tag never types anything
  }

  int i = findTag(uid);
  if (i < 0) {
    if (!appeared) return 0;  // left before it was learned (map was full)
    i = assignTag(uid);
    if (i < 0) {
      logf(LOG_EVENT, "tag map full, %s ignored (present the key tag twice to erase)", uid);
      return 0;
    }
  }
  return KEYS[reader][2 * i + (appeared ? 0 : 1)];
}

void tagMapPrint(Print& out) {
  char line[80];
  snprintf(line, sizeof(line), "key tag: %s", store.key[0] ? store.key : "(none, use 'key learn')");
  out.println(line);
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
