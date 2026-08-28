#!/usr/bin/env python3
"""
LilyStrike external-access relay server (Step 4).
Host this on any VPS / home server with a public IP. The device polls /pull
for pending requests and posts responses to /push; your browser uses
/t/<token>/<path> as a transparent reverse proxy to the device.

Run:  pip install flask && python3 relay_server.py
Set the device's External Access settings to:
  URL:   http://<your-server>:5000
  Token: same value you use in the browser URL below
Then browse:  http://<your-server>:5000/t/<token>/   -> device web UI
"""
import base64, threading, time
from flask import Flask, request, Response

app = Flask(__name__)
lock = threading.Condition()
jobs = {}       # token -> queue of pending requests
resp = {}       # token -> latest response
PULL_TIMEOUT = 30  # seconds browser waits for the device

def pull():
    tok = request.headers.get("X-Token", "")
    with lock:
        if tok not in jobs:
            jobs[tok] = []
        if not jobs[tok]:
            lock.wait_for(lambda: jobs.get(tok), timeout=1.0)
        if not jobs.get(tok):
            return "", 204
        return jobs[tok].pop(0)

def push():
    tok = request.headers.get("X-Token", "")
    with lock:
        resp[tok] = request.get_json(force=True)
        lock.notify_all()
    return "ok"

@app.route("/pull", methods=["GET"])
def _pull(): return pull()

@app.route("/push", methods=["POST"])
def _push(): return push()

@app.route("/t/<token>/<path:sub>", methods=["GET","POST","PUT","DELETE"])
@app.route("/t/<token>/", defaults={"sub": ""}, methods=["GET","POST","PUT","DELETE"])
def tunnel(token, sub):
    qs = ("?" + request.query_string.decode()) if request.query_string else ""
    job = {
        "method": request.method,
        "path": "/" + sub + qs,
        "body": base64.b64encode(request.get_data() or b"").decode(),
    }
    with lock:
        jobs.setdefault(token, []).append(job)
        lock.notify_all()
        # wait for the device to push a response
        deadline = time.time() + PULL_TIMEOUT
        resp_before = None
        while time.time() < deadline:
            lock.wait(0.5)
            if resp.get(token) and resp[token] is not resp_before:
                r = resp.pop(token, None)
                break
            resp_before = resp.get(token)
        else:
            return "device unreachable (tunnel timeout)", 504
    headers = {}
    for k, v in (r.get("headers") or {}).items():
        if k.lower() in ("content-type", "location", "set-cookie"):
            headers[k] = v
    body = base64.b64decode(r.get("body") or "")
    return Response(body, status=r.get("status", 200), headers=headers)

@app.route("/")
def index():
    return "LilyStrike relay is running. Use /t/&lt;token&gt;/ to reach a device."

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
