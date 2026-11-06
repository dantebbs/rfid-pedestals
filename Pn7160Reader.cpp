#include "Pn7160Reader.h"
#include "Log.h"

// NCI message types (top three bits of the first header byte).
static const uint8_t MT_DATA = 0;
static const uint8_t MT_CMD = 1;
static const uint8_t MT_RSP = 2;
static const uint8_t MT_NTF = 3;

// NCI RF technology-and-mode values for poll mode.
static const uint8_t NFC_A_POLL = 0x00;
static const uint8_t NFC_B_POLL = 0x01;
static const uint8_t NFC_F_POLL = 0x02;
static const uint8_t NFC_V_POLL = 0x06;

static const uint16_t VEN_LOW_MS = 20;      // datasheet minimum is 10 us
static const uint16_t BOOT_MS = 10;         // datasheet tboot is 2.5 ms max
static const uint16_t CMD_TIMEOUT_MS = 500;
static const uint16_t RESET_TIMEOUT_MS = 1000;
static const uint16_t DEACT_NTF_WAIT_MS = 200;
static const uint32_t BACKOFF_MIN_MS = 500;
static const uint32_t BACKOFF_MAX_MS = 10000;

void Pn7160Reader::begin(uint8_t index, ReaderConfig* rc, TagSeenFn onSeen) {
  index_ = index;
  rc_ = rc;
  onSeen_ = onSeen;
  restart();
}

void Pn7160Reader::restart() {
  bus_ = &i2cBus(rc_->bus);
  if (!bus_->started()) bus_->begin(cfg.i2cHz);
  if (rc_->irqPin >= 0) pinMode(rc_->irqPin, INPUT);
  // Let go of a VEN pin that is no longer ours (e.g. after "ven none"), so it
  // stops holding the module in power-down.
  if (venPinInUse_ >= 0 && venPinInUse_ != rc_->venPin) pinMode(venPinInUse_, INPUT);
  venPinInUse_ = rc_->venPin;
  if (rc_->venPin >= 0) pinMode(rc_->venPin, OUTPUT);
  consecutiveErrors_ = 0;
  backoffMs_ = BACKOFF_MIN_MS;
  if (rc_->enabled) {
    powerCycle();
  } else {
    enterDisabled();
  }
}

void Pn7160Reader::enter(State s, uint16_t timeoutMs) {
  if (s != state_) logf(LOG_DEBUG, "R%u %s", index_ + 1, stateName(s));
  state_ = s;
  stateSince_ = millis();
  timeoutMs_ = timeoutMs;
  if (s == ST_DISCOVERING) {
    ready_ = true;
  } else if (s < ST_W4_DISCOVER_RSP || s == ST_BACKOFF) {
    ready_ = false;
  }

  bool on = online();
  if (on && !wasOnline_) {
    logf(LOG_EVENT, "R%u online: %s fw %02X.%02X.%02X", index_ + 1, model_ == 0x71 ? "PN7161" : "PN7160", fw_[0],
         fw_[1], fw_[2]);
    everOnline_ = true;
    consecutiveErrors_ = 0;
    backoffMs_ = BACKOFF_MIN_MS;
  }
  wasOnline_ = on;
}

void Pn7160Reader::enterDisabled() {
  if (rc_->venPin >= 0) digitalWrite(rc_->venPin, LOW);
  if (wasOnline_) logf(LOG_EVENT, "R%u disabled", index_ + 1);
  enter(ST_DISABLED);
}

void Pn7160Reader::powerCycle() {
  if (rc_->venPin >= 0) {
    digitalWrite(rc_->venPin, LOW);
    enter(ST_POWER_DOWN);
  } else {
    // Without VEN the chip stays powered; CORE_RESET alone has to do.
    enter(ST_BOOT);
  }
}

void Pn7160Reader::fail(const char* why) {
  errors_++;
  consecutiveErrors_++;
  lastError_ = why;
  // Readers that aren't plugged in fail every retry; only say so once.
  uint8_t level = (wasOnline_ || (consecutiveErrors_ == 1 && everOnline_)) ? LOG_EVENT : LOG_INFO;
  if (consecutiveErrors_ > 1) level = LOG_DEBUG;
  logf(level, "R%u error: %s (in %s), retrying in %lu ms", index_ + 1, why, stateName(state_),
       (unsigned long)backoffMs_);
  if (wasOnline_) logf(LOG_EVENT, "R%u offline", index_ + 1);

  if (rc_->venPin >= 0) digitalWrite(rc_->venPin, LOW);
  enter(ST_BACKOFF, (uint16_t)backoffMs_);
  backoffMs_ = min(backoffMs_ * 2, BACKOFF_MAX_MS);
}

// IRQ is a level, not an edge: it stays high while the PN7160 has a packet
// waiting and drops once the packet has been read. Polling the level means a
// busy loop can never "miss" an interrupt.
bool Pn7160Reader::irqActive() {
  if (rc_->irqPin < 0) return false;
  return digitalRead(rc_->irqPin) == HIGH;
}

// NXP's reference driver reads the 3-byte NCI header and then the payload
// in a second transfer; the PN7160 expects exactly that.
bool Pn7160Reader::receive() {
  if (!bus_->read(rc_->addr, rx_, 3)) return false;
  uint8_t mt = rx_[0] >> 5;
  if (mt != MT_DATA && mt != MT_RSP && mt != MT_NTF) {
    logHex(LOG_INFO, "bad NCI header:", rx_, 3);
    return false;
  }
  if (rx_[2] && !bus_->read(rc_->addr, rx_ + 3, rx_[2])) return false;
  if (logOn(LOG_TRACE)) {
    char prefix[12];
    snprintf(prefix, sizeof(prefix), "R%u <<", index_ + 1);
    logHex(LOG_TRACE, prefix, rx_, 3 + rx_[2]);
  }
  return true;
}

bool Pn7160Reader::send(const uint8_t* pkt, uint8_t len) {
  if (logOn(LOG_TRACE)) {
    char prefix[12];
    snprintf(prefix, sizeof(prefix), "R%u >>", index_ + 1);
    logHex(LOG_TRACE, prefix, pkt, len);
  }
  if (bus_->write(rc_->addr, pkt, len)) return true;
  // The PN7160 NACKs its address while waking from standby; NXP's driver
  // retries once after a short pause.
  delay(5);
  return bus_->write(rc_->addr, pkt, len);
}

bool Pn7160Reader::command(const uint8_t* pkt, uint8_t len, State next, uint16_t timeoutMs) {
  expGid_ = pkt[0] & 0x0F;
  expOid_ = pkt[1] & 0x3F;
  if (!send(pkt, len)) {
    fail("I2C write failed");
    return false;
  }
  enter(next, timeoutMs);
  return true;
}

bool Pn7160Reader::sendRaw(const uint8_t* pkt, uint8_t len) {
  if (state_ == ST_DISABLED || state_ == ST_POWER_DOWN || state_ == ST_BACKOFF) return false;
  return send(pkt, len);
}

// ---- Initialisation sequence --------------------------------------------

void Pn7160Reader::sendCoreReset() {
  static const uint8_t CORE_RESET_CMD[] = {0x20, 0x00, 0x01, 0x01};  // reset config
  gotResetNtf_ = false;
  command(CORE_RESET_CMD, sizeof(CORE_RESET_CMD), ST_W4_RESET_RSP, RESET_TIMEOUT_MS);
}

void Pn7160Reader::sendCoreInit() {
  static const uint8_t CORE_INIT_CMD[] = {0x20, 0x01, 0x02, 0x00, 0x00};  // NCI 2.0
  command(CORE_INIT_CMD, sizeof(CORE_INIT_CMD), ST_W4_INIT_RSP, RESET_TIMEOUT_MS);
}

// Enables NXP's proprietary extensions, which MIFARE Classic support needs.
void Pn7160Reader::sendPropAct() {
  static const uint8_t PROP_ACT_CMD[] = {0x2F, 0x02, 0x00};
  command(PROP_ACT_CMD, sizeof(PROP_ACT_CMD), ST_W4_PROP_RSP, CMD_TIMEOUT_MS);
}

// TOTAL_DURATION: how often the polling loop repeats while no tag is found,
// which sets how quickly a newly placed tag is noticed.
void Pn7160Reader::sendConfig() {
  uint16_t ms = cfg.pollPeriodMs;
  uint8_t cmd[] = {0x20, 0x02, 0x05, 0x01, 0x00, 0x02, (uint8_t)(ms & 0xFF), (uint8_t)(ms >> 8)};
  command(cmd, sizeof(cmd), ST_W4_CONFIG_RSP, CMD_TIMEOUT_MS);
}

// Protocol-to-interface map from NXP's PN7160 examples: ISO-DEP to the
// ISO-DEP interface, MIFARE Classic to the tag-command interface, the rest
// to the frame interface. T5T falls back to the frame interface unmapped.
void Pn7160Reader::sendMap() {
  static const uint8_t MAP_CMD[] = {0x21, 0x00, 0x10, 0x05,
                                    0x01, 0x01, 0x01,   // T1T -> frame
                                    0x02, 0x01, 0x01,   // T2T -> frame
                                    0x03, 0x01, 0x01,   // T3T -> frame
                                    0x04, 0x01, 0x02,   // ISO-DEP -> ISO-DEP
                                    0x80, 0x01, 0x80};  // MIFARE Classic -> tag cmd
  command(MAP_CMD, sizeof(MAP_CMD), ST_W4_MAP_RSP, CMD_TIMEOUT_MS);
}

void Pn7160Reader::sendDiscover() {
  uint8_t cmd[3 + 1 + 2 * 4] = {0x21, 0x03, 0, 0};
  uint8_t n = 0;
  if (cfg.techMask & TECH_A) { cmd[4 + 2 * n] = NFC_A_POLL; cmd[5 + 2 * n++] = 0x01; }
  if (cfg.techMask & TECH_B) { cmd[4 + 2 * n] = NFC_B_POLL; cmd[5 + 2 * n++] = 0x01; }
  if (cfg.techMask & TECH_F) { cmd[4 + 2 * n] = NFC_F_POLL; cmd[5 + 2 * n++] = 0x01; }
  if (cfg.techMask & TECH_V) { cmd[4 + 2 * n] = NFC_V_POLL; cmd[5 + 2 * n++] = 0x01; }
  if (n == 0) {
    fail("no technologies enabled (set tech)");
    return;
  }
  cmd[2] = 1 + 2 * n;
  cmd[3] = n;
  command(cmd, 4 + 2 * n, ST_W4_DISCOVER_RSP, CMD_TIMEOUT_MS);
}

void Pn7160Reader::sendDeactivate(bool expectNtf) {
  static const uint8_t DEACTIVATE_IDLE_CMD[] = {0x21, 0x06, 0x01, 0x00};
  deactNtfExpected_ = expectNtf;
  gotDeactNtf_ = false;
  command(DEACTIVATE_IDLE_CMD, sizeof(DEACTIVATE_IDLE_CMD), ST_W4_DEACTIVATE_RSP, CMD_TIMEOUT_MS);
}

// ---- Main loop -----------------------------------------------------------

void Pn7160Reader::service(uint32_t nowMs) {
  now_ = nowMs;
  if (!rc_->enabled) {
    if (state_ != ST_DISABLED) enterDisabled();
    return;
  }
  if (state_ == ST_DISABLED) {
    restart();
    return;
  }

  // Read whatever is waiting, a few packets at most so one reader can't
  // starve the others.
  if (state_ != ST_POWER_DOWN && state_ != ST_BACKOFF) {
    for (uint8_t n = 0; n < 4 && irqActive(); n++) {
      if (!receive()) {
        fail("I2C read failed");
        return;
      }
      handlePacket();
      if (state_ == ST_BACKOFF) return;
    }
  }

  // millis(), not now_: enter() stamps stateSince_ with millis(), and a
  // packet handled above can move to a new state later in this same pass.
  // now_ - stateSince_ would then wrap to a huge value and time out at once.
  uint32_t elapsed = millis() - stateSince_;
  switch (state_) {
    case ST_DISABLED:
    case ST_DISCOVERING:
      break;
    case ST_POWER_DOWN:
      if (elapsed >= VEN_LOW_MS) {
        digitalWrite(rc_->venPin, HIGH);
        enter(ST_BOOT);
      }
      break;
    case ST_BOOT:
      if (elapsed >= BOOT_MS) sendCoreReset();
      break;
    case ST_RF_OFF:
      if (elapsed >= cfg.rfOffMs && (!deactNtfExpected_ || gotDeactNtf_ || elapsed >= DEACT_NTF_WAIT_MS)) {
        sendDiscover();
      }
      break;
    case ST_BACKOFF:
      if (elapsed >= timeoutMs_) powerCycle();
      break;
    default:  // waiting for a response or notification
      if (elapsed >= timeoutMs_) fail("no answer from reader");
      break;
  }
}

void Pn7160Reader::handlePacket() {
  uint8_t mt = rx_[0] >> 5;
  uint8_t gid = rx_[0] & 0x0F;
  uint8_t oid = rx_[1] & 0x3F;
  uint8_t len = rx_[2];
  const uint8_t* p = rx_ + 3;

  if (mt == MT_RSP) {
    bool waiting = state_ >= ST_W4_RESET_RSP && state_ <= ST_W4_DISCOVER_RSP && state_ != ST_W4_RESET_NTF;
    waiting = waiting || state_ == ST_W4_DEACTIVATE_RSP;
    if (waiting && gid == expGid_ && oid == expOid_) {
      onResponse(len ? p[0] : 0xFF);
    } else {
      logHex(LOG_DEBUG, "unexpected response:", rx_, 3 + len);
    }
  } else if (mt == MT_NTF) {
    onNotification(gid, oid, p, len);
  }
  // Data packets aren't used: tags are never read beyond their UID.
}

void Pn7160Reader::onResponse(uint8_t status) {
  switch (state_) {
    case ST_W4_RESET_RSP:
      if (status) return fail("CORE_RESET rejected");
      if (gotResetNtf_) {
        sendCoreInit();
      } else {
        enter(ST_W4_RESET_NTF, RESET_TIMEOUT_MS);
      }
      break;
    case ST_W4_INIT_RSP:
      if (status) return fail("CORE_INIT rejected");
      sendPropAct();
      break;
    case ST_W4_PROP_RSP:
      if (status) logf(LOG_INFO, "R%u proprietary activation status %02X", index_ + 1, status);
      sendConfig();
      break;
    case ST_W4_CONFIG_RSP:
      if (status) logf(LOG_INFO, "R%u CORE_SET_CONFIG status %02X", index_ + 1, status);
      sendMap();
      break;
    case ST_W4_MAP_RSP:
      if (status) logf(LOG_INFO, "R%u RF_DISCOVER_MAP status %02X", index_ + 1, status);
      sendDiscover();
      break;
    case ST_W4_DISCOVER_RSP:
      if (status) return fail("RF_DISCOVER rejected (check 'set tech')");
      enter(ST_DISCOVERING);
      break;
    case ST_W4_DEACTIVATE_RSP:
      // A rejection here means the chip already left discovery; restarting
      // discovery from RF_OFF is still the right next step.
      if (status) logf(LOG_DEBUG, "R%u RF_DEACTIVATE status %02X", index_ + 1, status);
      enter(ST_RF_OFF);
      break;
    default:
      break;
  }
}

void Pn7160Reader::onNotification(uint8_t gid, uint8_t oid, const uint8_t* p, uint8_t len) {
  if (gid == 0x00 && oid == 0x00) {  // CORE_RESET_NTF
    // Payload: reason, config status, NCI version, manufacturer id,
    // manufacturer info length, then model id and three firmware bytes.
    if (len >= 9) {
      model_ = p[5];
      memcpy(fw_, p + 6, 3);
    }
    if (state_ == ST_W4_RESET_RSP) {
      gotResetNtf_ = true;
    } else if (state_ == ST_W4_RESET_NTF) {
      sendCoreInit();
    } else {
      logf(state_ == ST_BOOT ? LOG_DEBUG : LOG_INFO, "R%u reset itself (reason %02X), re-initialising", index_ + 1,
           len ? p[0] : 0);
      sendCoreReset();
    }
    return;
  }
  if (gid == 0x00 && (oid == 0x07 || oid == 0x08)) {  // generic / interface error
    // Collisions and failed activations land here; the chip carries on
    // polling by itself.
    logf(LOG_DEBUG, "R%u %s error NTF %02X", index_ + 1, oid == 7 ? "generic" : "interface", len ? p[0] : 0);
    return;
  }
  if (gid == 0x01 && oid == 0x05) {  // RF_INTF_ACTIVATED_NTF: one tag, activated
    // discovery id, interface, protocol, tech&mode, max payload, credits,
    // tech params length, tech params...
    if (len >= 7 && 7 + p[6] <= len) reportTag(p[3], p + 7, p[6]);
    if (state_ == ST_DISCOVERING) sendDeactivate(true);
    return;
  }
  if (gid == 0x01 && oid == 0x03) {  // RF_DISCOVER_NTF: one of several tags
    // discovery id, protocol, tech&mode, tech params length, tech params,
    // notification type (2 = more to come).
    if (len < 4 || 4 + p[3] >= len) return;
    reportTag(p[2], p + 4, p[3]);
    uint8_t notifType = p[4 + p[3]];
    if (notifType != 2 && state_ == ST_DISCOVERING) sendDeactivate(false);
    return;
  }
  if (gid == 0x01 && oid == 0x06) {  // RF_DEACTIVATE_NTF
    gotDeactNtf_ = true;
    return;
  }
  if (logOn(LOG_DEBUG)) {
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "R%u other NTF:", index_ + 1);
    logHex(LOG_DEBUG, prefix, rx_, 3 + len);
  }
}

// Pulls the UID out of the NCI "RF technology specific parameters" block.
void Pn7160Reader::reportTag(uint8_t techMode, const uint8_t* tp, uint8_t tpLen) {
  const uint8_t* uid = nullptr;
  uint8_t uidLen = 0;
  switch (techMode) {
    case NFC_A_POLL:  // SENS_RES(2), NFCID1 length, NFCID1, SEL_RES length, SEL_RES
      if (tpLen >= 3 && 3 + tp[2] <= tpLen) {
        uid = tp + 3;
        uidLen = tp[2];
      }
      break;
    case NFC_B_POLL:  // SENSB_RES length, SENSB_RES (starts with the 4-byte NFCID0)
      if (tpLen >= 5 && tp[0] >= 4) {
        uid = tp + 1;
        uidLen = 4;
      }
      break;
    case NFC_F_POLL:  // bit rate, SENSF_RES length, SENSF_RES (starts with the 8-byte NFCID2)
      if (tpLen >= 10 && tp[1] >= 8) {
        uid = tp + 2;
        uidLen = 8;
      }
      break;
    case NFC_V_POLL:  // RES_FLAG, DSFID, UID(8) as sent over the air (LSB first)
      if (tpLen >= 10) {
        uid = tp + 2;
        uidLen = 8;
      }
      break;
  }
  if (!uid || uidLen == 0 || uidLen > MAX_UID_LEN) {
    logf(LOG_DEBUG, "R%u tag with unreadable UID (tech %02X)", index_ + 1, techMode);
    return;
  }
  sightings_++;
  if (onSeen_) onSeen_(index_, uid, uidLen);
}

// ---- Status --------------------------------------------------------------

const char* Pn7160Reader::stateName(State s) {
  switch (s) {
    case ST_DISABLED: return "disabled";
    case ST_POWER_DOWN: return "power-down";
    case ST_BOOT: return "boot";
    case ST_W4_RESET_RSP: return "wait CORE_RESET_RSP";
    case ST_W4_RESET_NTF: return "wait CORE_RESET_NTF";
    case ST_W4_INIT_RSP: return "wait CORE_INIT_RSP";
    case ST_W4_PROP_RSP: return "wait PROP_ACT_RSP";
    case ST_W4_CONFIG_RSP: return "wait SET_CONFIG_RSP";
    case ST_W4_MAP_RSP: return "wait DISCOVER_MAP_RSP";
    case ST_W4_DISCOVER_RSP: return "wait DISCOVER_RSP";
    case ST_DISCOVERING: return "discovering";
    case ST_W4_DEACTIVATE_RSP: return "wait DEACTIVATE_RSP";
    case ST_RF_OFF: return "rf-off";
    case ST_BACKOFF: return "backoff";
  }
  return "?";
}

void Pn7160Reader::printStatus(Print& out) {
  char line[160];
  char ven[8];
  if (rc_->venPin >= 0) {
    snprintf(ven, sizeof(ven), "%d", rc_->venPin);
  } else {
    strcpy(ven, "none");
  }
  snprintf(line, sizeof(line), "R%u %-3s bus %u addr 0x%02X irq %d ven %-4s  %-8s %-20s", index_ + 1,
           rc_->enabled ? "on" : "off", rc_->bus, rc_->addr, rc_->irqPin, ven, online() ? "ONLINE" : "offline",
           stateName(state_));
  out.print(line);
  if (model_) {
    snprintf(line, sizeof(line), "  %s fw %02X.%02X.%02X", model_ == 0x71 ? "PN7161" : "PN7160", fw_[0], fw_[1],
             fw_[2]);
    out.print(line);
  }
  snprintf(line, sizeof(line), "  seen %lu  errors %lu", (unsigned long)sightings_, (unsigned long)errors_);
  out.print(line);
  if (errors_ && !online()) {
    out.print("  last: ");
    out.print(lastError_);
  }
  out.println();
}
