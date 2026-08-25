// ============================================================================
// app.js - SPA for the device web UI
// ----------------------------------------------------------------------------
// Views: tools (BadUSB editor) / files (browser) / status / settings.
// All popups are in-page (modal system + toasts) - no alert/confirm/prompt.
// Text is normalised to LF everywhere so Windows/Linux/Mac all behave the same.
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

/* ===================== TOASTS + MODALS (no native popups) ================= */
function toast(msg, kind="ok") {
  const el = document.createElement("div");
  el.className = "toast " + kind;
  el.textContent = msg;
  $("#toastHost").appendChild(el);
  setTimeout(() => { el.classList.add("show"); }, 10);
  setTimeout(() => { el.classList.remove("show"); setTimeout(()=>el.remove(), 400); }, 3000);
}
// confirmModal(msg) -> Promise<bool>; promptModal(title,value)->Promise<string|null>
function modal(html) {
  return new Promise(resolve => {
    const host = $("#modalHost");
    host.innerHTML = `<div class="overlay"><div class="modal">${html}</div></div>`;
    const close = v => { host.innerHTML = ""; resolve(v); };
    window._closeModal = close;
  });
}
const confirmModal = msg => modal(`<p>${esc(msg)}</p>
  <div class="row-end"><button class="danger" onclick="_closeModal(true)">Confirm</button>
  <button onclick="_closeModal(false)">Cancel</button></div>`);
const promptModal = (title, value="") => modal(`<label>${esc(title)}
  <input id="mInput" value="${esc(value)}"></label>
  <div class="row-end"><button onclick="_closeModal($('#mInput').value)">Save</button>
  <button onclick="_closeModal(null)">Cancel</button></div>`);

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
      <button class="small danger" id="stopBtn" onclick="api('/api/stop',{method:'POST'})" style="display:none">&#9632; Stop</button>
    </div>
    <div class="editor-wrap">
      <pre id="gutter">1</pre>
      <textarea id="code" spellcheck="false"
        placeholder="Type DuckyScript here...&#10;e.g.&#10;DELAY 1000&#10;GUI r&#10;STRING notepad&#10;ENTER"></textarea>
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
  </div>`;
  // Normalise CRLF -> LF and wire up editor events once per render.
  const code = CODE();
  code.value = code.value.replace(/\r\n?/g, "\n");
  code.addEventListener("input", syncGutter);
  code.addEventListener("scroll", ()=>{ $("#gutter").scrollTop=code.scrollTop; });
  syncGutter();
  refreshScripts();
}

/* -------- editor helpers -------- */
// NOTE(cursor): the syntax-highlight overlay was removed - transparent-text
// overlays are fragile across browsers/zoom levels and misaligned the caret.
// Plain textarea + synced line-number gutter for Step 1; a proper editor
// component returns in Step 5 UI polish.
const CODE = () => $("#code");
function syncGutter(){
  const c=CODE(), gt=$("#gutter"); if(!c||!gt)return;
  const n=c.value.split("\n").length;
  let g=""; for(let i=1;i<=n;i++) g+=i+"\n";
  gt.textContent=g; gt.scrollTop=c.scrollTop;
}
// Poll script state: hide Run while running, hide Stop when idle.
setInterval(async()=>{
  const rb=$("#runBtn"), sb=$("#stopBtn"); if(!rb||!sb)return;
  try{ const s=await api("/api/status");
    const running=(s.scriptState==="RUNNING");
    rb.style.display=running?"none":"";
    sb.style.display=running?"":"none";
    if(running) sb.innerHTML="&#9632; Stop "+esc(s.scriptName);
  }catch(e){}
},2000);

async function refreshScripts() {
  const list = await api("/api/scripts");
  $("#scriptList").innerHTML = list.map(s=>`<option>${esc(s.name)}</option>`).join("");
  $("#autoPick").innerHTML  = list.map(s=>`<option>${esc(s.name)}</option>`).join("");
  renderAuto();
}
async function loadScript(){
  const n=$("#scriptList").value; if(!n)return;
  const t = await api("/api/script?name="+encodeURIComponent(n));
  CODE().value = String(t).replace(/\r\n?/g,"\n");   // normalise EOL cross-platform
  $("#scriptName").value=n; syncGutter(); toast("Loaded "+n);
}
function newScript(){ CODE().value=""; $("#scriptName").value=""; syncGutter(); toast("New script"); }
async function saveScript(){
  const n=$("#scriptName").value.trim(); if(!n) return toast("Enter a filename first","err");
  await jpost("/api/script",{name:n,text:CODE().value}); refreshScripts();
  toast("Saved "+n);
}
async function delScript(){
  const n=$("#scriptList").value;
  if(!n || !(await confirmModal("Delete "+n+"?")))return;
  await api("/api/script?name="+encodeURIComponent(n),{method:"DELETE"}); refreshScripts();
  toast("Deleted "+n);
}
async function runScript(){
  const text=CODE().value.replace(/\r\n?/g,"\n");
  await jpost("/api/run", text.trim()?{text}: {name:$("#scriptList").value});
  toast("Script started","ok");
}
/* -------- autostart -------- */
let autoQ=[];
async function renderAuto(){
  autoQ=await api("/api/autostart");
  $("#autoList").innerHTML=autoQ.map((n,i)=>`<li>${i+1}. ${esc(n)}
    <button class="small" onclick="rmAuto(${i})">x</button></li>`).join("")||"<li>(none)</li>";
}
async function autoSet(names){
  const r=await jpost("/api/autostart",{names});
  if(r && r.ok===false) toast("Save failed: "+(r.error||"SD write error"),"err");
}
async function addAuto(){ const n=$("#autoPick").value; if(!n)return;
  autoQ.push(n); await autoSet(autoQ); await renderAuto(); toast("Added to autostart"); }
async function rmAuto(i){ autoQ.splice(i,1); await autoSet(autoQ); await renderAuto(); toast("Removed"); }
async function clearAuto(){ await autoSet([]); await renderAuto(); toast("Autostart cleared"); }

/* ============================ FILES VIEW ================================ */
let fbCur="/";
function filesView(){
  view.innerHTML=`<div class="panel"><h2>File Browser</h2>
    <div style="display:flex;gap:8px;margin-bottom:8px">
      <span id="fbPath" class="muted">/</span><span style="flex:1"></span>
      <button class="small" onclick="fbUp()">Up</button>
      <input type="file" id="upFile" style="display:none" onchange="upload()">
      <button class="small" onclick="$('#upFile').click()">Upload here</button>
      <button class="small" onclick="fbNew()">New file</button>
      <button class="small" onclick="fbNewDir()">New folder</button>
    </div>
    <table id="fbTable"></table></div>`;
  fbGo("/");
}
async function fbGo(p){
  fbCur=p; $("#fbPath").textContent=p;
  try{
    const items=await api("/api/files?path="+encodeURIComponent(p));
    $("#fbTable").innerHTML="<tr><th>Name</th><th>Type</th><th>Size</th><th></th></tr>"+
     items.map(f=>{
       const fp=(p==="/")?"/"+f.name:p+"/"+f.name;
       const act=f.dir?`<a href="#" onclick="fbGo('${esc(fp)}');return false">Open</a>`
        :`${f.name.endsWith(".ds")?`<a href="#" onclick="runSd('${esc(fp)}');return false">Run</a> | `:""}<a href="#" onclick="editSd('${esc(fp)}');return false">Edit</a>`;
       return `<tr><td>${esc(f.name)}</td><td>${f.dir?"dir":"file"}</td><td>${f.size}</td>
         <td>${act} | <a href="#" onclick="delFb('${esc(fp)}');return false" class="err">Del</a></td></tr>`;
     }).join("");
  }catch(e){ toast("Cannot open "+p,"err"); }
}
function fbUp(){ fbGo(fbCur.replace(/\/[^/]*$/,"")||"/"); }
async function delFb(p){ if(await confirmModal("Delete "+p+"?")){await api("/api/file?path="+encodeURIComponent(p),{method:"DELETE"});fbGo(fbCur);} }
async function runSd(p){ await jpost("/api/run",{name:p.split("/").pop()}); toast("Running "+p); }
async function upload(){
  const f=$("#upFile").files[0]; if(!f)return;
  try{
    const b64=btoa(await f.text());          // small files only (Step-1 scope)
    const r=await fetch("/api/filebin?path="+encodeURIComponent(fbCur+"/"+f.name),
      {method:"POST",headers:{"Content-Type":"application/json"},
       body:JSON.stringify({b64})});
    if(r.ok){toast("Uploaded "+f.name); fbGo(fbCur);}
    else toast("Upload failed ("+r.status+")","err");
  }catch(e){ toast("Upload failed","err"); }
}
// Edit any file in an in-page modal. .ds files come back decrypted from API.
async function editSd(p){
  const t = await fetch("/api/file?path="+encodeURIComponent(p)).then(r=>r.ok?r.text():null)
                 .catch(()=>null);
  if(t===null) return toast("Could not read "+p,"err");
  await modal(`<label>Edit ${esc(p)}
    <textarea id="mEdit" rows="16" style="font-family:'Courier New',monospace">${esc(String(t).replace(/\r\n?/g,"\n"))}</textarea></label>
    <div class="row-end"><button onclick="_closeModal($('#mEdit').value)">Save</button>
    <button onclick="_closeModal(null)">Cancel</button></div>`)
    .then(async content=>{
      if(content===null)return;
      await jpost("/api/file",{path:p,content});
      toast("Saved "+p); fbGo(fbCur);
    });
}
async function fbNew(){
  const n=await promptModal("New file name"); if(!n)return;
  await jpost("/api/file",{path:(fbCur==="/"?"":fbCur)+"/"+n,content:""});
  fbGo(fbCur); toast("Created "+n);
}
async function fbNewDir(){
  const n=await promptModal("New folder name"); if(!n)return;
  await api("/api/mkdir?path="+encodeURIComponent((fbCur==="/"?"":fbCur)+"/"+n),{method:"POST"});
  fbGo(fbCur);
}

/* ============================ STATUS VIEW ============================= */
let statusTimer=null;
function statusView(){
  view.innerHTML=`<div class="panel"><h2>System Status</h2><table id="statT"></table></div>
  <div class="panel"><h2>Debug Log</h2><button class="small" onclick="refreshLog()">Refresh</button>
    <pre id="logBox" style="max-height:300px;overflow:auto;background:#0d0d16;padding:8px;border-radius:5px"></pre></div>
  <div class="panel"><h2>Danger Zone</h2>
    <button class="danger" onclick="doReboot()">Reboot</button>
    <button class="danger" onclick="doReset()">Reset Firmware Settings</button>
    <button class="danger" onclick="doFormat()">Format Micro-SD</button>
  </div>`;
  refreshStatus(); statusTimer=setInterval(refreshStatus,2000); refreshLog();
}
async function doReboot(){ if(await confirmModal("Reboot device?")) await jpost("/api/reboot",{}); toast("Rebooting..."); }
async function doReset(){ if(await confirmModal("Factory reset ALL settings?")) await jpost("/api/reset",{}); toast("Settings reset"); }
async function doFormat(){ if(await confirmModal("FORMAT SD CARD? ALL FILES WILL BE LOST!"))
  if(await confirmModal("Are you REALLY sure? This cannot be undone.")) await jpost("/api/format-sd",{}); toast("SD wiped"); }
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
   <tr><td>Connection</td><td>${s.usbHost?"Plugged into computer":"Power only"}${s.detectedOS&&s.detectedOS!=="Unknown"?" &mdash; "+esc(s.detectedOS):""}</td></tr>
   <tr><td>WiFi AP</td><td>${s.ip} (${s.wifiClients} client(s))</td></tr>
   <tr><td>Script</td><td><span class="badge ${st[0]}">${st[1]}</span> ${esc(s.scriptName)}</td></tr>`;
}
async function refreshLog(){ const b=$("#logBox"); if(b) b.textContent=await api("/api/log"); }

/* =========================== SETTINGS VIEW ============================ */
function settingsView(){
  view.innerHTML=`<div class="panel"><h2>WiFi Access Point</h2>
    <p class="muted">Leave a box empty to keep the current value.</p>
    <label>SSID<input id="ssid"></label><label>Password<input id="wifiPass" type="password"></label></div>
  <div class="panel"><h2>Login Credentials</h2>
    <label>Username<input id="user"></label><label>Password<input id="webPass" type="password"></label></div>
  <div class="panel"><h2>Encryption Password</h2>
    <p class="muted">Used to encrypt scripts/logs on the SD card. Changing it makes old files unreadable.</p>
    <label>Password<input id="encPassword" type="password"></label></div>
  <div class="panel"><h2>Display &amp; LED Defaults</h2>
    <label><input type="checkbox" id="screenOnBoot" style="width:auto"> Screen on at boot</label>
    <label><input type="checkbox" id="ledOnBoot" style="width:auto"> LED on at boot</label>
    <label>Backlight brightness <input type="number" id="brightness" min="0" max="255" value="128" style="max-width:120px"></label>
    <label><input type="checkbox" id="autoDetectOS" style="width:auto"> Auto-detect OS when plugged into a computer (cached until unplug)</label></div>
  <div class="panel"><h2>USB Identity (Spoofing)</h2>
    <p class="muted">What the host sees when the dongle enumerates. Applies on next boot/plug-in.</p>
    <label>Preset<select id="spoofPreset" onchange="applyPreset()"><option value="">- custom -</option></select></label>
    <label>VID (hex)<input id="spoofVid" placeholder="1E7D" style="max-width:140px"></label>
    <label>PID (hex)<input id="spoofPid" placeholder="2E7D" style="max-width:140px"></label>
    <label>Vendor<input id="spoofVendor"></label>
    <label>Product<input id="spoofProduct"></label>
    <label>Serial (empty = random 12-digit each change)<input id="spoofSerial"></label>
    <button class="small" onclick="saveSpoof()">Save Identity</button>
    <button class="small" onclick="randomSpoof()">Randomize</button>
  </div>
  <div class="panel"><h2>Interface</h2>
    <label><input type="checkbox" id="tempOff" style="width:auto"> Temporarily disable web interface (press BOOT button to re-enable)</label>
    <p class="err">Permanent mode disables the interface until the firmware is re-flashed!</p>
    <button class="danger" onclick="permDisable()">Enable Permanent Disable</button></div>
  <button onclick="saveSettings()">Save Settings</button>`;
  loadSettingsState();
}
// Populate the form with the currently saved values (secrets stay empty).
async function loadSettingsState(){
  try{
    const s=await api("/api/settings");
    $("#ssid").value=s.ssid||""; $("#user").value=s.user||"";
    $("#screenOnBoot").checked=!!s.screenOnBoot;
    $("#ledOnBoot").checked=!!s.ledOnBoot;
    $("#brightness").value=s.brightness??128;
    $("#autoDetectOS").checked=!!s.autoDetectOS;
    if(s.permOff) toast("Interface is PERMANENTLY disabled (takes effect on reboot)","err");
  }catch(e){ /* leave defaults */ }
  loadSpoof();
}
async function loadSpoof(){
  try{
    const sp=await api("/api/spoof");
    $("#spoofVid").value=sp.vid; $("#spoofPid").value=sp.pid;
    $("#spoofVendor").value=sp.vendor; $("#spoofProduct").value=sp.product;
    $("#spoofSerial").value=sp.serial;
    $("#spoofPreset").innerHTML='<option value="">- custom -</option>'+
      sp.presets.map((p,i)=>`<option value="${i}">${esc(p.vendor)} - ${esc(p.product)}</option>`).join("");
    window._presets=sp.presets;
  }catch(e){}
}
function applyPreset(){
  const i=$("#spoofPreset").value; if(i==="")return;
  const p=window._presets[+i];
  $("#spoofVid").value=p.vid; $("#spoofPid").value=p.pid;
  $("#spoofVendor").value=p.vendor; $("#spoofProduct").value=p.product;
}
async function saveSpoof(){
  await jpost("/api/spoof",{vid:$("#spoofVid").value,pid:$("#spoofPid").value,
    vendor:$("#spoofVendor").value,product:$("#spoofProduct").value,serial:$("#spoofSerial").value});
  toast("Identity saved - applies on next boot/plug-in");
}
async function randomSpoof(){
  await jpost("/api/spoof",{randomize:true});
  loadSpoof(); toast("Random identity saved");
}
async function saveSettings(){
  // Only send filled boxes - server treats empty strings as "unchanged".
  const b={};
  for(const [id,key] of [["ssid","ssid"],["wifiPass","wifiPass"],["user","user"],
                          ["webPass","webPass"],["encPassword","encPassword"]]){
    const v=$(("#"+id)).value; if(v.length) b[key]=v;
  }
  b.screenOnBoot=$("#screenOnBoot").checked;
  b.ledOnBoot=$("#ledOnBoot").checked;
  b.brightness=+$("#brightness").value;
  b.autoDetectOS=$("#autoDetectOS").checked;
  b.tempOff=$("#tempOff").checked;
  await jpost("/api/settings", b);
  toast("Settings saved. Some changes apply after reboot.");
}
async function permDisable(){
  if(!(await confirmModal("PERMANENTLY disable web interface?\nOnly a firmware re-flash can undo this!")))return;
  await jpost("/api/settings",{permOff:true});
  toast("Interface will stay off after next reboot.","err");
}

/* =========================== REFERENCE VIEW ============================ */
// Grouped, detailed command documentation + loadable sample programs.
const CMD_DOCS = [
 ["Core DuckyScript", [
  ["DELAY <ms>","Pause for ms milliseconds (max 60000)."],
  ["DEFAULTDELAY <ms>","Delay inserted after every command line. Set once at the top of the script."],
  ["STRING <text>","Types text exactly (5ms/char pacing)."],
  ["STRINGLN <text>","Types text then presses Enter."],
  ["GUI / CTRL / ALT / SHIFT / ALTGR combos","Hold modifiers and tap keys: `GUI r`, `CTRL-SHIFT ESC`. Single keys can be used alone: `ENTER`, `F5`."],
  ["Special keys","ENTER SPACE TAB ESC ESCAPE BACKSPACE DELETE DEL HOME END INSERT PAGEUP PAGEDOWN CAPSLOCK APP UP DOWN LEFT RIGHT (+ARROW variants)"],
  ["REPEAT <n>","Re-executes the previous command line n times."],
  ["REM <text> / REM_BLOCK_START ... REM_BLOCK_END","Comments."],
 ]],
 ["Logic & Detection", [
  ["DETECT_OS","Fingerprints the host OS via the keyboard-LED side-channel (~10 seconds; toggles your lock keys and restores them). Result is cached for IF_OS and shown on the Status page."],
  ["IF_OS <windows|linux|macos|ios|android|chromeos|unknown>","Runs block if detected OS matches. Requires DETECT_OS to have run first (otherwise compares against Unknown)."],
  ["IF_SSID <name>","True if a WiFi AP with that SSID is currently visible (scans ~2s)."],
  ["IF <expr>","Compare value commands: `IF GET_IP = 192.168.0.1`, `IF WIFI_CONNECTED != false`. Bare `IF DETECT_OS` is truthy when non-empty. Whole-token value commands (GET_IP, DETECT_OS, WIFI_CONNECTED, RANDOM_NUM min max, RANDOM_CHAR len) can also be embedded in STRING/STRINGLN/HUMAN_TYPE payloads and are replaced with their live values."],
  ["IF_WIFI","True if the device is connected to a network as client (after CONNECT_AP)."],
  ["ELSE_IF <value>","Alternative branch; inherits the parent condition type (OS vs SSID). Evaluated lazily."],
  ["ELSE","Fallback branch. All blocks end with END_IF; nesting is supported."],
 ]],
 ["Device Hardware", [
  ["LED_ON #RRGGBB","Light the status LED with a hex color, e.g. `LED_ON #00FF00`."],
  ["LED_OFF","Turn the LED off."],
  ["LED_BLINK <times> #RRGGBB","Blink n times (250ms on/off). Default: 5x red."],
  ["SCREEN_ON / SCREEN_OFF","Backlight on/off."],
  ["SCREEN_TEXT <text> [#RRGGBB]","Show text on the screen, optional color."],
  ["SCREEN_CLR","Clear the display."],
  ["WAIT_BUTTON [secs] [CONTINUE|STOP]","Wait for the BOOT button. On timeout either continue or stop the script. Defaults: 30 CONTINUE."],
 ]],
 ["Input & Randomness", [
  ["HUMAN_TYPE <text>","Types at ~40 wpm with random jitter - looks human, beats timing analysis."],
  ["RANDOM_NUM <min> <max>","Types a random number in range."],
  ["RANDOM_CHAR <len>","Types len random alphanumeric characters (good for fake passwords)."],
  ["JIGGLE_MOUSE <secs>","Move the mouse +/-1px every half second so the host never sleeps. Subtle by design."],
 ]],
 ["Network & System", [
  ["GET_IP","Types the device IP address (station IP if connected, else our own AP IP)."],
  ["CONNECT_AP <ssid> [password]","Join a WiFi network as client while keeping the config AP alive. Logs result."],
  ["RESET_FIRM","Factory-reset all settings and reboot. DESTRUCTIVE - use with care."],
  ["LOG <message>","Write a message to the encrypted device log (visible on Status page)."],
 ]],
];

const SAMPLES = [
 ["Hello Notepad", "Opens Notepad on Windows and types a message.",
`REM Basic Windows payload
DELAY 1000
GUI r
DELAY 500
STRING notepad
ENTER
DELAY 1000
STRING Hello from your T-Dongle-S3!
`],
 ["OS-Aware Greeting", "Detects the OS and opens the right run dialog.",
`DETECT_OS
IF_OS windows
  DELAY 1000
  GUI r
  STRING notepad
  ENTER
ELSE_IF macos
  DELAY 1000
  GUI SPACE
  STRING textedit
  ENTER
ELSE_IF linux
  ALT F2
  STRING gedit
  ENTER
ELSE
  LOG unknown host OS
END_IF
`],
 ["Stealth Check", "Waits for you to press BOOT before firing.",
`REM Wait up to 60s for button press; abort if nobody does
WAIT_BUTTON 60 STOP
DELAY 1000
LED_BLINK 3 #00FF00
GUI r
STRING notepad
ENTER
DELAY 800
STRING Button-triggered!
`],
 ["Human Typing Demo", "Random password + human-like typing.",
`DELAY 1000
STRING username: admin
ENTER
STRING password: 
RANDOM_CHAR 12
ENTER
DELAY 500
HUMAN_TYPE This sentence was typed like a human at about forty words per minute.
`],
 ["Network Report", "Joins WiFi and reports the device IP into Notepad.",
`DELAY 1000
GUI r
STRING cmd
ENTER
DELAY 1500
CONNECT_AP MyHomeNetwork MyPassword
IF_WIFI
  STRING Device IP:
  GET_IP
  ENTER
ELSE
  STRING Could not connect to WiFi
  ENTER
END_IF
`],
 ["Light Show", "Pure hardware demo - no host needed.",
`LED_BLINK 3 #FF0000
LED_ON #00FF00
DELAY 1000
LED_OFF
SCREEN_TEXT Dongle alive! #00FF00
DELAY 2000
SCREEN_CLR
SCREEN_OFF
`],
];

function refView(){
  let html = `<div class="panel"><h2>DuckyScript Command Reference</h2>
    <p class="muted">Custom commands marked in <b style="color:var(--accent)">bold groups</b> are extensions beyond standard DuckyScript v3.</p>`;
  for(const [group, cmds] of CMD_DOCS){
    html += `<h3 style="color:var(--accent);margin-bottom:4px">${group}</h3><table>`;
    for(const [cmd, desc] of cmds)
      html += `<tr><td style="width:40%"><b>${esc(cmd)}</b></td><td class="muted">${esc(desc)}</td></tr>`;
    html += `</table>`;
  }
  html += `</div><div class="panel"><h2>Sample Programs</h2>
    <p class="muted">Click Load to open a sample in the editor.</p><table id="samples">`;
  for(const [title,desc,code] of SAMPLES)
    html += `<tr><td><b>${esc(title)}</b><br><span class="muted" style="font-size:13px">${esc(desc)}</span></td>
      <td style="width:auto"><button class="small" onclick="loadSample('${esc(title)}')">Load</button></td></tr>`;
  html += `</table></div>`;
  view.innerHTML = html;
}
function loadSample(title){
  const s = SAMPLES.find(x=>x[0]===title); if(!s) return;
  location.hash = "#tools";
  // toolsView renders on hashchange; wait one tick then fill the editor
  setTimeout(()=>{
    CODE().value = s[2].replace(/\r\n?/g,"\n");
    $("#scriptName").value = title.toLowerCase().replace(/[^a-z0-9]+/g,"_")+".ds";
    syncGutter();
    toast("Sample loaded: "+title);
  }, 50);
}

/* ============================ CONTROL VIEW ============================= */
// On-screen keyboard (sticky modifiers) + mouse pad. Deltas stream at most
// every 60ms; buttons and scroll are click-based.
let ctrlTimer=null, heldMods=new Set();
const KEY_ROWS=[["`","1","2","3","4","5","6","7","8","9","0","-","=","BACKSPACE"],
 ["TAB","q","w","e","r","t","y","u","i","o","p","[","]","\\"],
 ["CAPSLOCK","a","s","d","f","g","h","j","k","l",";","'","ENTER"],
 ["SHIFT","z","x","c","v","b","n","m",",",".","/","UP"],
 ["CTRL","GUI","ALT","SPACE","ESC","LEFT","DOWN","RIGHT"]];
const MOD_NAMES=["CTRL","GUI","ALT","SHIFT","ALTGR"];

function controlView(){
  clearInterval(ctrlTimer);
  let kbHtml="";
  for(const row of KEY_ROWS){
    kbHtml+='<div class="kbrow">';
    for(const k of row){
      const wide=(k.length>1)?' style="min-width:'+(k==="SPACE"?120:52)+'px"':"";
      kbHtml+=`<button class="key small" data-key="${esc(k)}"${wide}>${k==="SPACE"?"":k}</button>`;
    }
    kbHtml+='</div>';
  }
  view.innerHTML=`<div class="panel"><h2>HID Control</h2>
   <div style="display:flex;gap:16px;flex-wrap:wrap">
    <div><h3>Keyboard</h3><p class="muted">Modifier buttons are sticky - click to hold, click again to release.</p>
     <div id="mods">${MOD_NAMES.map(m=>`<button class="key small mod" data-mod="${m}">${m}</button>`).join(" ")}</div>
     <div id="kbd" class="mt8">${kbHtml}</div></div>
    <div><h3>Mouse</h3>
     <div id="mousepad">move mouse here</div>
     <div style="display:flex;gap:8px;margin-top:8px;justify-content:center">
      <button class="small" data-btn="left">&#128073;</button>
      <button class="small" data-btn="middle">&bull;</button>
      <button class="small" data-btn="right">&#128072;</button>
     </div>
     <div style="text-align:center;margin-top:6px">
      <button class="small" onclick="mScroll(1)">&#9650;</button>
      scroll
      <button class="small" onclick="mScroll(-1)">&#9660;</button>
     </div></div>
    <div><h3>Host Lock Keys</h3><div id="lockKeys" class="muted">(no reports yet)</div>
     <p class="muted mt8" style="max-width:220px;font-size:13px">Updates when the host sends LED reports (toggle caps lock on the host once to see it live).</p></div>
   </div></div>`;

  // wire keys
  document.querySelectorAll("#kbd .key").forEach(b=>{
    b.onclick=()=>api("/api/hid/key",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({key:b.dataset.key,type:"tap"})});
  });
  document.querySelectorAll("#mods .mod").forEach(b=>{
    b.onclick=()=>{ toggleMod(b.dataset.mod); b.classList.toggle("held"); };
  });
  // mouse pad: pointer deltas
  const pad=$("#mousepad"); let last=null;
  pad.addEventListener("pointermove",e=>{
    e.preventDefault();
    if(last){ mMove(e.clientX-last.x, e.clientY-last.y); }
    last={x:e.clientX,y:e.clientY};
  });
  pad.addEventListener("pointerleave",()=>last=null);
  // mouse buttons: press/release on down/up
  document.querySelectorAll("[data-btn]").forEach(b=>{
    const btn=b.dataset.btn;
    b.addEventListener("pointerdown",()=>mButton(btn,true));
    b.addEventListener("pointerup",()=>mButton(btn,false));
  });
  refreshLocks(); ctrlTimer=setInterval(refreshLocks,2000);
}
function toggleMod(m){
  heldMods.has(m)?heldMods.delete(m):heldMods.add(m);
  api("/api/hid/mods",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({mods:[...heldMods]})});
}
let mQueue={dx:0,dy:0}, mT=null;
function mMove(dx,dy){
  mQueue.dx+=dx; mQueue.dy+=dy;
  if(!mT) mT=setTimeout(()=>{ mT=null;
    if(mQueue.dx||mQueue.dy)
      api("/api/hid/mouse",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({dx:mQueue.dx,dy:mQueue.dy})});
    mQueue={dx:0,dy:0};
  },60);
}
function mButton(b,down){
  api("/api/hid/mouse",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({button:b,down})});
}
function mScroll(n){
  api("/api/hid/mouse",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({scroll:n})});
}
async function refreshLocks(){
  const el=$("#lockKeys"); if(!el)return;
  try{ const s=await api("/api/status");
    el.textContent=s.lockKeys?s.lockKeys+" active":"(none reported)";
  }catch(e){}
}

/* ============================== ROUTER ================================ */
function route(){
  clearInterval(statusTimer);
  const h=location.hash||"#tools";
  document.querySelectorAll("nav a").forEach(a=>a.classList.toggle("active",a.getAttribute("href")===h));
  if(h==="#files")filesView();
  else if(h==="#control")controlView();
  else if(h==="#reference")refView();
  else if(h==="#status")statusView();
  else if(h==="#settings")settingsView();
  else toolsView();
}
window.onhashchange=route;
$("#logout").onclick=()=>fetch("/api/login",{method:"POST"}).then(()=>location.href="/login.html");
route();
