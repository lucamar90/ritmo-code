#include "extras.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "certs.h"

// ---- mini parser JSON: basta per le risposte piccole e note di Open-Meteo e Ritmo Code PC Monitor ----
// cerca "key" a partire da `from` e restituisce la posizione subito dopo i due punti (-1 se manca)
static int jkey(const String& s, const char* key, int from = 0) {
  String k = String("\"") + key + "\"";
  int i = s.indexOf(k, from);
  if (i < 0) return -1;
  i = s.indexOf(':', i + k.length());
  return i < 0 ? -1 : i + 1;
}
static float jnum(const String& s, const char* key, int from = 0, float def = 0) {
  int i = jkey(s, key, from);
  return i < 0 ? def : s.substring(i).toFloat();
}
// n-esimo elemento di un array: numero
static float jarr(const String& s, const char* key, int n, int from = 0, float def = 0) {
  int i = jkey(s, key, from);
  if (i < 0) return def;
  i = s.indexOf('[', i);
  if (i < 0) return def;
  int end = s.indexOf(']', i);                      // oltre la fine dell'array: valore mancante
  i++;
  for (int k = 0; k < n; k++) {
    i = s.indexOf(',', i);
    if (i < 0 || i > end) return def;
    i++;
  }
  return i >= end ? def : s.substring(i).toFloat();
}
// n-esimo elemento di un array di stringhe ISO: "2026-09-17T07:03" -> "07:03"
static void jarrTime(const String& s, const char* key, int n, int from, char* out) {
  out[0] = 0;
  int i = jkey(s, key, from);
  if (i < 0) return;
  i = s.indexOf('[', i);
  int open = -1;
  for (int k = 0; k <= n && i >= 0; k++) {
    open = s.indexOf('"', i + 1);                   // apertura della stringa k
    if (open < 0) return;
    i = s.indexOf('"', open + 1);                   // chiusura
  }
  if (open < 0 || i < 0) return;
  int t = s.indexOf('T', open);
  if (t < 0 || t > i) return;
  strlcpy(out, s.substring(t + 1, t + 6).c_str(), 6);
}

bool fetchWeather(float lat, float lon, WeatherData& out) {
  out.ok = false;
  WiFiClientSecure client;
  client.setCACert(CA_BUNDLE);           // open-meteo.com: catena Let's Encrypt -> ISRG Root X1
  HTTPClient http;
  char url[360];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day&hourly=precipitation_probability"
           "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset,weather_code,precipitation_probability_max"
           "&timezone=auto&forecast_days=7&forecast_hours=12", lat, lon);
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("[METEO] HTTP %d\n", code); return false; }
  String s = http.getString();
  http.end();

  int cur = s.indexOf("\"current\":");
  int hourly = s.indexOf("\"hourly\":");
  int daily = s.indexOf("\"daily\":");
  if (cur < 0 || hourly < 0 || daily < 0) return false;

  out.temp  = jnum(s, "temperature_2m", cur);
  out.code  = (int)jnum(s, "weather_code", cur);
  out.isDay = jnum(s, "is_day", cur, 1) > 0.5f;

  // ora della prima voce oraria ("2026-09-17T13:00")
  char h0[6]; jarrTime(s, "time", 0, hourly, h0);
  out.rainHour0 = h0[0] ? atoi(h0) : 0;
  for (int i = 0; i < 12; i++) {
    float p = jarr(s, "precipitation_probability", i, hourly, 0);
    out.rain[i] = (uint8_t)(p < 0 ? 0 : (p > 100 ? 100 : p));
  }
  out.days = 0;
  for (int d = 0; d < 7; d++) {
    const float NONE = -999;
    float mx = jarr(s, "temperature_2m_max", d, daily, NONE);
    if (mx == NONE) break;
    out.dmax[d]  = mx;
    out.dmin[d]  = jarr(s, "temperature_2m_min", d, daily);
    out.dcode[d] = (int)jarr(s, "weather_code", d, daily);
    float p = jarr(s, "precipitation_probability_max", d, daily);
    out.drain[d] = (uint8_t)(p < 0 ? 0 : (p > 100 ? 100 : p));
    out.days = d + 1;
  }
  out.tmax  = out.dmax[0];
  out.tmin  = out.dmin[0];
  out.tmax2 = out.dmax[1];
  out.tmin2 = out.dmin[1];
  out.code2 = out.dcode[1];
  jarrTime(s, "sunrise", 0, daily, out.sunrise);
  jarrTime(s, "sunset", 0, daily, out.sunset);
  out.ok = true;
  Serial.printf("[METEO] %.1f C, codice %d, max %.0f min %.0f\n", out.temp, out.code, out.tmax, out.tmin);
  return true;
}

// valore stringa di "key" (senza escape: bastano nomi di hardware)
static void jstrv(const String& s, const char* key, char* out, size_t sz, int from = 0) {
  out[0] = 0;
  int i = jkey(s, key, from);
  if (i < 0) return;
  int a = s.indexOf('"', i), b = a < 0 ? -1 : s.indexOf('"', a + 1);
  if (a < 0 || b < 0) return;
  strlcpy(out, s.substring(a + 1, b).c_str(), sz);
}

bool fetchPcStats(const char* host, PcStats& out) {
  out.ok = false;
  if (!host || !host[0]) return false;
  WiFiClient client;
  HTTPClient http;
  String url = String("http://") + host + "/data.json";
  if (!http.begin(client, url)) return false;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  uint32_t t0 = millis();
  int code = http.GET();
  static int lastCode = 200;
  if (code != lastCode) {                                  // log solo ai cambi di stato
    Serial.printf("[PC] %s -> %d (%s) in %lu ms\n", url.c_str(), code, code > 0 ? "http" : http.errorToString(code).c_str(),
                  (unsigned long)(millis() - t0));
    lastCode = code;
  }
  if (code != 200) { http.end(); return false; }
  String s = http.getString();
  http.end();
  if (s.indexOf("\"cpu_load\"") < 0) return false;
  out.cpu     = jnum(s, "cpu_load");
  out.cpuTemp = jnum(s, "cpu_temp");
  out.ram     = jnum(s, "ram_load");
  out.gpu     = jnum(s, "gpu_load");
  out.gpuTemp = jnum(s, "gpu_temp");
  out.disk    = jnum(s, "disk_used_pct");
  out.cpuMhz  = jnum(s, "cpu_mhz");
  out.cpuPower = jnum(s, "cpu_power");
  out.ramUsedMb = jnum(s, "ram_used_mb");
  // dati estesi
  out.extended = s.indexOf("\"cpu_name\"") >= 0;
  jstrv(s, "cpu_name", out.cpuName, sizeof(out.cpuName));
  jstrv(s, "gpu_name", out.gpuName, sizeof(out.gpuName));
  out.ramTotalMb  = jnum(s, "ram_total_mb");
  out.gpuPower    = jnum(s, "gpu_power");
  out.vramUsedMb  = jnum(s, "vram_used_mb");
  out.vramTotalMb = jnum(s, "vram_total_mb");
  out.netDown     = jnum(s, "net_down_bps");
  out.netUp       = jnum(s, "net_up_bps");
  out.diskRead    = jnum(s, "disk_read_bps");
  out.diskWrite   = jnum(s, "disk_write_bps");
  out.uptime      = (uint32_t)jnum(s, "uptime_s");
  out.claude      = (int)jnum(s, "claude_sessions", 0, -1);
  out.diskN = 0;
  int arr = s.indexOf("\"disks\"");
  int end = arr < 0 ? -1 : s.indexOf(']', arr);
  for (int p = arr; p >= 0 && out.diskN < PC_MAX_DISKS; ) {
    p = s.indexOf("\"d\":", p + 1);
    if (p < 0 || p > end) break;
    PcDisk &d = out.disks[out.diskN++];
    char l[4]; jstrv(s, "d", l, sizeof(l), p - 1);
    d.letter = l[0];
    d.used = jnum(s, "used_pct", p);
    d.freeGb = jnum(s, "free_gb", p);
    d.totalGb = jnum(s, "total_gb", p);
  }
  out.fanN = 0;
  arr = s.indexOf("\"fans\"");
  end = arr < 0 ? -1 : s.indexOf(']', arr);
  for (int p = arr; p >= 0 && out.fanN < PC_MAX_FANS; ) {
    p = s.indexOf("\"name\":", p + 1);
    if (p < 0 || p > end) break;
    PcFan &f = out.fans[out.fanN++];
    jstrv(s, "name", f.name, sizeof(f.name), p - 1);
    f.rpm = (int)jnum(s, "rpm", p);
  }
  // dettagli CPU/GPU e sensori di scheda madre, RAM e dischi
  out.cpuCoreMax  = jnum(s, "cpu_core_max", 0, -1);
  out.cpuVolt     = jnum(s, "cpu_voltage");
  out.gpuHotspot  = jnum(s, "gpu_hotspot");
  out.gpuMemTemp  = jnum(s, "gpu_mem_temp");
  out.gpuClockMhz = jnum(s, "gpu_clock_mhz");
  out.gpuMemLoad  = jnum(s, "gpu_mem_load", 0, -1);
  out.boardN = 0;
  arr = s.indexOf("\"board_temps\"");
  end = arr < 0 ? -1 : s.indexOf(']', arr);
  for (int p = arr; p >= 0 && out.boardN < 6; ) {
    p = s.indexOf("\"name\":", p + 1);
    if (p < 0 || p > end) break;
    PcTemp &b = out.board[out.boardN++];
    jstrv(s, "name", b.name, sizeof(b.name), p - 1);
    b.value = jnum(s, "value", p);
  }
  out.ramTempN = 0;
  arr = s.indexOf("\"ram_temps\"");
  if (arr >= 0) {
    int a0 = s.indexOf('[', arr), a1 = s.indexOf(']', arr);
    for (int p = a0; p >= 0 && p < a1 && out.ramTempN < 4; p = s.indexOf(',', p + 1)) {
      if (p + 1 >= a1) break;
      out.ramTemps[out.ramTempN++] = s.substring(p + 1).toFloat();
    }
  }
  out.driveN = 0;
  arr = s.indexOf("\"storage\"");
  end = arr < 0 ? -1 : s.indexOf(']', arr);
  for (int p = arr; p >= 0 && out.driveN < 4; ) {
    p = s.indexOf("\"name\":", p + 1);
    if (p < 0 || p > end) break;
    PcDrive &d = out.drives[out.driveN++];
    jstrv(s, "name", d.name, sizeof(d.name), p - 1);
    int obj = s.indexOf('}', p);
    int lp = s.indexOf("\"life\":", p);
    d.temp = jnum(s, "temp", p);
    d.life = (lp < 0 || lp > obj || s.substring(lp + 7).startsWith("null")) ? -1 : jnum(s, "life", p);
  }
  // Claude Code (hook): l'id supera la precisione di un float, si legge come intero
  int ci = jkey(s, "cc_ev_id");
  out.ccId = ci < 0 ? 0 : s.substring(ci).toInt();
  jstrv(s, "cc_ev", out.ccEv, sizeof(out.ccEv));
  jstrv(s, "cc_ev_proj", out.ccProj, sizeof(out.ccProj));
  out.ccDur  = (int)jnum(s, "cc_ev_dur", 0, -1);
  out.ccAge  = (int)jnum(s, "cc_ev_age", 0, 9999);
  out.ccBusy = (int)jnum(s, "cc_busy");
  out.ok = true;
  return true;
}

static String urlenc(const char* s) {
  String o;
  static const char* H = "0123456789ABCDEF";
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
    else { o += '%'; o += H[c >> 4]; o += H[c & 15]; }
  }
  return o;
}

bool postPcNotify(const char* host, const char* ev, const char* title, const char* msg) {
  if (!host || !host[0]) return false;
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, String("http://") + host + "/notify")) return false;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "ev=" + urlenc(ev) + "&title=" + urlenc(title) + "&msg=" + urlenc(msg);
  int code = http.POST(body);
  http.end();
  Serial.printf("[PC] avviso %s -> %d\n", ev, code);
  return code >= 200 && code < 300;
}
