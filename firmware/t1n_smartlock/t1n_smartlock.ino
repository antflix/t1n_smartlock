/*
  T1N Sprinter Smart Lock - van-specific lock logic
  Target: classic ESP32 / ESP-WROOM-32 (Arduino ESP32 core)
  Arduino IDE: Partition Scheme = Minimal SPIFFS (Large APPS with OTA)

  Proven behavior on this van:
    - GPIO23 pulls factory WT/YL command line to ground.
    - If CTM is awake, one pulse requests a lock-state change.
    - If CTM is asleep, first pulse wakes it; only after wake is positively detected may LEFT LED be trusted.
    - Once CTM is awake, a command sends at most ONE lock-state pulse, then stops.
    - GPIO21 watches LEFT/driver lock LED: SOLID=LOCKED, OFF=UNLOCKED while CTM is awake.
    - Sleeping LEFT OFF is never interpreted as UNLOCKED.
    - Remembered state is an immediate estimate only. Real awake LEFT LED always reconciles it.
    - CTM GPIO19 rates observed: awake ~448 edges/5s; asleep ~238-266 edges/5s.
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

static constexpr int PIN_LOCK_PULSE = 23;
static constexpr int PIN_CTM_SENSE  = 19;
static constexpr int PIN_PAX_LED    = 18;
static constexpr int PIN_DRV_LED    = 21;
static constexpr int PIN_STATUS_LED = 2;

#include "secrets.h"
static const char* WIFI_SSID = T1N_WIFI_SSID;
static const char* WIFI_PASS = T1N_WIFI_PASS;
static const char* MDNS_NAME = "t1n-lock";
WebServer webServer(80);

static constexpr uint32_t PULSE_MS = 500;
static constexpr uint32_t BETWEEN_PULSES_MS = 500;
static constexpr uint32_t STATE_SETTLE_MS = 1800;
static constexpr uint32_t LED_SAMPLE_MS = 50;
static constexpr uint32_t LED_WINDOW_MS = 1500;
static constexpr int LED_SAMPLES = LED_WINDOW_MS / LED_SAMPLE_MS;
static constexpr int SOLID_CONFIRM_SAMPLES = 10;
static constexpr int OFF_CONFIRM_SAMPLES = 4;
static constexpr uint32_t CTM_REPORT_WINDOW_MS = 5000;
static constexpr int CTM_AWAKE_THRESHOLD_5S = 350;
static constexpr uint32_t CTM_WAKE_SAMPLE_MS = 1200;
static constexpr uint32_t CTM_WAKE_TIMEOUT_MS = 3000;
static constexpr int CTM_WAKE_MIN_EDGES = 80;

int rssiUnlockThreshold = -80;
int rssiLockThreshold = -95;
uint32_t phoneGoneTimeoutMs = 10000;
uint32_t weakRssiTimeoutMs = 10000;
int strongConfirmCount = 3;
bool blockAutoLockOnBlink = true;

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

enum LedClass : uint8_t { LED_UNKNOWN, LED_OFF, LED_SOLID, LED_BLINK };
struct LedHistory {
  uint8_t samples[LED_SAMPLES] = {0};
  int head = 0;
  int count = 0;
  LedClass cls = LED_UNKNOWN;
};
enum VehicleLockState : int8_t { VEH_UNKNOWN=-1, VEH_UNLOCKED=0, VEH_LOCKED=1 };

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

LedHistory drvLed;
LedHistory paxLed;
uint32_t lastLedSampleMs = 0;
LedClass prevDrvLedClass = LED_UNKNOWN;
LedClass prevPaxLedClass = LED_UNKNOWN;
uint8_t logicalLedOn(int pin) { return digitalRead(pin) == LOW ? 1 : 0; }
void appendLedSample(LedHistory& h, uint8_t v) {
  h.samples[h.head] = v;
  h.head = (h.head + 1) % LED_SAMPLES;
  if (h.count < LED_SAMPLES) h.count++;
}
LedClass classifyLed(const LedHistory& h) {
  if (h.count == 0) return LED_UNKNOWN;
  int idx = (h.head - 1 + LED_SAMPLES) % LED_SAMPLES;
  int newest = h.samples[idx], newestRun = 0, transitions = 0, prev = newest;
  bool runBroken = false;
  for (int i=0;i<h.count;i++) {
    int s=h.samples[idx];
    if (!runBroken) { if (s==newest) newestRun++; else runBroken=true; }
    if (i>0 && s != prev) transitions++;
    prev = s; idx=(idx-1+LED_SAMPLES)%LED_SAMPLES;
  }
  if (transitions >= 2) return LED_BLINK;
  if (newest == 1 && newestRun >= SOLID_CONFIRM_SAMPLES) return LED_SOLID;
  if (newest == 0 && newestRun >= OFF_CONFIRM_SAMPLES) return LED_OFF;
  return h.cls;
}
void sampleLedNow() {
  appendLedSample(drvLed, logicalLedOn(PIN_DRV_LED));
  appendLedSample(paxLed, logicalLedOn(PIN_PAX_LED));
  drvLed.cls=classifyLed(drvLed); paxLed.cls=classifyLed(paxLed);
}
void updateLedClassifiers() {
  uint32_t now=millis(); if(now-lastLedSampleMs<LED_SAMPLE_MS)return;
  lastLedSampleMs=now; sampleLedNow();
}
void waitAndSample(uint32_t ms) {
  uint32_t start=millis(); while(millis()-start<ms){sampleLedNow();delay(LED_SAMPLE_MS);}
}
bool anyBlink(){return drvLed.cls==LED_BLINK||paxLed.cls==LED_BLINK;}

VehicleLockState lastKnownLockState=VEH_UNKNOWN;
void saveLastKnownLockState(){EEPROM.writeChar(ADDR_LAST_LOCK,(int8_t)lastKnownLockState);EEPROM.commit();}
void setLastKnownLockState(VehicleLockState st,const char* why,bool persist=true){
  if(st==VEH_UNKNOWN)return;
  if(lastKnownLockState!=st){lastKnownLockState=st;addLog(String("[STATE] ")+(st==VEH_LOCKED?"LOCKED":"UNLOCKED")+" reason="+why);if(persist)saveLastKnownLockState();}
}
const char* rememberedLockName(){return lastKnownLockState==VEH_LOCKED?"LOCKED":lastKnownLockState==VEH_UNLOCKED?"UNLOCKED":"UNKNOWN";}
const char* ledClassName(LedClass c){return c==LED_OFF?"OFF":c==LED_SOLID?"SOLID":c==LED_BLINK?"BLINK":"UNKNOWN";}
const char* stateName(VehicleLockState s){return s==VEH_LOCKED?"LOCKED":s==VEH_UNLOCKED?"UNLOCKED":"UNKNOWN";}

volatile uint32_t ctmEdgeTotal=0;
volatile uint32_t ctmEdgeWindow=0;
portMUX_TYPE ctmMux=portMUX_INITIALIZER_UNLOCKED;
uint32_t ctmLast5sEdges=0,ctmWindowStart=0,ctmLastEdgeMs=0;
bool ctmOfficialAwake=false,ctmRawAwake=false;
void IRAM_ATTR onCtmEdge(){portENTER_CRITICAL_ISR(&ctmMux);ctmEdgeTotal++;ctmEdgeWindow++;ctmLastEdgeMs=millis();portEXIT_CRITICAL_ISR(&ctmMux);}
void updateCtmDiagnostics(){
  uint32_t now=millis(); if(now-ctmWindowStart<CTM_REPORT_WINDOW_MS)return;
  portENTER_CRITICAL(&ctmMux);ctmLast5sEdges=ctmEdgeWindow;ctmEdgeWindow=0;portEXIT_CRITICAL(&ctmMux);ctmWindowStart=now;
  bool awake=ctmLast5sEdges>=CTM_AWAKE_THRESHOLD_5S;
  if(awake!=ctmOfficialAwake){ctmOfficialAwake=awake;addLog(String("[CTM] ")+(awake?"AWAKE":"ASLEEP")+" edges5s="+ctmLast5sEdges);}
  ctmRawAwake=ctmOfficialAwake;
}
String ctmDiagnosticName(){return String(ctmOfficialAwake?"AWAKE":"ASLEEP")+" ("+String(ctmLast5sEdges)+" edges/5s)";}

bool measureCtmAwake(uint32_t sampleMs=CTM_WAKE_SAMPLE_MS){
  uint32_t startEdges;
  portENTER_CRITICAL(&ctmMux);startEdges=ctmEdgeTotal;portEXIT_CRITICAL(&ctmMux);
  uint32_t start=millis();
  while(millis()-start<sampleMs){sampleLedNow();delay(LED_SAMPLE_MS);}
  uint32_t endEdges;
  portENTER_CRITICAL(&ctmMux);endEdges=ctmEdgeTotal;portEXIT_CRITICAL(&ctmMux);
  uint32_t edges=endEdges-startEdges;
  bool awake=edges>=CTM_WAKE_MIN_EDGES;
  addLog(String("[CTM] short-check edges=")+edges+"/"+sampleMs+"ms -> "+(awake?"AWAKE":"ASLEEP"));
  return awake;
}

VehicleLockState leftLedState(){if(drvLed.cls==LED_SOLID)return VEH_LOCKED;if(drvLed.cls==LED_OFF)return VEH_UNLOCKED;return VEH_UNKNOWN;}
VehicleLockState currentLockState(){if(!ctmOfficialAwake)return lastKnownLockState;return leftLedState();}
void updateRememberedLockFromLed(){if(!ctmOfficialAwake)return;VehicleLockState a=leftLedState();if(a!=VEH_UNKNOWN)setLastKnownLockState(a,"awake LEFT LED");}
bool fullyLocked(){return currentLockState()==VEH_LOCKED;}
bool bothUnlocked(){return currentLockState()==VEH_UNLOCKED;}
String lockStateName(){VehicleLockState s=currentLockState();String n=stateName(s);if(!ctmOfficialAwake&&s!=VEH_UNKNOWN)n+=" / REMEMBERED";if(anyBlink())n+=" / DOOR-OPEN";return n;}

bool gpio23Forced=false;uint32_t gpio23ForceUntil=0,pulseCountTotal=0,commandCountTotal=0;
String lastCommand="none",lastCommandResult="none";VehicleLockState desiredLockState=VEH_UNKNOWN;
void setLockOutput(bool high,const char* reason){digitalWrite(PIN_LOCK_PULSE,high?HIGH:LOW);digitalWrite(PIN_STATUS_LED,high?HIGH:LOW);delay(2);int rb=digitalRead(PIN_LOCK_PULSE);addLog(String("[GPIO23] ")+(high?"HIGH":"LOW")+" reason="+reason+" readback="+(rb?"HIGH":"LOW"));}
void directPulse(uint32_t widthMs=PULSE_MS){pulseCountTotal++;addLog(String("[PULSE] #")+pulseCountTotal+" width="+widthMs+"ms");setLockOutput(true,"pulse-start");delay(widthMs);setLockOutput(false,"pulse-end");}
void directTwoPulses(){directPulse();delay(BETWEEN_PULSES_MS);directPulse();}
VehicleLockState verifiedAwakeLedState(){waitAndSample(300);return leftLedState();}

bool ensureDesiredState(bool wantLocked,bool manualRequest){
  commandCountTotal++;lastCommand=wantLocked?"LOCK":"UNLOCK";
  VehicleLockState wanted=wantLocked?VEH_LOCKED:VEH_UNLOCKED;desiredLockState=wanted;
  if(!manualRequest&&wantLocked&&blockAutoLockOnBlink&&anyBlink()){lastCommandResult="auto lock blocked by door/blink";addLog("[COMMAND] no pulse - auto lock blocked by door/blink");return false;}
  waitAndSample(300);
  bool awake=ctmOfficialAwake;
  VehicleLockState actual=awake?leftLedState():VEH_UNKNOWN;
  addLog(String("[COMMAND] ")+lastCommand+(manualRequest?" manual":" auto")+" start="+(awake?"AWAKE":"ASLEEP")+" remembered="+rememberedLockName()+" left="+ledClassName(drvLed.cls));
  if(awake&&actual!=VEH_UNKNOWN)setLastKnownLockState(actual,"command precheck LEFT LED");
  if(!awake){
    addLog(String("[COMMAND] wake pulse before ")+lastCommand);
    directPulse();
    uint32_t wakeStart=millis();
    do {awake=measureCtmAwake();if(awake)break;} while(millis()-wakeStart<CTM_WAKE_TIMEOUT_MS);
    if(!awake){lastCommandResult="failed - CTM did not wake; attempt complete";addLog(String("[COMMAND] ")+lastCommandResult);return false;}
    actual=verifiedAwakeLedState();
    addLog(String("[VERIFY] awake LEFT=")+ledClassName(drvLed.cls)+" actual="+stateName(actual));
    if(actual!=VEH_UNKNOWN)setLastKnownLockState(actual,"verified LEFT after wake");
  }
  if(actual==wanted){lastCommandResult=wantLocked?"already locked - LED verified":"already unlocked - LED verified";addLog(String("[COMMAND] no state pulse - ")+lastCommandResult);return true;}
  if(actual==VEH_UNKNOWN){lastCommandResult="failed - awake LEFT unreadable; attempt complete";addLog(String("[COMMAND] ")+lastCommandResult);return false;}
  addLog(String("[COMMAND] one state pulse toward ")+lastCommand);
  directPulse();waitAndSample(STATE_SETTLE_MS);
  VehicleLockState after=leftLedState();
  addLog(String("[VERIFY] after state pulse LEFT=")+ledClassName(drvLed.cls)+" actual="+stateName(after)+" wanted="+stateName(wanted));
  if(after!=VEH_UNKNOWN)setLastKnownLockState(after,"LEFT after state pulse");
  if(after==wanted){lastCommandResult=wantLocked?"LOCK confirmed":"UNLOCK confirmed";addLog(String("[COMMAND] ")+lastCommandResult);return true;}
  lastCommandResult=(after==VEH_UNKNOWN)?"state pulse sent; result unverified; no retry":"state pulse sent; LEFT disagrees; no retry";
  addLog(String("[COMMAND] ")+lastCommandResult);return false;
}
