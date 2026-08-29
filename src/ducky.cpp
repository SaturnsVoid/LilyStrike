// ============================================================================
// ducky.cpp - DuckyScript v3 interpreter + Step-2 custom commands
// ----------------------------------------------------------------------------
// USB HID: TinyUSB-backed USBHIDKeyboard (+ USBHIDMouse for JIGGLE_MOUSE).
//
// Supported syntax:
//   Core:    REM/REM_BLOCK, DELAY, DEFAULTDELAY, STRING, STRINGLN, ENTER,
//            specials/arrows/F1-12, modifier combos ("GUI r"), REPEAT n
//   Custom:  LOG msg                       -> encrypted device log
//            DETECT_OS                     -> run host OS fingerprint (~10s)
//            IF_OS <windows|linux|macos|ios|android|chromeos|unknown>
//            ELSE_IF <value>               -> inherits parent condition type
//            IF_SSID <ssid>                -> AP visible?
//            IF_WIFI                       -> station connected?
//            ELSE / END_IF                 -> block structure (nestable)
//            LED_ON #RRGGBB | LED_OFF | LED_BLINK <times> #RRGGBB
//            SCREEN_ON | SCREEN_OFF | SCREEN_CLR | SCREEN_TEXT txt [#fg #bg]
//            RANDOM_NUM <min> <max>        -> types a random number
//            RANDOM_CHAR <len>             -> types random chars
//            HUMAN_TYPE txt                -> ~40wpm jittered typing
//            GET_IP                        -> types device IP (or "no-ip")
//            WAIT_BUTTON [secs] [CONTINUE|STOP]   (default 30 CONTINUE)
//            JIGGLE_MOUSE <secs>           -> subtle mouse motion
//            CONNECT_AP <ssid> [password]  -> join network as station
//            RESET_FIRM                    -> factory reset + reboot
//   NOTE: SSID_SPAM and SCREEN_IMG deferred (Step 3 wifi-lowlevel / image
//         loader work); unknown commands log an error but don't abort.
//
// Control flow is implemented with an index-based line walker + a block
// matcher (findMatching) so IF blocks can nest.
// ============================================================================
#include "ducky.h"
#include "config.h"
#include "crypt.h"
#include "hw.h"
#include "detect_os.h"
#include "spoof.h"
#include "sys.h"
#include "msc.h"
#include "tunnel.h"
#include "wifiattack.h"
#include "version.h"
#include <esp32-hal-tinyusb.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <WiFi.h>
#include <esp_random.h>
#include <math.h>
#include <SD_MMC.h>
#include <vector>

namespace ducky {

USBHIDKeyboard kb;      // declared first: setLayout below uses it

// Keyboard layout support (Step 4.x): kb.begin(layout) re-initializes with
// the given scancode map. The extra externs cover layouts whose symbols the
// core header doesn't declare but ships as .cpp files anyway.
// NOTE: these live outside our namespace (C symbols from the core lib)
extern "C" const uint8_t KeyboardLayout_da_DK[];
extern "C" const uint8_t KeyboardLayout_hu_HU[];
extern "C" const uint8_t KeyboardLayout_ja_JP[];
extern "C" const uint8_t KeyboardLayout_pt_BR[];
struct LayoutEntry { const char* name; const uint8_t* layout; };
static const LayoutEntry LAYOUTS[] = {
    {"en_US", KeyboardLayout_en_US},
    {"de_DE", KeyboardLayout_de_DE},
    {"es_ES", KeyboardLayout_es_ES},
    {"fr_CH", KeyboardLayout_fr_CH},
    {"fr_FR", KeyboardLayout_fr_FR},
    {"it_IT", KeyboardLayout_it_IT},
    {"pt_PT", KeyboardLayout_pt_PT},
    {"pt_BR", KeyboardLayout_pt_BR},
    {"sv_SE", KeyboardLayout_sv_SE},
    {"da_DK", KeyboardLayout_da_DK},
    {"hu_HU", KeyboardLayout_hu_HU},
    {"ja_JP", KeyboardLayout_ja_JP},
};
std::vector<String> layoutNames() {
    std::vector<String> out;
    for (auto& l : LAYOUTS) out.push_back(l.name);
    return out;
}
void setLayout(const String& name) {
    for (auto& l : LAYOUTS)
        if (name == l.name) { kb.begin(l.layout); logLine("layout: " + name); return; }
}

static USBHIDMouse mouse;
static bool kbStarted = false;
static volatile bool g_running = false;
static volatile bool g_stopRequested = false;
static String s_state = "STANDBY";

// ---------------------------------------------------------------- modifiers
struct KeyName { const char* name; uint8_t keymod; };
static const KeyName MODS[] = {
    {"GUI", KEY_LEFT_GUI}, {"WINDOWS", KEY_LEFT_GUI}, {"COMMAND", KEY_LEFT_GUI},
    {"CTRL", KEY_LEFT_CTRL}, {"CONTROL", KEY_LEFT_CTRL},
    {"ALT", KEY_LEFT_ALT}, {"ALTGR", KEY_RIGHT_ALT},
    {"SHIFT", KEY_LEFT_SHIFT},
};
static const KeyName SPECIALS[] = {
    {"ENTER", KEY_RETURN}, {"SPACE", ' '}, {"TAB", KEY_TAB},
    {"ESCAPE", KEY_ESC}, {"ESC", KEY_ESC},
    {"DOWNARROW", KEY_DOWN_ARROW}, {"DOWN", KEY_DOWN_ARROW},
    {"UPARROW", KEY_UP_ARROW}, {"UP", KEY_UP_ARROW},
    {"LEFTARROW", KEY_LEFT_ARROW}, {"LEFT", KEY_LEFT_ARROW},
    {"RIGHTARROW", KEY_RIGHT_ARROW}, {"RIGHT", KEY_RIGHT_ARROW},
    {"BACKSPACE", KEY_BACKSPACE}, {"DELETE", KEY_DELETE}, {"DEL", KEY_DELETE},
    {"HOME", KEY_HOME}, {"END", KEY_END},
    {"INSERT", KEY_INSERT}, {"PAGEUP", KEY_PAGE_UP}, {"PAGEDOWN", KEY_PAGE_DOWN},
    {"CAPSLOCK", KEY_CAPS_LOCK}, {"APP", HID_KEY_APPLICATION},
    {"F1", KEY_F1},{"F2", KEY_F2},{"F3", KEY_F3},{"F4", KEY_F4},{"F5", KEY_F5},
    {"F6", KEY_F6},{"F7", KEY_F7},{"F8", KEY_F8},{"F9", KEY_F9},{"F10", KEY_F10},
    {"F11", KEY_F11},{"F12", KEY_F12},
};

static bool lookup(const KeyName* table, size_t n, const String& k, uint8_t& out) {
    for (size_t i = 0; i < n; i++)
        if (k.equalsIgnoreCase(table[i].name)) { out = pgm_read_byte(&table[i].keymod); return true; }
    return false;
}
static bool resolveKey(const String& tok, uint8_t& out) {
    if (lookup(MODS, sizeof(MODS)/sizeof(MODS[0]), tok, out)) return true;
    if (lookup(SPECIALS, sizeof(SPECIALS)/sizeof(SPECIALS[0]), tok, out)) return true;
    if (tok.length() == 1) { out = (uint8_t)tok[0]; return true; }
    return false;
}

// ---------------------------------------------------------------- helpers
static void pressCombo(const String& args) {
    std::vector<uint8_t> taps;
    int start = 0;
    while (start <= (int)args.length()) {
        int sp = args.indexOf(' ', start);
        String tok = (sp < 0) ? args.substring(start) : args.substring(start, sp);
        tok.trim();
        if (tok.length()) {
            uint8_t k;
            if (resolveKey(tok, k)) {
                if (lookup(MODS, sizeof(MODS)/sizeof(MODS[0]), tok, k)) kb.press(k);
                else taps.push_back(k);
            }
        }
        if (sp < 0) break;
        start = sp + 1;
    }
    for (auto t : taps) { kb.press(t); delay(8); kb.release(t); }
    kb.releaseAll();
    delay(100);   // host settle time after combos
}

static void typeString(const String& s) {
    for (size_t i = 0; i < s.length(); i++) { kb.write(s[i]); delay(5); }
}

// ~40 wpm with jitter - looks human, defeats keystroke-timing analysis.
static void humanType(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        kb.write(s[i]);
        // base 120ms +/- up to 100ms jitter => roughly 35-45 wpm average
        delay(70 + esp_random() % 100);
    }
}

// "#RRGGBB" or "RRGGBB" -> RGB struct. Returns black on parse failure.
static RGB parseColor(String hex) {
    hex.trim();
    if (hex.startsWith("#")) hex.remove(0, 1);
    if (hex.length() != 6) return {0, 0, 0};
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    RGB c;
    c.r = nyb(hex[0]) * 16 + nyb(hex[1]);
    c.g = nyb(hex[2]) * 16 + nyb(hex[3]);
    c.b = nyb(hex[4]) * 16 + nyb(hex[5]);
    return c;
}

// ------------------------------------------------------- condition handling
enum class Cond : uint8_t { NONE, OS, SSID, WIFI, EXPR };

// ---------------------------------------------------------------- values
// "Value commands" can be evaluated to a string. They are substituted inside
// STRING/STRINGLN/HUMAN_TYPE payloads (whole-token matches) and usable on the
// left side of IF comparisons:  IF GET_IP = 192.168.0.1
static String evalValueCmd(String token) {
    token.trim();
    String up = token; up.toUpperCase();
    if (up == "GET_IP") {
        return (WiFi.status()==WL_CONNECTED) ? WiFi.localIP().toString()
                                             : WiFi.softAPIP().toString();
    }
    if (up == "DETECT_OS")   return detectos::nameOf(detectos::lastResult());
    if (up == "WIFI_CONNECTED") return WiFi.status()==WL_CONNECTED ? "true":"false";
    // Parameterised value commands: NAME arg [arg]
    int sp = token.indexOf(' ');
    String name = (sp>0)?token.substring(0,sp):token;
    String rest = (sp>0)?token.substring(sp+1):String("");
    name.trim(); String nmUp = name; nmUp.toUpperCase();
    if (nmUp == "RANDOM_NUM") {
        int lo=0, hi=100, sp2=rest.indexOf(' ');
        if (sp2>0){lo=rest.substring(0,sp2).toInt();hi=rest.substring(sp2+1).toInt();}
        else if (rest.length()) hi=rest.toInt();
        if (hi<lo){int t=lo;lo=hi;hi=t;}
        return String(lo + (esp_random() % (uint32_t)(hi-lo+1)));
    }
    if (nmUp == "RANDOM_CHAR") {
        int len = constrain(rest.toInt(),1,256);
        const char* alpha="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        String out; for(int i2=0;i2<len;i2++) out+=alpha[esp_random()%(sizeof(alpha)-1)];
        return out;
    }
    return "";   // not a value command
}

// ---- Script variables (VAR cmd / $name substitution) ----
struct ScriptVar { String name, value; };
static std::vector<ScriptVar> g_vars;
static void setVar(const String& name, const String& value) {
    for (auto& v : g_vars) if (v.name == name) { v.value = value; return; }
    g_vars.push_back({name, value});
}
static String getVar(const String& name) {
    for (auto& v : g_vars) if (v.name == name) return v.value;
    return "";
}

// Replace value-command occurrences with their values.
// Parameterised forms consume their arguments ("RANDOM_CHAR 12" -> 12 chars),
// so nothing leaks through as literal text (the old whole-token-only version
// turned "STRING RANDOM_CHAR 12" into "<char> 12").
static String substValues(const String& s) {
    // split into whitespace tokens
    std::vector<String> toks;
    int start = 0;
    while (start <= (int)s.length()) {
        int sp = s.indexOf(' ', start);
        String t = (sp<0)?s.substring(start):s.substring(start,sp);
        t.trim();
        if (t.length()) toks.push_back(t);
        if (sp<0) break;
        start = sp+1;
    }
    String out;
    for (size_t k=0; k<toks.size(); k++) {
        // $name variable substitution first (also embedded: $name/x patterns)
        if (toks[k].startsWith("$")) {
            String nm = toks[k].substring(1);
            String v = getVar(nm);
            if (v.length() || g_vars.size()) { out += (v.length()?v:"") + " "; continue; }
        }
        String up = toks[k]; up.toUpperCase();
        // parameterised forms gather their arguments before evaluating
        size_t argc = (up=="RANDOM_NUM") ? 2 : (up=="RANDOM_CHAR") ? 1 : 0;
        if (argc) {
            String call = toks[k];
            for (size_t a=1; a<=argc && k+a<toks.size(); a++) call += " "+toks[k+a];
            String v = evalValueCmd(call);
            if (v.length()) { out += v + " "; k += argc; continue; }
        }
        String v = evalValueCmd(toks[k]);
        out += (v.length() ? v : toks[k]) + " ";
    }
    while (out.endsWith(" ")) out.remove(out.length()-1);
    return out;
}

// EXPR: "<value-cmd> [=|!=] <literal>" or bare truthy check.
// Returns empty error-string on success, else a problem description.
static String evalExpr(const String& exprIn, bool& result) {
    String e = exprIn; e.trim();
    // find comparison operator
    int opIdx=-1, opLen=0;
    for (int k=0;k+1<(int)e.length();k++) {
        if (e[k]=='!'&&e[k+1]=='=') { opIdx=k; opLen=2; break; }
        if (e[k]=='='&&e[k+1]!='=') { opIdx=k; opLen=1; break; }
    }
    String left=e, right="", op="==";
    if (opIdx>=0) {
        left=e.substring(0,opIdx); right=e.substring(opIdx+opLen); op=(opLen==2?"!=":"==");
        left.trim(); right.trim();
        if (right.startsWith("\"") && right.endsWith("\"") && right.length()>=2)
            right=right.substring(1,right.length()-1);
    }
    String lv = evalValueCmd(left);
    if (!lv.length() && !left.isEmpty()) {
        // not a value command - treat as bare literal truthiness
        lv = left;
    }
    if (opIdx<0) { result = (lv.length()>0 && lv!="false" && lv!="0"); return ""; }
    result = (op=="==") ? (lv==right) : (lv!=right);
    return "";
}

// Evaluate an IF_* condition at runtime.
static bool evalCondition(Cond type, const String& arg) {
    switch (type) {
        case Cond::OS: {
            HostOS cur = detectos::lastResult();
            return detectos::matches(cur, arg);
        }
        case Cond::SSID: {
            int n = WiFi.scanNetworks();
            bool found = false;
            for (int i = 0; i < n && !found; i++)
                if (WiFi.SSID(i) == arg) found = true;
            WiFi.scanDelete();
            return found;
        }
        case Cond::WIFI:
            return WiFi.status() == WL_CONNECTED;
        case Cond::EXPR: {
            bool r = false;
            evalExpr(arg, r);
            return r;
        }
        default:
            return false;
    }
}

// ------------------------------------------------------------- interpreter
struct Line { String cmd, args; int srcLine; };   // pre-parsed script line

// Map an IF_/ELSE_IF command to its condition type.
static Cond condTypeOf(const String& cmd) {
    if (cmd.equalsIgnoreCase("IF_SSID") ) return Cond::SSID;
    if (cmd.equalsIgnoreCase("IF_WIFI") ) return Cond::WIFI;
    if (cmd.equalsIgnoreCase("IF") )       return Cond::EXPR;  // generic IF <expr>
    return Cond::OS;   // IF_OS and bare ELSE_IF default to OS comparison
}

RunResult run(const String& scriptText, const String& name) {
    RunResult res{true, 0, ""};
    g_stopRequested = false;

    g_running = true;
    s_state = "RUNNING";
    g_state.scriptState = ScriptState::RUNNING;
    g_state.scriptStateSince = time(nullptr);
    g_state.lastScriptName = name;

    // ---- pre-parse into a line vector ----
    std::vector<Line> lines;
    {
        int idx = 0, ln = 0;
        bool inRemBlock = false;
        while (idx <= (int)scriptText.length()) {
            int nl = scriptText.indexOf('\n', idx);
            String raw = (nl < 0) ? scriptText.substring(idx)
                                  : scriptText.substring(idx, nl);
            idx = (nl < 0) ? scriptText.length() + 1 : nl + 1;
            ln++;
            raw.trim();
            if (!inRemBlock) {
                if (raw.equalsIgnoreCase("REM_BLOCK_START")) { inRemBlock = true; continue; }
                if (raw.isEmpty() || raw.startsWith("#") ||
                    (!raw.isEmpty() && raw.startsWith("REM"))) continue;
            } else {
                if (raw.equalsIgnoreCase("REM_BLOCK_END")) inRemBlock = false;
                continue;
            }
            Line L; L.srcLine = ln;
            int sp = raw.indexOf(' ');
            if (sp > 0) { L.cmd = raw.substring(0, sp); L.args = raw.substring(sp + 1); }
            else          L.cmd = raw;
            lines.push_back(L);
        }
    }

    g_vars.clear();
    int defaultDelay = 0;
    String lastCmdLine;
    // ON_ERROR policy: CONTINUE (default), STOP, or JUMP <label>
    String onError = "CONTINUE";
    String onErrorTarget = "";
    std::vector<int> labels;   // parallel: label positions resolved lazily

    // findMatching: index of END_IF matching the IF at `i` (handles nesting).
    auto findMatching = [&](int i) -> int {
        int depth = 0;
        for (int j = i + 1; j < (int)lines.size(); j++) {
            String& c = lines[j].cmd;
            if (c.equalsIgnoreCase("IF_OS") || c.equalsIgnoreCase("IF_SSID") ||
                c.equalsIgnoreCase("IF_WIFI")) depth++;
            else if (c.equalsIgnoreCase("END_IF")) {
                if (depth == 0) return j;
                depth--;
            }
        }
        return -1;
    };
    auto isIfCmd = [](const String& c) {
        return c.equalsIgnoreCase("IF_OS") || c.equalsIgnoreCase("IF_SSID") ||
               c.equalsIgnoreCase("IF_WIFI") || c.equalsIgnoreCase("IF");
    };

    // Pending IF blocks whose TRUE branch is executing; top = current block.
    // Lets ELSE / ELSE_IF know where their enclosing END_IF lives even when
    // nested IFs run inside the branch (those pop themselves first).
    std::vector<int> ifStack;
    // Jump to the first matching depth-0 ELSE_IF/ELSE after IF at `from`,
    // evaluating ELSE_IF conditions lazily with the PARENT's condition type
    // (an ELSE_IF inside IF_SSID tests SSIDs). Returns index to EXECUTE next,
    // or -1 if no branch matches (caller jumps past END_IF).
    auto nextFalseBranch = [&](int from, int endIdx, Cond ptype) -> int {
        int depth = 0;
        for (int j = from; j < endIdx; j++) {
            String& c = lines[j].cmd;
            if (isIfCmd(c)) { depth++; continue; }
            if (c.equalsIgnoreCase("END_IF")) { depth--; continue; }
            if (depth != 0) continue;
            if (c.equalsIgnoreCase("ELSE_IF")) {
                // "ELSE_IF windows" style shorthand -> strip nothing; args are
                // already the comparison value for the parent's condition.
                if (evalCondition(ptype, lines[j].args)) return j + 1;
                continue;   // try next ELSE_IF / ELSE
            }
            if (c.equalsIgnoreCase("ELSE")) return j + 1;
        }
        return -1;   // no branch matched
    };

    std::vector<int> stack;   // manual loop stack instead of recursion

    int i = 0;
    while (i < (int)lines.size()) {
        if (g_stopRequested) { res.ok = false; res.error = "stopped"; break; }
        Line& L = lines[i];
        String cmd = L.cmd, args = L.args;
        res.linesRun++;

        // ---- REPEAT: substitute previous command line N times ----
        int times = 1;
        if (cmd.equalsIgnoreCase("REPEAT")) {
            times = constrain(args.toInt(), 1, 10000);
            if (lastCmdLine.isEmpty()) { i++; continue; }
            int sp2 = lastCmdLine.indexOf(' ');
            cmd = lastCmdLine.substring(0, (sp2 > 0) ? sp2 : (int)lastCmdLine.length());
            args = (sp2 > 0) ? lastCmdLine.substring(sp2 + 1) : String("");
        } else {
            lastCmdLine = L.cmd + (L.args.length() ? " " + L.args : "");
        }

        // ---- control flow (IF / ELSE_IF / ELSE / END_IF) ----
        if (isIfCmd(cmd)) {
            int endIdx = findMatching(i);
            if (endIdx < 0) {
                res.error += "L" + String(L.srcLine) + ":missing END_IF ";
                break;
            }
            // Evaluate lazily: our condition, then any depth-0 ELSE_IFs, then ELSE.
            int target = -1;
            if (evalCondition(condTypeOf(cmd), args)) {
                target = i + 1;                        // take the IF branch
            } else {
                target = nextFalseBranch(i + 1, endIdx, condTypeOf(cmd));
                if (target < 0) target = endIdx + 1;   // no branch matched
            }
            ifStack.push_back(endIdx);                 // branch taken -> remember END_IF
            i = target;
            continue;
        }
        if (cmd.equalsIgnoreCase("ELSE") || cmd.equalsIgnoreCase("ELSE_IF")) {
            // Previous TRUE branch finished - skip past its END_IF.
            i = ifStack.empty() ? (int)lines.size() : ifStack.back() + 1;
            if (!ifStack.empty()) ifStack.pop_back();
            continue;
        }
        if (cmd.equalsIgnoreCase("END_IF")) {
            if (!ifStack.empty()) ifStack.pop_back();
            i++; continue;
        }

        // ---- core commands ----
        bool executed = true;
        if      (cmd.equalsIgnoreCase("DELAY"))         { delay(constrain(args.toInt(),0,60000)); }
        else if (cmd.equalsIgnoreCase("DEFAULTDELAY") ||
                 cmd.equalsIgnoreCase("DEFAULT_DELAY")) { defaultDelay = constrain(args.toInt(),0,60000); }
        else if (cmd.equalsIgnoreCase("STRING"))        { typeString(substValues(args)); }
        else if (cmd.equalsIgnoreCase("STRINGLN"))      { typeString(substValues(args)); kb.press(KEY_RETURN); kb.release(KEY_RETURN); }
        else if (cmd.equalsIgnoreCase("LOG"))           { logLine("[script:" + name + "] " + substValues(args)); }

        // ---- Step 2 custom commands ----
        else if (cmd.equalsIgnoreCase("DETECT_OS"))     {
            HostOS h = detectos::detect();
            g_state.detectedOS = detectos::nameOf(h);
            logLine("[script:" + name + "] DETECT_OS => " + g_state.detectedOS);
        }
        else if (cmd.equalsIgnoreCase("LED_ON"))        { hw::ledSet(parseColor(args)); }
        else if (cmd.equalsIgnoreCase("LED_OFF"))       { hw::ledOff(); }
        else if (cmd.equalsIgnoreCase("LED_BLINK"))     {
            int nTimes = 5;
            String col = "#FF0000";
            int sp2 = args.indexOf('#');
            if (sp2 > 0) { nTimes = constrain(args.substring(0, sp2).toInt(), 1, 60); col = args.substring(sp2); }
            RGB c = parseColor(col);
            for (int b = 0; b < nTimes && !g_stopRequested; b++) {
                hw::ledSet(c); delay(250); hw::ledOff(); delay(250);
            }
        }
        else if (cmd.equalsIgnoreCase("SCREEN_ON"))     { hw::screenOn(); }
        else if (cmd.equalsIgnoreCase("SCREEN_OFF"))    { hw::screenOff(); }
        else if (cmd.equalsIgnoreCase("SCREEN_CLR"))    { hw::screenClear(); }
        else if (cmd.equalsIgnoreCase("SCREEN_TEXT"))   {
            // SCREEN_TEXT Hello [#FF0000 [#000000]] - colors optional
            String txt = args, fg = "#00FF00";
            int hash = args.indexOf('#');
            if (hash >= 0) {
                txt = args.substring(0, hash);
                fg = args.substring(hash);
            }
            // Value commands work here too: SCREEN_TEXT IP: GET_IP #00FF00
            txt = substValues(txt);
            txt.trim();
            hw::screenOn();
            hw::screenText(txt);
        }
        else if (cmd.equalsIgnoreCase("RANDOM_NUM"))    {
            int lo = 0, hi = 100;
            int sp2 = args.indexOf(' ');
            if (sp2 > 0) { lo = args.substring(0,sp2).toInt(); hi = args.substring(sp2+1).toInt(); }
            else if (sp2 == -1 && args.length()) hi = args.toInt();
            if (hi < lo) { int tmp=lo; lo=hi; hi=tmp; }
            uint32_t v = lo + (esp_random() % (uint32_t)(hi - lo + 1));
            typeString(String(v));
        }
        else if (cmd.equalsIgnoreCase("RANDOM_CHAR"))   {
            int len = constrain(args.toInt(), 1, 256);
            const char* alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            for (int c2 = 0; c2 < len; c2++) {
                kb.write(alphabet[esp_random() % (sizeof(alphabet)-1)]);
                delay(5);
            }
        }
        else if (cmd.equalsIgnoreCase("HUMAN_TYPE"))    { humanType(substValues(args)); }
        else if (cmd.equalsIgnoreCase("DISABLE_CAPS"))  {
            // Detect host caps state and turn it off if on.
            if (detectos::capsOn()) {
                kb.press(KEY_CAPS_LOCK); delay(20); kb.release(KEY_CAPS_LOCK);
                delay(150);
                logLine("[script:" + name + "] DISABLE_CAPS: caps was on - disabled");
            } else {
                logLine("[script:" + name + "] DISABLE_CAPS: caps already off");
            }
        }
        else if (cmd.equalsIgnoreCase("DEAUTH"))        {
            // DEAUTH <ssid> [seconds] - attempt-once attack (plan Step 3b).
            // Blocks until done; device offline during execution.
            int sp2 = args.indexOf(' ');
            String ssid = (sp2>0)?args.substring(0,sp2):args;
            long secs = (sp2>0)?constrain(args.substring(sp2+1).toInt(),5,300):30;
            ssid.trim();
            logLine("[script:" + name + "] DEAUTH " + ssid + " for " + secs + "s");
            hw::screenOff();
            wifiattack::startDeauth(ssid, (uint32_t)secs);
            // startDeauth spawns a task; wait for completion so the script
            // sequence stays ordered
            while (wifiattack::attacking() && !g_stopRequested) delay(200);
            hw::screenOn();
        }
        else if (cmd.equalsIgnoreCase("PCAP_CAPTURE"))  {
            // PCAP_CAPTURE <seconds> [channel] - full-traffic promiscuous
            // capture to /pcap/<name>.pcap. (Plan said "NCM mode"; this
            // device has no Ethernet NIC - promiscuous WiFi capture is the
            // practical equivalent and is what the hardware can do.)
            int sp2 = args.indexOf(' ');
            long secs = (sp2>0)?constrain(args.substring(0,sp2).toInt(),5,600)
                               :constrain(args.toInt(),5,600);
            uint8_t ch = 1;
            int sp3 = (sp2>0)?args.indexOf(' ', sp2+1):-1;
            if (sp3>0) ch = constrain(args.substring(sp2+1,sp3).toInt(),1,13);
            else if (sp2>0) ch = constrain(args.substring(sp2+1).toInt(),1,13);
            logLine("[script:" + name + "] PCAP_CAPTURE " + secs + "s ch" + ch);
            hw::screenOff();
            String nm = "script_" + String(millis()/1000);
            wifiattack::startPcap(nm, (uint8_t)ch, (uint32_t)secs);
            hw::screenOn();
        }
        else if (cmd.equalsIgnoreCase("TRIGGER_KEY"))    {
            // TRIGGER_KEY <caps|num|scroll> <timeoutMs> <RUN|SKIP>
            // Waits for the HUMAN to press the chosen lock key (LED report
            // changes state). Branch on whether it happened:
            //   RUN  = continue if pressed (abort script if timeout)
            //   SKIP = skip the rest of the script if pressed... actually
            // inverted for usefulness: RUN waits for press then continues;
            // on timeout with RUN -> abort. SKIP = abort only if pressed.
            int sp2 = args.indexOf(' ');
            if (sp2 < 0) { res.error += "TRIGGER_KEY: lock timeout RUN|SKIP "; break; }
            String lock = args.substring(0, sp2); lock.trim(); lock.toLowerCase();
            String rest = args.substring(sp2+1); rest.trim();
            int sp3 = rest.indexOf(' ');
            uint32_t timeoutMs = constrain((long)rest.substring(0, sp3<0?rest.length():sp3).toInt(), 100, 3600000);
            String mode = (sp3<0) ? "RUN" : rest.substring(sp3+1); mode.trim(); mode.toUpperCase();
            if (mode != "RUN" && mode != "SKIP") mode = "RUN";
            logLine("[script:" + name + "] TRIGGER_KEY " + lock + " waiting " + timeoutMs + "ms (" + mode + ")");
            // wait for ANY state change of the chosen lock (host-side press)
            bool before = (lock=="num") ? detectos::numOn() :
                          (lock=="scroll") ? detectos::scrollOn() : detectos::capsOn();
            uint32_t t0 = millis();
            bool pressed = false;
            while (millis() - t0 < timeoutMs) {
                if (g_stopRequested) { res.ok=false; res.error="stopped"; break; }
                bool now = (lock=="num") ? detectos::numOn() :
                           (lock=="scroll") ? detectos::scrollOn() : detectos::capsOn();
                if (now != before) { pressed = true; break; }
                delay(40);
            }
            if (mode == "RUN" && !pressed) {
                res.ok = false; res.error = "TRIGGER_KEY timeout";
                logLine("[script:" + name + "] TRIGGER_KEY: timeout - aborting");
            } else if (mode == "SKIP" && pressed) {
                logLine("[script:" + name + "] TRIGGER_KEY: pressed - skipping rest");
                break;   // stop executing further lines, report finished
            } else {
                logLine("[script:" + name + "] TRIGGER_KEY: " + (pressed?"pressed":"timeout(skip)"));
            }
        }
        else if (cmd.equalsIgnoreCase("HOLD_KEY"))      {
            // HOLD_KEY <key> [ms] - hold for duration, or until RELEASE_KEY
            int sp2 = args.indexOf(' ');
            String key = (sp2>0)?args.substring(0,sp2):args;
            long ms = (sp2>0)?constrain(args.substring(sp2+1).toInt(),0,600000):0;
            uint8_t k;
            if (!resolveKey(key, k)) { res.error += "HOLD_KEY: unknown key '"+key+"' "; }
            else {
                kb.press(k);
                if (ms > 0) { delay(ms); kb.release(k); }
            }
        }
        else if (cmd.equalsIgnoreCase("RELEASE_KEY"))   {
            uint8_t k;
            if (resolveKey(args, k)) kb.release(k);
            kb.releaseAll();   // safety: never leave stuck keys
        }
        else if (cmd.equalsIgnoreCase("MOUSE_CLICK"))   {
            String b = args; b.trim(); b.toLowerCase();
            if (b == "double") {
                for (int c2=0;c2<2;c2++){ mouse.press(MOUSE_LEFT); delay(15); mouse.release(MOUSE_LEFT); delay(40); }
            } else {
                uint8_t btn = (b=="right")?MOUSE_RIGHT:(b=="middle")?MOUSE_MIDDLE:MOUSE_LEFT;
                mouse.press(btn); delay(25); mouse.release(btn);
            }
        }
        else if (cmd.equalsIgnoreCase("MOUSE_MOVE_SMOOTH")) {
            // MOUSE_MOVE_SMOOTH <x> <y> [ms] - eased human-like glide
            int sp2 = args.indexOf(' ');
            int sp3 = (sp2>0)?args.indexOf(' ', sp2+1):-1;
            long tx = (sp2>0)?args.substring(0,sp2).toInt():0;
            long ty = (sp3>0)?args.substring(sp2+1,sp3).toInt():args.substring(sp2+1).toInt();
            long ms = (sp3>0)?constrain(args.substring(sp3+1).toInt(),50,10000):400;
            static int lastX=0, lastY=0;   // persists across calls (absolute targets)
            int steps = constrain((int)(ms/16), 4, 60);   // ~60fps
            for (int s2=1; s2<=steps && !g_stopRequested; s2++) {
                float t = (float)s2/steps;
                float ease = t*t*(3-2*t);                  // smoothstep
                int nx = (int)(tx*ease), ny = (int)(ty*ease);
                mouse.move(nx-lastX, ny-lastY);
                lastX=nx; lastY=ny;
                delay(ms/steps);
            }
        }
        else if (cmd.equalsIgnoreCase("SCREEN_STATS"))  {
            // Live status render: script, line, RAM, IPs
            String ips = (WiFi.status()==WL_CONNECTED) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
            hw::screenOn();
            String t = FW_NAME "\nv" FW_VERSION "  " + String(getCpuFrequencyMhz()) + "MHz\n"
                "RAM " + String(ESP.getFreeHeap()/1024) + "KB\n"
                "IP " + ips + "\n"
                "[" + name + "]";
            hw::screenText(t);
            logLine("[script:" + name + "] SCREEN_STATS shown");
        }
        else if (cmd.equalsIgnoreCase("ON_ERROR"))      {
            String pol = args; pol.trim(); pol.toUpperCase();
            if (pol.startsWith("JUMP ")) {
                onError = "JUMP";
                onErrorTarget = pol.substring(5); onErrorTarget.trim();
            } else if (pol=="STOP" || pol=="CONTINUE") {
                onError = pol; onErrorTarget = "";
            } else res.error += "ON_ERROR: CONTINUE|STOP|JUMP label ";
        }
        else if (cmd.equalsIgnoreCase("VAR"))           {
            // VAR name value... - value may contain value-commands ($ vars too)
            int sp2 = args.indexOf(' ');
            if (sp2 < 0) { res.error += "VAR: needs name + value "; }
            else {
                String nm = args.substring(0, sp2), val = args.substring(sp2+1);
                setVar(nm, substValues(val));
                logLine("[script:" + name + "] $" + nm + " = " + getVar(nm));
            }
        }
        else if (cmd.equalsIgnoreCase("GET_IP"))        {
            // Types our IP so scripts can exfil it to the user/host screen.
            IPAddress ip = WiFi.localIP();
            String s = (WiFi.status()==WL_CONNECTED) ? ip.toString() : WiFi.softAPIP().toString();
            typeString(s);
        }
        else if (cmd.equalsIgnoreCase("WAIT_BUTTON"))   {
            // WAIT_BUTTON [secs] [CONTINUE|STOP]
            long secs = 30; String mode = "CONTINUE";
            int sp2 = args.indexOf(' ');
            if (sp2 > 0) { secs = constrain(args.substring(0,sp2).toInt(),1,3600); mode = args.substring(sp2+1); mode.trim(); }
            else if (args.length()) secs = constrain(args.toInt(),1,3600);
            bool pressed = hw::buttonWait(secs * 1000);
            if (!pressed && mode.equalsIgnoreCase("STOP")) {
                res.ok = false; res.error = "WAIT_BUTTON timeout(STOP)";
                break;
            }
        }
        else if (cmd.equalsIgnoreCase("JIGGLE_MOUSE"))  {
            long secs = constrain((long)(args.toFloat()), 1, 600);
            uint32_t end = millis() + secs * 1000;
            while (millis() < end && !g_stopRequested) {
                mouse.move((esp_random()%3)-1, (esp_random()%3)-1);  // -1..1 px
                delay(500);
            }
        }
        else if (cmd.equalsIgnoreCase("CONNECT_AP"))    {
            // CONNECT_AP ssid [password]
            int sp2 = args.indexOf(' ');
            String ssid = (sp2>0)?args.substring(0,sp2):args;
            String pass = (sp2>0)?args.substring(sp2+1):String("");
            ssid.trim(); pass.trim();
            WiFi.mode(WIFI_AP_STA);                      // keep our AP alive too
            // AP+STA stability: modem power-save + softAP is a known drop
            // source; disable sleep and let the stack auto-reconnect.
            WiFi.setSleep(WIFI_PS_NONE);
            WiFi.setAutoReconnect(true);
            WiFi.persistent(false);
            WiFi.begin(ssid.c_str(), pass.c_str());
            int tries = 0;
            while (WiFi.status()!=WL_CONNECTED && tries++<30 && !g_stopRequested) delay(500);
            bool ok = WiFi.status()==WL_CONNECTED;
            logLine("[script:" + name + "] CONNECT_AP '" + ssid + "' " +
                    (ok ? "connected "+WiFi.localIP().toString() : "FAILED"));
            if (ok) {
                // Confirm stability: if it drops within 5s, report it
                delay(5000);
                if (WiFi.status()!=WL_CONNECTED)
                    logLine("[script:" + name + "] CONNECT_AP: connection unstable - check channel/signal");
            }
        }
        else if (cmd.equalsIgnoreCase("USB_STORAGE")) {
            // USB_STORAGE enable|disable - toggles the SD-over-USB interface.
            String mode = args; mode.trim(); mode.toLowerCase();
            if (mode.startsWith("enable")) {
                msc::setStorageEnabled(true);
                logLine("USB_STORAGE enabled - re-enumerating");
                delay(200); usb_persist_restart(RESTART_PERSIST);
            } else if (mode.startsWith("disable")) {
                msc::setStorageEnabled(false);
                logLine("USB_STORAGE disabled - re-enumerating");
                delay(200); usb_persist_restart(RESTART_PERSIST);
            } else if (mode.startsWith("readonly")) {
                msc::setStorageEnabled(true);
                logLine("USB_STORAGE readonly set - applies next plug-in");
            } else {
                res.error += "USB_STORAGE: use enable/disable/readonly ";
            }
        }
        else if (cmd.equalsIgnoreCase("BRUTEFORCE_PIN")) {
            // BRUTEFORCE_PIN <len> [delayMs] - types 0000..9999 style codes
            int len = constrain(args.toInt(), 1, 8);
            int delayMs = 500;
            int sp2 = args.indexOf(' ');
            if (sp2 > 0) delayMs = constrain(args.substring(sp2+1).toInt(), 50, 60000);
            String fmt = "%0" + String(len) + "u";
            logLine("[script:" + name + "] PIN bruteforce len=" + String(len));
            char buf[12];
            for (uint32_t pin = 0; pin < (uint32_t)(pow(10, len)); pin++) {
                if (g_stopRequested) { res.ok = false; res.error = "stopped"; break; }
                snprintf(buf, sizeof(buf), fmt.c_str(), pin);
                typeString(buf);
                kb.press(KEY_RETURN); kb.release(KEY_RETURN);
                delay(delayMs);
                // backspace over the typed code + ENTER for the next attempt
                for (int b2 = 0; b2 < len+1; b2++) {
                    kb.press(KEY_BACKSPACE); kb.release(KEY_BACKSPACE); delay(10);
                }
                if (pin % 100 == 0) logLine("[script:" + name + "] PIN " + String(pin));
            }
        }
        else if (cmd.equalsIgnoreCase("BRUTEFORCE_LOGIN")) {
            // BRUTEFORCE_LOGIN /path/file.txt - lines "user:pass" or "user,pass"
            String path = args; path.trim();
            File f = SD_MMC.open(path, FILE_READ);
            if (!f) { res.error += "BRUTEFORCE_LOGIN: cannot open " + path + " "; }
            else {
                while (f.available() && !g_stopRequested) {
                    String line = f.readStringUntil('\n');
                    line.trim(); line.replace("\r","");
                    if (!line.length()) continue;
                    int sep = line.indexOf(':');
                    if (sep < 0) sep = line.indexOf(',');
                    if (sep < 0) continue;
                    String user = line.substring(0, sep);
                    String pass = line.substring(sep+1);
                    typeString(user); delay(100);
                    kb.press(KEY_TAB); kb.release(KEY_TAB); delay(100);
                    typeString(pass); delay(100);
                    kb.press(KEY_RETURN); kb.release(KEY_RETURN);
                    delay(800);
                    logLine("[script:" + name + "] tried " + user);
                }
                f.close();
            }
        }
        else if (cmd.equalsIgnoreCase("TUNNEL")) {
            // TUNNEL ON|OFF - enable/disable external relay access
            if (args.startsWith("ON"))  { tunnel::setEnabled(true);  }
            else if (args.startsWith("OFF")) { tunnel::setEnabled(false); }
            else res.error += "TUNNEL: use ON or OFF ";
        }
        else if (cmd.equalsIgnoreCase("SELF_DESTRUCT")) {
            logLine("script triggered SELF DESTRUCT");
            delay(300);
            sys::selfDestruct();
        }
        else if (cmd.equalsIgnoreCase("RESET_FIRM"))    {
            configFactoryReset();
            logLine("script requested firmware reset");
            delay(300);
            ESP.restart();
        }
        else {
            // combo / special-key line ("GUI r", "ENTER", ...)
            uint8_t k;
            if (resolveKey(cmd, k)) pressCombo(L.cmd + (L.args.length()? " "+L.args : ""));
            else {
                executed = false;
                res.error += "L" + String(L.srcLine) + ":unknown '" + cmd + "' ";
                if (onError == "STOP") {
                    res.ok = false;
                    break;
                } else if (onError == "JUMP") {
                    // find LABEL <target>
                    bool found=false;
                    for (int j=i+1; j<(int)lines.size(); j++) {
                        if (lines[j].cmd.equalsIgnoreCase("LABEL") &&
                            lines[j].args.equalsIgnoreCase(onErrorTarget)) { i=j+1; found=true; break; }
                    }
                    if (found) continue;
                    res.ok = false;   // label missing
                    res.error += "label '" + onErrorTarget + "' not found ";
                    break;
                }
            }
        }
        if (executed) res.linesRun++;
        if (defaultDelay && executed) delay(defaultDelay);
        if (!executed) res.linesRun--;   // don't count failures as executed lines
        // LABEL lines are no-ops (targets for ON_ERROR JUMP)
        if (cmd.equalsIgnoreCase("LABEL")) { /* no-op */ }
        i++;
    }

    g_running = false;
    s_state = "FINISHED";
    g_state.scriptState = ScriptState::FINISHED;
    g_state.scriptStateSince = time(nullptr);
    logLine(String("script ") + name + (res.ok ? " finished" : " stopped: " + res.error));
    return res;
}

void stop() { g_stopRequested = true; }

void initOnce() {
    if (!kbStarted) {
        // Identity spoofing must be applied BEFORE the single USB.begin() -
        // descriptors are read at enumeration time only.
        spoof::applyToUsb();
        // USB_STORAGE: expose the SD card alongside HID when enabled.
        if (msc::storageEnabled()) msc::beginCard(false);
        kb.begin();
        mouse.begin();
        USB.begin();       // single call - composite HID keyboard+mouse device
        kbStarted = true;
    }
}

bool isRunning() { return g_running; }
String stateString() { return s_state; }

// ---- HID control implementations ----
static uint8_t heldModifiers = 0;

std::vector<String> MODIFIER_NAMES() {
    return {"GUI","WINDOWS","COMMAND","CTRL","CONTROL","ALT","ALTGR","SHIFT"};
}
bool isModifierName(const String& n) {
    for (auto& m : MODIFIER_NAMES()) if (n.equalsIgnoreCase(m)) return true;
    return false;
}
void hidKey(const String& keyName, bool down) {
    uint8_t k;
    if (!resolveKey(keyName, k)) return;
    if (isModifierName(keyName)) return;   // use hidModifier for stickiness
    if (down) kb.press(k); else kb.release(k);
}
void hidModifier(const String& name, bool down) {
    uint8_t k;
    if (lookup(MODS, sizeof(MODS)/sizeof(MODS[0]), name, k))
        down ? kb.press(k) : kb.release(k);
}
void hidMouseMove(int dx, int dy)      { mouse.move(dx, dy); }
void hidMouseButton(const String& b, bool down) {
    if      (b=="left")   down ? mouse.press(MOUSE_LEFT)   : mouse.release(MOUSE_LEFT);
    else if (b=="right")  down ? mouse.press(MOUSE_RIGHT)  : mouse.release(MOUSE_RIGHT);
    else if (b=="middle") down ? mouse.press(MOUSE_MIDDLE) : mouse.release(MOUSE_MIDDLE);
}
void hidMouseScroll(int clicks)        { mouse.move(0,0,clicks); }

} // namespace ducky
