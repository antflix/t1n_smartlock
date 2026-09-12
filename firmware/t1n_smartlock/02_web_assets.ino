// ---------------- Web UI ----------------
const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#101010">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="T1N Lock">
<link rel="manifest" href="/manifest.webmanifest">
<link rel="apple-touch-icon" href="/icon.svg">
<title>T1N Lock</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;min-height:100%;background:#101010;color:#fff;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display",system-ui,sans-serif}
body{min-height:100dvh;padding:calc(env(safe-area-inset-top) + 12px) 14px calc(env(safe-area-inset-bottom) + 14px);display:flex;flex-direction:column;gap:12px}
.status{display:flex;align-items:center;justify-content:center;gap:10px;min-height:54px;font-size:22px;font-weight:800;letter-spacing:.2px}
.dot{width:16px;height:16px;border-radius:50%;background:#777;box-shadow:0 0 12px rgba(255,255,255,.15)}
.dot.locked{background:#45d483;box-shadow:0 0 18px rgba(69,212,131,.55)}
.dot.unlocked{background:#ff6b6b;box-shadow:0 0 18px rgba(255,107,107,.45)}
.dot.door{background:#ffd166;box-shadow:0 0 18px rgba(255,209,102,.45)}
.controls{flex:1;display:grid;grid-template-rows:1fr 1fr;gap:14px;min-height:0}
.big{border:0;border-radius:24px;color:#fff;font-weight:900;font-size:clamp(34px,10vw,58px);box-shadow:inset 0 1px rgba(255,255,255,.15),0 8px 28px rgba(0,0,0,.28)}
.big:active{transform:scale(.985)}
.lock{background:#b33f3f}.unlock{background:#2f7d49}
.footer{display:flex;align-items:center;justify-content:space-between;gap:8px;min-height:42px;color:#aaa;font-size:13px}
.debug{position:fixed;right:10px;bottom:calc(env(safe-area-inset-bottom) + 8px);border:0;border-radius:12px;background:#29292c;color:#aaa;padding:9px 11px;font-size:12px;opacity:.82}
.smallstate{padding-left:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:80%}
</style></head><body>
<div class="status"><span id="dot" class="dot"></span><span id="state">CONNECTING…</span></div>
<div class="controls">
<button class="big lock" onclick="sendCmd('lock')">LOCK</button>
<button class="big unlock" onclick="sendCmd('unlock')">UNLOCK</button>
</div>
<div class="footer"><span class="smallstate" id="detail">T1N Smart Lock</span></div>
<button class="debug" onclick="location.href='/debug'">DEBUG</button>
<script>
const e=id=>document.getElementById(id);
async function sendCmd(c){
  try{await fetch('/api/cmd?do='+c,{method:'POST'});}catch(x){}
  setTimeout(refresh,250);
}
function paint(s){
  const st=(s.lockState||'UNKNOWN').toUpperCase();
  e('state').textContent=st;
  e('dot').className='dot';
  if(st==='LOCKED') e('dot').classList.add('locked');
  else if(st==='UNLOCKED') e('dot').classList.add('unlocked');
  else if(st.includes('DOOR')||st.includes('BLINK')) e('dot').classList.add('door');
  e('detail').textContent='Auto '+s.auto+' • RSSI '+s.rssi;
}
async function refresh(){try{paint(await (await fetch('/api/status',{cache:'no-store'})).json())}catch(x){e('state').textContent='OFFLINE';e('dot').className='dot';}}
setInterval(refresh,700);refresh();
</script></body></html>
)HTML";

const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101010"><link rel="manifest" href="/manifest.webmanifest"><link rel="apple-touch-icon" href="/icon.svg">
<title>T1N Smart Lock Diagnostics</title>
<style>
*{box-sizing:border-box}body{font-family:-apple-system,system-ui,sans-serif;background:#101010;color:#eee;margin:0;padding:14px;max-width:900px;margin:auto}h1{font-size:23px;margin:8px 0 14px}.card{background:#1c1c1e;border-radius:12px;padding:13px;margin:10px 0}.grid{display:grid;grid-template-columns:1fr 1fr;gap:7px 14px}.k{color:#aaa}.v{font-weight:700;word-break:break-word}.good{color:#68d391}.bad{color:#ff7b72}.warn{color:#ffd166}button{font-size:15px;padding:12px 8px;border:0;border-radius:9px;margin:4px;background:#555;color:white;min-height:44px}.red{background:#b94a48}.green{background:#438a49}.blue{background:#356aa0}.orange{background:#a66a2b}.row{display:grid;grid-template-columns:1fr 1fr;gap:5px}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:5px}pre{white-space:pre-wrap;font-size:11px;max-height:390px;overflow:auto;background:#070707;padding:9px;border-radius:8px}input{width:90px;background:#2c2c2e;color:white;border:1px solid #555;padding:7px;border-radius:6px}.tiny{font-size:12px;color:#aaa}.section{font-weight:700;margin-bottom:8px;color:#8ec5ff}.home{position:fixed;right:10px;top:10px;text-decoration:none;color:#aaa;background:#29292c;padding:8px 10px;border-radius:10px;font-size:12px}@media(max-width:600px){.grid{grid-template-columns:1fr 1fr}.row3{grid-template-columns:1fr}.row{grid-template-columns:1fr 1fr}}
</style></head><body>
<a class=home href="/">MAIN</a>
<h1>T1N Smart Lock — Full Diagnostics</h1>

<div class=card><div class=section>Vehicle / GPIO</div><div class=grid>
<div><span class=k>Lock state </span><span class=v id=lockState>--</span></div><div><span class=k>Last command </span><span class=v id=cmd>--</span></div>
<div><span class=k>Driver LED class </span><span class=v id=drvClass>--</span></div><div><span class=k>PAX LED class </span><span class=v id=paxClass>--</span></div>
<div><span class=k>GPIO21 raw </span><span class=v id=g21>--</span></div><div><span class=k>GPIO18 raw </span><span class=v id=g18>--</span></div>
<div><span class=k>GPIO23 output/readback </span><span class=v id=g23>--</span></div><div><span class=k>Pulses sent </span><span class=v id=pulses>--</span></div>
<div><span class=k>GPIO19 raw </span><span class=v id=g19>--</span></div><div><span class=k>CTM edges last 5s </span><span class=v id=ctmEdges>--</span></div>
<div><span class=k>CTM edge total </span><span class=v id=ctmTotal>--</span></div><div><span class=k>CTM diagnostic </span><span class=v id=ctmDiag>--</span></div>
</div></div>

<div class=card><div class=section>Manual lock controls — no CTM/cooldown blockers</div>
<div class=row><button class=red onclick="cmd('lock')">ENSURE LOCK</button><button class=green onclick="cmd('unlock')">ENSURE UNLOCK</button></div>
<div class=row><button onclick="cmd('pulse1')">RAW 1× 500ms PULSE</button><button onclick="cmd('pulse2')">RAW 2× PULSES</button></div>
<div class=row><button class=orange onclick="cmd('gpioOn')">GPIO23 HIGH — HOLD 8s</button><button onclick="cmd('gpioOff')">GPIO23 LOW NOW</button></div>
<div class=tiny>GPIO23 HIGH also lights the ESP32 D2 LED. This lets you verify the ESP32 output without relying on a meter catching a 500ms pulse.</div>
</div>

<div class=card><div class=section>Bluetooth / iPhone</div><div class=grid>
<div><span class=k>BLE mode </span><span class=v id=bleMode>--</span></div><div><span class=k>GATT connected </span><span class=v id=gatt>--</span></div>
<div><span class=k>Authentication </span><span class=v id=auth>--</span></div><div><span class=k>Bond count </span><span class=v id=bonds>--</span></div>
<div><span class=k>IRK stored </span><span class=v id=irk>--</span></div><div><span class=k>IRK fingerprint </span><span class=v id=irkfp>--</span></div>
<div><span class=k>Phone RPA matched </span><span class=v id=phone>--</span></div><div><span class=k>Last RSSI </span><span class=v id=rssi>--</span></div>
<div><span class=k>Last seen age </span><span class=v id=age>--</span></div><div><span class=k>Strong count </span><span class=v id=strong>--</span></div>
<div><span class=k>Scan cycles </span><span class=v id=scans>--</span></div><div><span class=k>Advertisements </span><span class=v id=adverts>--</span></div>
<div><span class=k>RPA candidates </span><span class=v id=rpas>--</span></div><div><span class=k>RPA matches </span><span class=v id=matches>--</span></div>
<div><span class=k>Last BLE event </span><span class=v id=bleEvent>--</span></div><div><span class=k>Last BLE address </span><span class=v id=bleAddr>--</span></div>
</div>
<div class=row3 style="margin-top:10px"><button class=blue onclick="cmd('recoverIrk')">RECOVER IRK FROM BOND</button><button class=orange onclick="cmd('clearBle')">CLEAR BONDS + IRK + REBOOT</button><button onclick="cmd('reboot')">REBOOT ESP32</button></div>
<div class=tiny>If Bond count is greater than 0 but IRK stored says NO, use Recover IRK. If Bond count is 0 after iPhone connects, use Clear Bonds + IRK, forget T1N-Keyless on iPhone, then pair again.</div>
</div>

<div class=card><div class=section>Automatic proximity</div>
<div class=grid><div><span class=k>Auto proximity </span><span class=v id=auto>--</span></div><div><span class=k>Proximity state </span><span class=v id=prox>--</span></div><div><span class=k>Door trend </span><span class=v id=trend>--</span></div><div><span class=k>Weak-RSSI timer </span><span class=v id=weakAge>--</span></div></div>
<div class=row></div>
<div class=row><button onclick="cmd('simApproach')">SIMULATE APPROACH</button><button onclick="cmd('simDepart')">SIMULATE DEPARTURE</button></div>
<div style="margin-top:8px" class=tiny>Unlock RSSI <input id=thr type=number value=-80> dBm &nbsp; Lock RSSI <input id=lockthr type=number value=-95> dBm &nbsp; Timeout <input id=to type=number value=10> sec &nbsp; <button onclick="saveSettings()">APPLY</button></div>
</div>

<div class=card><div class=section>System</div><div class=grid>
<div><span class=k>WiFi </span><span class=v id=wifi>--</span></div><div><span class=k>Uptime </span><span class=v id=uptime>--</span></div>
<div><span class=k>Free heap </span><span class=v id=heap>--</span></div><div><span class=k>Command result </span><span class=v id=result>--</span></div>
</div></div>

<div class=card><div class=section>Live event log</div><pre id=log>Loading...</pre><button onclick="cmd('clearLog')">CLEAR LOG</button></div>
<script>
function e(x){return document.getElementById(x)}
async function cmd(c){try{await fetch('/api/cmd?do='+c,{method:'POST'});}catch(x){} setTimeout(refresh,200)}
async function saveSettings(){let u='/api/settings?unlock='+encodeURIComponent(e('thr').value)+'&lock='+encodeURIComponent(e('lockthr').value)+'&timeout='+encodeURIComponent(e('to').value);await fetch(u,{method:'POST'});refresh()}
async function refresh(){try{let s=await (await fetch('/api/status')).json();for(let k in s){if(e(k))e(k).textContent=s[k]}if(document.activeElement!==e('thr'))e('thr').value=s.unlockThreshold;if(document.activeElement!==e('lockthr'))e('lockthr').value=s.lockThreshold;if(document.activeElement!==e('to'))e('to').value=s.goneTimeout; e('log').textContent=await (await fetch('/api/log')).text();}catch(x){}}
setInterval(refresh,1000);refresh();
</script><button onclick="location.href='/update'">FIRMWARE UPDATE</button></body></html>
)HTML";

const char MANIFEST_JSON[] PROGMEM = R"JSON({
  "name":"T1N Smart Lock",
  "short_name":"T1N Lock",
  "start_url":"/",
  "scope":"/",
  "display":"standalone",
  "background_color":"#101010",
  "theme_color":"#101010",
  "icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any maskable"}]
})JSON";

const char ICON_SVG[] PROGMEM = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"><rect width="512" height="512" rx="110" fill="#101010"/><path d="M154 228v-57c0-58 45-104 102-104s102 46 102 104v57h32c18 0 32 14 32 32v154c0 18-14 32-32 32H122c-18 0-32-14-32-32V260c0-18 14-32 32-32h32zm54 0h96v-57c0-29-21-50-48-50s-48 21-48 50v57zm48 61a39 39 0 0 0-19 73v35h38v-35a39 39 0 0 0-19-73z" fill="#fff"/></svg>)SVG";


const char OTA_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#101010">
<title>T1N Firmware Update</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;background:#101010;color:#fff;margin:0;padding:24px}
.wrap{max-width:620px;margin:auto}.card{background:#1c1c1e;border-radius:18px;padding:20px;margin-top:18px}
h1{font-size:25px;margin:0 0 8px}h2{font-size:19px;margin:0 0 10px}.muted{color:#aaa;line-height:1.45}
input{width:100%;box-sizing:border-box;padding:14px;background:#2c2c2e;border-radius:12px;color:#fff;border:1px solid #444;margin-top:10px}
button,.back{display:block;width:100%;box-sizing:border-box;margin-top:14px;padding:15px;border:0;border-radius:12px;font-size:17px;font-weight:700;text-align:center;text-decoration:none}
button{background:#0a84ff;color:#fff}.back{background:#2c2c2e;color:#fff}#status,#urlStatus{margin-top:14px;white-space:pre-wrap}.warn{color:#ffd60a}
progress{width:100%;height:18px;margin-top:14px}.tiny{font-size:12px;color:#888;line-height:1.4}
</style></head><body><div class="wrap">
<h1>Firmware Update</h1>
<div class="card">
<h2>Upload from this device</h2>
<div class="muted">Choose a compiled ESP32 <b>.bin</b>. An Arduino <b>.ino</b> cannot be compiled by the ESP32.</div>
<p class="warn"><b>Keep power connected until the ESP32 reboots.</b></p>
<input id="fw" type="file" accept=".bin,application/octet-stream">
<button onclick="uploadFw()">UPLOAD & INSTALL</button>
<progress id="bar" value="0" max="100" style="display:none"></progress>
<div id="status"></div>
</div>
<div class="card">
<h2>Install from URL</h2>
<div class="muted">The ESP32 can download a compiled firmware image itself. This is useful when the .bin is hosted on your server.</div>
<input id="fwurl" type="url" placeholder="https://example.com/t1n/latest.bin">
<input id="sha" type="text" autocapitalize="none" autocomplete="off" placeholder="Optional SHA-256 (64 hex characters)">
<button onclick="pullFw()">DOWNLOAD & INSTALL</button>
<div id="urlStatus"></div>
<div class="tiny">HTTPS is supported. Current firmware uses TLS without CA verification for URL OTA; supplying the expected SHA-256 verifies the downloaded image before it is activated. Do not expose this WebUI directly to the public internet.</div>
</div>
<a class="back" href="/debug">BACK TO DEBUG</a>
</div>
<script>
function uploadFw(){
 const f=document.getElementById('fw').files[0], st=document.getElementById('status'), bar=document.getElementById('bar');
 if(!f){st.textContent='Choose a .bin file first.';return;}
 if(!f.name.toLowerCase().endsWith('.bin')){st.textContent='Firmware must be a compiled .bin file.';return;}
 const fd=new FormData(); fd.append('firmware',f);
 const x=new XMLHttpRequest(); x.open('POST','/api/update',true);
 bar.style.display='block'; bar.value=0; st.textContent='Uploading...';
 x.upload.onprogress=e=>{if(e.lengthComputable)bar.value=Math.round(e.loaded*100/e.total);}
 x.onload=()=>{st.textContent=x.status==200?x.responseText:'Update failed: '+x.responseText;};
 x.onerror=()=>{st.textContent='Connection lost. If upload reached 100%, wait for reboot and reconnect.';};
 x.send(fd);
}
async function pullFw(){
 const u=document.getElementById('fwurl').value.trim(), h=document.getElementById('sha').value.trim(), st=document.getElementById('urlStatus');
 if(!/^https?:\/\//i.test(u)){st.textContent='Enter a full http:// or https:// URL.';return;}
 if(h && !/^[0-9a-fA-F]{64}$/.test(h)){st.textContent='SHA-256 must be exactly 64 hexadecimal characters.';return;}
 st.textContent='ESP32 is downloading and flashing firmware. Do not remove power...';
 try{
   const body=new URLSearchParams({url:u,sha256:h});
   const r=await fetch('/api/update-url',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
   st.textContent=await r.text();
 }catch(e){st.textContent='Connection lost. If the update completed, wait for reboot and reconnect.';}
}
</script></body></html>
)HTML";

String jsonQuote(const String& s) {
  String o="\"";
  for (size_t i=0;i<s.length();i++) {
    char c=s[i];
    if (c=='\"' || c=='\\') {o+='\\';o+=c;}
    else if (c=='\n') o+="\\n";
    else o+=c;
  }
  o+='\"';
  return o;
}

String boolText(bool v) { return v ? "YES" : "NO"; }

void clearEventLog() {
  eventLogHead=0; eventLogCount=0;
  for(int i=0;i<LOG_LINES;i++) eventLog[i]="";
}


String bytesToHex(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

bool validSha256(String s) {
  s.trim();
  if (s.length() == 0) return true;
  if (s.length() != 64) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

