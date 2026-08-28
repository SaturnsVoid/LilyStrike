// ============================================================================
// LilyStrike UI - SPA (Tools / Files / Live Control / EvilAP / Reference /
// Status / Settings). Dark+light themes, IDE-style editor, expandable docs.
// All popups are in-page; text normalised to LF everywhere.
// ============================================================================
"use strict";
const $ = s => document.querySelector(s);
const view = $("#view");

/* ------------------------------ API helpers ------------------------------ */
async function api(path, opts = {}) {
  const r = await fetch(path, opts);
  if (r.status === 401) { location.href = "/login.html"; throw new Error("auth"); }
  const t = await r.text();
  try { return JSON.parse(t); } catch { return t; }
}
const jpost = (p, body) => api(p, {method:"POST", headers:{"Content-Type":"application/json"}, body: JSON.stringify(body)});
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

/* --------------------------------- icons --------------------------------- */
const ICONS = {
  tools:'<path d="M13 2 4.5 13.5H11L9.5 22 19.5 9.5H12.5L13 2Z" fill="currentColor"/>',
  folder:'<path d="M3 6a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V6Z" fill="currentColor"/>',
  file:'<path d="M6 2h8l4 4v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2Z" fill="currentColor"/>',
  keyboard:'<rect x="2" y="6" width="20" height="12" rx="2" fill="currentColor"/><rect x="5" y="9" width="2" height="2" fill="#fff"/><rect x="9" y="9" width="2" height="2" fill="#fff"/><rect x="13" y="9" width="2" height="2" fill="#fff"/><rect x="17" y="9" width="2" height="2" fill="#fff"/><rect x="7" y="13" width="10" height="2" fill="#fff"/>',
  wifi:'<path d="M12 20a2 2 0 1 0 0-4 2 2 0 0 0 0 4Zm-4.9-6.1a7 7 0 0 1 9.8 0l-1.8 1.8a4.5 4.5 0 0 0-6.2 0l-1.8-1.8ZM3.7 10.4a12 12 0 0 1 16.6 0l-1.8 1.8a9.5 9.5 0 0 0-13 0l-1.8-1.8Z" fill="currentColor"/>',
  book:'<path d="M4 4a2 2 0 0 1 2-2h13v18H6a2 2 0 0 0-2 2V4Z" fill="currentColor"/><path d="M6 17h13v2H6z" fill="#fff" opacity=".4"/>',
  gauge:'<path d="M12 4a8 8 0 0 1 8 8h-3a5 5 0 0 0-10 0H4a8 8 0 0 1 8-8Zm1.4 6.6 3-3 1.4 1.4-3 3a2 2 0 1 1-1.4-1.4Z" fill="currentColor"/>',
  gear:'<path d="M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8Zm9 4-2 1 .3 2.1-1.8 1.8-2.1-.3-1 1.8-2.1.4-1.3 1.7-2.6.1L9.4 20l-2.1-.4-1-1.8-2.1.3-1.8-1.8.3-2.1-2-1v-2.4l2-1-.3-2.1L4.2 5.9l2.1.3 1-1.8 2.1-.4L10.7 2.3l2.6-.1 1.3 1.7 2.1-.4 1 1.8 2.1-.3 1.8 1.8-.3 2.1 2 1V12Z" fill="currentColor"/>',
  bolt:'<path d="M13 2 4.5 13.5H11L9.5 22 19.5 9.5H12.5L13 2Z" fill="currentColor"/>',
  os:'<rect x="3" y="4" width="18" height="12" rx="2" fill="currentColor"/><path d="M8 20h8l-1-3H9l-1 3Z" fill="currentColor"/>',
  script:'<path d="M6 2h8l4 4v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2Z" fill="currentColor"/><text x="8" y="17" font-size="9" fill="#fff" font-family="monospace">ds</text>',
};
const icon = (n,s=16) => `<svg width="${s}" height="${s}" viewBox="0 0 24 24" fill="none">${ICONS[n]||ICONS.file}</svg>`;

/* ----------------------------- toast + modals ----------------------------- */
function toast(msg, kind="ok") {
  const el = document.createElement("div");
  el.className = "toast " + kind; el.textContent = msg;
  $("#toastHost").appendChild(el);
  setTimeout(() => el.classList.add("show"), 10);
  setTimeout(() => { el.classList.remove("show"); setTimeout(()=>el.remove(), 400); }, 3000);
}
function modal(html) {
  return new Promise(resolve => {
    $("#modalHost").innerHTML = `<div class="overlay"><div class="modal">${html}</div></div>`;
    window._closeModal = v => { $("#modalHost").innerHTML = ""; resolve(v); };
  });
}
const confirmModal = msg => modal(`<h3>Confirm</h3><p>${esc(msg)}</p>
  <div class="row-end"><button class="danger" onclick="_closeModal(true)">Confirm</button>
  <button onclick="_closeModal(false)">Cancel</button></div>`);
const promptModal = (title, value="") => modal(`<label>${esc(title)}
  <input id="mInput" value="${esc(value)}"></label>
  <div class="row-end"><button class="primary" onclick="_closeModal($('#mInput').value)">Save</button>
  <button onclick="_closeModal(null)">Cancel</button></div>`);

/* --------------------------------- theme --------------------------------- */
function initTheme() {
  const saved = localStorage.getItem("ls-theme") || "dark";
  document.documentElement.dataset.theme = saved;
  $("#themeBtn").innerHTML = saved==="dark"
    ? '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="5"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4" stroke="currentColor" stroke-width="2"/></svg>'
    : '<svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8Z"/></svg>';
}
$("#themeBtn").onclick = () => {
  const cur = document.documentElement.dataset.theme;
  localStorage.setItem("ls-theme", cur==="dark"?"light":"dark");
  initTheme();
};

/* --------------------------------- EULA --------------------------------- */
const EULA_TEXT =
`LILYSTRIKE END USER LICENSE & RESPONSIBILITY AGREEMENT

This device runs penetration-testing firmware intended SOLELY for
authorized security assessments on systems you own or have explicit
written permission to test.

By continuing you acknowledge and agree that:

1. AUTHORIZED USE ONLY. You will only use this device on networks,
   computers and systems that you own or have express, prior
   authorization to test (e.g. signed scope authorization, lab
   equipment, CTF environments).

2. NO WARRANTY. This firmware is provided "as is" without warranty
   of any kind. The authors and contributors accept NO LIABILITY
   for any damage, data loss, or legal consequences arising from
   its use or misuse.

3. YOUR RESPONSIBILITY. Unauthorized access to computer systems is
   a crime in most jurisdictions (e.g. CFAA, Computer Misuse Act).
   You are solely responsible for complying with all applicable
   local, state and federal laws.

4. NO ILLICIT USE. The device will not be used to access, damage,
   disrupt or exfiltrate data from systems without authorization.

If you do not agree, discontinue use of this device immediately.
`;

async function checkEula() {
  try {
    const e = await api("/api/eula");
    if (e.agreed) return;
    // NOTE: no scroll-gating - ticking the checkbox IS the agreement.
    // Scroll detection proved unreliable across mobile browsers.
    // IMPORTANT: modal() returns a Promise that resolves only when the modal
    // CLOSES - so handlers must be wired immediately after the call, NOT
    // after awaiting it (awaiting here deadlocked the whole dialog).
    modal(`<h3>Before you begin</h3>
      <div class="eula-text" id="eulaBox">${esc(EULA_TEXT)}</div>
      <label style="margin-top:12px"><input type="checkbox" id="eulaChk">
        I have read and agree to the terms above</label>
      <div class="row-end"><button class="primary" id="eulaBtn" disabled
        onclick="window._eulaGo()">Agree &amp; Continue</button></div>`);
    const chk = $("#eulaChk"), btn = $("#eulaBtn");
    chk.onchange = () => btn.disabled = !chk.checked;
    window._eulaGo = async () => {
      await jpost("/api/eula", {agreed:true});
      $("#modalHost").innerHTML = "";
      toast("Welcome to LilyStrike");
    };
  } catch(e) {}
}

/* ============================== TOOLS VIEW ============================== */
let curScript = "";
function toolsView() {
  view.innerHTML = `
  <div class="panel">
    <h2>${icon("bolt")} BadUSB &mdash; Script Studio</h2>
    <div style="display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap">
      <button class="small" onclick="saveScript()">${icon("file",13)} Save</button>
      <button class="small danger" onclick="delScript()">Delete</button>
      <span style="flex:1"></span>
      <button class="small ok primary" id="runBtn" onclick="runScript()">&#9654; Run</button>
      <button class="small danger" id="stopBtn" onclick="api('/api/stop',{method:'POST'})"
        style="display:none">&#9632; Stop</button>
    </div>
    <div class="ide">
      <div class="script-list" id="scriptList">
        <div class="sl-head">Scripts <button class="small" onclick="newScript()">+</button></div>
      </div>
      <div style="flex:1;min-width:0">
        <div class="editor-wrap">
          <pre id="gutter">1</pre>
          <div class="editor-stack">
            <pre id="hl"></pre>
            <textarea id="code" spellcheck="false"
              placeholder="Type DuckyScript here..."></textarea>
          </div>
        </div>
        <div style="display:flex;gap:8px;margin-top:10px;flex-wrap:wrap">
          <input id="scriptName" placeholder="filename.ds" style="max-width:200px">
          <input id="scriptDesc" maxlength="60" placeholder="Short description (shown in sidebar)" style="flex:1;min-width:180px">
          <select id="scriptLayout" style="max-width:130px" title="Keyboard layout for this script"></select>
        </div>
      </div>
    </div>
  </div>

  <div class="panel"><h2>${icon("bolt")} Autostart Queue <span class="muted" style="font-size:12px">(max 5, runs in order on plug-in)</span></h2>
    <div id="autoList"></div>
    <div style="display:flex;gap:8px;margin-top:8px">
      <select id="autoPick" style="width:auto"></select>
      <button class="small" onclick="addAuto()">Add</button>
      <button class="small danger" onclick="clearAuto()">Clear all</button>
    </div>
  </div>`;
  const code = CODE();
  code.value = code.value.replace(/\r\n?/g, "\n");
  code.addEventListener("input", () => { highlight(); syncScroll(); });
  code.addEventListener("scroll", syncScroll);
  highlight(); syncScroll();
  loadLayouts();
  refreshScriptFiles();
}

/* -------- IDE editor -------- */
const CODE = () => $("#code");
const FLOW = /^(IF|ELSE|ELSE_IF|END_IF|WHILE|REPEAT)\b/i;
const CMD = /^(REM|REM_BLOCK_START|REM_BLOCK_END|DEFAULTDELAY|DEFAULT_DELAY|DELAY|STRING|STRINGLN|ENTER|SPACE|TAB|ESCAPE|DOWNARROW|UPARROW|LEFTARROW|RIGHTARROW|BACKSPACE|DELETE|HOME|INSERT|PAGEUP|PAGEDOWN|CAPSLOCK|APP|GUI|WINDOWS|COMMAND|CTRL|CONTROL|ALT|ALTGR|SHIFT|F\d{1,2})\b/i;
const CUSTOM = /^(DETECT_OS|LED_ON|LED_OFF|LED_BLINK|SCREEN_ON|SCREEN_OFF|SCREEN_CLR|SCREEN_TEXT|SCREEN_IMG|RANDOM_NUM|RANDOM_CHAR|HUMAN_TYPE|SSID_TRIGGER|CONNECT_AP|WIFI_CONNECTED|GET_IP|WAIT_BUTTON|JIGGLE_MOUSE|SSID_SPAM|RESET_FIRM|USB_STORAGE|SELF_DESTRUCT|LOG)\b/i;
function highlight() {
  const lines = CODE().value.split("\n");
  let out = "", g = "";
  lines.forEach((ln, i) => {
    g += (i+1) + "\n";
    if (/^\s*(REM|#)/i.test(ln)) { out += `<span class="tok-rem">${esc(ln)}</span>`; }
    else {
      let m = ln.match(FLOW);
      if (m) out += `<span class="tok-flow">${esc(m[0])}</span>` + esc(ln.slice(m[0].length));
      else if ((m = ln.match(CUSTOM))) out += `<span class="tok-custom">${esc(m[0])}</span>` + esc(ln.slice(m[0].length));
      else if ((m = ln.match(CMD))) {
        const rest = ln.slice(m[0].length);
        out += `<span class="tok-cmd">${esc(m[0])}</span>` +
               (/^STRING/i.test(m[0]) ? `<span class="tok-str">${esc(rest)}</span>` : esc(rest));
      } else out += esc(ln).replace(/\b(\d+)\b/g, '<span class="tok-num">$1</span>');
    }
    out += "\n";
  });
  $("#hl").innerHTML = out + "\n";
  $("#gutter").textContent = g;
}
function syncScroll(){
  const hl=$("#hl"), gt=$("#gutter"), c=CODE();
  if(!hl||!c) return;
  hl.scrollTop=c.scrollTop; hl.scrollLeft=c.scrollLeft;
  if(gt) gt.scrollTop=c.scrollTop;
}
/* poll run state for Run/Stop buttons */
setInterval(async()=>{
  const rb=$("#runBtn"), sb=$("#stopBtn"); if(!rb||!sb)return;
  try{ const s=await api("/api/status");
    const running=(s.scriptState==="RUNNING");
    rb.style.display=running?"none":""; sb.style.display=running?"":"none";
  }catch(e){}
},2000);

/* -------- script file list (sidebar) -------- */
let _scriptsLoaded=false;
async function refreshScriptFiles(retry=true){
  try{
    const list = await api("/api/scripts");
    const box = $("#scriptList");
    if(!box) return;
    box.innerHTML = `<div class="sl-head">Scripts <button class="small" onclick="newScript()">+</button></div>` +
      list.map(s=>`<button class="sfile ${s.name===curScript?"active":""}"
        title="${esc(s.desc||"")}"
        onclick="loadScript('${esc(s.name)}')">${icon("script",13)}
        <span>${esc(s.name)}${s.desc?`<br><small style="color:var(--muted);font-weight:400">${esc(s.desc)}</small>`:""}</span></button>`).join("")
      || `<div class="sl-head" style="font-weight:400">No scripts yet</div>`;
    const pick=$("#autoPick"); if(pick) pick.innerHTML = list.map(s=>`<option>${esc(s.name)}</option>`).join("");
    _scriptsLoaded=true;
    renderAuto();
  }catch(e){
    if(retry) setTimeout(()=>refreshScriptFiles(false), 800);   // one retry (SD can be slow at boot)
  }
}
async function loadScript(n){
  if (typeof n !== "string") return;
  const t = await api("/api/script?name="+encodeURIComponent(n));
  CODE().value = String(t).replace(/\r\n?/g,"\n");
  curScript = n; $("#scriptName").value = n;
  // load description + layout from the meta sidecar
  api("/api/scriptmeta?name="+encodeURIComponent(n)).then(m=>{
    $("#scriptDesc").value = m.desc||"";
    $("#scriptLayout").value = m.layout||"en_US";
  }).catch(()=>{ $("#scriptDesc").value=""; $("#scriptLayout").value="en_US"; });
  highlight(); syncScroll(); refreshScriptFiles();
  toast("Loaded "+n);
}
function newScript(){
  CODE().value=""; curScript=""; $("#scriptName").value=""; $("#scriptDesc").value="";
  $("#scriptLayout").value="en_US";
  highlight(); syncScroll(); refreshScriptFiles(); toast("New script");
}
let _layoutsLoaded=false;
function loadLayouts(){
  if(_layoutsLoaded) return; _layoutsLoaded=true;
  api("/api/layouts").then(l=>{
    $("#scriptLayout").innerHTML = l.map(x=>`<option>${esc(x)}</option>`).join("");
    $("#scriptLayout").value="en_US";
  }).catch(()=>{});
}
async function saveScript(){
  let n=$("#scriptName").value.trim();
  if(!n) return toast("Enter a filename first","err");
  if(!n.endsWith(".ds")) n += ".ds";
  $("#scriptName").value = n;
  await jpost("/api/script",{name:n,text:CODE().value});
  // description + layout saved to an encrypted sidecar next to the script
  await jpost("/api/scriptmeta",{name:n,desc:$("#scriptDesc").value,layout:$("#scriptLayout").value});
  curScript=n; refreshScriptFiles(); toast("Saved "+n);
}
async function delScript(){
  const n = curScript || $("#scriptName").value.trim();
  if(!n || !(await confirmModal("Delete "+n+"?"))) return;
  await api("/api/script?name="+encodeURIComponent(n),{method:"DELETE"});
  newScript(); toast("Deleted "+n);
}
async function runScript(){
  const text=CODE().value.replace(/\r\n?/g,"\n");
  await jpost("/api/run", text.trim()?{text}: {name:curScript});
  toast("Script started");
}

/* -------- autostart queue (max 5, reorderable) -------- */
let autoQ=[];
async function renderAuto(){
  autoQ = await api("/api/autostart");
  const box = $("#autoList"); if(!box) return;
  box.innerHTML = autoQ.map((n,i)=>`
    <div class="auto-item">
      <span class="ord">${i+1}</span>
      ${icon("script",13)}
      <span>${esc(n)}</span>
      <button class="small" title="Up" onclick="moveAuto(${i},-1)" ${i===0?"disabled":""}>&#9650;</button>
      <button class="small" title="Down" onclick="moveAuto(${i},1)" ${i===autoQ.length-1?"disabled":""}>&#9660;</button>
      <button class="small danger" title="Remove" onclick="rmAuto(${i})">&#10005;</button>
    </div>`).join("") || `<p class="muted" style="font-size:13px">No autostart scripts. Add one below.</p>`;
}
async function autoSet(names){
  const r = await jpost("/api/autostart",{names});
  if(r && r.ok===false) toast("Save failed: "+(r.error||"SD write error"),"err");
}
async function moveAuto(i,d){
  const j=i+d;
  [autoQ[i],autoQ[j]]=[autoQ[j],autoQ[i]];
  await autoSet(autoQ); renderAuto();
}
async function addAuto(){
  if(autoQ.length>=5) return toast("Autostart queue is full (max 5)","err");
  const n=$("#autoPick").value; if(!n)return;
  autoQ.push(n); await autoSet(autoQ); renderAuto(); toast("Added to autostart");
}
async function rmAuto(i){ autoQ.splice(i,1); await autoSet(autoQ); renderAuto(); }
async function clearAuto(){ await autoSet([]); renderAuto(); toast("Autostart cleared"); }

/* ============================== FILES VIEW ============================== */
let fbCur="/";
function fileIcon(name,isDir){
  if(isDir) return icon("folder");
  if(name.endsWith(".ds")) return icon("script");
  if(/\.(png|jpg|bmp|gif)$/i.test(name)) return icon("gauge");
  return icon("file");
}
function filesView(){
  view.innerHTML=`<div class="panel"><h2>${icon("folder")} File Manager</h2>
    <div class="crumbs" id="crumbs"></div>
    <div style="display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap">
      <button class="small" onclick="fbUp()">&#8592; Up</button>
      <span style="flex:1"></span>
      <input type="file" id="upFile" style="display:none" onchange="upload()">
      <button class="small" onclick="$('#upFile').click()">${icon("file",12)} Upload</button>
      <button class="small" onclick="fbNew()">New file</button>
      <button class="small" onclick="fbNewDir()">New folder</button>
    </div>
    <table id="fbTable"></table></div>`;
  fbGo("/");
}
function fbCrumbs(p){
  const parts = p.split("/").filter(x=>x);
  let html = `<a href="#" onclick="fbGo('/');return false">${icon("folder",14)} root</a>`;
  let acc="";
  for(const part of parts){ acc+="/"+part;
    html += ` <span class="sep">/</span> <a href="#" onclick="fbGo('${esc(acc)}');return false">${esc(part)}</a>`; }
  $("#crumbs").innerHTML = html;
}
async function fbGo(p){
  fbCur = (p==="/"||!p.endsWith("/"))?p:p.slice(0,-1);
  $("#fbPath") ; fbCrumbs(fbCur);
  try{
    const items=await api("/api/files?path="+encodeURIComponent(fbCur));
    $("#fbTable").innerHTML="<tr><th>Name</th><th>Size</th><th style='width:40%'>Actions</th></tr>"+
     items.map(f=>{
       const fp=(fbCur==="/")?"/"+f.name:fbCur+"/"+f.name;
       const cls=f.dir?"f-dir":f.name.endsWith(".ds")?"f-ds":/\.(zip|img|bin)$/i.test(f.name)?"f-img":"f-file";
       const act=f.dir
         ?`<a href="#" onclick="fbGo('${esc(fp)}');return false">Open</a>`
         :`${f.name.endsWith(".ds")?`<a href="#" onclick="runSd('${esc(fp)}');return false">Run</a> · `:""}
           <a href="#" onclick="editSd('${esc(fp)}');return false">Edit</a> ·
           <a href="#" onclick="delFb('${esc(fp)}');return false" class="err">Delete</a>`;
       return `<tr class="frow ${cls}"><td>${fileIcon(f.name,f.dir)}${esc(f.name)}</td>
         <td class="muted">${f.dir?"—":f.size+" B"}</td><td class="f-actions">${act}</td></tr>`;
     }).join("") || `<tr><td colspan="3" class="muted">Empty folder</td></tr>`;
  }catch(e){ toast("Cannot open "+p,"err"); }
}
function fbUp(){ fbGo(fbCur.replace(/\/[^/]*$/,"")||"/"); }
async function delFb(p){ if(await confirmModal("Delete "+p+"?")){await api("/api/file?path="+encodeURIComponent(p),{method:"DELETE"});fbGo(fbCur);} }
async function runSd(p){ await jpost("/api/run",{name:p.split("/").pop()}); toast("Running "+p); }
async function upload(){
  const f=$("#upFile").files[0]; if(!f)return;
  try{
    const b64=btoa(await f.text());
    const r=await fetch("/api/filebin?path="+encodeURIComponent(fbCur+"/"+f.name),
      {method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({b64})});
    if(r.ok){toast("Uploaded "+f.name); fbGo(fbCur);}
    else toast("Upload failed ("+r.status+")","err");
  }catch(e){ toast("Upload failed","err"); }
}
async function editSd(p){
  const t = await fetch("/api/file?path="+encodeURIComponent(p)).then(r=>r.ok?r.text():null).catch(()=>null);
  if(t===null) return toast("Could not read "+p,"err");
  // .ds files open in the Script Studio instead
  if(p.endsWith(".ds")){
    location.hash="#tools";
    setTimeout(()=>{ CODE().value=String(t).replace(/\r\n?/g,"\n");
      curScript=p.split("/").pop(); $("#scriptName").value=curScript;
      highlight(); syncScroll(); },60);
    return;
  }
  await modal(`<label>Edit ${esc(p)}
    <textarea id="mEdit" rows="14" class="mono">${esc(String(t).replace(/\r\n?/g,"\n"))}</textarea></label>
    <div class="row-end"><button class="primary" onclick="_closeModal($('#mEdit').value)">Save</button>
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

/* =========================== LIVE CONTROL VIEW =========================== */
let ctrlTimer=null, heldMods=new Set();
const KEY_ROWS=[["`","1","2","3","4","5","6","7","8","9","0","-","=","BACKSPACE"],
 ["TAB","q","w","e","r","t","y","u","i","o","p","[","]","\\"],
 ["CAPSLOCK","a","s","d","f","g","h","j","k","l",";","'","ENTER"],
 ["SHIFT","z","x","c","v","b","n","m",",",".","/","UP"],
 ["CTRL","GUI","ALT","SPACE","ESC","LEFT","DOWN","RIGHT"]];
const MOD_NAMES=["CTRL","GUI","ALT","SHIFT","ALTGR"];
let mouseMode="pad";

function controlView(){
  clearInterval(ctrlTimer);
  let kbHtml="";
  for(const row of KEY_ROWS){
    kbHtml+='<div class="kbrow">';
    for(const k of row){
      const wide=(k.length>1)?` style="min-width:${k==="SPACE"?110:48}px;font-size:10px"`:"";
      kbHtml+=`<button class="key" data-key="${esc(k)}"${wide}>${k==="SPACE"?"":k}</button>`;
    }
    kbHtml+='</div>';
  }
  view.innerHTML=`<div class="panel"><h2>${icon("keyboard")} Live Control</h2>
   <p class="muted">Control the host computer directly. Modifier buttons are sticky.</p>
   <div class="live-wrap">
    <div class="live-sec" style="flex:1.4;min-width:0">
      <h3 style="font-size:13px;color:var(--muted)">KEYBOARD</h3>
      <div style="display:flex;gap:4px;justify-content:center;margin-bottom:6px" id="mods">
        ${MOD_NAMES.map(m=>`<button class="key mod" data-mod="${m}" style="min-width:52px">${m}</button>`).join("")}
      </div>
      <div id="kbd">${kbHtml}</div>
    </div>
    <div class="live-sec" style="flex:1;min-width:250px">
      <h3 style="font-size:13px;color:var(--muted)">HOST LOCK KEYS</h3>
      <div class="lockbox" id="lockKeys" style="margin-bottom:14px">(no reports yet)</div>
      <h3 style="font-size:13px;color:var(--muted)">POINTER</h3>
      <div style="text-align:center;margin-bottom:10px">
        <button class="small" id="modePad" onclick="setMouseMode('pad')">Touchpad</button>
        <button class="small" id="modeBall" onclick="setMouseMode('ball')">Trackball</button>
      </div>
      <div class="touchpad" id="pad" style="${mouseMode==="pad"?"":"display:none"}">
        <div class="padlabel">slide to move &middot; tap corners: bottom-left = left click, bottom-right = right click</div>
      </div>
      <div class="trackball" id="ball" style="${mouseMode==="ball"?"":"display:none"}">
        <div class="knob" id="knob"></div>
      </div>
      <div style="display:flex;gap:8px;justify-content:center;margin-top:12px">
        <button class="key" data-btn="left" style="min-width:64px">Left</button>
        <button class="key" data-btn="middle" style="min-width:56px">Mid</button>
        <button class="key" data-btn="right" style="min-width:56px">Right</button>
      </div>
      <div style="text-align:center;margin-top:10px">
        <button class="key" onclick="mScroll(1)">&#9650;</button>
        <span class="muted" style="margin:0 8px">scroll</span>
        <button class="key" onclick="mScroll(-1)">&#9660;</button>
      </div>
    </div>
   </div></div>`;
  document.querySelectorAll("#kbd .key").forEach(b=>{
    b.onclick=()=>api("/api/hid/key",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({key:b.dataset.key,type:"tap"})});
  });
  document.querySelectorAll("#mods .mod").forEach(b=>{
    b.onclick=()=>{ toggleMod(b.dataset.mod); b.classList.toggle("held"); };
  });
  setupPad();
  setupBall();
  document.querySelectorAll("[data-btn]").forEach(b=>{
    b.addEventListener("pointerdown",()=>mButton(b.dataset.btn,true));
    b.addEventListener("pointerup",()=>mButton(b.dataset.btn,false));
  });
  refreshLocks(); ctrlTimer=setInterval(refreshLocks,2000);
}
function setMouseMode(m){
  mouseMode=m;
  $("#pad").style.display = m==="pad"?"":"none";
  $("#ball").style.display = m==="ball"?"":"none";
  $("#modePad").style.fontWeight = m==="pad"?"700":"400";
  $("#modeBall").style.fontWeight = m==="ball"?"700":"400";
}
function toggleMod(m){
  heldMods.has(m)?heldMods.delete(m):heldMods.add(m);
  api("/api/hid/mods",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({mods:[...heldMods]})});
}
/* touchpad: delta streaming */
let mQueue={dx:0,dy:0}, mT=null;
function mFlush(){
  mT=null;
  if(mQueue.dx||mQueue.dy)
    api("/api/hid/mouse",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({dx:mQueue.dx,dy:mQueue.dy})});
  mQueue={dx:0,dy:0};
}
function mMove(dx,dy){
  mQueue.dx+=dx; mQueue.dy+=dy;
  if(!mT) mT=setTimeout(mFlush,60);
}
function mButton(b,down){
  api("/api/hid/mouse",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({button:b,down})});
}
function mScroll(n){
  api("/api/hid/mouse",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({scroll:n})});
}
function setupPad(){
  const pad=$("#pad"); let last=null;
  pad.addEventListener("pointermove",e=>{
    e.preventDefault();
    if(last) mMove(e.clientX-last.x, e.clientY-last.y);
    last={x:e.clientX,y:e.clientY};
  });
  pad.addEventListener("pointerleave",()=>last=null);
  pad.addEventListener("pointerdown",e=>{
    const r=pad.getBoundingClientRect();
    // corner tap zones = mouse clicks (laptop touchpad style)
    if(e.clientY > r.bottom-36){
      if(e.clientX < r.left+r.width*0.45) mButton("left",true),setTimeout(()=>mButton("left",false),60);
      else if(e.clientX > r.right-r.width*0.45) mButton("right",true),setTimeout(()=>mButton("right",false),60);
    }
  });
}
/* trackball/joystick: knob follows finger; offset from centre = velocity.
   BUGFIX: the old version read dx/dy ONLY at pointerdown and had no
   pointermove handler, so the knob never moved and velocity never changed. */
let ballInt=null;
function setupBall(){
  const ball=$("#ball"), knob=$("#knob");
  let vel={x:0,y:0};
  const setKnob=(x,y)=>{ knob.style.left=`calc(50% + ${x}px)`; knob.style.top=`calc(50% + ${y}px)`; };
  const clamp=v=>Math.max(-60,Math.min(60,v));
  const stop=()=>{
    vel={x:0,y:0}; setKnob(0,0);
    if(ballInt){clearInterval(ballInt);ballInt=null;}
  };
  ball.addEventListener("pointerdown",e=>{
    e.preventDefault();
    ball.setPointerCapture(e.pointerId);
    if(!ballInt) ballInt=setInterval(()=>{
      if(vel.x||vel.y) mMove(vel.x,vel.y);
    },50);
  });
  ball.addEventListener("pointermove",e=>{
    e.preventDefault();
    if(!ballInt) return;                       // only while dragging
    const r=ball.getBoundingClientRect();
    const dx=clamp(e.clientX-r.left-r.width/2);
    const dy=clamp(e.clientY-r.top-r.height/2);
    vel={x:Math.round(dx/8), y:Math.round(dy/8)};
    setKnob(dx,dy);
  });
  ball.addEventListener("pointerup",stop);
  ball.addEventListener("pointercancel",stop);
}
async function refreshLocks(){
  const el=$("#lockKeys"); if(!el)return;
  try{ const s=await api("/api/status");
    const lk=s.lockKeys||"";
    el.innerHTML=["CAPS","NUM","SCROLL"].map(k=>
      `<span class="led ${lk.includes(k)?"on":""}"></span>${k}`).join(" &nbsp; ");
  }catch(e){}
}

/* ============================= EVILAP VIEW ============================= */
let evilTimer=null, TPLS=["Generic WiFi","Apple","Google"];
function evilapView(){
  clearInterval(evilTimer);
  view.innerHTML=`<div class="panel"><h2>${icon("wifi")} EvilAP / Captive Portal</h2>
   <p class="err">Starting replaces your management AP until stopped. Stop via this page, http://portal-ip/disable, or holding BOOT ~1.5s.</p>
   <label>Portal SSID<input id="evilSsid" placeholder="Free WiFi"></label>
   <label>Template<select id="evilTpl">${TPLS.map(t=>`<option>${t}</option>`).join("")}</select></label>
   <div style="display:flex;gap:8px">
     <button class="danger" id="evilStartBtn" onclick="evilStart()">Start EvilAP</button>
     <button class="small" id="evilStopBtn" style="display:none" onclick="api('/api/evilap/stop',{method:'POST'})">Stop</button>
   </div></div>
  <div class="panel"><h2>Capture Statistics</h2><table>
   <tr><td>Portal hits</td><td id="eHits">-</td></tr>
   <tr><td>Credentials captured</td><td id="eCaps">-</td></tr></table>
   <div style="display:flex;gap:8px;margin-top:10px">
     <button class="small primary" onclick="showCreds()">View captured logins</button>
     <button class="small danger" onclick="clearCreds()">Clear captured logins</button>
   </div>
   <pre id="credsBox" style="display:none;margin-top:10px;background:var(--code-bg);padding:10px;border-radius:8px;max-height:240px;overflow:auto" class="mono"></pre></div>
  <div class="panel"><h2>Custom Portal Page</h2>
   <p class="muted">Stored encrypted as /portal.html.enc on the SD card. Overrides any template.</p>
   <textarea id="evilHtml" rows="11" class="mono" placeholder="<html>...custom login page..."></textarea>
   <div class="row-end">
     <button class="small" onclick="loadEvilHtml()">Load current</button>
     <button class="small primary" onclick="saveEvilHtml()">Save page</button>
     <button class="small danger" onclick="clearEvilHtml()">Remove custom page</button>
   </div></div>`;
  refreshEvil(); evilTimer=setInterval(refreshEvil,3000);
}
async function refreshEvil(){
  try{ const s=await api("/api/evilap/status");
    $("#eHits").textContent=s.hits; $("#eCaps").textContent=s.captures;
    $("#evilStartBtn").style.display=s.running?"none":"";
    $("#evilStopBtn").style.display=s.running?"":"none";
  }catch(e){}
}
async function evilStart(){
  const ssid=$("#evilSsid").value.trim(); if(!ssid)return toast("Enter an SSID","err");
  if(!(await confirmModal("Start EvilAP '"+ssid+"'?\nYour management AP will be replaced until you stop it.")))return;
  await jpost("/api/evilap/start",{ssid,template:$("#evilTpl").value});
  toast("EvilAP started"); refreshEvil();
}
async function loadEvilHtml(){
  const t=await fetch("/api/evilap/html").then(r=>r.text());
  $("#evilHtml").value=t.replace(/\r\n?/g,"\n"); toast("Loaded custom page");
}
async function saveEvilHtml(){
  await jpost("/api/evilap/html",{content:$("#evilHtml").value});
  toast("Custom portal page saved");
}
async function clearEvilHtml(){
  if(!(await confirmModal("Remove custom page (fall back to template)?")))return;
  await jpost("/api/evilap/html",{content:""});
  $("#evilHtml").value=""; toast("Custom page removed");
}
async function showCreds(){
  const t=await fetch("/api/evilap/creds").then(r=>r.text());
  const box=$("#credsBox");
  box.style.display="block";
  box.textContent=t.trim()||"(nothing captured yet)";
}
async function clearCreds(){
  if(!(await confirmModal("Delete all captured credentials?")))return;
  await api("/api/file?path="+encodeURIComponent("/logs/creds.enc"),{method:"DELETE"});
  showCreds(); toast("Captured credentials cleared");
}

/* ============================ REFERENCE VIEW ============================= */
const REF_GROUPS=[
["Core DuckyScript",[
 ["DELAY <ms>","Pause for the given milliseconds (max 60000 per line). Essential before and after GUI commands so the host can react.","DELAY 1000\nGUI r\nDELAY 500\nSTRING notepad\nENTER"],
 ["DEFAULTDELAY <ms>","A pause inserted automatically after every command line. Set it once near the top of a script instead of sprinkling DELAYs everywhere.","DEFAULTDELAY 200\nGUI r\nSTRING calc\nENTER"],
 ["STRING <text>","Types text exactly as written (5 ms/char pacing). Value commands like GET_IP or RANDOM_CHAR 12 are evaluated and substituted when used as whole words.","STRING Hello world\nSTRING password: RANDOM_CHAR 12"],
 ["STRINGLN <text>","Types text, then presses Enter.","STRINGLN echo done"],
 ["Modifier combos","Hold modifiers and tap keys: GUI, CTRL, ALT, SHIFT, ALTGR. Combine in one line: `CTRL-SHIFT ESC` opens Task Manager. Also works alone: `ENTER`, `F5`.","GUI r\nCTRL-SHIFT ESC\nALT F4"],
 ["Special keys","ENTER SPACE TAB ESC ESCAPE BACKSPACE DELETE DEL HOME END INSERT PAGEUP PAGEDOWN CAPSLOCK APP UP DOWN LEFT RIGHT (ARROW variants too).","ENTER\nTAB\nBACKSPACE"],
 ["REPEAT <n>","Re-executes the previous command line n times. The line being repeated is the last non-REPEAT line.","STRINGLN spam\nREPEAT 5"],
 ["REM / REM_BLOCK","Comments. REM skips one line; REM_BLOCK_START ... REM_BLOCK_END skips a whole block.","REM this line is ignored\nREM_BLOCK_START\nNothing here runs\nREM_BLOCK_END"],
]],
["Logic & Conditions",[
 ["IF / ELSE_IF / ELSE / END_IF","The general conditional. Conditions compare VALUE COMMANDS (see below) against literals with = or !=, or use bare truthiness. Blocks nest, and ELSE_IF is evaluated lazily top-to-bottom.","IF GET_IP = 192.168.0.1\n  STRING we are on the home network\nELSE_IF WIFI_CONNECTED != false\n  STRING on some other network\nELSE\n  STRING offline\nEND_IF"],
 ["IF_OS <name>","Runs the block only if the last DETECT_OS result matches: windows, linux, macos, ios, android, chromeos or unknown. Requires DETECT_OS to have run (device auto-detect can be enabled in Settings).","DETECT_OS\nIF_OS windows\n  GUI r\nELSE_IF macos\n  GUI SPACE\nEND_IF"],
 ["IF_SSID <name>","True if a WiFi access point with that SSID is currently visible. Scans for ~2 seconds.","IF_SSID HomeNetwork\n  CONNECT_AP HomeNetwork mypassword\nEND_IF"],
 ["IF_WIFI","True when the device is connected to a network as a client (after a successful CONNECT_AP).","CONNECT_AP Office ap-password\nIF_WIFI\n  STRING connected\nELSE\n  STRING failed\nEND_IF"],
 ["ELSE_IF <value>","Alternative branch that INHERITS the parent condition type: inside IF_OS it compares OS names, inside IF_SSID it checks another SSID.","IF_SSID CorpWiFi\n  CONNECT_AP CorpWiFi pw1\nELSE_IF HomeWiFi\n  CONNECT_AP HomeWiFi pw2\nEND_IF"],
 ["Value commands","Commands that produce a value usable in STRING payloads or IF conditions: GET_IP, DETECT_OS, WIFI_CONNECTED, RANDOM_NUM <min> <max>, RANDOM_CHAR <len>.","STRING IP is GET_IP\nIF RANDOM_NUM 1 10 = 7\n  STRING lucky\nEND_IF"],
]],
["Device Hardware",[
 ["LED_ON #RRGGBB","Light the status LED with a hex color.","LED_ON #FF00AA"],
 ["LED_OFF","Turn the LED off.","LED_OFF"],
 ["LED_BLINK <times> #RRGGBB","Blink n times, 250 ms on/off. Defaults: 5x red.","LED_BLINK 3 #00FF00"],
 ["SCREEN_ON / SCREEN_OFF","Backlight on/off.","SCREEN_ON\nDELAY 2000\nSCREEN_OFF"],
 ["SCREEN_TEXT <text> [#RRGGBB]","Show text on the built-in screen, optional hex color.","SCREEN_TEXT Pwned #00FF00"],
 ["SCREEN_CLR","Clear the display.","SCREEN_CLR"],
 ["WAIT_BUTTON [secs] [CONTINUE|STOP]","Wait for the BOOT button. If timeout expires: CONTINUE (default) keeps running, STOP aborts the script.","WAIT_BUTTON 60 STOP\nSTRING nobody pressed the button in 60s"],
]],
["Input & Randomness",[
 ["HUMAN_TYPE <text>","Types at ~40 wpm with random jitter - defeats keystroke-timing analysis and looks natural on screen.","HUMAN_TYPE this looks like a human typed it"],
 ["RANDOM_NUM <min> <max>","Types a random number in range.","STRING PIN: RANDOM_NUM 1000 9999"],
 ["RANDOM_CHAR <len>","Types len random alphanumeric characters.","STRING password: RANDOM_CHAR 16"],
 ["JIGGLE_MOUSE <secs>","Move the mouse 1px every half second so the host never sleeps. Subtle by design.","JIGGLE_MOUSE 300"],
]],
["Network & System",[
 ["GET_IP","Types the device IP (station IP if connected, else the AP IP). As a value command it can be compared in IF blocks.","IF GET_IP = 192.168.0.42\n  STRING home network\nEND_IF"],
 ["CONNECT_AP <ssid> [password]","Join a WiFi network as a client while keeping the config AP alive. Logs the result.","CONNECT_AP MyNetwork s3cret"],
 ["WIFI_CONNECTED","Value command: true/false depending on station state.","IF WIFI_CONNECTED = true\n  STRING online\nEND_IF"],
 ["DETECT_OS","Fingerprint the host OS via the keyboard-LED side channel (~10 s, toggles your lock keys and restores them). Result is cached until unplug and shown on Status.","DETECT_OS\nIF_OS windows\n  GUI r\nEND_IF"],
 ["RESET_FIRM","Factory-reset all settings and reboot. DESTRUCTIVE.","RESET_FIRM"],
 ["SELF_DESTRUCT","Wipes EVERYTHING including firmware. Recovery only by re-flash. Absolute last resort.","SELF_DESTRUCT"],
 ["LOG <message>","Write a message to the encrypted device log (Status page).","LOG payload finished cleanly"],
 ["USB_STORAGE <enable|disable>","Expose the SD card as a USB drive alongside HID so scripts can move files. Re-enumerates USB on change.","USB_STORAGE enable\nDELAY 3000"],
 ["BRUTEFORCE_PIN <len> [delayMs]","Types every numeric code of the given length (0000, 0001, ...), pressing Enter after each and backspacing for the next. Default 500 ms between attempts. Stop anytime via the web UI.","BRUTEFORCE_PIN 4 300"],
 ["BRUTEFORCE_LOGIN <file>","Types user/password pairs from a file on the SD card (lines like user:pass or user,pass). Tab between fields, Enter to submit, 800 ms pace.","BRUTEFORCE_LOGIN /creds.txt"],
 ["TUNNEL ON|OFF","Enable or disable external relay access (Settings > External Access).","TUNNEL ON"],
]],
];

function refView(){
  let html = `<div class="panel"><h2>${icon("book")} DuckyScript Reference</h2>
   <p class="muted">Click any command to expand its explanation and example. Samples that can be loaded straight into the editor are at the bottom.</p>`;
  for(const [group, cmds] of REF_GROUPS){
    html += `<div class="refgroup">${group}</div>`;
    for(const [cmd,desc,ex] of cmds){
      html += `<details class="refitem"><summary><span class="mono" style="color:var(--accent2)">${esc(cmd)}</span></summary>
        <div class="refbody"><p>${esc(desc)}</p>
        ${ex?`<pre class="example">${esc(ex)}</pre>`:""}</div></details>`;
    }
  }
  html += `</div><div class="panel"><h2>${icon("script")} Sample Programs</h2>
   <p class="muted">One click loads them into the Script Studio.</p><div id="samples"></div></div>`;
  view.innerHTML = html;
  const box=$("#samples");
  for(const [title,desc,code] of SAMPLES){
    const d=document.createElement("details");
    d.className="refitem";
    d.innerHTML=`<summary><span style="color:var(--accent2)">${esc(title)}</span>
      <span class="muted" style="font-weight:400;font-size:12px">&nbsp;${esc(desc)}</span></summary>
      <div class="refbody"><pre class="example">${esc(code)}</pre>
      <button class="small" onclick="loadSample('${esc(title)}')">Load into editor</button></div>`;
    box.appendChild(d);
  }
}
function loadSample(title){
  const s = SAMPLES.find(x=>x[0]===title); if(!s) return;
  location.hash = "#tools";
  setTimeout(()=>{
    CODE().value = s[2].replace(/\r\n?/g,"\n");
    $("#scriptName").value = title.toLowerCase().replace(/[^a-z0-9]+/g,"_")+".ds";
    curScript=""; highlight(); syncScroll();
    toast("Sample loaded: "+title);
  }, 60);
}

/* ============================ STATUS VIEW ============================= */
let statusTimer=null;
function statusView(){
  view.innerHTML=`<div class="panel"><h2>${icon("gauge")} System Status</h2><table id="statT"></table></div>
  <div class="panel"><h2>${icon("file")} Debug Log</h2>
    <button class="small" onclick="refreshLog()">Refresh</button>
    <pre id="logBox" style="max-height:280px;overflow:auto;background:var(--code-bg);padding:10px;border-radius:8px;margin-top:10px" class="mono"></pre></div>
  </div>`;
  refreshStatus(); statusTimer=setInterval(refreshStatus,2000); refreshLog();
}
async function doReboot(){ if(await confirmModal("Reboot device?")) await jpost("/api/reboot",{}); toast("Rebooting..."); }
async function doReset(){ if(await confirmModal("Factory reset ALL settings?")) await jpost("/api/reset",{}); toast("Settings reset"); }
async function doFormat(){ if(await confirmModal("Wipe the SD card? ALL FILES WILL BE LOST!"))
  if(await confirmModal("Are you REALLY sure? This cannot be undone.")) await jpost("/api/format-sd",{}); toast("SD wiped"); }
async function doDestroy(){
  if(!(await confirmModal("SELF DESTRUCT? Wipes ALL settings, web files, SD contents AND the firmware!")))return;
  if(!(await confirmModal("FINAL WARNING: recovery requires a full re-flash. Continue?")))return;
  await fetch("/api/selfdestruct?confirm=DESTROY",{method:"POST"});
  toast("Self destruct executed","err");
}
async function refreshStatus(){
  const s=await api("/api/status");
  const up=Math.floor(s.uptime), hh=Math.floor(up/3600), mm=Math.floor(up%3600/60), ss=up%60;
  const st=s.scriptState==="RUNNING"?["run","Running"]:s.scriptState==="FINISHED"?["fin","Finished"]:["sb","Standby"];
  $("#statT").innerHTML=`
   <tr><td>Firmware</td><td>${esc(s.fwName||"")} <span class="muted">v${esc(s.fw||"")}</span></td></tr>
   <tr><td>Free RAM</td><td>${(s.heap/1024).toFixed(0)} KB <span class="muted">(min ${(s.heapMin/1024)|0} KB)</span></td></tr>
   <tr><td>CPU</td><td>${s.cpuMhz} MHz</td></tr>
   <tr><td>Uptime</td><td>${hh}h ${mm}m ${ss}s</td></tr>
   <tr><td>SD Free</td><td>${s.sdTotal?((s.sdFree/1048576).toFixed(1)+" / "+(s.sdTotal/1048576).toFixed(1)+" MB"):"not detected"}</td></tr>
   <tr><td>Connection</td><td>${s.usbHost?"Plugged into computer":"Power only"}${s.detectedOS&&s.detectedOS!=="Unknown"?` — <span style="color:var(--accent)">${icon("os",15)}</span> ${esc(s.detectedOS)}`:""}</td></tr>
   <tr><td>WiFi AP</td><td>${s.ip} (${s.wifiClients} client(s))</td></tr>
   <tr><td>Script</td><td><span class="badge ${st[0]}">${st[1]}</span> ${esc(s.scriptName)}</td></tr>`;
}
async function refreshLog(){ const b=$("#logBox"); if(b) b.textContent=await api("/api/log"); }

/* =========================== SETTINGS VIEW ============================ */
function settingsView(){
  view.innerHTML=`
  <div class="setgrid">
  <div class="setcard"><h3>${icon("wifi")} WiFi Access Point</h3>
    <p class="desc">The management network this device broadcasts for browser access. Applies after reboot.</p>
    <label>SSID<input id="ssid"></label>
    <label>Password<input id="wifiPass" type="password"></label>
    <label>Hostname (device reachable at &lt;hostname&gt;.local on networks it joins)
      <input id="hostname" placeholder="lilystrike"></label>
    <label><input type="checkbox" id="wifiHidden" style="width:auto"> Hidden SSID — won't broadcast; join manually</label>
  </div>
  <div class="setcard"><h3>${icon("gauge")} Login Credentials</h3>
    <p class="desc">Web interface login. Leave a box empty to keep the current value.</p>
    <label>Username<input id="user"></label>
    <label>Password<input id="webPass" type="password"></label>
  </div>
  <div class="setcard"><h3>${icon("file")} Encryption Password</h3>
    <p class="desc">Encrypts scripts, logs and captured data on the SD card (AES-256-GCM). Changing it makes previously stored files unreadable.</p>
    <label>Password<input id="encPassword" type="password"></label>
  </div>
  <div class="setcard"><h3>${icon("bolt")} Hardware Defaults</h3>
    <p class="desc">Boot behavior of the screen and LED. Stealth by default: everything off.</p>
    <label><input type="checkbox" id="screenOnBoot" style="width:auto"> Screen on at boot</label>
    <label><input type="checkbox" id="ledOnBoot" style="width:auto"> LED on at boot</label>
    <label>Backlight brightness (0–255)
      <input type="number" id="brightness" min="0" max="255" value="128"></label>
    <label><input type="checkbox" id="autoDetectOS" style="width:auto"> Auto-detect OS on plug-in (cached until unplug)</label>
  </div>
  <div class="setcard"><h3>${icon("keyboard")} USB Identity (Spoofing)</h3>
    <p class="desc">What the host sees at enumeration. Applies on next plug-in.</p>
    <label>Preset<select id="spoofPreset" onchange="applyPreset()"><option value="">— custom —</option></select></label>
    <label>VID (hex)<input id="spoofVid" style="max-width:140px"></label>
    <label>PID (hex)<input id="spoofPid" style="max-width:140px"></label>
    <label>Vendor<input id="spoofVendor"></label>
    <label>Product<input id="spoofProduct"></label>
    <label>Serial (empty = random)<input id="spoofSerial"></label>
    <label><input type="checkbox" id="spoofRandBoot" style="width:auto"> New random identity on every boot</label>
    <div style="display:flex;gap:8px">
      <button class="small primary" onclick="saveSpoof()">Save Identity</button>
      <button class="small" onclick="randomSpoof()">Randomize now</button>
    </div>
  </div>
  <div class="setcard"><h3>${icon("folder")} USB Storage &amp; Stealth Drive</h3>
    <p class="desc">Expose the Micro-SD as a USB drive, or boot as a read-only "innocent" stick backed by an isolated disk image containing only disk.zip. Recovery token: put UNLOCK.txt on the card root.</p>
    <label>False Thumbdrive mode
      <select id="thumbMode" onchange="saveMsc()">
        <option value="0">Off</option>
        <option value="1">First Load — always boots as thumbdrive</option>
        <option value="2">Second Load — normal once, then always thumbdrive</option>
      </select></label>
    <label><input type="checkbox" id="usbStorage" style="width:auto" onchange="saveMsc()">
      USB_STORAGE — full card read-write alongside HID</label>
  </div>
  <div class="setcard"><h3>${icon("gauge")} Power Mode</h3>
    <p class="desc">CPU clock + WiFi transmit power. High may trip weak USB ports. Script DELAY timings change with clock speed — re-tune payloads when switching.</p>
    <label>Mode<select id="powerMode" onchange="savePower()">
      <option value="0">Low — 80 MHz | 10 dBm (max stealth)</option>
      <option value="1">Normal — 160 MHz | 17 dBm</option>
      <option value="2">High — 240 MHz | 19.5 dBm</option>
    </select></label>
  </div>
  <div class="setcard"><h3>${icon("os")} MAC Spoofing</h3>
    <p class="desc">Changes the MAC address the device presents over WiFi. Applies at next boot.</p>
    <label>Mode<select id="macMode" onchange="saveMac()">
      <option value="0">Hardware default</option>
      <option value="1">Randomize on every boot</option>
      <option value="2">Custom MAC</option>
    </select></label>
    <label>Custom MAC (AA:BB:CC:DD:EE:FF)<input id="macCustom" placeholder="02:AB:CD:EF:11:22" onchange="saveMac()"></label>
  </div>
  <div class="setcard"><h3>${icon("wifi")} External Access (Tunnel)</h3>
    <p class="desc">Reach this device's interface from outside its network via a relay. Host <b class="mono">tools/relay_server.py</b> on any VPS, then point the device here. Browser: <b class="mono">http://relay/t/&lt;token&gt;/</b></p>
    <label>Relay URL<input id="tunnelUrl" placeholder="http://my-vps:5000" onchange="saveTunnel()"></label>
    <label>Token (auto-generated; override if you like)<input id="tunnelToken" onchange="saveTunnel()"></label>
    <label><input type="checkbox" id="tunnelEnabled" style="width:auto" onchange="saveTunnel()"> Enable tunnel when on an internet network</label>
  </div>
  <div class="setcard"><h3>${icon("gear")} Interface Availability</h3>
    <p class="desc">Temporarily disable the web interface (hold BOOT 1.5 s on next boot to re-enable). Permanent mode requires a firmware re-flash to undo.</p>
    <label><input type="checkbox" id="tempOff" style="width:auto"> Temporarily disable web interface</label>
    <p class="err" style="font-size:12px">Permanent disable is irreversible without a re-flash!</p>
    <button class="danger" onclick="permDisable()">Enable Permanent Disable</button>
  </div>
  </div>
  <div class="setcard" style="margin-top:14px;border-color:var(--err)">
    <h3 style="color:var(--err)">${icon("bolt")} System / Danger Zone</h3>
    <p class="desc">Destructive operations. Self destruct wipes settings, web files, SD card AND the firmware itself — recovery only by re-flashing.</p>
    <div style="display:flex;gap:10px;flex-wrap:wrap">
      <button class="danger" onclick="doReboot()">Reboot</button>
      <button class="danger" onclick="doReset()">Reset Firmware Settings</button>
      <button class="danger" onclick="doFormat()">Wipe Micro-SD</button>
      <button class="danger" onclick="doDestroy()">SELF DESTRUCT</button>
    </div>
  </div>
  </div>
  <div style="margin-top:16px"><button class="primary" onclick="saveSettings()">Save Settings</button></div>`;
  loadSettingsState();
}
async function loadSettingsState(){
  try{
    const s=await api("/api/settings");
    $("#ssid").value=s.ssid||""; $("#user").value=s.user||"";
    $("#hostname").value=s.hostname||"lilystrike";
    $("#wifiHidden").checked=!!s.wifiHidden;
    $("#screenOnBoot").checked=!!s.screenOnBoot;
    $("#ledOnBoot").checked=!!s.ledOnBoot;
    $("#brightness").value=s.brightness??128;
    $("#autoDetectOS").checked=!!s.autoDetectOS;
    if(s.permOff) toast("Interface is PERMANENTLY disabled (on reboot)","err");
  }catch(e){}
  api("/api/sys").then(c=>{
    $("#powerMode").value=String(c.powerMode);
    $("#macMode").value=String(c.macMode);
    $("#macCustom").value=c.macCustom||"";
    $("#tunnelUrl").value=c.tunnelUrl||"";
    $("#tunnelToken").value=c.tunnelToken||"";
    $("#tunnelEnabled").checked=!!c.tunnelEnabled;
  }).catch(()=>{});
  api("/api/msc").then(m=>{
    $("#thumbMode").value=String(m.thumb);
    $("#usbStorage").checked=!!m.storage;
  }).catch(()=>{});
  loadSpoof();
}
// Per-group saves: only the group the user touched is sent + toasted.
let _lastSys = {};
async function savePower(){
  const v = +$("#powerMode").value;
  if (_lastSys.powerMode === v) return;
  _lastSys.powerMode = v;
  await jpost("/api/sys",{powerMode:v});
  toast("Power mode saved and applied");
}
async function saveMac(){
  const mode = +$("#macMode").value, custom = $("#macCustom").value.trim();
  if (_lastSys.macMode === mode && _lastSys.macCustom === custom) return;
  _lastSys.macMode = mode; _lastSys.macCustom = custom;
  await jpost("/api/sys",{macMode:mode, macCustom:custom});
  toast("MAC saved — applies at next boot");
}
async function saveTunnel(){
  const payload = {tunnelUrl:$("#tunnelUrl").value, tunnelToken:$("#tunnelToken").value,
                   tunnelEnabled:$("#tunnelEnabled").checked};
  const sig = JSON.stringify(payload);
  if (_lastSys.tunnel === sig) return;
  _lastSys.tunnel = sig;
  await jpost("/api/sys", payload);
  toast("Tunnel settings saved");
}
async function saveMsc(){
  await jpost("/api/msc",{thumb:+$("#thumbMode").value, storage:$("#usbStorage").checked});
  toast("USB storage settings saved");
}
async function loadSpoof(){
  try{
    const sp=await api("/api/spoof");
    $("#spoofVid").value=sp.vid; $("#spoofPid").value=sp.pid;
    $("#spoofVendor").value=sp.vendor; $("#spoofProduct").value=sp.product;
    $("#spoofSerial").value=sp.serial;
    $("#spoofRandBoot").checked=!!sp.randomPerBoot;
    $("#spoofPreset").innerHTML='<option value="">— custom —</option>'+
      sp.presets.map((p,i)=>`<option value="${i}">${esc(p.vendor)} — ${esc(p.product)}</option>`).join("");
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
  await jpost("/api/spoof",{randomPerBoot:$("#spoofRandBoot").checked});
  toast("Identity saved — applies on next boot/plug-in");
}
async function randomSpoof(){
  await jpost("/api/spoof",{randomize:true});
  loadSpoof(); toast("Random identity saved");
}
async function saveSettings(){
  const b={};
  for(const [id,key] of [["ssid","ssid"],["wifiPass","wifiPass"],["user","user"],
                          ["webPass","webPass"],["encPassword","encPassword"],["hostname","hostname"]]){
    const v=$("#"+id).value; if(v.length) b[key]=v;
  }
  b.wifiHidden=$("#wifiHidden").checked;
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

/* ============================== SAMPLES ============================== */
const SAMPLES=[
 ["Hello Notepad","Opens Notepad on Windows and types a message.",
`REM Basic Windows payload
DELAY 1000
GUI r
DELAY 500
STRING notepad
ENTER
DELAY 1000
STRING Hello from LilyStrike!`],
 ["OS-Aware Greeting","Different run dialog per operating system.",
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
END_IF`],
 ["Stealth Check","Waits for you to press BOOT before firing.",
`REM Wait up to 60s for button press; abort if nobody does
WAIT_BUTTON 60 STOP
DELAY 1000
LED_BLINK 3 #00FF00
GUI r
STRING notepad
ENTER
DELAY 800
STRING Button-triggered!`],
 ["Human Typing Demo","Random password + human-like typing.",
`DELAY 1000
STRING username: admin
ENTER
STRING password: RANDOM_CHAR 12
ENTER
DELAY 500
HUMAN_TYPE This sentence was typed like a human at about forty words per minute.`],
 ["Network Report","Joins WiFi and reports the device IP.",
`DELAY 1000
GUI r
STRING cmd
ENTER
DELAY 1500
CONNECT_AP MyHomeNetwork MyPassword
IF_WIFI
  STRING Device IP: GET_IP
  ENTER
ELSE
  STRING Could not connect to WiFi
  ENTER
END_IF`],
 ["Light Show","Pure hardware demo - no host needed.",
`LED_BLINK 3 #FF0000
LED_ON #00FF00
DELAY 1000
LED_OFF
SCREEN_TEXT LilyStrike ready #00FF00
DELAY 2000
SCREEN_CLR
SCREEN_OFF`],
];

/* ============================== ROUTER ================================ */
const NAV=[
 ["tools","BadUSB","bolt"],["files","Files","folder"],
 ["control","Live Control","keyboard"],["evilap","EvilAP","wifi"],
 ["reference","Reference","book"],["status","Status","gauge"],
 ["settings","Settings","gear"],
];
function buildNav(){
  $("#nav").innerHTML = NAV.map(([h,label,ic])=>
    `<a href="#${h}" data-h="${h}">${icon(ic)} ${label}</a>`).join("");
}
function route(){
  clearInterval(statusTimer); clearInterval(evilTimer); clearInterval(ctrlTimer);
  const h=(location.hash||"#tools").slice(1);
  document.querySelectorAll("#nav a").forEach(a=>a.classList.toggle("active",a.dataset.h===h));
  if(h==="files")filesView();
  else if(h==="control")controlView();
  else if(h==="evilap")evilapView();
  else if(h==="reference")refView();
  else if(h==="status")statusView();
  else if(h==="settings")settingsView();
  else toolsView();
}
window.onhashchange=route;
$("#logout").onclick=()=>fetch("/api/login",{method:"POST"}).then(()=>location.href="/login.html");

/* -------------------------------- boot --------------------------------- */
initTheme();
buildNav();
route();
checkEula();
api("/api/status").then(s=>{
  if(s.fw){ $("#brandVer").textContent="v"+s.fw; $("#footVer").textContent=FW_FOOT; }
}).catch(()=>{});
const FW_FOOT="LilyStrike — for authorized testing only";
