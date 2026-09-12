/*
  T1N Sprinter Smart Lock - v9 Remote OTA
  Target: classic ESP32 / ESP-WROOM-32 (Arduino ESP32 core)
  Arduino IDE: Partition Scheme = Minimal SPIFFS (Large APPS with OTA)

  Hardware:
    GPIO23 -> 2.2k -> 2N2222 base -> collector pulls WT/YL to GND
    GPIO19 <- LM393 CTM WT/RD comparator output (diagnostic only)
    GPIO21 <- PC817 driver lock LED
    GPIO18 <- PC817 passenger/cargo lock LED

  Design rule in this version:
    - CTM edge-rate sleep detection preserves last-known lock state and gates LED state corrections.
    - Manual diagnostic controls NEVER use cooldowns or state blockers.
    - Desired LOCK/UNLOCK uses ONE toggle pulse only; never blindly sends a second pulse.
    - LOCK state uses ONLY driver/left LED: SOLID = locked; sleeping OFF does not erase remembered state.
    - Either LED transitioning BLINK -> not BLINK starts a 6-second RSSI trend check.
    - Normal departure lock: RSSI <= -95 dBm for 10 seconds, or no phone match for 10 seconds.
    - Web UI supports local .bin OTA and pull-from-URL OTA.
    - Web UI exposes raw pins, BLE pairing/bond/IRK state, scan counters,
      output readback, logs, direct GPIO controls, and simulated proximity actions.
*/

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"
#include <ESPmDNS.h>
#include "mbedtls/aes.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"

// ---------------- Pins ----------------
static constexpr int PIN_LOCK_PULSE = 23;
static constexpr int PIN_CTM_SENSE  = 19;
static constexpr int PIN_PAX_LED    = 18;
static constexpr int PIN_DRV_LED    = 21;
static constexpr int PIN_STATUS_LED = 2;

// ---------------- WiFi ----------------
#include "secrets.h"
static const char* WIFI_SSID = T1N_WIFI_SSID;
static const char* WIFI_PASS = T1N_WIFI_PASS;
static const char* MDNS_NAME = "t1n-lock";
WebServer webServer(80);

// ---------------- Timing / behavior ----------------
static constexpr uint32_t PULSE_MS = 500;
static constexpr uint32_t BETWEEN_PULSES_MS = 500; // RAW diagnostic button only
static constexpr uint32_t STATE_SETTLE_MS = 1800;
static constexpr uint32_t LED_SAMPLE_MS = 50;
static constexpr uint32_t LED_WINDOW_MS = 1500;
static constexpr int LED_SAMPLES = LED_WINDOW_MS / LED_SAMPLE_MS;
static constexpr int SOLID_CONFIRM_SAMPLES = 10;
static constexpr int OFF_CONFIRM_SAMPLES = 4;
static constexpr uint32_t CTM_REPORT_WINDOW_MS = 5000;
static constexpr int CTM_REFERENCE_AWAKE_EDGES = 350; // display only

int rssiUnlockThreshold = -80;
int rssiLockThreshold = -95;
uint32_t phoneGoneTimeoutMs = 10000;
uint32_t weakRssiTimeoutMs = 10000;
int strongConfirmCount = 3;
bool blockAutoLockOnBlink = true;

// ---------------- EEPROM ----------------
static constexpr uint32_t EEPROM_MAGIC = 0x54414E34; // TAN4
static constexpr int EEPROM_SIZE = 128;
static constexpr int ADDR_MAGIC = 0;
static constexpr int ADDR_HAS_IRK = 4;
static constexpr int ADDR_IRK = 8;
static constexpr int ADDR_LAST_LOCK = 24;
static constexpr int ADDR_UNLOCK_RSSI = 28;
static constexpr int ADDR_LOCK_RSSI = 32;
static constexpr int ADDR_TIMEOUT_SEC = 36;
static constexpr int ADDR_SETTINGS_VERSION = 40;
static constexpr uint8_t SETTINGS_VERSION = 7;

// ---------------- Types declared before first function ----------------
// Keep these above Arduino's auto-generated function-prototype insertion point.
enum LedClass : uint8_t { LED_UNKNOWN, LED_OFF, LED_SOLID, LED_BLINK };
struct LedHistory {
  uint8_t samples[LED_SAMPLES] = {0};
  int head = 0;
  int count = 0;
  LedClass cls = LED_UNKNOWN;
};

// ---------------- Logs ----------------
static constexpr int LOG_LINES = 120;
String eventLog[LOG_LINES];
int eventLogHead = 0;
int eventLogCount = 0;

void addLog(const String& s) {
  String line = String(millis() / 1000) + "s  " + s;
  eventLog[eventLogHead] = line;
  eventLogHead = (eventLogHead + 1) % LOG_LINES;
  if (eventLogCount < LOG_LINES) eventLogCount++;
  Serial.println(line);
}

// ---------------- LED sense ----------------
LedHistory drvLed;
LedHistory paxLed;
uint32_t lastLedSampleMs = 0;
LedClass prevDrvLedClass = LED_UNKNOWN;
LedClass prevPaxLedClass = LED_UNKNOWN;

// ---------------- Stateful vehicle state ----------------
// Ported from the original t1n-smart-lock architecture:
// - CTM state is inferred from edge RATE, not instantaneous GPIO level.
// - raw CTM state must remain stable before official awake/asleep changes.
// - LED observations are only allowed to change remembered lock state while
//   the CTM is awake.
// - a candidate LED lock state must remain stable for 10 seconds before it
//   corrects the remembered state, filtering the pre-sleep LED fade.
// This van differs from the reference repo: ONLY the LEFT/driver LED is used
// for lock state. LEFT SOLID = locked. LEFT OFF while CTM awake = unlocked.
// The right LED is retained only for door/blink detection.
enum VehicleLockState : int8_t { VEH_UNKNOWN=-1, VEH_UNLOCKED=0, VEH_LOCKED=1 };
VehicleLockState lastKnownLockState = VEH_UNKNOWN;

static constexpr uint32_t CTM_WINDOW_MS = 5000;
static constexpr int CTM_AWAKE_THRESHOLD_5S = 350;
static constexpr uint32_t CTM_STICKY_MS = 5000;
static constexpr uint32_t LED_BLACKOUT_MS = 1500;
static constexpr uint32_t LOCK_STATE_STABILITY_MS = 10000;

bool ctmOfficialAwake = false;
bool ctmRawAwake = false;
uint32_t ctmRawStableSinceMs = 0;
uint32_t ledBlackoutUntilMs = 0;

int8_t pendingLockState = -1;
uint32_t pendingLockSinceMs = 0;

void saveLastKnownLockState() {
  EEPROM.writeChar(ADDR_LAST_LOCK, (int8_t)lastKnownLockState);
  EEPROM.commit();
}

void setLastKnownLockState(VehicleLockState st, const char* why, bool persist=true) {
  if (st == VEH_UNKNOWN) return;
  if (lastKnownLockState != st) {
    lastKnownLockState = st;
    addLog(String("[STATE] remembered ") + (st==VEH_LOCKED?"LOCKED":"UNLOCKED") + " reason=" + why);
    if (persist) saveLastKnownLockState();
  }
  pendingLockState = -1;
  pendingLockSinceMs = 0;
}

const char* rememberedLockName() {
  if (lastKnownLockState == VEH_LOCKED) return "LOCKED";
  if (lastKnownLockState == VEH_UNLOCKED) return "UNLOCKED";
  return "UNKNOWN";
}

const char* ledClassName(LedClass c) {
  switch (c) {
    case LED_OFF: return "OFF";
    case LED_SOLID: return "SOLID";
    case LED_BLINK: return "BLINK";
    default: return "UNKNOWN";
  }
}

uint8_t logicalLedOn(int pin) {
  return digitalRead(pin) == LOW ? 1 : 0; // PC817 collector LOW = vehicle LED on
}

void appendLedSample(LedHistory& h, uint8_t v) {
  h.samples[h.head] = v;
  h.head = (h.head + 1) % LED_SAMPLES;
  if (h.count < LED_SAMPLES) h.count++;
}

LedClass classifyLed(const LedHistory& h) {
  if (h.count == 0) return LED_UNKNOWN;

  int idx = (h.head - 1 + LED_SAMPLES) % LED_SAMPLES;
  int newest = h.samples[idx];
  int newestRun = 0;
  int transitions = 0;
  int prev = newest;
  bool runBroken = false;

  for (int i = 0; i < h.count; i++) {
    int s = h.samples[idx];
    if (!runBroken) {
      if (s == newest) newestRun++;
      else runBroken = true;
    }
    if (i > 0 && s != prev) transitions++;
    prev = s;
    idx = (idx - 1 + LED_SAMPLES) % LED_SAMPLES;
  }

  if (transitions >= 2) return LED_BLINK;
  if (newest == 1 && newestRun >= SOLID_CONFIRM_SAMPLES) return LED_SOLID;
  if (newest == 0 && newestRun >= OFF_CONFIRM_SAMPLES) return LED_OFF;
  return h.cls;
}

void sampleLedNow() {
  appendLedSample(drvLed, logicalLedOn(PIN_DRV_LED));
  appendLedSample(paxLed, logicalLedOn(PIN_PAX_LED));
  drvLed.cls = classifyLed(drvLed);
  paxLed.cls = classifyLed(paxLed);
}

void updateLedClassifiers() {
  uint32_t now = millis();
  if (now - lastLedSampleMs < LED_SAMPLE_MS) return;
  lastLedSampleMs = now;
  sampleLedNow();
}

void waitAndSample(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    sampleLedNow();
    delay(LED_SAMPLE_MS);
  }
}

bool anyBlink() { return drvLed.cls == LED_BLINK || paxLed.cls == LED_BLINK; }
bool driverObservedLocked() { return drvLed.cls == LED_SOLID; }
bool driverObservedUnlocked() { return drvLed.cls == LED_OFF; }
bool fullyLocked() { return lastKnownLockState == VEH_LOCKED; }
bool bothUnlocked() { return lastKnownLockState == VEH_UNLOCKED; }

String lockStateName() {
  if (anyBlink()) return String(rememberedLockName()) + " / DOOR-OPEN";
  return String(rememberedLockName());
}

// ---------------- CTM raw diagnostics ----------------
volatile uint32_t ctmEdgeTotal = 0;
volatile uint32_t ctmEdgeWindow = 0;
portMUX_TYPE ctmMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t ctmLast5sEdges = 0;
uint32_t ctmWindowStart = 0;
uint32_t ctmLastEdgeMs = 0;

void IRAM_ATTR onCtmEdge() {
  portENTER_CRITICAL_ISR(&ctmMux);
  ctmEdgeTotal++;
  ctmEdgeWindow++;
  ctmLastEdgeMs = millis();
  portEXIT_CRITICAL_ISR(&ctmMux);
}

void updateCtmDiagnostics() {
  uint32_t now = millis();
  if (now - ctmWindowStart < CTM_REPORT_WINDOW_MS) return;

  portENTER_CRITICAL(&ctmMux);
  ctmLast5sEdges = ctmEdgeWindow;
  ctmEdgeWindow = 0;
  portEXIT_CRITICAL(&ctmMux);
  ctmWindowStart = now;

  bool raw = (ctmLast5sEdges >= CTM_AWAKE_THRESHOLD_5S);
  if (raw != ctmRawAwake) {
    ctmRawAwake = raw;
    ctmRawStableSinceMs = now;
  } else if (now - ctmRawStableSinceMs >= CTM_STICKY_MS && ctmOfficialAwake != raw) {
    ctmOfficialAwake = raw;
    ledBlackoutUntilMs = now + LED_BLACKOUT_MS;
    pendingLockState = -1;
    pendingLockSinceMs = 0;
    addLog(String("[CTM] official state -> ") + (ctmOfficialAwake ? "AWAKE" : "ASLEEP") +
           " edges5s=" + ctmLast5sEdges);
  }
}

String ctmDiagnosticName() {
  return String(ctmOfficialAwake ? "AWAKE" : "ASLEEP") +
         " (raw " + (ctmRawAwake ? "AWAKE" : "ASLEEP") +
         ", " + String(ctmLast5sEdges) + " edges/5s)";
}

void updateRememberedLockFromLed() {
  uint32_t now = millis();
  if (!ctmOfficialAwake) return;              // HOLD last-known state through CTM sleep
  if ((int32_t)(now - ledBlackoutUntilMs) < 0) return;
  if (drvLed.cls != LED_SOLID && drvLed.cls != LED_OFF) {
    pendingLockState = -1;
    pendingLockSinceMs = 0;
    return;
  }

  int8_t observed = (drvLed.cls == LED_SOLID) ? VEH_LOCKED : VEH_UNLOCKED;
  if (observed == (int8_t)lastKnownLockState) {
    pendingLockState = -1;
    pendingLockSinceMs = 0;
    return;
  }

  if (pendingLockState != observed) {
    pendingLockState = observed;
    pendingLockSinceMs = now;
    addLog(String("[STATE] candidate ") + (observed==VEH_LOCKED?"LOCKED":"UNLOCKED") +
           " from LEFT LED; starting 10s stability gate");
    return;
  }

  if (now - pendingLockSinceMs >= LOCK_STATE_STABILITY_MS) {
    setLastKnownLockState((VehicleLockState)observed, "stable LEFT LED observation");
  }
}

// ---------------- Output / lock commands ----------------
bool gpio23Forced = false;
uint32_t gpio23ForceUntil = 0;
uint32_t pulseCountTotal = 0;
uint32_t commandCountTotal = 0;
String lastCommand = "none";
String lastCommandResult = "none";

void setLockOutput(bool high, const char* reason) {
  digitalWrite(PIN_LOCK_PULSE, high ? HIGH : LOW);
  digitalWrite(PIN_STATUS_LED, high ? HIGH : LOW); // visible mirror
  delay(2);
  int rb = digitalRead(PIN_LOCK_PULSE);
  addLog(String("[GPIO23] ") + (high ? "HIGH" : "LOW") +
         " reason=" + reason + " readback=" + (rb ? "HIGH" : "LOW"));
}

void directPulse(uint32_t widthMs = PULSE_MS) {
  pulseCountTotal++;
  addLog(String("[PULSE] #") + pulseCountTotal + " width=" + widthMs + "ms");
  setLockOutput(true, "pulse-start");
  delay(widthMs);
  setLockOutput(false, "pulse-end");
}

void directTwoPulses() {
  directPulse();
  delay(BETWEEN_PULSES_MS);
  directPulse();
}

void ensureDesiredState(bool wantLocked, bool manualRequest) {
  commandCountTotal++;
  lastCommand = wantLocked ? "LOCK" : "UNLOCK";
  addLog(String("[COMMAND] ") + lastCommand + (manualRequest ? " manual" : " auto") +
         " start remembered=" + rememberedLockName() +
         " left=" + ledClassName(drvLed.cls) +
         " ctm=" + (ctmOfficialAwake ? "AWAKE" : "ASLEEP"));

  sampleLedNow();

  // A solid LEFT LED is positive immediate evidence of LOCKED even before
  // the long correction gate. Do not require the right LED to be solid.
  if (driverObservedLocked()) setLastKnownLockState(VEH_LOCKED, "LEFT LED solid", true);

  if (wantLocked && lastKnownLockState == VEH_LOCKED) {
    lastCommandResult = "already locked";
    addLog("[COMMAND] already LOCKED - no pulse needed");
    return;
  }
  if (!wantLocked && lastKnownLockState == VEH_UNLOCKED && ctmOfficialAwake && driverObservedUnlocked()) {
    lastCommandResult = "already unlocked";
    addLog("[COMMAND] awake + LEFT OFF confirms already UNLOCKED");
    return;
  }

  if (!manualRequest && wantLocked && blockAutoLockOnBlink && anyBlink()) {
    lastCommandResult = "auto lock blocked by blink";
    addLog("[COMMAND] AUTO LOCK blocked because LED blink/door state detected");
    return;
  }

  // THIS VAN: WT/YL is a toggle command even when the CTM was asleep.
  // A second automatic pulse can undo the first command (unlock -> relock).
  // Therefore desired LOCK/UNLOCK always sends exactly ONE pulse. CTM state is
  // still used to protect remembered-state/LED interpretation, not pulse count.
  addLog(String("[COMMAND] 1 toggle pulse; CTM=") +
         (ctmOfficialAwake ? "AWAKE" : "ASLEEP"));
  directPulse();
  setLastKnownLockState(wantLocked ? VEH_LOCKED : VEH_UNLOCKED,
                        wantLocked ? "LOCK command accepted" : "UNLOCK command accepted");
  waitAndSample(STATE_SETTLE_MS);

  // Positive LEFT-solid feedback can immediately confirm lock. For unlock,
  // LEFT OFF is accepted immediately only while CTM is officially awake.
  if (wantLocked && driverObservedLocked()) {
    setLastKnownLockState(VEH_LOCKED, "post-pulse LEFT solid");
    lastCommandResult = "success / LEFT solid";
  } else if (!wantLocked && ctmOfficialAwake && driverObservedUnlocked()) {
    setLastKnownLockState(VEH_UNLOCKED, "post-pulse awake LEFT off");
    lastCommandResult = "success / awake LEFT off";
  } else {
    lastCommandResult = String("pulse sent / remembered=") + rememberedLockName();
  }
  addLog(String("[COMMAND] after pulse remembered=") + rememberedLockName() +
         " left=" + ledClassName(drvLed.cls));
}

