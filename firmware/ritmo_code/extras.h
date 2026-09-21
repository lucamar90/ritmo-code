#pragma once
// Dati extra della pagina home: meteo (Open-Meteo, gratuito e senza chiave) e
// statistiche del PC (app Ritmo Code PC Monitor sul computer, GET /data.json).
// Nessuna di queste richieste va ad Anthropic.
#include <Arduino.h>

struct WeatherData {
  bool     ok;
  float    temp;           // temperatura attuale (°C)
  int      code;           // codice meteo WMO attuale
  bool     isDay;
  float    tmax, tmin;     // oggi
  float    tmax2, tmin2;   // domani
  int      code2;          // codice meteo di domani
  char     sunrise[6];     // "07:03"
  char     sunset[6];
  uint8_t  rain[12];       // probabilita' di pioggia (%) per le prossime 12 ore
  int      rainHour0;      // ora locale della prima voce di rain[]
  // previsioni della settimana (indice 0 = oggi)
  int      days;
  float    dmax[7], dmin[7];
  int      dcode[7];
  uint8_t  drain[7];       // probabilita' massima di pioggia del giorno (%)
};

#define PC_MAX_DISKS 6
#define PC_MAX_FANS 4
struct PcDisk { char letter; float used, freeGb, totalGb; };
struct PcFan { char name[20]; int rpm; };
struct PcTemp { char name[20]; float value; };
struct PcDrive { char name[28]; float temp, life; };   // disco fisico (life < 0 = sconosciuta)
struct PcStats {
  bool  ok;
  bool  extended;          // dati estesi (nomi, rete, dischi, uptime...)
  float cpu, cpuTemp;      // % e °C (temperatura 0 se LibreHardwareMonitor non gira)
  float cpuMhz, cpuPower;
  float ram;               // %
  float ramUsedMb, ramTotalMb;
  float gpu, gpuTemp, gpuPower;
  float vramUsedMb, vramTotalMb;
  float disk;              // % usato di C:
  float netDown, netUp;    // byte/s
  float diskRead, diskWrite;
  uint32_t uptime;         // secondi
  int   claude;            // sessioni di Claude Code aperte (-1 = sconosciuto)
  char  cpuName[48], gpuName[48];
  int   diskN, fanN;
  PcDisk disks[PC_MAX_DISKS];
  PcFan  fans[PC_MAX_FANS];
  // dettagli e sensori (LibreHardwareMonitor)
  float cpuCoreMax, cpuVolt;
  float gpuHotspot, gpuMemTemp, gpuClockMhz, gpuMemLoad;
  int   boardN, ramTempN, driveN;
  PcTemp board[6];
  float  ramTemps[4];
  PcDrive drives[4];
  // ultimo evento degli hook di Claude Code (id 0 = nessuno)
  long  ccId;
  char  ccEv[8];           // busy, done, perm, ask
  char  ccProj[28];
  int   ccDur, ccAge;      // secondi (ccDur -1 = sconosciuta)
  int   ccBusy;            // sessioni al lavoro
  int   actMin;            // minuti di uso continuo del PC (-1 = PC Monitor vecchio)
  // media in riproduzione (PC Monitor 1.8): stato 0 niente, 1 in riproduzione, 2 in pausa
  int   mediaSt, mediaPos, mediaDur, mediaCv, mediaCtl;
  uint32_t mediaId;
  char  mediaTitle[64], mediaArtist[48], mediaApp[20];
  // prossimi eventi del calendario (letti dal PC Monitor dal link iCal); calN < 0 = calendario non impostato
  int   calN;
  struct { uint32_t s, e; char title[64]; } cal[3];
};

bool fetchWeather(float lat, float lon, WeatherData& out);
bool fetchPcStats(const char* host, PcStats& out);
// avviso al PC (Ritmo Code PC Monitor suona e mostra una notifica di Windows)
bool postPcNotify(const char* host, const char* ev, const char* title, const char* msg);

// aggiornamento del firmware dalle release di GitHub (GITHUB_REPO in config.h)
bool fetchLatestRelease(char* tag, size_t tagSz, char* url, size_t urlSz);
bool installFromUrl(const char* url, void (*progress)(int pct), String& err);

// eventi del calendario per le viste giorno/settimana/mese (GET http://<pc>/calendar.json)
struct CalItem { uint32_t s, e; uint8_t allday; char title[64]; };
int fetchCalRange(const char* host, CalItem* out, int max);   // voci lette, -1 errore
// pagina media: comando al PC (toggle, next, prev, seek, volup, voldown, mute) e copertina RGB565
bool postPcMedia(const char* host, const char* action, int sec);
bool fetchPcCover(const char* host, uint8_t* buf, size_t len, uint32_t* id);
