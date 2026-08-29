// ============================================================================
// mcp.h - Model Context Protocol server (Step 4, AI mode)
// ----------------------------------------------------------------------------
// Streamable-HTTP transport: POST /mcp with JSON-RPC 2.0 bodies. Lets an LLM
// (Claude Desktop, MCP clients) drive the device's tool surface:
//
//   device_status | run_script | stop_script | list_scripts | write_script
//   read_script | keystroke | mouse | wifi_scan | led | screen | system_config
//
// AUTH: header "X-MCP-Token: <token>". Token auto-generated on first GET of
// /api/mcptoken (Settings page shows it); override by POST {"token":"..."}.
//
// DESIGN RULES:
//   * tools/call only wraps FAST actions. run_script returns "started"
//     immediately - the agent polls device_status. No 10s timeouts.
//   * Destructive/stealth operations (self destruct, factory reset, EvilAP,
//     settings) are deliberately NOT exposed - the LLM asks the human who
//     then uses the web UI.
//   * Responses are compact JSON - token economy matters.
// ============================================================================
#pragma once
#include <Arduino.h>

namespace mcp {

void begin();                   // register routes (call from web::begin)
bool enabled();                 // MCP on/off (boot setting)
void setEnabled(bool on);       // persist enable/disable
String token();                 // current MCP token (generates if unset)
void setToken(const String& t); // override
uint32_t totalCalls();          // lifetime JSON-RPC calls received
bool everInitialized();         // an LLM has completed initialize at least once
uint32_t lastInitAgoMs();       // ms since last initialize (0xFFFFFFFF = never)

} // namespace mcp
