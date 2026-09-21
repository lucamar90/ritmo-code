"""
Ritmo Code PC Monitor - invia le statistiche di questo PC al dispositivo Ritmo Code.

Legge CPU, GPU, RAM, dischi, rete, uptime e sessioni di Claude Code dalle API di Windows
(solo libreria standard di Python) e, per temperature, consumi, ventole e salute dei dischi,
da LibreHardwareMonitor tramite il suo web server locale (http://127.0.0.1:8085/data.json).

Il dispositivo legge http://<ip-del-pc>:8765/data.json con l'intervallo scelto nelle sue
impostazioni. La pagina http://127.0.0.1:8765/ mostra lo stato e collega il PC al dispositivo
(serve il PIN del dispositivo, che non viene salvato).
"""

import argparse
import concurrent.futures
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import winreg
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APP = "ritmo-code-pc-monitor"
VERSION = "1.7.0"
DEFAULT_PORT = 8765
CONFIG_DIR = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "RitmoCodePcMonitor")
CONFIG_FILE = os.path.join(CONFIG_DIR, "config.json")


# ---------------------------------------------------------------------------
# Sensori Windows (nessuna dipendenza esterna)
# ---------------------------------------------------------------------------

class _PdhValue(ctypes.Structure):
    _fields_ = [("CStatus", wt.DWORD), ("doubleValue", ctypes.c_double)]


class _PdhItem(ctypes.Structure):
    _fields_ = [("szName", wt.LPWSTR), ("FmtValue", _PdhValue)]


class _ProcessEntry32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class _MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", wt.DWORD), ("dwMemoryLoad", wt.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


class WindowsSensors:
    """CPU (come Task Manager), frequenza reale, RAM, dischi, rete, uptime e sessioni di Claude Code."""

    PDH_FMT_DOUBLE_NOCAP = 0x00000200 | 0x00008000
    PDH_MORE_DATA = 0x800007D2
    # interfacce virtuali escluse dal conteggio della rete (per non contare due volte lo stesso traffico)
    NET_SKIP = ("loopback", "isatap", "teredo", "6to4", "hyper-v", "vethernet", "tailscale", "wintun", "vpn")

    def __init__(self, drive):
        self.drive = drive.rstrip(":\\/").upper() + ":\\"
        self._pdh = ctypes.WinDLL("pdh")
        self._query = wt.HANDLE()
        self._pdh.PdhOpenQueryW(None, 0, ctypes.byref(self._query))
        self._util = self._add_counter(r"\Processor Information(_Total)\% Processor Utility")
        self._perf = self._add_counter(r"\Processor Information(_Total)\% Processor Performance")
        self._net_rx = self._add_counter(r"\Network Interface(*)\Bytes Received/sec")
        self._net_tx = self._add_counter(r"\Network Interface(*)\Bytes Sent/sec")
        self._disk_rd = self._add_counter(r"\PhysicalDisk(_Total)\Disk Read Bytes/sec")
        self._disk_wr = self._add_counter(r"\PhysicalDisk(_Total)\Disk Write Bytes/sec")
        self._pdh.PdhGetFormattedCounterArrayW.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.DWORD),
                                                            ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
        self._pdh.PdhGetFormattedCounterArrayW.restype = wt.DWORD
        ctypes.windll.kernel32.GetTickCount64.restype = ctypes.c_ulonglong
        self._pdh.PdhCollectQueryData(self._query)
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0")
        self.base_mhz = winreg.QueryValueEx(key, "~MHz")[0]
        self.cpu_name = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()

    def _add_counter(self, path):
        handle = wt.HANDLE()
        if self._pdh.PdhAddEnglishCounterW(self._query, path, 0, ctypes.byref(handle)) != 0:
            return None
        return handle

    def _counter(self, handle):
        if handle is None:
            return 0.0
        value = _PdhValue()
        if self._pdh.PdhGetFormattedCounterValue(handle, self.PDH_FMT_DOUBLE_NOCAP, None, ctypes.byref(value)) != 0:
            return 0.0
        return value.doubleValue

    def _counter_sum(self, handle):
        """Somma delle istanze di un contatore con (*), escluse le interfacce virtuali."""
        if handle is None:
            return 0.0
        size, count = wt.DWORD(0), wt.DWORD(0)
        status = self._pdh.PdhGetFormattedCounterArrayW(handle, self.PDH_FMT_DOUBLE_NOCAP, ctypes.byref(size),
                                                        ctypes.byref(count), None)
        if status != self.PDH_MORE_DATA or not size.value:
            return 0.0
        buf = ctypes.create_string_buffer(size.value)
        if self._pdh.PdhGetFormattedCounterArrayW(handle, self.PDH_FMT_DOUBLE_NOCAP, ctypes.byref(size),
                                                  ctypes.byref(count), buf) != 0:
            return 0.0
        items = ctypes.cast(buf, ctypes.POINTER(_PdhItem))
        total = 0.0
        for i in range(count.value):
            name = (items[i].szName or "").lower()
            if not any(skip in name for skip in self.NET_SKIP):
                total += max(0.0, items[i].FmtValue.doubleValue)
        return total

    @staticmethod
    def _fixed_drives():
        """Dischi fissi (tipo 3) con percentuale usata e spazio in GB."""
        kernel = ctypes.windll.kernel32
        mask = kernel.GetLogicalDrives()
        drives, gb = [], 1024 ** 3
        for i in range(26):
            if not mask & (1 << i):
                continue
            root = f"{chr(65 + i)}:\\"
            if kernel.GetDriveTypeW(root) != 3:
                continue
            free, total = ctypes.c_ulonglong(), ctypes.c_ulonglong()
            if not kernel.GetDiskFreeSpaceExW(root, None, ctypes.byref(total), ctypes.byref(free)) or not total.value:
                continue
            drives.append({"d": chr(65 + i), "used_pct": round(100.0 * (total.value - free.value) / total.value, 1),
                           "free_gb": round(free.value / gb, 1), "total_gb": round(total.value / gb, 1)})
        return drives

    @staticmethod
    def _claude_sessions():
        """Sessioni di Claude Code aperte: processi claude.exe il cui genitore non e' un altro claude.exe."""
        kernel = ctypes.windll.kernel32
        kernel.CreateToolhelp32Snapshot.restype = wt.HANDLE
        snap = kernel.CreateToolhelp32Snapshot(0x2, 0)
        if not snap or snap == wt.HANDLE(-1).value:
            return -1
        entry = _ProcessEntry32W()
        entry.dwSize = ctypes.sizeof(entry)
        procs = []
        ok = kernel.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            procs.append((entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile.lower()))
            ok = kernel.Process32NextW(snap, ctypes.byref(entry))
        kernel.CloseHandle(snap)
        claude = {pid for pid, _, exe in procs if exe == "claude.exe"}
        return sum(1 for pid, parent, exe in procs if exe == "claude.exe" and parent not in claude)

    def read(self):
        self._pdh.PdhCollectQueryData(self._query)
        mem = _MemoryStatusEx()
        mem.dwLength = ctypes.sizeof(mem)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(mem))
        free, total = ctypes.c_ulonglong(), ctypes.c_ulonglong()
        ctypes.windll.kernel32.GetDiskFreeSpaceExW(self.drive, None, ctypes.byref(total), ctypes.byref(free))
        gb = 1024 ** 3
        return {
            "cpu_load": min(100.0, self._counter(self._util)),
            "cpu_mhz": self.base_mhz * self._counter(self._perf) / 100.0,
            "ram_load": float(mem.dwMemoryLoad),
            "ram_used_mb": (mem.ullTotalPhys - mem.ullAvailPhys) / 1024 ** 2,
            "disk_used_pct": 100.0 * (total.value - free.value) / total.value if total.value else 0.0,
            "disk_free_gb": free.value / gb,
            "disk_used_gb": (total.value - free.value) / gb,
            # dettagli per la pagina pc di Ritmo Code
            "cpu_name": self.cpu_name,
            "ram_total_mb": mem.ullTotalPhys / 1024 ** 2,
            "net_down_bps": self._counter_sum(self._net_rx),
            "net_up_bps": self._counter_sum(self._net_tx),
            "disk_read_bps": self._counter(self._disk_rd),
            "disk_write_bps": self._counter(self._disk_wr),
            "disks": self._fixed_drives(),
            "uptime_s": int(ctypes.windll.kernel32.GetTickCount64() // 1000),
            "claude_sessions": self._claude_sessions(),
        }


# ---------------------------------------------------------------------------
# LibreHardwareMonitor (opzionale): temperatura, consumo, GPU
# ---------------------------------------------------------------------------

class LibreHardwareMonitor:
    CPU_TEMP_NAMES = ["core (tctl/tdie)", "cpu package", "package", "tdie", "tctl", "core average", "core max"]
    CPU_POWER_NAMES = ["package", "cpu package"]
    GPU_NAMES = ["gpu core", "gpu hot spot"]

    def __init__(self, url):
        self.url = url
        self.online = False

    @staticmethod
    def _number(node):
        raw = node.get("RawValue")
        if isinstance(raw, (int, float)):
            return float(raw)
        match = re.search(r"-?\d+(?:[.,]\d+)?", str(node.get("Value", "")))
        return float(match.group().replace(",", ".")) if match else None

    def _sensors(self, node, hardware="", category=""):
        """Restituisce (hardware, tipo, nome, valore) per ogni sensore dell'albero JSON."""
        image = str(node.get("ImageURL", "")).lower()
        text = str(node.get("Text", ""))
        children = node.get("Children", [])
        if "SensorId" in node:
            parts = str(node["SensorId"]).strip("/").split("/")
            hw = parts[0] if parts else hardware
            kind = parts[2] if len(parts) > 2 else category
            yield hw.lower(), kind.lower(), text.lower(), self._number(node)
            return
        if not children and node.get("Value"):
            yield hardware, category, text.lower(), self._number(node)
            return
        if "cpu" in image or "ati" in image or "nvidia" in image or "gpu" in image:
            hardware = "cpu" if "cpu" in image else "gpu"
        elif "transparent" in image and text:
            category = {"temperatures": "temperature", "powers": "power", "load": "load"}.get(text.lower(), text.lower())
        for child in children:
            yield from self._sensors(child, hardware, category)

    @staticmethod
    def _pick(sensors, hw_match, kind, preferred):
        candidates = [(name, value) for hw, k, name, value in sensors
                      if hw_match(hw) and k == kind and value is not None]
        for wanted in preferred:
            for name, value in candidates:
                if name == wanted:
                    return value
        return candidates[0][1] if candidates else None

    def _hardware(self, node, names, sensors):
        """Nomi dell'hardware per id ("gpu-nvidia/0") e sensori (id, tipo, nome, valore)."""
        if "HardwareId" in node:
            names[str(node["HardwareId"]).strip("/")] = str(node.get("Text", ""))
        if "SensorId" in node:
            parts = str(node["SensorId"]).strip("/").split("/")
            if len(parts) >= 3 and not (parts[0] == "memory" and len(parts) < 4):
                depth = 3 if parts[0] == "memory" and len(parts) >= 4 else 2
                sensors.append(("/".join(parts[:depth]), parts[depth].lower(), str(node.get("Text", "")).lower(), self._number(node)))
        for child in node.get("Children", []):
            self._hardware(child, names, sensors)

    def read(self):
        try:
            with urllib.request.urlopen(self.url, timeout=0.8) as response:
                tree = json.load(response)
        except Exception:
            self.online = False
            return {}
        self.online = True
        sensors = list(self._sensors(tree))
        names, detailed = {}, []
        self._hardware(tree, names, detailed)
        # GPU principale: quella con piu' memoria video (la dedicata rispetto all'integrata)
        gpus = {hw for hw, _, _, _ in detailed if hw.startswith("gpu")}
        def vram_total(hw):
            return next((v for h, k, n, v in detailed if h == hw and n == "gpu memory total" and v), 0)
        gpu = max(gpus, key=vram_total) if gpus else None
        is_cpu = lambda hw: "cpu" in hw
        is_gpu = (lambda hw: hw == gpu.split("/")[0]) if gpu else (lambda hw: "gpu" in hw)
        pick_gpu = lambda kind, wanted: next((v for h, k, n, v in detailed
                                              if h == gpu and k == kind and n in wanted and v is not None), None)
        fans = []
        for hw in dict.fromkeys(h for h, k, _, _ in detailed if k == "fan"):
            chip = [(n, v) for h, k, n, v in detailed if h == hw and k == "fan" and v is not None]
            if hw.startswith("lpc") and chip and all(v == 0 for _, v in chip):
                continue
            fans += [{"name": n, "rpm": round(v)} for n, v in chip]
        cpu = next((h for h, _, _, _ in detailed if "cpu" in h), None)
        pick = lambda hw, kind, wanted: next((v for h, k, n, v in detailed
                                             if h == hw and k == kind and n in wanted and v is not None), None)
        # voltaggio CPU: sensore del core se c'e', altrimenti media dei VID dei core
        volt = pick(cpu, "voltage", ["core (svi2 tfn)", "core (svi3 tfn)", "cpu core", "vcore"]) if cpu else None
        if volt is None:   # Vcore letto dalla scheda madre, se abilitata
            volt = next((v for h, k, n, v in detailed if h.startswith("lpc") and k == "voltage"
                         and n in ("vcore", "cpu vcore") and v and 0.5 < v < 1.7), None)
        if volt is None and cpu:   # altrimenti il VID piu' alto richiesto dai core
            vids = [v for h, k, n, v in detailed if h == cpu and k == "voltage" and n.endswith(" vid") and v]
            volt = max(vids) if vids else None
        # scheda madre (chip "lpc"/superio), RAM e dischi: compaiono se abilitati in LibreHardwareMonitor
        board = [{"name": n, "value": round(v, 1)} for h, k, n, v in detailed
                 if (h.startswith("lpc") or h.startswith("motherboard")) and k == "temperature" and v and 0 < v < 125]
        ram_temps = [round(v, 1) for h, k, n, v in detailed
                     if (h.startswith("ram") or h.startswith("memory")) and k == "temperature" and n.startswith("dimm")
                     and v and 0 < v < 125]
        storage = []
        for hw in sorted({h for h, _, _, _ in detailed if h.split("/")[0] in ("nvme", "hdd", "ssd", "ata", "storage")}):
            temp = next((v for h, k, n, v in detailed if h == hw and k == "temperature" and v), None)
            life = pick(hw, "level", ["life", "remaining life", "life remaining"])
            used = pick(hw, "level", ["percentage used"])
            if life is None and used is not None:
                life = max(0.0, 100.0 - used)
            storage.append({"name": names.get(hw, hw), "temp": temp, "life": life})
        return {k: v for k, v in {
            "cpu_core_max": pick(cpu, "load", ["cpu core max"]) if cpu else None,
            "cpu_voltage": round(volt, 3) if volt else None,
            "gpu_hotspot": pick_gpu("temperature", ["gpu hot spot"]) if gpu else None,
            "gpu_mem_temp": pick_gpu("temperature", ["gpu memory junction", "gpu memory"]) if gpu else None,
            "gpu_clock_mhz": pick_gpu("clock", ["gpu core"]) if gpu else None,
            "gpu_mem_load": pick_gpu("load", ["gpu memory"]) if gpu else None,
            "board_temps": board or None,
            "ram_temps": ram_temps or None,
            "storage": storage or None,
            "cpu_temp": self._pick(sensors, is_cpu, "temperature", self.CPU_TEMP_NAMES),
            "cpu_power": self._pick(sensors, is_cpu, "power", self.CPU_POWER_NAMES),
            "gpu_load": pick_gpu("load", ["gpu core"]) if gpu else self._pick(sensors, is_gpu, "load", self.GPU_NAMES),
            "gpu_temp": pick_gpu("temperature", ["gpu core", "gpu hot spot"]) if gpu else self._pick(sensors, is_gpu, "temperature", self.GPU_NAMES),
            "gpu_name": names.get(gpu) if gpu else None,
            "gpu_power": pick_gpu("power", ["gpu package", "gpu power", "gpu core"]) if gpu else None,
            "vram_used_mb": pick_gpu("smalldata", ["gpu memory used"]) if gpu else None,
            "vram_total_mb": pick_gpu("smalldata", ["gpu memory total"]) if gpu else None,
            "fans": fans or None,
        }.items() if v is not None}


# ---------------------------------------------------------------------------
# Campionamento in background
# ---------------------------------------------------------------------------

class _LastInput(ctypes.Structure):
    _fields_ = [("cbSize", wt.UINT), ("dwTime", wt.DWORD)]


def idle_seconds():
    """Secondi dall'ultimo input di mouse o tastiera in questa sessione (anche da desktop remoto)."""
    info = _LastInput(ctypes.sizeof(_LastInput), 0)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(info)):
        return 0.0
    return ((ctypes.windll.kernel32.GetTickCount() - info.dwTime) & 0xFFFFFFFF) / 1000.0


class Activity:
    """Minuti di uso continuo del PC: si azzerano dopo BREAK_S secondi senza mouse e tastiera."""

    BREAK_S = 300

    def __init__(self):
        self.start = None

    def update(self):
        now, idle = time.time(), idle_seconds()
        if idle >= self.BREAK_S:
            self.start = None
        elif self.start is None:
            self.start = now - idle
        return {"idle_s": int(idle), "act_min": 0 if self.start is None else int((now - self.start) // 60)}


class Sampler(threading.Thread):
    def __init__(self, windows, lhm, interval):
        super().__init__(daemon=True)
        self.windows, self.lhm, self.interval = windows, lhm, interval
        self.lock = threading.Lock()
        self.data = {}
        self.activity = Activity()

    def run(self):
        while True:
            started = time.monotonic()
            data = {"cpu_temp": 0.0, "cpu_power": 0.0}
            try:
                data.update(self.windows.read())
            except Exception as error:
                print(f"[sensori] errore: {error}")
            if self.lhm:
                data.update(self.lhm.read())
            try:
                data.update(self.activity.update())       # promemoria pausa del dispositivo
            except Exception as error:
                print(f"[attivita'] errore: {error}")
            with self.lock:
                self.data = data
            time.sleep(max(0.1, self.interval - (time.monotonic() - started)))

    def snapshot(self):
        with self.lock:
            return dict(self.data)


# ---------------------------------------------------------------------------
# Collegamento con il dispositivo Ritmo Code
# ---------------------------------------------------------------------------

def load_config():
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return {}


def save_config(cfg):
    os.makedirs(CONFIG_DIR, exist_ok=True)
    with open(CONFIG_FILE, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=1)


def ip_towards(target="10.255.255.255"):
    """IP di questo PC sull'interfaccia che porta a `target` (evita VPN come Tailscale)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect((target, 1))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def get_json(url, timeout=2.0):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", "replace"))


def probe_device(ip):
    """Stato del dispositivo se `ip` e' un Ritmo Code (risponde a /api/status con la versione)."""
    try:
        data = get_json(f"http://{ip}/api/status", 1.5)
        if "fw" in data:
            pc = data.get("pc") or {}
            return {"ip": ip, "fw": data.get("fw"), "account": data.get("account", ""),
                    "pc_host": pc.get("host", ""), "pc_ok": pc.get("ok", False)}
    except Exception:
        pass
    return None


def discover_devices(cfg):
    """Cerca i dispositivi: prima l'ultimo noto e ritmo-code.local, poi tutta la rete locale /24."""
    found = {}
    for candidate in [cfg.get("device"), "ritmo-code.local"]:
        if candidate:
            dev = probe_device(candidate)
            if dev:
                found[dev["ip"]] = dev
    base = ip_towards().rsplit(".", 1)[0]
    if not base.startswith("127"):
        with concurrent.futures.ThreadPoolExecutor(64) as pool:
            for dev in pool.map(probe_device, [f"{base}.{i}" for i in range(1, 255)]):
                if dev:
                    found.setdefault(dev["ip"], dev)
    return list(found.values())


def pair_device(ip, pin, port):
    """Comunica al dispositivo l'indirizzo di questo PC (il dispositivo verifica il PIN)."""
    host = f"{ip_towards(ip)}:{port}"
    body = urllib.parse.urlencode({"pin": pin, "pc": host}).encode()
    req = urllib.request.Request(f"http://{ip}/pcpair", data=body, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        with urllib.request.urlopen(req, timeout=6) as response:
            result = json.loads(response.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as error:
        try:
            result = json.loads(error.read().decode("utf-8", "replace"))
        except Exception:
            result = {"ok": False, "error": f"HTTP {error.code}"}
    except Exception as error:
        result = {"ok": False, "error": f"dispositivo non raggiungibile ({error})"}
    result["pc"] = host
    return result


# ---------------------------------------------------------------------------
# Claude Code: gli hook avvisano quando Claude ha finito o aspetta un permesso
# ---------------------------------------------------------------------------

CLAUDE_SETTINGS = os.path.join(os.path.expanduser("~"), ".claude", "settings.json")
HOOK_EVENTS = {"UserPromptSubmit": "prompt", "Stop": "stop", "Notification": "notify"}
HOOK_RE = re.compile(r"127\.0\.0\.1:\d+/claude/(prompt|stop|notify)")


def hook_command(port, kind):
    # l'input JSON dell'hook passa su stdin; "|| exit 0" se il monitor e' spento (bash e cmd)
    return f"curl -s -m 2 --data-binary @- http://127.0.0.1:{port}/claude/{kind} || exit 0"


def _load_claude_settings():
    if not os.path.exists(CLAUDE_SETTINGS):
        return {}
    with open(CLAUDE_SETTINGS, "r", encoding="utf-8") as fh:
        text = fh.read()
    return json.loads(text) if text.strip() else {}


def _without_ours(groups):
    """Gruppi di hook senza i comandi di Ritmo Code (i gruppi rimasti vuoti vengono tolti)."""
    kept = []
    for group in groups if isinstance(groups, list) else []:
        hooks = [h for h in group.get("hooks", []) if not HOOK_RE.search(str(h.get("command", "")))]
        if hooks:
            kept.append({**group, "hooks": hooks})
    return kept


def hooks_status(port):
    try:
        hooks = _load_claude_settings().get("hooks", {})
    except Exception as error:
        return {"ok": False, "error": f"settings.json non leggibile ({error})"}
    found = {event for event, groups in hooks.items() for group in groups if isinstance(group, dict)
             for h in group.get("hooks", []) if HOOK_RE.search(str(h.get("command", "")))}
    current = all(any(hook_command(port, kind) == h.get("command") for group in hooks.get(event, [])
                      for h in group.get("hooks", [])) for event, kind in HOOK_EVENTS.items())
    return {"ok": True, "installed": found == set(HOOK_EVENTS) and current, "partial": bool(found), "path": CLAUDE_SETTINGS}


def hooks_write(port, install):
    """Aggiunge o toglie gli hook di Ritmo Code in ~/.claude/settings.json lasciando intatto il resto."""
    try:
        settings = _load_claude_settings()
    except Exception as error:
        return {"ok": False, "error": f"settings.json non leggibile, non lo modifico ({error})"}
    hooks = settings.get("hooks") if isinstance(settings.get("hooks"), dict) else {}
    for event in list(hooks):
        hooks[event] = _without_ours(hooks[event])
        if not hooks[event]:
            del hooks[event]
    if install:
        for event, kind in HOOK_EVENTS.items():
            hooks.setdefault(event, []).append({"hooks": [{"type": "command", "command": hook_command(port, kind), "timeout": 5}]})
    if hooks:
        settings["hooks"] = hooks
    else:
        settings.pop("hooks", None)
    os.makedirs(os.path.dirname(CLAUDE_SETTINGS), exist_ok=True)
    backup = CLAUDE_SETTINGS + ".ritmo-bak"
    if os.path.exists(CLAUDE_SETTINGS) and not os.path.exists(backup):
        with open(CLAUDE_SETTINGS, "rb") as src, open(backup, "wb") as dst:
            dst.write(src.read())
    tmp = CLAUDE_SETTINGS + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(settings, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, CLAUDE_SETTINGS)
    return {"ok": True, **hooks_status(port)}


class ClaudeEvents:
    """Stato delle sessioni di Claude Code dagli hook: chi sta lavorando e l'ultimo evento da mostrare.

    Eventi per il dispositivo: busy (hai scritto a Claude), done (ha finito), perm (serve un
    permesso), ask (ha una domanda). L'id cresce sempre, cosi' il dispositivo li mostra una volta sola.
    """

    STALE_S = 3 * 3600

    def __init__(self):
        self.lock = threading.Lock()
        self.working = {}            # session_id -> inizio del turno
        self.last = None
        self.last_id = 0

    @staticmethod
    def _project(payload):
        name = os.path.basename(str(payload.get("cwd", "")).rstrip("\\/"))
        return re.sub(r"[^0-9A-Za-z._ -]", "", name)[:24]

    def handle(self, kind, payload):
        now = time.time()
        session = str(payload.get("session_id", ""))
        with self.lock:
            self.working = {s: t for s, t in self.working.items() if now - t < self.STALE_S}
            if kind == "prompt":
                self.working[session] = now
                ev, dur = "busy", -1
            elif kind == "stop":
                started = self.working.pop(session, None)
                ev, dur = "done", int(now - started) if started else -1
            else:
                ntype = str(payload.get("notification_type", ""))
                message = str(payload.get("message", "")).lower()
                if ntype == "permission_prompt" or (not ntype and "permission" in message):
                    ev = "perm"
                elif ntype == "elicitation_dialog":
                    ev = "ask"
                else:
                    return None                  # "ti aspetta" dopo 60 s di inattivita' e altro: niente avviso
                dur = -1
            self.last_id = max(self.last_id + 1, int(now) % 1_000_000_000)
            self.last = {"id": self.last_id, "ev": ev, "proj": self._project(payload), "dur": dur, "at": now}
            return dict(self.last)

    def fields(self):
        """Campi per data.json (piatti: il dispositivo ha un parser minimo)."""
        with self.lock:
            now = time.time()
            if WindowsSensors._claude_sessions() == 0:       # Claude Code chiuso (anche con Esc a meta'): nessuno al lavoro
                self.working.clear()
            data = {"cc_busy": sum(1 for t in self.working.values() if now - t < self.STALE_S)}
            if self.last:
                data.update({"cc_ev_id": self.last["id"], "cc_ev": self.last["ev"], "cc_ev_proj": self.last["proj"],
                             "cc_ev_dur": self.last["dur"], "cc_ev_age": int(now - self.last["at"])})
            return data


CLAUDE = ClaudeEvents()


def push_event(event):
    """Manda subito l'evento al dispositivo; se non risponde, lo legge comunque da data.json."""
    ip = DEVICE_SEEN["ip"] if DEVICE_SEEN["ip"] and time.time() - DEVICE_SEEN["at"] < 600 else load_config().get("device")
    if not ip:
        return
    fields = {k: event[k] for k in ("id", "ev", "proj", "dur")}
    fields["busy"] = CLAUDE.fields()["cc_busy"]               # sessioni al lavoro: l'indicatore si aggiorna subito
    body = urllib.parse.urlencode(fields).encode()
    try:
        urllib.request.urlopen(urllib.request.Request(f"http://{ip}/claude", data=body, method="POST"), timeout=2).close()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Suoni e notifiche sul PC per gli avvisi del dispositivo (timer, pomodoro, Claude, soglie)
# ---------------------------------------------------------------------------

# note (frequenza Hz, durata s) per evento; 0 = pausa
SOUNDS = {
    "break": [(659, 0.18), (523, 0.4)],                          # pausa: scende, tranquillo
    "focus": [(523, 0.13), (659, 0.13), (784, 0.35)],            # si riparte: sale
    "timer": [(880, 0.15), (0, 0.08), (880, 0.15), (0, 0.08), (880, 0.35)],
    "cycle": [(523, 0.12), (659, 0.12), (784, 0.12), (1047, 0.45)],
    "done":  [(784, 0.12), (1047, 0.4)],                         # Claude ha finito
    "perm":  [(659, 0.2), (0, 0.06), (659, 0.3)],                # Claude aspetta
    "ask":   [(659, 0.2), (0, 0.06), (659, 0.3)],
    "thr":   [(440, 0.18), (0, 0.06), (440, 0.3)],               # soglia di utilizzo
    "reset": [(523, 0.15), (784, 0.4)],                          # finestra di nuovo disponibile
    "cal":   [(784, 0.15), (988, 0.15), (1175, 0.4)],            # evento del calendario tra poco
}
# suoni di Windows (MessageBeep): seguono il volume dei Suoni di sistema e arrivano anche nelle
# sessioni remote, dove le melodie qui sopra (PlaySound) possono restare mute
BEEPS = {"break": 0x40, "cycle": 0x40, "reset": 0x40,        # asterisco
         "timer": 0x30, "perm": 0x30, "ask": 0x30,           # esclamazione
         "thr": 0x10,                                        # errore critico
         "cal": 0x30,                                        # esclamazione
         "focus": 0x0, "done": 0x0}                          # predefinito
DEFAULT_NOTIFY = {"sound": True, "toast": True, "volume": 60, "style": "windows"}
_sound_lock = threading.Lock()
_sound_cache = {}
TRAY = None                                   # icona nell'area di notifica, se attiva


def notify_settings():
    cfg = load_config()
    return {k: cfg.get(k, v) for k, v in DEFAULT_NOTIFY.items()}


def _chime(notes, volume):
    """WAV mono 16 bit in memoria: toni con attacco breve e coda che si spegne (suono da campanella)."""
    import io
    import math
    import struct
    import wave
    rate, amp = 22050, 32767 * 0.8 * max(0, min(100, volume)) / 100
    frames = bytearray()
    for freq, dur in notes:
        n = int(rate * dur)
        for i in range(n):
            if not freq:
                frames += b"\0\0"
                continue
            env = min(1.0, i / (0.004 * rate)) * math.exp(-4.0 * i / n)
            t = 2 * math.pi * freq * i / rate
            frames += struct.pack("<h", int(amp * env * (0.8 * math.sin(t) + 0.2 * math.sin(2 * t))))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def play_sound(ev, volume, style="windows"):
    if style == "windows":
        if ev in BEEPS:
            ctypes.windll.user32.MessageBeep(BEEPS[ev])
        return
    import winsound
    notes = SOUNDS.get(ev)
    if not notes or volume <= 0:
        return
    key = (ev, volume)
    if key not in _sound_cache:
        _sound_cache[key] = _chime(notes, volume)
    with _sound_lock:                          # un suono alla volta
        try:
            winsound.PlaySound(_sound_cache[key], winsound.SND_MEMORY)
        except Exception as error:
            print(f"[suono] errore: {error}")


def pc_notify(ev, title, msg):
    """Suono e notifica di Windows secondo le impostazioni della pagina di stato."""
    s = notify_settings()
    if s["toast"] and TRAY and title:
        TRAY.balloon(title, msg)
    if s["sound"]:
        play_sound(ev, int(s["volume"]), s["style"])


STATUS_PAGE = """<!doctype html><html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Ritmo Code PC Monitor</title>
<style>
:root{--bg:#141413;--tr:#2C2A27;--bd:#3A3834;--tx:#E8E6DF;--mu:#8E8B82;--fa:#5C5A55;--ac:#D97757;--ok:#9BC08A;--bad:#E06C5A}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:14px/1.5 "JetBrains Mono",ui-monospace,Consolas,monospace;padding:24px 16px}
main{max-width:720px;margin:0 auto;display:flex;flex-direction:column;gap:20px}
h1{font-size:16px;font-weight:500;margin:0;border-bottom:1px solid var(--bd);padding-bottom:10px}h1 b{color:var(--ac);font-weight:500}
.box{position:relative;border:1px solid var(--bd);border-radius:4px;padding:18px 16px 14px}
.lg{position:absolute;top:-10px;left:10px;background:var(--bg);padding:0 6px;color:var(--mu);font-size:12px}
.row{display:flex;flex-wrap:wrap;gap:6px 18px}.k{color:var(--mu)}.warn{color:var(--bad)}.okc{color:var(--ok)}
select,input,button{font:inherit;background:var(--bg);color:var(--tx);border:1px solid var(--bd);border-radius:4px;padding:8px 10px}
button{color:var(--ac);border-color:var(--ac);cursor:pointer}button:disabled{opacity:.5;cursor:wait}
.form{display:flex;flex-wrap:wrap;gap:10px;align-items:center;margin-top:12px}#msg{margin-top:10px}
code{color:var(--ac)}
</style></head><body><main>
<h1><b>&#10043; ritmo-code</b> pc monitor <span class=k>v__VERSION__</span></h1>
<div class=box><span class=lg>questo pc</span><div class=row id=pc>lettura in corso...</div>
 <p class=k style="margin:10px 0 0">Indirizzo per il dispositivo: <code>__HOST__</code> &middot; LibreHardwareMonitor: <b>__LHM__</b></p></div>
<div class=box><span class=lg>dispositivo</span><div id=dev class=k>ricerca in corso...</div>
 <div class=form><select id=sel hidden></select><input id=pin type=password inputmode=numeric maxlength=4 placeholder="PIN" size=6>
 <button id=go>Collega questo PC</button><button id=scan>Cerca di nuovo</button></div><div id=msg></div>
 <p class=k style="margin:10px 0 0">Il PIN serve solo ad autorizzare il collegamento e non viene salvato.</p></div>
<div class=box><span class=lg>avvisi di claude code</span><div id=hk class=k>controllo in corso...</div>
 <div class=form><button id=hkon>Attiva gli avvisi</button><button id=hkoff>Disattiva</button></div>
 <p class=k style="margin:10px 0 0">Aggiunge tre hook a <code>~/.claude/settings.json</code> (il resto del file non cambia): il dispositivo
 mostra quando Claude ha finito o aspetta un permesso. Valgono per le sessioni di Claude Code aperte da ora in poi.</p></div>
<div class=box><span class=lg>calendario</span><div id=cal class=k>...</div>
 <div class=form><button id=calr>Aggiorna ora</button></div>
 <p class=k style="margin:10px 0 0">Il link iCal si imposta nel pannello del dispositivo (pagina <code>/home</code>); questo PC scarica
 gli eventi ogni 5 minuti e passa al dispositivo i prossimi 3.</p></div>
<div class=box><span class=lg>suoni e notifiche</span>
 <div class=form><label><input type=checkbox id=snd> suono</label><label><input type=checkbox id=tst> notifica di Windows</label>
 <select id=sty><option value=windows>suoni di Windows</option><option value=ritmo>melodie Ritmo Code</option></select>
 <label id=voll>volume <input type=range id=vol min=0 max=100 step=5></label><button id=try>Prova</button></div>
 <p class=k style="margin:10px 0 0">Quando il dispositivo mostra un avviso (fine del timer, pausa e ripresa del pomodoro, Claude
 ha finito o aspetta, soglie di utilizzo) questo PC suona e mostra una notifica. Sul dispositivo si attiva in
 <i>Impostazioni &rarr; suoni sul pc</i>; di notte resta muto. I suoni di Windows seguono il volume dei
 <i>Suoni di sistema</i> e si sentono anche in desktop remoto; le melodie hanno un volume proprio.</p></div>
</main><script>
var $=function(i){return document.getElementById(i)};
function pc(){fetch('/data.json').then(function(r){return r.json()}).then(function(d){
 var r=function(v,s){return v==null?'--':Math.round(v)+(s||'')};
 $('pc').innerHTML='<span><span class=k>cpu</span> '+r(d.cpu_load,'%')+' '+r(d.cpu_temp,'&deg;')+'</span><span><span class=k>gpu</span> '+r(d.gpu_load,'%')+' '+r(d.gpu_temp,'&deg;')+
 '</span><span><span class=k>ram</span> '+r(d.ram_load,'%')+'</span><span><span class=k>claude code</span> '+(d.claude_sessions==null?'--':d.claude_sessions)+'</span><span class=k>'+(d.cpu_name||'')+'</span>'})}
function scan(){$('dev').textContent='ricerca in corso...';$('sel').hidden=true;
 fetch('/api/devices').then(function(r){return r.json()}).then(function(l){
  if(!l.length){$('dev').innerHTML='<span class=warn>nessun dispositivo trovato</span>: acceso, sbloccato con il PIN e sulla stessa rete?';return}
  $('sel').innerHTML=l.map(function(d){return '<option value="'+d.ip+'">'+d.ip+' &middot; v'+d.fw+(d.account?' &middot; @'+d.account:'')+'</option>'}).join('');$('sel').hidden=false;
  var d=l[0];$('dev').innerHTML='legge: <b>'+(d.pc_host||'nessun pc')+'</b> '+(d.pc_ok?'<span class=okc>collegato</span>':'<span class=warn>non collegato</span>')})}
$('scan').onclick=scan;
$('go').onclick=function(){var b=this;b.disabled=true;$('msg').textContent='';
 fetch('/api/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ip:$('sel').value,pin:$('pin').value})})
 .then(function(r){return r.json()}).then(function(j){b.disabled=false;$('pin').value='';
  $('msg').innerHTML=j.ok?'<span class=okc>Collegato: il dispositivo ora legge '+j.pc+'</span>':'<span class=warn>'+(j.error||'errore')+'</span>';if(j.ok)setTimeout(scan,4000)})
 .catch(function(){b.disabled=false})};
var EV={busy:'al lavoro',done:'ha finito',perm:'aspetta un permesso',ask:'ha una domanda'};
function hk(j){if(!j.ok){$('hk').innerHTML='<span class=warn>'+j.error+'</span>';return}
 var last=j.last?' &middot; ultimo evento: <b>'+EV[j.last.ev]+'</b>'+(j.last.proj?' ('+j.last.proj+')':'')+' '+j.last.age+' s fa':'';
 $('hk').innerHTML=(j.installed?'<span class=okc>attivi</span>':j.partial?'<span class=warn>da aggiornare</span>':'<span class=warn>non attivi</span>')+last}
function hks(){fetch('/api/hooks').then(function(r){return r.json()}).then(hk)}
function hkset(on){fetch('/api/hooks',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({install:on})})
 .then(function(r){return r.json()}).then(hk)}
$('hkon').onclick=function(){hkset(true)};$('hkoff').onclick=function(){hkset(false)};
function ns(j){$('snd').checked=j.sound;$('tst').checked=j.toast;$('vol').value=j.volume;$('sty').value=j.style;$('voll').hidden=j.style!='ritmo'}
function nset(extra){var b={sound:$('snd').checked,toast:$('tst').checked,volume:+$('vol').value,style:$('sty').value};for(var k in extra)b[k]=extra[k];
 fetch('/api/notify',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}).then(function(r){return r.json()}).then(ns)}
$('snd').onchange=$('tst').onchange=$('vol').onchange=$('sty').onchange=function(){nset()};$('try').onclick=function(){nset({test:true})};
fetch('/api/notify').then(function(r){return r.json()}).then(ns);
function cal(){fetch('/api/calendar').then(function(r){return r.json()}).then(function(j){
 var n=j.next?' &middot; prossimo: <b>'+j.next.title+'</b> alle '+new Date(j.next.start*1000).toTimeString().slice(0,5):'';
 $('cal').innerHTML=j.state+n})}
$('calr').onclick=function(){fetch('/api/calendar',{method:'POST'}).then(function(){setTimeout(cal,3000)})};
cal();setInterval(cal,15000);
pc();setInterval(pc,2000);scan();hks();setInterval(hks,5000);
</script></body></html>"""


def make_handler(sampler, lhm, port):
    class Handler(BaseHTTPRequestHandler):
        server_version = f"RitmoCodePcMonitor/{VERSION}"

        def _send(self, status, content_type, body):
            payload = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)

        def _local(self):
            return self.client_address[0] in ("127.0.0.1", "::1")

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/data.json":
                if not self._local():
                    DEVICE_SEEN.update(ip=self.client_address[0], at=time.time())
                data = sampler.snapshot()
                data.update(CLAUDE.fields())
                data.update(CAL.fields())
                if data.get("claude_sessions", -1) >= 0:          # mai piu' sessioni al lavoro di quelle aperte
                    data["cc_busy"] = min(data["cc_busy"], data["claude_sessions"])
                data.update({"app": APP, "version": VERSION})
                self._send(200, "application/json", json.dumps(data, ensure_ascii=False))   # titoli con accenti
            elif path == "/" and self._local():
                lhm_state = "disattivato" if lhm is None else ("connesso" if lhm.online else "non raggiungibile")
                page = (STATUS_PAGE.replace("__VERSION__", VERSION)
                        .replace("__HOST__", f"{ip_towards()}:{port}").replace("__LHM__", lhm_state))
                self._send(200, "text/html; charset=utf-8", page)
            elif path == "/api/devices" and self._local():
                cfg = load_config()
                devices = discover_devices(cfg)
                if devices:
                    cfg["device"] = devices[0]["ip"]
                    save_config(cfg)
                self._send(200, "application/json", json.dumps(devices))
            elif path == "/calendar.json" and (self._from_device() or self._local()):
                self._send(200, "application/json", json.dumps(CAL.range_json(), ensure_ascii=False))
            elif path == "/api/calendar" and self._local():
                self._send(200, "application/json", json.dumps(CAL.status()))
            elif path == "/api/notify" and self._local():
                self._send(200, "application/json", json.dumps(notify_settings()))
            elif path == "/api/hooks" and self._local():
                self._send(200, "application/json", json.dumps(self._hooks(hooks_status(port))))
            elif path == "/":
                self._send(200, "text/plain; charset=utf-8", f"Ritmo Code PC Monitor {VERSION}: dati su /data.json")
            else:
                self._send(404, "text/plain", "not found")

        @staticmethod
        def _hooks(result):
            fields = CLAUDE.fields()
            if "cc_ev_id" in fields:
                result["last"] = {"ev": fields["cc_ev"], "proj": fields["cc_ev_proj"], "age": fields["cc_ev_age"]}
            return result

        def _body(self):
            length = min(int(self.headers.get("Content-Length", "0") or 0), 1 << 20)
            return json.loads(self.rfile.read(length).decode("utf-8", "replace") or "{}")

        def _from_device(self):
            ip = self.client_address[0]
            return ip in (DEVICE_SEEN["ip"], load_config().get("device"))

        def do_POST(self):
            path = self.path.split("?")[0]
            if path == "/notify" and self._from_device():   # avviso dal dispositivo: suono e notifica
                try:
                    length = min(int(self.headers.get("Content-Length", "0") or 0), 4096)
                    form = urllib.parse.parse_qs(self.rfile.read(length).decode("utf-8", "replace"))
                except Exception:
                    form = {}
                self.send_response(204)
                self.send_header("Connection", "close")
                self.end_headers()
                get = lambda k: (form.get(k) or [""])[0][:120]
                threading.Thread(target=pc_notify, args=(get("ev"), get("title"), get("msg")), daemon=True).start()
                return
            if not self._local():
                self._send(404, "text/plain", "not found")
                return
            if path == "/api/calendar":
                CAL.refresh()
                self._send(200, "application/json", json.dumps({"ok": True}))
                return
            if path == "/api/notify":
                try:
                    req = self._body()
                except Exception:
                    req = {}
                cfg = load_config()
                for k, v in DEFAULT_NOTIFY.items():
                    if k in req:
                        if k == "volume":
                            cfg[k] = max(0, min(100, int(req[k])))
                        elif k == "style":
                            cfg[k] = "ritmo" if req[k] == "ritmo" else "windows"
                        else:
                            cfg[k] = bool(req[k])
                save_config(cfg)
                if req.get("test"):
                    threading.Thread(target=pc_notify, args=("break", "Ritmo Code - prova",
                                                             "Cosi' suonano gli avvisi del dispositivo"), daemon=True).start()
                self._send(200, "application/json", json.dumps(notify_settings()))
                return
            if path.startswith("/claude/"):          # hook di Claude Code: nessuna risposta (finirebbe nel contesto)
                kind = path[len("/claude/"):]
                try:
                    payload = self._body()
                except Exception:
                    payload = {}
                self.send_response(204)
                self.send_header("Connection", "close")
                self.end_headers()
                event = CLAUDE.handle(kind, payload if isinstance(payload, dict) else {}) if kind in HOOK_EVENTS.values() else None
                if event:
                    threading.Thread(target=push_event, args=(event,), daemon=True).start()
                return
            if path == "/api/hooks":
                try:
                    install = bool(self._body().get("install"))
                except Exception:
                    self._send(400, "application/json", json.dumps({"ok": False, "error": "richiesta non valida"}))
                    return
                self._send(200, "application/json", json.dumps(self._hooks(hooks_write(port, install))))
                return
            if path != "/api/pair":
                self._send(404, "text/plain", "not found")
                return
            try:
                req = self._body()
                ip, pin = str(req.get("ip", "")), str(req.get("pin", ""))
            except Exception:
                self._send(400, "application/json", json.dumps({"ok": False, "error": "richiesta non valida"}))
                return
            if not re.fullmatch(r"[0-9a-zA-Z.\-]{1,64}", ip) or not re.fullmatch(r"\d{4}", pin):
                self._send(200, "application/json",
                           json.dumps({"ok": False, "error": "scegli il dispositivo e inserisci il PIN di 4 cifre"}))
                return
            result = pair_device(ip, pin, port)
            if result.get("ok"):
                cfg = load_config()
                cfg["device"] = ip
                save_config(cfg)
            self._send(200, "application/json", json.dumps(result))

        def log_message(self, *args):
            pass

    return Handler


# ---------------------------------------------------------------------------
# Icona nell'area di notifica (Win32 via ctypes, nessuna dipendenza)
# ---------------------------------------------------------------------------

DEVICE_SEEN = {"ip": None, "at": 0.0}      # ultimo dispositivo che ha letto /data.json

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class _WndClass(ctypes.Structure):
    _fields_ = [("style", wt.UINT), ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE), ("hIcon", wt.HICON), ("hCursor", wt.HANDLE), ("hbrBackground", wt.HBRUSH),
                ("lpszMenuName", wt.LPCWSTR), ("lpszClassName", wt.LPCWSTR)]


class _NotifyIconData(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("hWnd", wt.HWND), ("uID", wt.UINT), ("uFlags", wt.UINT),
                ("uCallbackMessage", wt.UINT), ("hIcon", wt.HICON), ("szTip", ctypes.c_wchar * 128),
                ("dwState", wt.DWORD), ("dwStateMask", wt.DWORD), ("szInfo", ctypes.c_wchar * 256),
                ("uVersion", wt.UINT), ("szInfoTitle", ctypes.c_wchar * 64), ("dwInfoFlags", wt.DWORD),
                ("guidItem", ctypes.c_byte * 16), ("hBalloonIcon", wt.HICON)]


# ---------------------------------------------------------------------------
# Timer e pomodoro sul dispositivo, dal menu dell'icona
# ---------------------------------------------------------------------------

# id del menu -> (azione, parametri, testo)
TIMER_ITEMS = {
    20: ("pomo", {"p": 0}, "Pomodoro 25/5"),
    21: ("pomo", {"p": 1}, "Pomodoro 50/10"),
    22: ("pomo", {"p": 2}, "Pomodoro 15/3"),
    23: ("timer", {"m": 5}, "Timer 5 min"),
    24: ("timer", {"m": 10}, "Timer 10 min"),
    25: ("timer", {"m": 15}, "Timer 15 min"),
    26: ("timer", {"m": 30}, "Timer 30 min"),
    27: ("skip", {}, "Salta fase"),
    28: ("plus", {}, "+5 min"),
    29: ("stop", {}, "Ferma"),
}


def device_ip():
    return DEVICE_SEEN["ip"] if DEVICE_SEEN["ip"] and time.time() - DEVICE_SEEN["at"] < 600 else load_config().get("device")


def device_timer(ip):
    """Stato del timer sul dispositivo ({mode, left, label}), None se non risponde (spento o bloccato dal PIN)."""
    try:
        return get_json(f"http://{ip}/api/status", 1.0).get("timer")
    except Exception:
        return None


def timer_command(ip, action, params):
    body = urllib.parse.urlencode({"a": action, **params}).encode()
    try:
        with urllib.request.urlopen(urllib.request.Request(f"http://{ip}/timer", data=body, method="POST"), timeout=3) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as error:
        return {"ok": False, "error": "comando non valido adesso" if error.code == 409 else f"HTTP {error.code}"}
    except Exception:
        return {"ok": False, "error": "dispositivo non raggiungibile"}


# ---------------------------------------------------------------------------
# Calendario: prossimi eventi dal link iCal impostato nel pannello del dispositivo
# ---------------------------------------------------------------------------

def _ics_lines(text):
    """Righe logiche di un file iCal (le righe che iniziano con spazio continuano la precedente)."""
    out = []
    for raw in text.replace("\r\n", "\n").split("\n"):
        if raw[:1] in (" ", "\t") and out:
            out[-1] += raw[1:]
        elif raw:
            out.append(raw)
    return out


def _ics_prop(line):
    """'DTSTART;TZID=Europe/Rome:20260919T140000' -> ('DTSTART', {'TZID': 'Europe/Rome'}, '20260919T140000')"""
    head, _, value = line.partition(":")
    parts = head.split(";")
    params = {}
    for p in parts[1:]:
        k, _, v = p.partition("=")
        params[k.upper()] = v
    return parts[0].upper(), params, value


def _ics_time(value, params):
    """(epoch, tutto il giorno). Ora UTC con Z; altrimenti ora locale del PC (TZID del calendario = fuso del PC)."""
    import calendar
    import datetime as dt
    v = value.strip()
    if params.get("VALUE") == "DATE" or len(v) == 8:
        d = dt.datetime.strptime(v[:8], "%Y%m%d")
        return time.mktime(d.timetuple()), True
    d = dt.datetime.strptime(v[:15], "%Y%m%dT%H%M%S")
    if v.endswith("Z"):
        return calendar.timegm(d.timetuple()), False
    return time.mktime(d.timetuple()), False


def _ics_duration(v):
    m = re.fullmatch(r"([+-])?P(?:(\d+)W)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?)?", v.strip())
    if not m:
        return 0
    w, d, h, mi, s = (int(x or 0) for x in m.groups()[1:])
    return (((w * 7 + d) * 24 + h) * 60 + mi) * 60 + s


def _ics_occurrences(start, rule, exdates, until_limit):
    """Inizi delle ripetizioni fino a until_limit: FREQ DAILY/WEEKLY(BYDAY)/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL."""
    import datetime as dt
    r = dict(p.partition("=")[::2] for p in rule.split(";") if "=" in p)
    freq, step = r.get("FREQ", ""), max(1, int(r.get("INTERVAL", "1") or 1))
    count = int(r["COUNT"]) if r.get("COUNT", "").isdigit() else None
    until = _ics_time(r["UNTIL"], {})[0] if r.get("UNTIL") else None
    days = {"MO": 0, "TU": 1, "WE": 2, "TH": 3, "FR": 4, "SA": 5, "SU": 6}
    byday = sorted(days[d[-2:]] for d in r.get("BYDAY", "").split(",") if d[-2:] in days)
    base = dt.datetime.fromtimestamp(start)
    out, n, i = [], 0, 0
    while i < 3000:
        if freq == "DAILY":
            cands = [base + dt.timedelta(days=i * step)]
        elif freq == "WEEKLY":
            week = base - dt.timedelta(days=base.weekday()) + dt.timedelta(weeks=i * step)
            cands = [week + dt.timedelta(days=d) for d in (byday or [base.weekday()])]
            cands = [c for c in cands if c >= base]
        elif freq == "MONTHLY":
            mo = base.month - 1 + i * step
            try:
                cands = [base.replace(year=base.year + mo // 12, month=mo % 12 + 1)]
            except ValueError:                    # 31 in un mese piu' corto: saltato, come fa Google
                cands = []
        elif freq == "YEARLY":
            try:
                cands = [base.replace(year=base.year + i * step)]
            except ValueError:
                cands = []
        else:
            return [start]
        i += 1
        for c in cands:
            t = time.mktime(c.timetuple())
            if (until is not None and t > until) or t > until_limit:
                return out
            n += 1
            if int(t) not in exdates:
                out.append(t)
            if count is not None and n >= count:
                return out
    return out


def ics_events(text, start_t, end_t):
    """Eventi (inizio, fine, titolo, tutto il giorno) che toccano [start_t, end_t), ricorrenze comprese, senza gli annullati."""
    events, overrides = [], {}
    cur = None
    for line in _ics_lines(text):
        if line == "BEGIN:VEVENT":
            cur = {"exdate": set()}
        elif line == "END:VEVENT" and cur is not None:
            events.append(cur)
            cur = None
        elif cur is not None:
            name, params, value = _ics_prop(line)
            if name in ("DTSTART", "DTEND", "RECURRENCE-ID"):
                cur[name] = _ics_time(value, params)
            elif name == "EXDATE":
                for v in value.split(","):
                    cur["exdate"].add(int(_ics_time(v, params)[0]))
            elif name in ("SUMMARY", "RRULE", "UID", "STATUS", "DURATION"):
                cur[name] = value
    end_limit = end_t
    found = []
    for e in events:
        if "DTSTART" not in e or e.get("STATUS", "").upper() == "CANCELLED":
            if "RECURRENCE-ID" in e:                       # un'occorrenza annullata
                overrides[(e.get("UID"), int(e["RECURRENCE-ID"][0]))] = None
            continue
        start, allday = e["DTSTART"]
        dur = e["DTEND"][0] - start if "DTEND" in e else (86400 if allday else _ics_duration(e.get("DURATION", "PT1H")))
        title = e.get("SUMMARY", "").replace("\\,", ",").replace("\\;", ";").replace("\\n", " ").replace("\\", "")
        if "RECURRENCE-ID" in e:                           # occorrenza spostata di un evento ricorrente
            overrides[(e.get("UID"), int(e["RECURRENCE-ID"][0]))] = (start, start + dur, title, allday)
            continue
        starts = _ics_occurrences(start, e["RRULE"], e["exdate"], end_limit) if "RRULE" in e else [start]
        for s in starts:
            found.append((e.get("UID"), int(s), (s, s + dur, title, allday)))
    result = []
    for uid, key, ev in found:
        if (uid, key) in overrides:
            continue                                       # sostituita (o annullata) da un'eccezione
        result.append(ev)
    result += [ev for ev in overrides.values() if ev]
    result = [ev for ev in result if ev[1] > start_t and ev[0] < end_t]
    result.sort()
    return result


def ics_upcoming(text, now, horizon_days=7, limit=3):
    """Prossimi eventi (inizio, fine, titolo) non ancora finiti, senza quelli di tutto il giorno."""
    return [ev[:3] for ev in ics_events(text, now, now + horizon_days * 86400) if not ev[3]][:limit]


def cal_range_bounds(now):
    """Periodo per le viste del dispositivo: dal primo del mese a sei settimane da oggi."""
    import datetime as dt
    d = dt.datetime.fromtimestamp(now)
    first = d.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
    return time.mktime(first.timetuple()), now + 42 * 86400


def _cal_title(t):
    """Titolo leggibile dal dispositivo: niente virgolette/escape, solo caratteri del suo font (Latin-1)."""
    t = t.replace('"', "'").replace("\\", "/")
    t = "".join(ch for ch in t if ch >= " " and ord(ch) <= 0xFF)
    return t.strip()[:60] or "(senza titolo)"


class Calendar(threading.Thread):
    """Ogni 5 minuti: link dal dispositivo, file iCal, prossimi 3 eventi per data.json."""

    INTERVAL = 300

    def __init__(self):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.events, self.state, self.at = None, "in attesa del dispositivo", 0
        self.range = []
        self.wake = threading.Event()

    def refresh(self):
        self.wake.set()

    def run(self):
        while True:
            self._update()
            self.wake.wait(self.INTERVAL)
            self.wake.clear()

    def _update(self):
        ip = device_ip()
        if not ip:
            return self._set(None, "nessun dispositivo collegato")
        try:
            url = get_json(f"http://{ip}/api/ical", 3).get("url", "")
        except Exception:
            return self._set(self.events, "dispositivo non raggiungibile (eventi precedenti mantenuti)")
        if not url:
            return self._set(None, "nessun link: impostalo nel pannello del dispositivo, pagina /home")
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ritmo-code-pc-monitor"})
            with urllib.request.urlopen(req, timeout=20) as r:
                text = r.read(8 * 1024 * 1024).decode("utf-8", "replace")
            now = time.time()
            events = ics_upcoming(text, now)
            rng = ics_events(text, *cal_range_bounds(now))
            with self.lock:
                self.range = rng[:120]                     # per le viste giorno/settimana/mese del dispositivo
            self._set(events, f"{len(events)} eventi nei prossimi 7 giorni")
        except Exception as error:
            self._set(self.events, f"download non riuscito: {error}")

    def _set(self, events, state):
        with self.lock:
            self.events, self.state, self.at = events, state, time.time()

    def fields(self):
        with self.lock:
            if self.events is None:
                return {}
            data = {"cal_n": len(self.events)}
            for i, (s, e, t) in enumerate(self.events):
                data.update({f"cal{i}_t": _cal_title(t), f"cal{i}_s": int(s), f"cal{i}_e": int(e)})
            return data

    def range_json(self):
        """Eventi del periodo per le viste del dispositivo (righe corte: [inizio, fine, tutto il giorno, titolo])."""
        with self.lock:
            return {"n": len(self.range), "ev": [[int(s), int(e), 1 if a else 0, _cal_title(t)] for s, e, t, a in self.range]}

    def status(self):
        with self.lock:
            nxt = self.events[0] if self.events else None
            return {"state": self.state, "at": int(self.at),
                    "next": {"title": nxt[2], "start": int(nxt[0])} if nxt else None}


CAL = Calendar()


class TrayIcon:
    """Icona con suggerimento aggiornato e menu: pagina di collegamento, pannello del dispositivo, esci."""

    WM_TRAY = 0x8000 + 1          # WM_APP + 1
    WM_BALLOON = 0x8000 + 2       # notifica chiesta da un altro thread
    WM_TIMER, WM_DESTROY, WM_COMMAND = 0x0113, 0x0002, 0x0111
    ID_OPEN, ID_DEVICE, ID_EXIT = 1, 2, 3

    def __init__(self, sampler, port):
        self.sampler, self.port = sampler, port
        self.user32, self.shell32, self.kernel32 = ctypes.windll.user32, ctypes.windll.shell32, ctypes.windll.kernel32
        u = self.user32
        u.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        u.DefWindowProcW.restype = LRESULT
        u.CreateWindowExW.restype = wt.HWND
        u.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
        u.CreatePopupMenu.restype = wt.HMENU
        u.AppendMenuW.argtypes = [wt.HMENU, wt.UINT, ctypes.c_size_t, wt.LPCWSTR]
        u.TrackPopupMenu.argtypes = [wt.HMENU, wt.UINT, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.HWND, wt.LPVOID]
        u.DestroyMenu.argtypes = [wt.HMENU]
        u.SetForegroundWindow.argtypes = [wt.HWND]
        u.SetTimer.argtypes = [wt.HWND, ctypes.c_size_t, wt.UINT, wt.LPVOID]
        u.SetTimer.restype = ctypes.c_size_t
        u.LoadImageW.argtypes = [wt.HINSTANCE, wt.LPCWSTR, wt.UINT, ctypes.c_int, ctypes.c_int, wt.UINT]
        u.LoadImageW.restype = wt.HANDLE
        u.LoadIconW.argtypes = [wt.HINSTANCE, wt.LPVOID]
        u.LoadIconW.restype = wt.HICON
        u.RegisterWindowMessageW.restype = wt.UINT
        u.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
        self.balloon_text = None
        self.shell32.Shell_NotifyIconW.argtypes = [wt.DWORD, ctypes.POINTER(_NotifyIconData)]
        self.shell32.ExtractIconW.argtypes = [wt.HINSTANCE, wt.LPCWSTR, wt.UINT]   # handle a 64 bit
        self.shell32.ExtractIconW.restype = wt.HICON
        self.kernel32.GetModuleHandleW.restype = wt.HINSTANCE
        self.taskbar_created = None                  # la finestra riceve messaggi gia' durante la creazione
        self._proc = WNDPROC(self._wndproc)          # riferimento tenuto vivo
        self.hinst = self.kernel32.GetModuleHandleW(None)
        cls = _WndClass(lpfnWndProc=self._proc, hInstance=self.hinst, lpszClassName="RitmoCodePcMonitorTray")
        u.RegisterClassW(ctypes.byref(cls))
        self.hwnd = u.CreateWindowExW(0, "RitmoCodePcMonitorTray", "Ritmo Code PC Monitor", 0, 0, 0, 0, 0, None, None, self.hinst, None)
        self.taskbar_created = u.RegisterWindowMessageW("TaskbarCreated")
        self.nid = _NotifyIconData()
        self.nid.cbSize = ctypes.sizeof(_NotifyIconData)
        self.nid.hWnd = self.hwnd
        self.nid.uID = 1
        self.nid.uFlags = 0x1 | 0x2 | 0x4               # NIF_MESSAGE | NIF_ICON | NIF_TIP
        self.nid.uCallbackMessage = self.WM_TRAY
        self.nid.hIcon = self._icon()
        self.nid.szTip = self._tip()
        self.shell32.Shell_NotifyIconW(0, ctypes.byref(self.nid))    # NIM_ADD
        u.SetTimer(self.hwnd, 1, 3000, None)

    def _icon(self):
        if getattr(sys, "frozen", False):
            icon = self.shell32.ExtractIconW(self.hinst, sys.executable, 0)
            if icon and icon > 1:
                return icon
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.ico")
        icon = self.user32.LoadImageW(None, path, 1, 16, 16, 0x10) if os.path.exists(path) else None   # LR_LOADFROMFILE
        return icon or self.user32.LoadIconW(None, ctypes.c_void_p(32512))                            # IDI_APPLICATION

    def _device_line(self):
        ip, at = DEVICE_SEEN["ip"], DEVICE_SEEN["at"]
        if ip and time.time() - at < 90:
            return f"dispositivo {ip} collegato"
        return "nessun dispositivo collegato"

    def _tip(self):
        d = self.sampler.snapshot()
        r = lambda v: "--" if v is None else f"{v:.0f}"
        lines = [f"Ritmo Code PC Monitor {VERSION}",
                 f"cpu {r(d.get('cpu_load'))}% · gpu {r(d.get('gpu_load'))}% · ram {r(d.get('ram_load'))}%",
                 self._device_line()]
        nxt = CAL.status().get("next")
        if nxt:
            when = time.strftime("%H:%M", time.localtime(nxt["start"]))
            day = "" if time.strftime("%d", time.localtime(nxt["start"])) == time.strftime("%d") else time.strftime("%d/%m ", time.localtime(nxt["start"]))
            lines.append(f"prossimo: {day}{when} {nxt['title'][:40]}")
        return "\n".join(lines)[:127]

    def _menu(self):
        u = self.user32
        menu = u.CreatePopupMenu()
        u.AppendMenuW(menu, 0x1, 0, f"Ritmo Code PC Monitor {VERSION}")          # MF_GRAYED
        u.AppendMenuW(menu, 0x1, 0, self._device_line())
        u.AppendMenuW(menu, 0x800, 0, None)                                       # separatore
        ip = device_ip()
        u.AppendMenuW(menu, 0x10 | (0 if ip else 0x1), self._timer_menu(ip), "Timer e pomodoro")   # MF_POPUP
        u.AppendMenuW(menu, 0x800, 0, None)
        u.AppendMenuW(menu, 0, self.ID_OPEN, "Apri stato e collegamento")
        device = DEVICE_SEEN["ip"] or load_config().get("device")
        u.AppendMenuW(menu, 0 if device else 0x1, self.ID_DEVICE, "Apri il pannello del dispositivo")
        u.AppendMenuW(menu, 0x800, 0, None)
        u.AppendMenuW(menu, 0, self.ID_EXIT, "Esci")
        pt = wt.POINT()
        u.GetCursorPos(ctypes.byref(pt))
        u.SetForegroundWindow(self.hwnd)
        cmd = u.TrackPopupMenu(menu, 0x100 | 0x2, pt.x, pt.y, 0, self.hwnd, None)   # TPM_RETURNCMD | TPM_RIGHTBUTTON
        u.DestroyMenu(menu)
        self._command(cmd, device)

    def _timer_menu(self, ip):
        u = self.user32
        sub = u.CreatePopupMenu()
        st = device_timer(ip) if ip else None
        mode = (st or {}).get("mode", 0)
        head = st.get("label") if st and mode else ("nessun timer in corso" if st else "dispositivo non raggiungibile")
        u.AppendMenuW(sub, 0x1, 0, head)
        u.AppendMenuW(sub, 0x800, 0, None)
        for i in (20, 21, 22):
            u.AppendMenuW(sub, 0, i, TIMER_ITEMS[i][2])
        u.AppendMenuW(sub, 0x800, 0, None)
        for i in (23, 24, 25, 26):
            u.AppendMenuW(sub, 0, i, TIMER_ITEMS[i][2])
        u.AppendMenuW(sub, 0x800, 0, None)
        u.AppendMenuW(sub, 0 if mode in (2, 3) else 0x1, 27, TIMER_ITEMS[27][2])   # salta fase: solo pomodoro
        u.AppendMenuW(sub, 0 if mode == 1 else 0x1, 28, TIMER_ITEMS[28][2])        # +5 min: solo timer
        u.AppendMenuW(sub, 0 if mode else 0x1, 29, TIMER_ITEMS[29][2])
        return sub

    def _timer(self, cmd):
        ip = device_ip()
        action, params, text = TIMER_ITEMS[cmd]
        result = timer_command(ip, action, params) if ip else {"ok": False, "error": "nessun dispositivo collegato"}
        if result.get("ok"):
            msg = "timer fermato" if action == "stop" else (result.get("label") or text)
            self.balloon("Ritmo Code", f"{msg} sul dispositivo")
        else:
            self.balloon("Ritmo Code", f"{text}: {result.get('error', 'non riuscito')}")

    def _command(self, cmd, device=None):
        import webbrowser
        if cmd in TIMER_ITEMS:
            threading.Thread(target=self._timer, args=(cmd,), daemon=True).start()
            return
        if cmd == self.ID_OPEN:
            webbrowser.open(f"http://127.0.0.1:{self.port}/")
        elif cmd == self.ID_DEVICE and device:
            webbrowser.open(f"http://{device}/")
        elif cmd == self.ID_EXIT:
            self.shell32.Shell_NotifyIconW(2, ctypes.byref(self.nid))            # NIM_DELETE
            os._exit(0)

    def balloon(self, title, msg):
        """Notifica di Windows dall'icona (qualsiasi thread: la mostra il thread della finestra)."""
        self.balloon_text = (title, msg)
        self.user32.PostMessageW(self.hwnd, self.WM_BALLOON, 0, 0)

    def _show_balloon(self):
        if not self.balloon_text:
            return
        title, msg = self.balloon_text
        self.balloon_text = None
        self.nid.uFlags = 0x1 | 0x2 | 0x4 | 0x10          # + NIF_INFO
        self.nid.szInfoTitle = title[:63]
        self.nid.szInfo = (msg or " ")[:255]
        self.nid.dwInfoFlags = 0x1 | 0x10                 # NIIF_INFO | NIIF_NOSOUND (il suono lo facciamo noi)
        self.shell32.Shell_NotifyIconW(1, ctypes.byref(self.nid))
        self.nid.uFlags = 0x1 | 0x2 | 0x4

    def _wndproc(self, hwnd, msg, wparam, lparam):
        if msg == self.WM_BALLOON:
            self._show_balloon()
            return 0
        if msg == self.WM_TRAY:
            event = lparam & 0xFFFF
            if event in (0x0205, 0x007B):            # WM_RBUTTONUP, WM_CONTEXTMENU
                self._menu()
            elif event == 0x0203:                    # WM_LBUTTONDBLCLK
                self._command(self.ID_OPEN)
            return 0
        if msg == self.WM_TIMER:
            self.nid.szTip = self._tip()
            self.shell32.Shell_NotifyIconW(1, ctypes.byref(self.nid))            # NIM_MODIFY
            return 0
        if msg == self.taskbar_created:              # Esplora risorse riavviato: si rimette l'icona
            self.shell32.Shell_NotifyIconW(0, ctypes.byref(self.nid))
            return 0
        if msg == self.WM_DESTROY:
            self.shell32.Shell_NotifyIconW(2, ctypes.byref(self.nid))
            self.user32.PostQuitMessage(0)
            return 0
        return self.user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def run(self):
        msg = wt.MSG()
        while self.user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            self.user32.TranslateMessage(ctypes.byref(msg))
            self.user32.DispatchMessageW(ctypes.byref(msg))


def main():
    parser = argparse.ArgumentParser(description="Statistiche del PC per il dispositivo Ritmo Code")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"porta HTTP (default {DEFAULT_PORT})")
    parser.add_argument("--drive", default="C", help="disco principale (default C)")
    parser.add_argument("--interval", type=float, default=1.0, help="secondi tra le letture (default 1)")
    parser.add_argument("--lhm-url", default="http://127.0.0.1:8085/data.json", help="URL data.json di LibreHardwareMonitor")
    parser.add_argument("--no-lhm", action="store_true", help="non usare LibreHardwareMonitor")
    parser.add_argument("--print", action="store_true", help="stampa i dati letti e termina (diagnostica)")
    parser.add_argument("--no-tray", action="store_true", help="senza icona nell'area di notifica")
    args = parser.parse_args()
    if sys.stdout:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    windows = WindowsSensors(args.drive)
    lhm = None if args.no_lhm else LibreHardwareMonitor(args.lhm_url)
    sampler = Sampler(windows, lhm, args.interval)
    sampler.start()
    time.sleep(min(1.5, args.interval + 0.5))

    if args.print:
        print(json.dumps(sampler.snapshot(), indent=1, ensure_ascii=False))
        return

    print(f"Ritmo Code PC Monitor {VERSION} - {windows.cpu_name}")
    print(f"Dati per il dispositivo: http://{ip_towards()}:{args.port}/data.json")
    print(f"Stato e collegamento:    http://127.0.0.1:{args.port}/")
    if lhm:
        print("LibreHardwareMonitor: " + ("connesso" if lhm.online else "non raggiungibile (temperature, consumi e ventole a 0)"))
    try:
        server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(sampler, lhm, args.port))
    except OSError as error:
        message = (f"Impossibile usare la porta {args.port}: {error}\n"
                   f"Ritmo Code PC Monitor e' forse gia' in esecuzione?")
        if sys.stdout:
            print(message)
            input("Premi Invio per chiudere...")
        else:
            ctypes.windll.user32.MessageBoxW(None, message, "Ritmo Code PC Monitor", 0x10)
        sys.exit(1)
    CAL.start()                         # calendario: link dal dispositivo, eventi ogni 5 minuti
    if args.no_tray:
        server.serve_forever()
        return
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        global TRAY
        TRAY = TrayIcon(sampler, args.port)
        TRAY.run()
    except Exception as error:          # senza area di notifica (es. sessione senza desktop): solo server
        print(f"Icona nell'area di notifica non disponibile: {error}")
        while True:
            time.sleep(3600)


if __name__ == "__main__":
    main()
