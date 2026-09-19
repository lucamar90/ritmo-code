#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// Ritmo Code — Guition JC4832W535 (ESP32-S3, AXS15231B)
// Pin: vedi firmware/RIFERIMENTO-HARDWARE-LVGL.md (bring-up validato)
// ============================================================

// ── Firmware ─────────────────────────────────────────────
#define FW_VERSION              "3.9.4"
#define GITHUB_REPO             "lucamar90/ritmo-code"   // aggiornamenti: ultima release di GitHub

// ── Display QSPI (AXS15231B) ─────────────────────────────
#define TFT_CS    45
#define TFT_SCK   47
#define TFT_SDA0  21
#define TFT_SDA1  48
#define TFT_SDA2  40
#define TFT_SDA3  39
#define TFT_BL    1
#define TFT_TE    38

#define SCREEN_WIDTH   480
#define SCREEN_HEIGHT  320
// Provato 80 MHz sulla scheda (set 2026): immagine leggermente sdoppiata, il bus
// non regge. Il clock SPI dell'ESP32 non ha valori intermedi (80 / 40 / 26.7...),
// quindi 40 MHz e' il massimo stabile.
#define QSPI_FREQ      40000000UL

// ── Touch I2C (AXS15231B) ────────────────────────────────
#define TOUCH_SDA  4
#define TOUCH_SCL  8
#define TOUCH_INT  3
#define TOUCH_ADDR 0x3B
// rotation=3 = USB a sinistra (coerente con il flush 270° CW)
#define TOUCH_ROTATION 3

// ── Polling ──────────────────────────────────────────────
#define DEFAULT_POLL_SEC        120
#define MIN_POLL_SEC            30
#define MAX_POLL_SEC            1800     // fino a 30 min (meno richieste alla API)
#define STATUS_POLL_SEC         300      // status.claude.com ogni 5 min

// Home: citta' predefinita del meteo (modificabile dal browser su /home) e PC con Ritmo Code PC Monitor
#define DEFAULT_WX_CITY         "Milano"
#define DEFAULT_WX_LAT          45.4643f
#define DEFAULT_WX_LON          9.1895f
#define DEFAULT_PC_HOST         ""        // lo imposta l'app Ritmo Code PC Monitor ("Collega questo PC") o /home

// ── Sicurezza (PIN + AES-256-GCM) ────────────────────────
#define PIN_LEN                 4
#define MAX_PIN_ATTEMPTS        10
#define LOCKOUT_BASE_SEC        60       // raddoppia a ogni errore
#define KDF_ROUNDS              10000

// ── Rete / API Claude ────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS 8000
#define API_TIMEOUT_MS          15000
#define MESSAGES_ENDPOINT       "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION       "2023-06-01"
#define PROBE_MODEL             "claude-haiku-4-5-20251001"
// status.anthropic.com reindirizza qui — interrogare direttamente l'host canonico
#define STATUS_ENDPOINT         "https://status.claude.com/api/v2/incidents/unresolved.json"

// NTP (necessario per i contatori di reset)
#define NTP_SERVER_1            "pool.ntp.org"
#define NTP_SERVER_2            "time.cloudflare.com"

// ── NVS ──────────────────────────────────────────────────
#define NVS_NAMESPACE           "claude"

#endif // CONFIG_H
