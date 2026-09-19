#!/usr/bin/env bash
#
# Build / upload / monitor di Ritmo Code (Guition JC4832W535, ESP32-S3).
#
# Uso:
#   ./build.sh                 # compila
#   ./build.sh upload          # compila + carica (porta predefinita sotto)
#   ./build.sh upload <porta>  # compila + carica sulla porta indicata
#   ./build.sh monitor <porta> # apre il monitor seriale (115200)
#   ./build.sh ota <ip> <cod>  # compila + carica via Wi-Fi (Impostazioni -> aggiorna firmware)
#
# Prerequisiti (vedi firmware/RIFERIMENTO-HARDWARE-LVGL.md):
#   - arduino-cli 1.4.x, core esp32:esp32 3.3.11
#   - librerie: GFX Library for Arduino 1.6.5, lvgl 9.2.2
#
# -DLV_CONF_INCLUDE_SIMPLE + -I<sketch> fanno trovare a LVGL il nostro lv_conf.h.
# Ripiego, se compare "lv_conf.h not found": copia lv_conf.h nella cartella
# libraries di Arduino (un livello sopra la cartella `lvgl`).
set -euo pipefail

SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc,USBMode=hwcdc,FlashMode=qio"
PORT_DEFAULT="/dev/cu.usbmodem101"

LVFLAGS="-DLV_CONF_INCLUDE_SIMPLE -I${SKETCH_DIR}"
# -O2 al posto del -Os del core (velocita' invece di dimensione): il rendering LVGL e'
# il lavoro piu' pesante della scheda. Va passato come compiler.optimization_flags:
# gli extra_flags finiscono PRIMA di -Os nella riga di compilazione e verrebbero ignorati.
OPTFLAGS="compiler.optimization_flags=-O2"

cmd="${1:-build}"
port="${2:-$PORT_DEFAULT}"

case "$cmd" in
  monitor)
    exec arduino-cli monitor -p "$port" -c baudrate=115200
    ;;
  build)
    echo "==> compilazione ($FQBN)"
    arduino-cli compile \
      --fqbn "$FQBN" \
      --build-property "compiler.cpp.extra_flags=$LVFLAGS" \
      --build-property "compiler.c.extra_flags=$LVFLAGS" \
      --build-property "$OPTFLAGS"       --export-binaries \
      "$SKETCH_DIR"
    ;;
  upload)
    # `compile --upload` compila e carica in un solo passo (upload da solo non accetta --build-property)
    echo "==> compilazione + caricamento su $port ($FQBN)"
    arduino-cli compile \
      --fqbn "$FQBN" \
      --build-property "compiler.cpp.extra_flags=$LVFLAGS" \
      --build-property "compiler.c.extra_flags=$LVFLAGS" \
      --build-property "$OPTFLAGS"       --export-binaries \
      --upload -p "$port" \
      "$SKETCH_DIR"
    ;;
  ota)
    # compila e carica via Wi-Fi: sul dispositivo apri Impostazioni -> aggiorna firmware
    # uso: ./build.sh ota <ip> <codice di 6 cifre mostrato sullo schermo>
    ip="${2:?indirizzo IP del dispositivo}"
    code="${3:?codice mostrato sullo schermo}"
    "$0" build
    bin="$SKETCH_DIR/build/esp32.esp32.esp32s3/ritmo_code.ino.bin"   # esportato da --export-binaries
    echo "==> caricando $bin su http://$ip/update"
    curl -fS --max-time 180 -F "firmware=@$bin" "http://$ip/update?code=$code"
    echo
    ;;
  *)
    echo "comando sconosciuto: $cmd (usa: build | upload | monitor | ota)" >&2
    exit 1
    ;;
esac
