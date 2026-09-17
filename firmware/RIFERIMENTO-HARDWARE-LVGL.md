# Riferimento Hardware & LVGL — Gadget Valorant

Estratto dal progetto già funzionante (`../esp32_controller/`). **Questa è la configurazione del display testata e validata**: va usata come base del firmware del gadget per non ripetere la fatica di azzeccare i colori.

---

## 0. ⭐ TL;DR — cosa fa funzionare i colori

La scheda ha un display **AXS15231B su QSPI**. Ciò che dà **colori corretti** (dopo molti tentativi) è:

1. `Arduino_Canvas` (framebuffer in RAM/PSRAM) creato con **`rotation = 0`** — commento nel codice originale: *"Canvas rotation=0 (perfect colors)"*.
2. La **rotazione in orizzontale è fatta manualmente nel flush** (ruota di 270° in senso orario, USB a sinistra), copiando i pixel dal buffer di LVGL nel framebuffer del Canvas.
3. L'invio al pannello è `gfx->flush()` (spinge l'intero Canvas via QSPI).

> ❌ **NON** usare l'approccio di `display.h.bak` (Canvas `rotation=1` + `draw16bitRGBBitmap` nel flush): è quello che dava **colori sbagliati**. La versione corretta è quella di `esp32_controller.ino`.

Perché funziona: `Arduino_AXS15231B` si aspetta il framebuffer nel formato nativo (verticale 320×480) senza rotazione interna; ruotare tramite il Canvas rimescolava l'ordine dei byte/colori. Tenendo il Canvas a `rotation=0` e ruotando "a mano" nel flush, i pixel RGB565 arrivano nell'ordine giusto.

---

## 1. Scheda & display

| | |
|---|---|
| Scheda | **Guition JC4832W535** (ESP32‑S3) |
| Flash / PSRAM | 16 MB flash · **8 MB PSRAM OPI** (obbligatoria) |
| Display | **AXS15231B**, interfaccia **QSPI**, **480×320** orizzontale (nativo 320×480 verticale) |
| Touch | **AXS15231B capacitivo**, I²C, addr `0x3B` |
| USB | CDC nativo (Serial direttamente sulla USB) |

## 2. Pin (`config.h`)

```
// Display QSPI (AXS15231B)
TFT_CS=45  TFT_SCK=47  TFT_SDA0=21  TFT_SDA1=48  TFT_SDA2=40  TFT_SDA3=39
TFT_BL=1 (backlight, HIGH=on)   TFT_TE=38
QSPI_FREQ = 40 MHz

// Touch I2C (AXS15231B)
TOUCH_SDA=4  TOUCH_SCL=8  TOUCH_INT=3  TOUCH_ADDR=0x3B

// SD Card SPI (probabilmente non la useremo — abbiamo LittleFS)
SD_CS=10  SD_MOSI=11  SD_SCLK=12  SD_MISO=13

SCREEN_WIDTH=480  SCREEN_HEIGHT=320
```

## 3. Librerie (versioni testate)

| Lib | Versione | Uso |
|---|---|---|
| `esp32` (core) | **3.3.11** | toolchain ESP32‑S3 |
| `GFX Library for Arduino` (Arduino_GFX) | **1.6.5** | driver QSPI + Canvas (`Arduino_ESP32QSPI`, `Arduino_AXS15231B`, `Arduino_Canvas`) |
| `lvgl` | **9.2.2** | UI (API v9: `lv_display_create`, ecc.) |
| `ArduinoJson` | **7.2.0** | parsing del payload del backend |
| `LittleFS`, `WiFi`, `Preferences` | builtin (core) | asset / rete / config NVS |

> È installata anche `LovyanGFX`, ma **non viene usata** su questa scheda: si usa Arduino_GFX.

---

## 4. ⭐ Inizializzazione del display (codice funzionante)

```cpp
#include <Arduino_GFX_Library.h>
#include <lvgl.h>

Arduino_Canvas *gfx = nullptr;
static uint16_t *canvas_fb = nullptr;   // framebuffer del Canvas (320x480)

// --- setup() ---
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
Arduino_GFX *g = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, 320, 480);
gfx = new Arduino_Canvas(320, 480, g, 0, 0, 0);   // ⭐ rotation = 0

if (!gfx->begin(40000000UL)) { /* FATAL */ }
gfx->fillScreen(0x0000);
gfx->flush();
canvas_fb = gfx->getFramebuffer();                 // ⭐ accesso diretto al FB

pinMode(TFT_BL, OUTPUT);
digitalWrite(TFT_BL, HIGH);                         // backlight ON
```

### Flush callback (rotazione manuale 270° in senso orario — USB a sinistra)

```cpp
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint16_t *src = (uint16_t *)px_map;
    for (int ly = 0; ly < SCREEN_HEIGHT; ly++) {        // 320
        uint16_t *src_row = src + ly * SCREEN_WIDTH;    // 480
        for (int lx = 0; lx < SCREEN_WIDTH; lx++) {
            canvas_fb[(479 - lx) * 320 + ly] = src_row[lx];  // ruota di 270° CW
        }
    }
    gfx->flush();
    lv_disp_flush_ready(disp);
}
```

> Questo flush usa il **render mode FULL** (LVGL consegna l'intero schermo in una volta). Per questo il buffer di LVGL ha la dimensione dello schermo intero (480×320) e risiede nella **PSRAM**.

---

## 5. LVGL 9 — setup

```cpp
lv_init();
lv_tick_set_cb([]() -> uint32_t { return millis(); });   // tick tramite millis()

uint32_t bufSize = SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(lv_color_t); // 480*320*2
lv_color_t *buf = (lv_color_t *)heap_caps_malloc(
    bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);       // ⭐ PSRAM

lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT); // 480x320
lv_display_set_flush_cb(disp, disp_flush_cb);
lv_display_set_buffers(disp, buf, NULL, bufSize, LV_DISPLAY_RENDER_MODE_FULL);

lv_indev_t *indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, touch_read_cb);
```

Nel **loop**: `lv_task_handler();` + `delay(16);` (~60 fps). Aggiornare le schermate con dati live a intervalli (es. ogni 1 s).

### `lv_conf.h` (punti critici)
- `LV_COLOR_DEPTH 16` (RGB565).
- Abilitare i font Montserrat usati: 10, 12, 14, 18, 24, 28.
- `LV_USE_OS 0` (loop cooperativo) e tick tramite `lv_tick_set_cb` (vedi sopra).
- PSRAM: il buffer è allocato manualmente, quindi `LV_MEM_*` può restare al default (RAM interna) per il resto.

---

## 6. Touch (AXS15231B, I²C) — `touch.h`

- Driver custom (classe `AXS15231B_Touch`), I²C 400 kHz, INT su `FALLING`.
- Comando di lettura: `{0xB5,0xAB,0xA5,0x5A,0x00,0x00,0x00,0x08}` → legge 8 byte; X/Y nei byte 2–5.
- Istanziare con **rotation = 3** per l'orientamento "USB a sinistra":
  ```cpp
  AXS15231B_Touch touch_dev(TOUCH_SCL, TOUCH_SDA, TOUCH_INT, TOUCH_ADDR, 3);
  ```
- Mappatura (nativo verticale 320×480):
  ```
  rot 1 (orizz.):    x = raw_y;        y = 319 - raw_x
  rot 3 (USB-sin.):  x = 479 - raw_y;  y = raw_x
  ```

## 7. ⚠️ Orientamento — display e touch DEVONO combaciare

USB a sinistra → **il display ruota di 270° in senso orario nel flush** (`479 - lx`) **e** **touch rotation = 3**. Se se ne inverte uno senza l'altro, il tocco risulta specchiato/scambiato. Tenerli sempre in coppia.

---

## 8. WiFi — riutilizzare `wifi_manager.h` (quasi così com'è)

Classe `WiFiManager` pronta, memorizza fino a **3 reti nella NVS** (`Preferences` namespace `"wifi"`):

```cpp
g_wifi.begin();                       // carica le reti salvate (NVS)
bool ok = g_wifi.autoConnect(5000);   // prova quelle salvate, 5 s ciascuna
// se fallisce → schermata di config:
int n = g_wifi.scanNetworks(results, max);          // elenca le reti
g_wifi.connectTo(ssid, pass, 15000);  // connette E salva nella NVS (la porta in cima)
g_wifi.getIP(); g_wifi.getSSID(); g_wifi.getSavedSSID();
```

**Flusso nel firmware del gadget:** al boot, `begin()` → `autoConnect()`. Se non si connette, aprire la schermata WiFi (scan + tastiera LVGL per la password) → `connectTo()`. Riutilizzare `ui_wifi.cpp` come base della schermata.

## 9. Client HTTP — adattare `api_client.h`

Schema del riferimento (da mantenere): `HTTPClient` + `WiFiClientSecure` con `setInsecure()` per **HTTPS**, parsing con `ArduinoJson`.

**Differenza per il nostro gadget:** non c'è login email/password+JWT. Il gadget fa soltanto:
```cpp
GET https://<backend>/api/v1/gadget/dashboard
Header: Authorization: Bearer <DEVICE_TOKEN>   // il codice breve tipo "SV76-7KX2"
```
e fa il parsing del payload snello (vedi `../docs/03-CONTRATO-API.md`). Il device token viene digitato una sola volta sul gadget e salvato nella **NVS** (stesso schema `Preferences` del WiFi).

---

## 10. Build & upload (arduino-cli)

- **arduino-cli 1.4.1**, core **esp32:esp32 3.3.11** già installati.
- **Scheda collegata:** `/dev/cu.usbmodem101`.
- `partitions.csv` (16 MB): app0 2 MB + spiffs ~14 MB (LittleFS monta la partizione "spiffs").

FQBN consigliato (ESP32‑S3 + **PSRAM OPI** + 16 MB + partizione custom + **USB CDC**):
```bash
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc,USBMode=hwcdc,FlashMode=qio"

arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload  --fqbn "$FQBN" -p /dev/cu.usbmodem101 .
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```
> `PartitionScheme=custom` usa il `partitions.csv` della cartella dello sketch. **PSRAM=opi è obbligatorio** (il framebuffer 480×320×2 ≈ 300 KB + il buffer LVGL non entrano nella RAM interna). Verificare i nomi esatti delle opzioni dell'FQBN con `arduino-cli board details --fqbn esp32:esp32:esp32s3` al momento della compilazione.

---

## 11. Sequenza di implementazione del firmware suggerita (Valorant)

> Ordine pensato per validare presto la parte difficile (display/colori/touch) e poi aggiungere il resto.

- **Fase 0 — Scheletro:** struttura dello sketch in `firmware/`, copiare `config.h` (pin), `touch.h`, `wifi_manager.h` dal riferimento; `lv_conf.h` con depth 16 + font.
- **Fase 1 — Bring-up (CRITICO):** ✅ **FATTO e validato sull'hardware** — sketch in `firmware/bringup/` (colori, orientamento USB a sinistra e touch 100% OK). È la base nota-funzionante da cui copiare la config di display/touch.
- **Fase 2 — Base LVGL + tema Valorant:** stili (colori `#0b0b18`/`#7c3aed`/`#f43f5e`), helper per label/pannello/pulsante (rispecchiare `ui_theme.h`).
- **Fase 3 — WiFi:** `autoConnect` + schermata di configurazione (scan + tastiera) riutilizzando `wifi_manager.h`/`ui_wifi.cpp`.
- **Fase 4 — Device token:** schermata di onboarding sul gadget (tastiera LVGL) → salva il codice breve nella NVS.
- **Fase 5 — API client:** `GET /gadget/dashboard` con il token → parsing del payload con `ArduinoJson`.
- **Fase 6 — Schermata Dashboard:** elo (immagine per `rank.id`), `lv_bar` degli RR, tendenza. Con dati reali.
- **Fase 7 — Navigazione + schermate 2–4:** `lv_tabview` con swipe → Cronologia, Grafico (`lv_chart`), Stato.
- **Fase 8 — Asset + stati:** icone di elo/agente su LittleFS (per id); caricamento / errore / "non aggiornato" (stale).
- **Fase 9 — Polling + extra:** aggiornare a ogni `poll` del payload; LED WS2812B opzionale (colore dell'elo / post-partita).

## 12. Adattamenti rispetto al riferimento (roaster)

| Riferimento (esp32_controller) | Gadget Valorant |
|---|---|
| BLE (`ble_roaster.h`) | ❌ rimuovere (non usato) |
| SD Card (`sd_storage.h`) | usare **LittleFS** (asset) + **NVS** (config) |
| Auth: email/password → JWT | **device token** (Bearer), senza login |
| Tema arancione industriale | **viola/rosso** (palette Valorant) |
| Schermate di tostatura/profili | Dashboard / Cronologia / Grafico / Stato |

## 13. Trappole (da ricordare)

- 🔴 **PSRAM OPI obbligatoria** — senza, l'`heap_caps_malloc` del framebuffer fallisce e la scheda si blocca al boot.
- 🔴 **Usare lo schema del `.ino`** (Canvas rot=0 + rotazione manuale nel flush), **non** quello del `.bak` — altrimenti colori sbagliati.
- 🔴 **USB CDC On Boot** abilitato, altrimenti `Serial` non compare sulla USB nativa.
- 🟡 Display e touch devono avere lo **stesso orientamento** (270°/rot=3 per USB a sinistra).
- 🟡 LVGL **9.2.x** — API nuova (`lv_display_create`, `lv_tick_set_cb`); gli esempi vecchi (v8) non compilano.

## 14. Trucchi e insidie di LVGL 9.2 (imparati nel redesign v2.x)

> Scoperti sul campo durante il redesign del 2026-07 (indicatore segmentato, sprite
> ufficiali, animazioni di soglia, i18n). Valgono per qualsiasi firmware di questa scheda.

- 🔴 **I font Montserrat integrati contengono solo ASCII + `°` (0xB0) + bullet `•` (0x2022).**
  Niente accenti né lineetta lunga (—): vengono resi come glifo mancante. Scrivere la UI senza
  accenti e usare il bullet UTF-8 (`"\xE2\x80\xA2"`) come separatore visivo.
- 🔴 **I prototipi automatici del `.ino` si rompono con i tipi personalizzati**: una funzione che
  restituisce `MeuStruct*` riceve un prototipo generato IN CIMA al file, prima della definizione dello
  struct → errore `'MeuStruct' does not name a type`. Soluzione: restituire un indice `int`
  (vedi `day_slot()` nello sketch) oppure usare solo tipi già noti (`lv_obj_t*` ecc.).
- 🟢 **Immagini integrate (ARGB8888)**: `lv_image_dsc_t` nella 9.2 accetta l'init posizionale
  `{{LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_ARGB8888, 0, w, h, w*4, 0}, size, data}`
  (ordine little-endian verificato in `src/draw/lv_image_dsc.h`). Byte per pixel
  nell'ordine **B,G,R,A**. Pipeline: `tools/gen_logo_assets.py` (rsvg-convert + Pillow)
  rasterizza l'SVG → `logo_assets.h`.
- 🟢 **`lv_obj_set_style_image_recolor(+_opa)`** tinge lo sprite a runtime — usato per
  rendere Clawd grigio nello stato di errore senza generare un secondo asset.
- 🟡 **Animazioni procedurali nel `loop()` > `lv_anim`** per overlay che possono sparire in
  qualsiasi momento (il tocco li chiude): `lv_anim` con `exec_cb` custom che punta a oggetti
  eliminati = crash. Schema del firmware: tenere i puntatori in uno struct, animare in base al tempo
  (`millis()`) ed eliminare tutto insieme (`lv_obj_delete(scrim)`).
- 🟡 **Gli overlay devono vivere in `lv_layer_top()`** (sopravvivono agli aggiornamenti della schermata), ma
  vanno ripuliti manualmente a ogni cambio di stato (`lv_obj_clean(lv_layer_top())`
  in `render_state()`), altrimenti trapelano da una schermata all'altra.
- 🟡 **Area di tocco piccola**: oltre a ingrandire il pulsante, usare
  `lv_obj_set_ext_click_area(btn, px)` — l'ingranaggio dell'header usa 58×40 + 12px extra.
- 🟢 **i18n economico**: macro `#define TRS(pt, en) (g_lang ? (en) : (pt))` inline in ogni
  stringa (senza una tabella parallela che possa disallinearsi). Cambiare lingua = salvare in NVS +
  `request_state()` per ricostruire la schermata corrente.
- 🟢 **Gli occhi del Clawd ufficiale sono buchi trasparenti nel path** — su sfondo scuro
  sembrano già occhi; le espressioni diventano overlay (palpebre/X) posizionati tramite i define
  `CLAWD_*_EYE*` emessi dal generatore di asset.
