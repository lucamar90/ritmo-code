#!/usr/bin/env bash
#
# Build / upload / monitor del bring-up (Guition JC4832W535, ESP32-S3).
#
# Uso:
#   ./build.sh                 # compila
#   ./build.sh upload          # compila + flash (porta predefinita sotto)
#   ./build.sh upload <porta>  # compila + flash sulla porta indicata
#   ./build.sh monitor <porta> # apre il monitor seriale (115200)
#
# Due differenze rispetto al build.sh di claude_stick, entrambe necessarie:
#
#   1. PartitionScheme=huge_app (non `custom`): questa cartella non ha un proprio
#      partitions.csv — `custom` farebbe cercare al core un .csv qui e interrompere con
#      "cp: .../partitions/.csv: No such file or directory". Il bring-up non usa
#      né LittleFS né NVS, quindi lo schema predefinito basta.
#
#   2. Il -I punta a ../claude_stick e va messo ANCHE in compiler.S.extra_flags:
#      il bring-up non ha un proprio lv_conf.h e riutilizza quello del firmware (che
#      include già la guardia __ASSEMBLY__). Il core assembla i .S di LVGL con
#      compiler.S.extra_flags — c/cpp.extra_flags non arrivano a quella fase — e senza
#      il -I lì LVGL ricade sul fallback "../../lv_conf.h", un file isolato nella
#      cartella libraries di Arduino che può appartenere a qualsiasi altro progetto; un
#      #include <stdint.h> non protetto al suo interno rompe l'assemblaggio con
#      "unknown opcode or format name 'typedef'".
set -euo pipefail

SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LV_CONF_DIR="$(cd "${SKETCH_DIR}/../claude_stick" && pwd)"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app,CDCOnBoot=cdc,USBMode=hwcdc,FlashMode=qio"
PORT_DEFAULT="/dev/cu.usbmodem101"

LVFLAGS="-DLV_CONF_INCLUDE_SIMPLE -I${LV_CONF_DIR}"

cmd="${1:-build}"
port="${2:-$PORT_DEFAULT}"

case "$cmd" in
  monitor)
    exec arduino-cli monitor -p "$port" -c baudrate=115200
    ;;
  build)
    echo "==> compilazione bring-up ($FQBN)"
    arduino-cli compile \
      --fqbn "$FQBN" \
      --build-property "compiler.cpp.extra_flags=$LVFLAGS" \
      --build-property "compiler.c.extra_flags=$LVFLAGS" \
      --build-property "compiler.S.extra_flags=$LVFLAGS" \
      "$SKETCH_DIR"
    ;;
  upload)
    echo "==> compilazione + flash bring-up su $port ($FQBN)"
    arduino-cli compile \
      --fqbn "$FQBN" \
      --build-property "compiler.cpp.extra_flags=$LVFLAGS" \
      --build-property "compiler.c.extra_flags=$LVFLAGS" \
      --build-property "compiler.S.extra_flags=$LVFLAGS" \
      --upload -p "$port" \
      "$SKETCH_DIR"
    ;;
  *)
    echo "comando sconosciuto: $cmd (usa: build | upload | monitor)" >&2
    exit 1
    ;;
esac
