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
