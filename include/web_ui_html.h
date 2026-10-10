#pragma once

// Single-page web UI served by web_server.cpp. Self-contained: no external requests, so it also works in
// setup mode where the device has no internet.

static constexpr char kWebUiHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FPL Buddy</title>
<style>
:root{--bg:#0e1116;--card:#171b22;--line:#262c36;--text:#e8ecf1;--muted:#8b95a3;--accent:#00e5a0;--accent-ink:#04221a;--danger:#ff5a5a;--warn:#ffcc66}
@media (prefers-color-scheme:light){:root{--bg:#f3f5f8;--card:#fff;--line:#dde2e8;--text:#141920;--muted:#5d6775;--accent:#00a877;--accent-ink:#fff;--danger:#d93636;--warn:#a86b00}}
*{box-sizing:border-box}
[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:20px 16px 96px}
h1{font-size:22px;margin:0 0 2px}
h2{font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);margin:0 0 12px}
.sub{color:var(--muted);font-size:13px;margin-bottom:18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.banner{border-color:var(--warn)}
label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
input,select{width:100%;padding:10px 12px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--text);font:inherit}
input:focus,select:focus{outline:2px solid var(--accent);outline-offset:-1px}
.row{display:flex;gap:8px;align-items:center}
.row>input,.row>select{flex:1}
button{padding:10px 14px;border-radius:10px;border:1px solid var(--line);background:transparent;color:var(--text);font:inherit;cursor:pointer;white-space:nowrap}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-ink);font-weight:600}
button.danger{color:var(--danger)}
button:disabled{opacity:.5;cursor:default}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin:10px 0}
.chip{display:inline-flex;align-items:center;gap:4px;padding:4px 4px 4px 10px;border-radius:99px;background:var(--bg);border:1px solid var(--line);font-size:14px}
.chip button{border:0;padding:0 6px;color:var(--muted);font-size:16px;line-height:1}
.check{display:flex;align-items:center;gap:10px;padding:8px 0;border-top:1px solid var(--line);color:var(--text);font-size:15px;margin:0}
.check:first-child{border-top:0}
.check input{width:18px;height:18px;accent-color:var(--accent)}
.muted{color:var(--muted);font-size:13px}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}
progress{width:100%;height:8px;margin-top:10px;accent-color:var(--accent)}
.savebar{position:fixed;left:0;right:0;bottom:0;padding:12px 16px;background:var(--card);border-top:1px solid var(--line);display:none}
.savebar.show{display:block}
.savebar div{max-width:560px;margin:0 auto;display:flex;gap:8px;justify-content:space-between;align-items:center}
#toast{position:fixed;left:50%;top:16px;transform:translateX(-50%);background:var(--text);color:var(--bg);padding:8px 14px;border-radius:10px;font-size:14px;opacity:0;transition:opacity .2s;pointer-events:none;max-width:90vw}
#toast.show{opacity:1}
.tabs{display:flex;gap:4px;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:4px;margin-bottom:14px}
.tabs button{flex:1;border:0;padding:8px 10px}
.tabs button.on{background:var(--accent);color:var(--accent-ink);font-weight:600}
.badge{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--warn);margin-left:6px;vertical-align:middle}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(98px,1fr));gap:6px}
.grid button{padding:9px 6px;white-space:normal}
.grid button.cur{border-color:var(--accent);color:var(--accent)}
.grid button.neg{color:var(--danger)}
.fields{display:grid;grid-template-columns:1fr 1fr;gap:0 10px}
.opt{border-top:0;padding:10px 0 0}
.now{font-size:20px;font-weight:600}
.pl{display:flex;justify-content:space-between;gap:8px;padding:8px 10px;border:1px solid var(--line);border-radius:10px;margin:4px 0;cursor:pointer;font-size:14px}
.pl.bench{opacity:.6}
.pl.sel{border-color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
.pl b{font-weight:600}
.scene{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:10px 0;border-top:1px solid var(--line)}
.scene:first-child{border-top:0}
input[type=range]{padding:0;border:0;background:none;accent-color:var(--accent)}
.lbl{display:flex;justify-content:space-between}
</style>
</head>
<body>
<main>
<h1>FPL Buddy</h1>
<div class="sub" id="status">Loading...</div>

<nav class="tabs" id="tabs"><button class="on" data-tab="settings">Settings</button><button data-tab="demo">Demo<span class="badge" id="demoBadge" hidden></span></button></nav>

<div id="tab-settings">

<div class="card banner" id="setupBanner" hidden>
<h2>Setup mode</h2>
<div>Choose your WiFi network and FPL team ID below. The device restarts and joins your network, then this page is at <b>http://fpl-buddy.local</b>.</div>
</div>

<div id="displayCards">
<div class="card">
<h2>Screens</h2>
<div class="muted">Shown between gameweeks. Tap the display to skip ahead.</div>
<label for="interval">Seconds per screen (5-600)</label>
<input id="interval" type="number" min="5" max="600" inputmode="numeric">
<label for="graph">Rank graph</label>
<select id="graph"><option value="3">Last 3 gameweeks</option><option value="5">Last 5 gameweeks</option><option value="10">Last 10 gameweeks</option><option value="0">All gameweeks</option></select>
</div>

<div class="card">
<h2>Price watchlist</h2>
<div class="muted">Players by web name (as on the FPL site) or element ID, up to 8.</div>
<div class="chips" id="watchChips"></div>
<form class="row" id="watchForm"><input id="watchInput" placeholder="e.g. Saka" autocomplete="off" maxlength="23"><button>Add</button></form>
</div>

<div class="card">
<h2>Mini leagues</h2>
<div id="leagues" class="muted">Waiting for the first FPL update.</div>
</div>

<div class="card" id="ledCard">
<h2>LED ring</h2>
<label class="lbl" for="ledRankBright"><span>Rank brightness</span><span id="ledRankBrightVal"></span></label>
<input id="ledRankBright" type="range" min="0" max="100" step="5">
<label class="lbl" for="ledDeadlineBright"><span>Deadline countdown brightness</span><span id="ledDeadlineBrightVal"></span></label>
<input id="ledDeadlineBright" type="range" min="0" max="100" step="5">
<label class="lbl" for="ledNotifyBright"><span>Event flash brightness</span><span id="ledNotifyBrightVal"></span></label>
<input id="ledNotifyBright" type="range" min="0" max="100" step="5">
<label class="check opt"><input type="checkbox" id="ledRankLiveGw"> Rank LEDs only while the gameweek is live</label>
<label class="check opt"><input type="checkbox" id="ledRankMatch"> Rank LEDs only while a match is in play</label>
<div class="muted" style="margin-top:6px">With both ticked, the rank LEDs need both. The tickboxes do not affect the deadline countdown or event flashes.</div>
</div>
</div>

<div class="card">
<h2>Device</h2>
<label for="entryId">FPL team ID</label>
<input id="entryId" type="number" min="1" inputmode="numeric">
<div class="muted">The number in fantasy.premierleague.com/entry/<b>ID</b>/event/...</div>
<label for="ssid">WiFi network</label>
<div class="row"><input id="ssid" list="networks" autocomplete="off" maxlength="32"><button type="button" id="scanBtn">Scan</button></div>
<datalist id="networks"></datalist>
<label for="wifiPassword">WiFi password</label>
<input id="wifiPassword" type="password" placeholder="Unchanged" autocomplete="new-password" maxlength="64">
<div class="actions"><button class="primary" id="deviceSave">Save and restart</button></div>
</div>

<div class="card">
<h2>Web page password</h2>
<div class="muted" id="pwState"></div>
<label for="webPassword">New password (user name: admin)</label>
<div class="row"><input id="webPassword" type="password" autocomplete="new-password" maxlength="32"><button id="pwSave">Set</button></div>
<div class="actions"><button class="danger" id="pwClear">Remove password</button></div>
</div>

<div class="card">
<h2>Firmware</h2>
<div>Installed: <b id="fwVersion">-</b></div>
<label for="fwFile">Firmware file (.pio/build/&lt;env&gt;/firmware.bin)</label>
<input id="fwFile" type="file" accept=".bin">
<progress id="fwProgress" max="100" value="0" hidden></progress>
<div class="actions">
<button class="primary" id="fwUpload">Upload and install</button>
<button id="restartBtn">Restart</button>
<button id="setupBtn">Restart in setup mode</button>
</div>
</div>
</div>

<div id="tab-demo" hidden>
<div class="card">
<h2>Demo mode</h2>
<div id="dmStatus">Loading...</div>
<div class="muted">Shows made-up FPL data so you can film any screen when you want. The device stops polling FPL while the demo runs. Stop the demo (or restart the device) to go back to your live data.</div>
<div class="actions" id="dmStartBtns"><button class="primary" data-start="mine" id="dmStartMine">Start with my team</button><button data-start="sample">Start with sample team</button></div>
<div class="actions" id="dmRunBtns" hidden><button data-act="restart">Start over</button><button class="danger" data-act="stop">Stop demo</button></div>
</div>

<div id="dmControls" hidden>
<div class="card">
<h2>On the display</h2>
<div class="now" id="dmNow"></div>
<div class="muted" id="dmNowSub"></div>
<label>Live gameweek screens</label>
<div class="grid"><button data-screen="live">Live score</button><button data-screen="events">Events list</button><button data-screen="squad">Squad</button></div>
<label>Between-gameweeks screens</label>
<div class="grid"><button data-screen="rank">Rank</button><button data-screen="graph">Rank graph</button><button data-screen="prices">Prices</button><button data-screen="watchlist">Watchlist</button><button data-screen="leagues">Leagues</button><button data-screen="next">Next</button></div>
<label class="check opt"><input type="checkbox" id="dmPause"> Pause the between-gameweeks carousel</label>
</div>

<div class="card">
<h2>Gameweek</h2>
<div class="grid"><button data-phase="idle">Between gameweeks</button><button data-phase="live">Kick off (go live)</button></div>
<label>Deadline in</label>
<div class="grid"><button data-deadline="21540">5h 59m</button><button data-deadline="7200">2h</button><button data-deadline="3590">59m 50s</button><button data-deadline="900">15m</button><button data-deadline="180">3m</button><button data-deadline="30">30s</button><button data-deadline="10">10s</button></div>
<form class="row" id="dmDeadlineForm" style="margin-top:8px"><input id="dmDeadlineMin" type="number" min="0" max="10080" placeholder="minutes" inputmode="numeric"><input id="dmDeadlineSec" type="number" min="0" max="59" placeholder="seconds" inputmode="numeric"><button>Set</button></form>
<div class="muted" style="margin-top:6px">Under 6 hours shows the deadline countdown. Under 1 hour shows the final-hour countdown and the LED ring countdown.</div>
<label class="check opt"><input type="checkbox" id="dmAutoKick"> Go live when the deadline passes</label>
</div>

<div class="card" id="dmScoreCard">
<h2>Score and rank</h2>
<div class="fields">
<label>Gameweek<input id="dmGw" type="number" min="1" max="38" inputmode="numeric"></label>
<label>GW points<input id="dmPts" type="number" inputmode="numeric"></label>
<label>Total points<input id="dmTotal" type="number" min="0" inputmode="numeric"></label>
<label>Overall rank<input id="dmRank" type="number" min="1" inputmode="numeric"></label>
<label>Rank change (+ up)<input id="dmRankDiff" type="number"></label>
</div>
<div class="actions"><button class="primary" id="dmApply">Apply</button><button id="dmZero">Zero GW points</button></div>
<label>Quick rank moves</label>
<div class="grid"><button data-rank="0.6">&#9650; Big climb</button><button data-rank="0.9">&#9650; Climb</button><button data-rank="1.1" class="neg">&#9660; Drop</button><button data-rank="1.5" class="neg">&#9660; Big drop</button></div>
<label class="check opt"><input type="checkbox" id="dmFollow"> Rank moves with points from events</label>
<label class="check opt"><input type="checkbox" id="dmStale"> Stale data (amber live dot, "FPL fetch failed")</label>
</div>

<div class="card">
<h2>Player events</h2>
<div class="muted">Pick a player, then an event. Pop-ups show on the live screen, with the kit and an LED flash.</div>
<div id="dmSquad"></div>
<div class="grid" id="dmEvents" style="margin-top:10px"></div>
</div>

<div class="card">
<h2>Scenes</h2>
<div class="muted">Plays a sequence without you touching anything. Keep this page open while it runs.</div>
<label for="dmGap">Seconds between steps</label>
<input id="dmGap" type="number" min="2" max="60" value="7" inputmode="numeric">
<div id="dmScenes" style="margin-top:8px"></div>
<div class="muted" id="dmSceneStatus"></div>
<div class="actions"><button class="danger" id="dmSceneStop" hidden>Stop scene</button></div>
</div>
</div>
</div>
</main>

<div class="savebar" id="saveBar"><div><span>Unsaved changes</span><span class="row"><button id="discardBtn">Discard</button><button class="primary" id="saveBtn">Save</button></span></div></div>
<div id="toast"></div>

<script>
const $=id=>document.getElementById(id);
let state=null,draft=null;

function toast(msg){const t=$('toast');t.textContent=msg;t.classList.add('show');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),3000)}

async function api(path,opts={}){
  const r=await fetch(path,opts);
  const body=await r.json().catch(()=>({}));
  if(!r.ok)throw new Error(body.error||('HTTP '+r.status));
  return body;
}
const post=(path,data)=>api(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data||{})});

function fmtUptime(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?`${d}d ${h}h`:h?`${h}h ${m}m`:`${m}m`}
function signal(rssi){return rssi>=-60?'strong':rssi>=-72?'good':'weak'}

function render(){
  const s=state;
  $('status').textContent=s.mode==='setup'?`Setup mode | firmware ${s.firmware}`:
    `${s.ssid} (${signal(s.rssi)}) | ${s.ip} | up ${fmtUptime(s.uptimeSec)}`;
  $('setupBanner').hidden=s.mode!=='setup';
  $('fwVersion').textContent=s.firmware;
  $('pwState').textContent=s.webPasswordSet?'Password is set.':'No password: anyone on your network can open this page.';
  $('pwClear').hidden=!s.webPasswordSet;
  if(document.activeElement!==$('entryId'))$('entryId').value=s.entryId;
  if(document.activeElement!==$('ssid'))$('ssid').value=s.ssid;
  renderDraft();
}

function renderDraft(){
  $('interval').value=draft.interval;
  $('graph').value=draft.graph;
  ['ledRankBright','ledDeadlineBright','ledNotifyBright'].forEach(id=>{$(id).value=draft[id];$(id+'Val').textContent=draft[id]?draft[id]+'%':'Off'});
  $('ledRankLiveGw').checked=draft.ledRankLiveGw;$('ledRankMatch').checked=draft.ledRankMatch;
  const chips=$('watchChips');chips.innerHTML='';
  if(!draft.watchlist.length)chips.innerHTML='<span class="muted">No players watched, so the watchlist screen is skipped.</span>';
  draft.watchlist.forEach((name,i)=>{
    const c=document.createElement('span');c.className='chip';c.textContent=name;
    const b=document.createElement('button');b.textContent='×';b.title='Remove';
    b.onclick=()=>{draft.watchlist.splice(i,1);changed()};
    c.appendChild(b);chips.appendChild(c);
  });
  $('watchInput').disabled=draft.watchlist.length>=8;
  const box=$('leagues');
  if(!state.leagues){box.className='muted';box.textContent='Waiting for the first FPL update.';}
  else if(!state.leagues.length){box.className='muted';box.textContent='Not in any mini leagues.';}
  else{
    box.className='';box.innerHTML='';
    state.leagues.forEach(lg=>{
      const l=document.createElement('label');l.className='check';
      const cb=document.createElement('input');cb.type='checkbox';cb.checked=!draft.hiddenLeagues.includes(lg.id);
      cb.onchange=()=>{draft.hiddenLeagues=draft.hiddenLeagues.filter(id=>id!==lg.id);if(!cb.checked)draft.hiddenLeagues.push(lg.id);changed()};
      l.appendChild(cb);l.appendChild(document.createTextNode(lg.name));box.appendChild(l);
    });
  }
  $('saveBar').classList.toggle('show',isDirty());
}

const isDirty=()=>state&&JSON.stringify(draft)!==JSON.stringify(state.settings);
function changed(){renderDraft()}
function resetDraft(){draft=JSON.parse(JSON.stringify(state.settings))}

async function load(){
  state=await api('/api/state');
  if(!draft||!isDirty())resetDraft();
  $('displayCards').hidden=state.mode==='setup';
  $('ledCard').hidden=!state.ledRing;
  $('tabs').hidden=state.mode==='setup';
  if(state.mode==='setup'&&tab!=='settings')showTab('settings');
  render();
}

$('interval').oninput=e=>{draft.interval=parseInt(e.target.value)||0;$('saveBar').classList.toggle('show',isDirty())};
$('graph').onchange=e=>{draft.graph=parseInt(e.target.value);changed()};
['ledRankBright','ledDeadlineBright','ledNotifyBright'].forEach(id=>$(id).oninput=e=>{draft[id]=parseInt(e.target.value);changed()});
['ledRankLiveGw','ledRankMatch'].forEach(id=>$(id).onchange=e=>{draft[id]=e.target.checked;changed()});
$('watchForm').onsubmit=e=>{
  e.preventDefault();const v=$('watchInput').value.trim().toLowerCase();
  if(!v)return;
  if(draft.watchlist.includes(v)){toast('Already watched');return}
  draft.watchlist.push(v);$('watchInput').value='';changed();
};
$('discardBtn').onclick=()=>{resetDraft();renderDraft()};
$('saveBtn').onclick=async()=>{
  try{state=await post('/api/settings',draft);resetDraft();render();toast('Saved')}catch(e){toast(e.message)}
};

$('scanBtn').onclick=async()=>{
  const b=$('scanBtn');b.disabled=true;b.textContent='Scanning...';
  try{
    const nets=await api('/api/scan');const seen=new Set();$('networks').innerHTML='';
    nets.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{if(seen.has(n.ssid))return;seen.add(n.ssid);const o=document.createElement('option');o.value=n.ssid;$('networks').appendChild(o)});
    toast(`${seen.size} networks found`);$('ssid').focus();
  }catch(e){toast(e.message)}
  b.disabled=false;b.textContent='Scan';
};

async function waitForRestart(msg){
  toast(msg);
  await new Promise(r=>setTimeout(r,4000));
  for(let i=0;i<30;i++){
    try{await api('/api/state');location.reload();return}catch(e){}
    await new Promise(r=>setTimeout(r,2000));
  }
  toast('Device did not come back. If you changed WiFi, open http://fpl-buddy.local');
}

$('deviceSave').onclick=async()=>{
  const data={entryId:parseInt($('entryId').value)||0,ssid:$('ssid').value};
  // A new network always gets the typed password (empty for an open network); the same network keeps the saved one.
  if($('wifiPassword').value||data.ssid!==state.ssid)data.wifiPassword=$('wifiPassword').value;
  try{
    const r=await post('/api/device',data);
    if(!r.restart){toast('Nothing changed');return}
    if(state.mode==='setup'||data.ssid!==state.ssid){toast('Saved. Restarting: reconnect to your WiFi and open http://fpl-buddy.local');return}
    waitForRestart('Saved, restarting...');
  }catch(e){toast(e.message)}
};

$('pwSave').onclick=async()=>{
  const pw=$('webPassword').value;if(!pw){toast('Enter a password');return}
  try{await post('/api/device',{webPassword:pw});$('webPassword').value='';toast('Password set. Your browser will ask for it (user: admin).');setTimeout(()=>location.reload(),1500)}catch(e){toast(e.message)}
};
$('pwClear').onclick=async()=>{
  if(!confirm('Remove the password? Anyone on your network will be able to change settings and firmware.'))return;
  try{await post('/api/device',{webPassword:''});load();toast('Password removed')}catch(e){toast(e.message)}
};

$('fwUpload').onclick=()=>{
  const f=$('fwFile').files[0];if(!f){toast('Choose a firmware .bin file');return}
  if(!confirm(`Install ${f.name} (${Math.round(f.size/1024)} KB)? The device restarts afterwards.`))return;
  const fd=new FormData();fd.append('firmware',f,f.name);
  const x=new XMLHttpRequest();const p=$('fwProgress');p.hidden=false;p.value=0;$('fwUpload').disabled=true;
  x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded/e.total*100};
  x.onload=()=>{
    $('fwUpload').disabled=false;
    let body={};try{body=JSON.parse(x.responseText)}catch(e){}
    if(x.status===200)waitForRestart('Installed, restarting...');
    else{p.hidden=true;toast('Update failed: '+(body.error||x.status))}
  };
  x.onerror=()=>{$('fwUpload').disabled=false;p.hidden=true;toast('Upload failed (connection lost)')};
  x.open('POST','/update');x.send(fd);
};
$('restartBtn').onclick=async()=>{try{await post('/api/restart');waitForRestart('Restarting...')}catch(e){toast(e.message)}};
$('setupBtn').onclick=async()=>{
  if(!confirm('Restart as a WiFi access point (FPL-Buddy-XXXX)? Join that network to finish setup. It returns to normal after 10 minutes with nobody connected.'))return;
  try{await post('/api/restart?setup=1');toast('Restarting in setup mode')}catch(e){toast(e.message)}
};

// --- Demo tab ---
let tab='settings',demo=null,demoAt=0,selSlot=null,sceneRun=null,squadKey='';
const SCREENS={rank:'Rank',graph:'Rank graph',prices:'Price changes',watchlist:'Watchlist',leagues:'Mini leagues',deadline:'Deadline countdown',final:'Final-hour countdown',live:'Live score',popup:'Event pop-up',events:'Events list',squad:'Squad'};
const EVENTS=[['playing','Playing'],['sixty','60+ mins'],['goal','Goal'],['assist','Assist'],['cs','Clean sheet'],['save','3 saves'],['psave','Pen save'],['dc','Def. contribution'],['bonus1','Bonus 1'],['bonus2','Bonus 2'],['bonus3','Bonus 3'],['concede','Conceded 2',1],['yc','Yellow card',1],['rc','Red card',1],['og','Own goal',1],['pmiss','Pen miss',1]];
const POS=['','GK','DEF','MID','FWD'];

function showTab(t){
  tab=t;
  document.querySelectorAll('#tabs button').forEach(b=>b.classList.toggle('on',b.dataset.tab===t));
  $('tab-settings').hidden=t!=='settings';$('tab-demo').hidden=t!=='demo';
  history.replaceState(null,'',t==='demo'?'#demo':location.pathname);
  if(t==='demo')loadDemo().catch(e=>toast(e.message));
}
document.querySelectorAll('#tabs button').forEach(b=>b.onclick=()=>showTab(b.dataset.tab));

const fmtNum=n=>Number(n).toLocaleString('en-GB');
function fmtLeft(s){
  if(s<=0)return'0s';
  const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=Math.floor(s%60);
  return d?`${d}d ${h}h`:h?`${h}h ${m}m ${x}s`:m?`${m}m ${x}s`:`${x}s`;
}

async function loadDemo(){demo=await api('/api/demo');demoAt=Date.now();renderDemo()}
async function demoCmd(data,quiet){
  try{demo=await post('/api/demo',data);demoAt=Date.now();renderDemo();return true}
  catch(e){if(!quiet)toast(e.message);return false}
}

function renderNow(){
  const d=demo;if(!d||!d.enabled)return;
  $('dmNow').textContent=SCREENS[d.screen]||d.screen;
  let sub=`GW${d.gw} ${d.live?'live':'finished'} · ${d.gwPoints} pts · ${fmtNum(d.totalPoints)} total · rank ${d.rank?fmtNum(d.rank):'--'}`;
  if(d.rankDiff)sub+=d.rankDiff>0?` ▲${fmtNum(d.rankDiff)}`:` ▼${fmtNum(-d.rankDiff)}`;
  if(d.deadlineIn!=null&&d.nextGw)sub+=` · GW${d.nextGw} deadline in ${fmtLeft(d.deadlineIn-(Date.now()-demoAt)/1000)}`;
  if(d.stale)sub+=' · stale';
  $('dmNowSub').textContent=sub;
}

function renderDemo(){
  const d=demo;
  $('demoBadge').hidden=!d.enabled;
  $('dmStatus').innerHTML=d.enabled?`<b>Running</b> with ${d.source==='sample'?'the sample team':d.source==='mine'?'your team':'the serial demo squad'}.`:'<b>Off.</b> The display shows your live FPL data.';
  if(!d.clock)$('dmStatus').innerHTML+=' <span class="muted">The clock is not synced yet, so deadlines are unavailable.</span>';
  $('dmStartMine').disabled=!d.hasRealData;
  $('dmStartMine').title=d.hasRealData?'':'No FPL data yet: wait for the first update';
  $('dmStartBtns').hidden=d.enabled;$('dmRunBtns').hidden=!d.enabled;$('dmControls').hidden=!d.enabled;
  if(!d.enabled)return;
  renderNow();
  document.querySelectorAll('[data-screen]').forEach(b=>b.classList.toggle('cur',b.dataset.screen===d.screen));
  $('dmPause').checked=d.carouselPaused;$('dmAutoKick').checked=d.autoKickoff;
  $('dmFollow').checked=d.rankFollows;$('dmStale').checked=d.stale;
  if(!$('dmScoreCard').contains(document.activeElement)){
    $('dmGw').value=d.gw;$('dmPts').value=d.gwPoints;$('dmTotal').value=d.totalPoints;$('dmRank').value=d.rank||'';$('dmRankDiff').value=d.rankDiff;
  }
  const key=JSON.stringify(d.squad)+selSlot;
  if(key===squadKey)return;
  squadKey=key;
  if(!d.squad.some(p=>p.slot===selSlot))selSlot=(d.squad.find(p=>p.c)||d.squad[0]||{}).slot??null;
  const box=$('dmSquad');box.innerHTML='';
  d.squad.forEach(p=>{
    const row=document.createElement('div');
    row.className='pl'+(p.bench?' bench':'')+(p.slot===selSlot?' sel':'');
    const tag=p.c?' (C)':p.vc?' (V)':'';
    const pts=p.played?`${p.pts*(p.mult||1)} pts`:'-';
    row.innerHTML=`<span><b></b><span class="muted"></span></span><span></span>`;
    row.firstChild.firstChild.textContent=p.name+tag;
    row.firstChild.lastChild.textContent=` ${POS[p.pos]||''} ${p.team}${p.bench?' · bench':''}`;
    row.lastChild.textContent=pts;
    row.onclick=()=>{selSlot=p.slot;squadKey='';renderDemo()};
    box.appendChild(row);
  });
}

EVENTS.forEach(([type,label,neg])=>{
  const b=document.createElement('button');b.textContent=label;if(neg)b.className='neg';
  b.onclick=()=>{
    if(selSlot==null){toast('Pick a player first');return}
    const m=type.match(/^bonus(\d)$/);
    demoCmd(m?{action:'event',slot:selSlot,type:'bonus',count:+m[1]}:{action:'event',slot:selSlot,type});
  };
  $('dmEvents').appendChild(b);
});

$('tab-demo').addEventListener('click',e=>{
  const b=e.target.closest('button');if(!b||!demo)return;
  const ds=b.dataset;
  if(ds.start)demoCmd({action:'start',source:ds.start});
  else if(ds.act==='stop'){stopScene();demoCmd({action:'stop'}).then(ok=>ok&&toast('Demo stopped, fetching live data'))}
  else if(ds.act==='restart'){stopScene();demoCmd({action:'restart'})}
  else if(ds.screen)demoCmd({action:'screen',screen:ds.screen});
  else if(ds.phase)demoCmd({action:'phase',phase:ds.phase});
  else if(ds.deadline)demoCmd({action:'phase',phase:'deadline',seconds:+ds.deadline});
  else if(ds.rank){
    if(!demo.rank){toast('No rank to move');return}
    const r=Math.max(1,Math.round(demo.rank*parseFloat(ds.rank)));
    demoCmd({action:'set',rank:r,rankDiff:demo.rankDiff+(demo.rank-r)});
  }
});
$('dmDeadlineForm').onsubmit=e=>{
  e.preventDefault();
  const s=(parseInt($('dmDeadlineMin').value)||0)*60+(parseInt($('dmDeadlineSec').value)||0);
  demoCmd({action:'phase',phase:'deadline',seconds:s});
};
$('dmApply').onclick=()=>{
  const data={action:'set'};
  [['gw','dmGw'],['gwPoints','dmPts'],['totalPoints','dmTotal'],['rank','dmRank'],['rankDiff','dmRankDiff']].forEach(([k,id])=>{
    const v=parseInt($(id).value);if(!isNaN(v))data[k]=v;
  });
  demoCmd(data).then(ok=>ok&&toast('Applied'));
};
$('dmZero').onclick=()=>demoCmd({action:'clearScores'});
$('dmPause').onchange=e=>demoCmd({action:'set',carouselPaused:e.target.checked});
$('dmAutoKick').onchange=e=>demoCmd({action:'set',autoKickoff:e.target.checked});
$('dmFollow').onchange=e=>demoCmd({action:'set',rankFollows:e.target.checked});
$('dmStale').onchange=e=>demoCmd({action:'set',stale:e.target.checked});

// Scenes: steps run from this page, `gap` seconds apart unless a step sets its own wait (ms).
function roles(){
  const st=(demo.squad||[]).filter(p=>!p.bench);
  const cap=st.find(p=>p.c)||st[0];
  const by=pos=>st.find(p=>p.pos===pos&&p!==cap)||cap;
  return{cap,st,gk:by(1),def:by(2),mid:by(3),fwd:by(4),def2:st.filter(p=>p.pos===2)[1]||by(2),mid2:st.filter(p=>p.pos===3&&p!==cap)[1]||by(3)};
}
const ev=(p,type,count)=>({a:{action:'event',slot:p.slot,type,count}});
const set=(data,wait=0)=>({a:Object.assign({action:'set'},data),wait});
const scr=screen=>({a:{action:'screen',screen}});
const SCENES=[
  {name:'Deadline to kick-off',desc:'The last 15 seconds of the deadline, then the gameweek goes live and players come on.',steps:r=>[
    set({autoKickoff:true}),{a:{action:'phase',phase:'deadline',seconds:15},wait:19000},
    ev(r.cap,'playing'),ev(r.mid,'playing'),ev(r.def,'playing')]},
  {name:'Matchday highlights',desc:'Captain scores, an assist, a yellow card, a clean sheet and bonus points.',steps:r=>[
    {a:{action:'phase',phase:'live'},wait:3000},ev(r.cap,'goal'),ev(r.mid,'assist'),ev(r.def2,'yc'),ev(r.def,'cs'),ev(r.cap,'bonus',3),scr('squad')]},
  {name:'Rank rollercoaster',desc:'A penalty miss, an own goal and a red card send the rank down, then goals bring it back.',steps:r=>[
    set({rankFollows:true}),{a:{action:'phase',phase:'live'},wait:3000},
    ev(r.fwd,'pmiss'),ev(r.def,'og'),ev(r.mid2,'rc'),ev(r.cap,'goal'),ev(r.mid,'goal'),ev(r.cap,'goal'),ev(r.cap,'bonus',3)]},
  {name:'Deadline day',desc:'Deadline countdown at 5h 59m, then the final hour with the LED ring, then kick-off.',steps:r=>[
    set({autoKickoff:true}),{a:{action:'phase',phase:'deadline',seconds:21540},wait:gap()*2000},
    {a:{action:'phase',phase:'deadline',seconds:3590},wait:gap()*2000},{a:{action:'phase',phase:'deadline',seconds:12},wait:17000},
    ev(r.cap,'playing')]},
  {name:'Between-gameweeks tour',desc:'Finishes the gameweek and steps through every carousel screen.',steps:r=>[
    {a:{action:'phase',phase:'idle'},wait:3000},set({carouselPaused:true}),scr('rank'),scr('graph'),scr('prices'),scr('watchlist'),scr('leagues'),set({carouselPaused:false})]},
];
const gap=()=>Math.min(60,Math.max(2,parseInt($('dmGap').value)||7));
function stopScene(){if(sceneRun){sceneRun.stop=true;sceneRun.wake&&sceneRun.wake()}}
async function playScene(sc){
  stopScene();
  const r=roles();if(!r.cap){toast('The demo squad is empty');return}
  const run={stop:false};sceneRun=run;
  const steps=sc.steps(r);
  $('dmSceneStop').hidden=false;
  for(let i=0;i<steps.length&&!run.stop;i++){
    $('dmSceneStatus').textContent=`${sc.name}: step ${i+1} of ${steps.length}`;
    const ok=await demoCmd(steps[i].a,true);
    const wait=!ok?300:(steps[i].wait!=null?steps[i].wait:gap()*1000);
    if(i<steps.length-1)await new Promise(res=>{run.wake=res;setTimeout(res,wait)});
  }
  if(sceneRun===run){sceneRun=null;$('dmSceneStop').hidden=true;$('dmSceneStatus').textContent=run.stop?`${sc.name}: stopped`:`${sc.name}: done`}
}
SCENES.forEach(sc=>{
  const row=document.createElement('div');row.className='scene';
  row.innerHTML='<div><b></b><div class="muted"></div></div>';
  row.firstChild.firstChild.textContent=sc.name;row.firstChild.lastChild.textContent=sc.desc;
  const b=document.createElement('button');b.textContent='Play';b.onclick=()=>playScene(sc);
  row.appendChild(b);$('dmScenes').appendChild(row);
});
$('dmSceneStop').onclick=stopScene;

load().then(()=>{if(state.mode!=='setup')loadDemo().catch(()=>{})}).catch(e=>{$('status').textContent='Could not load: '+e.message});
if(location.hash==='#demo')showTab('demo');
setInterval(()=>{if(!document.hidden)load().catch(()=>{})},15000);
// Every 2 s while a demo runs; every 10 s otherwise, so the page adds little load while the device polls FPL.
let demoTick=0;
setInterval(()=>{if(!document.hidden&&tab==='demo'&&(demo&&demo.enabled||++demoTick%5===0))loadDemo().catch(()=>{})},2000);
setInterval(()=>{if(tab==='demo')renderNow()},1000);
</script>
</body>
</html>
)HTML";
