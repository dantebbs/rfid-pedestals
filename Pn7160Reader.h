// NXP PN7160/PN7161 NFC controller over I2C, spoken to in NCI 2.0.
//
// Presence detection: the reader polls; when a tag is found the PN7160
// reports it (RF_INTF_ACTIVATED_NTF for one tag, RF_DISCOVER_NTF for each of
// several), we record the UID, switch the RF field off briefly and restart
// discovery. A tag that stays in the field is therefore seen again every
// cycle, whatever its type, and a tag that leaves simply stops being seen.
#pragma once
#include "TagSensor.h"
#include "I2CBus.h"

class Pn7160Reader : public TagSensor {
 public:
  void begin(uint8_t index, ReaderConfig* rc, TagSeenFn onSeen) override;
  void service(uint32_t nowMs) override;
  void restart() override;
  bool online() const override { return ready_; }
  void printStatus(Print& out) override;

  bool sendRaw(const uint8_t* pkt, uint8_t len) override;

 private:
  enum State : uint8_t {
    ST_DISABLED,
    ST_POWER_DOWN,  // VEN held low
    ST_BOOT,        // VEN released, waiting for the chip to boot
    ST_W4_RESET_RSP,
    ST_W4_RESET_NTF,
    ST_W4_INIT_RSP,
    ST_W4_PROP_RSP,
    ST_W4_CONFIG_RSP,
    ST_W4_MAP_RSP,
    ST_W4_DISCOVER_RSP,
    ST_DISCOVERING,  // polling; nothing to do until a tag shows up
    ST_W4_DEACTIVATE_RSP,
    ST_RF_OFF,       // field off between presence checks
    ST_BACKOFF,      // after an error, before the next power cycle
  };

  void enter(State s, uint16_t timeoutMs = 0);
  void enterDisabled();
  void powerCycle();
  void fail(const char* why);

  bool irqActive();
  bool receive();
  bool send(const uint8_t* pkt, uint8_t len);
  bool command(const uint8_t* pkt, uint8_t len, State next, uint16_t timeoutMs);

  void sendCoreReset();
  void sendCoreInit();
  void sendPropAct();
  void sendConfig();
  void sendMap();
  void sendDiscover();
  void sendDeactivate(bool expectNtf);

  void handlePacket();
  void onResponse(uint8_t status);
  void onNotification(uint8_t gid, uint8_t oid, const uint8_t* p, uint8_t len);
  void reportTag(uint8_t techMode, const uint8_t* tp, uint8_t tpLen);

  static const char* stateName(State s);

  uint8_t index_ = 0;
  ReaderConfig* rc_ = nullptr;
  TagSeenFn onSeen_ = nullptr;
  I2CBus* bus_ = nullptr;
  int8_t venPinInUse_ = -1;

  State state_ = ST_DISABLED;
  uint32_t now_ = 0;
  uint32_t stateSince_ = 0;
  uint16_t timeoutMs_ = 0;
  uint32_t backoffMs_ = 0;

  uint8_t expGid_ = 0, expOid_ = 0;
  bool gotResetNtf_ = false;
  bool deactNtfExpected_ = false;
  bool gotDeactNtf_ = false;
  bool ready_ = false;  // initialised and cycling discovery
  bool wasOnline_ = false;
  bool everOnline_ = false;

  uint8_t model_ = 0;
  uint8_t fw_[3] = {0, 0, 0};
  uint32_t sightings_ = 0;
  uint32_t errors_ = 0;
  uint8_t consecutiveErrors_ = 0;
  const char* lastError_ = "";

  uint8_t rx_[3 + 255];
};
