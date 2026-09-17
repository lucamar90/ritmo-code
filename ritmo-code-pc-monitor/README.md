# Ritmo Code PC Monitor

Programma per Windows che invia le statistiche del PC al dispositivo **Ritmo Code**
(pagina **pc** e riga pc della **home**): CPU, GPU, RAM, rete, dischi, temperature, consumi, ventole,
uptime e sessioni di Claude Code aperte.

- Solo libreria standard di Python; i sensori hardware arrivano da
  [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) (MPL-2.0),
  incluso nel pacchetto e raggiungibile solo da questo PC (`127.0.0.1:8085`).
- Il dispositivo legge `http://<ip-del-pc>:8765/data.json` con l'intervallo scelto nelle sue
  impostazioni (1 s, 3 s, 5 s, 1 min). Niente passa da internet.
- Collegamento guidato: la pagina `http://127.0.0.1:8765/` trova il dispositivo sulla rete e gli
  comunica l'indirizzo del PC. Serve il PIN del dispositivo, che non viene salvato.
- **Icona nell'area di notifica**: il suggerimento mostra CPU, GPU, RAM e se il dispositivo sta
  leggendo i dati; doppio clic apre la pagina di stato, il menu (clic destro) apre anche il pannello
  del dispositivo ed esce dall'app.

## Uso

La guida completa, con problemi comuni e aggiornamento, è nel
[README principale](../README.md#installazione-su-windows).

1. `build_exe.bat` crea il pacchetto in `dist\` (serve Python 3.8+ e `pip install pyinstaller`; se
   manca LibreHardwareMonitor lo scarica `scarica_librehardwaremonitor.ps1`).
2. Copia `dist\` in una cartella fissa sul PC da monitorare ed esegui **`installa.bat`** (chiede
   l'amministratore): regola firewall sulla porta 8765 per la rete locale, LibreHardwareMonitor
   all'accesso, monitor all'avvio di Windows.
3. Nella pagina che si apre scegli il dispositivo, inserisci il PIN e premi **Collega questo PC**.

`disinstalla.bat` rimuove tutto. Da sorgente: `python ritmo_pc_monitor.py` (`--print` per una
lettura di prova, `--port`, `--interval`, `--no-lhm`, `--no-tray`).

## Dati inviati (`/data.json`)

| Campo | Fonte |
|---|---|
| `cpu_load`, `cpu_mhz`, `ram_load`, `ram_used_mb`, `ram_total_mb` | Windows |
| `net_down_bps`, `net_up_bps`, `disk_read_bps`, `disk_write_bps` | contatori di Windows |
| `disks` (lettera, % usata, GB liberi), `uptime_s`, `cpu_name` | Windows |
| `claude_sessions` | processi `claude.exe` avviati da un altro programma |
| `cpu_temp`, `cpu_power`, `cpu_core_max`, `cpu_voltage` | LibreHardwareMonitor |
| `gpu_name`, `gpu_load`, `gpu_temp`, `gpu_power`, `gpu_hotspot`, `gpu_mem_temp`, `gpu_clock_mhz`, `gpu_mem_load`, `vram_used_mb`, `vram_total_mb` | LibreHardwareMonitor (GPU con piu' memoria) |
| `fans`, `board_temps`, `ram_temps`, `storage` (temperatura e vita residua) | LibreHardwareMonitor (scheda madre, RAM, dischi) |

La pagina di stato e il collegamento rispondono solo a `127.0.0.1`; dalla rete e' leggibile solo
`/data.json`, che non contiene nomi di file, processi o finestre.
