#!/usr/bin/env node
/*
 * capture_screens.js — rigenera le immagini del README (assets/screen-*.png)
 * dal simulatore e dal pannello web, con Chrome in modalita' headless.
 *
 * Uso:
 *   python simulator/serve.py            # in un altro terminale (http://127.0.0.1:8480/simulator/)
 *   npm i puppeteer-core                 # una volta, dove preferisci
 *   node tools/capture_screens.js        # CHROME=/percorso/chrome per un browser diverso
 *
 * Il pannello web viene servito con dati di esempio (nessun dispositivo necessario).
 */
const fs = require('fs');
const path = require('path');
const puppeteer = require('puppeteer-core');

const ROOT = path.resolve(__dirname, '..');
const OUT = path.join(ROOT, 'assets');
const SIM = process.env.SIM_URL || 'http://127.0.0.1:8480/simulator/';
const CHROME = process.env.CHROME || [
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome',
].find((p) => fs.existsSync(p));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function simShots(browser) {
  const page = await browser.newPage();
  await page.setViewport({ width: 1400, height: 900, deviceScaleFactor: 2 });
  await page.goto(SIM, { waitUntil: 'networkidle0' });
  await page.evaluate(() => document.fonts.ready);
  // dati di esempio realistici, poi dritti al dashboard (salta il PIN)
  await page.evaluate(() => {
    document.documentElement.style.setProperty('--zoom', 1);
    Object.assign(__sim.API, { h5: 62, d7: 38, r5min: 104, r7h: 101, ust: 'allowed' });
    __sim.API.probe = [{ code: 200, ms: 850 }, { code: 200, ms: 1300 }, { code: 200, ms: 1900 }, { code: 429, ms: 700 }];
    __sim.boot(true);
  });
  await sleep(2600);
  await page.evaluate(() => __sim.requestState(__sim.ST.LOADING));
  await sleep(4000);
  await page.evaluate(() => { __sim.API.probe.forEach((p, i) => { __sim.G.models[i].pr = { ...p }; }); __sim.requestState(__sim.ST.MAIN); });
  await sleep(1200);

  const shot = async (name) => {
    const el = await page.$('.screen-wrap');
    await el.screenshot({ path: path.join(OUT, `screen-${name}.png`) });
    console.log('ok', name);
  };
  const names = ['home', 'usage', 'models', 'window', 'rhythm', 'weeks', 'pc'];
  for (let i = 0; i < names.length; i++) {
    await page.evaluate((k) => { __sim.setTile(k, false); __sim.refreshUiValues(); __sim.dashTick(); }, i);
    await sleep(700);
    await shot(names[i]);
  }
  await page.evaluate(() => { __sim.setTile(1, false); __sim.G.pendPeak = 91; __sim.showMoment(0, 0); });
  await sleep(1600);
  await shot('reset');
  await page.evaluate(() => { __sim.momentClose(); __sim.showMoment(0, 70); });
  await sleep(1500);
  await shot('alert');
  await page.evaluate(() => { __sim.momentClose(); __sim.pauseMenuOpen(); });
  await sleep(700);
  await shot('pause');
  await page.evaluate(() => { __sim.pauseMenuClose(); __sim.requestState(__sim.ST.SETTINGS); });
  await sleep(800);
  await shot('settings');
  await page.evaluate(() => __sim.requestState(__sim.ST.ABOUT));
  await sleep(900);
  await shot('info');
  await page.evaluate(() => { __sim.requestState(__sim.ST.MAIN); });
  await sleep(900);
  // orologio notturno: fuso scelto perche' nel simulatore siano le 23, 40 s senza tocchi
  await page.evaluate(() => {
    const utc = new Date().getUTCHours();
    let tz = (23 - utc + 24) % 24; if (tz > 12) tz -= 24;
    Object.assign(__sim.P, { tz, night: 1, nightclk: true, dim: 0 });
    __sim.G.lastTouch = performance.now() - 40000;
  });
  await sleep(1500);
  await page.evaluate(() => { document.querySelectorAll('[style*="brightness"]').forEach((e) => { e.style.filter = 'none'; }); });
  await shot('night');
  await page.close();
}

async function webShot(browser) {
  const src = fs.readFileSync(path.join(ROOT, 'firmware/claude_stick/status_page.h'), 'utf8');
  const html = src.slice(src.indexOf('R"HTML(') + 7, src.indexOf(')HTML"'));
  const now = Math.floor(Date.now() / 1000);
  const hist = [];
  for (let i = 0; i < 140; i++) {
    const t = now - (140 - i) * 600;
    hist.push([t, Math.round(Math.min(100, (i % 30) * 2.6)), Math.round(22 + i * 0.12)]);
  }
  const wave = (peak, shift) => Array.from({ length: 24 }, (_, h) =>
    +(Math.max(0, peak * Math.exp(-((h - 11 - shift) ** 2) / 10) + peak * 0.7 * Math.exp(-((h - 16) ** 2) / 8))).toFixed(1));
  const PCD = { ok: true, age: 400, int: 1, kwh: 0.3, host: '192.168.1.10:8765', cpu_name: 'AMD Ryzen 7 9700X 8-Core Processor',
    gpu_name: 'NVIDIA GeForce RTX 3070 Ti', cpu: 12, cpu_t: 56, cpu_mhz: 4730, cpu_w: 47, core_max: 33, volt: 1.18, gpu: 4, gpu_t: 35,
    gpu_w: 11, gpu_hot: 45, gpu_mem_t: 40, gpu_mhz: 210, gpu_mem_load: 12, ram: 61, ram_used_gb: 18.4, ram_total_gb: 31.2,
    vram_used_gb: 1.0, vram_total_gb: 8, disk: 57, net_down: 2.4 * 1048576, net_up: 180 * 1024, disk_r: 235 * 1024, disk_w: 492 * 1024,
    uptime: 103639, claude: 7, disks: [['C', 57, 199, 465], ['D', 15, 398, 466]], fans: [['gpu fan 1', 0]], board: [], ram_t: [38.5, 38.3],
    drives: [['Samsung SSD 970 EVO Plus 500GB', 47, 92]],
    hist: [0, 1, 2].map((k) => Array.from({ length: 240 }, (_, i) => Math.round([10, 6, 60][k] + [8, 5, 3][k] * Math.sin(i / [9, 13, 40][k])))) };
  const status = {
    fw: '3.6', now, ok: true, h5: 62, d7: 38, h5_reset: now + 6240, d7_reset: now + 101 * 3600,
    updated: now - 95, refreshing: false, night: false, status: 'allowed', account: 'Studio',
    models: [
      { name: 'Haiku', id: 'claude-haiku-4-5-20251001', code: 200, ms: 850, up: true, mood: 1, age: 540 },
      { name: 'Sonnet', id: 'claude-sonnet-5', code: 200, ms: 1300, up: true, mood: 1, age: 1140 },
      { name: 'Opus', id: 'claude-opus-5', code: 200, ms: 1900, up: true, mood: 1, age: 1740 },
      { name: 'Fable', id: 'claude-fable-5-1', code: 429, ms: 700, up: true, mood: 2, age: 240 },
    ],
    hist, heat: [wave(4, 0), wave(22, 1), wave(70, 0), wave(160, -1)], heat_mode: 1, pause_until: 0,
    wx: { ok: true, city: 'Milano', temp: 22.3, code: 2, day: true, tmax: 22.9, tmin: 18.3, tmax2: 22.3, tmin2: 18.1, code2: 80,
      sunrise: '07:03', sunset: '19:30', rain_h0: 13, rain: [0, 3, 3, 3, 3, 5, 18, 45, 55, 30, 23, 25] },
    pc: PCD,
    weeks: [48, 71, 55, 93, 62, 80, 58, 38].map((p, i) => [now + 101 * 3600 - (7 - i) * 604800, p]),
  };
  const page = await browser.newPage();
  await page.setViewport({ width: 900, height: 900, deviceScaleFactor: 2 });
  await page.setRequestInterception(true);
  page.on('request', (req) => {
    const u = req.url();
    if (u === 'http://ritmo-code.test/') return req.respond({ contentType: 'text/html; charset=utf-8', body: html });
    if (u.startsWith('http://ritmo-code.test/api/status')) return req.respond({ contentType: 'application/json', body: JSON.stringify(status) });
    if (u.startsWith('http://ritmo-code.test/api/pc')) return req.respond({ contentType: 'application/json', body: JSON.stringify(PCD) });
    return req.continue();
  });
  await page.goto('http://ritmo-code.test/', { waitUntil: 'networkidle0' });
  await page.evaluate(() => { try { localStorage.clear(); } catch (e) {} return document.fonts.ready; });
  await sleep(1200);
  await page.screenshot({ path: path.join(OUT, 'screen-web.png'), fullPage: true });
  console.log('ok web');
  await page.evaluate(() => document.querySelector('.tabs button[data-t="pc"]').click());
  await sleep(1200);
  await page.screenshot({ path: path.join(OUT, 'screen-web-pc.png'), fullPage: true });
  console.log('ok web-pc');
  await page.close();
}

(async () => {
  if (!CHROME) throw new Error('Chrome non trovato: imposta CHROME=/percorso/chrome');
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: true });
  try {
    await simShots(browser);
    await webShot(browser);
  } finally {
    await browser.close();
  }
})().catch((e) => { console.error(e); process.exit(1); });
