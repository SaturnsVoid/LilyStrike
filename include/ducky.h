// ============================================================================
// ducky.h - DuckyScript v3 interpreter + custom commands
// ----------------------------------------------------------------------------
// Step 1 supported syntax:
//   REM / REM_BLOCK ... , DELAY n, STRING, STRINGLN, ENTER, SPACE, TAB,
//   ESCAPE, DOWNARROW/UPARROW/LEFTARROW/RIGHTARROW, BACKSPACE, DELETE,
//   HOME, INSERT, PAGEUP/PAGEDOWN, CAPSLOCK, APP, F1..F12,
//   GUI/COMMAND/WINDOWS, CTRL/CONTROL, ALT, SHIFT, ALTGR (+ combos:
//   "GUI r", "CTRL-SHIFT esc"), REPEAT n (repeats last command line),
//   DEFAULTDELAY/DEFAULT_DELAY ms.
// Custom: LOG <message>  -> encrypted device debug log
// Future steps add DETECT_OS, LED_*, SCREEN_*, RANDOM_*, HUMAN_TYPE, ...
//
// Runs on its own FreeRTOS task so the web UI stays responsive while a
// script types. One script at a time (RubberDucky model).
// ============================================================================
#pragma once
#include <Arduino.h>

namespace ducky {

struct RunResult { bool ok; int linesRun; String error; };

// Execute script text. Blocks until finished or stopScript() is called.
// Intended to be called from the dedicated runner task, not loop().
RunResult run(const String& scriptText, const String& name);

// Ask the running script to abort (checked between every line).
void stop();

// Start USB HID stack once at boot. MUST be called before run().
void initOnce();
std::vector<String> layoutNames();
void setLayout(const String& name);   // applies to subsequent keystrokes

// ---- HID control (Control Page / future MCP mode) ----
void hidKey(const String& keyName, bool down);       // named key press/release
std::vector<String> MODIFIER_NAMES();                // ["GUI","CTRL","ALT","SHIFT","ALTGR"]
bool isModifierName(const String& n);
void hidModifier(const String& name, bool down);
void hidMouseMove(int dx, int dy);
void hidMouseButton(const String& b, bool down);     // "left","right","middle"
void hidMouseScroll(int clicks);

bool isRunning();
bool wasStopped();                  // stop() called since last run() began
String stateString();               // "STANDBY" / "RUNNING" / "FINISHED"

} // namespace ducky
