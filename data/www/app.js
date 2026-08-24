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
  ${refPanel()}`;
  // Normalise CRLF -> LF and wire up editor events once per render.
  const code = CODE();
  code.value = code.value.replace(/\r\n?/g, "\n");
  code.addEventListener("input", () => { highlight(); syncScroll(); });
  code.addEventListener("scroll", syncScroll);       // keep overlay + gutter in step
  highlight(); syncScroll();
  refreshScripts();
}

/* -------- editor helpers -------- */
// NOTE(cursor): #hl and #code share identical font metrics + padding + line-height
// (see style.css). The textarea scrolls; #hl/#gutter follow via syncScroll.
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
                                      : esc(rest));
      } else out += esc(ln);
    }
    out += "\n";
  });
  $("#hl").innerHTML = out;
  $("#gutter").textContent = g;
}
function syncScroll(){
  const hl=$("#hl"), gt=$("#gutter"), c=CODE();
  if(!hl||!c)return;
  hl.scrollTop=c.scrollTop; hl.scrollLeft=c.scrollLeft;
  if(gt) gt.scrollTop=c.scrollTop;
}
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
  $("#scriptName").value=n; highlight(); syncScroll();
  toast("Loaded "+n);
}
function newScript(){ CODE().value=""; $("#scriptName").value=""; highlight(); toast("New script"); }
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
function upload(){ const f=$("#upFile").files[0]; if(!f)return;
  fetch("/api/upload?path="+encodeURIComponent(fbCur+"/"+f.name),{method:"POST",body:f})
   .then(r=>{ if(r.ok){toast("Uploaded "+f.name); fbGo(fbCur);} else toast("Upload failed ("+r.status+")","err"); })
   .catch(()=>toast("Upload failed","err"));
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
   <tr><td>Connection</td><td>${s.usbHost?"Plugged into computer":"Power only"}</td></tr>
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
    <label>Backlight brightness <input type="number" id="brightness" min="0" max="255" value="128" style="max-width:120px"></label></div>
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
    if(s.permOff) toast("Interface is PERMANENTLY disabled (takes effect on reboot)","err");
  }catch(e){ /* leave defaults */ }
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
  b.tempOff=$("#tempOff").checked;
  await jpost("/api/settings", b);
  toast("Settings saved. Some changes apply after reboot.");
}
async function permDisable(){
  if(!(await confirmModal("PERMANENTLY disable web interface?\nOnly a firmware re-flash can undo this!")))return;
  await jpost("/api/settings",{permOff:true});
  toast("Interface will stay off after next reboot.","err");
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
  if(h==="#files")filesView();
  else if(h==="#status")statusView();
  else if(h==="#settings")settingsView();
  else toolsView();
}
window.onhashchange=route;
$("#logout").onclick=()=>fetch("/api/login",{method:"POST"}).then(()=>location.href="/login.html");
route();
