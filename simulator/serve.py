#!/usr/bin/env python3
"""Avvia il simulatore su http://127.0.0.1:8480/simulator/ senza cache del browser
(ogni modifica a sim.js si vede con un semplice ricarica)."""
import functools
import http.server
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 8480


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    handler = functools.partial(NoCache, directory=ROOT)
    print(f"Simulatore: http://127.0.0.1:{PORT}/simulator/")
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler).serve_forever()
