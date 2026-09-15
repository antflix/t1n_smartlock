// ---------------- BLE ----------------
#define DEVICE_INFO_SERVICE_UUID "180A"
#define BATTERY_SERVICE_UUID     "180F"
#define HEART_RATE_SERVICE_UUID  "180D"
#define HEART_RATE_CHAR_UUID     "2A37"

BLEServer* bleServer = nullptr;
BLEScan* bleScan = nullptr;
uint8_t phoneIRK[16] = {0};
bool hasIRK = false;
bool bleGattConnected = false;
bool bleAuthSuccess = false;
int bleAuthFailReason = 0;
uint32_t bleConnectCount = 0;
uint32_t bleAuthCount = 0;
uint32_t bleScanCycles = 0;
uint32_t bleAdvertCount = 0;
uint32_t bleRpaCount = 0;
uint32_t bleMatchCount = 0;
int lastRSSI = -127;
uint32_t lastSeenMs = 0;
bool phoneSeen = false;
int strongCount = 0;
bool proximityUnlocked = false;
bool approachAttempted = false;  // one unlock attempt per approach event
bool departureAttempted = false; // one lock attempt per departure event
String bleMode = "INIT";
String lastBleAddress = "--";
String lastBleEvent = "boot";

uint32_t weakRssiSinceMs = 0;
static constexpr uint32_t DOOR_TREND_MS = 6000;
static constexpr uint32_t TREND_SAMPLE_MS = 1000;
static constexpr int TREND_MAX_SAMPLES = 8;
static constexpr int TREND_MIN_SAMPLES = 6;
static constexpr int TREND_MIN_TOTAL_DROP_DB = 6;
static constexpr float TREND_MAX_SLOPE_DB_PER_SEC = -0.75f;
int trendRssi[TREND_MAX_SAMPLES] = {0};
uint32_t trendTime[TREND_MAX_SAMPLES] = {0};
int trendCount = 0;
bool doorTrendActive = false;
uint32_t doorTrendStartMs = 0;
uint32_t lastTrendSampleMs = 0;
String trendStatus = "IDLE";

void saveIRK(const uint8_t* irk) { EEPROM.writeUInt(ADDR_MAGIC, EEPROM_MAGIC); EEPROM.writeByte(ADDR_HAS_IRK, 1); for(int i=0;i<16;i++) EEPROM.writeByte(ADDR_IRK+i,irk[i]); EEPROM.commit(); }
void clearIRK(){EEPROM.writeUInt(ADDR_MAGIC,EEPROM_MAGIC);EEPROM.writeByte(ADDR_HAS_IRK,0);for(int i=0;i<16;i++)EEPROM.writeByte(ADDR_IRK+i,0);EEPROM.commit();memset(phoneIRK,0,sizeof(phoneIRK));hasIRK=false;}
bool loadIRK(){if(EEPROM.readUInt(ADDR_MAGIC)!=EEPROM_MAGIC||EEPROM.readByte(ADDR_HAS_IRK)!=1)return false;for(int i=0;i<16;i++)phoneIRK[i]=EEPROM.readByte(ADDR_IRK+i);return true;}
String irkShort(){if(!hasIRK)return "--";char b[20];snprintf(b,sizeof(b),"%02X%02X%02X%02X...",phoneIRK[0],phoneIRK[1],phoneIRK[2],phoneIRK[3]);return String(b);}
void aes128(const uint8_t key[16],const uint8_t in[16],uint8_t out[16]){mbedtls_aes_context ctx;mbedtls_aes_init(&ctx);mbedtls_aes_setkey_enc(&ctx,key,128);mbedtls_aes_crypt_ecb(&ctx,MBEDTLS_AES_ENCRYPT,in,out);mbedtls_aes_free(&ctx);}
bool rpaMatchesPhone(const uint8_t* rpa){if(!hasIRK||(rpa[0]&0xC0)!=0x40)return false;uint8_t input[16]={0};input[13]=rpa[0];input[14]=rpa[1];input[15]=rpa[2];uint8_t enc[16],rev[16];aes128(phoneIRK,input,enc);for(int i=0;i<16;i++)rev[i]=enc[15-i];return rev[0]==rpa[5]&&rev[1]==rpa[4]&&rev[2]==rpa[3];}
String formatAddr(const uint8_t* a){char b[20];snprintf(b,sizeof(b),"%02X:%02X:%02X:%02X:%02X:%02X",a[0],a[1],a[2],a[3],a[4],a[5]);return String(b);}
bool saveIRKFromBond(const esp_ble_bond_dev_t& bond){for(int k=0;k<16;k++)phoneIRK[k]=bond.bond_key.pid_key.irk[15-k];saveIRK(phoneIRK);hasIRK=true;addLog(String("[BLE] IRK saved from bond ")+formatAddr(bond.bd_addr)+" fingerprint="+irkShort());return true;}
bool recoverIRKFromFirstBond(){int n=esp_ble_get_bond_device_num();addLog(String("[BLE] recover IRK requested; bond count=")+n);if(n<=0)return false;esp_ble_bond_dev_t* list=(esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t)*n);if(!list)return false;esp_ble_get_bond_device_list(&n,list);bool ok=saveIRKFromBond(list[0]);free(list);return ok;}
void removeAllBonds(){int n=esp_ble_get_bond_device_num();if(n<=0){addLog("[BLE] no bonds to remove");return;}esp_ble_bond_dev_t* list=(esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t)*n);if(!list)return;esp_ble_get_bond_device_list(&n,list);for(int i=0;i<n;i++)esp_ble_remove_bond_device(list[i].bd_addr);free(list);addLog(String("[BLE] removed ")+n+" bond(s)");}

class ServerCallbacks:public BLEServerCallbacks{void onConnect(BLEServer*)override{bleGattConnected=true;bleConnectCount++;lastBleEvent="GATT connected";addLog("[BLE] iPhone/GATT connected");}void onDisconnect(BLEServer*)override{bleGattConnected=false;lastBleEvent="GATT disconnected";addLog("[BLE] GATT disconnected");if(bleMode=="PAIRING"){delay(200);BLEDevice::startAdvertising();addLog("[BLE] advertising restarted");}}};
class SecurityCallbacks:public BLESecurityCallbacks{
 uint32_t onPassKeyRequest()override{addLog("[BLE] passkey requested -> 123456");return 123456;}void onPassKeyNotify(uint32_t p)override{addLog(String("[BLE] passkey notify ")+p);}bool onConfirmPIN(uint32_t p)override{addLog(String("[BLE] confirm PIN ")+p);return true;}bool onSecurityRequest()override{return true;}
 void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl)override{bleAuthCount++;bleAuthSuccess=cmpl.success;bleAuthFailReason=cmpl.success?0:cmpl.fail_reason;if(!cmpl.success){lastBleEvent=String("auth failed ")+cmpl.fail_reason;addLog(String("[BLE] AUTH FAILED reason=")+cmpl.fail_reason);return;}lastBleEvent="authentication complete";addLog(String("[BLE] AUTH SUCCESS addr=")+formatAddr(cmpl.bd_addr));int n=esp_ble_get_bond_device_num();if(n<=0)return;esp_ble_bond_dev_t* list=(esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t)*n);if(!list)return;esp_ble_get_bond_device_list(&n,list);bool saved=false;for(int i=0;i<n;i++)if(memcmp(list[i].bd_addr,cmpl.bd_addr,6)==0){saveIRKFromBond(list[i]);saved=true;break;}free(list);if(saved){delay(2000);ESP.restart();}}
};
class ScanCallbacks:public BLEAdvertisedDeviceCallbacks{void onResult(BLEAdvertisedDevice d)override{bleAdvertCount++;const uint8_t* native=d.getAddress().getNative();uint8_t a[6],rev[6];memcpy(a,native,6);for(int i=0;i<6;i++)rev[i]=native[5-i];if(((a[0]&0xC0)==0x40)||((rev[0]&0xC0)==0x40))bleRpaCount++;if(!(rpaMatchesPhone(a)||rpaMatchesPhone(rev)))return;bleMatchCount++;lastRSSI=d.getRSSI();lastSeenMs=millis();phoneSeen=true;lastBleAddress=formatAddr(a);lastBleEvent="phone RPA matched";
 if(lastRSSI>=rssiUnlockThreshold)strongCount++;else strongCount=0;
 if(lastRSSI<=rssiLockThreshold){if(weakRssiSinceMs==0)weakRssiSinceMs=lastSeenMs;}else weakRssiSinceMs=0;
}};

void startPairingMode(){bleMode="PAIRING";lastBleEvent="pairing mode started";BLEDevice::init("T1N-Keyless");BLEDevice::setSecurityCallbacks(new SecurityCallbacks());bleServer=BLEDevice::createServer();bleServer->setCallbacks(new ServerCallbacks());BLEService* info=bleServer->createService(DEVICE_INFO_SERVICE_UUID);BLECharacteristic* manufacturer=info->createCharacteristic("2A29",BLECharacteristic::PROPERTY_READ);manufacturer->setValue("T1N Smart Lock");info->start();BLEService* battery=bleServer->createService(BATTERY_SERVICE_UUID);BLECharacteristic* batt=battery->createCharacteristic("2A19",BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY);batt->addDescriptor(new BLE2902());batt->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);uint8_t level=85;batt->setValue(&level,1);battery->start();BLEService* heart=bleServer->createService(HEART_RATE_SERVICE_UUID);BLECharacteristic* hr=heart->createCharacteristic(HEART_RATE_CHAR_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY);hr->addDescriptor(new BLE2902());hr->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);uint8_t fakeHr[2]={0,75};hr->setValue(fakeHr,2);heart->start();esp_ble_auth_req_t auth_req=ESP_LE_AUTH_REQ_SC_BOND;esp_ble_io_cap_t iocap=ESP_IO_CAP_KBDISP;uint8_t key_size=16,init_key=ESP_BLE_ENC_KEY_MASK|ESP_BLE_ID_KEY_MASK,rsp_key=init_key;esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,&auth_req,sizeof(uint8_t));esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,&iocap,sizeof(uint8_t));esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,&key_size,sizeof(uint8_t));esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,&init_key,sizeof(uint8_t));esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,&rsp_key,sizeof(uint8_t));BLEAdvertising* adv=BLEDevice::getAdvertising();adv->addServiceUUID(HEART_RATE_SERVICE_UUID);adv->addServiceUUID(BATTERY_SERVICE_UUID);adv->setScanResponse(true);adv->setMinPreferred(0x20);adv->setMaxPreferred(0x40);BLEDevice::startAdvertising();}
void startScanMode(){bleMode="SCANNING";lastBleEvent="scan mode started";BLEDevice::init("");bleScan=BLEDevice::getScan();bleScan->setAdvertisedDeviceCallbacks(new ScanCallbacks(),true);bleScan->setActiveScan(false);bleScan->setInterval(1600);bleScan->setWindow(800);}
uint32_t lastScanStart=0;void updateBleScanning(){if(!hasIRK||bleMode!="SCANNING"||!bleScan)return;if(millis()-lastScanStart<1400)return;lastScanStart=millis();bleScanCycles++;bleScan->start(1,false);bleScan->clearResults();}
void resetDoorTrend(const char* why){doorTrendActive=false;trendCount=0;trendStatus=why;}
void startDoorTrend(const char* whichLed){if(fullyLocked())return;doorTrendActive=true;doorTrendStartMs=millis();lastTrendSampleMs=0;trendCount=0;trendStatus=String("COUNTING 6s after ")+whichLed+" door-close";addLog(String("[DOOR] ")+whichLed+" LED stopped flashing -> start 6s RSSI trend");}
void detectDoorCloseTransitions(){LedClass d=drvLed.cls,p=paxLed.cls;if(prevDrvLedClass==LED_BLINK&&(d==LED_OFF||d==LED_SOLID))startDoorTrend("LEFT");if(prevPaxLedClass==LED_BLINK&&(p==LED_OFF||p==LED_SOLID))startDoorTrend("RIGHT");prevDrvLedClass=d;prevPaxLedClass=p;}
bool doorTrendIsClearlyWeaker(){if(trendCount<TREND_MIN_SAMPLES)return false;float sumT=0,sumR=0,sumTT=0,sumTR=0;int weaker=0,stronger=0;for(int i=0;i<trendCount;i++){float t=(trendTime[i]-trendTime[0])/1000.0f,r=(float)trendRssi[i];sumT+=t;sumR+=r;sumTT+=t*t;sumTR+=t*r;if(i>0){int delta=trendRssi[i]-trendRssi[i-1];if(delta<=-1)weaker++;else if(delta>=2)stronger++;}}float n=(float)trendCount,denom=n*sumTT-sumT*sumT,slope=denom!=0?(n*sumTR-sumT*sumR)/denom:0;int drop=trendRssi[0]-trendRssi[trendCount-1];bool majority=weaker>=max(3,(trendCount-1)/2)&&weaker>stronger;addLog(String("[TREND] samples=")+trendCount+" drop="+drop+" slope="+String(slope,2));return drop>=TREND_MIN_TOTAL_DROP_DB&&slope<=TREND_MAX_SLOPE_DB_PER_SEC&&majority;}

bool attemptDepartureLock(const char* reason){
 if(departureAttempted){return false;}
 departureAttempted=true;
 addLog(String("[AUTO] departure lock attempt reason=")+reason);
 bool ok=ensureDesiredState(true,false);
 approachAttempted=false;
 proximityUnlocked=false;phoneSeen=false;strongCount=0;weakRssiSinceMs=0;resetDoorTrend(ok?"LOCKED / WAITING FOR RETURN":"LOCK ATTEMPT COMPLETE / WAITING FOR RETURN");
 return ok;
}

void updateDoorTrend(){if(!doorTrendActive)return;uint32_t now=millis();if((lastTrendSampleMs==0||now-lastTrendSampleMs>=TREND_SAMPLE_MS)&&phoneSeen&&now-lastSeenMs<=2500&&trendCount<TREND_MAX_SAMPLES){trendRssi[trendCount]=lastRSSI;trendTime[trendCount]=now;trendCount++;lastTrendSampleMs=now;trendStatus=String("COUNTING ")+trendCount+" samples; RSSI="+lastRSSI;}if(now-doorTrendStartMs<DOOR_TREND_MS)return;if(doorTrendIsClearlyWeaker()){trendStatus="FAST DEPARTURE -> LOCK";attemptDepartureLock("door RSSI trend");}else{trendStatus="NO CLEAR TREND; normal rule";}doorTrendActive=false;trendCount=0;}

void updateProximityLogic(){
 if(!hasIRK)return;uint32_t now=millis();
 if(strongCount>=strongConfirmCount){
  if(!approachAttempted){
   approachAttempted=true;
   departureAttempted=false;
   addLog(String("[AUTO] approach confirmed RSSI=")+lastRSSI);
   bool ok=ensureDesiredState(false,false);
   proximityUnlocked=ok;
   if(ok)weakRssiSinceMs=0;
  }
  strongCount=0;
 }
 updateDoorTrend();
 if(departureAttempted)return;
 if(weakRssiSinceMs&&now-weakRssiSinceMs>=weakRssiTimeoutMs){addLog(String("[AUTO] RSSI <= ")+rssiLockThreshold+" dBm for "+(now-weakRssiSinceMs)+"ms -> lock");attemptDepartureLock("weak RSSI timeout");return;}
 if(phoneSeen&&now-lastSeenMs>=phoneGoneTimeoutMs){addLog(String("[AUTO] phone unseen age=")+(now-lastSeenMs)+"ms -> lock");attemptDepartureLock("phone unseen timeout");}
}
