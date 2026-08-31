# MCP / AI Integration

LilyStrike embeds a **Model Context Protocol** server, so an MCP client (Claude Desktop, any MCP-capable agent, or a plain HTTP client) can enumerate and drive the device with structured tools instead of scraping the web UI.

## Enabling

1. Web UI → **Settings → MCP**: toggle **enabled**, set a token (min 8 characters). Takes effect immediately — no reboot.
2. Point your MCP client at:
   - URL: `http://<device-ip>/mcp` (e.g. `http://192.168.12.209/mcp`)
   - Method: `POST`, body: JSON-RPC 2.0
   - Auth: header `X-MCP-Token: <token>` **or** query `?token=<token>` (header preferred — query strings end up in logs)

## Protocol

Standard JSON-RPC 2.0:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"device_status","arguments":{}}}
```

Errors are JSON-RPC error objects: `-32001` auth, `-32002` MCP disabled.

## The 12 tools

| Tool | What it does |
|---|---|
| `device_status` | RAM, uptime, CPU, IPs, WiFi + USB state, script state, detected OS |
| `run_script` | Start a saved script (`name`) **or** inline DuckyScript (`text`). Returns immediately; poll `device_status` for completion |
| `stop_script` | Abort the running script (also aborts an autostart chain) |
| `list_scripts` | Names of the encrypted scripts on the card |
| `write_script` | Save a script (encrypted) |
| `read_script` | Read a script's decrypted text |
| `keystroke` | Inject a raw keystroke/combos into the host |
| `mouse` | Inject mouse movements/clicks |
| `wifi_scan` | Run an AP scan, return results |
| `led` | Set/clear/blink the status LED |
| `screen` | Draw on the device LCD |
| `system_config` | Read/update settings (power mode, MAC spoof, MCP, tunnel, …) |

## Resources (Phase 2)

Read-only data the LLM can browse instead of calling tools:

| URI | Contents |
|---|---|
| `lilystrike://status` | Live status snapshot (JSON) |
| `lilystrike://logs/system` | Recent log lines (text) |
| `lilystrike://scripts` | JSON array of saved script names |
| `lilystrike://scripts/<name>` | Decrypted script text |
| `lilystrike://analyzer/last` | Last WiFi analyzer session (JSON) |

```json
{"jsonrpc":"2.0","id":1,"method":"resources/list"}
{"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"uri":"lilystrike://scripts/demo-chain.ds"}}
```

**Subscriptions** (pragmatic model): `resources/subscribe` / `unsubscribe` are
accepted, but the plain-POST transport can't push — clients instead poll:

```json
{"jsonrpc":"2.0","id":3,"method":"resources/poll","params":{"sinceVersion":1}}
→ {"version":12,"changed":["lilystrike://logs/system"]}
```

Bump hooks fire on log lines and script saves, so a client polling every few
seconds sees exactly which resources changed. Re-add `subscribe` semantics
natively when the transport moves to SSE/WebSocket.

## Prompts (Phase 2)

`prompts/list` → `triage_device`, `payload_author` (arg: `goal`),
`analyze_capture`. `prompts/get` returns ready-made user messages that walk
the LLM through reading the right resources first.

## Example: curl

```bash
TOKEN=[REDACTED]
DEV=192.168.4.1

# enumerate tools
curl -s -X POST http://$DEV/mcp \\
  -H "Content-Type: application/json" -H "X-MCP-Token: $TOKEN" \\
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'

# run an inline payload
curl -s -X POST http://$DEV/mcp \\
  -H "Content-Type: application/json" -H "X-MCP-Token: $TOKEN" \\
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"run_script",
       "arguments":{"text":"DELAY 500\\nGUI r\\nDELAY 300\\nSTRING notepad\\nENTER"}}}'

# poll until finished
curl -s -X POST http://$DEV/mcp \\
  -H "Content-Type: application/json" -H "X-MCP-Token: $TOKEN" \\
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"device_status","arguments":{}}}'
```

## Example: Claude Desktop

`claude_desktop_config.json` (requires an MCP-to-HTTP bridge such as `mcp-remote` or a small stdio shim):

```json
{
  "mcpServers": {
    "lilystrike": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "http://192.168.4.1/mcp",
               "--header", "X-MCP-Token: [REDACTED]"]
    }
  }
}
```

Then Claude can answer "what scripts are on the dongle?", "run hello_notepad", or "scan wifi and tell me what's around" directly.

## Agent workflow tips

- `run_script` returns immediately — poll `device_status` and watch `scriptState` (`RUNNING` → `FINISHED`).
- Prefer `write_script` + `run_script` over inline text for payloads you'll reuse.
- The AI sees `detectedOS` in status — a good first step is `DETECT_OS` inside a script, then branch.
- All MCP activity is authenticated, logged, and counted (`mcpCalls` in status).

## Security notes

- The token is the *only* gate — treat it like a password (Settings → MCP token, min 8 chars, regenerate any time).
- Prefer the header over the query string; the query form exists for dumb clients.
- MCP is **disabled by default**. When disabled, requests return a JSON-RPC `-32002` error; the route itself stays registered so enabling takes effect without a reboot.
- Anyone with the token can run HID payloads against the host — don't expose the device's port beyond networks you control. For remote access use the relay tunnel, which keeps the device's API off the wider network.
