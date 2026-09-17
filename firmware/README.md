# firmware/ — Ritmo Code (ESP32-S3 + LVGL)

Firmware per il display touch **Guition JC4832W535** (AXS15231B QSPI, 480×320).

- **`claude_stick/`** — il progetto. Sketch arduino-cli completo: recupera l'utilizzo di
  Claude (header di `api.anthropic.com`), lo stato dei modelli (`status.claude.com`)
  e visualizza tutto in LVGL 9 con navigazione touch. Token OAuth memorizzato cifrato
  (AES-256-GCM + PIN). Per la build vedi [`claude_stick/build.sh`](claude_stick/build.sh).
- **`bringup/`** — bring-up validato sull'hardware (colori corretti, orientamento
  con USB a sinistra e touch allineato). Riferimento noto-funzionante per la config di
  display/touch; non è l'app. Compila con il `build.sh` **di questa cartella**
  ([`bringup/build.sh`](bringup/build.sh)): usa `PartitionScheme=huge_app`
  (qui non c'è un `partitions.csv`) e riutilizza il `lv_conf.h` di `claude_stick`;
  l'FQBN del firmware non va bene per questa cartella.
- **`RIFERIMENTO-HARDWARE-LVGL.md`** — pin, librerie testate, pipeline di flush
  (rotazione di 270° in senso orario fatta a mano) e trappole (PSRAM OPI obbligatoria, ecc.).

Parti da [`../README.md`](../README.md) per la panoramica generale e la procedura di build passo passo.
