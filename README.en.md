<div align="center">

<img src="assets/brand/claudecode-color.svg" width="76" alt="Clawd">

# Ritmo Code

**Your Claude Code usage limits, live on a 3.5" touch screen.**<br>
No computer running, no app, no cloud.

[Italiano](README.md) · **English**

<img src="https://img.shields.io/badge/firmware-v3.7-D97757?style=for-the-badge" alt="firmware v3.7">
<img src="https://img.shields.io/badge/ESP32--S3-AXS15231B%20480×320-1B1A18?style=for-the-badge" alt="ESP32-S3 AXS15231B">
<img src="https://img.shields.io/badge/LVGL-9.2.2-9BC08A?style=for-the-badge" alt="LVGL 9.2.2">
<img src="https://img.shields.io/badge/accounts-up%20to%204-8E8B82?style=for-the-badge" alt="up to 4 accounts">

<img src="assets/screen-home.png" width="560" alt="Ritmo Code: home with clock, weather, Claude summary and PC">

</div>

Ritmo Code is a small desk screen that shows how much of your Claude subscription's **5-hour** and
**weekly** windows you have used, when they reset, whether your weekly pace is sustainable, and
whether the models are responding. The device talks to Anthropic's API directly over Wi-Fi and reads
the numbers from the response headers.

It also works as a desk dashboard: clock and weather and, with the
[**Ritmo Code PC Monitor**](#ritmo-code-pc-monitor) app, full statistics of your PC.

The interface follows the Claude Code terminal look, in **Italian** or English, with 100% touch
navigation (swipe ← → between pages, no physical button).

> Screenshots below show the Italian UI, the device default. Switch to English in *Impostazioni →
> lingua*.

---

## Screens

| | |
|---|---|
| <img src="assets/screen-home.png" alt="Home"> **Home** — clock, date, weather (Open-Meteo), 5-hour and weekly summary, model status and a line for the connected PC. The device returns to it after a few minutes without touches. | <img src="assets/screen-weeks.png" alt="Weeks"> **Weeks** — peak reached in each of the last 8 weeks, with average and maximum. |
| <img src="assets/screen-usage.png" alt="Now"> **Now** — percentage and block meter for the 5-hour and weekly windows, reset countdown and time, overall status and **weekly pace** (with a tick showing where you "should" be). | <img src="assets/screen-models.png" alt="Models"> **Models** — Clawd reacts to the state; a real probe for Haiku, Sonnet, Opus and Fable with result and latency, plus incidents from status.claude.com. |
| <img src="assets/screen-window.png" alt="5-hour window"> **5-hour window** — usage in the current window and a dotted projection: tells you whether, at this pace, you run out before the reset. | <img src="assets/screen-rhythm.png" alt="Rhythm"> **Rhythm** — quota burned per hour of day, filtered by **today · 7d · 30d · all** (per-day history stored on the device). |
| <img src="assets/screen-pc.png" alt="PC"> **PC** — CPU (load, GHz, temperature, watts, busiest core), GPU (VRAM and hot spot), RAM with module temperature, network, disk, graphs that advance with every reading (4 minutes at 1 s), uptime, open Claude Code sessions, power draw and daily cost, drive temperature and remaining life. | <img src="assets/screen-web-pc.png" alt="Web panel, pc page"> **Web panel · pc** — the device's pages in the browser, with every drive listed. |
| <img src="assets/screen-alert.png" alt="Threshold alert"> **Threshold alerts** — at 25, 50, 70 and 100% a full-screen animation with Clawd reacting to the level. | <img src="assets/screen-reset.png" alt="Reset alert"> **Reset alert** — when a window that went above 80% becomes available again. |
| <img src="assets/screen-claude.png" alt="Claude is done"> **Claude Code** — with the hooks installed by the PC Monitor, Clawd tells you when Claude has finished (and how long it took) or needs a permission. It stays until you tap it or write to Claude again. | <img src="assets/screen-pomodoro.png" alt="Pomodoro"> **Timer and pomodoro** — tap the clock on the home page: pomodoro 25/5 or a 5 to 30-minute timer. Countdown in the header, alert at the end of each phase, today's pomodoros. |
| <img src="assets/screen-night.png" alt="Night clock"> **Night clock** — at night, after 30 seconds without touches, a full-screen clock in warm grey with date, 5-hour and weekly usage and upcoming rain. Dim or very dim; alternatively the screen turns off. | <img src="assets/screen-info.png" alt="Info"> **Info** — firmware version, hardware and credits. |
| <img src="assets/screen-pause.png" alt="Timed pause"> **Pause requests** — the ❚❚ button in the header stops requests; a long press offers 30 min, 1 hour, until 7:00 or no limit. | <img src="assets/screen-settings.png" alt="Settings"> **Settings** — interval, language, timezone, alerts, night mode and night clock, home, pc interval, brightness, Wi-Fi networks and signal, accounts, models, firmware update. |

> Double-tap **✻ ritmo-code** at the top to preview every alert in sequence.

The images come from the [simulator](#simulator) and are regenerated with `node tools/capture_screens.js`.

### Web panel

<img src="assets/screen-web.png" width="560" align="right" alt="Ritmo Code web panel">

From any browser on the same network open **`http://<device-ip>/`**. The IP is under
*Settings → local network* (on Mac and iPhone `http://ritmo-code.local/` works too).

- Weather and connected PC in a box at the top.
- 5-hour and weekly windows with countdown, status, weekly pace and forecast.
- Same tabs as the device: **home · now · models · 5h · rhythm · weeks · pc**.
- Usage **history** filtered by **6h · 24h · all**.
- **Hourly rhythm** filtered by **today · 7d · 30d · all**.
- Probed models: ID, result, latency, last check.
- **Weekly peak** of the last 8 weeks.
- Links to **edit model IDs**, set **city, PC and energy price** (`/home`) and **update the firmware**.

It refreshes itself every 30 seconds from the device's memory (`/api/status`): no extra requests to
Anthropic, and the token is never exposed.

<br clear="right">

---

## Features

- **5-hour and weekly windows** with reset times, projection and overall result (`ok` · `warning` · `blocked`).
- **Weekly pace**: percentage used minus percentage of the week elapsed. Green when under pace, amber up to +15%, red beyond. Alternates with a **forecast**: "at reset you reach ~70%" or "at this pace ends thu 14:00".
- **Weekly history**: the peak of every week, stored on the device per account.
- Threshold **alerts** (25/50/70/100%) and a **reset** alert (can be turned off).
- **Timer and pomodoro**: tap the clock on the home page. Pomodoro 25/5 with a 15-minute long break after the fourth, or a 5, 10, 15 or 30-minute timer. The countdown sits in the header, Clawd announces the end of each phase full screen (at night too) and today's pomodoros are counted.
- **Claude Code alerts**: Clawd tells you when Claude has finished a task (with how long it took) or needs a permission, via hooks and the [PC Monitor](#ritmo-code-pc-monitor) app.
- **Models**: one probe per cycle, rotating. **Model IDs are editable** on the device or in the browser, so a renamed model needs no rebuild.
- **Night mode** during a time band (22, 23 or 00 → 07): a **full-screen clock**, dim or very dim, or the screen off, and by default **updates paused**. The first tap only wakes the screen, without pressing anything. **Dimming** after 1, 5 or 10 minutes without touches.
- **Pause requests** from the header button, indefinitely or for a set time; resumes by itself.
- **Home and weather**: clock, date and forecast for your city (Open-Meteo, free and keyless), refreshed every 30 minutes.
- **PC statistics** through the [Ritmo Code PC Monitor](#ritmo-code-pc-monitor) app, read every 1, 3, 5 seconds or 1 minute.
- **Up to 4 accounts**, encrypted under the same PIN; only the active one is polled.
- **Saved Wi-Fi networks**: up to 3, with a preferred network and "forget"; signal strength in dBm under Settings.
- **Firmware update over Wi-Fi**, protected by a code shown on the screen, with **automatic rollback** to the previous version if the new firmware fails to start.
- **Golden-ratio layout**: margins, columns, box heights and gaps follow φ and the Fibonacci scale (5 · 8 · 13 · 21 · 34 · 55 · 89), with symmetric padding on every page.
- Refresh **interval** from 30 s to 30 min, auto **slideshow**, **brightness**, **timezone** (Rome with automatic DST, or GMT±N).

---

## Ritmo Code PC Monitor

<img src="assets/screen-pc.png" width="400" align="right" alt="PC page">

A Windows app in [`ritmo-code-pc-monitor/`](ritmo-code-pc-monitor/) that sends the computer's
statistics to the device: CPU, GPU, RAM, network, drives, uptime, open Claude Code sessions and,
through the bundled [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor),
temperatures, power, fans and drive health.

Data stays on your home network: the device reads `http://<pc-ip>:8765/data.json`, which contains
no file, process or window names. Available sensors depend on the hardware: temperatures, power and
drive data appear only if LibreHardwareMonitor can read them on that PC.

<br clear="right">

### Installing on Windows

**You need:** Windows 10 or 11, an administrator account, and the PC and the device on the **same
network** (the device only uses **2.4 GHz** Wi-Fi; the PC can be wired or on the 5 GHz band of the
same router). Building the package also needs [Python 3.8+](https://www.python.org/downloads/).

The app's own interface is in Italian; the button names below are quoted as they appear.

**1. Get the package**

Quickest: download **`RitmoCodePcMonitor-…-windows.zip`** from the [latest release](https://github.com/lucamar90/ritmo-code/releases/latest)
and unzip it — it contains everything, no Python needed. To build it from source instead (on any Windows PC):

```bat
cd ritmo-code-pc-monitor
pip install pyinstaller
build_exe.bat
```

`build_exe.bat` builds two executables and downloads LibreHardwareMonitor 0.9.6 if missing. The
`dist\` folder then holds everything you need:

```
dist\
  RitmoCodePcMonitor-nascosto.exe   windowless, started with Windows (icon next to the clock)
  RitmoCodePcMonitor.exe            with a console window, for troubleshooting
  LibreHardwareMonitor\             sensors, web server on 127.0.0.1:8085 only
  installa.bat · disinstalla.bat · LEGGIMI.txt
```

**2. Install** on the PC you want to monitor

1. Copy the folder (`dist\` or the one unzipped from the release) to a permanent place, e.g. `C:\RitmoCodePcMonitor` (the autostart entries point
   there, so don't move it later).
2. Double-click **`installa.bat`** and accept the administrator prompt. The script:
   - adds the **"Ritmo Code PC Monitor"** firewall rule: TCP port 8765 open to the **local network only**;
   - registers LibreHardwareMonitor as an elevated logon task (needed for temperatures);
   - adds the app to *Startup* and starts it;
   - opens the pairing page.
3. If Windows SmartScreen says the app is unrecognized (the executables are not signed), choose
   **More info → Run anyway**.

**3. Pair the PC with the device**

1. The device must be on, unlocked with its PIN and on the same network.
2. The page **`http://127.0.0.1:8765/`** (it opens by itself, or double-click the tray icon) searches
   the local network for the device. If it isn't found, check the network and press **Cerca di
   nuovo** (search again), or set the PC address from the device's web panel (see below).
3. Enter the device **PIN** and press **Collega questo PC** (connect this PC). The app sends its
   address to the device; the PIN is used only for this step and is never stored.
4. Within a few seconds the device's **pc** page fills in. The reading rate is set on the device in
   *Settings → pc interval* (1 s, 3 s, 5 s, 1 min).

The PC address (`192.168.x.x:8765`) can also be set from the device's web panel, page `/home`.

**4. Tray icon**

The app sits next to the Windows clock with the Clawd icon:

- **hover** shows CPU, GPU, RAM and whether the device is reading the data;
- **double-click** opens the status and pairing page;
- **right-click** opens the device panel or quits the app (**Esci**).

**5. Claude Code alerts**

<img src="assets/screen-claude.png" width="400" align="right" alt="Alert: Claude is done">

The device can tell you when Claude Code **has finished** a task or **is waiting for your permission**,
even while you are looking at another window or away from the desk.

1. On **`http://127.0.0.1:8765/`**, box *avvisi di claude code*, press **Attiva gli avvisi**.
   The app adds three [hooks](https://docs.claude.com/en/docs/claude-code/hooks) to
   `~/.claude/settings.json` (`UserPromptSubmit`, `Stop`, `Notification`) and leaves the rest of the file
   alone; the first time it keeps a copy in `settings.json.ritmo-bak`.
2. Start a new Claude Code session: hooks apply to sessions started afterwards.

The hooks only call `http://127.0.0.1:8765`, and the app forwards the alert to the device, which accepts
it only from the paired PC. The project folder name is sent, nothing from the session itself. The alert
stays on screen until you tap it or write to Claude again (30 minutes at most); at night, with the screen
off or the night clock on, it does not show. In *Settings → claude code alerts* choose when to show it:
**always**, only for tasks **over 1 min** (default) or **over 5 min**, or **off**. Permission requests
always show unless it is *off*.

To remove them: **Disattiva** on the same page.

<br clear="right">

**Troubleshooting**

| Symptom | What to do |
|---|---|
| Device says *pc spento o Ritmo Code PC Monitor non attivo* (PC off or monitor not running) | Check the icon is next to the clock. Open `http://<pc-ip>:8765/data.json` from another device: if it doesn't answer, the firewall rule is missing (run `installa.bat` again) or the router isolates Wi-Fi clients. |
| The device loses the PC after a router restart | The PC's IP changed: pair again, or reserve the PC's address in the router (static DHCP). |
| The PC can't find the device | The device is on another network (e.g. guest or a different SSID): forget it in *Settings → wifi networks*. |
| Temperatures, watts or drives are empty | LibreHardwareMonitor must run as administrator: sign out and back in, or run `installa.bat` again. Some motherboard sensor chips are not supported. |
| *Port 8765 already in use* | Both the windowed and the hidden version are running: quit one (icon → **Esci**). |
| You already had LibreHardwareMonitor for another app | `installa.bat` stops it and uses the bundled one (one is enough); `disinstalla.bat` re-enables it. |

**Updating or uninstalling**

- To update: icon → **Esci**, replace the files in the folder with the new `dist\` and run
  `installa.bat` again.
- To remove everything: **`disinstalla.bat`** removes autostart, the logon task and the firewall
  rule; the folder can then be deleted.

From source the app also runs with `python ritmo_pc_monitor.py` (`--print` for a test reading,
`--port`, `--interval`, `--no-lhm`, `--no-tray`). Details on the data sent are in the
[app README](ritmo-code-pc-monitor/README.md) (Italian).

---

## Hardware

| | |
|---|---|
| Board | **Guition JC4832W535**, ESP32-S3 with a 3.5" capacitive touch IPS screen · 480×320 · **8 MB PSRAM** · **16 MB flash** |
| Chip | ESP32-S3 (native USB) |
| Display | **AXS15231B**, QSPI interface |
| Touch | **AXS15231B** capacitive, I²C `0x3B` |

> **OPI PSRAM is mandatory**: the graphics buffers do not fit in internal RAM.

Pins and the validated display/color/touch setup are in
[`firmware/RIFERIMENTO-HARDWARE-LVGL.md`](firmware/RIFERIMENTO-HARDWARE-LVGL.md); the reference
bring-up sketch is in [`firmware/bringup/`](firmware/bringup/).

**3D-printable case:** [`3D Case/`](3D%20Case/) holds a simple case (`Case_JC3248W535C.stl`) and
an articulated version with a display holder (`Articolato/`).

---

## How it works

The device sends a **minimal** request (`max_tokens: 1`) to
`https://api.anthropic.com/v1/messages`, **ignores the body** and reads usage from the headers:

```
anthropic-ratelimit-unified-status                allowed | allowed_warning | rejected
anthropic-ratelimit-unified-5h-utilization        0–1   (5-hour window)
anthropic-ratelimit-unified-5h-reset              epoch
anthropic-ratelimit-unified-7d-utilization        0–1   (weekly window)
anthropic-ratelimit-unified-7d-reset              epoch
anthropic-ratelimit-unified-representative-claim  five_hour | seven_day
anthropic-ratelimit-unified-fallback-percentage
anthropic-ratelimit-unified-overage-status / -overage-disabled-reason
```

Model health combines incidents from `status.claude.com` with a **per-model probe**: each cycle the
device tries the next model in the rotation and records the HTTP code and latency. The probe stays a
separate request, so refreshing usage costs no more.

Every request is a real (if minimal) API call: on a company account it is visible to admins like any
Claude Code usage. To send fewer, use a longer interval and the night pause.

### The token (`claude setup-token`)

In a terminal, with **Claude Code** installed and logged into your subscription (**Pro** or **Max**):

```bash
claude setup-token
```

An **OAuth** login opens in the browser and you get a **long-lived token** `sk-ant-oat01-…`. It is a
**Claude Code** token: a plain Messages API call with it is usually rejected. The device sends the
same headers as Claude Code (`anthropic-beta: oauth-2025-04-20` and its `User-Agent`), so the API
answers with the rate-limit headers. You enter the token **once** from the browser and it stays
**encrypted** on the device.

### ⚠️ Read this before using a subscription token

**Anthropic does not permit subscription OAuth tokens in third-party tools.** In a policy formalised
on **4 April 2026**, Anthropic stated that Free/Pro/Max OAuth (the credential `claude setup-token`
produces) is intended **only** for Claude Code and claude.ai, and that using it in other products
violates the Consumer Terms. Ritmo Code is a third-party tool, and the method above, which presents
itself as the Claude Code client, is exactly the pattern that policy addresses.

- **It works today**, but *working* is not *permitted*.
- The risk is on **your account**: authentication failures and account disruption have been reported.
- Anthropic may change the API or block the pattern at any time.

This project is not affiliated with Anthropic. **If you do not accept that risk on your own account,
do not use a subscription token with this firmware.**

Sources: [The Register](https://www.theregister.com/2026/02/20/anthropic_clarifies_ban_third_party_claude_access/) ·
[WinBuzzer](https://winbuzzer.com/2026/02/19/anthropic-bans-claude-subscription-oauth-in-third-party-apps-xcxwbn/)

---

## Build & flash

Prerequisites (tested versions):

- `arduino-cli` 1.4.x · core `esp32:esp32` **3.3.11**
- libraries: **GFX Library for Arduino** 1.6.5 · **lvgl** 9.2.2
- on Windows: **Git Bash** for the `.sh` scripts

```bash
cd firmware/claude_stick
./build.sh                    # compile (-O2) and export the .bin
./build.sh upload COM3        # compile and flash over USB (macOS: /dev/cu.usbmodemXXXX)
./build.sh monitor COM3       # serial log at 115200
./build.sh ota <ip> <code>    # compile and flash over Wi-Fi
```

FQBN: `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc,USBMode=hwcdc,FlashMode=qio`

### Updating over Wi-Fi

On the device open **Settings → update firmware**: it shows its address and a **6-digit code**
valid for 5 minutes (5 wrong attempts close the session). Then, from the same Wi-Fi:

- `./build.sh ota <ip> <code>`, or
- open `http://<ip>/update`, pick `build/esp32.esp32.esp32s3/claude_stick.ino.bin` and enter the code.

Token, PIN, accounts, history and settings are kept. The USB cable always works as a fallback.

### Build notes

`build.sh` passes `-DLV_CONF_INCLUDE_SIMPLE -I<sketch>` so LVGL finds the sketch's `lv_conf.h`. If
you get `lv_conf.h not found`, copy `firmware/claude_stick/lv_conf.h` into your Arduino `libraries`
folder (one level above `lvgl`), **this** file specifically: its `#include <stdint.h>` is wrapped in
`#ifndef __ASSEMBLY__`, without which assembling lvgl's `.S` files fails with
`Error: unknown opcode or format name 'typedef'`.

The bring-up sketch has its own script, `firmware/bringup/build.sh`, with a different FQBN.

The theme fonts (JetBrains Mono, plus DejaVu Sans Mono for symbols) are regenerated with
`tools/gen_fonts.sh`.

---

## First-time setup

Everything from the screen and the browser, no rebuild:

1. **Wi-Fi**: tap your network and type the password on the on-screen keyboard (up to 3 saved networks).
2. **Token**: the screen shows the device address (e.g. `http://192.168.1.42`). Open it from a PC or
   phone on the same network, name the account (e.g. *Studio*) and **paste the token**. The device
   **validates** it with a real call before accepting it.
3. **PIN**: choose a 4-digit PIN (entered twice); the token is encrypted with it.

On later boots only the **PIN** is needed. Accounts added later (*Settings → accounts*) use the same
form and the same PIN.

---

## Security

- Tokens are stored **encrypted** (AES-256-GCM, key derived from the PIN with SHA-256), one NVS slot
  per account, all under **the same PIN**. The PIN is **never stored**: a wrong PIN fails the GCM check.
- After **10 wrong attempts** the credentials are **wiped** and the device returns to setup (each
  failure doubles the lockout).
- The PIN stays in RAM for the session so switching accounts does not ask again; the active token is
  already decrypted in RAM anyway.
- History and hourly rhythm live in **LittleFS**, one file per account (no token).
- The **web panel** is readable without a password by anyone on your local network: it shows
  percentages, status, account name, weather and PC data, never the token. **Changes** from the
  browser (model IDs, city, PC, energy price) and **pairing the PC app** require the PIN (5 wrong
  attempts lock for 5 minutes); **firmware updates** require the code shown on the screen.
- The **Ritmo Code PC Monitor** app exposes only `/data.json` to the network (no file, process or
  window names); its status page and pairing answer only to the PC itself.
- `.env` and `.mcp.json` are in `.gitignore`: **no secrets go to git**.

---

## Simulator

A faithful copy of the interface runs in the browser, with a panel to simulate percentages, resets,
errors, models and time speed:

```bash
python simulator/serve.py     # then open http://127.0.0.1:8480/simulator/
```

To regenerate this README's images (needs `puppeteer-core` and Chrome):

```bash
node tools/capture_screens.js
```

---

## Repository layout

```
firmware/
  claude_stick/                 # the firmware (arduino-cli sketch)
    claude_stick.ino            # setup/loop, states, dashboard, screens, web server
    status_page.h               # web panel served on "/"
    api.cpp/.h                  # fetchUsage() and model probe
    extras.cpp/.h               # weather (Open-Meteo) and PC statistics
    ota_guard.cpp               # confirms the firmware after an update (rollback)
    status.cpp/.h               # incidents from status.claude.com
    crypto.cpp/.h               # AES-256-GCM + PIN-derived key
    accounts.cpp/.h             # account slots in NVS
    certs.cpp/.h                # CA bundle for HTTPS
    wifi_manager.h              # networks saved in NVS (up to 3)
    touch.h                     # AXS15231B touch driver
    font_jbm_*.c                # Terminal theme fonts (96 px for the home clock)
    logo_assets.h               # Clawd and symbols (from assets/brand)
    config.h                    # pins, endpoints, constants, version
    lv_conf.h                   # LVGL 9.2 config
    partitions.csv              # 16 MB: two app slots (OTA) + nvs + LittleFS
    build.sh                    # build / upload / monitor / ota
  bringup/                      # validated bring-up (hardware reference)
  RIFERIMENTO-HARDWARE-LVGL.md   # display, colors and touch that work
simulator/                      # web simulator of the interface
ritmo-code-pc-monitor/          # Windows app for PC statistics
tools/
  capture_screens.js            # README images from the simulator
  gen_fonts.sh                  # theme LVGL fonts
  gen_logo_assets.py            # brand SVGs -> logo_assets.h
assets/                         # screenshots and brand (brand/)
3D Case/                        # printable cases (STL)
flash.sh                        # build and flash, finding the port itself (macOS)
```

---

## Credits

**Ritmo Code** is developed by **Luca Marullo · [Innova Design Studio](https://innovadesignstudio.it)**
([info@innovadesignstudio.it](mailto:info@innovadesignstudio.it)).

It is based on [**claude-usage-stick**](https://github.com/benevid/claude-usage-stick-SVGL) by
**Benevid Felix**, itself derived from the original Claude Usage Stick by
[@oauramos](https://github.com/oauramos), with contributions from
[@jzimath-lab](https://github.com/jzimath-lab), [@renanravelli](https://github.com/renanravelli),
[@mpsd18](https://github.com/mpsd18) and [@ViniciusLoureiro67](https://github.com/ViniciusLoureiro67).
Reading usage from the headers, token encryption, multi-account support and the hardware groundwork
come from there; this version's interface, pages, features and web panel were rewritten.

Not an official Anthropic product. Claude and Claude Code are trademarks of Anthropic.
