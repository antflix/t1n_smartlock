/*
  T1N Sprinter Smart Lock - simplified van-specific lock logic
  Target: classic ESP32 / ESP-WROOM-32 (Arduino ESP32 core)
  Arduino IDE: Partition Scheme = Minimal SPIFFS (Large APPS with OTA)

  Proven behavior on this van:
    - GPIO23 pulls factory WT/YL command line to ground.
    - WT/YL is a TOGGLE. Desired LOCK/UNLOCK uses exactly ONE pulse.
    - GPIO21 watches the LEFT/driver lock LED.
    - LEFT SOLID = LOCKED.
    - LEFT OFF means UNLOCKED only while the CTM is awake.
    - When CTM sleeps the LEDs go dark, so we preserve the last confirmed state.
    - CTM awake/asleep comes from GPIO19 WT/RD edge rate:
        >= 350 edges / 5 seconds = AWAKE
        <  350 edges / 5 seconds = ASLEEP
    - GPIO18/right LED is used only for blink/door detection.
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
static constexpr uint32_t BETWEEN_PULSES_MS = 500; // RAW diagnostic only
static constexpr uint32_t STATE_SETTLE_MS = 1800;
static constexpr uint32_t LED_SAMPLE_MS = 50;
static constexpr uint32_t LED_WINDOW_MS = 1500;
static constexpr int LED_SAMPLES = LED_WINDOW_MS / LED_SAMPLE_MS;
static constexpr int SOLID_CONFIRM_SAMPLES = 10;
static constexpr int OFF_CONFIRM_SAMPLES = 4;
static constexpr uint32_t CTM_REPORT_WINDOW_MS = 5000;
static constexpr int CTM_AWAKE_THRESHOLD_5S = 350;

int rssiUnlockThreshold = -80;
int rssiLockThreshold = -95;
uint32_t phoneGoneTimeoutMs = 10000;
uint32_t weakRssiTimeoutMs = 10000;
int strongConfirmCount = 3;
bool blockAutoLockOnBlink = true;

// ---------------- EEPROM ----------------
static constexpr uint32_t EEPROM_MAGIC = 0x54414E34;
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

// ---------------- Types ----------------
enum LedClass : uint8_t { LED_UNKNOWN, LED_OFF, LED_SOLID, LED_BLINK };
struct LedHistory {
  uint8_t samples[LED_SAMPLES] = {0};
  int head = 0;
  int count = 0;
  LedClass cls = LED_UNKNOWN;
};

enum VehicleLockState : int8_t { VEH_UNKNOWN=-1, VEH_UNLOCKED=0, VEH_LOCKED=1 };

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

// ---------------- Vehicle state ----------------
VehicleLockState lastKnownLockState = VEH_UNKNOWN;

void saveLastKnownLockState() {
  EEPROM.writeChar(ADDR_LAST_LOCK, (int8_t)lastKnownLockState);
  EEPROM.commit();
}

void setLastKnownLockState(VehicleLockState st, const char* why, bool persist=true) {
  if (st == VEH_UNKNOWN) return;
  if (lastKnownLockState != st) {
    lastKnownLockState = st;
    addLog(String("[STATE] ") + (st == VEH_LOCKED ? "LOCKED" : "UNLOCKED") +
           " reason=" + why);
    if (persist) saveLastKnownLockState();
  }
}

void setLockStateUnknown(const char* why) {
  if (lastKnownLockState != VEH_UNKNOWN) {
    lastKnownLockState = VEH_UNKNOWN;
    EEPROM.writeChar(ADDR_LAST_LOCK, (int8_t)VEH_UNKNOWN);
    EEPROM.commit();
    addLog(String("[STATE] UNKNOWN reason=") + why);
  }
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

// ---------------- CTM awake/sleep ----------------
volatile uint32_t ctmEdgeTotal = 0;
volatile uint32_t ctmEdgeWindow = 0;
portMUX_TYPE ctmMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t ctmLast5sEdges = 0;
uint32_t ctmWindowStart = 0;
uint32_t ctmLastEdgeMs = 0;
bool ctmOfficialAwake = false;
bool ctmRawAwake = false; // kept for WebUI compatibility; always equals official state

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

  bool awake = ctmLast5sEdges >= CTM_AWAKE_THRESHOLD_5S;
  if (awake != ctmOfficialAwake) {
    ctmOfficialAwake = awake;
    ctmRawAwake = awake;
    addLog(String("[CTM] ") + (awake ? "AWAKE" : "ASLEEP") +
           " edges5s=" + ctmLast5sEdges);
  } else {
    ctmRawAwake = awake;
  }
}

String ctmDiagnosticName() {
  return String(ctmOfficialAwake ? "AWAKE" : "ASLEEP") +
         " (" + String(ctmLast5sEdges) + " edges/5s)";
}

// LEFT LED is authoritative only while CTM is awake.
void updateRememberedLockFromLed() {
  if (!ctmOfficialAwake) return;
  if (drvLed.cls == LED_SOLID) {
    setLastKnownLockState(VEH_LOCKED, "awake LEFT LED solid");
  } else if (drvLed.cls == LED_OFF) {
    setLastKnownLockState(VEH_UNLOCKED, "awake LEFT LED off");
  }
}

bool fullyLocked() {
  if (ctmOfficialAwake) return driverObservedLocked();
  return lastKnownLockState == VEH_LOCKED;
}

bool bothUnlocked() {
  if (ctmOfficialAwake) return driverObservedUnlocked();
  return lastKnownLockState == VEH_UNLOCKED;
}

String lockStateName() {
  String name = rememberedLockName();
  if (!ctmOfficialAwake && lastKnownLockState != VEH_UNKNOWN) name += " / REMEMBERED";
  if (anyBlink()) name += " / DOOR-OPEN";
  return name;
}

// ---------------- Output / lock commands ----------------
bool gpio23Forced = false;
uint32_t gpio23ForceUntil = 0;
uint32_t pulseCountTotal = 0;
uint32_t commandCountTotal = 0;
String lastCommand = "none";
String lastCommandResult = "none";
VehicleLockState desiredLockState = VEH_UNKNOWN;
VehicleLockState lastAutoDesired = VEH_UNKNOWN;
uint32_t lastAutoAttemptMs = 0;
static constexpr uint32_t AUTO_RETRY_COOLDOWN_MS = 10000;

void setLockOutput(bool high, const char* reason) {
  digitalWrite(PIN_LOCK_PULSE, high ? HIGH : LOW);
  digitalWrite(PIN_STATUS_LED, high ? HIGH : LOW);
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

VehicleLockState stateForCommandDecision() {
  if (driverObservedLocked()) return VEH_LOCKED;
  if (driverObservedUnlocked()) return VEH_UNLOCKED;

  // Fresh physical evidence wins whenever CTM is awake.
  if (ctmOfficialAwake) {
    return VEH_UNKNOWN;
  }

  // While asleep the LEDs are dark, so only use the last confirmed state.
  return lastKnownLockState;
}

VehicleLockState observedLockStateFromLights() {
  if (driverObservedLocked()) return VEH_LOCKED;
  if (driverObservedUnlocked()) return VEH_UNLOCKED;
  return VEH_UNKNOWN;
}

bool ensureDesiredState(bool wantLocked, bool manualRequest) {
  commandCountTotal++;
  lastCommand = wantLocked ? "LOCK" : "UNLOCK";
  VehicleLockState wanted = wantLocked ? VEH_LOCKED : VEH_UNLOCKED;
  desiredLockState = wanted;

  // Pull in fresh LED samples before making a toggle decision.
  waitAndSample(300);

  addLog(String("[COMMAND] ") + lastCommand + (manualRequest ? " manual" : " auto") +
         " remembered=" + rememberedLockName() +
         " left=" + ledClassName(drvLed.cls) +
         " ctm=" + (ctmOfficialAwake ? "AWAKE" : "ASLEEP"));

  if (!manualRequest && lastAutoDesired == wanted &&
      millis() - lastAutoAttemptMs < AUTO_RETRY_COOLDOWN_MS) {
    lastCommandResult = "auto retry cooldown";
    addLog("[COMMAND] auto retry cooldown");
    return false;
  }
  if (!manualRequest) {
    lastAutoDesired = wanted;
    lastAutoAttemptMs = millis();
  }

  VehicleLockState current = stateForCommandDecision();

  // If CTM is awake, immediately synchronize memory to the actual LEFT LED.
  if (ctmOfficialAwake && current != VEH_UNKNOWN) {
    setLastKnownLockState(current, "command precheck LEFT LED");
  }

  if (current == wanted) {
    lastCommandResult = wantLocked ? "already locked" : "already unlocked";
    addLog(String("[COMMAND] no pulse - ") + lastCommandResult);
    return true;
  }

  for (int attempt = 1; attempt <= 2; attempt++) {
    addLog(String("[COMMAND] pulse attempt ") + attempt + " toward " + lastCommand);
    directPulse();
    waitAndSample(STATE_SETTLE_MS);

    VehicleLockState after = observedLockStateFromLights();
    if (after != VEH_UNKNOWN) {
      setLastKnownLockState(after, "post-pulse LEFT LED");
      if (after == wanted) {
        lastCommandResult = wantLocked ? "LOCK confirmed" : "UNLOCK confirmed";
        addLog(String("[COMMAND] ") + lastCommandResult);
        return true;
      }
      addLog(String("[COMMAND] LEFT shows ") +
             (after == VEH_LOCKED ? "LOCKED" : "UNLOCKED") +
             "; corrective pulse needed");
      continue;
    }

    setLockStateUnknown("post-pulse LEFT LED unreadable");
    lastCommandResult = "pulse sent; result not confirmed";
    addLog(String("[COMMAND] ") + lastCommandResult);
    return false;
  }

  lastCommandResult = "corrective pulse sent; desired state not confirmed";
  addLog(String("[COMMAND] ") + lastCommandResult);
  return false;
}
