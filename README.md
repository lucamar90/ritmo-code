<div align="center">

<img src="assets/brand/claudecode-color.svg" width="76" alt="Clawd">

# Ritmo Code

**I limiti d'uso di Claude Code, in tempo reale su uno schermo touch da 3,5".**<br>
Senza computer acceso, senza app, senza cloud.

**Italiano** · [English](README.en.md)

<img src="https://img.shields.io/badge/firmware-v3.9.9-D97757?style=for-the-badge" alt="firmware v3.9.9">
<img src="https://img.shields.io/badge/ESP32--S3-AXS15231B%20480×320-1B1A18?style=for-the-badge" alt="ESP32-S3 AXS15231B">
<img src="https://img.shields.io/badge/LVGL-9.2.2-9BC08A?style=for-the-badge" alt="LVGL 9.2.2">
<img src="https://img.shields.io/badge/account-fino%20a%204-8E8B82?style=for-the-badge" alt="fino a 4 account">

<img src="assets/screen-home.png" width="560" alt="Ritmo Code: home con ora, meteo, riepilogo Claude e PC">

</div>

Ritmo Code è un piccolo schermo da scrivania che mostra quanto hai consumato delle finestre
di **5 ore** e **settimanale** del tuo abbonamento Claude, quando si azzerano, se il ritmo della
settimana è sostenibile e se i modelli rispondono. Il dispositivo interroga direttamente la API
di Anthropic via Wi-Fi e legge i dati dagli header della risposta.

Sulla stessa scrivania fa anche da cruscotto: ora e meteo, e con l'app
[**Ritmo Code PC Monitor**](#ritmo-code-pc-monitor) le statistiche complete del tuo PC.

L'interfaccia è in stile terminale di Claude Code, in **italiano** o inglese, con navigazione
100% touch (scorri ← → tra le pagine, nessun tasto fisico).

---

## Schermate

| | |
|---|---|
| <img src="assets/screen-home.png" alt="Home"> **Home** — ora, data, meteo (Open-Meteo), riepilogo di 5h e settimana, stato dei modelli e riga del PC collegato. Ci torna da sola dopo qualche minuto senza tocchi. | <img src="assets/screen-weeks.png" alt="Settimane"> **Settimane** — picco raggiunto in ciascuna delle ultime 8 settimane, con media e massimo. |
| <img src="assets/screen-usage.png" alt="Ora"> **Ora** — percentuale e barra a blocchi di 5h e settimana, conto alla rovescia e orario del reset, stato complessivo e **ritmo settimanale** (con la tacca "dove dovresti essere"). | <img src="assets/screen-models.png" alt="Modelli"> **Modelli** — Clawd reagisce allo stato; per Haiku, Sonnet, Opus e Fable una sonda reale con esito e latenza, più gli incidenti da status.claude.com. |
| <img src="assets/screen-window.png" alt="Finestra 5h"> **Finestra 5h** — uso della finestra corrente e proiezione tratteggiata: ti dice se, a questo ritmo, finisci la quota prima del reset. | <img src="assets/screen-rhythm.png" alt="Ritmo"> **Ritmo** — quota consumata per ora del giorno, con filtro **oggi · 7g · 30g · tutto** (storico per giorno salvato sul dispositivo). |
| <img src="assets/screen-pc.png" alt="PC"> **PC** — CPU (carico, GHz, temperatura, watt, core più carico), GPU (VRAM e hot spot), RAM con temperatura dei moduli, rete, disco, grafici che avanzano a ogni lettura (4 minuti a 1 s), uptime, sessioni di Claude Code, consumo e costo al giorno, temperatura e vita residua dei dischi. | <img src="assets/screen-web-pc.png" alt="Pannello web, pagina pc"> **Pannello web · pc** — le stesse pagine del dispositivo nel browser, con i dischi uno per uno. |
| <img src="assets/screen-alert.png" alt="Avviso di soglia"> **Avvisi di soglia** — a 25, 50, 70 e 100% un'animazione a schermo intero con Clawd che reagisce al livello. | <img src="assets/screen-reset.png" alt="Avviso di reset"> **Avviso di reset** — quando una finestra che aveva superato l'80% torna disponibile. |
| <img src="assets/screen-claude.png" alt="Claude ha finito"> **Claude Code** — con gli hook installati dal PC Monitor, Clawd ti avvisa quando Claude ha finito (e dopo quanto) o aspetta un permesso. Resta finché non lo tocchi o riscrivi a Claude. | <img src="assets/screen-pomodoro.png" alt="Pomodoro"> **Timer e pomodoro** — tocca l'ora o il pomodoro nella home: pomodoro 25/5, 50/10 o 15/3, oppure timer da 5 a 30 minuti. Conto alla rovescia sotto l'ora, avviso a fine fase, pomodori di oggi. |
| <img src="assets/screen-night.png" alt="Orologio notturno"> **Orologio notturno** — di notte, dopo 30 secondi senza tocchi, l'ora a tutto schermo in grigio caldo con data, uso di 5h e settimana e pioggia in arrivo. Luminosità tenue o molto tenue; in alternativa lo schermo si spegne. | <img src="assets/screen-info.png" alt="Info"> **Info** — versione del firmware, hardware e crediti. |
| <img src="assets/screen-pause.png" alt="Pausa a tempo"> **Pausa richieste** — il tasto ❚❚ in testata ferma le richieste; tenuto premuto offre 30 min, 1 ora, fino alle 7:00 o senza limite. | <img src="assets/screen-settings.png" alt="Impostazioni"> **Impostazioni** — in cinque gruppi: **claude** (intervallo, account, modelli, token), **avvisi** (Claude Code, suoni sul pc, reset), **schermo** (luminosità, notte e orologio notturno, attenuazione, home, slideshow), **rete e pc** (Wi-Fi, indirizzo, intervallo pc) e **sistema** (lingua, fuso, aggiornamento firmware, info; *cancella tutto* chiede due tocchi e il PIN). |

> Doppio tocco su **✻ ritmo-code** in alto: riapre l'ultimo avviso (Claude, timer, pomodoro o soglia).

Le immagini arrivano dal [simulatore](#simulatore) e si rigenerano con `node tools/capture_screens.js`.

### Pannello web

<img src="assets/screen-web.png" width="560" align="right" alt="Pannello web di Ritmo Code">

Da qualsiasi browser sulla stessa rete apri **`http://<ip-del-dispositivo>/`**: l'IP è in
*Impostazioni → rete locale* (su Mac e iPhone funziona anche `http://ritmo-code.local/`).

- Meteo e PC collegato in un riquadro in alto.
- Finestra 5h e settimana con conto alla rovescia, stato, ritmo e previsione settimanale.
- Stesse schede del dispositivo: **home · ora · modelli · 5h · ritmo · settimane · pc**.
- **Andamento** dello storico con filtro **6h · 24h · tutto**.
- **Ritmo orario** con filtro **oggi · 7g · 30g · tutto**.
- Modelli sondati: ID, esito, latenza, ultimo controllo.
- **Picco settimanale** delle ultime 8 settimane.
- Collegamenti per **modificare gli ID dei modelli**, impostare **città, PC e prezzo dell'energia** (`/home`) e **aggiornare il firmware**.

Si aggiorna da solo ogni 30 secondi leggendo solo la memoria del dispositivo (`/api/status`):
nessuna richiesta in più ad Anthropic, e il token non viene mai esposto.

<br clear="right">

---

## Funzioni

- **Finestre 5h e settimanale** con reset, proiezione ed esito complessivo (`ok` · `attenzione` · `bloccato`).
- **Ritmo settimanale**: percentuale usata meno percentuale di settimana trascorsa. Verde se sei sotto ritmo, ambra fino a +15%, rosso oltre. Alternato alla **previsione**: "al reset arrivi al ~70%" oppure "a questo ritmo finisce gio 14:00".
- **Storico settimanale**: picco di ogni settimana, salvato sul dispositivo per account.
- **Avvisi** di soglia (25/50/70/100%) e di **reset** (disattivabile).
- **Timer e pomodoro**: tocca l'ora o la riga con il pomodoro nella home. Tre impostazioni pronte, **25/5**, **50/10** e **15/3** (focus/pausa in minuti, pausa lunga dopo il quarto), oppure timer da 5, 10, 15 o 30 minuti. Il conto alla rovescia scorre sotto l'ora e nella testata, a fine fase Clawd lo annuncia a schermo intero (anche di notte) e i pomodori di oggi restano contati. Si comanda anche dal PC (menu dell'icona) e, con *Impostazioni → avvisi → claude durante il focus → alla pausa*, gli avvisi di Claude aspettano la fine del focus.
- **Calendario**: incolla il link segreto iCal di Google Calendar (o Outlook) nel pannello web, pagina `/home`. Il PC Monitor scarica gli eventi ogni 5 minuti (anche quelli ricorrenti); sotto l'ora compare "tra 12 min: call cliente" o "in corso: … fino 16:00", e 5, 10 o 15 minuti prima arriva un avviso a schermo intero con suono sul PC (*Impostazioni → avvisi → avviso calendario*). Il link lo legge solo il PC collegato.
- **Avvisi di Claude Code**: Clawd ti dice quando Claude ha finito un lavoro (con la durata) o aspetta un permesso, tramite gli hook e l'app [PC Monitor](#ritmo-code-pc-monitor). Mentre Claude lavora la ✻ in testata gira come in Claude Code e il riquadro della home dice quante sessioni sono **al lavoro**.
- **Modelli**: una sonda per ciclo a rotazione. Gli **ID sono modificabili** dal dispositivo o dal browser: se Anthropic rinomina un modello non serve ricompilare.
- **Modalità notte** in una fascia oraria (22, 23 o 00 → 07): **orologio a tutto schermo** con luminosità tenue o molto tenue, oppure schermo spento, e di predefinito **aggiornamenti in pausa**. Il primo tocco riaccende soltanto, senza premere nulla. **Attenuazione** dopo 1, 5 o 10 minuti senza tocchi.
- **Pausa richieste** dal tasto in testata, senza limite o a tempo; riparte da sola.
- **Home e meteo**: ora, data e previsioni per la città scelta (Open-Meteo, gratuito e senza chiave), aggiornate ogni 30 minuti. Tocca il meteo per le **previsioni dei 7 giorni**.
- **Statistiche del PC** tramite l'app [Ritmo Code PC Monitor](#ritmo-code-pc-monitor), lette ogni 1, 3, 5 secondi o 1 minuto.
- **Fino a 4 account**, cifrati con lo stesso PIN: viene interrogato solo quello attivo.
- **Reti Wi-Fi salvate**: fino a 3, con rete preferita e "dimentica"; il segnale in dBm è nelle impostazioni.
- **Aggiornamento firmware con un tocco**: il dispositivo controlla da solo l'ultima release su GitHub (dopo l'avvio e una volta al giorno), la testata mostra *nuova v…* e in *Impostazioni → sistema → aggiornamenti* si installa con un tocco. Resta anche l'aggiornamento dal browser, protetto da un codice mostrato sullo schermo; in entrambi i casi c'è il **ritorno automatico** alla versione precedente se il nuovo firmware non parte.
- **Layout in sezione aurea**: margini, colonne, altezze dei riquadri e spazi seguono φ e la scala di Fibonacci (5 · 8 · 13 · 21 · 34 · 55 · 89), con padding simmetrici in ogni pagina.
- **Intervallo** di aggiornamento da 30 s a 30 min, **slideshow** automatico, **luminosità**, **fuso orario** (Roma con ora legale automatica, o GMT±N).

---

## Ritmo Code PC Monitor

<img src="assets/screen-pc.png" width="400" align="right" alt="Pagina pc">

App per Windows nella cartella [`ritmo-code-pc-monitor/`](ritmo-code-pc-monitor/) che invia al
dispositivo le statistiche del computer: CPU, GPU, RAM, rete, dischi, uptime, sessioni di Claude
Code aperte e, tramite [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
incluso nel pacchetto, temperature, consumi, ventole e salute dei dischi.

I dati restano sulla rete di casa: il dispositivo legge `http://<ip-del-pc>:8765/data.json`, che
non contiene nomi di file, processi o finestre. I sensori disponibili dipendono dall'hardware:
temperature, consumi e dischi compaiono solo se LibreHardwareMonitor riesce a leggerli su quel PC.

<br clear="right">

### Installazione su Windows

**Serve:** Windows 10 o 11, un account amministratore, il PC e il dispositivo sulla **stessa rete**
(il dispositivo usa solo il Wi-Fi a **2,4 GHz**; il PC può stare anche via cavo o sul 5 GHz dello
stesso router). Per creare il pacchetto serve anche [Python 3.8+](https://www.python.org/downloads/).

**1. Prendi il pacchetto**

Il modo più rapido: scarica **`RitmoCodePcMonitor-…-windows.zip`** dall'[ultima release](https://github.com/lucamar90/ritmo-code/releases/latest)
e scompattalo; contiene già tutto, non serve Python. Per crearlo da sorgente invece (su qualsiasi PC Windows):

```bat
cd ritmo-code-pc-monitor
pip install pyinstaller
build_exe.bat
```

`build_exe.bat` crea due eseguibili e, se manca, scarica LibreHardwareMonitor 0.9.6. Alla fine la
cartella `dist\` contiene tutto quello che serve:

```
dist\
  RitmoCodePcMonitor-nascosto.exe   senza finestra, avviato con Windows (icona vicino all'orologio)
  RitmoCodePcMonitor.exe            con finestra, per le verifiche
  LibreHardwareMonitor\             sensori, web server solo su 127.0.0.1:8085
  installa.bat · disinstalla.bat · LEGGIMI.txt
```

**2. Installa** sul PC da monitorare

1. Copia la cartella (`dist\` o quella scompattata dallo zip) in un posto fisso, per esempio `C:\RitmoCodePcMonitor` (gli avvii automatici
   puntano lì: non spostarla dopo).
2. Doppio clic su **`installa.bat`** e conferma i permessi di amministratore. Lo script:
   - crea la regola del firewall **"Ritmo Code PC Monitor"**: porta TCP 8765 aperta **solo alla rete locale**;
   - registra LibreHardwareMonitor come attività all'accesso con privilegi elevati (servono per le temperature);
   - mette l'app in *Esecuzione automatica* e la avvia;
   - apre la pagina di collegamento.
3. Se Windows SmartScreen avvisa che l'app non è riconosciuta (gli eseguibili non sono firmati),
   scegli **Ulteriori informazioni → Esegui comunque**.

**3. Collega il PC al dispositivo**

1. Il dispositivo deve essere acceso, sbloccato con il PIN e sulla stessa rete.
2. La pagina **`http://127.0.0.1:8765/`** (si apre da sola, oppure doppio clic sull'icona) cerca il
   dispositivo sulla rete locale. Se non lo trova, controlla la rete e premi **Cerca di nuovo**;
   in alternativa imposta l'indirizzo del PC dal pannello web del dispositivo (vedi sotto).
3. Inserisci il **PIN** del dispositivo e premi **Collega questo PC**. L'app comunica al dispositivo
   il suo indirizzo; il PIN serve solo per questa operazione e non viene salvato.
4. Dopo pochi secondi la pagina **pc** del dispositivo si riempie. La frequenza di lettura si sceglie
   sul dispositivo in *Impostazioni → intervallo pc* (1 s, 3 s, 5 s, 1 min).

In alternativa l'indirizzo del PC (`192.168.x.x:8765`) si imposta anche dal pannello web del
dispositivo, pagina `/home`.

**4. Icona nell'area di notifica**

L'app resta vicino all'orologio di Windows con l'icona di Clawd:

- **passandoci sopra** mostra CPU, GPU, RAM e se il dispositivo sta leggendo i dati;
- **doppio clic** apre la pagina di stato e collegamento;
- **clic destro** apre il pannello del dispositivo o chiude l'app (**Esci**);
- **clic destro → Timer e pomodoro** avvia sul dispositivo un pomodoro (25/5, 50/10, 15/3) o un timer, lo ferma o salta la fase; in cima mostra il timer in corso.

**5. Avvisi di Claude Code**

<img src="assets/screen-claude.png" width="400" align="right" alt="Avviso: Claude ha finito">

Il dispositivo può avvisarti quando Claude Code **ha finito** un lavoro o **aspetta un tuo permesso**,
anche se stai guardando un'altra finestra o ti sei alzato dalla scrivania.

1. Nella pagina **`http://127.0.0.1:8765/`**, riquadro *avvisi di claude code*, premi **Attiva gli avvisi**.
   L'app aggiunge tre [hook](https://docs.claude.com/en/docs/claude-code/hooks) a
   `~/.claude/settings.json` (`UserPromptSubmit`, `Stop`, `Notification`) senza toccare il resto del file;
   la prima volta ne salva una copia in `settings.json.ritmo-bak`.
2. Apri una nuova sessione di Claude Code: gli hook valgono per le sessioni avviate dopo.

Gli hook chiamano solo `http://127.0.0.1:8765` e l'app inoltra l'avviso al dispositivo, che lo accetta
solo dal PC collegato. Arriva il nome della cartella del progetto, niente del contenuto della sessione.
A schermo l'avviso si chiude da solo dopo 5, 10 o 30 secondi (*Impostazioni → chiudi avviso claude*, 30 s
predefinito) oppure resta finché non lo tocchi o scrivi di nuovo a Claude (*mai*); di
notte, con lo schermo spento o l'orologio notturno, non compare. In *Impostazioni → avvisi claude code*
scegli quando mostrarlo: **sempre**, solo per lavori **oltre 1 min** (predefinito) o **oltre 5 min**,
oppure **spento**. Le richieste di permesso compaiono sempre, tranne con *spento*.

Per toglierli: **Disattiva** nella stessa pagina.

**6. Suoni e notifiche sul PC**

Quando il dispositivo mostra un avviso (fine del timer, pausa e ripresa del pomodoro, Claude ha finito o
aspetta, soglie di utilizzo e reset) lo manda anche al PC collegato, che suona (un suono diverso per ogni
tipo) e mostra una notifica di Windows. Nella pagina `http://127.0.0.1:8765/`, riquadro *suoni e
notifiche*, scegli suono e notifica e il tipo di suono: **suoni di Windows** (predefiniti, seguono il volume
dei *Suoni di sistema* e si sentono anche in desktop remoto) o **melodie Ritmo Code** (con volume proprio);
**Prova** fa sentire com'è. Sul dispositivo si accende e
spegne da *Impostazioni → suoni sul pc*; di notte resta muto. Il PC accetta gli avvisi solo dal
dispositivo collegato.

<br clear="right">

**Problemi comuni**

| Sintomo | Cosa fare |
|---|---|
| Sul dispositivo: *pc spento o Ritmo Code PC Monitor non attivo* | Controlla che l'icona sia vicino all'orologio. Apri `http://<ip-del-pc>:8765/data.json` da un altro dispositivo: se non risponde, la regola del firewall manca (rilancia `installa.bat`) o il router isola i client Wi-Fi. |
| Il dispositivo non trova il PC dopo un riavvio del router | L'IP del PC è cambiato: rifai **Collega questo PC**, oppure riserva l'indirizzo del PC nel router (DHCP statico). |
| Il PC non trova il dispositivo | Il dispositivo è su un'altra rete (per esempio l'ospite o un altro SSID): in *Impostazioni → reti wifi* dimentica quella sbagliata. |
| Temperature, watt o dischi vuoti | LibreHardwareMonitor deve girare come amministratore: esci e rientra in Windows, o rilancia `installa.bat`. Alcuni chip della scheda madre non sono supportati. |
| *Porta 8765 già in uso* | Sono aperte insieme la versione con finestra e quella nascosta: chiudine una (icona → **Esci**). |
| Avevi già LibreHardwareMonitor per un'altra app | `installa.bat` lo ferma e usa il suo (ne basta uno); `disinstalla.bat` lo riattiva. |

**Aggiornare o disinstallare**

- Per aggiornare: icona → **Esci**, sostituisci i file nella cartella con quelli del nuovo `dist\`
  e rilancia `installa.bat`.
- Per rimuovere tutto: **`disinstalla.bat`** toglie avvio automatico, attività e regola del firewall;
  poi la cartella si può cancellare.

Da sorgente l'app si avvia anche con `python ritmo_pc_monitor.py` (`--print` per una lettura di
prova, `--port`, `--interval`, `--no-lhm`, `--no-tray`). Dettagli sui dati inviati nel
[README dell'app](ritmo-code-pc-monitor/README.md).

---

## Hardware

| | |
|---|---|
| Scheda | **Guition JC4832W535**, ESP32-S3 con schermo IPS touch capacitivo da 3,5" · 480×320 · **8 MB PSRAM** · **16 MB flash** |
| Chip | ESP32-S3 (USB nativa) |
| Display | **AXS15231B**, interfaccia QSPI |
| Touch | **AXS15231B** capacitivo, I²C `0x3B` |

> **La PSRAM OPI è obbligatoria**: i buffer grafici non entrano nella RAM interna.

Pin e configurazione validata di display, colori e touch sono in
[`firmware/RIFERIMENTO-HARDWARE-LVGL.md`](firmware/RIFERIMENTO-HARDWARE-LVGL.md); lo sketch di
bring-up di riferimento è in [`firmware/bringup/`](firmware/bringup/).

**Case stampabile in 3D:** [`3D Case/`](3D%20Case/) contiene un case semplice
(`Case_JC3248W535C.stl`) e una versione articolata con supporto per il display (`Articolato/`).

---

## Come funziona

Il dispositivo invia una richiesta **minima** (`max_tokens: 1`) a
`https://api.anthropic.com/v1/messages`, **ignora il corpo** della risposta e legge l'uso dagli header:

```
anthropic-ratelimit-unified-status                allowed | allowed_warning | rejected
anthropic-ratelimit-unified-5h-utilization        0–1   (finestra 5 ore)
anthropic-ratelimit-unified-5h-reset              epoch
anthropic-ratelimit-unified-7d-utilization        0–1   (finestra settimanale)
anthropic-ratelimit-unified-7d-reset              epoch
anthropic-ratelimit-unified-representative-claim  five_hour | seven_day
anthropic-ratelimit-unified-fallback-percentage
anthropic-ratelimit-unified-overage-status / -overage-disabled-reason
```

Lo stato dei modelli combina gli incidenti di `status.claude.com` con una **sonda per modello**:
a ogni ciclo il dispositivo prova il modello successivo della rotazione e registra codice HTTP e
latenza. La sonda resta una richiesta separata, così l'aggiornamento dell'uso non costa di più.

Ogni richiesta è una vera chiamata API, anche se minima: con un account aziendale è visibile agli
amministratori come qualsiasi uso di Claude Code. Per ridurle usa un intervallo più lungo e la
pausa notturna.

### Il token (`claude setup-token`)

In un terminale, con **Claude Code** installato e collegato al tuo abbonamento (**Pro** o **Max**):

```bash
claude setup-token
```

Si apre un login **OAuth** nel browser e ricevi un **token a lunga durata** `sk-ant-oat01-…`.
È un token di **Claude Code**: una chiamata "normale" alla Messages API con questo token viene di
solito rifiutata. Il dispositivo invia gli stessi header di Claude Code
(`anthropic-beta: oauth-2025-04-20` e il suo `User-Agent`), così la API risponde con gli header dei
limiti. Il token si inserisce **una volta** dal browser e resta **cifrato** sul dispositivo.

### ⚠️ Leggi prima di usare un token di abbonamento

**Anthropic non consente l'uso dei token OAuth degli abbonamenti in strumenti di terze parti.**
Con una policy formalizzata il **4 aprile 2026**, Anthropic ha chiarito che l'OAuth di
Free/Pro/Max (la credenziale prodotta da `claude setup-token`) è destinato **solo** a Claude Code e
claude.ai, e che usarlo in altri prodotti viola i Consumer Terms. Ritmo Code è uno strumento di
terze parti e il metodo descritto sopra, che si presenta come il client Claude Code, è proprio lo
schema a cui si riferisce quella policy.

- **Oggi funziona**, ma *funzionare* non significa *essere permesso*.
- Il rischio è sul **tuo account**: sono stati segnalati errori di autenticazione e blocchi.
- Anthropic può cambiare la API o bloccare questo schema in qualsiasi momento.

Questo progetto non è affiliato ad Anthropic. **Se non accetti questo rischio sul tuo account, non
usare un token di abbonamento con questo firmware.**

Fonti: [The Register](https://www.theregister.com/2026/02/20/anthropic_clarifies_ban_third_party_claude_access/) ·
[WinBuzzer](https://winbuzzer.com/2026/02/19/anthropic-bans-claude-subscription-oauth-in-third-party-apps-xcxwbn/)

---

## Compilare e caricare

Prerequisiti (versioni provate):

- `arduino-cli` 1.4.x · core `esp32:esp32` **3.3.11**
- librerie: **GFX Library for Arduino** 1.6.5 · **lvgl** 9.2.2
- su Windows: **Git Bash** per gli script `.sh`

```bash
cd firmware/ritmo_code
./build.sh                    # compila (ottimizzato -O2) ed esporta il .bin
./build.sh upload COM3        # compila e carica via USB (macOS: /dev/cu.usbmodemXXXX)
./build.sh monitor COM3       # log seriale a 115200
./build.sh ota <ip> <codice>  # compila e carica via Wi-Fi
```

FQBN: `esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom,CDCOnBoot=cdc,USBMode=hwcdc,FlashMode=qio`

### Aggiornamento via Wi-Fi

Sul dispositivo apri **Impostazioni → aggiorna firmware**: compaiono l'indirizzo e un **codice di
6 cifre** valido 5 minuti (5 tentativi sbagliati chiudono la sessione). Poi, dallo stesso Wi-Fi:

- `./build.sh ota <ip> <codice>`, oppure
- apri `http://<ip>/update`, scegli `build/esp32.esp32.esp32s3/ritmo_code.ino.bin` e inserisci il codice.

Token, PIN, account, storico e impostazioni restano intatti. Il cavo USB funziona sempre come
alternativa.

### Note di build

`build.sh` passa `-DLV_CONF_INCLUDE_SIMPLE -I<sketch>` così LVGL trova il `lv_conf.h` dello sketch.
Se compare `lv_conf.h not found`, copia `firmware/ritmo_code/lv_conf.h` nella cartella
`libraries` di Arduino (un livello sopra `lvgl`), proprio **questo** file: il suo
`#include <stdint.h>` è protetto da `#ifndef __ASSEMBLY__`, senza il quale la compilazione dei file
`.S` di lvgl fallisce con `Error: unknown opcode or format name 'typedef'`.

Lo sketch di bring-up ha il suo script, `firmware/bringup/build.sh`, con un FQBN diverso.

I font del tema (JetBrains Mono e DejaVu Sans Mono per i simboli) si rigenerano con
`tools/gen_fonts.sh`.

---

## Primo avvio

Tutto dallo schermo e dal browser, senza ricompilare:

1. **Wi-Fi**: tocca la rete e scrivi la password con la tastiera a schermo (fino a 3 reti salvate).
2. **Token**: lo schermo mostra l'indirizzo del dispositivo (es. `http://192.168.1.42`). Aprilo da
   PC o telefono sulla stessa rete, dai un nome all'account (es. *Studio*) e **incolla il token**.
   Il dispositivo lo **verifica** subito con una chiamata reale prima di accettarlo.
3. **PIN**: scegli un PIN di 4 cifre (inserito due volte); il token viene cifrato con questo PIN.

Agli avvii successivi basta il **PIN**. Gli account aggiunti dopo (*Impostazioni → account*) usano
lo stesso modulo e lo stesso PIN.

---

## Sicurezza

- I token sono salvati **cifrati** (AES-256-GCM, chiave derivata dal PIN con SHA-256), uno slot NVS
  per account, tutti con **lo stesso PIN**. Il PIN **non viene mai salvato**: un PIN sbagliato fa
  fallire la verifica GCM.
- Dopo **10 tentativi** sbagliati le credenziali vengono **cancellate** e si torna al primo avvio
  (dopo ogni errore si attendono 15 secondi).
- Il PIN resta in RAM durante la sessione per cambiare account senza richiederlo; il token attivo è
  comunque già decifrato in RAM.
- Storico e ritmo orario stanno in **LittleFS**, un file per account (senza token).
- Il **pannello web** si legge senza password da chi è sulla tua rete locale: mostra percentuali,
  stato, nome dell'account, meteo e dati del PC, mai il token. Le **modifiche** dal browser (ID dei
  modelli, città, PC, prezzo dell'energia) e il **collegamento dell'app PC** richiedono il PIN (5
  errori bloccano per 5 minuti); l'**aggiornamento firmware** richiede il codice mostrato sullo schermo.
- L'app **Ritmo Code PC Monitor** espone in rete solo `/data.json` (niente nomi di file, processi o
  finestre); la sua pagina di stato e il collegamento rispondono solo al PC stesso.
- `.env` e `.mcp.json` sono in `.gitignore`: **nessun segreto va su git**.

---

## Simulatore

Una copia fedele dell'interfaccia gira nel browser, con un pannello per simulare percentuali,
reset, errori, modelli e velocità del tempo:

```bash
python simulator/serve.py     # poi apri http://127.0.0.1:8480/simulator/
```

Per rigenerare le immagini di questo README (serve `puppeteer-core` e Chrome):

```bash
node tools/capture_screens.js
```

---

## Struttura del repository

```
firmware/
  ritmo_code/                 # il firmware (sketch arduino-cli)
    ritmo_code.ino            # setup/loop, stati, dashboard, schermate, server web
    status_page.h               # pannello web servito su "/"
    api.cpp/.h                  # fetchUsage() e sonda dei modelli
    extras.cpp/.h               # meteo (Open-Meteo) e statistiche del PC
    ota_guard.cpp               # conferma del firmware dopo l'aggiornamento (rollback)
    status.cpp/.h               # incidenti da status.claude.com
    crypto.cpp/.h               # AES-256-GCM + chiave dal PIN
    accounts.cpp/.h             # slot account in NVS
    certs.cpp/.h                # CA bundle per HTTPS
    wifi_manager.h              # reti salvate in NVS (fino a 3)
    touch.h                     # driver touch AXS15231B
    font_jbm_*.c                # font del tema Terminale (96 px orologio della home, 150 px orologio notturno)
    logo_assets.h               # Clawd e simboli (da assets/brand)
    config.h                    # pin, endpoint, costanti, versione
    lv_conf.h                   # configurazione LVGL 9.2
    partitions.csv              # 16 MB: due slot app (OTA) + nvs + LittleFS
    build.sh                    # compila / carica / monitor / ota
  bringup/                      # bring-up validato (riferimento hardware)
  RIFERIMENTO-HARDWARE-LVGL.md   # display, colori e touch che funzionano
simulator/                      # simulatore web dell'interfaccia
ritmo-code-pc-monitor/          # app Windows per le statistiche del PC
tools/
  capture_screens.js            # immagini del README dal simulatore
  gen_fonts.sh                  # font LVGL del tema
  gen_logo_assets.py            # SVG del brand -> logo_assets.h
assets/                         # schermate e brand (brand/)
3D Case/                        # case stampabili (STL)
flash.sh                        # compila e carica trovando da solo la porta (macOS)
```

---

## Crediti

**Ritmo Code** è sviluppato da **Luca Marullo · [Innova Design Studio](https://innovadesignstudio.it)**
([info@innovadesignstudio.it](mailto:info@innovadesignstudio.it)).

È basato su [**claude-usage-stick**](https://github.com/benevid/claude-usage-stick-SVGL) di
**Benevid Felix**, a sua volta derivato dal Claude Usage Stick originale di
[@oauramos](https://github.com/oauramos), con i contributi di
[@jzimath-lab](https://github.com/jzimath-lab), [@renanravelli](https://github.com/renanravelli),
[@mpsd18](https://github.com/mpsd18) e [@ViniciusLoureiro67](https://github.com/ViniciusLoureiro67).
Da lì arrivano la lettura dell'uso dagli header, la cifratura del token, il supporto multi-account
e la base hardware; interfaccia, pagine, funzioni e pannello web di questa versione sono stati
riscritti.

Non è un prodotto ufficiale Anthropic. Claude e Claude Code sono marchi di Anthropic.
