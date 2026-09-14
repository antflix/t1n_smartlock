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
bool ctmRawAwake = false; // WebUI compatibility; always equals official state

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
    addLog(String("[CTM] ") + (awake ? "AWAKE" : "ASLEEP") +
           " edges5s=" + ctmLast5sEdges);
  }
  ctmRawAwake = ctmOfficialAwake;
}

String ctmDiagnosticName() {
  return String(ctmOfficialAwake ? "AWAKE" : "ASLEEP") +
         " (" + String(ctmLast5sEdges) + " edges/5s)";
}

// The entire lock-state model is intentionally only these two rules:
//   CTM AWAKE  -> LEFT LED is truth: SOLID=LOCKED, OFF=UNLOCKED.
//   CTM ASLEEP -> LEDs mean nothing; remembered state is truth.
VehicleLockState currentLockState() {
  if (!ctmOfficialAwake) return lastKnownLockState;
  if (drvLed.cls == LED_SOLID) return VEH_LOCKED;
  if (drvLed.cls == LED_OFF) return VEH_UNLOCKED;
  return VEH_UNKNOWN;
}

void updateRememberedLockFromLed() {
  if (!ctmOfficialAwake) return;
  if (drvLed.cls == LED_SOLID) setLastKnownLockState(VEH_LOCKED, "awake LEFT solid");
  else if (drvLed.cls == LED_OFF) setLastKnownLockState(VEH_UNLOCKED, "awake LEFT off");
}

bool fullyLocked() {
  return currentLockState() == VEH_LOCKED;
}

bool bothUnlocked() {
  return currentLockState() == VEH_UNLOCKED;
}

String lockStateName() {
  VehicleLockState st = currentLockState();
  String name = st == VEH_LOCKED ? "LOCKED" : st == VEH_UNLOCKED ? "UNLOCKED" : "UNKNOWN";
  if (!ctmOfficialAwake && st != VEH_UNKNOWN) name += " / REMEMBERED";
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

bool ensureDesiredState(bool wantLocked, bool manualRequest) {
  commandCountTotal++;
  lastCommand = wantLocked ? "LOCK" : "UNLOCK";
  VehicleLockState wanted = wantLocked ? VEH_LOCKED : VEH_UNLOCKED;
  desiredLockState = wanted;

  // Door/blink protection applies only to automatic locking.
  if (!manualRequest && wantLocked && blockAutoLockOnBlink && anyBlink()) {
    lastCommandResult = "auto lock blocked by door/blink";
    addLog("[COMMAND] no pulse - auto lock blocked by door/blink");
    return false;
  }

  // Refresh only the LED classifier. CTM awake/asleep remains the single gate
  // that decides whether LED data is allowed to mean anything.
  waitAndSample(300);
  bool wasAwake = ctmOfficialAwake;
  VehicleLockState current = currentLockState();

  addLog(String("[COMMAND] ") + lastCommand + (manualRequest ? " manual" : " auto") +
         " current=" + (current == VEH_LOCKED ? "LOCKED" : current == VEH_UNLOCKED ? "UNLOCKED" : "UNKNOWN") +
         " source=" + (wasAwake ? "LEFT_LED" : "MEMORY") +
         " remembered=" + rememberedLockName() +
         " left=" + ledClassName(drvLed.cls) +
         " ctm=" + (wasAwake ? "AWAKE" : "ASLEEP"));

  // While awake, copy the authoritative LED state into memory.
  if (wasAwake && current != VEH_UNKNOWN) {
    setLastKnownLockState(current, "awake command precheck");
  }

  if (current == VEH_UNKNOWN) {
    lastCommandResult = "blocked - state unknown";
    addLog("[COMMAND] no pulse - state unknown");
    return false;
  }

  if (current == wanted) {
    lastCommandResult = wantLocked ? "already locked" : "already unlocked";
    addLog(String("[COMMAND] no pulse - ") + lastCommandResult);
    return true;
  }

  // Known opposite state. WT/YL is a toggle, so exactly ONE pulse is required.
  addLog(String("[COMMAND] one pulse toward ") + lastCommand);
  directPulse();
  waitAndSample(STATE_SETTLE_MS);

  // If this command started while CTM was asleep, LED OFF is meaningless.
  // Do not inspect it at all. A single toggle from a known opposite state means
  // the remembered state becomes the requested state.
  if (!wasAwake) {
    setLastKnownLockState(wanted, "single toggle from remembered state");
    lastCommandResult = wantLocked ? "LOCK pulse sent" : "UNLOCK pulse sent";
    addLog(String("[COMMAND] ") + lastCommandResult);
    return true;
  }

  // If the command started awake, LEFT LED is authoritative after the pulse.
  VehicleLockState after = VEH_UNKNOWN;
  if (drvLed.cls == LED_SOLID) after = VEH_LOCKED;
  else if (drvLed.cls == LED_OFF) after = VEH_UNLOCKED;

  if (after != VEH_UNKNOWN) {
    setLastKnownLockState(after, "post-pulse awake LEFT LED");
    bool ok = after == wanted;
    lastCommandResult = ok ? (wantLocked ? "LOCK confirmed" : "UNLOCK confirmed")
                           : "pulse sent; LEFT LED disagrees";
    addLog(String("[COMMAND] ") + lastCommandResult);
    return ok;
  }

  // We knew the starting state and sent one toggle. If the LED is temporarily
  // unreadable, remember the requested state. Never send a second pulse.
  setLastKnownLockState(wanted, "single toggle; LEFT unreadable");
  lastCommandResult = wantLocked ? "LOCK pulse sent" : "UNLOCK pulse sent";
  addLog(String("[COMMAND] ") + lastCommandResult);
  return true;
}
