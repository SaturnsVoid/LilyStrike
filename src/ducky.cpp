// ============================================================================
// ducky.cpp - DuckyScript v3 interpreter (see ducky.h for supported syntax)
// ----------------------------------------------------------------------------
// USB HID: uses the core's TinyUSB-backed USBHIDKeyboard. The board def sets
// ARDUINO_USB_MODE=0 so the HID device enumerates as keyboard.
//
// Design notes:
//  * Interpreter is a straightforward line-at-a-time state machine; custom
//    commands from later steps plug in where marked [CUSTOM HOOK].
//  * `g_stopRequested` is polled between lines so the web UI can abort.
// ============================================================================
#include "ducky.h"
#include "config.h"
#include "crypt.h"
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <vector>

namespace ducky {

static USBHIDKeyboard kb;
static bool kbStarted = false;
static volatile bool g_running = false;
static volatile bool g_stopRequested = false;
static String s_state = "STANDBY";   // local mirror; authoritative state in config.h g_state

// ---------------------------------------------------------------- modifiers
struct KeyName { const char* name; uint8_t keymod; };
// Modifier keys map onto HID keycodes used by USBHIDKeyboard.
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
    // F1..F12
    {"F1", KEY_F1},{"F2", KEY_F2},{"F3", KEY_F3},{"F4", KEY_F4},{"F5", KEY_F5},
    {"F6", KEY_F6},{"F7", KEY_F7},{"F8", KEY_F8},{"F9", KEY_F9},{"F10", KEY_F10},
    {"F11", KEY_F11},{"F12", KEY_F12},
};

static bool lookup(const KeyName* table, size_t n, const String& k, uint8_t& out) {
    for (size_t i = 0; i < n; i++)
        if (k.equalsIgnoreCase(table[i].name)) { out = pgm_read_byte(&table[i].keymod); return true; }
    return false;
}
// Resolve a token to a keycode: single char, or named special/modifier.
static bool resolveKey(const String& tok, uint8_t& out) {
    if (lookup(MODS, sizeof(MODS)/sizeof(MODS[0]), tok, out)) return true;
    if (lookup(SPECIALS, sizeof(SPECIALS)/sizeof(SPECIALS[0]), tok, out)) return true;
    if (tok.length() == 1) { out = (uint8_t)tok[0]; return true; }
    return false;
}

// Press-and-release a combo like "GUI r" / "CTRL-SHIFT t".
static void pressCombo(const String& args) {
    uint8_t held = 0; std::vector<uint8_t> taps;
    int start = 0;
    while (start <= (int)args.length()) {
        int sp = args.indexOf(' ', start);
        String tok = (sp < 0) ? args.substring(start) : args.substring(start, sp);
        tok.trim();
        if (tok.length()) {
            uint8_t k;
            if (resolveKey(tok, k)) {
                if (lookup(MODS, sizeof(MODS)/sizeof(MODS[0]), tok, k)) kb.press(k), held |= 0; // press modifier
                else taps.push_back(k);
            }
        }
        if (sp < 0) break;
        start = sp + 1;
    }
    for (auto t : taps) { kb.press(t); delay(8); kb.release(t); }
    kb.releaseAll();
}

// Type a string verbatim (ASCII; unicode not needed in step 1).
static void typeString(const String& s) { kb.print(s); }

// ------------------------------------------------------------- interpreter
RunResult run(const String& scriptText, const String& name) {
    RunResult res{true, 0, ""};
    g_stopRequested = false;
    // NOTE: HID stack must already be up (ducky::initOnce from setup()).

    g_running = true;
    s_state = "RUNNING";
    // Publish state to the shared runtime status (Status page reads this).
    g_state.scriptState = ScriptState::RUNNING;
    g_state.scriptStateSince = time(nullptr);
    g_state.lastScriptName = name;

    int defaultDelay = 0;
    int repeatLast = 1;

    int lineNo = 0, idx = 0;
    String lastCmdLine;
    while (idx <= (int)scriptText.length()) {
        if (g_stopRequested) { res.ok = false; res.error = "stopped"; break; }
        int nl = scriptText.indexOf('\n', idx);
        String line = (nl < 0) ? scriptText.substring(idx)
                               : scriptText.substring(idx, nl);
        idx = nl + 1;
        lineNo++;
        line.trim();
        if (line.isEmpty() || line.startsWith("#")) continue;
        if (line.startsWith("REM")) {
            if (line.equalsIgnoreCase("REM_BLOCK_START") || line.equalsIgnoreCase("REM BLOCK START")) {
                // Skip until matching REM_BLOCK_END / END (DuckyScript v3 block comments)
                while (idx <= (int)scriptText.length()) {
                    int nl2 = scriptText.indexOf('\n', idx);
                    String blk = scriptText.substring(idx, nl2 < 0 ? scriptText.length() : nl2);
                    idx = nl2 + 1;
                    blk.trim();
                    if (blk.equalsIgnoreCase("REM_BLOCK_END") || blk.equalsIgnoreCase("END")) break;
                }
            }
            continue;
        }

        // split "CMD arg arg..." (first space)
        String cmd = line, args;
        int sp = line.indexOf(' ');
        if (sp > 0) { cmd = line.substring(0, sp); args = line.substring(sp + 1); }

        // ---- REPEAT: re-execute previous command N times ----
        int times = 1;
        if (cmd.equalsIgnoreCase("REPEAT")) {
            times = args.toInt(); if (times < 1) times = 1;
            if (lastCmdLine.isEmpty()) continue;
            line = lastCmdLine;
            sp = line.indexOf(' ');
            cmd = line.substring(0, (sp>0)?sp:(int)line.length());
            args = (sp>0)?line.substring(sp+1):String("");
        } else lastCmdLine = line;

        bool executed = true;
        if      (cmd.equalsIgnoreCase("DELAY"))         { delay(constrain(args.toInt(),0,60000)); }
        else if (cmd.equalsIgnoreCase("DEFAULTDELAY") ||
                 cmd.equalsIgnoreCase("DEFAULT_DELAY")) { defaultDelay = constrain(args.toInt(),0,60000); }
        else if (cmd.equalsIgnoreCase("STRING"))        { typeString(args); }
        else if (cmd.equalsIgnoreCase("STRINGLN"))      { typeString(args); kb.press(KEY_RETURN); kb.release(KEY_RETURN); }
        else if (cmd.equalsIgnoreCase("LOG"))           { logLine("[script:" + name + "] " + args); }
        else {
            // combo / special-key line ("GUI r", "ENTER", ...)
            uint8_t k;
            if (resolveKey(cmd, k)) pressCombo(line);
            else { executed = false; res.error += "L" + String(lineNo) + ":unknown '" + cmd + "' "; }
        }
        if (!executed && !res.error.isEmpty()) { /* keep running other lines */ }
        if (executed || !res.error.isEmpty()) res.linesRun++;
        if (defaultDelay && executed) delay(defaultDelay);
    }

    g_running = false;
    s_state = res.ok ? "FINISHED" : "FINISHED";   // aborted also reports FINISHED
    g_state.scriptState = ScriptState::FINISHED;
    g_state.scriptStateSince = time(nullptr);
    logLine(String("script ") + name + (res.ok ? " finished" : " stopped: " + res.error));
    return res;
}

void stop() { g_stopRequested = true; }

// Call ONCE from setup(): starts the TinyUSB stack with the HID keyboard.
// Doing this inside run() crashed the device because the core (with CDC on
// boot disabled) expects a single USB.begin() at startup.
void initOnce() {
    if (!kbStarted) { kb.begin(); USB.begin(); kbStarted = true; }
}

bool isRunning() { return g_running; }
String stateString() { return s_state; }

} // namespace ducky
