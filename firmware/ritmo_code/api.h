#pragma once
#include <stdint.h>

// Uso del rate-limit di Claude (unified), estratto dagli header della risposta.
// Header verificati su un account reale (vedi sondaggio): tutti gli unified-*.
struct UsageData {
    float    h5;                 // utilizzo 5h in % (0–100)
    float    d7;                 // utilizzo 7d in % (0–100)
    uint32_t h5ResetEpoch;       // unix ts del reset della finestra 5h
    uint32_t d7ResetEpoch;       // unix ts del reset della finestra 7d
    uint32_t unifiedResetEpoch;  // unix ts del reset della finestra rappresentativa

    char     statusOverall[16];  // allowed | allowed_warning | rejected
    char     status5h[16];       // allowed | rejected | ...
    char     status7d[16];
    char     repClaim[20];       // five_hour | seven_day (quale fa da collo di bottiglia)
    float    fallbackPct;        // 0–100 (fallback-percentage * 100)
    char     overageStatus[16];  // allowed | rejected
    char     overageReason[32];  // out_of_credits | org_level_disabled | ...

    bool     ok;                 // true se il fetch e' riuscito
    char     error[64];          // messaggio di errore se ok=false
};

bool fetchUsage(const char* token, UsageData& out);

// Sonda leggera di UN modello (POST max_tokens:1): misura latenza e codice HTTP.
// 200 = disponibile · 429 = limitato per questo piano/finestra · 5xx = errore della API.
struct ProbeResult {
    int      code;   // codice HTTP (0 = mai sondato, <0 = errore di rete)
    uint16_t ms;     // latenza della richiesta
};
bool probeModel(const char* token, const char* modelId, ProbeResult& out);
