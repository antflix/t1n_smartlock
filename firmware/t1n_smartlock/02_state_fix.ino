// ---------------- State / command safety fix ----------------
// This file intentionally lives after 01_ble.ino and before 03/04.
// Use built-in return types so Arduino's auto-generated prototypes compile.

static constexpr uint32_t SAFE_UNLOCK_OBSERVE_MS = 750;
static constexpr uint32_t SAFE_PENDING_COMMAND_MS = 15000;
static constexpr uint32_t SAFE_COMMAND_AWAKE_EDGES_MIN = 150; // ~2.3s command+settle window

static constexpr uint8_t SAFE_CMD_BLOCKED = 0;
static constexpr uint8_t SAFE_CMD_CONFIRMED = 1;
static constexpr uint8_t SAFE_CMD_SENT = 2;

VehicleLockState safePendingTarget = VEH_UNKNOWN;
uint32_t safePendingSinceMs = 0;
uint32_t safeUnlockObservedSinceMs = 0;

uint32_t safeCtmEdgeSnapshot() {
  uint32_t v;
  portENTER_CRITICAL(&ctmMux);
  v = ctmEdgeTotal;
  portEXIT_CRITICAL(&ctmMux);
  return v;
}

void safeClearPendingIfConfirmed() {
  if (safePendingTarget != VEH_UNKNOWN && lastKnownLockState == safePendingTarget) {
    addLog(String("[SAFE] pending ") +
           (safePendingTarget == VEH_LOCKED ? "LOCK" : "UNLOCK") +
           " physically confirmed");
    safePendingTarget = VEH_UNKNOWN;
    safePendingSinceMs = 0;
  }
}

void safeMarkUnknown(const char* why) {
  if (lastKnownLockState != VEH_UNKNOWN) {
    lastKnownLockState = VEH_UNKNOWN;
    EEPROM.writeChar(ADDR_LAST_LOCK, (int8_t)VEH_UNKNOWN);
    EEPROM.commit();
    addLog(String("[SAFE] state -> UNKNOWN: ") + why);
  }
  pendingLockState = -1;
  pendingLockSinceMs = 0;
}

void updateRememberedLockFromLedSafe() {
  uint32_t now = millis();

  if (drvLed.cls == LED_SOLID) {
    setLastKnownLockState(VEH_LOCKED, "SAFE: LEFT LED solid");
    safeUnlockObservedSinceMs = 0;
    safeClearPendingIfConfirmed();
    return;
  }

  if (drvLed.cls == LED_OFF && ctmOfficialAwake && ctmRawAwake &&
      (int32_t)(now - ledBlackoutUntilMs) >= 0) {
    if (safeUnlockObservedSinceMs == 0) safeUnlockObservedSinceMs = now;
    if (now - safeUnlockObservedSinceMs >= SAFE_UNLOCK_OBSERVE_MS) {
      setLastKnownLockState(VEH_UNLOCKED, "SAFE: awake LEFT LED off");
      safeClearPendingIfConfirmed();
    }
  } else {
    safeUnlockObservedSinceMs = 0;
  }

  if (safePendingTarget != VEH_UNKNOWN &&
      now - safePendingSinceMs >= SAFE_PENDING_COMMAND_MS) {
    VehicleLockState expiredTarget = safePendingTarget;
    safePendingTarget = VEH_UNKNOWN;
    safePendingSinceMs = 0;
    safeMarkUnknown(expiredTarget == VEH_LOCKED ?
                    "LOCK command was never physically confirmed" :
                    "UNLOCK command was never physically confirmed");
    if (expiredTarget == VEH_UNLOCKED) proximityUnlocked = false;
  }
}

uint8_t ensureDesiredStateSafe(bool wantLocked, bool manualRequest) {
  commandCountTotal++;
  lastCommand = wantLocked ? "LOCK" : "UNLOCK";

  addLog(String("[SAFE CMD] ") + lastCommand +
         (manualRequest ? " manual" : " auto") +
         " start remembered=" + rememberedLockName() +
         " left=" + ledClassName(drvLed.cls) +
         " ctmOfficial=" + (ctmOfficialAwake ? "AWAKE" : "ASLEEP") +
         " ctmRaw=" + (ctmRawAwake ? "AWAKE" : "ASLEEP"));

  sampleLedNow();

  if (driverObservedLocked()) {
    setLastKnownLockState(VEH_LOCKED, "SAFE command precheck: LEFT solid");
  } else if (driverObservedUnlocked() && ctmOfficialAwake && ctmRawAwake &&
             (int32_t)(millis() - ledBlackoutUntilMs) >= 0) {
    setLastKnownLockState(VEH_UNLOCKED, "SAFE command precheck: awake LEFT off");
  }
  safeClearPendingIfConfirmed();

  VehicleLockState wanted = wantLocked ? VEH_LOCKED : VEH_UNLOCKED;

  if (lastKnownLockState == wanted) {
    lastCommandResult = wantLocked ? "confirmed already locked" : "confirmed already unlocked";
    addLog(String("[SAFE CMD] no pulse - ") + lastCommandResult);
    return SAFE_CMD_CONFIRMED;
  }

  if (safePendingTarget != VEH_UNKNOWN) {
    lastCommandResult = String("blocked; awaiting ") +
                        (safePendingTarget == VEH_LOCKED ? "LOCK" : "UNLOCK") +
                        " confirmation";
    addLog(String("[SAFE CMD] ") + lastCommandResult);
    return SAFE_CMD_BLOCKED;
  }

  if (!manualRequest && wantLocked && blockAutoLockOnBlink && anyBlink()) {
    lastCommandResult = "auto lock blocked by blink";
    addLog("[SAFE CMD] AUTO LOCK blocked by door/blink state");
    return SAFE_CMD_BLOCKED;
  }

  if (lastKnownLockState == VEH_UNKNOWN) {
    lastCommandResult = "blocked; physical lock state unknown";
    addLog("[SAFE CMD] no toggle: state UNKNOWN");
    return SAFE_CMD_BLOCKED;
  }

  uint32_t edgesBefore = safeCtmEdgeSnapshot();
  directPulse();
  waitAndSample(STATE_SETTLE_MS);
  uint32_t commandEdges = safeCtmEdgeSnapshot() - edgesBefore;

  addLog(String("[SAFE CMD] post-pulse LEFT=") + ledClassName(drvLed.cls) +
         " commandWindowEdges=" + commandEdges);

  if (wantLocked && driverObservedLocked()) {
    setLastKnownLockState(VEH_LOCKED, "SAFE post-pulse: LEFT solid");
    lastCommandResult = "LOCK physically confirmed";
    safePendingTarget = VEH_UNKNOWN;
    safePendingSinceMs = 0;
    return SAFE_CMD_CONFIRMED;
  }

  bool commandWindowAwake = commandEdges >= SAFE_COMMAND_AWAKE_EDGES_MIN;
  if (!wantLocked && driverObservedUnlocked() &&
      ((ctmOfficialAwake && ctmRawAwake) || commandWindowAwake)) {
    setLastKnownLockState(VEH_UNLOCKED,
                          commandWindowAwake ? "SAFE post-pulse: awake edge-rate + LEFT off" :
                                               "SAFE post-pulse: awake LEFT off");
    lastCommandResult = "UNLOCK physically confirmed";
    safePendingTarget = VEH_UNKNOWN;
    safePendingSinceMs = 0;
    return SAFE_CMD_CONFIRMED;
  }

  safePendingTarget = wanted;
  safePendingSinceMs = millis();
  lastCommandResult = String("pulse sent; awaiting physical ") +
                      (wantLocked ? "LOCK" : "UNLOCK") + " confirmation";
  addLog(String("[SAFE CMD] ") + lastCommandResult);
  return SAFE_CMD_SENT;
}

void updateDoorTrendSafe() {
  if (!doorTrendActive) return;
  uint32_t now = millis();

  if ((lastTrendSampleMs == 0 || now - lastTrendSampleMs >= TREND_SAMPLE_MS) &&
      phoneSeen && now - lastSeenMs <= 2500 && trendCount < TREND_MAX_SAMPLES) {
    trendRssi[trendCount] = lastRSSI;
    trendTime[trendCount] = now;
    trendCount++;
    lastTrendSampleMs = now;
    trendStatus = String("COUNTING ") + trendCount + " samples; RSSI=" + lastRSSI;
  }

  if (now - doorTrendStartMs < DOOR_TREND_MS) return;

  if (doorTrendIsClearlyWeaker()) {
    trendStatus = "FAST DEPARTURE -> LOCK";
    addLog("[SAFE AUTO] 6s post-door-close RSSI trend confirms departure");
    uint8_t r = ensureDesiredStateSafe(true, false);
    if (r != SAFE_CMD_BLOCKED) {
      proximityUnlocked = false;
      strongCount = 0;
      weakRssiSinceMs = 0;
    }
  } else {
    trendStatus = "NO CLEAR TREND; normal rule";
    addLog("[SAFE AUTO] 6s trend not convincing; normal departure rule continues");
  }
  doorTrendActive = false;
  trendCount = 0;
}

void updateProximityLogicSafe() {
  if (!hasIRK) return;
  uint32_t now = millis();

  if (strongCount >= strongConfirmCount && !proximityUnlocked) {
    addLog(String("[SAFE AUTO] approach confirmed RSSI=") + lastRSSI);
    uint8_t r = ensureDesiredStateSafe(false, false);
    if (r != SAFE_CMD_BLOCKED) proximityUnlocked = true;
    strongCount = 0;
    weakRssiSinceMs = 0;
  }

  updateDoorTrendSafe();

  if (weakRssiSinceMs && now - weakRssiSinceMs >= weakRssiTimeoutMs) {
    addLog(String("[SAFE AUTO] RSSI <= ") + rssiLockThreshold + " dBm for " +
           (now - weakRssiSinceMs) + "ms -> lock");
    uint8_t r = ensureDesiredStateSafe(true, false);
    if (r != SAFE_CMD_BLOCKED) {
      proximityUnlocked = false;
      phoneSeen = false;
      strongCount = 0;
      weakRssiSinceMs = 0;
      resetDoorTrend("IDLE");
    }
    return;
  }

  if (phoneSeen && now - lastSeenMs >= phoneGoneTimeoutMs) {
    addLog(String("[SAFE AUTO] phone unseen age=") + (now - lastSeenMs) + "ms -> lock");
    uint8_t r = ensureDesiredStateSafe(true, false);
    if (r != SAFE_CMD_BLOCKED) {
      proximityUnlocked = false;
      phoneSeen = false;
      strongCount = 0;
      weakRssiSinceMs = 0;
      resetDoorTrend("IDLE");
    }
  }
}

#define ensureDesiredState ensureDesiredStateSafe
#define updateRememberedLockFromLed updateRememberedLockFromLedSafe
#define updateProximityLogic updateProximityLogicSafe
