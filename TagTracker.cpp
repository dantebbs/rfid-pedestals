#include "TagTracker.h"
#include "Log.h"

static const uint8_t MAX_TAGS = 24;

struct Entry {
  bool used;
  uint8_t reader;  // last reader that saw it
  uint8_t len;
  uint8_t uid[MAX_UID_LEN];
  uint32_t lastSeen;
};

static Entry tags[MAX_TAGS];
static TagEventFn onEvent_ = nullptr;

static void emit(bool appeared, const Entry& e) {
  char hex[2 * MAX_UID_LEN + 1];
  toHex(e.uid, e.len, hex);
  if (onEvent_) onEvent_(appeared, e.reader, hex);
}

static Entry* find(uint8_t reader, const uint8_t* uid, uint8_t len) {
  for (Entry& e : tags) {
    if (!e.used || e.len != len || memcmp(e.uid, uid, len) != 0) continue;
    if (e.reader != reader) continue;  // each reader tracks its own tags
    return &e;
  }
  return nullptr;
}

void trackerBegin(TagEventFn onEvent) {
  onEvent_ = onEvent;
  memset(tags, 0, sizeof(tags));
}

void trackerSeen(uint8_t reader, const uint8_t* uid, uint8_t len) {
  if (logOn(LOG_DEBUG)) {
    char hex[2 * MAX_UID_LEN + 1];
    toHex(uid, len, hex);
    logf(LOG_DEBUG, "R%u sees %s", reader + 1, hex);
  }
  uint32_t now = millis();
  Entry* e = find(reader, uid, len);
  if (e) {
    e->lastSeen = now;
    e->reader = reader;
    return;
  }
  for (Entry& slot : tags) {
    if (slot.used) continue;
    slot.used = true;
    slot.reader = reader;
    slot.len = len;
    memcpy(slot.uid, uid, len);
    slot.lastSeen = now;
    emit(true, slot);
    return;
  }
  logf(LOG_INFO, "tag table full (%u tags), ignoring new tag", MAX_TAGS);
}

void trackerService(uint32_t nowMs) {
  for (Entry& e : tags) {
    // Signed: a reader may have stamped a sighting after nowMs was taken.
    if (e.used && (int32_t)(nowMs - e.lastSeen) >= (int32_t)cfg.removeMs) {
      e.used = false;
      emit(false, e);
    }
  }
}

void trackerClear() {
  for (Entry& e : tags) {
    if (!e.used) continue;
    e.used = false;
    emit(false, e);
  }
}

void trackerPrint(Print& out) {
  uint32_t now = millis();
  uint8_t n = 0;
  char line[64];
  for (const Entry& e : tags) {
    if (!e.used) continue;
    char hex[2 * MAX_UID_LEN + 1];
    toHex(e.uid, e.len, hex);
    snprintf(line, sizeof(line), "  %-20s R%u  seen %lu ms ago", hex, e.reader + 1,
             (unsigned long)(now - e.lastSeen));
    out.println(line);
    n++;
  }
  if (!n) out.println("  (no tags present)");
}
