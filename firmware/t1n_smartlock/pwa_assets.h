#pragma once

const char MAIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#101010">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="apple-mobile-web-app-title" content="T1N Lock">
<link rel="manifest" href="/manifest.webmanifest">
<link rel="apple-touch-icon" sizes="180x180" href="https://raw.githubusercontent.com/antflix/t1n_smartlock/main/assets/sprinter-smart-lock-180.png">
<title>T1N Lock</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;min-height:100%;background:#101010;color:#fff;font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display",system-ui,sans-serif}
body{min-height:100dvh;padding:calc(env(safe-area-inset-top) + 12px) 14px calc(env(safe-area-inset-bottom) + 14px);display:flex;flex-direction:column;gap:12px}
.status{display:flex;align-items:center;justify-content:center;gap:10px;min-height:54px;font-size:22px;font-weight:800}.dot{width:16px;height:16px;border-radius:50%;background:#777}.dot.locked{background:#45d483}.dot.unlocked{background:#ff6b6b}.dot.door{background:#ffd166}
.controls{flex:1;display:grid;grid-template-rows:1fr 1fr;gap:14px;min-height:0}.big{border:0;border-radius:24px;color:#fff;font-weight:900;font-size:clamp(34px,10vw,58px);box-shadow:inset 0 1px rgba(255,255,255,.15),0 8px 28px rgba(0,0,0,.28)}.big:active{transform:scale(.985)}.lock{background:#b33f3f}.unlock{background:#2f7d49}
.footer{display:flex;align-items:center;justify-content:space-between;gap:8px;min-height:42px;color:#aaa;font-size:13px}.debug{position:fixed;right:10px;bottom:calc(env(safe-area-inset-bottom) + 8px);border:0;border-radius:12px;background:#29292c;color:#aaa;padding:9px 11px;font-size:12px;opacity:.82}.smallstate{padding-left:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:80%}
</style></head><body>
<div class="status"><span id="dot" class="dot"></span><span id="state">CONNECTING…</span></div>
<div class="controls"><button class="big lock" onclick="sendCmd('lock')">LOCK</button><button class="big unlock" onclick="sendCmd('unlock')">UNLOCK</button></div>
<div class="footer"><span class="smallstate" id="detail">T1N Smart Lock</span></div><button class="debug" onclick="location.href='/debug'">DEBUG</button>
<script>
const e=id=>document.getElementById(id);
async function sendCmd(c){try{await fetch('/api/cmd?do='+c,{method:'POST'});}catch(x){}setTimeout(refresh,250)}
function paint(s){const st=(s.lockState||'UNKNOWN').toUpperCase();e('state').textContent=st;e('dot').className='dot';if(st==='LOCKED')e('dot').classList.add('locked');else if(st==='UNLOCKED')e('dot').classList.add('unlocked');else if(st.includes('DOOR')||st.includes('BLINK'))e('dot').classList.add('door');e('detail').textContent='Auto '+s.auto+' • RSSI '+s.rssi}
async function refresh(){try{paint(await(await fetch('/api/status',{cache:'no-store'})).json())}catch(x){e('state').textContent='OFFLINE';e('dot').className='dot'}}
setInterval(refresh,700);refresh();
</script></body></html>
)HTML";

const char MANIFEST_JSON[] PROGMEM = R"JSON({
  "name":"T1N Smart Lock",
  "short_name":"T1N Lock",
  "start_url":"/",
  "scope":"/",
  "display":"standalone",
  "background_color":"#101010",
  "theme_color":"#101010",
  "icons":[{"src":"https://raw.githubusercontent.com/antflix/t1n_smartlock/main/assets/sprinter-smart-lock-180.png","sizes":"180x180","type":"image/png","purpose":"any"}]
})JSON";

const char OTA_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#101010"><title>T1N Firmware Update</title>
<style>
*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,"SF Pro Display",system-ui,sans-serif;background:#101010;color:#fff;margin:0;padding:20px}.wrap{max-width:620px;margin:auto}.card{background:#1c1c1e;border-radius:18px;padding:20px;margin:16px 0}h1{font-size:26px;margin:4px 0 8px}h2{font-size:18px;margin:0 0 10px}.muted{color:#aaa;line-height:1.45}.btn{width:100%;border:0;border-radius:14px;padding:16px;font-size:17px;font-weight:800;background:#356aa0;color:#fff;margin-top:12px}.btn:disabled{opacity:.45}.secondary{background:#555}.status{margin-top:14px;white-space:pre-wrap;word-break:break-word;color:#ddd}.back{color:#8ec5ff;text-decoration:none}
</style></head><body><div class="wrap">
<a class="back" href="/debug">← Diagnostics</a>
<h1>Firmware Update</h1>
<div class="card"><h2>Latest GitHub build</h2>
<div class="muted">Press once to download and install the current <b>latest</b> firmware release from the T1N Smart Lock repository. The ESP32 will reboot automatically after a successful install.</div>
<button id="latest" class="btn" onclick="installLatest()">CHECK FOR UPDATES &amp; INSTALL</button>
<div id="status" class="status"></div></div>
<div class="card"><h2>Manual .bin upload</h2><div class="muted">The original local upload path is still available at <code>/api/update</code> if it is ever needed for recovery.</div></div>
</div><script>
const latestUrl='https://github.com/antflix/t1n_smartlock/releases/download/latest/firmware.bin';
async function installLatest(){
 const b=document.getElementById('latest'),s=document.getElementById('status');
 b.disabled=true;s.textContent='Downloading latest firmware from GitHub…';
 try{
   const r=await fetch('/api/update-url?url='+encodeURIComponent(latestUrl),{method:'POST'});
   const t=await r.text();
   s.textContent=t;
   if(!r.ok)b.disabled=false;
 }catch(e){s.textContent='Connection lost. If the update succeeded, the ESP32 may already be rebooting.';}
}
</script></body></html>
)HTML";
