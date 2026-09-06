#!/usr/bin/env python3
"""Serve the LilyStrike web flasher on localhost (Web Serial needs a secure
context - localhost qualifies). Then open http://localhost:8722 in Chrome."""
import http.server, os, sys
os.chdir(os.path.dirname(os.path.abspath(__file__)))
print("LilyStrike flasher:  http://localhost:8722   (Ctrl+C to stop)")
http.server.HTTPServer(("127.0.0.1", 8722), http.server.SimpleHTTPRequestHandler).serve_forever()
