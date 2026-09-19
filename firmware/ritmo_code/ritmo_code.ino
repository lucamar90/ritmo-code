/**
 * Ritmo Code — monitor d'uso per Claude Code, schermo touch LVGL
 * Scheda: Guition JC4832W535 (ESP32-S3, AXS15231B QSPI), 480x320 orizzontale.
 *
 * Dashboard del rate-limit di Claude Code (finestre 5h e 7g, header unified-*),
 * sonda reale per modello (latenza + HTTP), proiezione di esaurimento della
 * finestra 5h e ritmo d'uso orario con filtro di periodo. Token OAuth inserito
 * sullo schermo e salvato cifrato (AES-256-GCM, chiave derivata da un PIN di 4 cifre).
 *
 * Nessun pulsante fisico: navigazione 100% touch (swipe tra le schermate + slideshow).
 * Init di display/touch validato nel bring-up (vedi RIFERIMENTO-HARDWARE-LVGL.md).
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <display/lv_display_private.h>   // perf_label / timer del contatore FPS
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>
#include "config.h"
#include "touch.h"
#include "wifi_manager.h"
#include "api.h"
#include "status.h"
#include "crypto.h"
#include "accounts.h"
#include "extras.h"         // meteo (Open-Meteo) e statistiche del PC per la home
#include "status_page.h"   // pagina di stato servita su /
#include "logo_assets.h"   // Clawd + logotipo ufficiali (generato da tools/gen_logo_assets.py)
struct NoticeText;                 // avviso a schermo intero (prototipi generati da Arduino)
struct AlertRec;                   // voce della cronologia degli avvisi (idem)

// ---- Tema "Terminale": font JetBrains Mono (tools/gen_fonts.sh) ----
// 12/14 Regular, 22 Medium, 54 ExtraBold (solo cifre e %). I simboli che JetBrains
// Mono non ha (✻ ↻ ↵ ✎ e lo spinner di Claude Code) arrivano da DejaVu Sans Mono;
// i LV_SYMBOL_* della tastiera LVGL dal fallback Montserrat.
LV_FONT_DECLARE(font_jbm_12)
LV_FONT_DECLARE(font_jbm_14)
LV_FONT_DECLARE(font_jbm_22)
LV_FONT_DECLARE(font_jbm_54)
LV_FONT_DECLARE(font_jbm_96)
LV_FONT_DECLARE(font_jbm_150)
#define F12 (&font_jbm_12)
#define F14 (&font_jbm_14)
#define F22 (&font_jbm_22)
#define F54 (&font_jbm_54)
#define F96 (&font_jbm_96)                  // orologio: solo cifre, due punti e trattino
#define F150 (&font_jbm_150)                // orologio notturno a tutto schermo
#define U_SPARK   "\xE2\x9C\xBB"   // ✻
#define U_REFRESH "\xE2\x86\xBB"   // ↻
#define U_MENU    "\xE2\x89\xA1"   // ≡
#define U_LEFT    "\xE2\x86\x90"   // ←
#define U_RIGHT   "\xE2\x86\x92"   // →
#define U_ENTER   "\xE2\x86\xB5"   // ↵
#define U_BKSP    "\xE2\x8C\xAB"   // ⌫
#define U_BLOCK   "\xE2\x96\x88"   // █
#define U_CURSOR  "\xE2\x96\x8C"   // ▌
#define U_MIDDOT  "\xC2\xB7"       // ·
#define U_PENCIL  "\xE2\x9C\x8E"   // ✎
#define U_CROSS   "\xE2\x9C\x95"   // ✕

// ---- Palette tema Terminale (stile Claude Code) ----
// Fondo terminale caldo, cornici a 1 px, un solo accento argilla; verde/ambra/rosso
// solo per lo stato.
#define C_BG       0x141413
#define C_SURFACE  0x1B1A18   // tasti e campi di testo
#define C_SURFACE2 0x23221F   // stato "premuto"
#define C_TRACK    0x2C2A27   // blocchi spenti
#define C_GRID     0x2C2A27   // griglie tratteggiate
#define C_BORDER   0x3A3834   // cornici
#define C_TEXT     0xE8E6DF
#define C_MUTED    0x8E8B82
#define C_FAINT    0x5C5A55
#define C_ACCENT   0xD97757   // argilla Claude
#define C_OK       0x9BC08A
#define C_WARN     0xE0B25A
#define C_BAD      0xE06C5A
#define C_BLUE     0x7DB9D6

// ---- Lingua (0 = italiano, 1 = english; Impostazioni -> NVS "lang") ----
// I font JetBrains Mono includono Latin-1: a schermo le lettere accentate ci sono.
static uint8_t g_lang = 0;
#define TRS(pt, en) (g_lang ? (en) : (pt))

// barra a blocchi: due label affiancate (piena + vuota) nello stesso font mono.
// Definita qui in cima: il preprocessore Arduino genera i prototipi prima del codice.
struct Blocks { lv_obj_t *on, *off; int n; };

// ---- Hardware ----
Arduino_Canvas *gfx = nullptr;
static uint16_t *canvas_fb = nullptr;
AXS15231B_Touch touch_dev(TOUCH_SCL, TOUCH_SDA, TOUCH_INT, TOUCH_ADDR, TOUCH_ROTATION);
WiFiManager g_wifi;
Preferences g_prefs;

// ---- Stato dell'applicazione ----
enum State {
  ST_BOOT, ST_PIN, ST_SETUP_PIN, ST_WIFI, ST_TOKEN,
  ST_LOADING, ST_MAIN, ST_SETTINGS, ST_ACCOUNTS, ST_ACCT_NAME, ST_ABOUT, ST_ERROR,
  ST_MODELS, ST_MODEL_EDIT, ST_OTA, ST_NETWORKS
};
static State g_state = ST_BOOT;
static State g_pending = ST_BOOT;
static bool  g_dirty = false;
static void request_state(State s) { g_pending = s; g_dirty = true; }

// ---- Dati ----
static UsageData   g_usage = {};
static ModelStatus g_status = {true, true, true, true, false};

// ---- Modelli sondati (1 per ciclo, a rotazione) ----
// Gli ID si cambiano dal browser (http://<ip>/models) o dal touch (Impostazioni ->
// Modelli) e restano in NVS ("mid0".."mid3"): quando Anthropic rinomina un modello
// basta aggiornare l'ID, senza riflashare. Il nome mostrato resta fisso perche' e'
// legato all'accessorio della mascotte. Nessuna richiesta in piu' alla API: la
// modifica azzera solo il risultato della sonda, che si rifa' al suo turno.
#define NMODELS 4
#define MODEL_ID_MAX 48
struct ModelInfo {
  const char *name; const char *defId; char id[MODEL_ID_MAX]; ProbeResult pr; uint32_t atMs;
  uint16_t lh[7]; uint8_t lhN;          // ultime latenze (mini-linea nella pagina Modelli)
};
static ModelInfo g_models[NMODELS] = {
  {"Haiku",  "claude-haiku-4-5-20251001", "", {0, 0}, 0},
  {"Sonnet", "claude-sonnet-5",           "", {0, 0}, 0},
  {"Opus",   "claude-opus-5",             "", {0, 0}, 0},
  {"Fable",  "claude-fable-5-1",          "", {0, 0}, 0},
};
static int g_probeIdx = 0;

// ---- Rete in background (task FreeRTOS sul core 0) ----
// Le chiamate HTTPS (utilizzo, status.claude.com, sonda modello) bloccavano il
// loop per 2-5 s: niente swipe ne' tocchi durante l'aggiornamento. Ora le esegue
// net_task sul core 0 (dove gira gia' lo stack WiFi), mentre LVGL resta libero sul
// core 1. Il task NON tocca mai LVGL, NVS o LittleFS: riempie g_net e alza
// g_netDone; il loop applica i risultati nel proprio thread (net_apply).
struct NetJob {
  // richiesta: scritta dal loop prima di svegliare il task
  char token[200];
  char modelId[MODEL_ID_MAX];
  int  modelIdx;
  uint32_t epoch;          // g_dataEpoch al momento della richiesta
  bool firstLoad;          // true = schermata di caricamento
  // risultato: scritto dal task
  UsageData usage;
  bool usageOk;
  ModelStatus status;
  bool statusOk;
  ProbeResult probe;
  bool probed;
  uint32_t durMs;
};
static NetJob g_net;
static TaskHandle_t g_netTask = nullptr;
static volatile bool g_netBusy = false;   // richiesta in corso (il task possiede g_net)
static volatile bool g_netDone = false;   // risultato pronto da applicare
static bool g_loadPending = false;        // ST_LOADING in attesa che il task sia libero
// Cambia quando token/account cambiano (switch, nuovo token, reset): un risultato
// partito prima appartiene ai dati vecchi e va scartato.
static uint32_t g_dataEpoch = 0;

// ID accettato: 3..47 caratteri a-z 0-9 . - _ (finisce dentro il JSON della sonda)
static bool model_id_valid(const char *v) {
  size_t n = strlen(v);
  if (n < 3 || n >= MODEL_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = v[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'))
      return false;
  }
  return true;
}
static void model_key(int i, char *key, size_t sz) { snprintf(key, sz, "mid%d", i); }
static void models_load() {
  for (int i = 0; i < NMODELS; i++) {
    char key[8]; model_key(i, key, sizeof(key));
    String v = g_prefs.isKey(key) ? g_prefs.getString(key, "") : String();
    strlcpy(g_models[i].id, (v.length() && model_id_valid(v.c_str())) ? v.c_str() : g_models[i].defId,
            MODEL_ID_MAX);
  }
}
// Imposta l'ID del modello i. Vuoto = torna al predefinito. Toglie spazi e
// maiuscole (copia-incolla dalla documentazione). false = ID non valido.
static bool model_set_id(int i, const char *raw) {
  if (i < 0 || i >= NMODELS || !raw) return false;
  char v[MODEL_ID_MAX + 1];
  size_t n = 0;
  for (const char *c = raw; *c; c++) {
    if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') continue;
    if (n >= MODEL_ID_MAX) return false;               // troppo lungo
    v[n++] = (*c >= 'A' && *c <= 'Z') ? (char)(*c + 32) : *c;
  }
  v[n] = 0;
  char key[8]; model_key(i, key, sizeof(key));
  const char *next;
  if (!v[0] || !strcmp(v, g_models[i].defId)) {
    if (g_prefs.isKey(key)) g_prefs.remove(key);
    next = g_models[i].defId;
  } else {
    if (!model_id_valid(v)) return false;
    g_prefs.putString(key, v);
    next = v;
  }
  if (strcmp(next, g_models[i].id) != 0) {
    strlcpy(g_models[i].id, next, MODEL_ID_MAX);
    g_models[i].pr.code = 0; g_models[i].pr.ms = 0;   // "--" fino alla prossima sonda
    g_models[i].atMs = 0;
    g_models[i].lhN = 0;
    Serial.printf("[MODEL] %s -> %s\n", g_models[i].name, g_models[i].id);
  }
  return true;
}


// ---- Token / sicurezza ----
static AccountSlots g_accts;
static EncryptedBlob g_blob;
static bool g_hasToken = false;              // esiste un account salvato in NVS
static bool g_onboarding = false;            // primo setup in corso
static char g_token[200] = {0};              // token decifrato (solo in RAM)
static char g_pendingToken[200] = {0};       // token inserito, in attesa del PIN
// PIN di sessione: resta in RAM dallo sblocco fino al riavvio, perche' cambiare account
// e aggiungerne uno richiedono di decifrare/cifrare ALTRI slot senza richiedere il PIN.
// Compromesso accettato: non indebolisce il modello — il token vive GIA' decifrato in
// g_token, quindi chi riesce a leggere la RAM ha gia' cio' che conta. Resta valido
// che: niente di tutto questo finisce in NVS, e factory_reset() azzera questo buffer.
static char g_sessionPin[PIN_LEN + 1] = {0};
static int  g_tokenTargetSlot = 0;
static char g_pendingLabel[ACCT_LBL_MAX] = {0};
static char g_pinEntry[PIN_LEN + 1] = {0};   // cifre in digitazione
static bool g_pinForWipe = false;            // schermata PIN aperta per confermare "cancella tutto"
static char g_pinFirst[PIN_LEN + 1] = {0};   // 1° inserimento nel setup del PIN
static bool g_pinConfirming = false;         // setup: conferma (2° inserimento)
static int  g_pinAttempts = 0;               // tentativi errati (persistito)
static uint32_t g_lockoutUntil = 0;          // millis fino al prossimo tentativo consentito
static bool g_timeInit = false;

// ---- Refresh in background ----
static bool g_wantRefresh = false;        // il pulsante di refresh ha chiesto un aggiornamento
static bool g_refreshing = false;         // richiesta in corso
static bool g_lastFetchOk = true;         // l'ultimo fetch e' andato a buon fine?
static uint32_t g_lastOkMs = 0;           // millis dell'ultimo successo (per "aggiornato Xs fa")
static lv_obj_t *g_hdrStatus = nullptr;   // testo di stato nell'intestazione del dashboard

// ---- Fuso orario ----
// TZ_ROME usa la regola POSIX CET/CEST: l'ora legale cambia da sola (ultima
// domenica di marzo / ottobre). Gli altri valori sono offset GMT fissi.
#define TZ_ROME     99
#define TZ_ROME_RULE "CET-1CEST,M3.5.0,M10.5.0/3"

// ---- Luminosita' ----
static const uint8_t BRI_LEVELS[3] = {60, 160, 255};
static int g_briIdx = 1;

static uint32_t g_lastPollMs = 0;         // millis dell'ultimo poll (per la barra di refresh)
static int g_pollSec = DEFAULT_POLL_SEC;  // intervallo di aggiornamento (config, NVS)
static int g_tzOffset = TZ_ROME;          // fuso: TZ_ROME (ora legale auto) o offset GMT fisso, NVS
static int g_slideSec = 0;                // slideshow: 0=off, 5/10/15/30s (config, NVS)
static int g_heatSrc = 0;                 // pagina ritmo: 0 claude, 1 pomodoro (NVS "heats")
static int g_heatMode = 3;                // 0=oggi 1=7g 2=30g 3=tutto (config, NVS)
static bool g_resetAlert = true;          // avviso quando una finestra oltre l'80% si libera (NVS "rstal")
static bool g_pcSound = true;             // suoni e notifiche sul PC per gli avvisi (NVS "pcsnd")
static int  g_ccCloseIdx = 2;             // chiude da solo l'avviso di Claude: 5 s, 10 s, 30 s, mai (NVS "ccclose")
static const uint8_t CC_CLOSE_S[4] = {5, 10, 30, 0};
static int  g_ccAlert = 2;                // avvisi di Claude Code: 0 spento, 1 sempre, 2/3 fine lavoro oltre 1/5 min (NVS "ccal")

// ---- Diagnostica prestazioni (Impostazioni -> Contatore FPS, NVS "perf") ----
// A schermo: overlay LVGL "FPS, CPU / ms (render | flush)". Sul seriale ogni 2s:
// fps, tempo di rotazione del frame, tempo di invio QSPI, loop piu' lento e
// memoria libera. Il tempo delle chiamate di rete viene stampato a ogni refresh.
// Nota: con il render FULL anche l'overlay stesso forza un ridisegno ogni ~300ms.
static bool g_perfOn = false;
struct PerfStats {
  uint32_t frames;
  uint64_t rotUs, copyUs, waitUs, txUs;
  uint32_t rotMaxUs, copyMaxUs, waitMaxUs, txMaxUs;
  uint32_t txFrames;       // frame inviati dal task del display (core 0)
  uint32_t loopMaxMs;
};
static PerfStats g_perf = {};
static uint32_t g_lastTouchMs = 0;        // ultimo tocco (mette in pausa lo slideshow)
// Notte / attenuazione (NVS "night", "nightp", "dim"). Valgono solo sul dashboard.
static const uint8_t NIGHT_FROM[4] = {0, 22, 23, 0};     // indice 0 = spento
static int  g_nightIdx = 0;                    // 0 spento, 1 22-07, 2 23-07, 3 00-07
static bool g_nightPause = true;               // di notte niente richieste ad Anthropic
static int  g_dimIdx = 0;                      // 0 spento, poi DIM_MIN minuti senza tocchi
static const uint8_t DIM_MIN[4] = {0, 1, 5, 10};
#define NIGHT_WAKE_MS 30000UL                  // di notte un tocco accende per 30 s
#define DIM_LEVEL 12                           // duty del backlight attenuato
static bool g_userPause = false;               // richieste messe in pausa dal tasto in testata (NVS "pause")
static uint32_t g_pauseUntil = 0;              // epoch di ripresa automatica, 0 = senza limite (NVS "pauseu")
static int  g_clockIdx = 0;                    // torna alla home dopo: 0 spento, poi CLOCK_MIN minuti (NVS "clock")
static const uint8_t CLOCK_MIN[4] = {0, 2, 5, 10};
// ---- Home: meteo e PC (task "extra" sul core 0, mai richieste ad Anthropic) ----
static float g_wxLat = DEFAULT_WX_LAT, g_wxLon = DEFAULT_WX_LON;   // NVS "wxlat", "wxlon"
static char  g_wxCity[32] = DEFAULT_WX_CITY;                        // NVS "wxcity"
static char  g_pcHost[48] = DEFAULT_PC_HOST;                        // NVS "pchost" (Ritmo Code PC Monitor, ip:porta)
static float g_kwhPrice = 0.30f;                                    // euro per kWh (NVS "kwh", /home)
static WeatherData g_wx = {};
static PcStats g_pc = {};
static uint32_t g_wxAtMs = 0, g_wxTryMs = 0, g_pcAtMs = 0, g_pcTryMs = 0;
static volatile bool g_wxReq = false, g_wxDone = false, g_pcReq = false, g_pcDone = false;
static WeatherData g_wxRes = {};
static PcStats g_pcRes = {};
static char g_pcReqHost[48] = {0};
// avviso da mandare al PC (task "extra"): l'ultimo vince
struct PcNotify { char host[48], ev[8], title[64], msg[96]; };
static PcNotify g_pcNtf = {};
static volatile bool g_pcNtfReq = false;
static SemaphoreHandle_t g_httpsLock = nullptr;
#define PC_HIST 240                                 // grafici: un punto per lettura (1 s = 4 min, 5 s = 20 min)
static int g_pcIntIdx = 0;                          // intervallo di lettura del PC (NVS "pcint")
static const uint16_t PC_INT_S[4] = {1, 3, 5, 60};
static uint32_t g_pcHistAtMs = 0;
// il PC e' considerato spento se non risponde da 3 letture (almeno 20 s)
static uint32_t pc_stale_ms() { uint32_t m = 3000UL * PC_INT_S[g_pcIntIdx]; return m < 20000UL ? 20000UL : m; }
static uint8_t g_pcHist[3][PC_HIST];               // cpu, gpu, ram in %
static int g_pcHistN = 0;
static lv_point_precise_t g_pcSparkPts[3][PC_HIST];   // una sola connessione TLS alla volta (RAM interna)
static uint8_t g_screenMode = 0;               // 0 normale, 1 attenuato, 2 spento, 3 orologio notturno
static bool g_nightClock = true;               // di notte: orologio tenue invece dello schermo spento (NVS "nightclk")
static int  g_nightBri = 0;                    // luminosita' dell'orologio notturno: 0 tenue, 1 molto tenue (NVS "nightbri")
static const uint8_t NIGHT_BRI[2] = {18, 6};
static bool g_touchSwallow = false;            // il tocco che risveglia non preme nulla
static uint32_t g_lastSlideMs = 0;

// ---- Storico (ring buffer; persistito in LittleFS) ----
#define HIST_MAX 160
struct Sample { uint32_t t; uint8_t h5; uint8_t d7; };   // t = epoch (0 = orologio non sincronizzato)
static Sample g_hist[HIST_MAX];
static int g_histN = 0;
static int g_histHead = 0;
static float g_hourBurn[24] = {0};   // consumo per ora del giorno (da sempre)
static float g_lastH5 = -1.0f;       // ultimo utilizzo 5h (delta dell'heatmap)

// ---- Picco di ogni settimana (per account, /weeks<slot>.bin) ----
#define NWEEKS 12
struct WeekRec { uint32_t reset; uint8_t peak; };    // reset = epoch di fine settimana
static WeekRec g_weeks[NWEEKS];
static int g_weekN = 0;

// ---- Heatmap per giorno (per il filtro oggi/7g/30g) ----
#define NDAYS 31
struct DayHeat { uint32_t day; float burn[24]; };   // day = giorni locali dall'epoch
static DayHeat g_days[NDAYS];
static int g_dayN = 0;

// ---- Mascotte Clawd ufficiali (pagina modelli; umore in base allo stato) ----
// mood: 0=mai sondato, 1=ok, 2=limitato(429), 3=errore/incidente, 4=n/d(404)
struct Mascot { lv_obj_t *cont, *img, *lid[2], *drop; int baseY, mood; };
static Mascot g_masc[NMODELS];
static int g_mascN = 0;
static lv_point_precise_t g_mXPts[NMODELS][4][2];   // occhi a X (mood 3)

// ---- Puntatori UI del dashboard (azzerati a ogni build di ST_MAIN) ----
#define NTILES 7
#define NBLK 19                       // blocchi della barra di utilizzo
struct DashUI {
  lv_obj_t *tv, *tile[NTILES], *tab[NTILES];
  lv_obj_t *hdrPath, *hdrAcct, *refBar, *pauseBtn, *pauseBar[2], *pausePlay;
  lv_obj_t *hdrSpark, *hmClLegend;                // ✻ che gira e "claude · al lavoro" quando Claude lavora
  // ora
  lv_obj_t *agPct5, *agCd5, *agAt5, *agPct7, *agCd7, *agAt7;
  Blocks blk5, blk7;
  lv_obj_t *agWord, *agCursor, *agPace, *agPaceMark;
  // modelli
  lv_obj_t *mDot[NMODELS], *mName[NMODELS], *mSpark[NMODELS], *mLat[NMODELS], *mStat[NMODELS];
  lv_obj_t *mSum, *mInc;
  // finestra 5h
  lv_obj_t *trHist, *trProj, *trDot, *trCap, *trT0, *trT1;
  // ritmo
  lv_obj_t *heat[24], *heatBtn[4], *heatTab[2], *heatCap;
  // settimane
  lv_obj_t *wkBar[8], *wkVal[8], *wkDate[8], *wkCap;
  // home
  lv_obj_t *hmTime, *hmDate, *hmWxIcon, *hmTemp, *hmDesc, *hmRain, *hmSun;
  lv_obj_t *hmPct[2], *hmInfo[2], *hmRight[2];
  Blocks hmBlk[2];
  lv_obj_t *hmPcRow, *hmPcOff, *hmPcVal[4], *hmPcLegend;
  // pc
  lv_obj_t *pcLeg[4], *pcMain[4], *pcSub[3], *pcSpark[3], *pcSys, *pcDisks;
  int hmWxCode;
};
static DashUI g_ui;
static lv_obj_t *g_pinDots = nullptr, *g_pinMsg = nullptr;
static int g_curTile = 0;

// punti delle linee del grafico di tendenza (devono restare validi)
static lv_point_precise_t g_trPts[HIST_MAX];
static lv_point_precise_t g_trProjPts[2];

// ---- Forward declarations ----
static void render_state();
static void refresh_ui_values();
static void dash_tick();
static void set_hdr_status();
static void apply_tz();
static void ui_pin();
static void ui_wifi();
static void ui_token();
static void ui_loading(const char *sub);
static void ui_main();
static void ui_settings();
static void ui_accounts();
static void ui_account_name();
static void ui_models();
static void perf_apply();
static void ui_model_edit();
static void ui_ota();
static void ui_networks();
static void ui_message(const char *title, const char *sub, uint32_t color);
static void nav_cb(lv_event_t *e);
static void start_data_web();
static void trend_redraw();
static void heat_redraw();
static void weeks_redraw();
static void home_redraw();
static void home_tick();
static void pc_redraw();
static void pause_menu_close();
static void show_moment(int win, int thr);
static void moment_tick();
static void moment_close();
static void cc_event(long id, const char *ev, const char *proj, int dur, int age);
static int g_setGroup = 0;
static bool g_ccFocusDefer = false;          // avvisi di Claude durante il focus: rimandati alla pausa (NVS "ccfocus")
static bool g_ccDeferred = false;            // un avviso di Claude aspetta la fine del focus
static bool tm_action(int opt);               // comandi del timer (menu del dispositivo e PC)
static uint32_t tm_left_s();
// pannello a tendina: il touch riconosce il gesto (giu' dalla testata / su per chiudere), il loop apre
static volatile int g_shadeReq = 0;               // 1 apri, 2 chiudi
static lv_obj_t *g_shade = nullptr;
// Aggiornamento del firmware dall'ultima release di GitHub: controllo (task extra) e installazione
// (task di rete: scrivere la flash da uno stack in PSRAM non si puo')
enum { UPD_IDLE = 0, UPD_CHECKING, UPD_AVAILABLE, UPD_UPTODATE, UPD_ERROR, UPD_INSTALLING, UPD_FAILED, UPD_REBOOT };
static volatile int g_updState = UPD_IDLE;
static volatile bool g_updCheckReq = false, g_updInstallReq = false, g_updRunning = false, g_updDone = false;
static volatile int g_updPct = 0;
static char g_updTag[16] = "", g_updUrl[200] = "";
static String g_updErr;
static uint32_t g_updCheckedMs = 0;
static uint32_t g_updCheckStartMs = 0;        // "controllo..." resta a schermo almeno 1,5 s
static void upd_progress(int pct) { g_updPct = pct; }
// "v3.9.3" piu' recente di FW_VERSION? (confronto numero per numero)
static bool ver_newer(const char *tag) {
  int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
  const char *t = tag; while (*t && !isdigit((unsigned char)*t)) t++;
  sscanf(t, "%d.%d.%d", &a[0], &a[1], &a[2]);
  sscanf(FW_VERSION, "%d.%d.%d", &b[0], &b[1], &b[2]);
  for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
  return false;
}                   // gruppo di impostazioni aperto (0 = pagina principale)
static char g_ccEv[8], g_ccProj[28];
static int  g_ccBusyN = 0;                      // sessioni di Claude Code al lavoro (PC Monitor)
static void cc_busy_ui();            // ultimo evento di Claude Code da mostrare
static void tm_menu_open(lv_event_t *e);
static void tm_label(char *out, size_t sz);
static uint32_t tm_color();
enum { TM_OFF = 0, TM_TIMER, TM_FOCUS, TM_BREAK };
static int  g_tmMode = TM_OFF;                // timer/pomodoro in corso
static int  g_pomoToday = 0;                  // pomodori completati oggi (NVS "pomn", giorno in "pomday")
static long g_pomoDay = 0;
static int  pomo_today();
static uint32_t g_tmEndMs = 0, g_tmLenS = 0;
static int  g_pomoN = 0;                      // pomodori completati nel ciclo in corso
// pomodoro: tre impostazioni pronte (focus / pausa / pausa lunga dopo il quarto, in minuti)
struct PomoPreset { uint8_t focus, brk, lng; };
static const PomoPreset POMO_PRESETS[3] = {{25, 5, 15}, {50, 10, 20}, {15, 3, 10}};
static int g_pomoPre = 0;                     // impostazione del ciclo in corso
#define POMO_FOCUS_S (POMO_PRESETS[g_pomoPre].focus * 60)
#define POMO_SHORT_S (POMO_PRESETS[g_pomoPre].brk * 60)
#define POMO_LONG_S  (POMO_PRESETS[g_pomoPre].lng * 60)
#define POMO_CYCLE   4

// ============================================================
// Pipeline di display/touch (validata nel bring-up)
// ============================================================
// Render PARZIALE: LVGL consegna solo le zone cambiate (a strisce, dal buffer
// piccolo in RAM interna). Ogni zona viene ruotata di 270 gradi nel framebuffer
// del Canvas (320x480, PSRAM) e, all'ultima zona del ciclo, il frame intero parte
// verso il pannello: l'AXS15231B di questa scheda non gestisce bene le finestre
// parziali, quindi l'invio resta a schermo intero.
// Mappatura invariata rispetto al bring-up: fb[(479 - x) * 320 + y] = pixel(x, y).
// Il ciclo esterno su x scrive il framebuffer in blocchi contigui (una riga del
// pannello per x): con la PSRAM dietro la cache e' ~decine di volte piu' veloce
// della vecchia copia a salti.
// ---- Invio del frame in parallelo (task "disp" sul core 0) ----
// L'invio QSPI del frame intero costa ~40 ms e prima bloccava LVGL. Ora:
//  - core 1 (LVGL) ruota le zone cambiate in canvas_fb, che resta l'immagine
//    completa e aggiornata;
//  - a fine frame aspetta che l'invio precedente sia finito, copia in tx_fb SOLO
//    il rettangolo cambiato in questo frame e sveglia il task;
//  - core 0 spedisce tx_fb al pannello mentre LVGL disegna gia' il frame dopo.
// tx_fb riceve ogni frame (nessuno viene saltato), quindi resta identico a
// canvas_fb dopo ogni copia: niente tearing, niente righe a meta' fra due frame.
static Arduino_GFX *g_panel = nullptr;        // driver AXS15231B (sotto al Canvas)
static uint16_t *tx_fb = nullptr;             // copia stabile del frame da inviare (PSRAM)
static SemaphoreHandle_t g_txIdle = nullptr;  // libero = nessun invio in corso
static TaskHandle_t g_txTask = nullptr;
static uint32_t g_frameRotUs = 0;
static int g_dirtyR0 = 480, g_dirtyR1 = -1, g_dirtyC0 = 320, g_dirtyC1 = -1;   // coordinate pannello

static void disp_tx_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t t0 = micros();
    g_panel->draw16bitRGBBitmap(0, 0, tx_fb, 320, 480);
    uint32_t tx = micros() - t0;
    g_perf.txFrames++;
    g_perf.txUs += tx; if (tx > g_perf.txMaxUs) g_perf.txMaxUs = tx;
    xSemaphoreGive(g_txIdle);
    // L'invio QSPI e' a polling e non cede mai la CPU: senza questa pausa, con lo
    // spinner che anima a ~27 fps e il TLS del task di rete sullo stesso core,
    // l'idle del core 0 non girava piu' e il task watchdog riavviava la scheda.
    vTaskDelay(1);
  }
}

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t t0 = micros();
  const uint16_t *src = (const uint16_t *)px_map;
  int w = area->x2 - area->x1 + 1;
  int h = area->y2 - area->y1 + 1;
  for (int i = 0; i < w; i++) {
    uint16_t *dst = canvas_fb + (479 - (area->x1 + i)) * 320 + area->y1;
    const uint16_t *s = src + i;
    for (int j = 0; j < h; j++, s += w) dst[j] = *s;
  }
  // rettangolo cambiato, in coordinate del pannello (riga = 479 - x, colonna = y)
  if (479 - area->x2 < g_dirtyR0) g_dirtyR0 = 479 - area->x2;
  if (479 - area->x1 > g_dirtyR1) g_dirtyR1 = 479 - area->x1;
  if (area->y1 < g_dirtyC0) g_dirtyC0 = area->y1;
  if (area->y2 > g_dirtyC1) g_dirtyC1 = area->y2;
  g_frameRotUs += micros() - t0;

  if (lv_display_flush_is_last(disp)) {
    uint32_t t1 = micros();
    xSemaphoreTake(g_txIdle, portMAX_DELAY);          // invio precedente finito
    uint32_t t2 = micros();
    if (g_dirtyR1 >= g_dirtyR0 && g_dirtyC1 >= g_dirtyC0) {
      size_t n = (size_t)(g_dirtyC1 - g_dirtyC0 + 1) * sizeof(uint16_t);
      for (int r = g_dirtyR0; r <= g_dirtyR1; r++)
        memcpy(tx_fb + r * 320 + g_dirtyC0, canvas_fb + r * 320 + g_dirtyC0, n);
    }
    uint32_t t3 = micros();
    g_dirtyR0 = 480; g_dirtyR1 = -1; g_dirtyC0 = 320; g_dirtyC1 = -1;
    xTaskNotifyGive(g_txTask);                        // parte l'invio sul core 0

    uint32_t wait = t2 - t1, copy = t3 - t2;
    g_perf.frames++;
    g_perf.rotUs  += g_frameRotUs; if (g_frameRotUs > g_perf.rotMaxUs) g_perf.rotMaxUs = g_frameRotUs;
    g_perf.waitUs += wait;         if (wait > g_perf.waitMaxUs)        g_perf.waitMaxUs = wait;
    g_perf.copyUs += copy;         if (copy > g_perf.copyMaxUs)        g_perf.copyMaxUs = copy;
    g_frameRotUs = 0;
  }
  lv_disp_flush_ready(disp);
}
static void screen_apply();
static bool g_touchDown = false, g_gestDone = false;
static int16_t g_gestX = 0, g_gestY = 0;
static void touch_gesture_reset() { g_touchDown = false; }
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t x, y;
  if (touch_dev.touched()) {
    touch_dev.readData(&x, &y);
    g_lastTouchMs = millis();          // mette in pausa lo slideshow durante l'interazione
    if (g_screenMode != 0) {           // schermo spento/attenuato: il tocco serve solo a risvegliare
      g_touchSwallow = true;
      g_screenMode = 0;
      screen_apply();
    }
    if (g_touchSwallow) { data->state = LV_INDEV_STATE_RELEASED; return; }
    // pannello a tendina: giu' partendo dalla testata apre, su chiude (piu' verticale che orizzontale)
    if (!g_touchDown) { g_touchDown = true; g_gestDone = false; g_gestX = x; g_gestY = y; }
    int16_t sx = g_gestX, sy = g_gestY; bool &done = g_gestDone;
    int dy = (int)y - sy, dx = abs((int)x - sx);
    if (!done && g_state == ST_MAIN) {
      if (!g_shade && sy < 40 && dy > 35 && dy > 2 * dx) { g_shadeReq = 1; done = true; }
      else if (g_shade && dy < -35 && -dy > 2 * dx)    { g_shadeReq = 2; done = true; }
      if (done) { g_touchSwallow = true; data->state = LV_INDEV_STATE_RELEASED; return; }
    }
    data->point.x = x; data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    g_touchSwallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
    touch_gesture_reset();
  }
}

// ============================================================
// Helper di UI
// ============================================================
// Aggiornano una label solo se testo/colore cambiano davvero: ogni modifica
// invalida l'area e, con il pannello che vuole il frame intero, costa ~40 ms di invio.
static void label_set(lv_obj_t *l, const char *txt) {
  if (!l) return;
  const char *cur = lv_label_get_text(l);
  if (cur && strcmp(cur, txt) == 0) return;
  lv_label_set_text(l, txt);
}
static void label_color(lv_obj_t *l, uint32_t col) {
  if (!l) return;
  lv_color_t c = lv_color_hex(col);
  if (lv_color_eq(lv_obj_get_style_text_color(l, LV_PART_MAIN), c)) return;
  lv_obj_set_style_text_color(l, c, 0);
}
static lv_obj_t *mklabel(lv_obj_t *p, const char *txt, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}
static void no_box(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, 0, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
// colore per soglia, a gradini come in un terminale: <50 ok, <80 argilla, poi rosso
static uint32_t level_hex(float p) {
  if (p >= 80.0f) return C_BAD;
  if (p >= 50.0f) return C_ACCENT;
  return C_OK;
}
static lv_color_t grad_color(float p) { return lv_color_hex(level_hex(p)); }

// ---------- primitive del tema Terminale ----------
// Oggetti senza stili del tema LVGL: niente ombre, padding o sfondi da calcolare.
static lv_obj_t *plain_obj(lv_obj_t *p) {
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
static lv_obj_t *hline(lv_obj_t *p, int x, int y, int w, uint32_t col) {
  lv_obj_t *o = plain_obj(p);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, 1);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}
// linea orizzontale tratteggiata: i punti devono restare validi (static)
static lv_point_precise_t PTS_W404[2] = {{0, 0}, {404, 0}};
static lv_point_precise_t PTS_W440[2] = {{0, 0}, {440, 0}};
static lv_obj_t *dline(lv_obj_t *p, int x, int y, lv_point_precise_t *pts) {
  lv_obj_t *l = lv_line_create(p);
  lv_obj_set_pos(l, x, y);
  lv_line_set_points(l, pts, 2);
  lv_obj_set_style_line_width(l, 1, 0);
  lv_obj_set_style_line_color(l, lv_color_hex(C_GRID), 0);
  lv_obj_set_style_line_dash_width(l, 3, 0);
  lv_obj_set_style_line_dash_gap(l, 3, 0);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}
// riquadro a filo sottile con il titolo inciso nella cornice
static lv_obj_t *tbox(lv_obj_t *p, int x, int y, int w, int h, const char *legend,
                      uint32_t col = C_BORDER, lv_obj_t **legendOut = nullptr) {
  lv_obj_t *b = plain_obj(p);
  lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(col), 0);
  lv_obj_set_style_radius(b, 4, 0);
  if (legend) {
    // Il titolo e' fratello del riquadro, non figlio: da figlio LVGL tagliava la parte
    // che sporge sopra il bordo. Sfondo pieno per "interrompere" la cornice.
    lv_obj_t *l = mklabel(p, legend, F12, C_MUTED);
    lv_obj_set_style_bg_color(l, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(l, 5, 0);
    lv_obj_set_pos(l, x + 8, y - 8);            // testo a x+13, allineato al rientro dei contenuti
    if (legendOut) *legendOut = l;
  }
  return b;
}
// pulsante: cornice 1 px, testo centrato, sfondo solo quando premuto
static lv_obj_t *tbtn(lv_obj_t *p, int x, int y, int w, int h, const char *txt, const lv_font_t *f,
                      uint32_t fg, uint32_t border, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = plain_obj(p);
  lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(border), 0);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE2), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 6);
  lv_obj_center(mklabel(b, txt, f, fg));
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}
// contenitore riga (flex) per comporre testi di colori diversi
static lv_obj_t *trow(lv_obj_t *p, int x, int y) {
  lv_obj_t *r = plain_obj(p);
  lv_obj_set_pos(r, x, y);
  lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  return r;
}
static Blocks blocks_create(lv_obj_t *p, int x, int y, int n, const lv_font_t *f) {
  lv_obj_t *r = trow(p, x, y);
  Blocks b = { mklabel(r, "", f, C_ACCENT), mklabel(r, "", f, C_TRACK), n };
  return b;
}
static void blocks_set(Blocks &b, float pct, uint32_t col) {
  if (!b.on) return;
  int f = (int)(pct / 100.0f * b.n + 0.5f);
  if (pct > 0.5f && f == 0) f = 1;
  if (f < 0) f = 0; if (f > b.n) f = b.n;
  char on[3 * NBLK + 1] = "", off[3 * NBLK + 1] = "";
  for (int i = 0; i < f; i++) strcat(on, U_BLOCK);
  for (int i = f; i < b.n; i++) strcat(off, U_BLOCK);
  label_set(b.on, on);
  label_set(b.off, off);
  label_color(b.on, col);
}
// intestazione delle schermate secondarie: ✻ titolo, [← indietro], filo
static void nav_cb(lv_event_t *e);
static void thead(lv_obj_t *scr, const char *title, int backTo) {
  lv_obj_t *r = trow(scr, 13, 11);
  mklabel(r, U_SPARK " ", F14, C_ACCENT);
  mklabel(r, title, F14, C_TEXT);
  if (backTo >= 0)
    tbtn(scr, 378, 5, 89, 32, TRS(U_LEFT " indietro", U_LEFT " back"), F14, C_MUTED, C_BORDER, nav_cb, (void *)(intptr_t)backTo);
  hline(scr, 13, 42, 454, C_BORDER);
}
// riga chiave/valore toccabile (impostazioni, modelli, reti wifi)
static lv_obj_t *kv_row(lv_obj_t *list, const char *key, const char *val, uint32_t keyCol, uint32_t valCol,
                        lv_event_cb_t cb, void *ud, lv_obj_t **valOut = nullptr) {
  lv_obj_t *r = plain_obj(list);
  lv_obj_set_size(r, 454, 42);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(r, lv_color_hex(C_SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_t *k = mklabel(r, key, F14, keyCol);
  lv_obj_align(k, LV_ALIGN_LEFT_MID, 13, 0);
  lv_obj_t *v = mklabel(r, val, F14, valCol);
  lv_obj_set_width(v, 230);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, -13, 0);
  static lv_point_precise_t PTS_W454[2] = {{0, 0}, {454, 0}};
  dline(r, 0, 41, PTS_W454);
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
  if (valOut) *valOut = v;
  return r;
}
static lv_obj_t *tlist(lv_obj_t *scr, int x, int y, int w, int h) {
  lv_obj_t *l = plain_obj(scr);
  lv_obj_set_pos(l, x, y); lv_obj_set_size(l, w, h);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(l, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(l, lv_color_hex(C_FAINT), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(l, 3, LV_PART_SCROLLBAR);
  return l;
}
// campo di testo e tastiera scuri (prima: tema chiaro di default di LVGL)
static void style_ta(lv_obj_t *ta) {
  lv_obj_set_style_bg_color(ta, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(ta, F14, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_BORDER), 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
  lv_obj_set_style_radius(ta, 4, 0);
  lv_obj_set_style_pad_hor(ta, 12, 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_FAINT), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_set_style_shadow_width(ta, 0, 0);
}
static void style_kb(lv_obj_t *kb) {
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_BG), 0);
  lv_obj_set_style_border_width(kb, 1, 0);
  lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_color(kb, lv_color_hex(C_BORDER), 0);
  lv_obj_set_style_pad_all(kb, 6, 0);
  lv_obj_set_style_pad_gap(kb, 4, 0);
  lv_obj_set_style_radius(kb, 0, 0);
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_SURFACE), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb, lv_color_hex(C_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb, F14, LV_PART_ITEMS);
  lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(kb, lv_color_hex(C_TRACK), LV_PART_ITEMS);
  lv_obj_set_style_radius(kb, 3, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_SURFACE2), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(kb, lv_color_hex(C_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(kb, lv_color_hex(C_MUTED), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(kb, lv_color_hex(C_BORDER), LV_PART_ITEMS | LV_STATE_CHECKED);
}
// spinner di Claude Code (· ✢ ✳ ✶ ✻ ✽ avanti e indietro): una label, 1 carattere
static const char *SPIN[10] = {"\xC2\xB7", "\xE2\x9C\xA2", "\xE2\x9C\xB3", "\xE2\x9C\xB6", "\xE2\x9C\xBB",
                               "\xE2\x9C\xBD", "\xE2\x9C\xBB", "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2"};
static lv_obj_t *g_spin = nullptr;
static void spin_tick() {
  static uint32_t last = 0; static int k = 0;
  if (!g_spin) return;
  uint32_t now = millis();
  if (now - last < 120) return;
  last = now; k = (k + 1) % 10;
  lv_label_set_text(g_spin, SPIN[k]);
}
// riga "✻ testo" centrata con lo spinner che gira
static lv_obj_t *spin_row(lv_obj_t *scr, const char *txt, int yOfs) {
  lv_obj_t *r = plain_obj(scr);
  lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 8, 0);
  g_spin = mklabel(r, SPIN[0], F14, C_ACCENT);
  lv_obj_t *t = mklabel(r, txt, F14, C_TEXT);
  lv_obj_align(r, LV_ALIGN_CENTER, 0, yOfs);
  return t;
}
// "reset tra 1h 23m" / "2d 4h" / "ora" / "--" (orologio non sincronizzato)
static void fmt_eta(uint32_t epoch, char *out, int sz) {
  time_t now = time(nullptr);
  if (now < 1000000000L || epoch == 0) { snprintf(out, sz, "--"); return; }
  long d = (long)epoch - (long)now;
  if (d <= 0) { snprintf(out, sz, "%s", TRS("ora", "now")); return; }
  int days = d / 86400; d %= 86400;
  int hrs  = d / 3600;  d %= 3600;
  int mins = d / 60;
  if (days > 0)      snprintf(out, sz, "%dd %dh", days, hrs);
  else if (hrs > 0)  snprintf(out, sz, "%dh %02dm", hrs, mins);
  else               snprintf(out, sz, "%dm", mins);
}
static void fmt_clock(uint32_t epoch, char *out, int sz) {
  if (epoch == 0 || time(nullptr) < 1000000000L) { strlcpy(out, "--:--", sz); return; }
  time_t t = (time_t)epoch; struct tm tmv;
  localtime_r(&t, &tmv);
  if (!g_lang) {                         // %a e' sempre in inglese (locale C)
    static const char *GG[7] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
    snprintf(out, sz, "%s %02d:%02d", GG[tmv.tm_wday % 7], tmv.tm_hour, tmv.tm_min);
    return;
  }
  strftime(out, sz, "%a %H:%M", &tmv);
}
static void fmt_hm(uint32_t epoch, char *out, int sz) {
  if (epoch == 0 || time(nullptr) < 1000000000L) { strlcpy(out, "--:--", sz); return; }
  time_t t = (time_t)epoch; struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, sz, "%H:%M", &tmv);
}
// 1234 -> "1.2k", 2345678 -> "2.3M"

// ============================================================
// NVS / persistenza
// ============================================================
static void load_persisted() {
  g_prefs.begin(NVS_NAMESPACE, false);
  accountsLoad(g_prefs, g_accts);
  if (g_accts.used[g_accts.active] &&
      accountLoadBlob(g_prefs, g_accts.active, g_blob)) {
    g_hasToken = true;
  }
  g_pinAttempts = g_prefs.getInt("pinatt", 0);
  g_briIdx = g_prefs.getInt("bri", 1);
  if (g_briIdx < 0 || g_briIdx > 2) g_briIdx = 1;
  g_pollSec = g_prefs.getInt("poll", DEFAULT_POLL_SEC);
  if (g_pollSec < MIN_POLL_SEC || g_pollSec > MAX_POLL_SEC) g_pollSec = DEFAULT_POLL_SEC;
  g_tzOffset = g_prefs.getInt("tz", TZ_ROME);
  if (g_tzOffset != TZ_ROME && (g_tzOffset < -12 || g_tzOffset > 14)) g_tzOffset = TZ_ROME;
  g_slideSec = g_prefs.getInt("slide", 0);
  if (g_slideSec != 0 && g_slideSec != 5 && g_slideSec != 10 &&
      g_slideSec != 15 && g_slideSec != 30) g_slideSec = 0;
  g_heatMode = g_prefs.getInt("heatm", 3);
  g_heatSrc = g_prefs.getInt("heats", 0) ? 1 : 0;
  g_perfOn = g_prefs.getBool("perf", false);
  g_resetAlert = g_prefs.getBool("rstal", true);
  g_pomoToday = g_prefs.getInt("pomn", 0);
  g_pomoDay = g_prefs.getLong("pomday", 0);
  g_pcSound = g_prefs.getBool("pcsnd", true);
  g_ccFocusDefer = g_prefs.getBool("ccfocus", false);
  g_ccCloseIdx = g_prefs.getInt("ccclose", 2);
  if (g_ccCloseIdx < 0 || g_ccCloseIdx > 3) g_ccCloseIdx = 2;
  g_ccAlert = g_prefs.getInt("ccal", 2);
  if (g_ccAlert < 0 || g_ccAlert > 3) g_ccAlert = 2;
  g_nightIdx = g_prefs.getInt("night", 0);
  if (g_nightIdx < 0 || g_nightIdx > 3) g_nightIdx = 0;
  g_nightPause = g_prefs.getBool("nightp", true);
  g_nightClock = g_prefs.getBool("nightclk", true);
  g_nightBri = g_prefs.getInt("nightbri", 0) ? 1 : 0;
  g_userPause = g_prefs.getBool("pause", false);
  g_pauseUntil = g_prefs.getUInt("pauseu", 0);
  g_clockIdx = g_prefs.getInt("clock", 0);
  g_pcIntIdx = g_prefs.getInt("pcint", 0);
  g_kwhPrice = g_prefs.getFloat("kwh", 0.30f);
  if (g_pcIntIdx < 0 || g_pcIntIdx > 3) g_pcIntIdx = 0;
  g_wxLat = g_prefs.getFloat("wxlat", DEFAULT_WX_LAT);
  g_wxLon = g_prefs.getFloat("wxlon", DEFAULT_WX_LON);
  strlcpy(g_wxCity, g_prefs.getString("wxcity", DEFAULT_WX_CITY).c_str(), sizeof(g_wxCity));
  strlcpy(g_pcHost, g_prefs.getString("pchost", DEFAULT_PC_HOST).c_str(), sizeof(g_pcHost));
  if (g_clockIdx < 0 || g_clockIdx > 3) g_clockIdx = 0;
  g_dimIdx = g_prefs.getInt("dim", 0);
  if (g_dimIdx < 0 || g_dimIdx > 3) g_dimIdx = 0;
  if (g_heatMode < 0 || g_heatMode > 3) g_heatMode = 3;
  g_lang = g_prefs.getInt("lang", 0) ? 1 : 0;
  models_load();
}
static void save_attempts() { g_prefs.putInt("pinatt", g_pinAttempts); }
static void apply_brightness() { if (g_screenMode == 0) ledcWrite(TFT_BL, BRI_LEVELS[g_briIdx]); }
static void screen_apply() {
  ledcWrite(TFT_BL, g_screenMode == 2 ? 0 : g_screenMode == 3 ? NIGHT_BRI[g_nightBri]
                    : (g_screenMode == 1 ? DIM_LEVEL : BRI_LEVELS[g_briIdx]));
}
// true se l'ora locale e' nella fascia notte scelta (fino alle 7:00)
static bool night_active() {
  if (!g_nightIdx) return false;
  time_t now = time(nullptr);
  if (now < 1000000000L) return false;
  struct tm tv; localtime_r(&now, &tv);
  int from = NIGHT_FROM[g_nightIdx];
  return from == 0 ? tv.tm_hour < 7 : (tv.tm_hour >= from || tv.tm_hour < 7);
}
static bool night_paused() { return g_nightPause && night_active(); }
// avviso al PC collegato (suono e notifica di Windows via Ritmo Code PC Monitor); di notte niente
static void pc_notify(const char *ev, const char *title, const char *msg) {
  if (!g_pcSound || !g_pcHost[0] || !ev[0] || night_active()) return;
  strlcpy(g_pcNtf.host, g_pcHost, sizeof(g_pcNtf.host));
  strlcpy(g_pcNtf.ev, ev, sizeof(g_pcNtf.ev));
  strlcpy(g_pcNtf.title, title, sizeof(g_pcNtf.title));
  strlcpy(g_pcNtf.msg, msg, sizeof(g_pcNtf.msg));
  g_pcNtfReq = true;
}
// niente richieste automatiche: pausa manuale o notte (il tasto ↻ aggiorna comunque)
static bool polling_paused() { return g_userPause || night_paused(); }
// Orologio notturno a tutto schermo (variante B): ora grande in grigio caldo, data e riepilogo
struct NightClockUI { lv_obj_t *scrim, *time, *date, *info; int lastMin; };
static NightClockUI g_nc = {};
static void night_clock_close() { if (g_nc.scrim) { lv_obj_delete(g_nc.scrim); memset(&g_nc, 0, sizeof(g_nc)); } }
static void night_clock_update(bool force) {
  if (!g_nc.scrim) return;
  time_t now = time(nullptr);
  if (now < 1000000000L) return;
  struct tm tv; localtime_r(&now, &tv);
  if (!force && tv.tm_min == g_nc.lastMin) return;
  g_nc.lastMin = tv.tm_min;
  char s[80];
  snprintf(s, sizeof(s), "%02d:%02d", tv.tm_hour, tv.tm_min);
  lv_label_set_text(g_nc.time, s);
  static const char *GIT[7] = {"domenica", "luned\xC3\xAC", "marted\xC3\xAC", "mercoled\xC3\xAC", "gioved\xC3\xAC", "venerd\xC3\xAC", "sabato"};
  static const char *GEN[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
  static const char *MIT[12] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};
  static const char *MEN[12] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
  if (g_lang) snprintf(s, sizeof(s), "%s, %s %d", GEN[tv.tm_wday], MEN[tv.tm_mon], tv.tm_mday);
  else        snprintf(s, sizeof(s), "%s %d %s", GIT[tv.tm_wday], tv.tm_mday, MIT[tv.tm_mon]);
  lv_label_set_text(g_nc.date, s);
  int n = 0; s[0] = 0;
  if (g_usage.ok) n += snprintf(s + n, sizeof(s) - n, TRS("5h %.0f%% " U_MIDDOT " sett. %.0f%%", "5h %.0f%% " U_MIDDOT " week %.0f%%"), g_usage.h5, g_usage.d7);
  if (g_wx.ok) {
    n += snprintf(s + n, sizeof(s) - n, "%s%.0f\xC2\xB0", n ? " " U_MIDDOT " " : "", g_wx.temp);
    for (int i = 0; i < 12; i++)
      if (g_wx.rain[i] >= 40) { snprintf(s + n, sizeof(s) - n, TRS(" pioggia alle %02d", " rain at %02d"), (g_wx.rainHour0 + i) % 24); break; }
  }
  lv_label_set_text(g_nc.info, s);
}
static void night_clock_show() {
  night_clock_close();
  lv_obj_t *s = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s);
  g_nc.scrim = s;
  lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);             // i tocchi non arrivano al dashboard sotto
  g_nc.time = lv_label_create(s);
  lv_obj_set_style_text_font(g_nc.time, F150, 0);
  lv_obj_set_style_text_color(g_nc.time, lv_color_hex(0xA8A49A), 0);
  lv_obj_align(g_nc.time, LV_ALIGN_CENTER, 0, -38);   // centro su y 122 = 320 / phi^2
  g_nc.date = lv_label_create(s);
  lv_obj_set_style_text_font(g_nc.date, F14, 0);
  lv_obj_set_style_text_color(g_nc.date, lv_color_hex(0x5C5A55), 0);
  lv_obj_align(g_nc.date, LV_ALIGN_CENTER, 0, 49);    // parte da y 199 ~ 320 / phi
  g_nc.info = lv_label_create(s);
  lv_obj_set_style_text_font(g_nc.info, F12, 0);
  lv_obj_set_style_text_color(g_nc.info, lv_color_hex(0x5C5A55), 0);
  lv_obj_align(g_nc.info, LV_ALIGN_BOTTOM_MID, 0, -21);
  g_nc.lastMin = -1;
  night_clock_update(true);
  lv_obj_align(g_nc.time, LV_ALIGN_CENTER, 0, -38);       // ricentra con il testo reale
  lv_obj_align(g_nc.date, LV_ALIGN_CENTER, 0, 49);    // parte da y 199 ~ 320 / phi
  lv_obj_align(g_nc.info, LV_ALIGN_BOTTOM_MID, 0, -21);
}
// allinea l'overlay allo stato dello schermo (fuori dalla callback del touch, dove non si toccano oggetti)
static void night_clock_sync() {
  if (g_screenMode == 3 && !g_nc.scrim) night_clock_show();
  else if (g_screenMode != 3 && g_nc.scrim) night_clock_close();
  if (g_nc.scrim) {
    static uint32_t last = 0;
    if (millis() - last > 1000) { last = millis(); night_clock_update(false); }
  }
}

// Stato dello schermo (ogni ~500 ms): notte -> orologio tenue o spento dopo 30 s senza tocchi,
// attenuazione -> dopo N minuti. Fuori dal dashboard resta sempre acceso.
static void screen_tick() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  uint32_t idle = millis() - g_lastTouchMs;
  uint8_t want = 0;
  if (g_state == ST_MAIN) {
    if (night_active() && idle > NIGHT_WAKE_MS) want = g_nightClock ? 3 : 2;
    else if (g_dimIdx && idle > DIM_MIN[g_dimIdx] * 60000UL) want = 1;
  }
  if (want != g_screenMode) {
    g_screenMode = want;
    screen_apply();
    Serial.printf("[SCREEN] %s\n", want == 3 ? "orologio (notte)" : want == 2 ? "spento (notte)" : want == 1 ? "attenuato" : "acceso");
  }
}

static void reset_history_ram();

static void factory_reset() {
  g_dataEpoch++;
  g_prefs.clear();              // cancella blob, pinatt, bri dal namespace claude
  for (int i = 0; i < NMODELS; i++) {         // gli ID personalizzati erano in NVS: torna ai predefiniti
    strlcpy(g_models[i].id, g_models[i].defId, MODEL_ID_MAX);
    g_models[i].pr.code = 0; g_models[i].pr.ms = 0;
  }
  g_wifi.forgetAll();
  for (int i = 0; i < ACCT_MAX; i++) {
    char pth[16]; snprintf(pth, sizeof(pth), "/hist%d.bin", i);
    LittleFS.remove(pth);
    snprintf(pth, sizeof(pth), "/weeks%d.bin", i);
    LittleFS.remove(pth);
  }
  memset(&g_accts, 0, sizeof(g_accts));
  memset(g_sessionPin, 0, sizeof(g_sessionPin));
  g_pendingLabel[0] = 0;
  g_tokenTargetSlot = 0;
  reset_history_ram();
  g_hasToken = false;
  g_token[0] = 0; g_pendingToken[0] = 0;
  g_pinAttempts = 0;
  g_onboarding = true;
  Serial.println("[RESET] tutto cancellato");
}

// ============================================================
// Schermata: PIN (tastierino touch) — inserisce il PIN per decifrare OPPURE ne imposta uno nuovo nel setup
// ============================================================
static const char *pin_map[] = {
  "1", "2", "3", "\n",
  "4", "5", "6", "\n",
  "7", "8", "9", "\n",
  U_BKSP, "0", U_ENTER, ""
};

static void pin_update_dots() {
  if (!g_pinDots) return;
  char dots[32] = {0};
  int len = strlen(g_pinEntry);
  strcat(dots, "[ ");
  for (int i = 0; i < PIN_LEN; i++) {
    strcat(dots, i < len ? "*" : "_");
    strcat(dots, " ");
  }
  strcat(dots, "]");
  lv_label_set_text(g_pinDots, dots);
}

static void pin_submit() {
  if (g_state == ST_SETUP_PIN) {
    if (!g_pinConfirming) {
      strlcpy(g_pinFirst, g_pinEntry, sizeof(g_pinFirst));
      g_pinConfirming = true;
      g_pinEntry[0] = 0;
      pin_update_dots();
      if (g_pinMsg) lv_label_set_text(g_pinMsg, TRS("Conferma il PIN", "Confirm the PIN"));
      return;
    }
    // conferma
    if (strcmp(g_pinFirst, g_pinEntry) != 0) {
      g_pinConfirming = false;
      g_pinFirst[0] = 0; g_pinEntry[0] = 0;
      pin_update_dots();
      if (g_pinMsg) lv_label_set_text(g_pinMsg, TRS("Non coincide. Reimpostalo.", "Didn't match. Set it again."));
      return;
    }
    // PIN impostato -> cifra il token in sospeso e salva
    if (!encryptToken(g_pendingToken, g_pinEntry, g_blob)) {
      if (g_pinMsg) lv_label_set_text(g_pinMsg, TRS("Cifratura fallita. Riprova.", "Encryption failed. Try again."));
      g_pinConfirming = false; g_pinFirst[0] = 0; g_pinEntry[0] = 0; pin_update_dots();
      return;
    }
    accountSave(g_prefs, g_accts, g_tokenTargetSlot, g_blob, g_pendingLabel);
    accountSetActive(g_prefs, g_accts, g_tokenTargetSlot);
    g_pendingLabel[0] = 0;
    strlcpy(g_sessionPin, g_pinEntry, sizeof(g_sessionPin));
    strlcpy(g_token, g_pendingToken, sizeof(g_token));
    memset(g_pendingToken, 0, sizeof(g_pendingToken));
    g_hasToken = true; g_onboarding = false;
    g_pinAttempts = 0; save_attempts();
    g_pinConfirming = false;
    memset(g_pinFirst, 0, sizeof(g_pinFirst));   // azzera davvero: le cifre restano
    memset(g_pinEntry, 0, sizeof(g_pinEntry));   // in RAM se si pulisce solo [0]
    Serial.println("[PIN] token cifrato e salvato");
    request_state(g_wifi.isConnected() ? ST_LOADING : ST_WIFI);
    return;
  }

  // conferma di "cancella tutto": serve il PIN di questa sessione
  if (g_pinForWipe) {
    char tmp[sizeof(g_token)];
    bool ok = g_sessionPin[0] ? strcmp(g_pinEntry, g_sessionPin) == 0
                              : decryptToken(g_blob, g_pinEntry, tmp, sizeof(tmp));
    memset(tmp, 0, sizeof(tmp));
    memset(g_pinEntry, 0, sizeof(g_pinEntry));
    if (ok) {
      Serial.println("[PIN] cancella tutto confermato");
      g_pinForWipe = false;
      factory_reset();
      request_state(ST_WIFI);
    } else {
      pin_update_dots();
      if (g_pinMsg) { lv_label_set_text(g_pinMsg, TRS("PIN errato: niente cancellato", "Wrong PIN: nothing erased")); }
    }
    return;
  }

  // ST_PIN: prova a decifrare
  if (decryptToken(g_blob, g_pinEntry, g_token, sizeof(g_token))) {
    g_pinAttempts = 0; save_attempts();
    strlcpy(g_sessionPin, g_pinEntry, sizeof(g_sessionPin));
    memset(g_pinEntry, 0, sizeof(g_pinEntry));
    Serial.printf("[PIN] ok, token %d chars\n", (int)strlen(g_token));
    if (!g_wifi.isConnected()) g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS);
    request_state(g_wifi.isConnected() ? ST_LOADING : ST_WIFI);
  } else {
    g_pinAttempts++; save_attempts();
    g_pinEntry[0] = 0; pin_update_dots();
    if (g_pinAttempts >= MAX_PIN_ATTEMPTS) {
      Serial.println("[PIN] limite superato -> wipe");
      factory_reset();
      request_state(ST_WIFI);
      return;
    }
    int wait = LOCKOUT_BASE_SEC * (1 << (g_pinAttempts - 1));
    if (wait > 3600) wait = 3600;
    g_lockoutUntil = millis() + (uint32_t)wait * 1000;
    if (g_pinMsg) {
      char m[64];
      snprintf(m, sizeof(m), TRS("PIN errato (%d/%d). Attendi %ds", "Wrong PIN (%d/%d). Wait %ds"),
               g_pinAttempts, MAX_PIN_ATTEMPTS, wait);
      lv_label_set_text(g_pinMsg, m);
    }
  }
}

static void pin_kb_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  if (millis() < g_lockoutUntil) return;     // bloccato
  lv_obj_t *bm = (lv_obj_t *)lv_event_get_target(e);
  uint32_t id = lv_buttonmatrix_get_selected_button(bm);
  const char *txt = lv_buttonmatrix_get_button_text(bm, id);
  if (!txt) return;
  int len = strlen(g_pinEntry);
  if (strcmp(txt, U_BKSP) == 0) {
    if (len > 0) g_pinEntry[len - 1] = 0;
    pin_update_dots();
  } else if (strcmp(txt, U_ENTER) == 0) {
    if (len == PIN_LEN) pin_submit();
  } else if (len < PIN_LEN) {
    g_pinEntry[len] = txt[0];
    g_pinEntry[len + 1] = 0;
    pin_update_dots();
    if (len + 1 == PIN_LEN) pin_submit();     // invio automatico al completamento
  }
}

static void wipe_cancel_cb(lv_event_t *e) {
  (void)e;
  g_pinForWipe = false;
  memset(g_pinEntry, 0, sizeof(g_pinEntry));
  request_state(ST_SETTINGS);
}
static void ui_pin() {
  lv_obj_t *scr = lv_screen_active();
  const char *title = (g_state == ST_SETUP_PIN)
    ? (g_pinConfirming ? TRS("conferma il PIN", "confirm the PIN") : TRS("imposta un PIN", "set a PIN"))
    : g_pinForWipe ? TRS("PIN per cancellare tutto", "PIN to erase everything")
    : TRS("inserisci il PIN", "enter the PIN");
  lv_obj_t *r = plain_obj(scr);
  lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  mklabel(r, U_SPARK " ", F14, C_ACCENT);
  mklabel(r, title, F14, C_TEXT);
  lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 12);

  g_pinDots = mklabel(scr, "", F22, C_ACCENT);
  lv_obj_align(g_pinDots, LV_ALIGN_TOP_MID, 0, 40);
  pin_update_dots();

  const char *sub = (g_state == ST_SETUP_PIN)
    ? TRS("lo inserirai a ogni avvio", "you'll type it on every boot")
    : g_pinForWipe ? TRS("token, account, reti e storico verranno cancellati", "token, accounts, networks and history will be erased")
    : TRS("serve per sbloccare il token", "needed to unlock the token");
  g_pinMsg = mklabel(scr, sub, F12, g_pinForWipe ? C_BAD : C_MUTED);
  if (g_pinForWipe)                               // si torna alle impostazioni senza cancellare
    tbtn(scr, 378, 5, 89, 32, TRS(U_LEFT " annulla", U_LEFT " cancel"), F14, C_MUTED, C_BORDER, wipe_cancel_cb, NULL);
  lv_obj_align(g_pinMsg, LV_ALIGN_TOP_MID, 0, 80);

  lv_obj_t *bm = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(bm, pin_map);
  lv_obj_set_size(bm, 260, 200);
  lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_opa(bm, 0, 0);
  lv_obj_set_style_border_width(bm, 0, 0);
  lv_obj_set_style_pad_all(bm, 0, 0);
  lv_obj_set_style_pad_gap(bm, 6, 0);
  lv_obj_set_style_text_font(bm, F22, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(bm, lv_color_hex(C_SURFACE2), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(bm, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(bm, lv_color_hex(C_BORDER), LV_PART_ITEMS);
  lv_obj_set_style_radius(bm, 4, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
  lv_obj_set_style_text_color(bm, lv_color_hex(C_TEXT), LV_PART_ITEMS);
  lv_obj_add_event_cb(bm, pin_kb_cb, LV_EVENT_VALUE_CHANGED, NULL);

  if (millis() < g_lockoutUntil && g_pinMsg) {
    int rem = (g_lockoutUntil - millis()) / 1000;
    char m[48]; snprintf(m, sizeof(m), TRS("attendi %ds", "wait %ds"), rem);
    lv_label_set_text(g_pinMsg, m);
  }
}

// ============================================================
// Schermata: WiFi (scan + tastiera)
// ============================================================
static lv_obj_t *wifi_list = nullptr, *wifi_ta = nullptr, *wifi_kb = nullptr, *wifi_status = nullptr;
static char sel_ssid[33] = {0};
static WiFiManager::NetworkInfo g_nets[12];
static void wifi_item_cb(lv_event_t *e);

static void wifi_populate() {
  lv_obj_clean(wifi_list);
  lv_label_set_text(wifi_status, TRS("ricerca reti...", "scanning networks..."));
  lv_refr_now(NULL);
  int n = g_wifi.scanNetworks(g_nets, 12);
  for (int i = 0; i < n; i++) {
    const char *sig = g_nets[i].rssi > -55 ? "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86\xE2\x96\x88"
                    : g_nets[i].rssi > -67 ? "\xE2\x96\x82\xE2\x96\x84\xE2\x96\x86"
                    : g_nets[i].rssi > -78 ? "\xE2\x96\x82\xE2\x96\x84" : "\xE2\x96\x82";
    kv_row(wifi_list, g_nets[i].ssid, sig, C_TEXT, C_ACCENT, wifi_item_cb, (void *)(intptr_t)i);
  }
  lv_label_set_text(wifi_status, n > 0 ? TRS("tocca la tua rete", "tap your network")
                                       : TRS("nessuna rete: tocca cerca", "no networks: tap scan"));
}
static void wifi_item_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= 12) return;
  strlcpy(sel_ssid, g_nets[i].ssid, sizeof(sel_ssid));
  lv_label_set_text_fmt(wifi_status, TRS("password di \"%s\":", "password for \"%s\":"), sel_ssid);
  lv_obj_add_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
  lv_textarea_set_text(wifi_ta, "");
  lv_obj_clear_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(wifi_kb, wifi_ta);
}
static void wifi_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char *pass = lv_textarea_get_text(wifi_ta);
    lv_label_set_text(wifi_status, TRS("connessione...", "connecting..."));
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    bool ok = g_wifi.connectTo(sel_ssid, pass, 15000);
    if (ok) request_state(g_onboarding ? ST_TOKEN : ST_LOADING);
    else {
      lv_label_set_text(wifi_status, TRS("non riuscito: tocca di nuovo una rete", "failed: tap a network again"));
      lv_obj_clear_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
    }
  } else if (code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_status, TRS("tocca la tua rete", "tap your network"));
  }
}
static void wifi_rescan_cb(lv_event_t *e) { (void)e; wifi_populate(); }

static void ui_wifi() {
  lv_obj_t *scr = lv_screen_active();
  bool canBack = !g_onboarding && g_hasToken;
  thead(scr, TRS("configura wifi", "configure wifi"), canBack ? (g_usage.ok ? ST_MAIN : ST_SETTINGS) : -1);
  tbtn(scr, canBack ? 278 : 375, 5, 92, 32, TRS(U_REFRESH " cerca", U_REFRESH " scan"), F14, C_ACCENT, C_BORDER, wifi_rescan_cb, NULL);

  wifi_status = mklabel(scr, "...", F12, C_MUTED);
  lv_obj_set_pos(wifi_status, 13, 50);

  wifi_list = tlist(scr, 13, 72, 454, 244);

  wifi_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(wifi_ta, true);
  lv_textarea_set_password_mode(wifi_ta, true);
  lv_textarea_set_placeholder_text(wifi_ta, TRS("password del wifi", "wifi password"));
  lv_obj_set_size(wifi_ta, 456, 40);
  lv_obj_set_pos(wifi_ta, 12, 72);
  style_ta(wifi_ta);
  lv_obj_add_flag(wifi_ta, LV_OBJ_FLAG_HIDDEN);

  wifi_kb = lv_keyboard_create(scr);
  style_kb(wifi_kb);
  lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(wifi_kb, wifi_kb_cb, LV_EVENT_ALL, NULL);

  wifi_populate();
}

// ============================================================
// WebServer: token (onboarding) + pagina /models (dashboard)
// ============================================================
static WebServer *g_web = nullptr;
static volatile bool g_tokenGot = false;
static lv_obj_t *g_tokMsg = nullptr;            // stato sullo schermo del dispositivo

static void stop_web() { if (g_web) { g_web->stop(); delete g_web; g_web = nullptr; } }

static bool g_mdnsUp = false;
static void ensure_mdns() {
  if (g_mdnsUp || !g_wifi.isConnected()) return;
  if (MDNS.begin("ritmo-code")) {
    MDNS.addService("http", "tcp", 80);
    g_mdnsUp = true;
    Serial.println("[MDNS] ritmo-code.local");
  }
}

static void anim_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

// Clawd ufficiale (pixel-art) con "respiro" — sostituisce il vecchio sole a raggi.
static lv_obj_t *build_claude_mark(lv_obj_t *parent) {
  lv_obj_t *img = lv_image_create(parent);
  lv_image_set_src(img, &img_clawd_big);
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, img);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, 140, 255);
  lv_anim_set_duration(&a, 900);
  lv_anim_set_playback_duration(&a, 900);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  return img;
}

// ---- pagine HTML ----
#define WEB_CSS \
  ":root{--bg:#0F0F12;--card:#1A1A20;--bd:#30303A;--tx:#F2F0EC;--mut:#8C8C98;--cor:#D97757}" \
  "*{box-sizing:border-box}" \
  "body{margin:0;background:var(--bg);color:var(--tx);font-family:-apple-system,Segoe UI,Roboto,sans-serif;" \
  "display:flex;min-height:100vh;align-items:center;justify-content:center}" \
  ".card{background:var(--card);border:1px solid var(--bd);border-radius:16px;padding:26px;max-width:520px;width:92%}" \
  "h1{font-size:19px;margin:0 0 6px;display:flex;align-items:center;gap:10px}" \
  "p{color:var(--mut);font-size:14px;line-height:1.5;margin:6px 0 14px}" \
  "textarea{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--bd);border-radius:10px;" \
  "padding:12px;font-family:ui-monospace,monospace;font-size:13px;min-height:96px;resize:vertical}" \
  "input{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--bd);border-radius:10px;" \
  "padding:12px;font-size:14px;margin-bottom:10px}" \
  "button{margin-top:14px;width:100%;background:var(--cor);color:#1A1A20;border:0;border-radius:10px;" \
  "padding:14px;font-size:16px;font-weight:700;cursor:pointer}" \
  ".spark{width:26px;height:26px;flex:0 0 auto}code,a{color:var(--cor)}"

#define WEB_SPARK \
  "<svg class=spark viewBox='0 0 100 100'><g stroke='#D97757' stroke-width='12' stroke-linecap='round'>" \
  "<line x1=50 y1=9 x2=50 y2=91/><line x1=9 y1=50 x2=91 y2=50/>" \
  "<line x1=21 y1=21 x2=79 y2=79/><line x1=79 y1=21 x2=21 y2=79/>" \
  "<line x1=34 y1=11 x2=66 y2=89/><line x1=66 y1=11 x2=34 y2=89/></g></svg>"

static String web_form() {
  String h = F("<!doctype html><html lang=it><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Ritmo Code</title><style>" WEB_CSS "</style></head><body><div class=card>"
               "<h1>" WEB_SPARK " Ritmo Code</h1>"
               "<p>Incolla il tuo token OAuth di Claude (<code>sk-ant-oat01-...</code>) e tocca <b>Salva</b>. "
               "Il dispositivo <b>verificherà</b> il token e chiederà un PIN sullo schermo.</p>"
               "<form method=POST action='/token'>"
               "<input name=label maxlength=16 placeholder='etichetta account (es.: Personale, Lavoro)' autocomplete=off>"
               "<textarea name=token placeholder='sk-ant-oat01-...' autocomplete=off autofocus></textarea>"
               "<button type=submit>Salva e verifica</button></form></div></body></html>");
  return h;
}
static String web_result(bool ok, const String &msg) {
  String h = F("<!doctype html><html lang=it><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Ritmo Code</title><style>" WEB_CSS "</style></head><body><div class=card>");
  if (ok) {
    h += F("<h1>" WEB_SPARK " Token verificato</h1>"
           "<p>Token accettato dalla API. Ora <b>imposta un PIN di 4 cifre</b> sullo schermo del dispositivo per completare. "
           "Puoi chiudere questa pagina.</p>");
  } else {
    h += F("<h1>" WEB_SPARK " Token rifiutato</h1><p>");
    h += msg;
    h += F("</p><p><a href='/'>Torna indietro e riprova</a></p>");
  }
  h += F("</div></body></html>");
  return h;
}

static void handleRoot()     { g_web->send(200, "text/html; charset=utf-8", web_form()); }
static void handleNotFound() { g_web->sendHeader("Location", "/"); g_web->send(302, "text/plain", ""); }

static void handleTokenPost() {
  String lb = g_web->arg("label");
  lb.trim();
  strlcpy(g_pendingLabel, lb.c_str(), sizeof(g_pendingLabel));
  String t = g_web->arg("token");
  t.trim();
  if (t.length() < 8) {
    if (g_tokMsg) lv_label_set_text(g_tokMsg, TRS("token vuoto", "empty token"));
    g_web->send(200, "text/html; charset=utf-8", web_result(false, "Token vuoto o troppo corto."));
    return;
  }
  if (g_netBusy) {                            // un aggiornamento e' ancora in corso
    g_web->send(200, "text/html; charset=utf-8",
                web_result(false, "Il dispositivo sta finendo un aggiornamento: riprova tra qualche secondo."));
    return;
  }
  // feedback sul dispositivo prima della chiamata bloccante
  if (g_tokMsg) { lv_label_set_text(g_tokMsg, TRS("verifica del token...", "validating token...")); lv_refr_now(NULL); }

  UsageData tmp = {};
  bool ok = fetchUsage(t.c_str(), tmp);
  if (ok) {
    strlcpy(g_pendingToken, t.c_str(), sizeof(g_pendingToken));
    g_usage = tmp;                              // abbiamo gia' i dati per il dashboard
    g_pinConfirming = false; g_pinFirst[0] = 0; g_pinEntry[0] = 0;
    g_tokenGot = true;                          // loop -> ST_SETUP_PIN
    if (g_tokMsg) lv_label_set_text(g_tokMsg, TRS("token OK! imposta il PIN", "token OK! set the PIN"));
    g_web->send(200, "text/html; charset=utf-8", web_result(true, ""));
  } else {
    String m = String("La API ha rifiutato il token (") + tmp.error + "). Controllalo e incollalo di nuovo.";
    if (g_tokMsg) lv_label_set_text(g_tokMsg, TRS("token rifiutato, riprova", "token rejected, try again"));
    g_web->send(200, "text/html; charset=utf-8", web_result(false, m));
  }
}

// ---- / : pagina di stato (stesso tema Terminale del display) ----
// Legge solo /api/status (memoria del dispositivo): nessuna richiesta ad Anthropic, token mai esposto.
// (HTML in status_page.h: il preprocessore Arduino non gestisce le raw string nel .ino)
static void handleInfo() { g_web->send_P(200, "text/html; charset=utf-8", STATUS_PAGE); }

static bool model_up(int i);
static bool night_paused();
static void json_str(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') o += '\\';
    if ((uint8_t)*s >= 0x20) o += *s;
  }
  o += '"';
}
static int model_mood(int i);
static void heat_mode_data(int mode, float out[24]);
// Dati del PC in JSON (pannello web): stessi valori della pagina pc, con i sensori.
// hist = true aggiunge i campioni dei grafici (endpoint /api/pc, letto spesso dalla pagina pc).
static void pc_json(String &o, bool hist) {
  char b[200];
  bool ok = g_pc.ok && g_pcAtMs && millis() - g_pcAtMs <= pc_stale_ms();
  snprintf(b, sizeof(b), "{\"ok\":%s,\"age\":%lu,\"int\":%u,\"kwh\":%.3f,\"host\":", ok ? "true" : "false",
           (unsigned long)(g_pcAtMs ? millis() - g_pcAtMs : 0), (unsigned)PC_INT_S[g_pcIntIdx], g_kwhPrice);
  o += b;
  json_str(o, g_pcHost);
  o += ",\"cpu_name\":"; json_str(o, g_pc.cpuName);
  o += ",\"gpu_name\":"; json_str(o, g_pc.gpuName);
  snprintf(b, sizeof(b), ",\"cpu\":%.1f,\"cpu_t\":%.1f,\"cpu_mhz\":%.0f,\"cpu_w\":%.1f,\"core_max\":%.0f,\"volt\":%.3f,",
           g_pc.cpu, g_pc.cpuTemp, g_pc.cpuMhz, g_pc.cpuPower, g_pc.cpuCoreMax, g_pc.cpuVolt);
  o += b;
  snprintf(b, sizeof(b), "\"gpu\":%.1f,\"gpu_t\":%.1f,\"gpu_w\":%.1f,\"gpu_hot\":%.1f,\"gpu_mem_t\":%.1f,\"gpu_mhz\":%.0f,\"gpu_mem_load\":%.0f,",
           g_pc.gpu, g_pc.gpuTemp, g_pc.gpuPower, g_pc.gpuHotspot, g_pc.gpuMemTemp, g_pc.gpuClockMhz, g_pc.gpuMemLoad);
  o += b;
  snprintf(b, sizeof(b), "\"ram\":%.1f,\"ram_used_gb\":%.2f,\"ram_total_gb\":%.2f,\"vram_used_gb\":%.2f,\"vram_total_gb\":%.2f,\"disk\":%.0f,",
           g_pc.ram, g_pc.ramUsedMb / 1024.0f, g_pc.ramTotalMb / 1024.0f, g_pc.vramUsedMb / 1024.0f, g_pc.vramTotalMb / 1024.0f, g_pc.disk);
  o += b;
  snprintf(b, sizeof(b), "\"net_down\":%.0f,\"net_up\":%.0f,\"disk_r\":%.0f,\"disk_w\":%.0f,\"uptime\":%u,\"claude\":%d,\"disks\":[",
           g_pc.netDown, g_pc.netUp, g_pc.diskRead, g_pc.diskWrite, (unsigned)g_pc.uptime, g_pc.claude);
  o += b;
  for (int d = 0; d < g_pc.diskN; d++) {
    snprintf(b, sizeof(b), "%s[\"%c\",%.1f,%.1f,%.1f]", d ? "," : "", g_pc.disks[d].letter, g_pc.disks[d].used,
             g_pc.disks[d].freeGb, g_pc.disks[d].totalGb);
    o += b;
  }
  o += "],\"fans\":[";
  for (int i = 0; i < g_pc.fanN; i++) {
    o += i ? ",[" : "["; json_str(o, g_pc.fans[i].name);
    snprintf(b, sizeof(b), ",%d]", g_pc.fans[i].rpm); o += b;
  }
  o += "],\"board\":[";
  for (int i = 0; i < g_pc.boardN; i++) {
    o += i ? ",[" : "["; json_str(o, g_pc.board[i].name);
    snprintf(b, sizeof(b), ",%.1f]", g_pc.board[i].value); o += b;
  }
  o += "],\"ram_t\":[";
  for (int i = 0; i < g_pc.ramTempN; i++) { snprintf(b, sizeof(b), "%s%.1f", i ? "," : "", g_pc.ramTemps[i]); o += b; }
  o += "],\"drives\":[";
  for (int i = 0; i < g_pc.driveN; i++) {
    o += i ? ",[" : "["; json_str(o, g_pc.drives[i].name);
    snprintf(b, sizeof(b), ",%.1f,%.1f]", g_pc.drives[i].temp, g_pc.drives[i].life); o += b;
  }
  o += "]";
  if (hist) {
    o += ",\"hist\":[";
    for (int k = 0; k < 3; k++) {
      o += k ? ",[" : "[";
      for (int i = 0; i < g_pcHistN; i++) { snprintf(b, sizeof(b), "%s%u", i ? "," : "", g_pcHist[k][i]); o += b; }
      o += "]";
    }
    o += "]";
  }
  o += "}";
}
static void handleApiPc() {
  String o;
  o.reserve(4000);
  pc_json(o, true);
  g_web->sendHeader("Cache-Control", "no-store");
  g_web->send(200, "application/json", o);
}
static void handleApiStatus() {
  String o;
  o.reserve(9000);
  time_t now = time(nullptr);
  char b[160];
  uint32_t updated = (g_lastOkMs && now > 1000000000L) ? (uint32_t)(now - (millis() - g_lastOkMs) / 1000) : 0;
  snprintf(b, sizeof(b), "{\"fw\":\"" FW_VERSION "\",\"now\":%ld,\"ok\":%s,\"h5\":%.1f,\"d7\":%.1f,\"h5_reset\":%u,\"d7_reset\":%u,",
           (long)now, g_usage.ok ? "true" : "false", g_usage.h5, g_usage.d7,
           (unsigned)g_usage.h5ResetEpoch, (unsigned)g_usage.d7ResetEpoch);
  o += b;
  snprintf(b, sizeof(b), "\"updated\":%u,\"refreshing\":%s,\"night\":%s,\"paused\":%s,\"status\":",
           (unsigned)updated, g_refreshing ? "true" : "false", night_paused() ? "true" : "false", g_userPause ? "true" : "false");
  o += b;
  json_str(o, g_usage.statusOverall);
  o += ",\"account\":";
  json_str(o, g_accts.label[g_accts.active]);
  o += ",\"models\":[";
  for (int i = 0; i < NMODELS; i++) {
    if (i) o += ',';
    o += "{\"name\":"; json_str(o, g_models[i].name);
    o += ",\"id\":";   json_str(o, g_models[i].id);
    long age = g_models[i].atMs ? (long)((millis() - g_models[i].atMs) / 1000) : -1;
    snprintf(b, sizeof(b), ",\"code\":%d,\"ms\":%u,\"up\":%s,\"mood\":%d,\"age\":%ld}",
             g_models[i].pr.code, (unsigned)g_models[i].pr.ms, model_up(i) ? "true" : "false", model_mood(i), age);
    o += b;
  }
  o += "],\"hist\":[";
  bool first = true;
  for (int i = 0; i < g_histN; i++) {
    const Sample &s = g_hist[(g_histHead - g_histN + i + HIST_MAX) % HIST_MAX];
    if (!s.t) continue;
    snprintf(b, sizeof(b), "%s[%u,%u,%u]", first ? "" : ",", (unsigned)s.t, s.h5, s.d7);
    o += b; first = false;
  }
  // ritmo orario per periodo (oggi, 7g, 30g, tutto): come la pagina ritmo del display
  o += "],\"heat\":[";
  for (int m = 0; m < 4; m++) {
    float d[24];
    heat_mode_data(m, d);
    o += m ? ",[" : "[";
    for (int h = 0; h < 24; h++) { snprintf(b, sizeof(b), "%s%.1f", h ? "," : "", d[h]); o += b; }
    o += "]";
  }
  o += "],\"weeks\":[";
  for (int i = 0; i < g_weekN; i++) {
    snprintf(b, sizeof(b), "%s[%u,%u]", i ? "," : "", (unsigned)g_weeks[i].reset, g_weeks[i].peak);
    o += b;
  }
  snprintf(b, sizeof(b), "],\"pause_until\":%u,\"heat_mode\":%d,", (unsigned)g_pauseUntil, g_heatMode);
  o += b;
  {                                                // timer e pomodoro (per il menu dell'icona sul PC)
    char tl[40] = "";
    if (g_tmMode) tm_label(tl, sizeof(tl));
    snprintf(b, sizeof(b), "\"timer\":{\"mode\":%d,\"left\":%u,\"label\":\"%s\"},", g_tmMode,
             (unsigned)(g_tmMode ? tm_left_s() : 0), tl);
    o += b;
  }
  snprintf(b, sizeof(b), "\"wx\":{\"ok\":%s,\"city\":", g_wx.ok ? "true" : "false");
  o += b;
  json_str(o, g_wxCity);
  snprintf(b, sizeof(b), ",\"temp\":%.1f,\"code\":%d,\"day\":%s,\"tmax\":%.1f,\"tmin\":%.1f,\"tmax2\":%.1f,\"tmin2\":%.1f,\"code2\":%d,",
           g_wx.temp, g_wx.code, g_wx.isDay ? "true" : "false", g_wx.tmax, g_wx.tmin, g_wx.tmax2, g_wx.tmin2, g_wx.code2);
  o += b;
  snprintf(b, sizeof(b), "\"sunrise\":\"%s\",\"sunset\":\"%s\",\"rain_h0\":%d,\"rain\":[", g_wx.sunrise, g_wx.sunset, g_wx.rainHour0);
  o += b;
  for (int i = 0; i < 12; i++) { snprintf(b, sizeof(b), "%s%u", i ? "," : "", g_wx.rain[i]); o += b; }
  o += "]},\"pc\":";
  pc_json(o, false);
  o += "}";
  g_web->sendHeader("Cache-Control", "no-store");
  g_web->send(200, "application/json", o);
}
// ---- /models: ID dei modelli sondati, modificabili dal browser ----
static String models_page(const String &msg, bool ok) {
  String h = F("<!doctype html><html lang=it><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Modelli - Ritmo Code</title><style>" WEB_CSS
               "label{display:block;font-size:13px;color:var(--mut);margin:12px 0 4px}"
               "input{margin-bottom:0;font-family:ui-monospace,monospace}"
               ".msg{padding:10px 12px;border-radius:10px;margin:0 0 12px;font-size:14px}"
               ".ok{background:#4ADE8022;color:#4ADE80}.err{background:#F8717122;color:#F87171}"
               "</style></head><body><div class=card>"
               "<h1>" WEB_SPARK " Modelli sondati</h1>"
               "<p>Il dispositivo controlla un modello per ciclo con l'ID indicato qui. "
               "Se Anthropic cambia i nomi, aggiornali copiandoli dalla "
               "<a href='https://docs.anthropic.com/en/docs/about-claude/models' target=_blank>documentazione</a>. "
               "Lascia vuoto per tornare al predefinito.</p>");
  if (msg.length()) {
    h += ok ? "<div class='msg ok'>" : "<div class='msg err'>";
    h += msg;
    h += "</div>";
  }
  h += F("<form method=POST action='/models'>");
  for (int i = 0; i < NMODELS; i++) {
    h += "<label for=m"; h += i; h += ">"; h += g_models[i].name; h += "</label>";
    h += "<input id=m"; h += i; h += " name=m"; h += i;
    h += " maxlength=47 autocomplete=off autocapitalize=off spellcheck=false value='";
    h += g_models[i].id;                      // gia' validato: solo a-z 0-9 . - _
    h += "' placeholder='"; h += g_models[i].defId; h += "'>";
  }
  h += F("<label for=pin>PIN del dispositivo</label>"
         "<input id=pin name=pin type=password inputmode=numeric maxlength=4 autocomplete=off>"
         "<button type=submit>Salva</button></form></div></body></html>");
  return h;
}
static void handleModelsGet() { g_web->send(200, "text/html; charset=utf-8", models_page("", true)); }
// PIN richiesto per modificare dal browser: 5 errori bloccano per 5 minuti
static int g_webPinBad = 0;
static uint32_t g_webPinLockMs = 0;
static bool web_pin_ok(String &err) {
  if (g_webPinLockMs && millis() - g_webPinLockMs < 5UL * 60UL * 1000UL) {
    err = "Troppi PIN errati: riprova tra qualche minuto."; return false;
  }
  g_webPinLockMs = 0;
  if (!g_sessionPin[0]) { err = "Sblocca prima il dispositivo con il PIN."; return false; }
  if (g_web->arg("pin") != g_sessionPin) {
    if (++g_webPinBad >= 5) { g_webPinBad = 0; g_webPinLockMs = millis(); }
    Serial.println("[WEB] PIN errato su /models");
    err = "PIN errato: nessuna modifica salvata."; return false;
  }
  g_webPinBad = 0;
  return true;
}
static void handleModelsPost() {
  String perr;
  if (!web_pin_ok(perr)) { g_web->send(403, "text/html; charset=utf-8", models_page(perr, false)); return; }
  String bad;
  for (int i = 0; i < NMODELS; i++) {
    char arg[4]; snprintf(arg, sizeof(arg), "m%d", i);
    if (!g_web->hasArg(arg)) continue;
    if (!model_set_id(i, g_web->arg(arg).c_str())) {
      if (bad.length()) bad += ", ";
      bad += g_models[i].name;
    }
  }
  if (g_state == ST_MAIN || g_state == ST_MODELS) request_state(g_state);   // ridisegna mascotte/lista
  if (bad.length())
    g_web->send(200, "text/html; charset=utf-8",
                models_page("ID non valido per " + bad + ": usa solo a-z, 0-9, punto, trattino e underscore (3-47 caratteri).", false));
  else
    g_web->send(200, "text/html; charset=utf-8",
                models_page("Salvato. Il controllo usa i nuovi ID dal prossimo giro, senza richieste extra.", true));
}

// ---- /update: firmware via Wi-Fi ----
// Attivo solo con la schermata "aggiorna firmware" aperta sul dispositivo: serve il codice
// di 6 cifre mostrato sullo schermo (presenza fisica). 5 codici sbagliati chiudono la sessione.
#define OTA_WINDOW_MS (5UL * 60UL * 1000UL)
static char g_otaCode[8] = {0};
static uint32_t g_otaStartMs = 0;
static int g_otaBad = 0;
static bool g_otaBusy = false, g_otaAuth = false, g_otaDone = false;
static uint32_t g_otaRebootAt = 0;
static size_t g_otaBytes = 0;
static String g_otaErr;
static lv_obj_t *g_otaStat = nullptr, *g_otaBar = nullptr, *g_otaTime = nullptr;

static bool ota_open() { return g_state == ST_OTA && g_otaCode[0]; }
static void ota_status(const char *txt, uint32_t col) {
  if (g_otaStat) { lv_label_set_text(g_otaStat, txt); lv_obj_set_style_text_color(g_otaStat, lv_color_hex(col), 0); }
}
static void handleUpdateGet() {
  String h = F("<!doctype html><html lang=it><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Aggiorna - Ritmo Code</title><style>" WEB_CSS
               "label{display:block;font-size:13px;color:var(--mut);margin:12px 0 4px}"
               "input[type=file]{padding:10px}"
               "progress{width:100%;height:10px;margin-top:14px;accent-color:var(--cor)}"
               "#msg{margin-top:12px;font-size:14px;color:var(--mut)}"
               "</style></head><body><div class=card>"
               "<h1>" WEB_SPARK " Aggiorna firmware</h1>");
  if (!ota_open()) {
    h += F("<p>Sul dispositivo apri <b>Impostazioni &rarr; aggiorna firmware</b>: comparira' un codice "
           "di 6 cifre valido 5 minuti. Poi ricarica questa pagina.</p></div></body></html>");
    g_web->send(200, "text/html; charset=utf-8", h);
    return;
  }
  h += F("<p>Firmware attuale v" FW_VERSION ". Scegli il file <code>ritmo-code-x.y.bin</code> della release "
         "(o <code>ritmo_code.ino.bin</code> da <code>firmware/ritmo_code/build/...</code> se lo compili tu) "
         "e inserisci il codice mostrato sullo schermo.</p>"
         "<label for=code>Codice</label><input id=code inputmode=numeric maxlength=6 autocomplete=off>"
         "<label for=bin>File firmware (.bin)</label><input id=bin type=file accept='.bin'>"
         "<button id=go>Carica</button><progress id=pr max=100 value=0 hidden></progress><div id=msg></div>"
         "<script>"
         "go.onclick=function(){var f=bin.files[0];if(!f){msg.textContent='Scegli un file .bin';return}"
         "var fd=new FormData();fd.append('firmware',f);var x=new XMLHttpRequest();"
         "x.open('POST','/update?code='+encodeURIComponent(code.value));pr.hidden=false;go.disabled=true;"
         "x.upload.onprogress=function(e){if(e.lengthComputable)pr.value=e.loaded*100/e.total};"
         "x.onload=function(){msg.textContent=x.responseText;go.disabled=false};"
         "x.onerror=function(){msg.textContent='Connessione interrotta';go.disabled=false};x.send(fd)}"
         "</script></div></body></html>");
  g_web->send(200, "text/html; charset=utf-8", h);
}
static void handleUpdateUpload() {
  HTTPUpload &up = g_web->upload();
  if (up.status == UPLOAD_FILE_START) {
    g_otaAuth = false; g_otaErr = ""; g_otaBytes = 0;
    if (!ota_open())                                   { g_otaErr = "Schermata di aggiornamento chiusa sul dispositivo"; return; }
    if (g_web->arg("code") != g_otaCode) {
      g_otaErr = "Codice errato";
      if (++g_otaBad >= 5) { g_otaCode[0] = 0; g_otaErr = "Troppi tentativi: riapri la schermata sul dispositivo"; }
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))   { g_otaErr = String("Avvio fallito: ") + Update.errorString(); return; }
    g_otaAuth = true; g_otaBusy = true;
    Serial.printf("[OTA] ricezione %s\n", up.filename.c_str());
    ota_status(TRS("ricezione in corso...", "receiving..."), C_WARN);
    lv_timer_handler();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!g_otaAuth) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      g_otaErr = String("Scrittura fallita: ") + Update.errorString();
      Update.abort(); g_otaAuth = false; g_otaBusy = false;
      return;
    }
    size_t before = g_otaBytes;
    g_otaBytes += up.currentSize;
    if (g_otaBytes / 65536 != before / 65536) {        // ~ogni 64 KB: aggiorna lo schermo
      char s[40]; snprintf(s, sizeof(s), TRS("ricevuti %u KB", "received %u KB"), (unsigned)(g_otaBytes / 1024));
      ota_status(s, C_WARN);
      // dimensione finale ignota: stima sul firmware attuale (le versioni hanno taglie simili)
      uint32_t est = ESP.getSketchSize();
      if (g_otaBar && est) lv_bar_set_value(g_otaBar, (int)min<uint64_t>(99, (uint64_t)g_otaBytes * 100 / est), LV_ANIM_OFF);
      lv_timer_handler();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!g_otaAuth) return;
    if (Update.end(true)) {
      g_otaDone = true;
      Serial.printf("[OTA] ok, %u byte\n", (unsigned)up.totalSize);
    } else {
      g_otaErr = String("Verifica fallita: ") + Update.errorString();
    }
    g_otaAuth = false; g_otaBusy = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (g_otaAuth) Update.abort();
    g_otaAuth = false; g_otaBusy = false;
    g_otaErr = "Caricamento interrotto";
  }
}
static void handleUpdatePost() {
  if (g_otaDone) {
    g_web->send(200, "text/plain; charset=utf-8", "Aggiornamento completato: il dispositivo si riavvia.");
    ota_status(TRS("completato " U_MIDDOT " riavvio...", "done " U_MIDDOT " rebooting..."), C_OK);
    if (g_otaBar) lv_bar_set_value(g_otaBar, 100, LV_ANIM_OFF);
    g_otaRebootAt = millis() + 1500;
    return;
  }
  if (!g_otaErr.length()) g_otaErr = "Nessun file ricevuto";
  Serial.printf("[OTA] rifiutato: %s\n", g_otaErr.c_str());
  ota_status(g_otaErr.c_str(), C_BAD);
  g_web->send(g_otaErr == "Codice errato" ? 403 : 400, "text/plain; charset=utf-8", g_otaErr);
}

// ---- /home: citta' del meteo e indirizzo del PC (ricerca citta' fatta dal browser) ----
static String home_page(const String &msg, bool ok) {
  String h = F("<!doctype html><html lang=it><head><meta charset=utf-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Home - Ritmo Code</title><style>" WEB_CSS
               "label{display:block;font-size:13px;color:var(--mut);margin:12px 0 4px}"
               "input,select{margin-bottom:0}select{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--bd);border-radius:10px;padding:12px}"
               ".msg{padding:10px 12px;border-radius:10px;margin:0 0 12px;font-size:14px}"
               ".ok{background:#4ADE8022;color:#4ADE80}.err{background:#F8717122;color:#F87171}"
               ".row{display:flex;gap:8px}.row button{margin-top:0;width:auto;padding:0 16px}"
               "</style></head><body><div class=card>"
               "<h1>" WEB_SPARK " Pagina home</h1>"
               "<p>Citta' per il meteo (Open-Meteo, gratuito) e indirizzo del PC con Ritmo Code PC Monitor (lo imposta anche l'app stessa con &quot;Collega questo PC&quot;).</p>");
  if (msg.length()) { h += ok ? "<div class='msg ok'>" : "<div class='msg err'>"; h += msg; h += "</div>"; }
  char num[24];
  h += F("<form method=POST action='/home' id=f><label for=q>Citta'</label><div class=row>"
         "<input id=q autocomplete=off value='");
  h += g_wxCity;
  h += F("'><button type=button id=s>Cerca</button></div><select id=r hidden></select>"
         "<input type=hidden name=city id=city value='");
  h += g_wxCity;
  h += F("'><input type=hidden name=lat id=lat value='");
  snprintf(num, sizeof(num), "%.4f", g_wxLat); h += num;
  h += F("'><input type=hidden name=lon id=lon value='");
  snprintf(num, sizeof(num), "%.4f", g_wxLon); h += num;
  h += F("'><label for=pc>Indirizzo del PC (ip:porta, vuoto per nasconderlo)</label>"
         "<input id=pc name=pc maxlength=47 autocomplete=off placeholder='192.168.1.10:8080' value='");
  h += g_pcHost;
  h += F("'><label for=kwh>Prezzo dell'energia (euro per kWh, per il costo del PC)</label>"
         "<input id=kwh name=kwh inputmode=decimal maxlength=6 autocomplete=off value='");
  snprintf(num, sizeof(num), "%.3f", g_kwhPrice); h += num;
  h += F("'><label for=pin>PIN del dispositivo</label>"
         "<input id=pin name=pin type=password inputmode=numeric maxlength=4 autocomplete=off>"
         "<button type=submit>Salva</button></form><p><a href='/'>&larr; pannello</a></p>"
         "<script>var R=[];s.onclick=function(){fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=it&name='+encodeURIComponent(q.value))"
         ".then(function(x){return x.json()}).then(function(j){R=j.results||[];r.hidden=!R.length;"
         "r.innerHTML=R.map(function(c,i){return '<option value='+i+'>'+c.name+(c.admin1?', '+c.admin1:'')+(c.country?' ('+c.country+')':'')+'</option>'}).join('');"
         "if(R.length)pick(0);else alert('Nessuna citta trovata')})};"
         "function pick(i){var c=R[i];city.value=c.name;lat.value=c.latitude.toFixed(4);lon.value=c.longitude.toFixed(4);q.value=c.name}"
         "r.onchange=function(){pick(+r.value)}</script></div></body></html>");
  return h;
}
static void handleHomeGet() { g_web->send(200, "text/html; charset=utf-8", home_page("", true)); }
// l'app Ritmo Code PC Monitor comunica il proprio indirizzo (serve il PIN)
static void handlePcPair() {
  String perr;
  if (!web_pin_ok(perr)) {
    String j = "{\"ok\":false,\"error\":\""; j += perr; j += "\"}";
    g_web->send(403, "application/json", j);
    return;
  }
  String pc = g_web->arg("pc");
  bool ok = pc.length() > 0 && pc.length() < 48;
  for (size_t i = 0; i < pc.length() && ok; i++) {
    char c = pc[i];
    ok = isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-';
  }
  if (!ok) { g_web->send(400, "application/json", "{\"ok\":false,\"error\":\"indirizzo del pc non valido\"}"); return; }
  strlcpy(g_pcHost, pc.c_str(), sizeof(g_pcHost));
  g_prefs.putString("pchost", g_pcHost);
  g_pc.ok = false; g_pcAtMs = 0; g_pcTryMs = 0; g_pcHistN = 0;
  Serial.printf("[PC] collegato dall'app: %s\n", g_pcHost);
  home_redraw();
  g_web->send(200, "application/json", "{\"ok\":true}");
}
// evento degli hook di Claude Code inoltrato da Ritmo Code PC Monitor: solo dal PC collegato
static void handleClaudeEvent() {
  String host = g_pcHost;
  int c = host.indexOf(':');
  if (c >= 0) host.remove(c);
  IPAddress pc;
  if (!g_pcHost[0] || (pc.fromString(host) && g_web->client().remoteIP() != pc)) {
    g_web->send(403, "application/json", "{\"ok\":false}");
    return;
  }
  String ev = g_web->arg("ev"), proj = g_web->arg("proj");
  if (ev != "busy" && ev != "done" && ev != "perm" && ev != "ask") {
    g_web->send(400, "application/json", "{\"ok\":false}");
    return;
  }
  char pj[28]; size_t n = 0;
  for (size_t i = 0; i < proj.length() && n < sizeof(pj) - 1; i++) {
    char ch = proj[i];
    if (isalnum((unsigned char)ch) || ch == '.' || ch == '_' || ch == '-' || ch == ' ') pj[n++] = ch;
  }
  pj[n] = 0;
  if (g_web->hasArg("busy")) { g_ccBusyN = g_web->arg("busy").toInt(); cc_busy_ui(); }
  cc_event(g_web->arg("id").toInt(), ev.c_str(), pj, g_web->hasArg("dur") ? g_web->arg("dur").toInt() : -1, 0);
  g_web->send(204, "text/plain", "");
}
// timer e pomodoro dal PC (menu dell'icona di Ritmo Code PC Monitor): solo dal PC collegato
static void handleTimerCmd() {
  String host = g_pcHost;
  int c = host.indexOf(':');
  if (c >= 0) host.remove(c);
  IPAddress pc;
  if (!g_pcHost[0] || (pc.fromString(host) && g_web->client().remoteIP() != pc)) {
    g_web->send(403, "application/json", "{\"ok\":false}");
    return;
  }
  String a = g_web->arg("a");
  int opt = -1;
  if (a == "pomo")       opt = constrain(g_web->arg("p").toInt(), 0, 2);
  else if (a == "timer") opt = 100 + constrain(g_web->arg("m").toInt(), 1, 180);
  else if (a == "stop")  opt = 10;
  else if (a == "skip")  opt = 11;
  else if (a == "plus")  opt = 12;
  bool ok = opt >= 0 && tm_action(opt);
  char st[40] = "";
  if (g_tmMode) tm_label(st, sizeof(st));
  String j = String("{\"ok\":") + (ok ? "true" : "false") + ",\"mode\":" + g_tmMode + ",\"label\":\"" + st + "\"}";
  g_web->send(ok ? 200 : 409, "application/json", j);
}
static void handleHomePost() {
  String perr;
  if (!web_pin_ok(perr)) { g_web->send(403, "text/html; charset=utf-8", home_page(perr, false)); return; }
  String city = g_web->arg("city"), pc = g_web->arg("pc");
  float lat = g_web->arg("lat").toFloat(), lon = g_web->arg("lon").toFloat();
  bool pcOk = pc.length() < 48;
  for (size_t i = 0; i < pc.length() && pcOk; i++) {
    char c = pc[i];
    pcOk = isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-';
  }
  String clean;
  for (size_t i = 0; i < city.length() && clean.length() < 30; i++) {
    char c = city[i];
    if (c != '<' && c != '>' && c != '"' && c != '\'' && c != '\\' && (uint8_t)c >= 0x20) clean += c;
  }
  if (!pcOk || lat < -90 || lat > 90 || lon < -180 || lon > 180 || !clean.length()) {
    g_web->send(200, "text/html; charset=utf-8", home_page("Dati non validi: controlla citta' e indirizzo del PC.", false));
    return;
  }
  strlcpy(g_wxCity, clean.c_str(), sizeof(g_wxCity));
  g_wxLat = lat; g_wxLon = lon;
  strlcpy(g_pcHost, pc.c_str(), sizeof(g_pcHost));
  g_prefs.putString("wxcity", g_wxCity);
  g_prefs.putFloat("wxlat", g_wxLat);
  g_prefs.putFloat("wxlon", g_wxLon);
  g_prefs.putString("pchost", g_pcHost);
  if (g_web->hasArg("kwh")) {
    String k = g_web->arg("kwh"); k.replace(',', '.');
    float price = k.toFloat();
    if (price >= 0 && price < 5) { g_kwhPrice = price; g_prefs.putFloat("kwh", g_kwhPrice); }
  }
  g_wx.ok = false; g_wxTryMs = 0;                   // nuovo meteo al prossimo giro
  g_pc.ok = false; g_pcAtMs = 0;
  Serial.printf("[HOME] citta' %s (%.4f, %.4f), pc %s\n", g_wxCity, g_wxLat, g_wxLon, g_pcHost);
  home_redraw();
  g_web->send(200, "text/html; charset=utf-8", home_page("Salvato: il meteo si aggiorna tra pochi secondi.", true));
}

static void start_data_web() {
  stop_web();
  ensure_mdns();
  g_web = new WebServer(80);
  g_web->on("/", HTTP_GET, handleInfo);
  g_web->on("/api/status", HTTP_GET, handleApiStatus);
  g_web->on("/api/pc", HTTP_GET, handleApiPc);
  g_web->on("/models", HTTP_GET, handleModelsGet);
  g_web->on("/models", HTTP_POST, handleModelsPost);
  g_web->on("/home", HTTP_GET, handleHomeGet);
  g_web->on("/home", HTTP_POST, handleHomePost);
  g_web->on("/pcpair", HTTP_POST, handlePcPair);
  g_web->on("/claude", HTTP_POST, handleClaudeEvent);
  g_web->on("/timer", HTTP_POST, handleTimerCmd);
  g_web->on("/update", HTTP_GET, handleUpdateGet);
  g_web->on("/update", HTTP_POST, handleUpdatePost, handleUpdateUpload);
  g_web->onNotFound([]() { g_web->send(404, "application/json", "{\"error\":\"not_found\"}"); });
  g_web->begin();
}

static void ui_token() {
  stop_web();
  lv_obj_t *scr = lv_screen_active();

  if (!g_onboarding && g_hasToken)
    tbtn(scr, 378, 5, 89, 32, TRS(U_LEFT " indietro", U_LEFT " back"), F14, C_MUTED, C_BORDER, nav_cb,
         (void *)(intptr_t)(g_usage.ok ? ST_MAIN : ST_SETTINGS));

  lv_obj_t *mark = build_claude_mark(scr);
  lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t *r = plain_obj(scr);
  lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  mklabel(r, "> ", F14, C_ACCENT);
  mklabel(r, TRS("incolla il token dal browser, su:", "paste the token from a browser, at:"), F14, C_MUTED);
  lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 116);

  String url = String("http://") + WiFi.localIP().toString();
  lv_obj_t *ip = mklabel(scr, url.c_str(), F22, C_ACCENT);
  lv_obj_align(ip, LV_ALIGN_TOP_MID, 0, 140);

  lv_obj_t *hint = mklabel(scr, TRS("stessa rete wifi del dispositivo", "same wifi network as the device"), F12, C_FAINT);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 176);

  g_tokMsg = spin_row(scr, TRS("in attesa del token", "waiting for the token"), 116);

  // avvia il server web (modulo del token)
  g_web = new WebServer(80);
  g_web->on("/", HTTP_GET, handleRoot);
  g_web->on("/token", HTTP_POST, handleTokenPost);
  g_web->onNotFound(handleNotFound);
  g_web->begin();
  Serial.printf("[WEB] server su %s\n", url.c_str());
}

// ============================================================
// Schermata: caricamento / messaggio
// ============================================================
static void ui_message(const char *title, const char *sub, uint32_t color) {
  lv_obj_t *scr = lv_screen_active();
  char t[48]; snprintf(t, sizeof(t), U_CROSS " %s", title);
  lv_obj_t *l = mklabel(scr, t, F22, color);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, -16);
  if (sub && sub[0]) {
    char b[96]; snprintf(b, sizeof(b), "> %s", sub);
    lv_obj_t *s = mklabel(scr, b, F14, C_MUTED);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 18);
  }
}
static void ui_loading(const char *sub) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *mark = build_claude_mark(scr);
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, -52);
  spin_row(scr, TRS("caricamento utilizzo", "loading usage"), 26);
  if (sub && sub[0]) {
    lv_obj_t *s = mklabel(scr, sub, F12, C_FAINT);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 52);
  }
}

// ---- Boot ----
//
// Il boot non passa da request_state(): gli stati li disegna loop(), che parte
// solo dopo che setup() ritorna. Tra il fillScreen(nero) e quel primo
// render c'erano LittleFS (che formatta la prima volta), la migrazione dello storico e
// l'autoConnect — bloccante fino a 8s PER rete salvata, 24s nel caso peggiore — tutto con
// la retroilluminazione gia' accesa su uno schermo nero. Chi ha appena flashato lo scambia per
// un blocco e stacca la corrente a meta' boot.
static lv_obj_t *g_bootSub = nullptr;

static void boot_splash(const char *sub) {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t *mark = build_claude_mark(scr);
  lv_obj_align(mark, LV_ALIGN_CENTER, 0, -52);
  spin_row(scr, "ritmo code", 26);
  g_bootSub = mklabel(scr, sub ? sub : "", F12, C_FAINT);
  lv_obj_align(g_bootSub, LV_ALIGN_CENTER, 0, 52);
  lv_obj_t *ver = mklabel(scr, "v" FW_VERSION, F12, C_FAINT);   // versione in piccolo, in basso
  lv_obj_align(ver, LV_ALIGN_BOTTOM_MID, 0, -13);

  lv_task_handler();
  lv_refr_now(NULL);                       // loop() non gira ancora: forza il disegno
}

static void boot_status(const char *sub) {
  if (!g_bootSub) return;
  lv_label_set_text(g_bootSub, sub);
  lv_task_handler();
  lv_refr_now(NULL);
}

// Passato ad autoConnect: tiene lo spinner in movimento e mostra quale rete si sta
// provando. Ridisegna solo quando il testo cambia — ogni 100ms un refresh completo
// di 320x480 farebbe concorrenza al WiFi stesso.
static void boot_wifi_tick(const char *ssid, int idx, int total) {
  static char ultimo[64] = "";
  char s[64];
  if (total > 1) snprintf(s, sizeof(s), "%s (%d/%d)", ssid, idx, total);
  else           snprintf(s, sizeof(s), "%s", ssid);
  if (g_bootSub && strcmp(s, ultimo) != 0) {
    snprintf(ultimo, sizeof(ultimo), "%s", s);
    lv_label_set_text(g_bootSub, s);
  }
  spin_tick();
  lv_task_handler();
}

// Errore prima che LVGL esista: disegna direttamente con Arduino_GFX. Accende anche la
// retroilluminazione, che resta a duty 0 da ledcAttach fino ad apply_brightness() — senza
// questo il messaggio finirebbe su uno schermo spento, indistinguibile da una scheda morta.
static void fatal_screen(const char *msg) {
  if (gfx) {
    ledcWrite(TFT_BL, 200);
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xDBAA);             // C_ACCENT in RGB565
    gfx->setTextSize(2);
    gfx->setCursor(14, 190);
    gfx->println("AVVIO FALLITO");
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(1);
    gfx->setCursor(14, 226);
    gfx->println(msg);
    gfx->setCursor(14, 248);
    gfx->println("Spegni e riaccendi la scheda.");
    gfx->setCursor(14, 262);
    gfx->println("Se persiste, riflasha il firmware.");
    gfx->flush();
  }
  while (1) delay(1000);
}

// ============================================================
// Storico / heatmap
// ============================================================
static void hist_push(float h5, float d7) {
  time_t now = time(nullptr);
  g_hist[g_histHead].t  = (now > 1000000000L) ? (uint32_t)now : 0;
  g_hist[g_histHead].h5 = (uint8_t)(h5 + 0.5f);
  g_hist[g_histHead].d7 = (uint8_t)(d7 + 0.5f);
  g_histHead = (g_histHead + 1) % HIST_MAX;
  if (g_histN < HIST_MAX) g_histN++;
}
static int hist_idx(int i) { return (g_histHead - g_histN + i + HIST_MAX * 2) % HIST_MAX; }

// giorno locale (giorni dall'epoch, corretto per il fuso configurato)
static uint32_t day_key() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return 0;
  // data locale (rispetta l'ora legale) -> giorni dal 1970-01-01 (days_from_civil)
  struct tm tv; localtime_r(&now, &tv);
  int y = tv.tm_year + 1900, m = tv.tm_mon + 1, d = tv.tm_mday;
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)(era * 146097 + (int)doe - 719468);
}
// restituisce l'indice del giorno in g_days (lo crea/ruota se serve) — restituisce int
// invece di DayHeat* per non inciampare nei prototipi automatici del .ino
static int day_slot(uint32_t dk) {
  for (int i = 0; i < g_dayN; i++)
    if (g_days[i].day == dk) return i;
  if (g_dayN == NDAYS) {              // scarta il piu' vecchio (array cronologico)
    memmove(&g_days[0], &g_days[1], sizeof(DayHeat) * (NDAYS - 1));
    g_dayN--;
  }
  int i = g_dayN++;
  g_days[i].day = dk;
  memset(g_days[i].burn, 0, sizeof(g_days[i].burn));
  return i;
}

// Heatmap: attribuisce il consumo (Δ utilizzo 5h) all'ora del giorno locale.
static void accumulate_heat(float h5) {
  time_t now = time(nullptr);
  if (g_lastH5 >= 0 && now > 1000000000L) {
    float d = h5 - g_lastH5;
    if (d > 0 && d < 100) {
      struct tm tmv; localtime_r(&now, &tmv);
      g_hourBurn[tmv.tm_hour] += d;
      uint32_t dk = day_key();
      if (dk) g_days[day_slot(dk)].burn[tmv.tm_hour] += d;
    }
  }
  g_lastH5 = h5;
}

// ---- Pomodori completati per ora del giorno (tutti gli account: /pomo.bin) ----
struct PomoDay { uint32_t day; uint8_t h[24]; };
#define POMO_MAGIC 0xC1A0DE10
static PomoDay g_pomoDays[NDAYS];
static int g_pomoDayN = 0;
static uint16_t g_pomoAll[24];                 // da sempre
static void pomo_save() {
  File f = LittleFS.open("/pomo.bin", "w");
  if (!f) return;
  uint32_t m = POMO_MAGIC;
  f.write((uint8_t *)&m, 4);
  f.write((uint8_t *)&g_pomoDayN, sizeof(g_pomoDayN));
  f.write((uint8_t *)g_pomoDays, sizeof(g_pomoDays));
  f.write((uint8_t *)g_pomoAll, sizeof(g_pomoAll));
  f.close();
}
static void pomo_load() {
  File f = LittleFS.open("/pomo.bin", "r");
  if (!f) return;
  uint32_t m = 0;
  if (f.read((uint8_t *)&m, 4) == 4 && m == POMO_MAGIC &&
      f.read((uint8_t *)&g_pomoDayN, sizeof(g_pomoDayN)) == sizeof(g_pomoDayN) &&
      f.read((uint8_t *)g_pomoDays, sizeof(g_pomoDays)) == sizeof(g_pomoDays) &&
      f.read((uint8_t *)g_pomoAll, sizeof(g_pomoAll)) == sizeof(g_pomoAll)) {
    if (g_pomoDayN < 0 || g_pomoDayN > NDAYS) g_pomoDayN = 0;
  } else {
    g_pomoDayN = 0; memset(g_pomoAll, 0, sizeof(g_pomoAll));
  }
  f.close();
}
// un pomodoro completato adesso: ora locale del giorno di oggi
static void pomo_record() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return;
  struct tm tv; localtime_r(&now, &tv);
  uint32_t dk = day_key();
  int i = -1;
  for (int k = 0; k < g_pomoDayN; k++) if (g_pomoDays[k].day == dk) i = k;
  if (i < 0) {
    if (g_pomoDayN == NDAYS) { memmove(&g_pomoDays[0], &g_pomoDays[1], sizeof(PomoDay) * (NDAYS - 1)); g_pomoDayN--; }
    i = g_pomoDayN++;
    g_pomoDays[i].day = dk;
    memset(g_pomoDays[i].h, 0, 24);
  }
  if (g_pomoDays[i].h[tv.tm_hour] < 255) g_pomoDays[i].h[tv.tm_hour]++;
  if (g_pomoAll[tv.tm_hour] < 65535) g_pomoAll[tv.tm_hour]++;
  pomo_save();
}
static void pomo_mode_data(int mode, float out[24]) {
  memset(out, 0, sizeof(float) * 24);
  if (mode == 3) { for (int h = 0; h < 24; h++) out[h] = g_pomoAll[h]; return; }
  uint32_t today = day_key();
  if (!today) return;
  uint32_t minDay = (mode == 0) ? today : (mode == 1) ? today - 6 : today - 29;
  for (int i = 0; i < g_pomoDayN; i++) {
    if (g_pomoDays[i].day < minDay || g_pomoDays[i].day > today) continue;
    for (int h = 0; h < 24; h++) out[h] += g_pomoDays[i].h[h];
  }
}

// somma l'heatmap secondo il periodo scelto (0=oggi 1=7g 2=30g 3=tutto)
static void heat_mode_data(int mode, float out[24]) {
  memset(out, 0, sizeof(float) * 24);
  if (mode == 3) { memcpy(out, g_hourBurn, sizeof(float) * 24); return; }
  uint32_t today = day_key();
  if (!today) return;
  uint32_t minDay = (mode == 0) ? today : (mode == 1) ? today - 6 : today - 29;
  for (int i = 0; i < g_dayN; i++) {
    if (g_days[i].day < minDay || g_days[i].day > today) continue;
    for (int h = 0; h < 24; h++) out[h] += g_days[i].burn[h];
  }
}

// Persistenza di storico/heatmap in LittleFS (sopravvive al riavvio).
// v2 = v1 + heatmap per giorno. Carica il vecchio v1 per non perdere lo storico.
#define HIST_MAGIC_V1 0xC1A0DE01
#define HIST_MAGIC_V2 0xC1A0DE02
#define HIST_MAX_V1 120
struct HistFileV1 { uint32_t magic; int n, head; Sample hist[HIST_MAX_V1]; float hourBurn[24]; float lastH5; };
struct HistFileV2 {
  uint32_t magic; int n, head; Sample hist[HIST_MAX]; float hourBurn[24]; float lastH5;
  int dayN; DayHeat days[NDAYS];
};
static void hist_path(char *out, size_t sz) {
  snprintf(out, sz, "/hist%d.bin", g_accts.active);
}
static void reset_history_ram() {
  memset(g_weeks, 0, sizeof(g_weeks));
  g_weekN = 0;
  memset(g_hist, 0, sizeof(g_hist));
  g_histN = 0; g_histHead = 0;
  memset(g_hourBurn, 0, sizeof(g_hourBurn));
  g_lastH5 = -1.0f;
  memset(g_days, 0, sizeof(g_days));
  g_dayN = 0;
}
static void save_history() {
  char pth[16]; hist_path(pth, sizeof(pth));
  File f = LittleFS.open(pth, "w");
  if (!f) return;
  static HistFileV2 hf;                       // troppo grande per lo stack
  hf.magic = HIST_MAGIC_V2; hf.n = g_histN; hf.head = g_histHead;
  memcpy(hf.hist, g_hist, sizeof(g_hist));
  memcpy(hf.hourBurn, g_hourBurn, sizeof(g_hourBurn));
  hf.lastH5 = g_lastH5;
  hf.dayN = g_dayN;
  memcpy(hf.days, g_days, sizeof(g_days));
  f.write((uint8_t *)&hf, sizeof(hf));
  f.close();
  // picchi settimanali: file separato e piccolo (lo storico v2 resta compatibile)
  snprintf(pth, sizeof(pth), "/weeks%d.bin", g_accts.active);
  File w = LittleFS.open(pth, "w");
  if (w) {
    uint32_t magic = 0xC1A0EE01;
    w.write((uint8_t *)&magic, sizeof(magic));
    w.write((uint8_t *)&g_weekN, sizeof(g_weekN));
    w.write((uint8_t *)g_weeks, sizeof(g_weeks));
    w.close();
  }
}
static void load_weeks() {
  char pth[16]; snprintf(pth, sizeof(pth), "/weeks%d.bin", g_accts.active);
  File w = LittleFS.open(pth, "r");
  if (!w) return;
  uint32_t magic = 0; int n = 0;
  if (w.read((uint8_t *)&magic, sizeof(magic)) == sizeof(magic) && magic == 0xC1A0EE01 &&
      w.read((uint8_t *)&n, sizeof(n)) == sizeof(n) && n >= 0 && n <= NWEEKS &&
      w.read((uint8_t *)g_weeks, sizeof(g_weeks)) == (int)sizeof(g_weeks))
    g_weekN = n;
  w.close();
}
// aggiorna il picco della settimana corrente (una nuova settimana se il reset e' cambiato)
// La API non fornisce settimane passate: la precedente si ricostruisce una volta dallo
// storico recente del dispositivo (picco di d7 nei campioni prima dell'ultimo reset).
static void week_backfill_prev(uint32_t reset) {
  uint32_t prev = reset - 7UL * 86400UL;
  for (int i = 0; i < g_weekN; i++) {
    long d = (long)g_weeks[i].reset - (long)prev;
    if (d > -3600 && d < 3600) return;                  // gia' presente
  }
  int pk = -1;
  for (int i = 0; i < g_histN; i++) {
    const Sample &s = g_hist[hist_idx(i)];
    if (s.t && s.t < prev && s.t + 7UL * 86400UL >= prev && s.d7 > pk) pk = s.d7;
  }
  if (pk < 0) return;
  int pos = 0;
  while (pos < g_weekN && g_weeks[pos].reset < prev) pos++;
  if (g_weekN == NWEEKS) {
    if (pos == 0) return;                               // sarebbe piu' vecchia di tutte
    memmove(&g_weeks[0], &g_weeks[1], sizeof(WeekRec) * (NWEEKS - 1)); g_weekN--; pos--;
  }
  memmove(&g_weeks[pos + 1], &g_weeks[pos], sizeof(WeekRec) * (g_weekN - pos));
  g_weeks[pos].reset = prev;
  g_weeks[pos].peak = (uint8_t)pk;
  g_weekN++;
  Serial.printf("[SETTIMANE] ricostruita la settimana precedente dallo storico: picco %d%%\n", pk);
}
static void week_record(float d7, uint32_t reset) {
  if (!reset) return;
  week_backfill_prev(reset);
  uint8_t p = (uint8_t)(d7 < 0 ? 0 : (d7 > 100 ? 100 : d7 + 0.5f));
  if (g_weekN > 0) {
    WeekRec &last = g_weeks[g_weekN - 1];
    long diff = (long)reset - (long)last.reset;
    if (diff > -3600 && diff < 3600) { if (p > last.peak) last.peak = p; return; }
    if (diff <= -3600) return;                          // dato vecchio: ignora
  }
  if (g_weekN == NWEEKS) { memmove(&g_weeks[0], &g_weeks[1], sizeof(WeekRec) * (NWEEKS - 1)); g_weekN--; }
  g_weeks[g_weekN].reset = reset;
  g_weeks[g_weekN].peak = p;
  g_weekN++;
}
static void load_history() {
  char pth[16]; hist_path(pth, sizeof(pth));
  File f = LittleFS.open(pth, "r");
  if (!f) return;
  uint32_t magic = 0;
  f.read((uint8_t *)&magic, sizeof(magic));
  f.seek(0);
  if (magic == HIST_MAGIC_V2) {
    static HistFileV2 hf;
    if (f.read((uint8_t *)&hf, sizeof(hf)) == (int)sizeof(hf)) {
      g_histN = hf.n; g_histHead = hf.head;
      memcpy(g_hist, hf.hist, sizeof(g_hist));
      memcpy(g_hourBurn, hf.hourBurn, sizeof(g_hourBurn));
      g_lastH5 = hf.lastH5;
      g_dayN = (hf.dayN >= 0 && hf.dayN <= NDAYS) ? hf.dayN : 0;
      memcpy(g_days, hf.days, sizeof(g_days));
    }
  } else if (magic == HIST_MAGIC_V1) {
    static HistFileV1 hf;
    if (f.read((uint8_t *)&hf, sizeof(hf)) == (int)sizeof(hf)) {
      int n = (hf.n > HIST_MAX_V1) ? HIST_MAX_V1 : hf.n;
      for (int i = 0; i < n; i++)
        g_hist[i] = hf.hist[(hf.head - n + i + HIST_MAX_V1 * 2) % HIST_MAX_V1];
      g_histN = n; g_histHead = n % HIST_MAX;
      memcpy(g_hourBurn, hf.hourBurn, sizeof(g_hourBurn));
      g_lastH5 = hf.lastH5;
      Serial.println("[HIST] migrato v1 -> v2");
    }
  }
  f.close();
  load_weeks();
}

// Copia byte per byte (LittleFS non ha copy). Usata nella migrazione multi-account:
// /hist.bin diventa /hist0.bin ma l'originale RESTA, cosi' un firmware precedente al
// multi-account (che conosce solo /hist.bin) trova ancora lo storico se si torna indietro.
// Costo: ~4,5 KB duplicati in una partizione di ~14 MB.
static bool copy_file(const char *from, const char *to) {
  File src = LittleFS.open(from, "r");
  if (!src) return false;
  File dst = LittleFS.open(to, "w");
  if (!dst) { src.close(); return false; }
  uint8_t buf[512];
  bool ok = true;
  for (;;) {
    int n = src.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (dst.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  dst.close();
  src.close();
  return ok;
}

static bool switch_account(int slot) {
  if (slot < 0 || slot >= ACCT_MAX || !g_accts.used[slot] || slot == g_accts.active)
    return false;
  EncryptedBlob b;
  if (!accountLoadBlob(g_prefs, slot, b)) return false;
  char tok[200];
  if (!decryptToken(b, g_sessionPin, tok, sizeof(tok))) return false;

  save_history();
  accountSetActive(g_prefs, g_accts, slot);
  g_dataEpoch++;
  g_blob = b;
  strlcpy(g_token, tok, sizeof(g_token));
  memset(tok, 0, sizeof(tok));
  reset_history_ram();
  load_history();
  memset(&g_usage, 0, sizeof(g_usage));
  Serial.printf("[ACCT] account attivo -> slot %d (%s)\n", slot, g_accts.label[slot]);
  request_state(ST_LOADING);
  return true;
}

static void finalize_pending_token() {
  g_dataEpoch++;
  EncryptedBlob nb;
  if (!encryptToken(g_pendingToken, g_sessionPin, nb)) {
    memset(g_pendingToken, 0, sizeof(g_pendingToken));
    request_state(ST_SETTINGS);
    return;
  }
  bool replacing = g_accts.used[g_tokenTargetSlot];
  bool switching = (g_tokenTargetSlot != g_accts.active);
  if (switching) save_history();
  const char *lbl = g_pendingLabel[0] ? g_pendingLabel
                    : (replacing ? g_accts.label[g_tokenTargetSlot] : "");
  accountSave(g_prefs, g_accts, g_tokenTargetSlot, nb, lbl);
  if (switching) {
    accountSetActive(g_prefs, g_accts, g_tokenTargetSlot);
    reset_history_ram();
    load_history();
  }
  g_blob = nb;
  strlcpy(g_token, g_pendingToken, sizeof(g_token));
  memset(g_pendingToken, 0, sizeof(g_pendingToken));
  g_pendingLabel[0] = 0;
  g_hasToken = true;
  g_lastOkMs = g_lastPollMs = millis();
  g_lastFetchOk = true;
  hist_push(g_usage.h5, g_usage.d7); accumulate_heat(g_usage.h5); week_record(g_usage.d7, g_usage.d7ResetEpoch); save_history();
  Serial.printf("[ACCT] token salvato nello slot %d (%s)\n",
                g_tokenTargetSlot, g_accts.label[g_tokenTargetSlot]);
  request_state(ST_MAIN);
}

// ============================================================
// Dashboard — helper visivi
// ============================================================
// parola di stato per la riga di prompt
static const char *status_word(const char *s, uint32_t *col) {
  if (!s || !s[0])            { *col = C_MUTED; return "--"; }
  if (!strcmp(s, "allowed"))  { *col = C_OK;    return "ok"; }
  if (strstr(s, "warning"))   { *col = C_WARN;  return TRS("attenzione", "warning"); }
  if (!strcmp(s, "rejected")) { *col = C_BAD;   return TRS("bloccato", "blocked"); }
  *col = C_MUTED; return s;
}

// label posizionata vuota (riempita in refresh_ui_values/dash_tick)
static lv_obj_t *tlabel(lv_obj_t *p, const lv_font_t *f, uint32_t c, int x, int y) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
  lv_label_set_text(l, "");
  lv_obj_set_pos(l, x, y);
  return l;
}
static lv_obj_t *tstatic(lv_obj_t *p, const char *txt, const lv_font_t *f, uint32_t c, int x, int y) {
  lv_obj_t *l = mklabel(p, txt, f, c);
  lv_obj_set_pos(l, x, y);
  return l;
}
static void tile_setup(lv_obj_t *t) {
  lv_obj_set_style_bg_opa(t, 0, 0);
  lv_obj_set_style_border_width(t, 0, 0);
  lv_obj_set_style_pad_all(t, 0, 0);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
}
// rettangolo pieno (pezzi della mascotte, barre del ritmo)
static lv_obj_t *rrect(lv_obj_t *p, int x, int y, int w, int h, int r, uint32_t col) {
  lv_obj_t *o = plain_obj(p);
  lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, r, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}
// umore del modello: sonda reale (HTTP) + incidenti di status.claude.com
static bool model_up(int i) {
  return (i == 0) ? g_status.haikuUp : (i == 1) ? g_status.sonnetUp : (i == 2) ? g_status.opusUp : g_status.fableUp;
}
static int model_mood(int i) {
  int c = g_models[i].pr.code;
  if (!model_up(i)) return 3;
  if (c == 0) return 0;
  if (c == 200) return 1;
  if (c == 429) return 2;
  if (c == 404) return 4;
  return 3;                              // rete / 5xx / auth
}
// umore complessivo della pagina Modelli: il peggiore dei quattro
static int aggregate_mood() {
  int m[NMODELS], all0 = 1, any2 = 0, any3 = 0, any4 = 0;
  for (int i = 0; i < NMODELS; i++) {
    m[i] = model_mood(i);
    if (m[i] != 0) all0 = 0;
    if (m[i] == 2) any2 = 1; if (m[i] == 3) any3 = 1; if (m[i] == 4) any4 = 1;
  }
  if (any3) return 3; if (any2) return 2; if (all0) return 0; if (any4) return 4;
  return 1;
}
// colore "sbiadito" verso lo sfondo (senza trasparenze)
static uint32_t fade_hex(uint32_t col, uint8_t keep) {
  if (keep >= 255) return col;
  lv_color_t c = lv_color_mix(lv_color_hex(col), lv_color_hex(C_BG), keep);
  return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}

// Mascotte della pagina Modelli: Clawd ufficiale con l'umore complessivo.
static void build_mascot(lv_obj_t *parent, int x, int y, int mood) {
  if (g_mascN >= NMODELS) return;
  lv_obj_t *c = plain_obj(parent);
  lv_obj_set_pos(c, x, y); lv_obj_set_size(c, 88, 64);

  lv_obj_t *img = lv_image_create(c);
  lv_image_set_src(img, &img_clawd_md);
  lv_obj_set_pos(img, 0, 4);

  const int ex[2] = {CLAWD_MD_EYE0_X, CLAWD_MD_EYE1_X};
  const int ey = CLAWD_MD_EYE0_Y + 4, ew = CLAWD_MD_EYE0_W, eh = CLAWD_MD_EYE0_H;

  Mascot &m = g_masc[g_mascN];
  m.cont = c; m.img = img; m.baseY = y; m.mood = mood;
  m.lid[0] = m.lid[1] = nullptr; m.drop = nullptr;

  if (mood == 1) {                       // ok: palpebre nascoste per sbattere
    for (int k = 0; k < 2; k++) {
      m.lid[k] = rrect(c, ex[k] - 1, ey - 1, ew + 2, eh + 2, 0, C_ACCENT);
      lv_obj_add_flag(m.lid[k], LV_OBJ_FLAG_HIDDEN);
    }
  } else if (mood == 2) {                // limitato: goccia di sudore
    m.drop = rrect(c, 72, 6, 6, 10, 3, C_BLUE);
  } else if (mood == 3) {                // errore/incidente: grigio + occhi a X
    lv_obj_set_style_image_recolor(img, lv_color_mix(lv_color_hex(0x6A6A74), lv_color_hex(C_ACCENT), 190), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_y(img, 8);
    for (int k = 0; k < 2; k++) {
      g_mXPts[0][k * 2][0]     = { (lv_value_precise_t)(ex[k] - 2), (lv_value_precise_t)(ey + 6) };
      g_mXPts[0][k * 2][1]     = { (lv_value_precise_t)(ex[k] + ew + 2), (lv_value_precise_t)(ey + eh + 10) };
      g_mXPts[0][k * 2 + 1][0] = { (lv_value_precise_t)(ex[k] + ew + 2), (lv_value_precise_t)(ey + 6) };
      g_mXPts[0][k * 2 + 1][1] = { (lv_value_precise_t)(ex[k] - 2), (lv_value_precise_t)(ey + eh + 10) };
      for (int l = 0; l < 2; l++) {
        lv_obj_t *ln = lv_line_create(c);
        lv_line_set_points(ln, g_mXPts[0][k * 2 + l], 2);
        lv_obj_set_style_line_width(ln, 3, 0);
        lv_obj_set_style_line_color(ln, lv_color_hex(C_BAD), 0);
      }
    }
  } else if (mood == 4) {                // id inesistente: addormentato
    lv_color_t gray = lv_color_mix(lv_color_hex(0x6A6A74), lv_color_hex(C_ACCENT), 170);
    lv_obj_set_style_image_recolor(img, lv_color_mix(gray, lv_color_hex(C_BG), 180), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    for (int k = 0; k < 2; k++)
      m.lid[k] = rrect(c, ex[k] - 1, ey + eh / 2, ew + 2, eh / 2 + 1, 0, fade_hex(0x8A8A94, 180));
  } else {                               // mai sondato: sbiadito
    lv_obj_set_style_image_recolor(img, lv_color_mix(lv_color_hex(C_ACCENT), lv_color_hex(C_BG), 140), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
  }
  g_mascN++;
}

// id breve: claude-haiku-4-5-20251001 -> haiku-4.5
static void short_id(const char *id, char *out, size_t sz) {
  const char *s = strncmp(id, "claude-", 7) == 0 ? id + 7 : id;
  strlcpy(out, s, sz);
  size_t n = strlen(out);
  if (n > 9 && out[n - 9] == '-') {      // suffisso data -YYYYMMDD
    bool date = true;
    for (size_t i = n - 8; i < n; i++) if (out[i] < '0' || out[i] > '9') date = false;
    if (date) { out[n - 9] = 0; n -= 9; }
  }
  // ultimo "-a-b" numerico -> "-a.b"
  char *last = strrchr(out, '-');
  if (last && last > out) {
    char *prev = last - 1;
    while (prev > out && *prev != '-') prev--;
    bool num = *prev == '-' && last[1] >= '0' && last[1] <= '9' && prev[1] >= '0' && prev[1] <= '9';
    for (char *q = prev + 1; num && q < last; q++) if (*q < '0' || *q > '9') num = false;
    for (char *q = last + 1; num && *q; q++) if (*q < '0' || *q > '9') num = false;
    if (num) *last = '.';
  }
}
static void model_stat(int i, char *out, size_t sz, uint32_t *col) {
  int c = g_models[i].pr.code;
  if (!model_up(i))                { strlcpy(out, "incid.", sz); *col = C_BAD; }
  else if (c == 0)                 { strlcpy(out, "--", sz);     *col = C_FAINT; }
  else if (c == 200)               { strlcpy(out, "ok", sz);     *col = C_OK; }
  else if (c == 429)               { strlcpy(out, "429", sz);    *col = C_WARN; }
  else if (c == 404)               { strlcpy(out, "id?", sz);    *col = C_WARN; }   // id da aggiornare
  else if (c == 401 || c == 403)   { strlcpy(out, "auth", sz);   *col = C_BAD; }
  else if (c < 0)                  { strlcpy(out, TRS("rete", "net"), sz); *col = C_BAD; }
  else                             { snprintf(out, sz, "%d", c); *col = C_BAD; }
}
// mini-linea delle latenze: ▁▂▃▄▅▆▇█ su 0..4 s, puntini dove manca lo storico
static void spark_text(int mi, char *out, size_t sz) {
  const ModelInfo &m = g_models[mi];
  static const char *LV[8] = {"\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
                              "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};
  out[0] = 0;
  for (int i = m.lhN; i < 7; i++) strlcat(out, U_MIDDOT, sz);
  for (int i = 0; i < m.lhN; i++) {
    int k = m.lh[i] * 8 / 4000; if (k > 7) k = 7;
    strlcat(out, LV[k], sz);
  }
}

// ============================================================
// Builder dei 4 tile
// ============================================================
// Tile 0 — ORA: due riquadri (5h / settimana) + riga di prompt
static void build_win_box(lv_obj_t *t, int x, const char *legend,
                          lv_obj_t **pct, Blocks *blk, lv_obj_t **at, lv_obj_t **cd) {
  // ritmo interno che scende per phi: 21 · 13 · 8, padding 21 sopra e 22 sotto
  lv_obj_t *b = tbox(t, x, 20, 223, 193, legend);
  lv_obj_t *r = trow(b, 13, 21);
  *pct = mklabel(r, "--", F54, C_OK);
  mklabel(r, "%", F22, C_MUTED);
  *blk = blocks_create(b, 13, 83, NBLK, F14);
  tstatic(b, TRS("reset tra", "resets in"), F12, C_MUTED, 13, 116);
  *at = tlabel(b, F12, C_MUTED, 13, 116);
  lv_obj_set_width(*at, 195);
  lv_obj_set_style_text_align(*at, LV_TEXT_ALIGN_RIGHT, 0);
  *cd = tlabel(b, F22, C_TEXT, 13, 141);
}
static void build_tile_agora(lv_obj_t *t) {
  build_win_box(t, 13,  TRS("finestra 5h", "5h window"), &g_ui.agPct5, &g_ui.blk5, &g_ui.agAt5, &g_ui.agCd5);
  build_win_box(t, 244, TRS("settimana", "week"),        &g_ui.agPct7, &g_ui.blk7, &g_ui.agAt7, &g_ui.agCd7);
  lv_obj_t *r = trow(t, 13, 226);
  mklabel(r, "> ", F14, C_ACCENT);
  mklabel(r, TRS("stato ", "status "), F14, C_MUTED);
  g_ui.agWord = mklabel(r, "--", F14, C_MUTED);
  g_ui.agCursor = mklabel(r, U_CURSOR, F14, C_ACCENT);
  // ritmo settimanale: % usata meno % di settimana trascorsa (aggiornato in dash_tick)
  g_ui.agPace = tlabel(t, F14, C_MUTED, 199, 226);
  lv_obj_set_width(g_ui.agPace, 268);
  lv_obj_set_style_text_align(g_ui.agPace, LV_TEXT_ALIGN_RIGHT, 0);
  // tacca sulla barra della settimana: dove "dovresti essere" col tempo trascorso
  g_ui.agPaceMark = rrect(t, 0, 20 + 79, 2, 26, 0, C_TEXT);
  lv_obj_add_flag(g_ui.agPaceMark, LV_OBJ_FLAG_HIDDEN);
}
// frazione della settimana trascorsa (0..1), -1 se l'ora o il reset non sono noti
static float week_elapsed() {
  time_t now = time(nullptr);
  if (now < 1000000000L || !g_usage.d7ResetEpoch) return -1;
  // reset gia' passato ma dati non ancora aggiornati: la nuova settimana e' appena iniziata
  uint32_t re = g_usage.d7ResetEpoch;
  while (re <= (uint32_t)now) re += 7UL * 86400UL;
  float f = 1.0f - (float)(re - now) / (7.0f * 86400.0f);
  return f < 0 ? 0 : (f > 1 ? 1 : f);
}
static void pace_update() {
  float f = week_elapsed();
  if (!g_ui.agPace) return;
  if (f < 0 || !g_usage.ok) {
    label_set(g_ui.agPace, "");
    lv_obj_add_flag(g_ui.agPaceMark, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  float used = g_usage.d7ResetEpoch <= (uint32_t)time(nullptr) ? 0.0f : g_usage.d7;   // settimana nuova
  int diff = (int)lroundf(used - f * 100.0f);
  char s[48];
  if (diff > 0)      snprintf(s, sizeof(s), TRS("ritmo sett. +%d%%", "week pace +%d%%"), diff);
  else if (diff < 0) snprintf(s, sizeof(s), TRS("ritmo sett. %d%%", "week pace %d%%"), diff);
  else               snprintf(s, sizeof(s), "%s", TRS("ritmo sett. in linea", "week pace on track"));
  uint32_t col = diff <= 0 ? C_OK : (diff <= 15 ? C_WARN : C_BAD);
  // ogni 6 s alterna col "dove arrivi": media della settimana finora proiettata fino al reset
  if ((millis() / 6000) % 2 == 1 && f > 0.02f) {
    float hoursIn = f * 168.0f, hoursLeft = (1.0f - f) * 168.0f;
    float rate = used / hoursIn;                        // % per ora
    if (used >= 99.5f) {
      snprintf(s, sizeof(s), "%s", TRS("quota settimanale esaurita", "weekly quota used up"));
      col = C_BAD;
    } else if (rate > 0.0001f && (100.0f - used) / rate < hoursLeft) {
      char c[24];
      fmt_clock((uint32_t)time(nullptr) + (uint32_t)((100.0f - used) / rate * 3600.0f), c, sizeof(c));
      for (char *q = c; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
      snprintf(s, sizeof(s), TRS("a questo ritmo finisce %s", "at this pace ends %s"), c);
      col = C_BAD;
    } else {
      int at = (int)lroundf(used + rate * hoursLeft);
      snprintf(s, sizeof(s), TRS("al reset arrivi al ~%d%%", "at reset you reach ~%d%%"), at > 100 ? 100 : at);
      col = at >= 90 ? C_WARN : C_OK;
    }
  }
  label_set(g_ui.agPace, s);
  label_color(g_ui.agPace, col);
  // blocchi della settimana: box x 244, blocchi a x+13 (+1 bordo), F14 = 8.4 px per blocco
  int x = 244 + 14 + (int)lroundf(f * NBLK * 8.4f) - 1;
  if (lv_obj_get_x(g_ui.agPaceMark) != x) lv_obj_set_x(g_ui.agPaceMark, x);
  lv_obj_clear_flag(g_ui.agPaceMark, LV_OBJ_FLAG_HIDDEN);
}
// Tile 1 — MODELLI: mascotte + riepilogo + tabella sonde
static void build_tile_models(lv_obj_t *t) {
  build_mascot(t, 12, 0, aggregate_mood());
  g_ui.mSum = tlabel(t, F14, C_TEXT, 112, 10);
  g_ui.mInc = tlabel(t, F12, C_MUTED, 112, 34);
  static lv_point_precise_t PTS_W454[2] = {{0, 0}, {454, 0}};
  for (int i = 0; i < NMODELS; i++) {
    int y = 72 + i * 40;
    g_ui.mDot[i]   = rrect(t, 16, y + 16, 8, 8, 0, C_FAINT);
    g_ui.mName[i]  = tlabel(t, F14, C_TEXT, 34, y + 10);
    g_ui.mSpark[i] = tlabel(t, F12, C_FAINT, 192, y + 11);
    g_ui.mLat[i]   = tlabel(t, F14, C_MUTED, 300, y + 10);
    lv_obj_set_width(g_ui.mLat[i], 70);
    lv_obj_set_style_text_align(g_ui.mLat[i], LV_TEXT_ALIGN_RIGHT, 0);
    g_ui.mStat[i]  = tlabel(t, F14, C_OK, 396, y + 10);
    lv_obj_set_width(g_ui.mStat[i], 64);
    lv_obj_set_style_text_align(g_ui.mStat[i], LV_TEXT_ALIGN_RIGHT, 0);
    if (i < NMODELS - 1) dline(t, 13, y + 39, PTS_W454);
  }
}
// didascalia in fondo ai riquadri grafici (box 454 x 226): filo a 186, testo a 193
static lv_obj_t *box_caption(lv_obj_t *b, const char *txt = nullptr) {
  hline(b, 13, 186, 426, C_TRACK);
  tstatic(b, ">", F14, C_ACCENT, 13, 193);
  lv_obj_t *l = txt ? tstatic(b, txt, F14, C_MUTED, 31, 193) : tlabel(b, F14, C_MUTED, 31, 193);
  lv_obj_set_width(l, 408);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  return l;
}
// Tile 2 — FINESTRA 5H: storico + proiezione tratteggiata
#define TR_X0 36
#define TR_Y0 13
#define TR_W  404
#define TR_H  140                                 // 226 / phi
static int tr_x(uint32_t tt, uint32_t ws, uint32_t we) {
  if (we <= ws) return TR_X0;
  long long v = (long long)(tt - ws) * TR_W / (long long)(we - ws);
  if (v < 0) v = 0; if (v > TR_W) v = TR_W;
  return TR_X0 + (int)v;
}
static int tr_y(float p) {
  if (p < 0) p = 0; if (p > 100) p = 100;
  return TR_Y0 + TR_H - (int)(p * TR_H / 100.0f);
}
static void build_tile_trend(lv_obj_t *t) {
  lv_obj_t *b = tbox(t, 13, 20, 454, 226, TRS("finestra 5h " U_MIDDOT " uso + proiezione", "5h window " U_MIDDOT " usage + projection"));
  for (int i = 1; i <= 3; i++) dline(b, TR_X0, tr_y(i * 25.0f), PTS_W404);
  hline(b, TR_X0, tr_y(0), TR_W, C_BORDER);
  const char *ax[3] = {"0", "50", "100"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *l = tstatic(b, ax[i], F12, C_FAINT, 2, tr_y(i * 50.0f) - 9);
    lv_obj_set_width(l, 28);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
  }
  g_ui.trHist = lv_line_create(b);
  lv_obj_set_pos(g_ui.trHist, 0, 0);
  lv_obj_set_style_line_width(g_ui.trHist, 2, 0);
  lv_obj_set_style_line_color(g_ui.trHist, lv_color_hex(C_ACCENT), 0);

  g_ui.trProj = lv_line_create(b);
  lv_obj_set_pos(g_ui.trProj, 0, 0);
  lv_obj_set_style_line_width(g_ui.trProj, 2, 0);
  lv_obj_set_style_line_color(g_ui.trProj, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_line_dash_width(g_ui.trProj, 4, 0);
  lv_obj_set_style_line_dash_gap(g_ui.trProj, 4, 0);

  g_ui.trDot = rrect(b, 0, 0, 7, 7, 0, C_TEXT);
  lv_obj_add_flag(g_ui.trDot, LV_OBJ_FLAG_HIDDEN);

  g_ui.trT0 = tlabel(b, F12, C_FAINT, TR_X0, TR_Y0 + TR_H + 5);
  g_ui.trT1 = tlabel(b, F12, C_FAINT, TR_X0 + TR_W - 60, TR_Y0 + TR_H + 5);
  lv_obj_set_width(g_ui.trT1, 60);
  lv_obj_set_style_text_align(g_ui.trT1, LV_TEXT_ALIGN_RIGHT, 0);

  g_ui.trCap = box_caption(b);
}
// Tile 3 — RITMO: barre per ora con filtro periodo
#define HEAT_BASE 162                                  // base delle barre nel riquadro
#define HEAT_H    112                                  // altezza massima (sotto i pulsanti del filtro)
static void heat_btn_style() {
  const char *names[4] = {TRS("oggi", "today"), TRS("7g", "7d"), TRS("30g", "30d"), TRS("tutto", "all")};
  for (int i = 0; i < 4; i++) {
    if (!g_ui.heatBtn[i]) continue;
    bool on = (i == g_heatMode);
    lv_obj_set_style_border_color(g_ui.heatBtn[i], lv_color_hex(on ? C_ACCENT : C_BORDER), 0);
    lv_obj_set_style_bg_color(g_ui.heatBtn[i], lv_color_mix(lv_color_hex(C_ACCENT), lv_color_hex(C_BG), 40), 0);
    lv_obj_set_style_bg_opa(g_ui.heatBtn[i], on ? LV_OPA_COVER : 0, 0);
    lv_obj_t *l = lv_obj_get_child(g_ui.heatBtn[i], 0);
    if (l) { lv_label_set_text(l, names[i]); lv_obj_set_style_text_color(l, lv_color_hex(on ? C_ACCENT : C_MUTED), 0); }
  }
}
static void heat_tab_style() {
  const char *names[2] = {"claude", "pomodoro"};
  for (int i = 0; i < 2; i++) {
    if (!g_ui.heatTab[i]) continue;
    bool on = (i == g_heatSrc);
    uint32_t c = i ? C_BAD : C_ACCENT;                      // pomodoro: rosso pomodoro
    lv_obj_set_style_border_color(g_ui.heatTab[i], lv_color_hex(on ? c : C_BORDER), 0);
    lv_obj_set_style_bg_color(g_ui.heatTab[i], lv_color_mix(lv_color_hex(c), lv_color_hex(C_BG), 40), 0);
    lv_obj_set_style_bg_opa(g_ui.heatTab[i], on ? LV_OPA_COVER : 0, 0);
    lv_obj_t *l = lv_obj_get_child(g_ui.heatTab[i], 0);
    if (l) { lv_label_set_text(l, names[i]); lv_obj_set_style_text_color(l, lv_color_hex(on ? c : C_MUTED), 0); }
  }
}
static void heat_tab_cb(lv_event_t *e) {
  int t = (int)(intptr_t)lv_event_get_user_data(e);
  if (t == g_heatSrc) return;
  g_heatSrc = t;
  g_prefs.putInt("heats", t);
  heat_tab_style();
  heat_redraw();
}
static void heat_btn_cb(lv_event_t *e) {
  int m = (int)(intptr_t)lv_event_get_user_data(e);
  if (m == g_heatMode) return;
  g_heatMode = m;
  g_prefs.putInt("heatm", m);
  heat_btn_style();
  heat_redraw();
}
static void build_tile_heat(lv_obj_t *t) {
  lv_obj_t *b = tbox(t, 13, 20, 454, 226, TRS("ritmo orario", "hourly rhythm"));
  g_ui.heatTab[0] = tbtn(b, 12, 12, 66, 26, "", F12, C_MUTED, C_BORDER, heat_tab_cb, (void *)(intptr_t)0);
  g_ui.heatTab[1] = tbtn(b, 84, 12, 84, 26, "", F12, C_MUTED, C_BORDER, heat_tab_cb, (void *)(intptr_t)1);
  heat_tab_style();
  for (int i = 0; i < 4; i++)
    g_ui.heatBtn[i] = tbtn(b, 180 + i * 66, 12, 60, 26, "", F12, C_MUTED, C_BORDER, heat_btn_cb, (void *)(intptr_t)i);
  heat_btn_style();
  for (int h = 0; h < 24; h++) g_ui.heat[h] = rrect(b, 14 + h * 18, HEAT_BASE - 2, 14, 2, 0, C_ACCENT);
  hline(b, 14, HEAT_BASE, 428, C_BORDER);
  int ticks[5] = {0, 6, 12, 18, 23};
  for (int i = 0; i < 5; i++) {
    char s[4]; snprintf(s, sizeof(s), "%dh", ticks[i]);
    tstatic(b, s, F12, C_FAINT, 12 + ticks[i] * 18, HEAT_BASE + 4);
  }
  g_ui.heatCap = box_caption(b, TRS("quota 5h consumata per ora locale", "5h quota burned per local hour"));
  heat_redraw();
}

// Tile 0 — HOME: ora, meteo, riepilogo Claude e PC collegato
static lv_point_precise_t g_wxBolt[4] = {{20, 26}, {14, 34}, {22, 34}, {16, 42}};
// icona meteo disegnata (il font non ha i simboli): sole/luna, nuvole, pioggia, neve, temporale, nebbia
static void wx_cloud(lv_obj_t *b, int x, int y, uint32_t col) {
  rrect(b, x + 2, y + 12, 34, 14, 7, col);
  rrect(b, x + 8, y + 3, 17, 17, 8, col);
  rrect(b, x + 19, y + 7, 14, 14, 7, col);
}
static int wx_group(int c) {
  if (c == 0) return 0;                       // sereno
  if (c <= 2) return 1;                       // poco nuvoloso
  if (c == 3) return 2;                       // coperto
  if (c == 45 || c == 48) return 3;           // nebbia
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return 5;   // neve
  if (c >= 95) return 6;                      // temporale
  if (c >= 51) return 4;                      // pioviggine, pioggia, rovesci
  return 2;
}
static const char *wx_desc(int c) {
  switch (wx_group(c)) {
    case 0: return TRS("sereno", "clear");
    case 1: return TRS("poco nuvoloso", "partly cloudy");
    case 2: return TRS("nuvoloso", "cloudy");
    case 3: return TRS("nebbia", "fog");
    case 4: return (c >= 80) ? TRS("rovesci", "showers") : (c < 60 ? TRS("pioviggine", "drizzle") : TRS("pioggia", "rain"));
    case 5: return TRS("neve", "snow");
    default: return TRS("temporale", "storm");
  }
}
static void wx_icon(lv_obj_t *b, int code, bool day) {
  lv_obj_clean(b);
  int g = wx_group(code);
  if (g == 0) {
    if (day) {
      rrect(b, 12, 10, 20, 20, 10, C_WARN);
      rrect(b, 21, 1, 2, 6, 0, C_WARN); rrect(b, 21, 33, 2, 6, 0, C_WARN);
      rrect(b, 3, 19, 6, 2, 0, C_WARN); rrect(b, 35, 19, 6, 2, 0, C_WARN);
    } else {
      rrect(b, 11, 8, 24, 24, 12, C_TEXT);
      rrect(b, 19, 3, 22, 22, 11, C_BG);
    }
    return;
  }
  if (g == 1) {
    if (day) rrect(b, 2, 0, 18, 18, 9, C_WARN);
    else { rrect(b, 2, 0, 18, 18, 9, C_TEXT); rrect(b, 8, -3, 16, 16, 8, C_BG); }
    wx_cloud(b, 6, 8, C_MUTED);
    return;
  }
  if (g == 3) {
    rrect(b, 4, 10, 36, 3, 1, C_MUTED); rrect(b, 8, 18, 32, 3, 1, C_MUTED); rrect(b, 4, 26, 36, 3, 1, C_MUTED);
    return;
  }
  wx_cloud(b, 3, 0, g == 2 ? C_MUTED : C_FAINT);
  if (g == 4) { rrect(b, 11, 30, 2, 8, 1, C_BLUE); rrect(b, 20, 32, 2, 8, 1, C_BLUE); rrect(b, 29, 30, 2, 8, 1, C_BLUE); }
  if (g == 5) { rrect(b, 10, 32, 4, 4, 2, C_TEXT); rrect(b, 20, 35, 4, 4, 2, C_TEXT); rrect(b, 30, 32, 4, 4, 2, C_TEXT); }
  if (g == 6) {
    lv_obj_t *l = lv_line_create(b);
    lv_line_set_points(l, g_wxBolt, 4);
    lv_obj_set_style_line_width(l, 3, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(C_WARN), 0);
  }
}
// ---- Previsioni della settimana: si aprono toccando il meteo nella home ----
static lv_obj_t *g_wxWeek = nullptr;
static lv_obj_t *g_wxWeekBtn = nullptr;          // etichetta del tasto aggiorna
// tasto aggiorna: scarica subito il meteo (task extra); a fine download il riquadro si ridisegna
static void wx_refresh_cb(lv_event_t *e) {
  (void)e;
  if (g_wxReq || g_wxDone || !g_wifi.isConnected() || g_wxLat == 0) return;
  g_wxTryMs = millis();
  g_wxReq = true;
  if (g_wxWeekBtn) lv_label_set_text(g_wxWeekBtn, TRS("aggiornamento...", "updating..."));
}
static void wx_week_close() { if (g_wxWeek) { lv_obj_delete(g_wxWeek); g_wxWeek = nullptr; g_wxWeekBtn = nullptr; } }
static void wx_week_close_cb(lv_event_t *e) { (void)e; wx_week_close(); }
static void wx_week_open(lv_event_t *e) {
  (void)e;
  if (g_wxWeek) return;
  lv_obj_t *s = plain_obj(lv_layer_top());
  g_wxWeek = s;
  lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, wx_week_close_cb, LV_EVENT_CLICKED, NULL);
  char lg[48], city[24];
  strlcpy(city, g_wxCity, sizeof(city));
  for (char *q = city; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
  snprintf(lg, sizeof(lg), TRS("meteo " U_MIDDOT " %s " U_MIDDOT " 7 giorni", "weather " U_MIDDOT " %s " U_MIDDOT " 7 days"), city);
  tbox(s, 10, 14, 460, 296, lg, C_BORDER);
  time_t now = time(nullptr);
  if (!g_wx.ok || g_wx.days == 0 || now < 1000000000L) {
    tstatic(s, TRS("previsioni in arrivo...", "loading forecast..."), F14, C_MUTED, 34, 60);
  } else {
    static const char *GIT[7] = {"dom", "lun", "mar", "mer", "gio", "ven", "sab"};
    static const char *GEN[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    // una colonna per giorno: giorno, data, icona, massima, minima, pioggia
    auto col = [&](const char *txt, const lv_font_t *f, uint32_t c, int x, int y) {
      lv_obj_t *l = tstatic(s, txt, f, c, x, y);
      lv_obj_set_width(l, 64);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    };
    for (int d = 0; d < g_wx.days && d < 7; d++) {
      int x = 16 + d * 64;
      time_t t = now + (time_t)d * 86400;
      struct tm tv; localtime_r(&t, &tv);
      char b[16];
      col(d == 0 ? TRS("oggi", "today") : (g_lang ? GEN : GIT)[tv.tm_wday], F14, d == 0 ? C_ACCENT : C_TEXT, x, 44);
      snprintf(b, sizeof(b), "%d/%d", tv.tm_mday, tv.tm_mon + 1);
      col(b, F12, C_MUTED, x, 64);
      lv_obj_t *ic = plain_obj(s);
      lv_obj_set_pos(ic, x + 10, 88);
      lv_obj_set_size(ic, 44, 44);
      wx_icon(ic, g_wx.dcode[d], true);
      snprintf(b, sizeof(b), "%.0f\xC2\xB0", g_wx.dmax[d]);
      col(b, F22, C_TEXT, x, 144);
      snprintf(b, sizeof(b), "%.0f\xC2\xB0", g_wx.dmin[d]);
      col(b, F14, C_MUTED, x, 174);
      snprintf(b, sizeof(b), "%d%%", g_wx.drain[d]);
      col(b, F12, g_wx.drain[d] >= 40 ? C_BLUE : C_FAINT, x, 198);
      if (d) {                                      // separatore tra i giorni
        lv_obj_t *sep = plain_obj(s);
        lv_obj_set_pos(sep, x, 48); lv_obj_set_size(sep, 1, 164);
        lv_obj_set_style_bg_color(sep, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
      }
    }
    char f[80], at[12] = "";
    if (g_wxAtMs) fmt_hm((uint32_t)now - (millis() - g_wxAtMs) / 1000, at, sizeof(at));
    snprintf(f, sizeof(f), TRS("oggi: %s " U_MIDDOT " sole %s-%s " U_MIDDOT " aggiornato %s", "today: %s " U_MIDDOT " sun %s-%s " U_MIDDOT " updated %s"),
             wx_desc(g_wx.code), g_wx.sunrise, g_wx.sunset, at);
    lv_obj_t *fl = tstatic(s, f, F12, C_MUTED, 10, 236);
    lv_obj_set_width(fl, 460);
    lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
  }
  lv_obj_t *rb = tbtn(s, 24, 268, 150, 30, "", F14, C_ACCENT, C_BORDER, wx_refresh_cb, NULL);
  g_wxWeekBtn = lv_obj_get_child(rb, 0);
  lv_label_set_text(g_wxWeekBtn, g_wxReq ? TRS("aggiornamento...", "updating...") : TRS(U_REFRESH " aggiorna", U_REFRESH " refresh"));
  tstatic(s, TRS("[ tocca per chiudere ]", "[ tap to close ]"), F12, C_FAINT, 290, 280);
}

// home: tocca il riquadro claude o pc per andare alla pagina con i dettagli
static void home_goto_cb(lv_event_t *e) {
  int tile = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_ui.tv) lv_tileview_set_tile_by_index(g_ui.tv, tile, 0, LV_ANIM_ON);
}
// Claude al lavoro: legenda del riquadro claude in home; la ✻ della testata gira nel loop
static void cc_busy_ui() {
  if (g_ui.hmClLegend) {
    char s[40];
    if (g_ccBusyN > 1)       snprintf(s, sizeof(s), TRS("claude " U_MIDDOT " %d al lavoro", "claude " U_MIDDOT " %d working"), g_ccBusyN);
    else if (g_ccBusyN == 1) strcpy(s, TRS("claude " U_MIDDOT " al lavoro", "claude " U_MIDDOT " working"));
    else                     strcpy(s, "claude");
    label_set(g_ui.hmClLegend, s);
    label_color(g_ui.hmClLegend, g_ccBusyN ? C_ACCENT : C_MUTED);
  }
  if (!g_ccBusyN && g_ui.hdrSpark) label_set(g_ui.hdrSpark, U_SPARK " ");   // ferma: di nuovo ✻
}
static void build_tile_home(lv_obj_t *t) {
  // griglia aurea: colonna sinistra fino a x 297 (480 / phi), meteo da 310
  g_ui.hmTime = tlabel(t, F96, C_TEXT, 7, 7);
  lv_obj_add_flag(g_ui.hmTime, LV_OBJ_FLAG_CLICKABLE);           // tocca l'ora: timer e pomodoro
  lv_obj_set_ext_click_area(g_ui.hmTime, 8);
  lv_obj_add_event_cb(g_ui.hmTime, tm_menu_open, LV_EVENT_SHORT_CLICKED, NULL);
  // pomodoro disegnato (il font non ha il glifo): dice che la riga sotto l'ora apre timer e pomodoro
  lv_obj_t *tom = plain_obj(t);
  lv_obj_set_pos(tom, 13, 81);
  lv_obj_set_size(tom, 14, 16);
  rrect(tom, 0, 3, 14, 13, 6, C_BAD);
  rrect(tom, 3, 1, 8, 3, 1, C_OK);
  rrect(tom, 6, 0, 2, 3, 0, C_OK);
  g_ui.hmDate = tlabel(t, F14, C_MUTED, 33, 86);       // a meta' tra l'ora e il riquadro claude
  lv_obj_set_width(g_ui.hmDate, 264);
  // centrato sulla scritta: -2 perche' le lettere stanno sopra il centro del riquadro della riga
  lv_obj_align_to(tom, g_ui.hmDate, LV_ALIGN_OUT_LEFT_MID, -6, -2);
  for (lv_obj_t *o : {tom, g_ui.hmDate}) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(o, 8);
    lv_obj_add_event_cb(o, tm_menu_open, LV_EVENT_SHORT_CLICKED, NULL);
  }
  lv_label_set_long_mode(g_ui.hmDate, LV_LABEL_LONG_DOT);

  g_ui.hmWxIcon = plain_obj(t);
  lv_obj_set_pos(g_ui.hmWxIcon, 310, 9);
  lv_obj_set_size(g_ui.hmWxIcon, 44, 44);
  g_ui.hmWxCode = -1;
  lv_obj_t *tr = trow(t, 362, 7);                   // F54 ha solo le cifre: il simbolo dei gradi e' in F22
  g_ui.hmTemp = mklabel(tr, "--", F54, C_TEXT);
  mklabel(tr, "\xC2\xB0", F22, C_MUTED);
  g_ui.hmDesc = tlabel(t, F12, C_MUTED, 310, 56);      // l'ultima riga finisce con la riga della data
  g_ui.hmRain = tlabel(t, F12, C_MUTED, 310, 72);
  g_ui.hmSun  = tlabel(t, F12, C_FAINT, 310, 88);
  for (lv_obj_t *l : {g_ui.hmDesc, g_ui.hmRain, g_ui.hmSun}) {
    lv_obj_set_width(l, 157);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
  // tocca il meteo: previsioni della settimana
  for (lv_obj_t *o : {g_ui.hmWxIcon, tr, g_ui.hmDesc, g_ui.hmRain, g_ui.hmSun}) {
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, wx_week_open, LV_EVENT_SHORT_CLICKED, NULL);
  }

  // centrato tra la riga della data (lettere fino a y 98) e il riquadro pc (y 206): 17 sopra e 17 sotto;
  // righe del metro a passo 30
  lv_obj_t *b = tbox(t, 13, 115, 454, 74, "claude", C_BORDER, &g_ui.hmClLegend);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);                       // -> pagina ora
  lv_obj_add_event_cb(b, home_goto_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)1);
  const char *k[2] = {"5h", TRS("sett.", "week")};
  for (int i = 0; i < 2; i++) {
    int y = 13 + i * 30;
    tstatic(b, k[i], F14, C_MUTED, 13, y);
    g_ui.hmBlk[i] = blocks_create(b, 57, y, 10, F14);
    g_ui.hmPct[i] = tlabel(b, F14, C_TEXT, 149, y);
    g_ui.hmInfo[i] = tlabel(b, F12, C_MUTED, 199, y + 2);
    g_ui.hmRight[i] = tlabel(b, F12, C_MUTED, 303, y + 2);
    lv_obj_set_width(g_ui.hmRight[i], 138);
    lv_obj_set_style_text_align(g_ui.hmRight[i], LV_TEXT_ALIGN_RIGHT, 0);
  }

  lv_obj_t *p = tbox(t, 13, 206, 454, 40, "pc", C_BORDER, &g_ui.hmPcLegend);
  lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);                       // -> pagina pc
  lv_obj_add_event_cb(p, home_goto_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)6);
  g_ui.hmPcRow = trow(p, 13, 9);
  const char *pk[4] = {"cpu ", "   ram ", "   gpu ", TRS("   disco ", "   disk ")};
  for (int i = 0; i < 4; i++) {
    mklabel(g_ui.hmPcRow, pk[i], F12, C_MUTED);
    g_ui.hmPcVal[i] = mklabel(g_ui.hmPcRow, "--", F14, C_TEXT);
  }
  g_ui.hmPcOff = tlabel(p, F12, C_FAINT, 13, 11);
  cc_busy_ui();
  lv_obj_set_width(g_ui.hmPcOff, 426);
  lv_label_set_long_mode(g_ui.hmPcOff, LV_LABEL_LONG_DOT);
  home_redraw();
}
// ora e data (ogni secondo, ridisegna solo se cambiano)
static void home_tick() {
  if (!g_ui.hmTime) return;
  time_t now = time(nullptr);
  if (now < 1000000000L) { label_set(g_ui.hmTime, "--:--"); return; }
  struct tm tv; localtime_r(&now, &tv);
  char s[64];
  snprintf(s, sizeof(s), "%02d:%02d", tv.tm_hour, tv.tm_min);
  label_set(g_ui.hmTime, s);
  static const char *GIT[7] = {"domenica", "luned\xC3\xAC", "marted\xC3\xAC", "mercoled\xC3\xAC", "gioved\xC3\xAC", "venerd\xC3\xAC", "sabato"};
  static const char *GEN[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
  static const char *MIT[12] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};
  static const char *MEN[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  char city[24]; strlcpy(city, g_wxCity, sizeof(city));
  for (char *q = city; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
  if (g_lang) snprintf(s, sizeof(s), "%s %d %s " U_MIDDOT " %s", GEN[tv.tm_wday], tv.tm_mday, MEN[tv.tm_mon], city);
  else        snprintf(s, sizeof(s), "%s %d %s " U_MIDDOT " %s", GIT[tv.tm_wday], tv.tm_mday, MIT[tv.tm_mon], city);
  if (g_tmMode) {                                  // timer in corso: sotto l'ora, conto alla rovescia e fine
    char hm[12], ph[32];
    int32_t left = (int32_t)(g_tmEndMs - millis());
    fmt_hm((uint32_t)now + (left > 0 ? (left + 999) / 1000 : 0), hm, sizeof(hm));
    tm_label(ph, sizeof(ph));
    snprintf(s, sizeof(s), TRS("%s " U_MIDDOT " fine %s", "%s " U_MIDDOT " ends %s"), ph, hm);
  }
  label_set(g_ui.hmDate, s);
  label_color(g_ui.hmDate, g_tmMode ? tm_color() : C_MUTED);
  // descrizione meteo di oggi (la colonna tiene 21 caratteri; domani e' nelle previsioni della settimana)
  if (g_wx.ok) {
    snprintf(s, sizeof(s), "%s %.0f/%.0f\xC2\xB0", wx_desc(g_wx.code), g_wx.tmax, g_wx.tmin);
    label_set(g_ui.hmDesc, s);
  }
  if (g_pcHost[0] && g_pcAtMs && millis() - g_pcAtMs > pc_stale_ms()) home_redraw();   // PC non risponde piu'
}
static void home_redraw() {
  if (!g_ui.hmTime) return;
  char s[64];
  // meteo
  if (g_wx.ok) {
    int code = g_wx.code * 2 + (g_wx.isDay ? 1 : 0);
    if (code != g_ui.hmWxCode) { g_ui.hmWxCode = code; wx_icon(g_ui.hmWxIcon, g_wx.code, g_wx.isDay); }
    snprintf(s, sizeof(s), "%.0f", g_wx.temp);
    label_set(g_ui.hmTemp, s);
    int best = 0, bestH = -1, first = -1;
    for (int i = 0; i < 12; i++) {
      if (g_wx.rain[i] > best) { best = g_wx.rain[i]; bestH = i; }
      if (first < 0 && g_wx.rain[i] >= 40) first = i;
    }
    if (first >= 0) snprintf(s, sizeof(s), TRS("pioggia %d%% alle %02d", "rain %d%% at %02d"), g_wx.rain[first], (g_wx.rainHour0 + first) % 24);
    else if (best >= 20) snprintf(s, sizeof(s), TRS("forse pioggia %d%%", "rain possible %d%%"), best);
    else snprintf(s, sizeof(s), "%s", TRS("niente pioggia 12h", "no rain in 12h"));
    (void)bestH;
    label_set(g_ui.hmRain, s);
    label_color(g_ui.hmRain, first >= 0 ? C_BLUE : C_MUTED);
    snprintf(s, sizeof(s), TRS("sole %s-%s", "sun %s-%s"), g_wx.sunrise, g_wx.sunset);
    label_set(g_ui.hmSun, s);
  } else {
    label_set(g_ui.hmTemp, "--");
    label_set(g_ui.hmDesc, g_wxLat != 0 ? TRS("meteo in arrivo...", "loading weather...") : TRS("imposta la citta' dal browser", "set the city from the browser"));
  }
  // claude
  float v[2] = {g_usage.h5, g_usage.d7};
  uint32_t re[2] = {g_usage.h5ResetEpoch, g_usage.d7ResetEpoch};
  for (int i = 0; i < 2; i++) {
    uint32_t col = level_hex(v[i]);
    blocks_set(g_ui.hmBlk[i], g_usage.ok ? v[i] : 0, col);
    snprintf(s, sizeof(s), "%d%%", (int)(v[i] + 0.5f));
    label_set(g_ui.hmPct[i], g_usage.ok ? s : "--");
    label_color(g_ui.hmPct[i], col);
    char c[24];
    if (i == 0) { fmt_hm(re[0], c, sizeof(c)); snprintf(s, sizeof(s), TRS("reset %s", "reset %s"), c); }
    else {
      fmt_eta(re[1], c, sizeof(c));
      snprintf(s, sizeof(s), TRS("reset tra %s", "reset in %s"), c);
    }
    label_set(g_ui.hmInfo[i], g_usage.ok ? s : "");
  }
  uint32_t wc; const char *w = status_word(g_usage.statusOverall, &wc);
  snprintf(s, sizeof(s), TRS("stato %s", "status %s"), w);
  label_set(g_ui.hmRight[0], s); label_color(g_ui.hmRight[0], wc);
  int ok = 0;
  for (int i = 0; i < NMODELS; i++) if (model_mood(i) == 1) ok++;
  if (g_userPause) { label_set(g_ui.hmRight[1], TRS("richieste in pausa", "requests paused")); label_color(g_ui.hmRight[1], C_WARN); }
  else {
    snprintf(s, sizeof(s), TRS("modelli %d/%d ok", "models %d/%d ok"), ok, NMODELS);
    label_set(g_ui.hmRight[1], s); label_color(g_ui.hmRight[1], ok == NMODELS ? C_MUTED : C_WARN);
  }
  // pc
  snprintf(s, sizeof(s), "pc " U_MIDDOT " %s", g_pcHost[0] ? g_pcHost : TRS("non impostato", "not set"));
  label_set(g_ui.hmPcLegend, s);
  bool pcOk = g_pc.ok && g_pcAtMs && millis() - g_pcAtMs <= pc_stale_ms();
  if (pcOk) {
    lv_obj_clear_flag(g_ui.hmPcRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.hmPcOff, LV_OBJ_FLAG_HIDDEN);
    if (g_pc.cpuTemp > 0) snprintf(s, sizeof(s), "%.0f%% %.0f\xC2\xB0", g_pc.cpu, g_pc.cpuTemp); else snprintf(s, sizeof(s), "%.0f%%", g_pc.cpu);
    label_set(g_ui.hmPcVal[0], s); label_color(g_ui.hmPcVal[0], (g_pc.cpu > 85 || g_pc.cpuTemp > 85) ? C_WARN : C_TEXT);
    snprintf(s, sizeof(s), "%.0f%%", g_pc.ram);
    label_set(g_ui.hmPcVal[1], s); label_color(g_ui.hmPcVal[1], g_pc.ram > 90 ? C_WARN : C_TEXT);
    if (g_pc.gpuTemp > 0) snprintf(s, sizeof(s), "%.0f%% %.0f\xC2\xB0", g_pc.gpu, g_pc.gpuTemp); else snprintf(s, sizeof(s), "%.0f%%", g_pc.gpu);
    label_set(g_ui.hmPcVal[2], s);
    snprintf(s, sizeof(s), "%.0f%%", g_pc.disk);
    label_set(g_ui.hmPcVal[3], s); label_color(g_ui.hmPcVal[3], g_pc.disk > 90 ? C_WARN : C_TEXT);
  } else {
    lv_obj_add_flag(g_ui.hmPcRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_ui.hmPcOff, LV_OBJ_FLAG_HIDDEN);
    label_set(g_ui.hmPcOff, !g_pcHost[0] ? TRS("imposta l'indirizzo del pc dal browser (/home)", "set the pc address from the browser (/home)")
                          : TRS("pc spento o Ritmo Code PC Monitor non attivo", "pc off or Ritmo Code PC Monitor not running"));
  }
}

// Tile 6 — PC: statistiche complete del computer (Ritmo Code PC Monitor sul PC)
#define PCS_W 197                                          // larghezza dei grafici (223 - 2 x 13)
static void pc_box(lv_obj_t *t, int i, int x, int y, int w, int h, int sparkY, int sparkH, uint32_t col) {
  lv_obj_t *b = tbox(t, x, y, w, h, "", C_BORDER, &g_ui.pcLeg[i]);
  g_ui.pcMain[i] = tlabel(b, F14, C_TEXT, 13, 6);
  lv_obj_set_width(g_ui.pcMain[i], w - 26);
  lv_label_set_long_mode(g_ui.pcMain[i], LV_LABEL_LONG_DOT);
  if (i < 3) {
    if (sparkY > 30) {
      g_ui.pcSub[i] = tlabel(b, F12, C_MUTED, 13, 25);
      lv_obj_set_width(g_ui.pcSub[i], w - 26);
      lv_label_set_long_mode(g_ui.pcSub[i], LV_LABEL_LONG_DOT);
    }
    hline(b, 13, sparkY + sparkH, PCS_W, C_TRACK);
    g_ui.pcSpark[i] = lv_line_create(b);
    lv_obj_set_pos(g_ui.pcSpark[i], 13, sparkY);
    lv_obj_set_style_line_width(g_ui.pcSpark[i], 2, 0);
    lv_obj_set_style_line_color(g_ui.pcSpark[i], lv_color_hex(col), 0);
    lv_obj_set_user_data(g_ui.pcSpark[i], (void *)(intptr_t)sparkH);
  }
}
static void build_tile_pc(lv_obj_t *t) {
  // righe 89 e 55 (coppia di Fibonacci), 21 tra le righe
  pc_box(t, 0, 13, 20, 223, 89, 46, 34, C_ACCENT);         // cpu
  pc_box(t, 1, 244, 20, 223, 89, 46, 34, C_OK);            // gpu
  pc_box(t, 2, 13, 130, 223, 55, 10, 35, C_BLUE);          // ram: tutto il riquadro al grafico
  lv_obj_t *nb = tbox(t, 244, 130, 223, 55, TRS("rete " U_MIDDOT " disco", "network " U_MIDDOT " disk"), C_BORDER, &g_ui.pcLeg[3]);
  g_ui.pcMain[3] = tlabel(nb, F14, C_TEXT, 13, 6);
  g_ui.pcSys = tlabel(nb, F12, C_MUTED, 13, 29);
  lv_obj_set_width(g_ui.pcSys, 197);
  lv_label_set_long_mode(g_ui.pcSys, LV_LABEL_LONG_DOT);
  lv_obj_t *sb = tbox(t, 13, 206, 454, 40, TRS("sistema", "system"));
  g_ui.pcDisks = tlabel(sb, F12, C_MUTED, 13, 11);
  lv_obj_set_width(g_ui.pcDisks, 428);
  lv_label_set_long_mode(g_ui.pcDisks, LV_LABEL_LONG_DOT);
  pc_redraw();
}
static void fmt_rate(float bps, char *out, int sz) {
  if (bps >= 1048576.0f)  snprintf(out, sz, "%.1f MB/s", bps / 1048576.0f);
  else if (bps >= 1024.0f) snprintf(out, sz, "%.0f KB/s", bps / 1024.0f);
  else                     snprintf(out, sz, "%.0f B/s", bps);
}
// nome breve e minuscolo: "AMD Ryzen 7 9700X 8-Core Processor" -> "amd ryzen 7 9700x"
static void short_name(const char *in, char *out, int sz) {
  String s(in);
  for (const char *cut : {" Processor", "(R)", "(TM)", "NVIDIA GeForce ", "NVIDIA ", "AMD Radeon ", " Graphics", "Intel ", "Core "}) s.replace(cut, "");
  int k = s.indexOf("-Core");
  if (k > 0) { int sp = s.lastIndexOf(' ', k); if (sp > 0) s = s.substring(0, sp); }
  s.trim();
  s.toLowerCase();
  strlcpy(out, s.c_str(), sz);
}
static void pc_spark(int i) {
  lv_obj_t *l = g_ui.pcSpark[i];
  if (!l) return;
  int h = (int)(intptr_t)lv_obj_get_user_data(l);
  int n = g_pcHistN;
  for (int k = 0; k < n; k++) {
    g_pcSparkPts[i][k].x = (lv_value_precise_t)(n > 1 ? PCS_W - (n - 1 - k) * PCS_W / (PC_HIST - 1) : PCS_W);
    g_pcSparkPts[i][k].y = (lv_value_precise_t)(h - g_pcHist[i][k] * h / 100);
  }
  lv_line_set_points(l, g_pcSparkPts[i], n);
}
static void pc_redraw() {
  if (!g_ui.pcDisks) return;
  char s[140], a[24], b[24];
  bool ok = g_pc.ok && g_pcAtMs && millis() - g_pcAtMs <= pc_stale_ms();
  // cpu
  short_name(g_pc.cpuName, a, sizeof(a));
  snprintf(s, sizeof(s), "cpu%s%s", a[0] ? " " U_MIDDOT " " : "", a);
  label_set(g_ui.pcLeg[0], s);
  if (ok) {
    int n = snprintf(s, sizeof(s), "%.0f%%  %.2f GHz", g_pc.cpu, g_pc.cpuMhz / 1000.0f);
    if (g_pc.cpuTemp > 0) n += snprintf(s + n, sizeof(s) - n, "  %.0f\xC2\xB0", g_pc.cpuTemp);
    if (g_pc.cpuPower > 0) snprintf(s + n, sizeof(s) - n, "  %.0fW", g_pc.cpuPower);
  } else strcpy(s, "--");
  label_set(g_ui.pcMain[0], s);
  label_color(g_ui.pcMain[0], ok && (g_pc.cpu > 85 || g_pc.cpuTemp > 85) ? C_WARN : C_TEXT);
  s[0] = 0;
  if (ok && g_pc.cpuCoreMax >= 0) {
    int n = snprintf(s, sizeof(s), TRS("core max %.0f%%", "core max %.0f%%"), g_pc.cpuCoreMax);
    if (g_pc.cpuVolt > 0) snprintf(s + n, sizeof(s) - n, " " U_MIDDOT " %.2f V", g_pc.cpuVolt);
  }
  label_set(g_ui.pcSub[0], s);
  label_color(g_ui.pcSub[0], ok && g_pc.cpuCoreMax >= 95 ? C_WARN : C_MUTED);
  // gpu
  short_name(g_pc.gpuName, a, sizeof(a));
  snprintf(s, sizeof(s), "gpu%s%s", a[0] ? " " U_MIDDOT " " : "", a);
  label_set(g_ui.pcLeg[1], s);
  if (ok) {
    int n = snprintf(s, sizeof(s), "%.0f%%", g_pc.gpu);
    if (g_pc.gpuTemp > 0) n += snprintf(s + n, sizeof(s) - n, "  %.0f\xC2\xB0", g_pc.gpuTemp);
    if (g_pc.gpuPower > 0) snprintf(s + n, sizeof(s) - n, "  %.0fW", g_pc.gpuPower);
  } else strcpy(s, "--");
  label_set(g_ui.pcMain[1], s);
  s[0] = 0;
  if (ok && g_pc.vramTotalMb > 0) {
    int n = snprintf(s, sizeof(s), "vram %.1f / %.0f GB", g_pc.vramUsedMb / 1024.0f, g_pc.vramTotalMb / 1024.0f);
    if (g_pc.gpuHotspot > 0) snprintf(s + n, sizeof(s) - n, " " U_MIDDOT " hot %.0f\xC2\xB0", g_pc.gpuHotspot);
  }
  label_set(g_ui.pcSub[1], s);
  // ram
  if (ok && g_pc.ramTotalMb > 0) {
    int n = snprintf(s, sizeof(s), "ram %.0f%% " U_MIDDOT " %.1f / %.0f GB", g_pc.ram, g_pc.ramUsedMb / 1024.0f, g_pc.ramTotalMb / 1024.0f);
    float rt = 0;
    for (int i = 0; i < g_pc.ramTempN; i++) if (g_pc.ramTemps[i] > rt) rt = g_pc.ramTemps[i];
    if (rt > 0) snprintf(s + n, sizeof(s) - n, " " U_MIDDOT " %.0f\xC2\xB0", rt);
  }
  else if (ok) snprintf(s, sizeof(s), "ram %.0f%%", g_pc.ram);
  else strcpy(s, "ram");
  label_set(g_ui.pcLeg[2], s);
  label_set(g_ui.pcMain[2], "");
  // rete e disco
  if (ok && g_pc.extended) {
    fmt_rate(g_pc.netDown, a, sizeof(a)); fmt_rate(g_pc.netUp, b, sizeof(b));
    snprintf(s, sizeof(s), "\xE2\x86\x93 %s  \xE2\x86\x91 %s", a, b);
    label_set(g_ui.pcMain[3], s);
    fmt_rate(g_pc.diskRead, a, sizeof(a)); fmt_rate(g_pc.diskWrite, b, sizeof(b));
    snprintf(s, sizeof(s), TRS("disco r %s w %s", "disk r %s w %s"), a, b);
    label_set(g_ui.pcSys, s);
  } else {
    label_set(g_ui.pcMain[3], "--");
    label_set(g_ui.pcSys, "");
  }
  for (int i = 0; i < 3; i++) pc_spark(i);
  // sistema: uptime, claude code, dischi
  if (!ok) {
    label_set(g_ui.pcDisks, !g_pcHost[0] ? TRS("imposta l'indirizzo del pc dal browser (/home)", "set the pc address from the browser (/home)")
                                         : TRS("pc spento o Ritmo Code PC Monitor non attivo", "pc off or Ritmo Code PC Monitor not running"));
    label_color(g_ui.pcDisks, C_FAINT);
    return;
  }
  if (!g_pc.extended) {
    snprintf(s, sizeof(s), TRS("disco C %.0f%% " U_MIDDOT " installa Ritmo Code PC Monitor per piu' dati", "disk C %.0f%% " U_MIDDOT " install Ritmo Code PC Monitor for more"), g_pc.disk);
    label_set(g_ui.pcDisks, s); label_color(g_ui.pcDisks, C_MUTED);
    return;
  }
  uint32_t up = g_pc.uptime;
  int n;
  if (up >= 86400) n = snprintf(s, sizeof(s), TRS("acceso %ug %uh", "up %ud %uh"), (unsigned)(up / 86400), (unsigned)(up % 86400 / 3600));
  else             n = snprintf(s, sizeof(s), TRS("acceso %uh %um", "up %uh %um"), (unsigned)(up / 3600), (unsigned)(up % 3600 / 60));
  if (g_pc.claude >= 0) n += snprintf(s + n, sizeof(s) - n, " " U_MIDDOT " claude %d", g_pc.claude);
  // consumo di CPU + GPU e costo al giorno a questo ritmo (resto del sistema escluso)
  float watt = g_pc.cpuPower + g_pc.gpuPower;
  if (watt > 0) {
    char eur[16]; snprintf(eur, sizeof(eur), "%.2f", watt * 24.0f / 1000.0f * g_kwhPrice);
    for (char *q = eur; *q; q++) if (*q == '.') *q = ',';
    n += snprintf(s + n, sizeof(s) - n, TRS(" " U_MIDDOT " %.0f W ~%s \xE2\x82\xAC/g", " " U_MIDDOT " %.0f W ~%s \xE2\x82\xAC/d"), watt, eur);
  }
  // dischi fisici: il piu' caldo e la vita residua piu' bassa (da LibreHardwareMonitor)
  float dt = 0, life = 101;
  for (int i = 0; i < g_pc.driveN; i++) {
    if (g_pc.drives[i].temp > dt) dt = g_pc.drives[i].temp;
    if (g_pc.drives[i].life >= 0 && g_pc.drives[i].life < life) life = g_pc.drives[i].life;
  }
  if (dt > 0) n += snprintf(s + n, sizeof(s) - n, TRS(" " U_MIDDOT " dischi %.0f\xC2\xB0", " " U_MIDDOT " disks %.0f\xC2\xB0"), dt);
  if (life <= 100) snprintf(s + n, sizeof(s) - n, "/%.0f%%", life);   // temperatura/vita residua
  label_set(g_ui.pcDisks, s);
  label_color(g_ui.pcDisks, C_MUTED);
}

// Tile 4 — SETTIMANE: picco raggiunto nelle ultime 8 settimane
#define WK_BASE 150
#define WK_H 110
static void build_tile_weeks(lv_obj_t *t) {
  lv_obj_t *b = tbox(t, 13, 20, 454, 226, TRS("picco settimanale " U_MIDDOT " ultime 8", "weekly peak " U_MIDDOT " last 8"));
  hline(b, 14, WK_BASE, 428, C_BORDER);
  for (int i = 0; i < 8; i++) {
    int x = 16 + i * 54;
    g_ui.wkBar[i] = rrect(b, x, WK_BASE - 2, 40, 2, 0, C_TRACK);
    g_ui.wkVal[i] = tlabel(b, F12, C_MUTED, x - 6, WK_BASE - 20);
    lv_obj_set_width(g_ui.wkVal[i], 52);
    lv_obj_set_style_text_align(g_ui.wkVal[i], LV_TEXT_ALIGN_CENTER, 0);
    g_ui.wkDate[i] = tlabel(b, F12, C_FAINT, x - 6, WK_BASE + 6);
    lv_obj_set_width(g_ui.wkDate[i], 52);
    lv_obj_set_style_text_align(g_ui.wkDate[i], LV_TEXT_ALIGN_CENTER, 0);
  }
  g_ui.wkCap = box_caption(b);
}
static void weeks_redraw() {
  if (!g_ui.wkCap) return;
  int n = g_weekN < 8 ? g_weekN : 8, first = g_weekN - n;
  int sum = 0, mx = 0;
  for (int i = 0; i < 8; i++) {
    int k = i - (8 - n);                                 // barre allineate a destra (la piu' recente in fondo)
    if (k < 0) {
      lv_obj_set_size(g_ui.wkBar[i], 40, 2); lv_obj_set_y(g_ui.wkBar[i], WK_BASE - 2);
      lv_obj_set_style_bg_color(g_ui.wkBar[i], lv_color_hex(C_TRACK), 0);
      label_set(g_ui.wkVal[i], ""); label_set(g_ui.wkDate[i], "");
      continue;
    }
    const WeekRec &w = g_weeks[first + k];
    bool cur = (first + k == g_weekN - 1);
    int h = 2 + w.peak * WK_H / 100;
    lv_obj_set_size(g_ui.wkBar[i], 40, h);
    lv_obj_set_y(g_ui.wkBar[i], WK_BASE - h);
    lv_obj_set_style_bg_color(g_ui.wkBar[i], lv_color_hex(level_hex(w.peak)), 0);
    lv_obj_set_style_bg_opa(g_ui.wkBar[i], cur ? LV_OPA_COVER : LV_OPA_70, 0);
    char s[16]; snprintf(s, sizeof(s), "%d%%", w.peak);
    label_set(g_ui.wkVal[i], s);
    lv_obj_set_y(g_ui.wkVal[i], WK_BASE - h - 18);
    label_color(g_ui.wkVal[i], cur ? C_TEXT : C_MUTED);
    time_t st = (time_t)w.reset - 7 * 86400; struct tm tv; localtime_r(&st, &tv);
    if (cur) snprintf(s, sizeof(s), "%s", TRS("ora", "now"));
    else     snprintf(s, sizeof(s), "%02d/%02d", tv.tm_mday, tv.tm_mon + 1);
    label_set(g_ui.wkDate[i], s);
    label_color(g_ui.wkDate[i], cur ? C_TEXT : C_FAINT);
    if (!cur) { sum += w.peak; if (w.peak > mx) mx = w.peak; }
  }
  char c[96];
  if (n < 2) snprintf(c, sizeof(c), "%s", TRS("raccolta dati: ogni settimana conclusa aggiunge una barra",
                                               "collecting data: each finished week adds a bar"));
  else snprintf(c, sizeof(c), TRS("media %d%% " U_MIDDOT " massimo %d%% (settimane concluse)",
                                  "average %d%% " U_MIDDOT " max %d%% (finished weeks)"), sum / (n - 1), mx);
  label_set(g_ui.wkCap, c);
}

// intestazione: percorso della pagina corrente + account attivo
static void hdr_identity() {
  static const char *PATH[NTILES] = {"/home", "/usage", "/models", "/window", "/rhythm", "/weeks", "/pc"};
  if (g_ui.hdrPath) label_set(g_ui.hdrPath, PATH[g_curTile < NTILES ? g_curTile : 0]);
}
static void on_tile_changed(lv_event_t *e) {
  (void)e;
  if (!g_ui.tv) return;
  lv_obj_t *act = lv_tileview_get_tile_active(g_ui.tv);
  const char *names[NTILES] = {"home", TRS("ora", "now"), TRS("modelli", "models"), "5h", TRS("ritmo", "rhythm"), TRS("settimane", "weeks"), "pc"};
  for (int i = 0; i < NTILES; i++) {
    bool on = (g_ui.tile[i] == act);
    if (on) g_curTile = i;
    if (!g_ui.tab[i]) continue;
    char b[16];
    if (on) snprintf(b, sizeof(b), "[%s]", names[i]); else snprintf(b, sizeof(b), "%s", names[i]);
    label_set(g_ui.tab[i], b);
    label_color(g_ui.tab[i], on ? C_TEXT : C_FAINT);
  }
  hdr_identity();
}
static void tab_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_ui.tv) lv_tileview_set_tile_by_index(g_ui.tv, i, 0, LV_ANIM_ON);
}

// ============================================================
// Aggiornamento dei valori
// ============================================================
// Contatori/orologi (1s) — separati dai valori del fetch.
static void pace_update();
static void reset_watch();
static void dash_tick() {
  if (g_state != ST_MAIN || !g_ui.agCd5) return;
  reset_watch();
  pace_update();
  char e[32], c[24];
  fmt_eta(g_usage.h5ResetEpoch, e, sizeof(e));
  label_set(g_ui.agCd5, e);
  fmt_clock(g_usage.h5ResetEpoch, c, sizeof(c));
  for (char *q = c; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
  label_set(g_ui.agAt5, c);

  fmt_eta(g_usage.d7ResetEpoch, e, sizeof(e));
  label_set(g_ui.agCd7, e);
  fmt_clock(g_usage.d7ResetEpoch, c, sizeof(c));
  for (char *q = c; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
  label_set(g_ui.agAt7, c);

  set_hdr_status();
}

static void trend_cap(const char *txt, uint32_t col) {
  label_set(g_ui.trCap, txt);
  label_color(g_ui.trCap, col);
}
// Tendenza della finestra 5h: storico + proiezione tratteggiata fino all'esaurimento.
static void trend_redraw() {
  if (!g_ui.trHist) return;
  time_t now = time(nullptr);
  uint32_t we = g_usage.h5ResetEpoch;
  bool clockOk = (now > 1000000000L) && we != 0;

  if (!clockOk) {
    lv_line_set_points(g_ui.trHist, g_trPts, 0);
    lv_line_set_points(g_ui.trProj, g_trProjPts, 0);
    lv_obj_add_flag(g_ui.trDot, LV_OBJ_FLAG_HIDDEN);
    trend_cap(TRS("in attesa dei dati della finestra...", "waiting for window data..."), C_MUTED);
    return;
  }
  uint32_t ws = we - 5 * 3600;

  char t0[12], t1[12], b[112];
  fmt_hm(ws, t0, sizeof(t0)); fmt_hm(we, t1, sizeof(t1));
  label_set(g_ui.trT0, t0);
  label_set(g_ui.trT1, t1);

  int n = 0;
  for (int i = 0; i < g_histN && n < HIST_MAX; i++) {
    Sample s = g_hist[hist_idx(i)];
    if (s.t == 0 || s.t < ws || s.t > (uint32_t)now) continue;
    g_trPts[n].x = tr_x(s.t, ws, we);
    g_trPts[n].y = tr_y(s.h5);
    n++;
  }
  uint32_t nowClamped = ((uint32_t)now > we) ? we : (uint32_t)now;
  if (n < HIST_MAX) {
    g_trPts[n].x = tr_x(nowClamped, ws, we);
    g_trPts[n].y = tr_y(g_usage.h5);
    n++;
  }
  lv_line_set_points(g_ui.trHist, g_trPts, n);

  int cx = tr_x(nowClamped, ws, we), cy = tr_y(g_usage.h5);
  lv_obj_set_pos(g_ui.trDot, cx - 3, cy - 3);
  lv_obj_clear_flag(g_ui.trDot, LV_OBJ_FLAG_HIDDEN);

  if (n < 3) {
    lv_line_set_points(g_ui.trProj, g_trProjPts, 0);
    trend_cap(TRS("raccolta dati... (~qualche minuto)", "collecting data... (~a few minutes)"), C_MUTED);
    return;
  }

  float rate = 0;                    // %/min negli ultimi 45 min
  {
    Sample first = {0, 0, 0};
    for (int i = 0; i < g_histN; i++) {
      Sample s = g_hist[hist_idx(i)];
      if (s.t == 0 || s.t < ws) continue;
      if (s.t >= (uint32_t)now - 2700) { first = s; break; }
    }
    if (first.t != 0 && (uint32_t)now > first.t + 300) {
      float dt = ((uint32_t)now - first.t) / 60.0f;
      rate = (g_usage.h5 - first.h5) / dt;
    }
  }

  char e[32];
  if (g_usage.h5 >= 99.5f) {
    lv_line_set_points(g_ui.trProj, g_trProjPts, 0);
    fmt_eta(we, e, sizeof(e));
    snprintf(b, sizeof(b), TRS("finestra esaurita " U_MIDDOT " reset tra %s", "window exhausted " U_MIDDOT " resets in %s"), e);
    trend_cap(b, C_BAD);
  } else if (rate > 0.02f) {
    float minsLeft = (100.0f - g_usage.h5) / rate;
    uint32_t etaT = (uint32_t)now + (uint32_t)(minsLeft * 60);
    g_trProjPts[0].x = cx; g_trProjPts[0].y = cy;
    if (etaT <= we) {
      g_trProjPts[1].x = tr_x(etaT, ws, we);
      g_trProjPts[1].y = tr_y(100);
      char hm[12]; fmt_hm(etaT, hm, sizeof(hm));
      snprintf(b, sizeof(b), TRS("a questo ritmo finisce alle %s (tra %dh%02dm)", "at this pace it runs out at %s (in %dh%02dm)"),
               hm, (int)minsLeft / 60, (int)minsLeft % 60);
      trend_cap(b, minsLeft < 60 ? C_BAD : C_WARN);
    } else {
      float endPct = g_usage.h5 + rate * ((we - (uint32_t)now) / 60.0f);
      g_trProjPts[1].x = tr_x(we, ws, we);
      g_trProjPts[1].y = tr_y(endPct);
      snprintf(b, sizeof(b), TRS("a questo ritmo NON finisce prima del reset (~%d%%)", "at this pace it does NOT run out before reset (~%d%%)"),
               (int)(endPct + 0.5f));
      trend_cap(b, C_OK);
    }
    lv_line_set_points(g_ui.trProj, g_trProjPts, 2);
  } else {
    lv_line_set_points(g_ui.trProj, g_trProjPts, 0);
    trend_cap(TRS("uso stabile " U_MIDDOT " nessun rischio ora", "stable usage " U_MIDDOT " no risk right now"), C_OK);
  }
}

static void heat_redraw() {
  if (!g_ui.heat[0]) return;
  float data[24];
  bool pomo = g_heatSrc == 1;
  if (pomo) pomo_mode_data(g_heatMode, data); else heat_mode_data(g_heatMode, data);
  if (g_ui.heatCap) {
    if (pomo) {
      static const char *PER_IT[4] = {"oggi", "negli ultimi 7 giorni", "negli ultimi 30 giorni", "da sempre"};
      static const char *PER_EN[4] = {"today", "in the last 7 days", "in the last 30 days", "all time"};
      int tot = 0; for (int h = 0; h < 24; h++) tot += (int)data[h];
      char c[80];
      snprintf(c, sizeof(c), TRS("%d %s %s, per ora locale", "%d %s %s, per local hour"), tot,
               tot == 1 ? "pomodoro" : TRS("pomodori", "pomodoros"), g_lang ? PER_EN[g_heatMode] : PER_IT[g_heatMode]);
      label_set(g_ui.heatCap, c);
    } else {
      label_set(g_ui.heatCap, TRS("quota 5h consumata per ora locale", "5h quota burned per local hour"));
    }
  }
  float mx = 1.0f;
  for (int h = 0; h < 24; h++) if (data[h] > mx) mx = data[h];
  int curHour = -1; time_t now = time(nullptr);
  if (now > 1000000000L) { struct tm tv; localtime_r(&now, &tv); curHour = tv.tm_hour; }
  for (int h = 0; h < 24; h++) {
    if (!g_ui.heat[h]) continue;
    float r = data[h] / mx; if (r < 0) r = 0; if (r > 1) r = 1;
    int hgt = 2 + (int)(r * HEAT_H);
    lv_obj_set_size(g_ui.heat[h], 14, hgt);
    lv_obj_set_y(g_ui.heat[h], HEAT_BASE - hgt);
    lv_obj_set_style_bg_color(g_ui.heat[h],
        h == curHour ? lv_color_hex(C_TEXT)
                     : lv_color_mix(lv_color_hex(pomo ? C_BAD : C_ACCENT), lv_color_hex(C_BG), (uint8_t)(70 + (int)(r * 185))), 0);
  }
}

// ============================================================
// Momenti — animazioni di soglia (25/50/70/100% su 5h e settimana)
// Overlay in lv_layer_top: riquadro con la cornice del colore del livello,
// Clawd XL con l'umore, percentuale che conta, barra a blocchi che si riempie.
// ============================================================
static const uint8_t THR[4] = {25, 50, 70, 100};
struct MomentUI {
  lv_obj_t *scrim, *box, *img, *pct, *frame;
  Blocks blk;
  lv_obj_t *drop[2];
  int win, thr, fromPct;
  uint32_t col;
  int boxY;
  uint32_t t0;
};
static MomentUI g_mo = {};
static uint32_t g_momentUntil = 0;
static int g_pendWin = -1, g_pendThr = 0;
static uint8_t g_thrFired[2] = {0, 0};
static float g_thrPrev[2] = {-1, -1};
static bool g_thrBase = false;
// avviso di reset: picco della finestra corrente e reset gia' segnalati
#define RESET_ALERT_PEAK 80
static float g_winPeak[2] = {0, 0};
static uint32_t g_winReset[2] = {0, 0}, g_resetSeen[2] = {0, 0};
static int g_pendPeak = 0;
static void reset_fire(int w) {
  g_resetSeen[w] = g_winReset[w];
  if (g_resetAlert && g_winPeak[w] >= RESET_ALERT_PEAK) {
    g_pendWin = w; g_pendThr = 0; g_pendPeak = (int)(g_winPeak[w] + 0.5f);
    Serial.printf("[RESET] finestra %s libera (picco %d%%)\n", w ? "7g" : "5h", g_pendPeak);
  }
  g_winPeak[w] = 0;
}
// allo scoccare dell'orario di reset (ogni secondo, senza aspettare il prossimo fetch)
static void reset_watch() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return;
  for (int w = 0; w < 2; w++)
    if (g_winReset[w] && (uint32_t)now >= g_winReset[w] && g_resetSeen[w] != g_winReset[w]) reset_fire(w);
}
static lv_point_precise_t g_moXPts[4][2];

// Rileva il superamento di soglia dopo ogni fetch (baseline al primo fetch,
// azzera quando la finestra si resetta: calo > 15 punti).
static void check_thresholds() {
  float c[2] = {g_usage.h5, g_usage.d7};
  uint32_t re[2] = {g_usage.h5ResetEpoch, g_usage.d7ResetEpoch};
  for (int w = 0; w < 2; w++) {
    // calo netto senza che l'orario l'abbia gia' segnalato (es. ora non sincronizzata)
    if (g_thrBase && (g_thrPrev[w] - c[w]) > 15.0f && g_resetSeen[w] != g_winReset[w]) reset_fire(w);
    if (re[w]) g_winReset[w] = re[w];
    if (c[w] > g_winPeak[w]) g_winPeak[w] = c[w];
    if (!g_thrBase || (g_thrPrev[w] - c[w]) > 15.0f) {
      g_thrFired[w] = 0;
      for (int i = 0; i < 4; i++) if (c[w] >= THR[i]) g_thrFired[w] |= 1 << i;
    } else {
      int hit = -1;
      for (int i = 0; i < 4; i++)
        if (c[w] >= THR[i] && !(g_thrFired[w] & (1 << i))) { g_thrFired[w] |= 1 << i; hit = i; }
      if (hit >= 0) { g_pendWin = w; g_pendThr = THR[hit]; }
    }
    g_thrPrev[w] = c[w];
  }
  g_thrBase = true;
}

static void moment_close() {
  if (!g_mo.scrim) return;
  lv_obj_delete(g_mo.scrim);
  memset(&g_mo, 0, sizeof(g_mo));
}
static void moment_close_cb(lv_event_t *e) { (void)e; moment_close(); }

static void show_moment(int win, int thr) {
  moment_close();
  g_mo.win = win; g_mo.thr = thr;
  // thr 0 = finestra di nuovo disponibile: la barra scende dal picco a zero
  g_mo.fromPct = (thr == 0) ? (g_pendPeak > 0 ? g_pendPeak : 90)
               : (thr == 25) ? 0 : (thr == 50) ? 25 : (thr == 70) ? 50 : 70;
  g_mo.col = (thr == 100) ? C_BAD : (thr == 70) ? C_WARN : (thr == 50) ? C_ACCENT : C_OK;
  g_mo.t0 = millis();
  g_momentUntil = g_mo.t0 + 4600;

  lv_obj_t *s = plain_obj(lv_layer_top());
  g_mo.scrim = s;
  lv_obj_set_pos(s, 0, 0); lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, moment_close_cb, LV_EVENT_CLICKED, NULL);

  char lg[48];
  if (thr == 0)
    snprintf(lg, sizeof(lg), TRS("reset " U_MIDDOT " finestra %s", "reset " U_MIDDOT " %s window"),
             win ? TRS("7 giorni", "7-day") : TRS("5 ore", "5-hour"));
  else
    snprintf(lg, sizeof(lg), TRS("avviso " U_MIDDOT " finestra %s", "alert " U_MIDDOT " %s window"),
             win ? TRS("7 giorni", "7-day") : TRS("5 ore", "5-hour"));
  lv_obj_t *lgl = nullptr;
  g_mo.frame = tbox(s, 10, 14, 460, 296, lg, g_mo.col, &lgl);
  if (lgl) lv_obj_set_style_text_color(lgl, lv_color_hex(g_mo.col), 0);

  g_mo.boxY = 104;
  lv_obj_t *bx = plain_obj(s);
  g_mo.box = bx;
  lv_obj_set_pos(bx, 30, g_mo.boxY - 40);
  lv_obj_set_size(bx, 176, 116);

  g_mo.img = lv_image_create(bx);
  lv_image_set_src(g_mo.img, &img_clawd_xl);
  lv_obj_set_pos(g_mo.img, 0, 0);

  const int ex[2] = {CLAWD_XL_EYE0_X, CLAWD_XL_EYE1_X};
  const int ey = CLAWD_XL_EYE0_Y, ew = CLAWD_XL_EYE0_W, eh = CLAWD_XL_EYE0_H;
  if (thr == 50) {
    for (int i = 0; i < 2; i++) rrect(bx, ex[i] - 1, ey - 1, ew + 2, eh / 2 + 2, 0, C_ACCENT);
    g_mo.drop[0] = rrect(bx, 150, 6, 8, 12, 4, C_BLUE);
  } else if (thr == 70) {
    for (int i = 0; i < 2; i++) rrect(bx, ex[i] - 3, ey - 4, ew + 6, eh + 8, 0, C_BG);
    g_mo.drop[0] = rrect(bx, 150, 6, 8, 12, 4, C_BLUE);
    g_mo.drop[1] = rrect(bx, 18, 12, 8, 12, 4, C_BLUE);
  } else if (thr == 100) {
    lv_obj_set_style_image_recolor(g_mo.img, lv_color_mix(lv_color_hex(0x6A6A74), lv_color_hex(C_ACCENT), 190), 0);
    lv_obj_set_style_image_recolor_opa(g_mo.img, LV_OPA_COVER, 0);
    for (int i = 0; i < 2; i++) {
      g_moXPts[i * 2][0]     = { (lv_value_precise_t)(ex[i] - 2), (lv_value_precise_t)(ey - 1) };
      g_moXPts[i * 2][1]     = { (lv_value_precise_t)(ex[i] + ew + 2), (lv_value_precise_t)(ey + eh + 1) };
      g_moXPts[i * 2 + 1][0] = { (lv_value_precise_t)(ex[i] + ew + 2), (lv_value_precise_t)(ey - 1) };
      g_moXPts[i * 2 + 1][1] = { (lv_value_precise_t)(ex[i] - 2), (lv_value_precise_t)(ey + eh + 1) };
      for (int k = 0; k < 2; k++) {
        lv_obj_t *ln = lv_line_create(bx);
        lv_line_set_points(ln, g_moXPts[i * 2 + k], 2);
        lv_obj_set_style_line_width(ln, 4, 0);
        lv_obj_set_style_line_color(ln, lv_color_hex(C_BAD), 0);
      }
    }
  }

  tstatic(s, win == 0 ? TRS("finestra 5 ore", "5-hour window") : TRS("finestra 7 giorni", "7-day window"),
          F14, C_MUTED, 234, 46);
  lv_obj_t *r = trow(s, 232, 72);
  g_mo.pct = mklabel(r, "0", F54, g_mo.col);
  mklabel(r, "%", F22, C_MUTED);
  const char *MSG[5] = {
    TRS("si parte: ritmo tranquillo",          "just starting: easy pace"),
    TRS("metà finestra consumata",             "half the window used"),
    TRS("attenzione: uso elevato",             "heads up: heavy usage"),
    TRS("limite raggiunto: attendi il reset",  "limit reached: wait for the reset"),
    TRS("di nuovo disponibile: si riparte",    "available again: back to work"),
  };
  int mi = (thr == 0) ? 4 : (thr == 25) ? 0 : (thr == 50) ? 1 : (thr == 70) ? 2 : 3;
  tstatic(s, ">", F14, C_ACCENT, 234, 138);
  lv_obj_t *msg = tstatic(s, MSG[mi], F14, C_TEXT, 252, 138);
  lv_obj_set_width(msg, 204);
  lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);

  g_mo.blk = blocks_create(s, 234, 196, NBLK, F12);

  char e[32], b[48];
  if (thr == 0) {
    snprintf(b, sizeof(b), TRS("picco della finestra: %d%%", "window peak: %d%%"), g_mo.fromPct);
  } else {
    fmt_eta(win == 0 ? g_usage.h5ResetEpoch : g_usage.d7ResetEpoch, e, sizeof(e));
    snprintf(b, sizeof(b), TRS("reset tra %s", "resets in %s"), e);
  }
  tstatic(s, b, F12, C_MUTED, 234, 222);
  tstatic(s, TRS("[ tocca per chiudere ]", "[ tap to close ]"), F12, C_FAINT, 290, 280);
}

// Anima il momento (chiamato a ogni frame del loop finche' l'overlay esiste).
static void moment_tick() {
  if (!g_mo.scrim) return;
  uint32_t t = millis() - g_mo.t0;

  int y = g_mo.boxY, x = 30;
  if (t < 450) {
    float p = t / 450.0f;
    y = g_mo.boxY - (int)((1.0f - p) * (1.0f - p) * 60.0f);
  } else if (g_mo.thr <= 50) {
    y = g_mo.boxY + (int)(4.0f * sinf((t - 450) / 260.0f));
  } else if (g_mo.thr == 70) {
    x = 30 + (((t / 70) % 2) ? 2 : -2);
  } else if (g_mo.thr == 100) {
    y = g_mo.boxY + 6;
  }
  lv_obj_set_pos(g_mo.box, x, y);

  float p = (t < 200) ? 0 : (t > 1100 ? 1.0f : (t - 200) / 900.0f);
  float v = g_mo.fromPct + (g_mo.thr - g_mo.fromPct) * p;
  char b[12]; snprintf(b, sizeof(b), "%d", (int)(v + 0.5f));
  label_set(g_mo.pct, b);
  blocks_set(g_mo.blk, v, g_mo.col);

  for (int i = 0; i < 2; i++) {
    if (!g_mo.drop[i]) continue;
    uint32_t c = (t + i * 450) % 900;
    lv_obj_set_y(g_mo.drop[i], (i ? 12 : 6) + (int)(c * 34 / 900));
    lv_obj_set_style_bg_color(g_mo.drop[i], lv_color_mix(lv_color_hex(C_BLUE), lv_color_hex(C_BG), (uint8_t)(255 - c * 190 / 900)), 0);
  }
  if (g_mo.thr == 100 && g_mo.frame)
    lv_obj_set_style_border_color(g_mo.frame, lv_color_hex(((t / 350) % 2) ? C_BAD : C_BORDER), 0);

  if (millis() > g_momentUntil) moment_close();
}

// ============================================================
// Avviso a schermo intero con Clawd: Claude Code, timer e pomodoro.
// Resta finche' non lo tocchi (o scade), sopravvive ai rebuild del dashboard.
// ============================================================
enum { NT_NONE = 0, NT_CLAUDE, NT_TIMER };
struct NoticeUI { lv_obj_t *scrim, *box, *frame, *ask; uint32_t t0, col, maxMs; int kind; bool hop, idle; };
static NoticeUI g_nt = {};
static int g_ntPend = NT_NONE;                 // avviso da mostrare appena si puo'
static uint32_t g_ntT0 = 0;                    // != 0: da rimettere dopo un rebuild (stesso inizio)
struct NoticeText {
  int kind; uint32_t col, maxMs;
  char ev[8];                                  // tipo di evento per il suono sul PC
  bool hop, ask;                               // Clawd salta contento / punto di domanda e cornice che pulsa
  char legend[48], top[40], word[24], unit[16], msg[64], foot[40];
  int big;                                     // numero grande (F54, solo cifre); < 0 = mostra `word`
};

static void notice_close() {
  if (!g_nt.scrim) return;
  lv_obj_delete(g_nt.scrim);
  memset(&g_nt, 0, sizeof(g_nt));
}
static void notice_close_cb(lv_event_t *e) { (void)e; notice_close(); }

// ultimi avvisi (a schermo intero e di soglia): pannello a tendina e doppio tocco su "ritmo-code"
struct AlertRec { uint32_t at; bool moment; int8_t win, thr; uint8_t peak; NoticeText nt; };
#define AL_MAX 5
static AlertRec g_al[AL_MAX];                  // [0] = il piu' recente
static int g_alN = 0;
#define AL_SAVED 3                             // quanti sopravvivono al riavvio
#define AL_MAGIC 0xC1A0DE20
static void al_save() {
  File f = LittleFS.open("/alerts.bin", "w");
  if (!f) return;
  uint32_t m = AL_MAGIC; int n = g_alN < AL_SAVED ? g_alN : AL_SAVED;
  f.write((uint8_t *)&m, 4);
  f.write((uint8_t *)&n, sizeof(n));
  f.write((uint8_t *)g_al, sizeof(AlertRec) * n);
  f.close();
}
static void al_load() {
  File f = LittleFS.open("/alerts.bin", "r");
  if (!f) return;
  uint32_t m = 0; int n = 0;
  if (f.read((uint8_t *)&m, 4) == 4 && m == AL_MAGIC && f.read((uint8_t *)&n, sizeof(n)) == sizeof(n) &&
      n >= 0 && n <= AL_SAVED && f.read((uint8_t *)g_al, sizeof(AlertRec) * n) == sizeof(AlertRec) * n)
    g_alN = n;
  f.close();
}
static void al_push(const AlertRec &r) {
  memmove(&g_al[1], &g_al[0], sizeof(AlertRec) * (AL_MAX - 1));
  g_al[0] = r;
  if (g_alN < AL_MAX) g_alN++;
  al_save();
}
static bool g_ntReplay = false;                // riaperto dall'utente: niente suono sul PC
static void notice_show(const NoticeText &n) {
  notice_close();
  g_nt.kind = n.kind; g_nt.col = n.col; g_nt.hop = n.hop; g_nt.maxMs = n.maxMs;
  if (!g_ntT0 && !g_ntReplay) {               // avviso nuovo: in cronologia
    AlertRec r = {};
    r.at = (uint32_t)time(nullptr); r.moment = false; r.nt = n;
    al_push(r);
  }
  if (!g_ntT0 && !g_ntReplay) {               // avviso nuovo (non rimesso dopo un rebuild): suono sul PC
    char title[64];
    if (n.big >= 0) snprintf(title, sizeof(title), "%s " U_MIDDOT " %d%s", n.top, n.big, n.unit);
    else            strlcpy(title, n.top, sizeof(title));
    char msg[96];
    if (n.kind == NT_CLAUDE && g_ccProj[0]) snprintf(msg, sizeof(msg), "%s (%s)", n.msg, g_ccProj);
    else                                    strlcpy(msg, n.msg, sizeof(msg));
    pc_notify(n.ev, title, msg);
  }
  g_nt.t0 = g_ntT0 ? g_ntT0 : millis();
  g_ntT0 = 0;

  lv_obj_t *s = plain_obj(lv_layer_top());
  g_nt.scrim = s;
  lv_obj_set_pos(s, 0, 0); lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, notice_close_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *lgl = nullptr;
  g_nt.frame = tbox(s, 10, 14, 460, 296, n.legend, n.col, &lgl);
  if (lgl) lv_obj_set_style_text_color(lgl, lv_color_hex(n.col), 0);

  lv_obj_t *bx = plain_obj(s);
  g_nt.box = bx;
  lv_obj_set_pos(bx, 30, 64);
  lv_obj_set_size(bx, 176, 116);
  lv_obj_t *img = lv_image_create(bx);
  lv_image_set_src(img, &img_clawd_xl);
  lv_obj_set_pos(img, 0, 0);
  if (n.ask) g_nt.ask = tstatic(bx, "?", F22, C_TEXT, 154, 0);

  tstatic(s, n.top, F14, C_MUTED, 234, 46);
  if (n.big >= 0) {
    lv_obj_t *r = trow(s, 232, 72);
    char b[12]; snprintf(b, sizeof(b), "%d", n.big);
    mklabel(r, b, F54, n.col);
    mklabel(r, n.unit, F22, C_MUTED);
  } else {
    tstatic(s, n.word, F22, n.col, 234, 92);
  }
  tstatic(s, ">", F14, C_ACCENT, 234, 138);
  lv_obj_t *m = tstatic(s, n.msg, F14, C_TEXT, 252, 138);
  lv_obj_set_width(m, 204);
  lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
  if (n.foot[0]) tstatic(s, n.foot, F12, C_MUTED, 234, 222);
  tstatic(s, TRS("[ tocca per chiudere ]", "[ tap to close ]"), F12, C_FAINT, 290, 280);
}

// Animazione piena nei primi 20 s, poi un richiamo ogni 15 s: niente ridisegni continui per mezz'ora.
static void notice_tick() {
  if (!g_nt.scrim) return;
  uint32_t t = millis() - g_nt.t0;
  if (t > g_nt.maxMs) { notice_close(); return; }
  uint32_t ph = t < 20000 ? t : (t - 20000) % 15000;
  bool live = t < 20000 || ph < 1600;
  if (!live && g_nt.idle) return;              // fermo: l'ultimo frame e' gia' a riposo
  g_nt.idle = !live;
  int y = 104;
  if (t < 450) {
    float p = t / 450.0f;
    y = 104 - (int)((1.0f - p) * (1.0f - p) * 60.0f);
  } else if (live && g_nt.hop) {               // salta contento
    uint32_t h = (t - 450) % 1600;
    if (h < 400) y = 104 - (int)(16.0f * sinf(3.14159f * h / 400.0f));
  } else if (live) {                           // ondeggia, il punto di domanda va su e giu'
    y = 104 + (int)(3.0f * sinf(t / 400.0f));
    if (g_nt.ask) lv_obj_set_y(g_nt.ask, (int)(3.0f - 3.0f * sinf(t / 300.0f)));
  }
  lv_obj_set_pos(g_nt.box, 30, y);
  if (!g_nt.hop && g_nt.frame)
    lv_obj_set_style_border_color(g_nt.frame, lv_color_hex(live && (t / 600) % 2 ? C_BORDER : g_nt.col), 0);
}

// ---- Claude Code: fine lavoro / permesso ----
// Hook di Claude Code -> Ritmo Code PC Monitor -> POST /claude (subito) o data.json (al giro dopo).
// Si chiude da solo dopo 5, 10 o 30 s (impostazione), con un tocco o quando scrivi di nuovo a Claude ("busy").
static const uint16_t CC_MIN_S[4] = {0, 0, 60, 300};   // durata minima del lavoro per l'avviso di fine
static long g_ccSeen = 0;                      // id dell'ultimo evento gia' visto (crescono sempre)
static int  g_ccDur = -1;

static void cc_event(long id, const char *ev, const char *proj, int dur, int age) {
  if (id <= g_ccSeen) return;                  // gia' visto (arriva sia dal POST sia da data.json)
  g_ccSeen = id;
  if (age > 120) return;                       // vecchio: letto all'avvio o dopo che il pc non rispondeva
  Serial.printf("[CLAUDE] %s %s (%d s)\n", ev, proj, dur);
  if (!strcmp(ev, "busy")) {                   // sei tornato a scrivere a Claude
    if (g_ntPend == NT_CLAUDE) g_ntPend = NT_NONE;
    if (g_nt.kind == NT_CLAUDE) notice_close();
    g_ccDeferred = false;                        // l'avviso rimandato non serve piu'
    return;
  }
  if (!g_ccAlert) return;
  if (!strcmp(ev, "done") && dur >= 0 && dur < CC_MIN_S[g_ccAlert]) return;
  strlcpy(g_ccEv, ev, sizeof(g_ccEv));
  strlcpy(g_ccProj, proj, sizeof(g_ccProj));
  g_ccDur = dur;
  if (g_ccFocusDefer && g_tmMode == TM_FOCUS) {    // focus: lo mostra alla pausa (o quando fermi il pomodoro)
    g_ccDeferred = true;
    Serial.println("[CLAUDE] rimandato alla pausa");
    return;
  }
  if (g_ntPend != NT_TIMER) { g_ntPend = NT_CLAUDE; g_ntT0 = 0; }
}

static void cc_show() {
  NoticeText n = {};
  bool done = !strcmp(g_ccEv, "done"), ask = !strcmp(g_ccEv, "ask");
  n.kind = NT_CLAUDE; n.col = done ? C_OK : C_ACCENT;
  n.maxMs = CC_CLOSE_S[g_ccCloseIdx] ? CC_CLOSE_S[g_ccCloseIdx] * 1000UL : 0xFFFFFFFFUL;   // "mai": solo tocco o nuovo prompt
  strlcpy(n.ev, g_ccEv, sizeof(n.ev));
  n.hop = done; n.ask = !done;
  if (g_ccProj[0]) snprintf(n.legend, sizeof(n.legend), "claude code " U_MIDDOT " %s", g_ccProj);
  else             strcpy(n.legend, "claude code");
  strlcpy(n.top, done ? TRS("claude ha finito", "claude is done") : TRS("claude ti aspetta", "claude needs you"), sizeof(n.top));
  n.big = -1;
  if (done && g_ccDur >= 0) {                  // durata del lavoro in grande
    n.big = g_ccDur >= 60 ? (g_ccDur + 30) / 60 : g_ccDur;
    strcpy(n.unit, g_ccDur >= 60 ? " min" : " s");
  }
  strlcpy(n.word, done ? TRS("fatto", "done") : ask ? TRS("una domanda", "a question") : TRS("un permesso", "a permission"), sizeof(n.word));
  strlcpy(n.msg, done ? TRS("tocca a te: rivedi e continua", "your turn: review and continue")
               : ask  ? TRS("ha una domanda per te", "has a question for you")
                      : TRS("serve un tuo permesso per continuare", "needs your permission to continue"), sizeof(n.msg));
  time_t now = time(nullptr);
  if (now > 1000000000L) {
    char hm[12]; fmt_hm((uint32_t)now, hm, sizeof(hm));
    snprintf(n.foot, sizeof(n.foot), done ? TRS("alle %s", "at %s") : TRS("dalle %s", "since %s"), hm);
  }
  notice_show(n);
}

// ---- Timer e pomodoro (tocca l'ora nella home) ----
// Pomodoro: 25 min di focus e 5 di pausa, pausa lunga di 15 dopo il quarto, poi si ferma.
enum { TN_TIMER = 1, TN_BREAK, TN_FOCUS, TN_CYCLE };
static int g_tmNotice = 0;                     // quale avviso di fine fase mostrare

static long local_day() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return 0;
  struct tm tv; localtime_r(&now, &tv);
  return (tv.tm_year + 1900) * 1000L + tv.tm_yday;
}
static int pomo_today() {
  long d = local_day();
  return !d || d == g_pomoDay ? g_pomoToday : 0;
}
static void pomo_count() {
  long d = local_day();
  if (d && d != g_pomoDay) { g_pomoDay = d; g_pomoToday = 0; g_prefs.putLong("pomday", d); }
  g_pomoToday++;
  g_prefs.putInt("pomn", g_pomoToday);
}
static const char *pomo_word(int n) {
  return n == 1 ? "pomodoro" : TRS("pomodori", "pomodoros");
}
static uint32_t tm_left_s() {
  int32_t l = (int32_t)(g_tmEndMs - millis());
  return l > 0 ? (uint32_t)(l + 999) / 1000 : 0;
}
static void tm_start(int mode, uint32_t secs) {
  g_tmMode = mode; g_tmLenS = secs;
  g_tmEndMs = millis() + secs * 1000UL;
  Serial.printf("[TIMER] %s %u s\n", mode == TM_TIMER ? "timer" : mode == TM_FOCUS ? "focus" : "pausa", (unsigned)secs);
}
// testo breve della fase: "focus 2/4 18:42", "pausa 04:10", "timer 07:30"
static void tm_label(char *out, size_t sz) {
  uint32_t l = tm_left_s();
  char c[12];
  if (l >= 3600) snprintf(c, sizeof(c), "%u:%02u:%02u", (unsigned)(l / 3600), (unsigned)(l % 3600 / 60), (unsigned)(l % 60));
  else           snprintf(c, sizeof(c), "%02u:%02u", (unsigned)(l / 60), (unsigned)(l % 60));
  if (g_tmMode == TM_FOCUS)      snprintf(out, sz, "focus %d/%d %s", g_pomoN + 1, POMO_CYCLE, c);
  else if (g_tmMode == TM_BREAK) snprintf(out, sz, TRS("pausa %s", "break %s"), c);
  else                           snprintf(out, sz, "timer %s", c);
}
static uint32_t tm_color() {
  return g_tmMode == TM_FOCUS ? C_ACCENT : g_tmMode == TM_BREAK ? C_OK : C_TEXT;
}

static void tm_show() {
  NoticeText n = {};
  n.kind = NT_TIMER; n.hop = true; n.maxMs = 2UL * 60UL * 1000UL;
  int today = pomo_today();
  if (g_tmNotice == TN_TIMER) {
    n.col = C_WARN; n.maxMs = 10UL * 60UL * 1000UL;
    strcpy(n.legend, "timer"); strcpy(n.ev, "timer");
    strlcpy(n.top, TRS("tempo scaduto", "time's up"), sizeof(n.top));
    n.big = g_tmLenS >= 60 ? g_tmLenS / 60 : g_tmLenS;
    strcpy(n.unit, g_tmLenS >= 60 ? " min" : " s");
    if (g_tmLenS >= 60) snprintf(n.msg, sizeof(n.msg), TRS("timer di %u min finito", "%u min timer finished"), (unsigned)(g_tmLenS / 60));
    else                strlcpy(n.msg, TRS("timer finito", "timer finished"), sizeof(n.msg));
    time_t now = time(nullptr);
    if (now > 1000000000L) { char hm[12]; fmt_hm((uint32_t)now, hm, sizeof(hm)); snprintf(n.foot, sizeof(n.foot), TRS("alle %s", "at %s"), hm); }
  } else if (g_tmNotice == TN_CYCLE) {
    n.col = C_OK; n.maxMs = 10UL * 60UL * 1000UL;
    strcpy(n.legend, "pomodoro"); strcpy(n.ev, "cycle");
    strlcpy(n.top, TRS("ciclo completato", "cycle complete"), sizeof(n.top));
    n.big = POMO_CYCLE; strlcpy(n.unit, TRS(" pomodori", " pomodoros"), sizeof(n.unit));
    strlcpy(n.msg, TRS("ottimo lavoro: fai una pausa vera", "great work: take a real break"), sizeof(n.msg));
    snprintf(n.foot, sizeof(n.foot), TRS("oggi: %d %s", "today: %d %s"), today, pomo_word(today));
  } else if (g_tmNotice == TN_BREAK) {
    n.col = C_OK;
    snprintf(n.legend, sizeof(n.legend), "pomodoro %d/%d", g_pomoN, POMO_CYCLE);
    strlcpy(n.top, TRS("pausa!", "break time"), sizeof(n.top)); strcpy(n.ev, "break");
    n.big = g_tmLenS / 60; strcpy(n.unit, " min");
    strlcpy(n.msg, TRS("pomodoro fatto: alzati e respira", "pomodoro done: stand up and breathe"), sizeof(n.msg));
    snprintf(n.foot, sizeof(n.foot), TRS("oggi: %d %s", "today: %d %s"), today, pomo_word(today));
  } else {                                     // TN_FOCUS: fine pausa, riparte il focus
    n.col = C_ACCENT; n.hop = false;
    snprintf(n.legend, sizeof(n.legend), "pomodoro %d/%d", g_pomoN + 1, POMO_CYCLE);
    strlcpy(n.top, TRS("si riparte", "back to focus"), sizeof(n.top)); strcpy(n.ev, "focus");
    n.big = g_tmLenS / 60; strcpy(n.unit, " min");
    strlcpy(n.msg, TRS("di concentrazione: una cosa sola", "of focus: one thing only"), sizeof(n.msg));
    snprintf(n.foot, sizeof(n.foot), TRS("oggi: %d %s", "today: %d %s"), today, pomo_word(today));
  }
  notice_show(n);
}

static void tm_changed() { set_hdr_status(); home_tick(); }

// fine della fase (ogni giro del loop, in qualsiasi schermata): avviso e fase successiva
static void tm_tick() {
  if (!g_tmMode || (int32_t)(millis() - g_tmEndMs) < 0) return;
  if (g_tmMode == TM_TIMER) {
    g_tmNotice = TN_TIMER; g_tmMode = TM_OFF;
  } else if (g_tmMode == TM_FOCUS) {
    g_pomoN++; pomo_count(); pomo_record();
    g_tmNotice = TN_BREAK;
    tm_start(TM_BREAK, g_pomoN >= POMO_CYCLE ? POMO_LONG_S : POMO_SHORT_S);
  } else if (g_pomoN >= POMO_CYCLE) {          // fine della pausa lunga: ciclo finito
    g_tmNotice = TN_CYCLE; g_tmMode = TM_OFF; g_pomoN = 0;
  } else {
    g_tmNotice = TN_FOCUS;
    tm_start(TM_FOCUS, POMO_FOCUS_S);
  }
  g_ntPend = NT_TIMER; g_ntT0 = 0;
  tm_changed();
}

// menu del timer: si apre toccando l'ora o la riga col pomodoro nella home
static lv_obj_t *g_tmMenu = nullptr;
static void tm_menu_close() { if (g_tmMenu) { lv_obj_delete(g_tmMenu); g_tmMenu = nullptr; } }
// 0-2 pomodoro (impostazione), 3-6 timer 5/10/15/30 min, 10 ferma, 11 salta fase, 12 +5 min,
// 100+m timer di m minuti (dal PC). false = comando non valido in questo momento
static bool tm_action(int opt) {
  static const uint16_t MIN[4] = {5, 10, 15, 30};
  if (opt >= 0 && opt <= 2) { g_pomoPre = opt; g_pomoN = 0; tm_start(TM_FOCUS, POMO_FOCUS_S); }
  else if (opt >= 3 && opt <= 6) tm_start(TM_TIMER, MIN[opt - 3] * 60);
  else if (opt > 100 && opt <= 100 + 180) tm_start(TM_TIMER, (opt - 100) * 60);
  else if (opt == 10 && g_tmMode) { g_tmMode = TM_OFF; g_pomoN = 0; Serial.println("[TIMER] fermato"); }
  else if (opt == 11 && (g_tmMode == TM_FOCUS || g_tmMode == TM_BREAK)) g_tmEndMs = millis();   // tm_tick passa oltre
  else if (opt == 12 && g_tmMode == TM_TIMER) { g_tmEndMs += 5UL * 60UL * 1000UL; g_tmLenS += 5 * 60; }
  else return false;
  tm_changed();
  return true;
}
static void tm_menu_cb(lv_event_t *e) {
  int opt = (int)(intptr_t)lv_event_get_user_data(e);
  tm_menu_close();
  if (opt >= 0) tm_action(opt);                               // -1 = annulla
}
static void tm_menu_open(lv_event_t *e) {
  (void)e;
  if (g_tmMenu) return;
  lv_obj_t *s = plain_obj(lv_layer_top());
  g_tmMenu = s;
  lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_80, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, tm_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
  char lg[40];
  int today = pomo_today();
  if (today) snprintf(lg, sizeof(lg), TRS("timer " U_MIDDOT " oggi %d %s", "timer " U_MIDDOT " today %d %s"), today, pomo_word(today));
  else       strcpy(lg, TRS("timer e pomodoro", "timer and pomodoro"));
  lv_obj_t *b = tbox(s, 90, 42, 300, 236, lg, C_ACCENT);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);             // i tocchi sul riquadro non chiudono
  if (!g_tmMode) {
    tstatic(b, TRS("pomodoro " U_MIDDOT " focus/pausa in minuti", "pomodoro " U_MIDDOT " focus/break in minutes"), F12, C_MUTED, 20, 12);
    for (int i = 0; i < 3; i++) {
      char l[12]; snprintf(l, sizeof(l), "%d/%d", POMO_PRESETS[i].focus, POMO_PRESETS[i].brk);
      tbtn(b, 20 + i * 90, 30, 80, 40, l, F14, C_ACCENT, C_BORDER, tm_menu_cb, (void *)(intptr_t)i);
    }
    tstatic(b, "timer", F12, C_MUTED, 20, 82);
    const char *T[4] = {"5 min", "10 min", "15 min", "30 min"};
    for (int i = 0; i < 4; i++)
      tbtn(b, 20 + i * 67, 100, 58, 40, T[i], F14, C_TEXT, C_BORDER, tm_menu_cb, (void *)(intptr_t)(3 + i));
    tbtn(b, 20, 158, 258, 38, TRS("annulla", "cancel"), F14, C_MUTED, C_BORDER, tm_menu_cb, (void *)(intptr_t)-1);
  } else {
    char st[32]; tm_label(st, sizeof(st));
    tstatic(b, st, F22, tm_color(), 20, 30);
    if (g_tmMode != TM_TIMER) {
      char pr[48];
      snprintf(pr, sizeof(pr), TRS("pomodoro %d/%d, pausa lunga %d", "pomodoro %d/%d, long break %d"),
               POMO_PRESETS[g_pomoPre].focus, POMO_PRESETS[g_pomoPre].brk, POMO_PRESETS[g_pomoPre].lng);
      tstatic(b, pr, F12, C_MUTED, 20, 64);
    }
    tbtn(b, 20, 100, 124, 40, TRS("ferma", "stop"), F14, C_BAD, C_BORDER, tm_menu_cb, (void *)(intptr_t)10);
    if (g_tmMode == TM_TIMER)
      tbtn(b, 154, 100, 124, 40, "+5 min", F14, C_TEXT, C_BORDER, tm_menu_cb, (void *)(intptr_t)12);
    else
      tbtn(b, 154, 100, 124, 40, TRS("salta fase", "skip phase"), F14, C_TEXT, C_BORDER, tm_menu_cb, (void *)(intptr_t)11);
    tbtn(b, 20, 158, 258, 38, TRS("annulla", "cancel"), F14, C_MUTED, C_BORDER, tm_menu_cb, (void *)(intptr_t)-1);
  }
}

// Riempie tutti i valori arrivati dal fetch (senza ricostruire la schermata).
static void refresh_ui_values() {
  if (g_state != ST_MAIN || !g_ui.agPct5) return;
  char b[96];

  snprintf(b, sizeof(b), "%d", (int)(g_usage.h5 + 0.5f)); label_set(g_ui.agPct5, b);
  label_color(g_ui.agPct5, level_hex(g_usage.h5));
  blocks_set(g_ui.blk5, g_usage.h5, level_hex(g_usage.h5));
  snprintf(b, sizeof(b), "%d", (int)(g_usage.d7 + 0.5f)); label_set(g_ui.agPct7, b);
  label_color(g_ui.agPct7, level_hex(g_usage.d7));
  blocks_set(g_ui.blk7, g_usage.d7, level_hex(g_usage.d7));

  uint32_t wc; const char *w = status_word(g_usage.statusOverall, &wc);
  label_set(g_ui.agWord, w); label_color(g_ui.agWord, wc);

  if (g_ui.mSum) {
    int nOk = 0, nLim = 0;
    for (int i = 0; i < NMODELS; i++) {
      char st[16], nm[MODEL_ID_MAX], sp[32], lat[12]; uint32_t col;
      const ModelInfo &m = g_models[i];
      model_stat(i, st, sizeof(st), &col);
      if (!strcmp(st, "ok")) nOk++;
      if (!strcmp(st, "429")) nLim++;
      lv_obj_set_style_bg_color(g_ui.mDot[i], lv_color_hex(col), 0);
      short_id(m.id, nm, sizeof(nm));
      label_set(g_ui.mName[i], nm);
      label_color(g_ui.mName[i], m.pr.code == 0 ? C_MUTED : C_TEXT);
      spark_text(i, sp, sizeof(sp));
      label_set(g_ui.mSpark[i], sp);
      if (m.pr.code > 0) snprintf(lat, sizeof(lat), "%.1fs", m.pr.ms / 1000.0f); else strcpy(lat, "--");
      label_set(g_ui.mLat[i], lat);
      label_set(g_ui.mStat[i], st); label_color(g_ui.mStat[i], col);
    }
    snprintf(b, sizeof(b), TRS("%d disponibili " U_MIDDOT " %d limitati", "%d available " U_MIDDOT " %d limited"), nOk, nLim);
    label_set(g_ui.mSum, b);
    bool any = !(g_status.haikuUp && g_status.sonnetUp && g_status.opusUp && g_status.fableUp);
    label_set(g_ui.mInc, !g_status.ok ? TRS("status.claude.com: nessun dato", "status.claude.com: no data")
                         : any ? TRS("incidente attivo su status.claude.com", "active incident on status.claude.com")
                               : TRS("status.claude.com: nessun incidente", "status.claude.com: no incidents"));
    label_color(g_ui.mInc, !g_status.ok ? C_MUTED : any ? C_WARN : C_OK);
  }

  trend_redraw();
  heat_redraw();
  weeks_redraw();
  home_redraw();
  pc_redraw();
  dash_tick();
}

// Stato nell'intestazione (senza cambiare schermata)
static void set_hdr_status() {
  if (!g_hdrStatus) return;
  char buf[40]; uint32_t color;
  if (g_refreshing)        { strcpy(buf, TRS("aggiornamento...", "updating...")); color = C_ACCENT; }
  else if (g_tmMode)       { tm_label(buf, sizeof(buf)); color = tm_color(); }
  else if (g_updState == UPD_AVAILABLE) { snprintf(buf, sizeof(buf), TRS("nuova %s", "new %s"), g_updTag); color = C_ACCENT; }
  else if (g_userPause && g_pauseUntil) {
    char c[12]; fmt_hm(g_pauseUntil, c, sizeof(c));
    snprintf(buf, sizeof(buf), TRS("pausa fino %s", "paused until %s"), c); color = C_WARN;
  }
  else if (g_userPause)    { strcpy(buf, TRS("in pausa", "paused")); color = C_WARN; }
  else if (night_paused()) { strcpy(buf, TRS("notte " U_MIDDOT " in pausa", "night " U_MIDDOT " paused")); color = C_FAINT; }
  else if (!g_lastFetchOk) { strcpy(buf, TRS("non aggiornato", "update failed")); color = C_BAD; }
  else {
    uint32_t s = (millis() - g_lastOkMs) / 1000;
    if (s < 60) strcpy(buf, TRS("aggiornato ora", "updated just now"));
    else        snprintf(buf, sizeof(buf), TRS("aggiornato %um fa", "updated %um ago"), (unsigned)(s / 60));
    color = C_MUTED;
  }
  label_set(g_hdrStatus, buf);
  label_color(g_hdrStatus, color);
}
static void refresh_cb(lv_event_t *e) { (void)e; g_wantRefresh = true; }
static void pause_style() {
  if (!g_ui.pauseBtn) return;
  lv_obj_set_style_border_color(g_ui.pauseBtn, lv_color_hex(g_userPause ? C_WARN : C_BORDER), 0);
  for (int i = 0; i < 2; i++) {
    if (g_userPause) lv_obj_add_flag(g_ui.pauseBar[i], LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_clear_flag(g_ui.pauseBar[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (g_userPause) lv_obj_clear_flag(g_ui.pausePlay, LV_OBJ_FLAG_HIDDEN);
  else             lv_obj_add_flag(g_ui.pausePlay, LV_OBJ_FLAG_HIDDEN);
}
static void pause_set(bool on, uint32_t until) {
  g_userPause = on;
  g_pauseUntil = on ? until : 0;
  g_prefs.putBool("pause", g_userPause);
  g_prefs.putUInt("pauseu", g_pauseUntil);
  Serial.printf("[PAUSA] richieste %s (fino a %u)\n", on ? "in pausa" : "riprese", (unsigned)g_pauseUntil);
  if (!on) g_wantRefresh = millis() - g_lastPollMs > (uint32_t)g_pollSec * 1000;
  pause_style();
  set_hdr_status();
  home_redraw();
}
// tocco breve: pausa senza limite / riprendi
static void pause_cb(lv_event_t *e) { (void)e; pause_set(!g_userPause, 0); }

// tenuto premuto: menu con la durata della pausa
static lv_obj_t *g_pauseMenu = nullptr;
static void pause_menu_close() { if (g_pauseMenu) { lv_obj_delete(g_pauseMenu); g_pauseMenu = nullptr; } }
static void pause_menu_cb(lv_event_t *e) {
  int opt = (int)(intptr_t)lv_event_get_user_data(e);
  time_t now = time(nullptr);
  pause_menu_close();
  if (opt < 0) return;                                   // annulla
  uint32_t until = 0;
  if (now > 1000000000L) {
    if (opt == 0) until = (uint32_t)now + 30 * 60;
    else if (opt == 1) until = (uint32_t)now + 60 * 60;
    else if (opt == 2) {                                  // fino alle 7:00 del prossimo mattino
      struct tm tv; localtime_r(&now, &tv);
      if (tv.tm_hour >= 7) tv.tm_mday += 1;
      tv.tm_hour = 7; tv.tm_min = 0; tv.tm_sec = 0;
      until = (uint32_t)mktime(&tv);
    }
  }
  pause_set(true, until);
}
static void pause_long_cb(lv_event_t *e) {
  (void)e;
  if (g_pauseMenu) return;
  lv_obj_t *s = plain_obj(lv_layer_top());
  g_pauseMenu = s;
  lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_80, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, pause_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
  lv_obj_t *b = tbox(s, 90, 60, 300, 200, TRS("pausa richieste", "pause requests"), C_WARN);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);             // i tocchi sul riquadro non chiudono
  const char *L[4] = {"30 min", TRS("1 ora", "1 hour"), TRS("fino alle 7:00", "until 7:00"), TRS("senza limite", "no limit")};
  for (int i = 0; i < 4; i++)
    tbtn(b, 20 + (i % 2) * 134, 22 + (i / 2) * 56, 124, 42, L[i], F14, i == 3 ? C_WARN : C_TEXT, C_BORDER,
         pause_menu_cb, (void *)(intptr_t)(i == 3 ? 3 : i));
  tbtn(b, 20, 136, 258, 38, TRS("annulla", "cancel"), F14, C_MUTED, C_BORDER, pause_menu_cb, (void *)(intptr_t)-1);
}
// doppio tocco su "✻ ritmo-code" = demo dei momenti
// riapre l'avviso i della cronologia (senza suono sul PC)
static void al_show(int i) {
  if (i < 0 || i >= g_alN) return;
  if (g_al[i].moment) { g_pendPeak = g_al[i].peak; show_moment(g_al[i].win, g_al[i].thr); return; }
  g_ntReplay = true;
  notice_show(g_al[i].nt);
  g_ntReplay = false;
}
// una riga della cronologia: "19:42  claude ha finito · 12 min · claude code · ritmo-code"
static void al_line(int i, char *out, size_t sz, uint32_t *col) {
  const AlertRec &r = g_al[i];
  char hm[12] = "--:--";
  if (r.at > 1000000000UL) fmt_hm(r.at, hm, sizeof(hm));
  if (r.moment) {
    const char *wn = r.win ? TRS("7 giorni", "7-day") : TRS("5 ore", "5-hour");
    if (r.thr) snprintf(out, sz, TRS("%s  finestra %s al %d%%", "%s  %s window at %d%%"), hm, wn, r.thr);
    else       snprintf(out, sz, TRS("%s  finestra %s di nuovo libera", "%s  %s window available again"), hm, wn);
    *col = r.thr >= 100 ? C_BAD : r.thr >= 70 ? C_WARN : r.thr >= 50 ? C_ACCENT : C_OK;
    return;
  }
  int n = snprintf(out, sz, "%s  %s", hm, r.nt.top);
  if (r.nt.big >= 0) n += snprintf(out + n, sz - n, " " U_MIDDOT " %d%s", r.nt.big, r.nt.unit);
  const char *lg = r.nt.legend;                  // per Claude basta il progetto: "claude code · progetto"
  if (r.nt.kind == NT_CLAUDE) { const char *m = strstr(lg, U_MIDDOT); lg = m ? m + strlen(U_MIDDOT) + 1 : ""; }
  if (lg[0]) snprintf(out + n, sz - n, " " U_MIDDOT " %s", lg);
  *col = r.nt.col;
}

// ---- Pannello a tendina: comandi rapidi e ultimi avvisi ----
static lv_obj_t *g_shadePanel = nullptr, *g_shadeTm = nullptr;   // g_shadeTm: tempo del timer, aggiornato ogni secondo
static void shade_close() { if (g_shade) { lv_obj_delete(g_shade); g_shade = nullptr; g_shadePanel = nullptr; g_shadeTm = nullptr; } }
// pulsante del pannello: nome sopra, stato sotto (nel colore dello stato)
static lv_obj_t *shade_btn(lv_obj_t *p, int x, const char *name, const char *state, uint32_t stCol, lv_event_cb_t cb, void *ud) {
  lv_obj_t *b = tbtn(p, x, 12, 104, 44, "", F12, C_TEXT, C_BORDER, cb, ud);
  lv_obj_t *n = lv_obj_get_child(b, 0);
  lv_label_set_text(n, name);
  lv_obj_set_style_text_font(n, F14, 0);
  lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_t *st = mklabel(b, state, F12, stCol);
  lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -4);
  return st;
}
static void shade_timer_text(char *out, size_t sz) {
  if (!g_tmMode) { strlcpy(out, TRS("spento", "off"), sz); return; }
  uint32_t l = tm_left_s();
  snprintf(out, sz, "%02u:%02u", (unsigned)(l / 60), (unsigned)(l % 60));
}
static void shade_open(bool anim);
static void shade_cb(lv_event_t *e) {
  int a = (int)(intptr_t)lv_event_get_user_data(e);
  if (a < 0) { shade_close(); return; }                       // tocco fuori dal pannello
  if (a >= 100) { shade_close(); al_show(a - 100); return; }   // riapre un avviso
  switch (a) {
    case 1: pause_set(!g_userPause, 0); break;
    case 2: shade_close(); tm_menu_open(nullptr); return;
    case 3: g_briIdx = (g_briIdx + 1) % 3; g_prefs.putInt("bri", g_briIdx); apply_brightness(); break;
    case 4: g_pcSound = !g_pcSound; g_prefs.putBool("pcsnd", g_pcSound); break;
  }
  shade_open(false);                                           // ridisegna con i nuovi valori
}
static void shade_open(bool anim) {
  shade_close();
  lv_obj_t *s = plain_obj(lv_layer_top());
  g_shade = s;
  lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_60, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, shade_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
  lv_obj_t *p = plain_obj(s);
  g_shadePanel = p;
  const int H = 262;
  lv_obj_set_size(p, 480, H);
  lv_obj_set_pos(p, 0, 0);
  lv_obj_set_style_bg_color(p, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(p, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_color(p, lv_color_hex(C_BORDER), 0);
  lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);                   // i tocchi sul pannello non chiudono
  // comandi rapidi: nome sopra, stato sotto
  shade_btn(p, 20, TRS("richieste", "requests"), g_userPause ? TRS("in pausa", "paused") : TRS("attive", "active"),
            g_userPause ? C_WARN : C_OK, shade_cb, (void *)(intptr_t)1);
  char tt[12]; shade_timer_text(tt, sizeof(tt));
  g_shadeTm = shade_btn(p, 132, "timer", tt, g_tmMode ? tm_color() : C_MUTED, shade_cb, (void *)(intptr_t)2);
  shade_btn(p, 244, TRS("luce", "light"), bri_label(), C_ACCENT, shade_cb, (void *)(intptr_t)3);
  shade_btn(p, 356, TRS("suoni pc", "pc sound"), g_pcSound ? TRS("s\xC3\xAC", "on") : "no", g_pcSound ? C_OK : C_MUTED,
            shade_cb, (void *)(intptr_t)4);
  tstatic(p, TRS("ultimi avvisi", "recent alerts"), F12, C_MUTED, 20, 70);
  if (!g_alN) tstatic(p, TRS("nessun avviso recente", "no recent alerts"), F14, C_FAINT, 20, 96);
  for (int i = 0; i < g_alN; i++) {
    lv_obj_t *r = plain_obj(p);
    lv_obj_set_pos(r, 12, 90 + i * 30);
    lv_obj_set_size(r, 456, 28);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_SURFACE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, shade_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(100 + i));
    char t[120]; uint32_t col;
    al_line(i, t, sizeof(t), &col);
    rrect(r, 8, 10, 8, 8, 4, col);
    lv_obj_t *l = tstatic(r, t, F12, i ? C_MUTED : C_TEXT, 24, 6);
    lv_obj_set_width(l, 424);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
  rrect(p, 220, H - 10, 40, 4, 2, C_BORDER);                   // maniglia: si chiude trascinando su
  if (anim) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, -H, 0);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  }
}

// doppio tocco su "✻ ritmo-code": riapre l'ultimo avviso (soglia/reset o Claude/timer, il piu' recente)
static void logo_cb(lv_event_t *e) {
  (void)e;
  static uint32_t lastClick = 0;
  uint32_t now = millis();
  if (now - lastClick >= 450) { lastClick = now; return; }
  lastClick = 0;
  if (g_alN) { al_show(0); return; }
  g_ntReplay = true;
  {
    NoticeText n = {};
    n.kind = NT_TIMER; n.col = C_MUTED; n.maxMs = 5000; n.big = -1;
    strcpy(n.legend, TRS("avvisi", "alerts"));
    strlcpy(n.top, TRS("nessun avviso recente", "no recent alerts"), sizeof(n.top));
    strlcpy(n.word, TRS("tutto tranquillo", "all quiet"), sizeof(n.word));
    strlcpy(n.msg, TRS("qui ritrovi l'ultimo avviso con un doppio tocco", "double-tap here to see the last alert again"), sizeof(n.msg));
    notice_show(n);
  }
  g_ntReplay = false;
}

static void ui_main() {
  lv_obj_t *scr = lv_screen_active();
  g_setGroup = 0;                            // impostazioni: si riparte dalla pagina principale
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

  start_data_web();

  // intestazione stile prompt: ✻ ritmo-code /usage [@account]
  lv_obj_t *id = trow(scr, 13, 11);
  lv_obj_add_flag(id, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(id, 8);
  lv_obj_add_event_cb(id, logo_cb, LV_EVENT_CLICKED, NULL);
  g_ui.hdrSpark = mklabel(id, U_SPARK " ", F14, C_ACCENT);
  mklabel(id, "ritmo-code ", F14, C_TEXT);
  g_ui.hdrPath = mklabel(id, "/usage", F14, C_FAINT);
  if (accountCount(g_accts) > 1) {
    char ab[ACCT_LBL_MAX + 3];
    snprintf(ab, sizeof(ab), " @%.10s", g_accts.label[g_accts.active]);
    g_ui.hdrAcct = mklabel(id, ab, F14, C_ACCENT);
  }

  g_hdrStatus = tlabel(scr, F12, C_MUTED, 157, 13);
  lv_obj_set_width(g_hdrStatus, 131);
  lv_obj_set_style_text_align(g_hdrStatus, LV_TEXT_ALIGN_RIGHT, 0);

  // pausa/riprendi: due barre (attivo) o triangolo (in pausa), disegnati perche' il font non ha i glifi
  // griglia aurea: pulsanti 52 x 32 (1,625), 5 sopra e sotto, passo 57, margine destro 13
  lv_obj_t *pb = tbtn(scr, 301, 5, 52, 32, "", F12, C_TEXT, C_BORDER, nullptr, NULL);
  lv_obj_add_event_cb(pb, pause_cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(pb, pause_long_cb, LV_EVENT_LONG_PRESSED, NULL);
  g_ui.pauseBtn = pb;
  g_ui.pauseBar[0] = rrect(pb, 19, 8, 4, 14, 1, C_TEXT);
  g_ui.pauseBar[1] = rrect(pb, 27, 8, 4, 14, 1, C_TEXT);
  static lv_point_precise_t PLAY[4] = {{0, 0}, {0, 14}, {12, 7}, {0, 0}};
  g_ui.pausePlay = lv_line_create(pb);
  lv_line_set_points(g_ui.pausePlay, PLAY, 4);
  lv_obj_set_pos(g_ui.pausePlay, 20, 8);
  lv_obj_set_style_line_width(g_ui.pausePlay, 2, 0);
  lv_obj_set_style_line_rounded(g_ui.pausePlay, true, 0);
  lv_obj_set_style_line_color(g_ui.pausePlay, lv_color_hex(C_WARN), 0);
  pause_style();
  tbtn(scr, 358, 5, 52, 32, U_REFRESH, F22, C_ACCENT, C_BORDER, refresh_cb, NULL);
  tbtn(scr, 415, 5, 52, 32, U_MENU, F22, C_TEXT, C_BORDER, nav_cb, (void *)(intptr_t)ST_SETTINGS);

  // filo sotto l'intestazione: il tratto argilla e' il conto alla rovescia del refresh
  // solo sotto nome e stato, a fianco dei pulsanti: a meta' tra il titolo (fondo ~26) e i contenuti (~51)
  hline(scr, 13, 38, 275, C_BORDER);
  g_ui.refBar = hline(scr, 13, 38, 275, C_ACCENT);

  g_ui.tv = lv_tileview_create(scr);
  lv_obj_set_pos(g_ui.tv, 0, 43);                 // tile: y schermo = y tile + 43
  lv_obj_set_size(g_ui.tv, 480, 251);
  lv_obj_set_style_bg_opa(g_ui.tv, 0, 0);
  lv_obj_set_style_border_width(g_ui.tv, 0, 0);
  lv_obj_set_scrollbar_mode(g_ui.tv, LV_SCROLLBAR_MODE_OFF);
  for (int i = 0; i < NTILES; i++) {
    g_ui.tile[i] = lv_tileview_add_tile(g_ui.tv, i, 0, LV_DIR_HOR);
    tile_setup(g_ui.tile[i]);
  }
  build_tile_home(g_ui.tile[0]);
  build_tile_agora(g_ui.tile[1]);
  build_tile_models(g_ui.tile[2]);
  build_tile_trend(g_ui.tile[3]);
  build_tile_heat(g_ui.tile[4]);
  build_tile_weeks(g_ui.tile[5]);
  build_tile_pc(g_ui.tile[6]);
  lv_obj_add_event_cb(g_ui.tv, on_tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

  // schede in basso, toccabili: [ora] modelli 5h ritmo
  lv_obj_t *tabs = plain_obj(scr);
  lv_obj_set_size(tabs, LV_SIZE_CONTENT, 21);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (int i = 0; i < NTILES; i++) {
    lv_obj_t *l = mklabel(tabs, "", F12, C_FAINT);
    lv_obj_set_style_pad_hor(l, 8, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    g_ui.tab[i] = l;
  }
  lv_obj_align(tabs, LV_ALIGN_BOTTOM_MID, 0, -5);   // schede 21, 5 sopra e 5 sotto

  if (g_curTile > 0 && g_curTile < NTILES)   // parte dalla home, salvo ridisegni della stessa schermata
    lv_tileview_set_tile_by_index(g_ui.tv, g_curTile, 0, LV_ANIM_OFF);

  refresh_ui_values();
  on_tile_changed(NULL);
}

// ============================================================
// Schermata: impostazioni (lista scorrevole; righe >=44px per il tocco)
// ============================================================
static bool g_wipeArmed = false;
static lv_obj_t *g_briLbl = nullptr, *g_wipeLbl = nullptr, *g_pollLbl = nullptr,
                *g_tzLbl = nullptr, *g_slideLbl = nullptr;
static const int POLL_OPTS[] = {30, 60, 120, 300, 600, 900, 1800};
#define NPOLL ((int)(sizeof(POLL_OPTS) / sizeof(POLL_OPTS[0])))
static const int TZ_OPTS[] = {TZ_ROME, 0, 1, 2, 3, 4, 5, -1, -2, -3, -4, -5, -6, -7, -8};
#define NTZ ((int)(sizeof(TZ_OPTS) / sizeof(TZ_OPTS[0])))
static void tz_label(char *out, size_t sz) {
  if (g_tzOffset == TZ_ROME) snprintf(out, sz, "%s", TRS("roma (auto)", "rome (auto)"));
  else                       snprintf(out, sz, "gmt%+d", g_tzOffset);
}
static void poll_label(char *out, size_t sz) {
  if (g_pollSec < 60) snprintf(out, sz, "%ds", g_pollSec);
  else                snprintf(out, sz, "%dmin", g_pollSec / 60);
}
static const char *bri_label() {
  static const char *n[3] = {"bassa", "media", "alta"};
  static const char *e[3] = {"low", "medium", "high"};
  return g_lang ? e[g_briIdx] : n[g_briIdx];
}

static int g_acctDelArmed = -1;
static int g_netDelArmed = -1;             // rete wifi da dimenticare (secondo tocco)
// lista impostazioni: quando la pagina si ridisegna per un cambio, resta dov'era
static lv_obj_t *g_setList = nullptr;
static int32_t g_setScroll = 0;

// schermata di installazione: versione, percentuale e barra; se fallisce si chiude con un tocco
struct UpdUI { lv_obj_t *scrim, *pct, *msg; Blocks blk; int lastPct, shown; };
static UpdUI g_upd = {};
static void upd_close() { if (g_upd.scrim) { lv_obj_delete(g_upd.scrim); memset(&g_upd, 0, sizeof(g_upd)); } }
static void upd_close_cb(lv_event_t *e) { (void)e; if (g_updState == UPD_FAILED) upd_close(); }
static void upd_overlay() {
  upd_close();
  lv_obj_t *s = plain_obj(lv_layer_top());
  g_upd.scrim = s;
  lv_obj_set_size(s, 480, 320);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s, upd_close_cb, LV_EVENT_CLICKED, NULL);
  tbox(s, 10, 14, 460, 296, TRS("aggiornamento firmware", "firmware update"), C_ACCENT);
  char b[48];
  snprintf(b, sizeof(b), "v" FW_VERSION " " U_RIGHT " %s", g_updTag);
  tstatic(s, b, F22, C_TEXT, 34, 50);
  lv_obj_t *r = trow(s, 32, 96);
  g_upd.pct = mklabel(r, "0", F54, C_ACCENT);
  mklabel(r, "%", F22, C_MUTED);
  g_upd.blk = blocks_create(s, 34, 172, NBLK, F14);
  g_upd.msg = tstatic(s, TRS("download da GitHub... non scollegare il dispositivo", "downloading from GitHub... do not unplug"), F14, C_MUTED, 34, 214);
  lv_obj_set_width(g_upd.msg, 412);
  lv_label_set_long_mode(g_upd.msg, LV_LABEL_LONG_WRAP);
  g_upd.lastPct = -1;
  g_upd.shown = -1;
}
static void upd_install_start() {
  g_updPct = 0; g_updErr = "";
  g_updState = UPD_INSTALLING;
  g_updInstallReq = true;                           // il loop sveglia il task di rete appena e' libero
  upd_overlay();
}
static void upd_tick() {
  if (!g_upd.scrim) return;
  int p = g_updPct;
  if (p != g_upd.lastPct && g_updState == UPD_INSTALLING) {
    g_upd.lastPct = p;
    char b[8]; snprintf(b, sizeof(b), "%d", p);
    label_set(g_upd.pct, b);
    blocks_set(g_upd.blk, p, C_ACCENT);
  }
  if (g_upd.shown == g_updState) return;
  g_upd.shown = g_updState;
  if (g_updState == UPD_REBOOT) {
    label_set(g_upd.pct, "100"); blocks_set(g_upd.blk, 100, C_OK);
    label_set(g_upd.msg, TRS("installato: riavvio...", "installed: restarting..."));
    label_color(g_upd.msg, C_OK);
  } else if (g_updState == UPD_FAILED) {
    char m[120];
    snprintf(m, sizeof(m), TRS("non riuscito: %s. Il firmware attuale resta. Tocca per chiudere.",
                               "failed: %s. The current firmware stays. Tap to close."), g_updErr.c_str());
    label_set(g_upd.msg, m);
    label_color(g_upd.msg, C_BAD);
  }
}
static void settings_action_cb(lv_event_t *e) {
  int act = (int)(intptr_t)lv_event_get_user_data(e);
  switch (act) {
    case 0: request_state(ST_LOADING); break;          // aggiorna
    case 1: g_netDelArmed = -1; request_state(ST_NETWORKS); break;   // reti salvate
    case 2:                                            // cambia token
      g_tokenTargetSlot = g_accts.active;
      g_pendingLabel[0] = 0;
      request_state(ST_TOKEN);
      break;
    case 3:                                            // luminosita'
      g_briIdx = (g_briIdx + 1) % 3; g_prefs.putInt("bri", g_briIdx); apply_brightness();
      if (g_briLbl) lv_label_set_text(g_briLbl, bri_label());
      break;
    case 4:                                            // cancella tutto (2 tocchi)
      if (!g_wipeArmed) {
        g_wipeArmed = true;
        if (g_wipeLbl) lv_label_set_text(g_wipeLbl, TRS("tocca ancora", "tap again"));
      } else {                                         // secondo tocco: conferma con il PIN
        g_wipeArmed = false;
        g_pinForWipe = true;
        memset(g_pinEntry, 0, sizeof(g_pinEntry));
        request_state(ST_PIN);
      }
      break;
    case 5: request_state(ST_MAIN); break;             // indietro
    case 6: {                                          // intervallo di aggiornamento
      int idx = 0;
      for (int i = 0; i < NPOLL; i++) if (POLL_OPTS[i] == g_pollSec) idx = i;
      g_pollSec = POLL_OPTS[(idx + 1) % NPOLL];
      g_prefs.putInt("poll", g_pollSec);
      if (g_pollLbl) { char m[16]; poll_label(m, sizeof(m)); lv_label_set_text(g_pollLbl, m); }
      break;
    }
    case 7: {                                          // fuso orario (GMT)
      int idx = 0;
      for (int i = 0; i < NTZ; i++) if (TZ_OPTS[i] == g_tzOffset) idx = i;
      g_tzOffset = TZ_OPTS[(idx + 1) % NTZ];
      g_prefs.putInt("tz", g_tzOffset);
      apply_tz();
      if (g_tzLbl) {
        char m[48];
        tz_label(m, sizeof(m));
        lv_label_set_text(g_tzLbl, m);
      }
      break;
    }
    case 8: {                                          // slideshow: off -> 5 -> 10 -> 15 -> 30 -> off
      static const int SL[5] = {0, 5, 10, 15, 30};
      int idx = 0;
      for (int i = 0; i < 5; i++) if (SL[i] == g_slideSec) idx = i;
      g_slideSec = SL[(idx + 1) % 5];
      g_prefs.putInt("slide", g_slideSec);
      if (g_slideLbl) {
        char m[16];
        if (g_slideSec) snprintf(m, sizeof(m), "%ds", g_slideSec);
        else            snprintf(m, sizeof(m), "%s", TRS("spento", "off"));
        lv_label_set_text(g_slideLbl, m);
      }
      break;
    }
    case 9:                                            // lingua / language
      g_lang ^= 1;
      g_prefs.putInt("lang", g_lang);
      request_state(ST_SETTINGS);                      // ridisegna tutto nella nuova lingua
      break;
    case 10: request_state(ST_ABOUT); break;           // info / about
    case 11: g_acctDelArmed = -1; request_state(ST_ACCOUNTS); break;
    case 12: request_state(ST_MODELS); break;          // modelli sondati
    case 14: request_state(ST_OTA); break;             // aggiorna firmware via wifi
    case 16:                                           // notte: off -> 22-07 -> 23-07 -> 00-07
      g_nightIdx = (g_nightIdx + 1) % 4;
      g_prefs.putInt("night", g_nightIdx);
      request_state(ST_SETTINGS);
      break;
    case 21:                                           // di notte: orologio tenue / schermo spento
      g_nightClock = !g_nightClock;
      g_prefs.putBool("nightclk", g_nightClock);
      request_state(ST_SETTINGS);
      break;
    case 22:                                           // luminosita' orologio notturno
      g_nightBri ^= 1;
      g_prefs.putInt("nightbri", g_nightBri);
      request_state(ST_SETTINGS);
      break;
    case 17:                                           // aggiornamenti di notte
      g_nightPause = !g_nightPause;
      g_prefs.putBool("nightp", g_nightPause);
      request_state(ST_SETTINGS);
      break;
    case 18:                                           // attenua dopo: off -> 1 -> 5 -> 10 min
      g_dimIdx = (g_dimIdx + 1) % 4;
      g_prefs.putInt("dim", g_dimIdx);
      request_state(ST_SETTINGS);
      break;
    case 20:                                           // intervallo lettura pc: 1s -> 3s -> 5s -> 1min
      g_pcIntIdx = (g_pcIntIdx + 1) % 4;
      g_prefs.putInt("pcint", g_pcIntIdx);
      request_state(ST_SETTINGS);
      break;
    case 19:                                           // torna alla home: off -> 2 -> 5 -> 10 min
      g_clockIdx = (g_clockIdx + 1) % 4;
      g_prefs.putInt("clock", g_clockIdx);
      request_state(ST_SETTINGS);
      break;
    case 15:                                           // avviso di reset
      g_resetAlert = !g_resetAlert;
      g_prefs.putBool("rstal", g_resetAlert);
      request_state(ST_SETTINGS);
      break;
    case 25:                                           // chiudi avviso claude: 5 s -> 10 s -> 30 s -> mai
      g_ccCloseIdx = (g_ccCloseIdx + 1) % 4;
      g_prefs.putInt("ccclose", g_ccCloseIdx);
      request_state(ST_SETTINGS);
      break;
    case 26:                                           // aggiornamenti: cerca / installa
      if (g_updState == UPD_AVAILABLE) upd_install_start();
      else if (g_updState != UPD_CHECKING && g_updState != UPD_INSTALLING) {
        g_updState = UPD_CHECKING; g_updCheckedMs = g_updCheckStartMs = millis(); g_updCheckReq = true;
        request_state(ST_SETTINGS);
      }
      break;
    case 27:                                           // claude durante il focus: subito / alla pausa
      g_ccFocusDefer = !g_ccFocusDefer;
      g_prefs.putBool("ccfocus", g_ccFocusDefer);
      request_state(ST_SETTINGS);
      break;
    case 24:                                           // suoni e notifiche sul pc
      g_pcSound = !g_pcSound;
      g_prefs.putBool("pcsnd", g_pcSound);
      request_state(ST_SETTINGS);
      break;
    case 23:                                           // avvisi claude code: sempre -> oltre 1 min -> oltre 5 min -> spento
      g_ccAlert = (g_ccAlert + 1) % 4;
      g_prefs.putInt("ccal", g_ccAlert);
      request_state(ST_SETTINGS);
      break;
    case 13:                                           // contatore FPS
      g_perfOn = !g_perfOn;
      g_prefs.putBool("perf", g_perfOn);
      perf_apply();
      request_state(ST_SETTINGS);
      break;
  }
}
// Impostazioni a gruppi: una pagina principale con le voci, e una pagina per gruppo
enum { SG_MAIN = 0, SG_CLAUDE, SG_ALERTS, SG_SCREEN, SG_NET, SG_SYSTEM };
static void set_group_cb(lv_event_t *e) {
  g_setGroup = (int)(intptr_t)lv_event_get_user_data(e);
  g_setScroll = 0;
  request_state(ST_SETTINGS);
}
static void ui_settings() {
  lv_obj_t *scr = lv_screen_active();
  g_wipeArmed = false;
  start_data_web();
  static int lastGroup = -1;                       // cambiando gruppo si riparte dall'inizio della lista
  if (lastGroup != g_setGroup) g_setScroll = 0;
  lastGroup = g_setGroup;
  static const char *GT_IT[6] = {"impostazioni", "claude", "avvisi", "schermo", "rete e pc", "sistema"};
  static const char *GT_EN[6] = {"settings", "claude", "alerts", "screen", "network and pc", "system"};
  char title[40];
  if (g_setGroup == SG_MAIN) strcpy(title, g_lang ? GT_EN[0] : GT_IT[0]);
  else snprintf(title, sizeof(title), "%s / %s", g_lang ? GT_EN[0] : GT_IT[0], g_lang ? GT_EN[g_setGroup] : GT_IT[g_setGroup]);
  if (g_setGroup == SG_MAIN) thead(scr, title, g_usage.ok ? ST_MAIN : ST_SETTINGS);
  else {
    thead(scr, title, -1);
    tbtn(scr, 378, 5, 89, 32, TRS(U_LEFT " indietro", U_LEFT " back"), F14, C_MUTED, C_BORDER, set_group_cb, (void *)(intptr_t)SG_MAIN);
  }
  lv_obj_t *lst = tlist(scr, 13, 47, 454, 273);
  g_setList = lst;
  auto sub = [&](int g, const char *val) {         // voce che apre un gruppo
    char v[48]; snprintf(v, sizeof(v), "%s " U_RIGHT, val);
    kv_row(lst, g_lang ? GT_EN[g] : GT_IT[g], v, C_TEXT, C_ACCENT, set_group_cb, (void *)(intptr_t)g);
  };
  static const char *CC_LBL_IT[4] = {"spento", "sempre", "oltre 1 min", "oltre 5 min"};
  static const char *CC_LBL_EN[4] = {"off", "always", "over 1 min", "over 5 min"};
  char acct[40];
  snprintf(acct, sizeof(acct), "%s (%d/%d)", g_accts.label[g_accts.active], accountCount(g_accts), ACCT_MAX);
  String ssidStr = g_wifi.isConnected() ? g_wifi.getSSID() : String("--");
  char ssidBuf[33];
  strlcpy(ssidBuf, ssidStr.c_str(), sizeof(ssidBuf));

  switch (g_setGroup) {
    case SG_MAIN: {
      kv_row(lst, TRS("aggiorna ora", "refresh now"), U_ENTER, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)0);
      sub(SG_CLAUDE, g_accts.label[g_accts.active]);
      char al[32];
      snprintf(al, sizeof(al), "claude %s", g_lang ? CC_LBL_EN[g_ccAlert] : CC_LBL_IT[g_ccAlert]);
      sub(SG_ALERTS, al);
      char sc[32];
      snprintf(sc, sizeof(sc), TRS("luce %s", "light %s"), bri_label());
      sub(SG_SCREEN, sc);
      sub(SG_NET, ssidBuf);
      if (g_updState == UPD_AVAILABLE) { char nv[32]; snprintf(nv, sizeof(nv), TRS("nuova %s", "new %s"), g_updTag); sub(SG_SYSTEM, nv); }
      else sub(SG_SYSTEM, "v" FW_VERSION);
      break;
    }
    case SG_CLAUDE: {
      char poll[16]; poll_label(poll, sizeof(poll));
      kv_row(lst, TRS("intervallo", "interval"), poll, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)6, &g_pollLbl);
      kv_row(lst, "account", acct, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)11);
      kv_row(lst, TRS("modelli", "models"), TRS("modifica id", "edit ids"), C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)12);
      kv_row(lst, "token", TRS("cambia", "change"), C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)2);
      break;
    }
    case SG_ALERTS: {
      kv_row(lst, TRS("avvisi claude code", "claude code alerts"), g_lang ? CC_LBL_EN[g_ccAlert] : CC_LBL_IT[g_ccAlert],
             C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)23);
      char ccc[16];
      if (CC_CLOSE_S[g_ccCloseIdx]) snprintf(ccc, sizeof(ccc), TRS("dopo %d s", "after %d s"), CC_CLOSE_S[g_ccCloseIdx]);
      else                          strcpy(ccc, TRS("mai", "never"));
      kv_row(lst, TRS("chiudi avviso claude", "close claude alert"), ccc, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)25);
      kv_row(lst, TRS("claude durante il focus", "claude during focus"), g_ccFocusDefer ? TRS("alla pausa", "at the break") : TRS("subito", "right away"),
             C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)27);
      kv_row(lst, TRS("suoni sul pc", "sounds on pc"), g_pcSound ? TRS("acceso", "on") : TRS("spento", "off"),
             C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)24);
      kv_row(lst, TRS("avviso reset", "reset alert"), g_resetAlert ? TRS("sopra 80%", "above 80%") : TRS("spento", "off"),
             C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)15);
      break;
    }
    case SG_SCREEN: {
      kv_row(lst, TRS("luminosit\xC3\xA0", "brightness"), bri_label(), C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)3, &g_briLbl);
      static const char *NIGHT_LBL[4] = {"", "22:00-07:00", "23:00-07:00", "00:00-07:00"};
      kv_row(lst, TRS("notte", "night"), g_nightIdx ? NIGHT_LBL[g_nightIdx] : TRS("spento", "off"),
             C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)16);
      if (g_nightIdx) {
        kv_row(lst, TRS("schermo di notte", "screen at night"), g_nightClock ? TRS("orologio", "clock") : TRS("spento", "off"),
               C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)21);
        if (g_nightClock)
          kv_row(lst, TRS("luminosit\xC3\xA0 notte", "night brightness"), g_nightBri ? TRS("molto tenue", "very dim") : TRS("tenue", "dim"),
                 C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)22);
        kv_row(lst, TRS("aggiornamenti di notte", "updates at night"), g_nightPause ? TRS("in pausa", "paused") : TRS("attivi", "active"),
               C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)17);
      }
      char dim[16];
      if (g_dimIdx) snprintf(dim, sizeof(dim), "%d min", DIM_MIN[g_dimIdx]); else strcpy(dim, TRS("spento", "off"));
      kv_row(lst, TRS("attenua dopo", "dim after"), dim, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)18);
      char clk[16];
      if (g_clockIdx) snprintf(clk, sizeof(clk), "%d min", CLOCK_MIN[g_clockIdx]); else strcpy(clk, TRS("spento", "off"));
      kv_row(lst, TRS("home dopo", "home after"), clk, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)19);
      char slide[16];
      if (g_slideSec) snprintf(slide, sizeof(slide), "%ds", g_slideSec); else strcpy(slide, TRS("spento", "off"));
      kv_row(lst, "slideshow", slide, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)8, &g_slideLbl);
      break;
    }
    case SG_NET: {
      kv_row(lst, "wifi", ssidBuf, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)1);
      // dove aprire il pannello web dal browser (stessa rete Wi-Fi)
      char ipBuf[40];
      if (g_wifi.isConnected()) snprintf(ipBuf, sizeof(ipBuf), "%s %ddBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      else                      strcpy(ipBuf, TRS("non connesso", "not connected"));
      kv_row(lst, TRS("rete locale", "local network"), ipBuf, C_TEXT, C_ACCENT, nullptr, nullptr);
      kv_row(lst, TRS("nome in rete", "network name"), "ritmo-code.local", C_MUTED, C_MUTED, nullptr, nullptr);
      char pci[12];
      if (PC_INT_S[g_pcIntIdx] >= 60) snprintf(pci, sizeof(pci), "%dmin", PC_INT_S[g_pcIntIdx] / 60); else snprintf(pci, sizeof(pci), "%ds", PC_INT_S[g_pcIntIdx]);
      kv_row(lst, TRS("intervallo pc", "pc interval"), pci, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)20);
      break;
    }
    case SG_SYSTEM: {
      kv_row(lst, TRS("lingua", "language"), TRS("italiano", "english"), C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)9);
      char tz[32]; tz_label(tz, sizeof(tz));
      kv_row(lst, TRS("fuso orario", "timezone"), tz, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)7, &g_tzLbl);
      {
        char uv[40];
        switch (g_updState) {
          case UPD_CHECKING:  strcpy(uv, TRS("controllo...", "checking...")); break;
          case UPD_AVAILABLE: snprintf(uv, sizeof(uv), TRS("installa %s " U_ENTER, "install %s " U_ENTER), g_updTag); break;
          case UPD_UPTODATE:  strcpy(uv, TRS("aggiornato", "up to date")); break;
          case UPD_ERROR:     strcpy(uv, TRS("errore, riprova", "error, retry")); break;
          case UPD_FAILED:    strcpy(uv, TRS("non riuscito, riprova", "failed, retry")); break;
          default:            strcpy(uv, TRS("cerca", "check")); break;
        }
        kv_row(lst, TRS("aggiornamenti", "updates"), uv, C_TEXT, g_updState == UPD_UPTODATE ? C_OK : C_ACCENT,
               settings_action_cb, (void *)(intptr_t)26);
      }
      kv_row(lst, TRS("aggiorna da browser", "update from browser"), "wifi", C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)14);
      kv_row(lst, "info", "v" FW_VERSION, C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)10);
      kv_row(lst, TRS("contatore fps", "fps counter"), g_perfOn ? TRS("acceso", "on") : TRS("spento", "off"),
             C_TEXT, C_ACCENT, settings_action_cb, (void *)(intptr_t)13);
      kv_row(lst, TRS("cancella tutto", "erase everything"), "", C_BAD, C_BAD, settings_action_cb, (void *)(intptr_t)4, &g_wipeLbl);
      break;
    }
  }
  if (g_setScroll > 0) { lv_obj_update_layout(lst); lv_obj_scroll_to_y(lst, g_setScroll, LV_ANIM_OFF); }
}

static void acct_switch_cb(lv_event_t *e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  g_acctDelArmed = -1;
  if (slot == g_accts.active) return;
  if (!switch_account(slot)) request_state(ST_ACCOUNTS);
}
// ---- Reti Wi-Fi salvate: preferita (provata per prima all'avvio) e dimentica ----

static void net_pref_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  g_netDelArmed = -1;
  g_wifi.promote(i);
  Serial.printf("[WIFI] rete preferita: %s\n", g_wifi.getSavedSSID(0));
  request_state(ST_NETWORKS);
}
static void net_del_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_netDelArmed != i) { g_netDelArmed = i; request_state(ST_NETWORKS); return; }
  g_netDelArmed = -1;
  bool current = g_wifi.isConnected() && g_wifi.getSSID() == g_wifi.getSavedSSID(i);
  Serial.printf("[WIFI] dimentico %s%s\n", g_wifi.getSavedSSID(i), current ? " (connessa)" : "");
  g_wifi.forgetNetwork(i);
  if (current) {
    // era quella in uso: si stacca e riprova le altre salvate (o torna alla scelta della rete)
    WiFi.disconnect(false, true);        // true = cancella anche le credenziali ricordate dal driver
    if (g_wifi.getSavedCount() == 0) { g_onboarding = false; request_state(ST_WIFI); }
    else request_state(ST_LOADING);
    return;
  }
  request_state(ST_NETWORKS);
}
static void net_add_cb(lv_event_t *e) { (void)e; g_onboarding = false; request_state(ST_WIFI); }
static void ui_networks() {
  lv_obj_t *scr = lv_screen_active();
  start_data_web();
  thead(scr, TRS("reti wifi", "wifi networks"), ST_SETTINGS);
  lv_obj_t *lst = tlist(scr, 13, 50, 454, 236);
  lv_obj_set_style_pad_row(lst, 6, 0);
  String cur = g_wifi.isConnected() ? g_wifi.getSSID() : String();
  int n = g_wifi.getSavedCount();
  for (int i = 0; i < n; i++) {
    const char *ssid = g_wifi.getSavedSSID(i);
    bool conn = cur.length() && cur == ssid;
    lv_obj_t *row = plain_obj(lst);
    lv_obj_set_size(row, 448, 40);
    lv_obj_t *b = tbtn(row, 0, 0, 340, 40, "", F14, C_TEXT, conn ? C_ACCENT : C_BORDER, nullptr, NULL);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    char txt[48];
    snprintf(txt, sizeof(txt), "%s%s", conn ? "> " : "  ", ssid);
    lv_label_set_text(l, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 220);
    lv_obj_set_style_text_color(l, lv_color_hex(conn ? C_ACCENT : C_TEXT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *tag = mklabel(b, conn ? TRS("connessa", "connected") : (i == 0 ? TRS("preferita", "preferred") : ""), F12, C_FAINT);
    lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -10, 0);
    if (i > 0) tbtn(row, 346, 0, 48, 40, "1", F14, C_MUTED, C_BORDER, net_pref_cb, (void *)(intptr_t)i);
    bool armed = (g_netDelArmed == i);
    tbtn(row, 400, 0, 48, 40, armed ? "!" : U_CROSS, F14, armed ? C_BAD : C_MUTED, armed ? C_BAD : C_BORDER,
         net_del_cb, (void *)(intptr_t)i);
  }
  lv_obj_t *add = tbtn(lst, 0, 0, 448, 40, "", F14, C_ACCENT, C_FAINT, net_add_cb, NULL);
  lv_obj_t *al = lv_obj_get_child(add, 0);
  lv_label_set_text(al, TRS("+ cerca e aggiungi rete", "+ scan and add network"));
  lv_obj_align(al, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_t *hint = mklabel(scr, g_netDelArmed >= 0 ? TRS("tocca ancora " U_CROSS " per dimenticare la rete", "tap " U_CROSS " again to forget the network")
                                                   : TRS("\"1\" = provata per prima all'avvio " U_MIDDOT " " U_CROSS " = dimentica (2 tocchi)",
                                                         "\"1\" = tried first at boot " U_MIDDOT " " U_CROSS " = forget (2 taps)"), F12, C_FAINT);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -10);
}

static void acct_del_cb(lv_event_t *e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  if (accountCount(g_accts) <= 1) return;
  if (g_acctDelArmed != slot) {
    g_acctDelArmed = slot;
    request_state(ST_ACCOUNTS);
    return;
  }
  g_acctDelArmed = -1;
  bool wasActive = (slot == g_accts.active);
  accountRemove(g_prefs, g_accts, slot);
  char pth[16]; snprintf(pth, sizeof(pth), "/hist%d.bin", slot);
  LittleFS.remove(pth);
  snprintf(pth, sizeof(pth), "/weeks%d.bin", slot);
  LittleFS.remove(pth);
  if (wasActive) {
    EncryptedBlob b; char tok[200];
    if (accountLoadBlob(g_prefs, g_accts.active, b) &&
        decryptToken(b, g_sessionPin, tok, sizeof(tok))) {
      g_dataEpoch++;
      g_blob = b;
      strlcpy(g_token, tok, sizeof(g_token));
      memset(tok, 0, sizeof(tok));
      reset_history_ram();
      load_history();
      memset(&g_usage, 0, sizeof(g_usage));
      request_state(ST_LOADING);
      return;
    }
  }
  request_state(ST_ACCOUNTS);
}
static void acct_add_cb(lv_event_t *e) {
  (void)e;
  int slot = accountFirstFree(g_accts);
  if (slot < 0) return;
  g_tokenTargetSlot = slot;
  g_pendingLabel[0] = 0;
  request_state(ST_TOKEN);
}
static int g_renameSlot = -1;
static lv_obj_t *g_nameTa = nullptr;

static void acct_edit_cb(lv_event_t *e) {
  g_renameSlot = (int)(intptr_t)lv_event_get_user_data(e);
  g_acctDelArmed = -1;
  request_state(ST_ACCT_NAME);
}
static void acct_name_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    accountSetLabel(g_prefs, g_accts, g_renameSlot, lv_textarea_get_text(g_nameTa));
    request_state(ST_ACCOUNTS);
  } else if (code == LV_EVENT_CANCEL) {
    request_state(ST_ACCOUNTS);
  }
}

static void ui_account_name() {
  if (g_renameSlot < 0 || g_renameSlot >= ACCT_MAX || !g_accts.used[g_renameSlot]) {
    request_state(ST_ACCOUNTS);
    return;
  }
  lv_obj_t *scr = lv_screen_active();
  thead(scr, TRS("rinomina account", "rename account"), ST_ACCOUNTS);

  g_nameTa = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_nameTa, true);
  lv_textarea_set_max_length(g_nameTa, ACCT_LBL_MAX - 1);
  lv_textarea_set_text(g_nameTa, g_accts.label[g_renameSlot]);
  lv_textarea_set_placeholder_text(g_nameTa, TRS("etichetta (es.: Personale, Lavoro)", "label (e.g. Personal, Work)"));
  lv_obj_set_size(g_nameTa, 456, 40);
  lv_obj_set_pos(g_nameTa, 12, 54);
  style_ta(g_nameTa);

  lv_obj_t *kb = lv_keyboard_create(scr);
  style_kb(kb);
  lv_keyboard_set_textarea(kb, g_nameTa);
  lv_obj_add_event_cb(kb, acct_name_kb_cb, LV_EVENT_ALL, NULL);
}

static void ui_accounts() {
  lv_obj_t *scr = lv_screen_active();
  start_data_web();
  thead(scr, TRS("account", "accounts"), ST_SETTINGS);

  lv_obj_t *lst = tlist(scr, 13, 50, 454, 236);
  lv_obj_set_style_pad_row(lst, 6, 0);

  bool canDelete = accountCount(g_accts) > 1;
  for (int i = 0; i < ACCT_MAX; i++) {
    if (!g_accts.used[i]) continue;
    bool active = (i == g_accts.active);

    lv_obj_t *row = plain_obj(lst);
    lv_obj_set_size(row, 448, 40);

    char txt[48];
    snprintf(txt, sizeof(txt), "%s%s", active ? "> " : "  ", g_accts.label[i]);
    lv_obj_t *b = tbtn(row, 0, 0, canDelete ? 340 : 394, 40, "", F14, C_TEXT,
                       active ? C_ACCENT : C_BORDER, acct_switch_cb, (void *)(intptr_t)i);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(active ? C_ACCENT : C_TEXT), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
    if (active) {
      lv_obj_t *tag = mklabel(b, TRS("attivo", "active"), F12, C_FAINT);
      lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -10, 0);
    }
    tbtn(row, canDelete ? 346 : 400, 0, 48, 40, U_PENCIL, F14, C_MUTED, C_BORDER, acct_edit_cb, (void *)(intptr_t)i);
    if (canDelete) {
      bool armed = (g_acctDelArmed == i);
      tbtn(row, 400, 0, 48, 40, armed ? "!" : U_CROSS, F14, armed ? C_BAD : C_MUTED, armed ? C_BAD : C_BORDER,
           acct_del_cb, (void *)(intptr_t)i);
    }
  }

  if (accountFirstFree(g_accts) >= 0) {
    lv_obj_t *add = tbtn(lst, 0, 0, 448, 40, "", F14, C_ACCENT, C_FAINT, acct_add_cb, NULL);
    lv_obj_t *al = lv_obj_get_child(add, 0);
    lv_label_set_text(al, TRS("+ aggiungi account", "+ add account"));
    lv_obj_align(al, LV_ALIGN_LEFT_MID, 10, 0);
  }

  lv_obj_t *hint = mklabel(scr, TRS("solo l'account attivo interroga la API", "only the active account is polled"), F12, C_FAINT);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -10);
}

// ============================================================
// Schermate: modelli sondati (lista + modifica ID con tastiera)
// ============================================================
static int g_modelEdit = -1;
static lv_obj_t *g_modelTa = nullptr, *g_modelErr = nullptr;

static void model_row_cb(lv_event_t *e) {
  g_modelEdit = (int)(intptr_t)lv_event_get_user_data(e);
  request_state(ST_MODEL_EDIT);
}
static void ui_models() {
  lv_obj_t *scr = lv_screen_active();
  start_data_web();
  thead(scr, TRS("modelli", "models"), ST_SETTINGS);

  lv_obj_t *lst = tlist(scr, 13, 47, 454, 176);
  for (int i = 0; i < NMODELS; i++) {
    char key[16]; strlcpy(key, g_models[i].name, sizeof(key));
    for (char *q = key; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
    bool custom = strcmp(g_models[i].id, g_models[i].defId) != 0;
    kv_row(lst, key, g_models[i].id, C_TEXT, custom ? C_ACCENT : C_MUTED, model_row_cb, (void *)(intptr_t)i);
  }

  tstatic(scr, ">", F12, C_ACCENT, 12, 292);
  String url = String(TRS("più comodo dal browser: http://", "easier from a browser: http://"))
               + WiFi.localIP().toString() + "/models";
  tstatic(scr, url.c_str(), F12, C_MUTED, 26, 292);
}
static void model_kb_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    if (model_set_id(g_modelEdit, lv_textarea_get_text(g_modelTa))) request_state(ST_MODELS);
    else if (g_modelErr) {
      lv_label_set_text(g_modelErr, TRS("id non valido: solo a-z 0-9 . - _ (3-47 caratteri)",
                                        "invalid id: only a-z 0-9 . - _ (3-47 chars)"));
      lv_obj_set_style_text_color(g_modelErr, lv_color_hex(C_BAD), 0);
    }
  } else if (code == LV_EVENT_CANCEL) {
    request_state(ST_MODELS);
  }
}
static void ui_model_edit() {
  if (g_modelEdit < 0 || g_modelEdit >= NMODELS) { request_state(ST_MODELS); return; }
  lv_obj_t *scr = lv_screen_active();
  start_data_web();
  const ModelInfo &m = g_models[g_modelEdit];

  char t[40]; snprintf(t, sizeof(t), TRS("modello: %s", "model: %s"), m.name);
  for (char *q = t; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
  thead(scr, t, ST_MODELS);

  g_modelTa = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_modelTa, true);
  lv_textarea_set_max_length(g_modelTa, MODEL_ID_MAX - 1);
  lv_textarea_set_text(g_modelTa, m.id);
  lv_textarea_set_placeholder_text(g_modelTa, m.defId);
  lv_obj_set_size(g_modelTa, 456, 40);
  lv_obj_set_pos(g_modelTa, 12, 52);
  style_ta(g_modelTa);

  char d[96]; snprintf(d, sizeof(d), TRS("vuoto = predefinito (%s)", "empty = default (%s)"), m.defId);
  g_modelErr = tstatic(scr, d, F12, C_FAINT, 12, 98);

  lv_obj_t *kb = lv_keyboard_create(scr);
  style_kb(kb);
  lv_keyboard_set_textarea(kb, g_modelTa);
  lv_obj_add_event_cb(kb, model_kb_cb, LV_EVENT_ALL, NULL);
}

// ============================================================
// Schermata: info / about
// ============================================================
static void ui_about() {
  lv_obj_t *scr = lv_screen_active();
  start_data_web();
  thead(scr, "info", ST_SETTINGS);

  lv_obj_t *mark = build_claude_mark(scr);
  lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 52);

  lv_obj_t *t = mklabel(scr, "ritmo code", F22, C_TEXT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 148);

  lv_obj_t *ver = mklabel(scr, "v" FW_VERSION " " U_MIDDOT " esp32-s3 " U_MIDDOT " lvgl 9.2", F12, C_FAINT);
  lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 180);

  lv_obj_t *d = mklabel(scr, TRS("uso di claude code in tempo reale: finestre 5h e settimanale lette dalla API anthropic",
                                 "real-time claude code usage: 5h and weekly windows read from the anthropic api"), F12, C_MUTED);
  lv_obj_set_width(d, 400);
  lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
  lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 198);

  lv_obj_t *h = mklabel(scr, "guition jc4832w535 " U_MIDDOT " ips 3.5\" 480x320 " U_MIDDOT " axs15231b", F12, C_FAINT);
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 236);

  lv_obj_t *dev = mklabel(scr, TRS("sviluppato da Luca Marullo " U_MIDDOT " Innova Design Studio",
                                   "developed by Luca Marullo " U_MIDDOT " Innova Design Studio"), F12, C_TEXT);
  lv_obj_align(dev, LV_ALIGN_TOP_MID, 0, 256);
  lv_obj_t *web = mklabel(scr, "innovadesignstudio.it " U_MIDDOT " info@innovadesignstudio.it", F12, C_ACCENT);
  lv_obj_align(web, LV_ALIGN_TOP_MID, 0, 274);
  lv_obj_t *orig = mklabel(scr, TRS("basato su claude-usage-stick di Benevid Felix",
                                    "based on claude-usage-stick by Benevid Felix"), F12, C_FAINT);
  lv_obj_align(orig, LV_ALIGN_TOP_MID, 0, 296);
}

// ============================================================
// Schermata: aggiornamento firmware via Wi-Fi
// ============================================================
static void ui_ota() {
  lv_obj_t *scr = lv_screen_active();
  snprintf(g_otaCode, sizeof(g_otaCode), "%06u", (unsigned)(esp_random() % 1000000UL));
  g_otaStartMs = millis();
  g_otaBad = 0; g_otaDone = false; g_otaErr = "";
  start_data_web();
  thead(scr, TRS("aggiorna firmware", "update firmware"), ST_SETTINGS);

  char url[64];
  snprintf(url, sizeof(url), "http://%s/update", WiFi.localIP().toString().c_str());
  lv_obj_t *r = trow(scr, 12, 60);
  mklabel(r, "> ", F14, C_ACCENT);
  mklabel(r, TRS("apri ", "open "), F14, C_MUTED);
  mklabel(r, url, F14, C_TEXT);

  tstatic(scr, TRS("codice", "code"), F12, C_MUTED, 12, 96);
  lv_obj_t *code = mklabel(scr, g_otaCode, F54, C_ACCENT);
  lv_obj_set_style_text_letter_space(code, 6, 0);
  lv_obj_set_pos(code, 12, 112);

  g_otaTime = tlabel(scr, F12, C_FAINT, 12, 176);

  g_otaBar = lv_bar_create(scr);
  lv_obj_remove_style_all(g_otaBar);
  lv_obj_set_pos(g_otaBar, 12, 214); lv_obj_set_size(g_otaBar, 456, 6);
  lv_obj_set_style_bg_color(g_otaBar, lv_color_hex(C_TRACK), 0);
  lv_obj_set_style_bg_opa(g_otaBar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(g_otaBar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_otaBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(g_otaBar, 0, 100);

  g_otaStat = tlabel(scr, F14, C_MUTED, 12, 232);
  lv_obj_set_width(g_otaStat, 456);
  lv_label_set_long_mode(g_otaStat, LV_LABEL_LONG_WRAP);
  ota_status(TRS("in attesa del file " U_MIDDOT " token, pin e storico restano intatti",
                 "waiting for the file " U_MIDDOT " token, pin and history are kept"), C_MUTED);
  if (esp_ota_get_last_invalid_partition())
    tstatic(scr, TRS("l'ultimo aggiornamento non partiva: ripristinata la versione precedente",
                     "the last update failed to start: previous version restored"), F12, C_WARN, 12, 270);
  tstatic(scr, TRS("oppure dal pc: ./build.sh ota <ip> <codice>", "or from the pc: ./build.sh ota <ip> <code>"),
          F12, C_FAINT, 12, 290);
}
// conferma il firmware dopo 30 s di funzionamento stabile (altrimenti il bootloader
// torna alla versione precedente al prossimo avvio); segnala un rollback avvenuto
static void ota_health_tick() {
  static bool done = false;
  if (done || millis() < 30000) return;
  done = true;
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[OTA] nuovo firmware confermato");
  }
}
static void ota_tick() {
  if (g_otaRebootAt && (int32_t)(millis() - g_otaRebootAt) >= 0) { Serial.println("[OTA] riavvio"); delay(100); ESP.restart(); }
  if (g_state != ST_OTA) { g_otaCode[0] = 0; return; }
  if (g_otaBusy || g_otaDone) return;
  uint32_t el = millis() - g_otaStartMs;
  if (el >= OTA_WINDOW_MS) { g_otaCode[0] = 0; request_state(ST_SETTINGS); return; }
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  if (!g_otaCode[0]) { ota_status(TRS("troppi codici errati " U_MIDDOT " riapri la schermata", "too many wrong codes " U_MIDDOT " reopen this screen"), C_BAD); return; }
  uint32_t left = (OTA_WINDOW_MS - el) / 1000;
  char s[40]; snprintf(s, sizeof(s), TRS("valido ancora %u:%02u", "valid for %u:%02u"), (unsigned)(left / 60), (unsigned)(left % 60));
  if (g_otaTime) lv_label_set_text(g_otaTime, s);
}

// ============================================================
// Navigazione generica
// ============================================================
static void nav_cb(lv_event_t *e) {
  State s = (State)(intptr_t)lv_event_get_user_data(e);
  request_state(s);
}

// ============================================================
// Render dello stato attuale
// ============================================================
static void render_state() {
  // la pagina impostazioni ridisegnata da se' stessa mantiene lo scorrimento
  g_setScroll = (g_state == ST_SETTINGS && g_pending == ST_SETTINGS && g_setList) ? lv_obj_get_scroll_y(g_setList) : 0;
  g_setList = nullptr;
  g_state = g_pending;
  stop_web();                                 // ogni schermata avvia il server che le serve
  moment_close();                             // l'overlay vive in lv_layer_top
  if (g_nt.scrim) {                           // l'avviso a schermo intero sopravvive ai rebuild del dashboard
    if (g_state == ST_MAIN) { g_ntPend = g_nt.kind; g_ntT0 = g_nt.t0; }
    notice_close();
  }
  pause_menu_close();
  tm_menu_close();
  wx_week_close();
  shade_close();
  upd_close();                                   // se l'installazione e' in corso il loop la rimette
  night_clock_close();
  lv_obj_clean(lv_layer_top());
  // invalida i puntatori vivi prima di distruggere la vecchia schermata
  memset(&g_ui, 0, sizeof(g_ui));
  g_mascN = 0;
  g_pinDots = g_pinMsg = nullptr;
  g_tokMsg = nullptr;
  g_nameTa = nullptr;
  g_modelTa = g_modelErr = nullptr;
  g_hdrStatus = nullptr;
  g_spin = nullptr;
  g_briLbl = g_wipeLbl = g_pollLbl = g_tzLbl = g_slideLbl = nullptr;
  g_otaStat = g_otaBar = g_otaTime = nullptr;

  lv_obj_clean(lv_screen_active());
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

  switch (g_state) {
    case ST_PIN:
    case ST_SETUP_PIN: ui_pin(); break;
    case ST_WIFI:      ui_wifi(); break;
    case ST_TOKEN:     ui_token(); break;
    case ST_LOADING:   ui_loading(g_wifi.isConnected() ? g_wifi.getSSID().c_str()
                                                       : TRS("connessione WiFi", "connecting WiFi")); break;
    case ST_MAIN:      ui_main(); break;
    case ST_SETTINGS:  ui_settings(); break;
    case ST_ACCOUNTS:  ui_accounts(); break;
    case ST_ACCT_NAME: ui_account_name(); break;
    case ST_ABOUT:     ui_about(); break;
    case ST_MODELS:    ui_models(); break;
    case ST_MODEL_EDIT: ui_model_edit(); break;
    case ST_OTA:       ui_ota(); break;
    case ST_NETWORKS:  ui_networks(); break;
    case ST_ERROR:     ui_message(TRS("errore", "failed"),
                                  g_usage.error[0] ? g_usage.error : TRS("nessun dato", "no data"), C_BAD); break;
    default: break;
  }
}

// ============================================================
// Tempo (NTP) e ciclo dei dati
// ============================================================
static void apply_tz() {
  if (g_tzOffset == TZ_ROME) { configTzTime(TZ_ROME_RULE, NTP_SERVER_1, NTP_SERVER_2); return; }
  char rule[16];                          // POSIX ha il segno invertito: GMT+2 = "<+02>-2"
  snprintf(rule, sizeof(rule), "<%+03d>%d", g_tzOffset, -g_tzOffset);
  configTzTime(rule, NTP_SERVER_1, NTP_SERVER_2);
}
static void ensure_time() {
  if (g_timeInit || !g_wifi.isConnected()) return;
  apply_tz();
  g_timeInit = true;
  Serial.println("[NTP] sync avviato");
}


// Task di rete (core 0): aspetta una notifica, esegue le chiamate e pubblica il
// risultato. Stesso lavoro di prima: utilizzo, poi (solo se ok) status + 1 sonda.
static void net_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (g_updRunning) {                             // aggiornamento del firmware al posto del giro normale
      xSemaphoreTake(g_httpsLock, portMAX_DELAY);
      String err;
      bool ok = g_wifi.isConnected() && installFromUrl(g_updUrl, upd_progress, err);
      if (!ok && !err.length()) err = "wifi non connesso";
      xSemaphoreGive(g_httpsLock);
      if (ok) { g_updState = UPD_REBOOT; delay(1500); ESP.restart(); }
      Serial.printf("[OTA] non riuscito: %s\n", err.c_str());
      g_updErr = err;
      g_updState = UPD_FAILED;
      g_updRunning = false; g_updInstallReq = false; g_netBusy = false;
      g_updDone = true;
      continue;
    }
    xSemaphoreTake(g_httpsLock, portMAX_DELAY);
    uint32_t t0 = millis();
    if (!g_wifi.isConnected()) g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS);
    memset(&g_net.usage, 0, sizeof(g_net.usage));
    g_net.usageOk = fetchUsage(g_net.token, g_net.usage);
    g_net.statusOk = false;
    g_net.probed = false;
    if (g_net.usageOk) {
      g_net.statusOk = fetchModelStatus(g_net.status);
      probeModel(g_net.token, g_net.modelId, g_net.probe);
      g_net.probed = true;
    }
    memset(g_net.token, 0, sizeof(g_net.token));   // il token non resta nel job
    Serial.printf("[NET] fine aggiornamento: RAM interna libera %u KB, minima dall'avvio %u KB\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    g_net.durMs = millis() - t0;
    xSemaphoreGive(g_httpsLock);
    g_netDone = true;
  }
}

// Meteo e PC per la home (core 0). Il meteo attende che net_task abbia finito:
// due handshake TLS insieme non stanno nella RAM interna.
static void extra_task(void *) {
  for (;;) {
    if (g_wxReq) {
      WeatherData w = {};
      if (g_wifi.isConnected()) {
        xSemaphoreTake(g_httpsLock, portMAX_DELAY);
        fetchWeather(g_wxLat, g_wxLon, w);
        xSemaphoreGive(g_httpsLock);
      }
      g_wxRes = w;
      g_wxReq = false;
      g_wxDone = true;
    }
    if (g_updCheckReq) {
      char tag[16], url[200];
      bool ok = false;
      if (g_wifi.isConnected()) {
        xSemaphoreTake(g_httpsLock, portMAX_DELAY);
        ok = fetchLatestRelease(tag, sizeof(tag), url, sizeof(url));
        xSemaphoreGive(g_httpsLock);
      }
      if (ok) {
        strlcpy(g_updTag, tag, sizeof(g_updTag));
        strlcpy(g_updUrl, url, sizeof(g_updUrl));
        g_updState = ver_newer(tag) ? UPD_AVAILABLE : UPD_UPTODATE;
      } else {
        g_updState = UPD_ERROR;
      }
      g_updCheckReq = false;
      g_updDone = true;
    }
    if (g_pcNtfReq) {
      PcNotify n = g_pcNtf;
      g_pcNtfReq = false;
      if (g_wifi.isConnected()) postPcNotify(n.host, n.ev, n.title, n.msg);
    }
    if (g_pcReq) {
      PcStats p = {};
      if (g_wifi.isConnected()) fetchPcStats(g_pcReqHost, p);
      g_pcRes = p;
      g_pcReq = false;
      g_pcDone = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Avvia un aggiornamento (loop, core 1). false = task occupato.
static bool net_start(bool firstLoad) {
  if (g_netBusy || g_netDone || !g_netTask) return false;
  ensure_time();
  strlcpy(g_net.token, g_token, sizeof(g_net.token));
  int mi = g_probeIdx % NMODELS;
  g_net.modelIdx = mi;
  strlcpy(g_net.modelId, g_models[mi].id, sizeof(g_net.modelId));
  g_net.status = g_status;             // se status.claude.com fallisce resta l'ultimo noto
  g_net.epoch = g_dataEpoch;
  g_net.firstLoad = firstLoad;
  g_netBusy = true;
  if (!firstLoad) { g_refreshing = true; set_hdr_status(); }
  xTaskNotifyGive(g_netTask);
  return true;
}

// Applica il risultato (loop, core 1): storico, soglie, UI.
static void net_apply() {
  g_netDone = false;
  g_netBusy = false;
  bool first = g_net.firstLoad;
  g_refreshing = false;

  if (g_net.epoch != g_dataEpoch) {
    // token/account cambiati durante la richiesta: dati di un altro account
    Serial.println("[NET] risultato scartato: account cambiato durante l'aggiornamento");
    if (g_state == ST_MAIN) set_hdr_status();
    return;                            // ST_LOADING riparte da solo (g_loadPending)
  }
  if (g_perfOn) Serial.printf("[PERF] rete in background: %u ms (UI libera)\n", (unsigned)g_net.durMs);

  bool ok = g_net.usageOk;
  bool rebuild = false;
  if (ok) {
    int moodBefore[NMODELS];
    for (int i = 0; i < NMODELS; i++) moodBefore[i] = model_mood(i);
    g_usage = g_net.usage;
    g_lastOkMs = millis();
    g_lastFetchOk = true;
    hist_push(g_usage.h5, g_usage.d7); accumulate_heat(g_usage.h5); week_record(g_usage.d7, g_usage.d7ResetEpoch); save_history();
    check_thresholds();
    if (g_net.statusOk) g_status = g_net.status;
    if (g_net.probed) {
      int mi = g_net.modelIdx;
      g_probeIdx++;
      // se l'ID e' stato modificato durante la richiesta, il risultato non vale piu'
      if (strcmp(g_models[mi].id, g_net.modelId) == 0) {
        g_models[mi].pr = g_net.probe;
        g_models[mi].atMs = millis();
        if (g_net.probe.code > 0) {                   // storico latenze (7 valori)
          ModelInfo &m = g_models[mi];
          if (m.lhN == 7) { memmove(m.lh, m.lh + 1, 6 * sizeof(m.lh[0])); m.lhN = 6; }
          m.lh[m.lhN++] = g_net.probe.ms;
        }
      }
    }
    for (int i = 0; i < NMODELS; i++)
      if (moodBefore[i] != model_mood(i)) rebuild = true;   // mascotte cambia umore
  } else {
    g_lastFetchOk = false;
    if (first) g_usage = g_net.usage;  // ST_ERROR mostra g_usage.error
  }
  g_lastPollMs = millis();

  if (first) {
    if (g_state == ST_LOADING) request_state(ok ? ST_MAIN : ST_ERROR);
    return;
  }
  if (g_state == ST_LOADING && ok) {    // "Aggiorna ora" arrivato mentre il poll era in corso:
    g_loadPending = false;               // i dati sono freschi, niente seconda richiesta
    request_state(ST_MAIN);
    return;
  }
  if (g_state != ST_MAIN) return;
  if (rebuild) request_state(ST_MAIN);   // mascotte cambiate -> rebuild (resta sul tile)
  else refresh_ui_values();              // resto: in place
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  // USB CDC nativo: se il cavo e' collegato a un PC che tiene la porta aperta ma non
  // legge (monitor seriale chiuso), ogni print aspettava fino a ~2 s (100 ms x 20
  // tentativi) e il loop si bloccava: PIN e swipe non rispondevano. Con timeout 0 i
  // messaggi che nessuno legge vengono scartati e l'interfaccia non si ferma mai.
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.println("\n=== Ritmo Code (touch) ===");

  // Display
  Arduino_DataBus *bus = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
  // sequenza di inizializzazione del pannello 320x480 di questa scheda (come l'esempio JC3248W535 di
  // Arduino_GFX). Senza, la libreria usa quella del pannello 180x640: tensioni e VCOM sbagliate e
  // l'immagine, dopo un po', sbiadiva lasciando i numeri in trasparenza.
  Arduino_GFX *g = new Arduino_AXS15231B(bus, GFX_NOT_DEFINED, 0, false, 320, 480, 0, 0, 0, 0,
                                         axs15231b_320480_type1_init_operations,
                                         sizeof(axs15231b_320480_type1_init_operations));
  g_panel = g;
  gfx = new Arduino_Canvas(320, 480, g, 0, 0, 0);
  if (!gfx->begin(QSPI_FREQ)) { Serial.println("FATAL display"); while (1) delay(1000); }
  gfx->fillScreen(0x0000); gfx->flush();
  canvas_fb = gfx->getFramebuffer();

  // secondo framebuffer per l'invio in parallelo (PSRAM, ~300 KB) + task sul core 0
  tx_fb = (uint16_t *)heap_caps_malloc(320 * 480 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tx_fb) { Serial.println("FATAL tx_fb"); fatal_screen("PSRAM non disponibile"); }
  memcpy(tx_fb, canvas_fb, 320 * 480 * sizeof(uint16_t));
  g_txIdle = xSemaphoreCreateBinary();
  xSemaphoreGive(g_txIdle);
  // stessa priorita' del task di rete (2): sul core 0 si alternano a turno.
  // NON usare disableCore0WDT(): su core 3.3.x l'hook dell'idle resta registrato e
  // stampa "esp_task_wdt_reset: task not found" a ogni ciclo (log intasato, CPU
  // sprecata). Basta la pausa di 1 ms in disp_tx_task per far girare l'idle.
  // stack in PSRAM: il task non tocca la flash, e ogni KB di RAM interna serve al TLS
  xTaskCreatePinnedToCoreWithCaps(disp_tx_task, "disp", 4096, nullptr, 2, &g_txTask, 0,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  // Retroilluminazione via PWM (luminosita' regolabile)
  ledcAttach(TFT_BL, 5000, 8);
  touch_dev.begin();

  // LVGL
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  // Buffer di disegno in PSRAM: meta' schermo (160 righe, ~154 KB). In RAM interna
  // era piu' veloce da scrivere, ma sottraeva RAM al TLS (mbedtls su questo core
  // alloca solo in RAM interna): durante gli aggiornamenti la RAM interna libera
  // scendeva a 14 KB e la scheda rallentava fino al crash. Buffer grande = meno
  // strisce per frame, che compensa in parte la PSRAM piu' lenta.
  const uint32_t DRAW_BUF_LINES = 160;
  uint32_t bufSize = SCREEN_WIDTH * DRAW_BUF_LINES * 2;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { Serial.println("FATAL buffer LVGL"); fatal_screen("Memoria insufficiente per il display"); }
  lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_display_set_flush_cb(disp, disp_flush_cb);
  lv_display_set_buffers(disp, buf, NULL, bufSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);

  load_persisted();
  apply_brightness();
  perf_apply();
  boot_splash(TRS("Avvio...", "Starting..."));

  if (!LittleFS.begin(true)) Serial.println("LittleFS: fallito");
  else {
    // Migrazione multi-account, fatta apposta in DUE passi: copia su un
    // .tmp e solo dopo rinomina. Se manca la corrente a meta', resta un .tmp
    // troncato — /hist0.bin non esiste ancora, quindi il boot successivo rifa' la
    // migrazione. Copiare direttamente sul file finale lascerebbe un /hist0.bin a
    // meta' che il controllo "exists" scambierebbe per migrazione completata.
    // Il /hist.bin originale non viene mai cancellato (rollback al firmware precedente).
    if (LittleFS.exists("/hist.bin") && !LittleFS.exists("/hist0.bin")) {
      LittleFS.remove("/hist0.tmp");                       // residuo di un tentativo precedente
      bool ok = copy_file("/hist.bin", "/hist0.tmp") &&
                LittleFS.rename("/hist0.tmp", "/hist0.bin");
      if (!ok) LittleFS.remove("/hist0.tmp");
      Serial.printf("[HIST] migrazione multi-account: %s\n", ok ? "ok" : "FALLITA");
    }
    load_history();
    pomo_load();
    al_load();
  }

  g_wifi.begin();
  // rete su un task del core 0 (8 KB di stack, come il loopTask che prima faceva HTTPS)
  g_httpsLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 2, &g_netTask, 0);
  xTaskCreatePinnedToCoreWithCaps(extra_task, "extra", 8192, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);

  boot_status(TRS("Connessione al WiFi...", "Connecting to WiFi..."));
  if (g_hasToken) {
    // Prova il WiFi subito (intanto l'utente digita il PIN)
    g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS, boot_wifi_tick);
    request_state(ST_PIN);
  } else {
    g_onboarding = true;
    // Se c'e' gia' un WiFi salvato (riavvio a meta' onboarding), salta diretto al token
    request_state(g_wifi.autoConnect(WIFI_CONNECT_TIMEOUT_MS, boot_wifi_tick) ? ST_TOKEN : ST_WIFI);
  }
  g_bootSub = nullptr;                     // render_state() distrugge la schermata di boot
}

// Il contatore FPS lo crea LVGL una sola volta in lv_display_create(). Qui lo si
// mostra/nasconde e si mette in pausa il suo timer. NON richiamare
// lv_sysmon_show_performance(): ogni chiamata crea un'altra label e stacca la
// precedente dai dati, che restava a schermo ferma a "0 FPS".
static void perf_apply() {
  lv_display_t *d = lv_display_get_default();
  if (!d || !d->perf_label) return;
  if (g_perfOn) {
    lv_obj_clear_flag(d->perf_label, LV_OBJ_FLAG_HIDDEN);
    if (d->perf_sysmon_backend.timer) lv_timer_resume(d->perf_sysmon_backend.timer);
  } else {
    lv_obj_add_flag(d->perf_label, LV_OBJ_FLAG_HIDDEN);
    if (d->perf_sysmon_backend.timer) lv_timer_pause(d->perf_sysmon_backend.timer);
  }
}
static void perf_tick() {
  static uint32_t lastLoop = 0, lastReport = 0;
  uint32_t now = millis();
  if (lastLoop) { uint32_t gap = now - lastLoop; if (gap > g_perf.loopMaxMs) g_perf.loopMaxMs = gap; }
  lastLoop = now;
  if (now - lastReport < 2000) return;
  uint32_t el = now - lastReport;
  lastReport = now;
  if (g_perfOn && g_perf.frames) {
    uint32_t txn = g_perf.txFrames ? g_perf.txFrames : 1;
    Serial.printf("[PERF] %.1f fps | ruota %.1f ms (max %.1f) | attesa invio %.1f ms (max %.1f) | "
                  "copia %.1f ms (max %.1f) | invio core0 %.1f ms (max %.1f) | loop max %u ms | "
                  "heap %u KB (min %u), psram %u KB\n",
                  g_perf.frames * 1000.0f / el,
                  g_perf.rotUs / 1000.0f / g_perf.frames, g_perf.rotMaxUs / 1000.0f,
                  g_perf.waitUs / 1000.0f / g_perf.frames, g_perf.waitMaxUs / 1000.0f,
                  g_perf.copyUs / 1000.0f / g_perf.frames, g_perf.copyMaxUs / 1000.0f,
                  g_perf.txUs / 1000.0f / txn, g_perf.txMaxUs / 1000.0f,
                  (unsigned)g_perf.loopMaxMs,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  }
  memset(&g_perf, 0, sizeof(g_perf));
}

void loop() {
  lv_task_handler();
  perf_tick();
  spin_tick();

  // Server web (token durante l'onboarding; /models sul dashboard)
  if (g_web) {
    g_web->handleClient();
    if (g_state == ST_TOKEN && g_tokenGot) {
      g_tokenGot = false;
      if (g_onboarding || !g_sessionPin[0]) request_state(ST_SETUP_PIN);
      else finalize_pending_token();
    }
  }
  ota_tick();
  ota_health_tick();

  if (g_dirty) {
    g_dirty = false;
    render_state();
    g_loadPending = (g_state == ST_LOADING);   // il caricamento parte appena il task e' libero
  }

  // Rete in background: avvio del primo caricamento, risultati pronti, poll/refresh
  if (g_loadPending && g_state == ST_LOADING && net_start(true)) g_loadPending = false;
  if (g_netDone) net_apply();
  // aggiornamento: il task di rete installa appena ha finito il giro in corso
  if (g_updInstallReq && !g_updRunning && !g_netBusy && !g_netDone) {
    g_updRunning = true; g_netBusy = true;
    xTaskNotifyGive(g_netTask);
  }
  if ((g_updState == UPD_INSTALLING || g_updState == UPD_REBOOT) && !g_upd.scrim) upd_overlay();   // dopo un cambio di schermata
  upd_tick();
  if (g_updDone && millis() - g_updCheckStartMs >= 1500) {   // il risultato sostituisce "controllo..." dopo 1,5 s
    g_updDone = false;
    set_hdr_status();
    if (g_state == ST_SETTINGS) request_state(ST_SETTINGS);
  }
  // controllo automatico della nuova versione: 90 s dopo l'avvio, poi una volta al giorno
  if (g_state == ST_MAIN && g_wifi.isConnected() && !g_updCheckReq && g_updState != UPD_INSTALLING &&
      g_updState != UPD_REBOOT && millis() > 90000UL &&
      (!g_updCheckedMs || millis() - g_updCheckedMs > 24UL * 3600UL * 1000UL)) {
    g_updCheckedMs = millis();
    if (g_updState != UPD_AVAILABLE) g_updState = UPD_CHECKING;
    g_updCheckReq = true;
  }
  screen_tick();
  night_clock_sync();
  tm_tick();
  // home: meteo ogni 30 min (nuovo tentativo dopo 5 min se fallisce), PC ogni 5 s se la home e' visibile
  if (g_wxDone) {
    g_wxDone = false;
    if (g_wxRes.ok) { g_wx = g_wxRes; g_wxAtMs = millis(); }
    home_redraw();
    if (g_wxWeek) { wx_week_close(); wx_week_open(nullptr); }
  }
  if (g_pcDone) {
    g_pcDone = false;
    g_pc = g_pcRes;
    if (g_pc.ok) {
      g_pcAtMs = millis();
      if (g_pc.ccId) cc_event(g_pc.ccId, g_pc.ccEv, g_pc.ccProj, g_pc.ccDur, g_pc.ccAge);
      if (g_pc.ccBusy != g_ccBusyN) { g_ccBusyN = g_pc.ccBusy; cc_busy_ui(); }
    }
    if (g_pc.ok) {                                   // il grafico avanza a ogni lettura
      g_pcHistAtMs = millis();
      float v[3] = {g_pc.cpu, g_pc.gpu, g_pc.ram};
      if (g_pcHistN == PC_HIST) { for (int k = 0; k < 3; k++) memmove(g_pcHist[k], g_pcHist[k] + 1, PC_HIST - 1); g_pcHistN--; }
      for (int k = 0; k < 3; k++) g_pcHist[k][g_pcHistN] = (uint8_t)(v[k] < 0 ? 0 : (v[k] > 100 ? 100 : v[k] + 0.5f));
      g_pcHistN++;
    }
    pc_redraw();
    home_redraw();
  }
  if (g_state == ST_MAIN && g_wifi.isConnected() && g_screenMode < 2 && !g_wxReq && !g_wxDone && g_wxLat != 0) {
    bool due = g_wx.ok ? millis() - g_wxAtMs > 30UL * 60UL * 1000UL : (g_wxTryMs == 0 || millis() - g_wxTryMs > 5UL * 60UL * 1000UL);
    if (due) { g_wxTryMs = millis(); g_wxReq = true; }
  }
  if (g_state == ST_MAIN && g_pcHost[0] && g_screenMode < 2 && !g_pcReq && !g_pcDone &&
      millis() - g_pcTryMs >= PC_INT_S[g_pcIntIdx] * 1000UL - 50) {
    g_pcTryMs = millis();
    strlcpy(g_pcReqHost, g_pcHost, sizeof(g_pcReqHost));
    g_pcReq = true;
  }
  if (g_state == ST_MAIN && !g_netBusy && (g_wantRefresh || !polling_paused()) &&
      (g_wantRefresh || millis() - g_lastPollMs > (uint32_t)g_pollSec * 1000)) {
    if (net_start(false)) g_wantRefresh = false;
  }

  // Aggiornamento continuo: contatori (1s), barra di refresh (250ms), mascotte,
  // slideshow (5s, pausa di 10s dopo qualsiasi tocco)
  if (g_state == ST_MAIN) {
    uint32_t now = millis();
    static uint32_t lastTick = 0, lastBar = 0, lastBob = 0, blinkAt = 0;
    static bool blinkClosed = false;
    if (now - lastTick > 1000) {
      lastTick = now; dash_tick(); home_tick(); if (g_tmMode) set_hdr_status();
      if (g_shadeTm) { char tt[12]; shade_timer_text(tt, sizeof(tt)); label_set(g_shadeTm, tt); }
      if (g_ccBusyN && (!g_pcAtMs || now - g_pcAtMs > pc_stale_ms())) { g_ccBusyN = 0; cc_busy_ui(); }
    }
    static uint32_t lastSpark = 0;
    static int sparkK = 4;
    if (g_ccBusyN > 0 && g_ui.hdrSpark && g_screenMode < 2 && now - lastSpark > 160) {
      lastSpark = now; sparkK = (sparkK + 1) % 10;
      char sp[8]; snprintf(sp, sizeof(sp), "%s ", SPIN[sparkK]);
      label_set(g_ui.hdrSpark, sp);
    }
    if (now - lastBar > 1000 && g_ui.refBar) {       // filo del refresh: al massimo 1 ridisegno/s
      lastBar = now;
      const int W = 275;
      int v;
      if (g_refreshing) v = W;
      else if (polling_paused()) v = 0;
      else {
        uint32_t el = now - g_lastPollMs, per = (uint32_t)g_pollSec * 1000;
        v = el >= per ? 0 : (int)(W - (uint64_t)el * W / per);
      }
      if (v != lv_obj_get_width(g_ui.refBar)) lv_obj_set_width(g_ui.refBar, v);   // solo se cambia
    }
    // cursore ▌ della riga di prompt: lampeggia solo sulla pagina Ora
    static uint32_t lastCur = 0;
    if (g_ui.agCursor && g_curTile == 1 && now - lastCur > 700 && now - g_lastTouchMs > 400) {
      lastCur = now;
      if (lv_obj_has_flag(g_ui.agCursor, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(g_ui.agCursor, LV_OBJ_FLAG_HIDDEN);
      else                                                     lv_obj_add_flag(g_ui.agCursor, LV_OBJ_FLAG_HIDDEN);
    }
    // Le mascotte vivono solo sulla pagina Modelli: animarle altrove (o mentre si
    // scorre) costava ridisegni continui senza che nessuno le vedesse.
    bool mascLive = (g_curTile == 2) && (now - g_lastTouchMs > 400) && !g_mo.scrim;
    if (mascLive && now - lastBob > 80) {           // animazione in base all'umore
      lastBob = now;
      float ph = now / 600.0f;
      for (int i = 0; i < g_mascN; i++) {
        if (!g_masc[i].cont) continue;
        if (g_masc[i].mood == 1)                    // ok: ondeggia allegro
          lv_obj_set_y(g_masc[i].cont, g_masc[i].baseY + (int)(2.0f * sinf(ph + i * 0.9f) - 1.0f));
        else if (g_masc[i].mood == 2) {             // limitato: ondeggio corto + sudore
          lv_obj_set_y(g_masc[i].cont, g_masc[i].baseY + (int)(1.2f * sinf(ph * 0.6f + i)));
          if (g_masc[i].drop) {
            uint32_t cyc = (now + i * 300) % 900;
            lv_obj_set_y(g_masc[i].drop, 6 + (int)(cyc * 22 / 900));
            lv_obj_set_style_bg_color(g_masc[i].drop, lv_color_mix(lv_color_hex(C_BLUE), lv_color_hex(C_BG),
                                                                   (uint8_t)(255 - cyc * 190 / 900)), 0);
          }
        }
      }
    }
    uint32_t bp = blinkClosed ? 150 : 3000;
    if (mascLive && now - blinkAt > bp) {            // sbatte le palpebre (solo chi e' ok)
      blinkAt = now; blinkClosed = !blinkClosed;
      for (int i = 0; i < g_mascN; i++) {
        if (g_masc[i].mood != 1) continue;
        for (int k = 0; k < 2; k++) {
          if (!g_masc[i].lid[k]) continue;
          if (blinkClosed) lv_obj_clear_flag(g_masc[i].lid[k], LV_OBJ_FLAG_HIDDEN);
          else             lv_obj_add_flag(g_masc[i].lid[k], LV_OBJ_FLAG_HIDDEN);
        }
      }
    }
    if (g_slideSec > 0 && g_ui.tv && !g_refreshing && !g_mo.scrim && !g_nt.scrim && !g_tmMenu && !g_wxWeek && !g_shade && g_screenMode < 2 &&
        now - g_lastTouchMs > 10000 && now - g_lastSlideMs > (uint32_t)g_slideSec * 1000) {
      g_lastSlideMs = now;
      int next = (g_curTile + 1) % NTILES;
      lv_tileview_set_tile_by_index(g_ui.tv, next, 0, LV_ANIM_ON);
    }

    // pausa a tempo scaduta: riprende da sola
    if (g_userPause && g_pauseUntil) {
      time_t tn = time(nullptr);
      if (tn > 1000000000L && (uint32_t)tn >= g_pauseUntil) pause_set(false, 0);
    }
    // torna alla home dopo N minuti senza tocchi (una volta per periodo di inattivita')
    static uint32_t homedFor = 0;
    if (g_clockIdx && g_ui.tv && g_curTile != 0 && !g_mo.scrim && !g_nt.scrim && !g_pauseMenu && !g_tmMenu && !g_wxWeek && !g_shade && g_screenMode < 2 &&
        homedFor != g_lastTouchMs && now - g_lastTouchMs > CLOCK_MIN[g_clockIdx] * 60000UL) {
      homedFor = g_lastTouchMs;
      lv_tileview_set_tile_by_index(g_ui.tv, 0, 0, LV_ANIM_ON);
    }

    // avviso di Claude rimandato: appena finisce il focus e non c'e' altro a schermo
    if (g_ccDeferred && g_tmMode != TM_FOCUS && !g_ntPend && !g_nt.scrim) {
      g_ccDeferred = false;
      g_ntPend = NT_CLAUDE; g_ntT0 = 0;
    }
    // pannello a tendina (chiesto dal gesto nel driver del touch)
    if (g_shadeReq) {
      int r = g_shadeReq; g_shadeReq = 0;
      if (r == 2) shade_close();
      else if (!g_nt.scrim && !g_mo.scrim && !g_pauseMenu && !g_tmMenu && !g_wxWeek && g_screenMode < 2) shade_open(true);
    }
    // Momenti di soglia: mostra quello in sospeso e anima l'overlay attivo
    if (g_pendWin >= 0 && !g_mo.scrim && !g_refreshing && g_screenMode < 2) {
      show_moment(g_pendWin, g_pendThr);
      AlertRec ar = {};
      ar.at = (uint32_t)time(nullptr); ar.moment = true;
      ar.win = g_pendWin; ar.thr = g_pendThr; ar.peak = (uint8_t)g_pendPeak;
      al_push(ar);
      char t[64], m[64];
      const char *wn = g_pendWin ? TRS("7 giorni", "7-day") : TRS("5 ore", "5-hour");
      if (g_pendThr) {
        snprintf(t, sizeof(t), TRS("finestra %s al %d%%", "%s window at %d%%"), wn, g_pendThr);
        strlcpy(m, g_pendThr >= 100 ? TRS("limite raggiunto: attendi il reset", "limit reached: wait for the reset")
                                    : TRS("controlla il ritmo di utilizzo", "keep an eye on your usage pace"), sizeof(m));
      } else {
        snprintf(t, sizeof(t), TRS("finestra %s di nuovo disponibile", "%s window available again"), wn);
        strlcpy(m, TRS("si riparte", "back to work"), sizeof(m));
      }
      pc_notify(g_pendThr ? "thr" : "reset", t, m);
      g_pendWin = -1;
    }
    if (g_mo.scrim) moment_tick();
    // Avvisi di Claude Code e del timer: uno nuovo riaccende lo schermo. Di notte (schermo spento o
    // orologio) quelli di Claude Code non compaiono; il timer invece si', l'hai chiesto tu.
    if (g_ntPend == NT_CLAUDE && g_screenMode >= 2 && !g_ntT0) g_ntPend = NT_NONE;
    if (g_ntPend && !g_mo.scrim && !g_refreshing) {
      int k = g_ntPend;
      g_ntPend = NT_NONE;
      if (!g_ntT0) g_lastTouchMs = millis();
      if (k == NT_CLAUDE) cc_show(); else tm_show();
    }
    if (g_nt.scrim) notice_tick();
  }

  delay(5);
}
