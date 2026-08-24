// ============================================================================
// app.js - SPA for the device web UI (Tools / Status / Settings)
// ----------------------------------------------------------------------------
// Views:
//  tools    : BadUSB editor (line numbers + DuckyScript highlighting, run,
//             save/load from SD, autostart manager) + File Browser
//             + Command Reference panel.
//  status   : system stats, script state, debug log, reboot/reset/format.
//  settings : WiFi creds, login creds, encryption password, display defaults,
//             interface disable modes (temporary / permanent).
// ============================================================================
"use strict";
const $ = s => document.querySelector(s);
const view = $("#view");
async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  if (r.status === 401) { location.href = "/login.html"; throw new Error("auth"); }
  const t = await r.text();
  try { return JSON.parse(t); } catch { return t; }
}
const jpost = (p, body) => api(p, {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(body)});
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

/* ============================== TOOLS VIEW ============================== */
function toolsView() {
  view.innerHTML = `
  <div class="panel"><h2>BadUSB &mdash; DuckyScript Editor</h2>
    <div style="display:flex;gap:8px;margin-bottom:8px">
      <select id="scriptList" style="width:auto"></select>
      <button class="small" onclick="loadScript()">Load</button>
      <button class="small danger" onclick="delScript()">Delete</button>
      <span style="flex:1"></span>
      <button class="small" onclick="newScript()">New</button>
      <button class="small" onclick="saveScript()">Save</button>
      <button class="small ok" id="runBtn" onclick="runScript()">&#9654; Run</button>
      <button class="small danger" onclick="api('/api/stop',{method:'POST'})">&#9632; Stop</button>
    </div>
    <div class="editor-wrap">
      <pre id="gutter">1</pre>
      <div id="editorPane"><pre id="hl"></pre>
        <textarea id="code" spellcheck="false"
          placeholder="Type DuckyScript here...&#10;e.g.&#10;DELAY 1000&#10;GUI r&#10;STRING notepad&#10;ENTER"></textarea>
      </div>
    </div>
    <label style="margin-top:8px">Save as filename (.ds)
      <input id="scriptName" placeholder="payload.ds" style="max-width:240px">
    </label>
  </div>

  <div class="panel"><h2>Autostart Scripts (run in order on plug-in)</h2>
    <ul id="autoList" class="muted"></ul>
    <div style="display:flex;gap:8px">
      <select id="autoPick" style="width:auto"></select>
      <button class="small" onclick="addAuto()">Add to queue</button>
      <button class="small" onclick="clearAuto()">Clear</button>
    </div>
  </div>

  <div class="panel"><h2>File Browser</h2>
    <div style="display:flex;gap:8px;margin-bottom:8px">
      <span id="fbPath" class="muted">/</span><span style="flex:1"></span>
      <button class="small" onclick="fbUp()">Up</button>
      <input type="file" id="upFile" style="display:none" onchange="upload()">
      <button class="small" onclick="$('#upFile').click()">Upload here</button>
      <button class="small" onclick="fbNew()">New file</button>
    </div>
    <table id="fbTable"></table>
  </div>

  ${refPanel()}`;
  refreshScripts();
  fbGo("/");
}

/* -------- editor helpers -------- */
const CODE = () => $("#code");
const COMMANDS = /^(REM|REM_BLOCK_(START|END)|DELAY|DEFAULTDELAY|DEFAULT_DELAY|STRING|STRINGLN|ENTER|SPACE|TAB|ESCAPE|DOWNARROW|UPARROW|LEFTARROW|RIGHTARROW|BACKSPACE|DELETE|HOME|INSERT|PAGEUP|PAGEDOWN|CAPSLOCK|APP|REPEAT|LOG|GUI|WINDOWS|COMMAND|CTRL|CONTROL|ALT|ALTGR|SHIFT|F\d{1,2})\b/i;
function highlight() {
  const lines = CODE().value.split("\n");
  let out = "", g = "";
  lines.forEach((ln, i) => {
    g += (i + 1) + "\n";
    if (/^\s*(REM|#)/i.test(ln)) out += `<span class="tok-rem">${esc(ln)}</span>`;
    else {
      const m = ln.match(COMMANDS);
      if (m) {
        const rest = ln.slice(m[0].length);
        out += `<span class="tok-cmd">${esc(m[0])}</span>` +
               (/^STRING/i.test(m[0]) ? `<span class="tok-str">${esc(rest)}</span>`
                                      : esc(rest).replace(/\b(\d+)\b/g,'<span class="tok-num">$1</span>'));
      } else out += esc(ln);
    }
    out += "\n";
  });
  $("#hl").innerHTML = out;
  $("#gutter").textContent = g;
}
function syncScroll(){ $("#hl").scrollTop=CODE().scrollTop;$("#hl").scrollLeft=CODE().scrollLeft;
  $("#gutter").scrollTop=CODE().scrollTop; }
document.addEventListener("input", e => { if(e.target.id==="code"){highlight();syncScroll();} });
document.addEventListener("scroll", e => {}, true);

async function refreshScripts() {
  const list = await api("/api/scripts");
  $("#scriptList").innerHTML = list.map(s=>`<option>${esc(s.name)}</option>`).join("");
  $("#autoPick").innerHTML  = list.map(s=>`<option>${esc(s.name)}</option>`).join("");
  renderAuto();
}
async function loadScript(){
  const n=$("#scriptList").value; if(!n)return;
  CODE().value = await api("/api/script?name="+encodeURIComponent(n));
  $("#scriptName").value=n; highlight();
}
function newScript(){ CODE().value=""; $("#scriptName").value=""; highlight(); }
async function saveScript(){
  const n=$("#scriptName").value.trim(); if(!n) return alert("Enter a filename first");
  await jpost("/api/script",{name:n,text:CODE().value}); refreshScripts();
}
async function delScript(){
  const n=$("#scriptList").value; if(!n||!confirm("Delete "+n+"?"))return;
  await api("/api/script?name="+encodeURIComponent(n),{method:"DELETE"}); refreshScripts();
}
async function runScript(){
  // Run what's in the editor; falls back to selected saved script server-side.
  const text=CODE().value;
  await jpost("/api/run", text?{text}: {name:$("#scriptList").value});
}
/* -------- autostart -------- */
let autoQ=[];
async function renderAuto(){
  autoQ=await api("/api/autostart");
  $("#autoList").innerHTML=autoQ.map((n,i)=>`<li>${i+1}. ${esc(n)}
    <button class="small" onclick="rmAuto(${i})">x</button></li>`).join("")||"<li>(none)</li>";
}
async function addAuto(){ const n=$("#autoPick").value; if(!n)return;
  autoQ.push(n); await jpost("/api/autostart",{names:autoQ}); renderAuto(); }
async function rmAuto(i){ autoQ.splice(i,1); await jpost("/api/autostart",{names:autoQ}); renderAuto(); }
async function clearAuto(){ await jpost("/api/autostart",{names:[]}); renderAuto(); }

/* -------- file browser -------- */
let fbCur="/";
async function fbGo(p){
  fbCur=p; $("#fbPath").textContent=p;
  const items=await api("/api/files?path="+encodeURIComponent(p));
  $("#fbTable").innerHTML="<tr><th>Name</th><th>Type</th><th>Size</th><th></th></tr>"+
   items.map(f=>{
     const fp=(p==="/")?"/"+f.name:p+"/"+f.name;
     let act=f.dir?`<a href="#" onclick="fbGo('${esc(fp)}');return false">Open</a>`:
       f.name.endsWith(".ds")?`<a href="#" onclick="runSd('${esc(fp)}');return false">Run</a> | `+
       `<a href="#" onclick="editSd('${esc(fp)}');return false">Edit</a>`:`<a href="#" onclick="editSd('${esc(fp)}');return false">Edit</a>`;
     return `<tr><td>${esc(f.name)}</td><td>${f.dir?"dir":"file"}</td><td>${f.size}</td>
       <td>${act} | <a href="#" onclick="delFb('${esc(fp)}');return false" class="err">Del</a></td></tr>`;
   }).join("");
}
function fbUp(){ fbGo(fbCur.replace(/\/[^/]*$/,"")||"/"); }
async function delFb(p){ if(confirm("Delete "+p+"?")){await api("/api/file?path="+encodeURIComponent(p),{method:"DELETE"});fbGo(fbCur);} }
async function runSd(p){ await jpost("/api/run",{name:p.split("/").pop()}); }
function upload(){ const f=$("#upFile").files[0]; if(!f)return;
  fetch("/api/upload?path="+encodeURIComponent(fbCur+"/"+f.name),{method:"POST",body:f})
   .then(()=>fbGo(fbCur)); }
function editSd(p){
  // Simple prompt-based editor for arbitrary files (scripts open big editor)
  if(p.endsWith(".ds")){ fetch("/api/file?path="+encodeURIComponent(p)).then(r=>r.text()).then(t=>{
      switchToToolsIf(); CODE().value=t; $("#scriptName").value=p.split("/").pop(); highlight(); });
    return; }
  fetch("/api/file?path="+encodeURIComponent(p)).then(r=>r.text()).then(t=>{
    const nv=prompt("Edit "+p, t.slice(0,4000)); if(nv===null)return;
    jpost("/api/file",{path:p,content:nv}).then(()=>fbGo(fbCur)); });
}
function fbNew(){ const n=prompt("New file name"); if(!n)return;
  jpost("/api/file",{path:fbCur+"/"+n,content:""}).then(()=>fbGo(fbCur)); }

/* ============================ STATUS VIEW ============================= */
let statusTimer=null;
function statusView(){
  view.innerHTML=`<div class="panel"><h2>System Status</h2><table id="statT"></table></div>
  <div class="panel"><h2>Debug Log</h2><button class="small" onclick="refreshLog()">Refresh</button>
    <pre id="logBox" style="max-height:300px;overflow:auto;background:#0d0d16;padding:8px;border-radius:5px"></pre></div>
  <div class="panel"><h2>Danger Zone</h2>
    <button class="danger" onclick="if(confirm('Reboot device?'))jpost('/api/reboot',{})">Reboot</button>
    <button class="danger" onclick="if(confirm('Factory reset settings?'))jpost('/api/reset',{})">Reset Firmware Settings</button>
    <button class="danger" onclick="if(confirm('FORMAT SD CARD? ALL FILES WILL BE LOST!'))jpost('/api/format-sd',{})">Format Micro-SD</button>
  </div>`;
  refreshStatus(); statusTimer=setInterval(refreshStatus,2000); refreshLog();
}
async function refreshStatus(){
  const s=await api("/api/status");
  const up=Math.floor(s.uptime), hh=Math.floor(up/3600), mm=Math.floor(up%3600/60), ss=up%60;
  const st=s.scriptState==="RUNNING"?["run","Running"]:s.scriptState==="FINISHED"?["fin","Finished"]:["sb","Standby"];
  $("#statT").innerHTML=`
   <tr><td>Free RAM</td><td>${(s.heap/1024).toFixed(0)} KB (min ${(s.heapMin/1024)|0} KB)</td></tr>
   <tr><td>CPU</td><td>${s.cpuMhz} MHz</td></tr>
   <tr><td>Uptime</td><td>${hh}h ${mm}m ${ss}s</td></tr>
   <tr><td>Flash</td><td>${(s.flashSize/1048576)|0} MB</td></tr>
   <tr><td>SD Free</td><td>${s.sdTotal?((s.sdFree/1048576).toFixed(1)+" / "+(s.sdTotal/1048576).toFixed(1)+" MB"):"not detected"}</td></tr>
   <tr><td>Connection</td><td>${s.usbHost?"Plugged into computer":"Power only"}</td></tr>
   <tr><td>WiFi AP</td><td>${s.ip} (${s.wifiClients} client(s))</td></tr>
   <tr><td>Script</td><td><span class="badge ${st[0]}">${st[1]}</span> ${esc(s.scriptName)}
        @${new Date(s.scriptSince*1000).toLocaleString()}</td></tr>`;
}
async function refreshLog(){ $("#logBox").textContent=await api("/api/log"); }

/* =========================== SETTINGS VIEW ============================ */
function settingsView(){
  view.innerHTML=`<div class="panel"><h2>WiFi Access Point</h2>
    <label>SSID<input id="ssid"></label><label>Password<input id="wifiPass" type="password"></label></div>
  <div class="panel"><h2>Login Credentials</h2>
    <label>Username<input id="user"></label><label>Password<input id="webPass" type="password"></label></div>
  <div class="panel"><h2>Encryption Password</h2>
    <p class="muted">Used to encrypt scripts/logs on the SD card. Changing it makes old files unreadable.</p>
    <label>Password<input id="encPassword" type="password"></label></div>
  <div class="panel"><h2>Display &amp; LED Defaults</h2>
    <label><input type="checkbox" id="screenOnBoot" style="width:auto"> Screen on at boot</label>
    <label><input type="checkbox" id="ledOnBoot" style="width:auto"> LED on at boot</label>
    <label>Backlight brightness <input type="number" id="brightness" min="0" max="255" style="max-width:120px"></label></div>
  <div class="panel"><h2>Interface</h2>
    <label><input type="checkbox" id="tempOff" style="width:auto"> Temporarily disable web interface (press BOOT button to re-enable)</label>
    <p class="err">Permanent mode disables the interface until the firmware is re-flashed!</p>
    <button class="danger" onclick="permDisable()">Enable Permanent Disable</button></div>
  <button onclick="saveSettings()">Save Settings</button>`;
}
async function saveSettings(){
  const b={ ssid:$("#ssid").value, wifiPass:$("#wifiPass").value,
    user:$("#user").value, webPass:$("#webPass").value,
    encPassword:$("#encPassword").value,
    screenOnBoot:$("#screenOnBoot").checked, ledOnBoot:$("#ledOnBoot").checked,
    brightness:+$("#brightness").value, tempOff:$("#tempOff").checked };
  await jpost("/api/settings", b);
  alert("Saved. Some settings apply after reboot.");
}
async function permDisable(){
  if(!confirm("PERMANENTLY disable web interface?\nOnly a firmware re-flash can undo this!"))return;
  await jpost("/api/settings",{permOff:true});
  alert("Interface disabled after next reboot.");
}

/* ======================= COMMAND REFERENCE PANEL ====================== */
function refPanel(){
  const rows=[["REM x","Comment"],["DELAY ms","Wait"],["DEFAULTDELAY ms","Delay after every command"],
   ["STRING txt","Type text"],["STRINGLN txt","Type text + Enter"],["ENTER / SPACE / TAB","Keys"],
   ["GUI/CTRL/ALT/SHIFT combo","e.g. `GUI r`, `CTRL-SHIFT ESC`"],["BACKSPACE/DELETE/ESC/...","Special keys"],
   ["F1..F12","Function keys"],["REPEAT n","Repeat previous line n times"],
   ["LOG msg","Write msg to encrypted device log"]];
  return `<div class="panel"><h2>DuckyScript Reference</h2><table>${
    rows.map(r=>`<tr><td><b>${r[0]}</b></td><td class="muted">${r[1]}</td></tr>`).join("")}</table></div>`;
}

/* ============================== ROUTER ================================ */
function route(){
  clearInterval(statusTimer);
  const h=location.hash||"#tools";
  document.querySelectorAll("nav a").forEach(a=>a.classList.toggle("active",a.getAttribute("href")===h));
  if(h==="#status")statusView(); else if(h==="#settings")settingsView(); else toolsView();
}
window.onhashchange=route;
$("#logout").onclick=()=>fetch("/api/login",{method:"POST"}).then(()=>location.href="/login.html");
route(); setInterval(()=>{ if($("#logBox"))refreshLog(); },5000);
