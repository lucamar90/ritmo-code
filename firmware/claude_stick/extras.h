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
};

bool fetchWeather(float lat, float lon, WeatherData& out);
bool fetchPcStats(const char* host, PcStats& out);
