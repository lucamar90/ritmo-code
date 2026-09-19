#!/usr/bin/env bash
#
# flash.sh — carica la versione attuale del firmware su un Ritmo Code.
#
# Uso: collega lo schermo (Guition JC4832W535) alla USB ed esegui:
#     ./flash.sh
#
# Lo script trova la porta da solo (attende fino a 30s se non e' ancora collegato),
# compila e carica. Prerequisiti: arduino-cli + core esp32 3.3.11 + librerie
# (vedi firmware/RIFERIMENTO-HARDWARE-LVGL.md).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

if ! command -v arduino-cli >/dev/null; then
  echo "errore: arduino-cli non trovato (brew install arduino-cli)" >&2
  exit 1
fi

find_port() { ls /dev/cu.usbmodem* 2>/dev/null | head -1; }

PORT="$(find_port || true)"
if [ -z "$PORT" ]; then
  echo "==> nessuno schermo sulla USB — collega il dispositivo (attesa fino a 30s)..."
  for _ in $(seq 1 30); do
    sleep 1
    PORT="$(find_port || true)"
    [ -n "$PORT" ] && break
  done
fi
if [ -z "$PORT" ]; then
  echo "errore: non e' comparso nessun /dev/cu.usbmodem*. Lo schermo e' collegato e acceso?" >&2
  exit 1
fi

echo "==> schermo trovato su $PORT"
echo "==> compilazione e caricamento della versione attuale..."
firmware/ritmo_code/build.sh upload "$PORT"
echo
echo "==> fatto! Il dispositivo si riavvia da solo (chiedera' il PIN sullo schermo)."
