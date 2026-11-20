// NXP PN7160/PN7161 NFC controller over I2C, spoken to in NCI 2.0.
//
// Presence detection, one reader at a time: every initialised reader waits
// with its RF field off. The readers take turns: the one whose turn it is
// starts discovery (RF on), and either reports a tag (RF_INTF_ACTIVATED_NTF
// for one tag, RF_DISCOVER_NTF for each of several) or finds nothing within
// one poll period; either way it switches RF off again and hands the turn
// on. Only one field is ever on, which keeps the supply current low. A tag
// that stays in the field is seen again on every turn, whatever its type,
// and a tag that leaves simply stops being seen.
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
    ST_DISCOVERING,  // RF on, this reader's turn: waiting for a tag
    ST_W4_DEACTIVATE_RSP,
    ST_RF_OFF,       // ready, RF off, waiting for this reader's turn
    ST_BACKOFF,      // after an error, before the next power cycle
  };

  void enter(State s, uint16_t timeoutMs = 0);
  void enterDisabled();
  void powerCycle();
  void fail(const char* why);

  bool irqActive();
  uint8_t resetTries_ = 0;   // CORE_RESET sends since this boot
  uint32_t lastPollMs_ = 0;  // "irq none": when the chip was last asked
  bool noData_ = false;      // last receive() found nothing waiting (polled mode)
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

  // RF turn-taking shared by all readers.
  bool myTurn() const;
  void releaseRf();
  static Pn7160Reader* all_[MAX_READERS];
  static int8_t rfOwner_;      // reader whose field is on, -1 if none
  static uint8_t lastTurn_;    // reader that had the last turn
  static uint32_t rfFreeSince_;
  uint32_t turnStart_ = 0;     // when this reader's last turn began
  uint32_t roundMs_ = 0;       // time between this reader's last two turns
  friend uint32_t sensorRoundMs();

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
