"""Run the F10 Shipping memory routes against the isolated F08 client."""

import argparse
import ctypes as ct
from ctypes import wintypes as win
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from sample_projection_memory import bind


ROOT = Path(__file__).resolve().parents[1]
DLL_NAME = "FactoryGameSteam-{}-Win64-Shipping.dll"
SLOTS = ("SAQ_F02_000_PILOT", "SAQ_F02_100_PILOT")
ORDER = (
    (1, False, "cycle"), (1, True, "idle"), (1, True, "cycle"), (1, False, "idle"),
    (2, True, "cycle"), (2, False, "idle"), (2, False, "cycle"), (2, True, "idle"),
)


def process_names():
    class Entry(ct.Structure):
        _fields_ = [
            ("size", win.DWORD), ("usage", win.DWORD), ("pid", win.DWORD),
            ("heap", ct.c_size_t), ("module", win.DWORD), ("threads", win.DWORD),
            ("parent", win.DWORD), ("priority", win.LONG), ("flags", win.DWORD),
            ("name", win.WCHAR * 260),
        ]

    kernel = ct.WinDLL("kernel32", use_last_error=True)
    snapshot = bind(kernel, "CreateToolhelp32Snapshot", win.HANDLE, win.DWORD, win.DWORD)(2, 0)
    if snapshot == ct.c_void_p(-1).value:
        raise ct.WinError(ct.get_last_error())
    first = bind(kernel, "Process32FirstW", win.BOOL, win.HANDLE, ct.POINTER(Entry))
    advance = bind(kernel, "Process32NextW", win.BOOL, win.HANDLE, ct.POINTER(Entry))
    close = bind(kernel, "CloseHandle", win.BOOL, win.HANDLE)
    names = []
    try:
        entry = Entry()
        entry.size = ct.sizeof(entry)
        if not first(snapshot, ct.byref(entry)):
            raise ct.WinError(ct.get_last_error())
        while True:
            names.append(entry.name.lower())
            if not advance(snapshot, ct.byref(entry)):
                if ct.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise ct.WinError(ct.get_last_error())
                return names
    finally:
        close(snapshot)


def hash_file(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest().upper()


def input_slots():
    save_root = Path(os.environ["LOCALAPPDATA"]) / "FactoryGame/Saved/SaveGames"
    result = {}
    for slot in SLOTS:
        matches = list(save_root.rglob(f"{slot}.sav"))
        if len(matches) != 1:
            raise ValueError(f"Missing or ambiguous input slot: {slot}")
        result[slot] = matches[0]
    return result


def run(output, plugin, label, ism, route, cycles, diagnostic=False):
    names = process_names()
    if any(name.startswith("factorygame") for name in names):
        raise RuntimeError("Satisfactory is already running")
    if "steam.exe" not in names:
        steam = Path(os.environ["ProgramFiles(x86)"]) / "Steam/steam.exe"
        subprocess.Popen([steam, "-silent"], creationflags=subprocess.CREATE_NO_WINDOW)
        time.sleep(10)
    f08 = ROOT / "Saved/F-projection-performance/F08"
    trial = json.loads((f08 / "hashes.json").read_text(encoding="utf-8-sig"))
    client = Path(trial["client"])
    dll = client / "FactoryGame/Mods/GameFeatures/ShadowArchitect/Binaries/Win64" / DLL_NAME.format("ShadowArchitect")
    if hash_file(trial["archive"]) != trial["archive_sha256"] or hash_file(dll) != trial["dll_sha256"]:
        raise ValueError("F08 archive or installed DLL changed")

    slots = input_slots()
    hashes = {slot: hash_file(path) for slot, path in slots.items()}
    log = output / f"{label}.log"
    profile = output / f"{label}-profile"
    profile.mkdir()
    menu = "/Game/FactoryGame/Map/MenuScenes/Map_Menu_1_02.Map_Menu_1_02"
    maps = "[/Script/EngineSettings.GameMapsSettings]"
    settle, window = 0.9, 0.9
    command = [
        str(client / "FactoryGameSteam.exe"), f"-UserDir={profile}", f"-ABSLOG={log}",
        f"-FGBlueprintDir={f08.parent / 'Blueprints'}", f"-PLUGIN={plugin}", "-EnablePlugins=SaveCheck",
        f"-ini:Engine:{maps}:GameDefaultMap={menu},{maps}:LocalMapOptions=",
        "-unattended", "-NoSplash", "-NoSound", "-RenderOffscreen", "-windowed", "-ResX=1280", "-ResY=720",
        "-NoCrashDialog", "-dx12", "-graphicsadapter=0", "-ExecCmds=t.MaxFPS 0,r.VSync 0,t.IdleWhenNotForeground 0",
        "-SaveCheckProjectionMemory", f"-SaveCheckMemoryCycles={cycles}", f"-SaveCheckMemoryRoute={route}",
        f"-SaveCheckMemorySettle={settle}", f"-SaveCheckMemoryWindow={window}",
    ]
    if ism:
        command.append("-ShadowPerPlanISM")
    with (output / f"{label}-identity.json").open("x", encoding="utf-8") as stream:
        json.dump({
            "archive_sha256": trial["archive_sha256"], "mod_dll_sha256": trial["dll_sha256"],
            "savecheck_sha256": hash_file(plugin.parent / "Binaries/Win64" / DLL_NAME.format("SaveCheck")),
            "save_sha256": hashes, "ism": ism, "route": route, "cycles": cycles,
            "grid_settle_seconds": settle, "observation_seconds": window, "command": command,
        }, stream, indent=2)
    print(f"Starting {label}", flush=True)
    launcher = subprocess.Popen(command, cwd=client, creationflags=subprocess.CREATE_NO_WINDOW)
    subprocess.run([
        sys.executable, "-B", str(ROOT / "scripts/sample_projection_memory.py"),
        "--log", str(log), "--output", str(output / f"{label}.csv"), "--dll", str(dll),
        "--ism", str(int(ism)), "--ram-only", "--timeout", str(480 + 6 * cycles),
        *(["--regions-output", str(output / f"{label}-regions.csv")] if diagnostic else []),
    ], check=True)
    launcher.wait(timeout=30)
    if hash_file(dll) != trial["dll_sha256"] or any(hash_file(path) != hashes[slot] for slot, path in slots.items()):
        raise ValueError(f"Input changed during {label}")
    print(f"Verified {label}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "Saved/F10-memory")
    parser.add_argument("--plugin", type=Path, help="SaveCheck descriptor, defaults to SaveCheck in the output directory")
    parser.add_argument("--pilot", action="store_true", help="Run two active cycles with ISM")
    parser.add_argument("--diagnostic", action="store_true", help="Repeat the four variants with OS region classification")
    parser.add_argument("--continue-series", action="store_true", help="Reuse previously verified runs in this output directory")
    args = parser.parse_args()
    output = args.output.resolve()
    plugin = (args.plugin or output / "SaveCheck/SaveCheck.uplugin").resolve()
    if args.pilot:
        run(output, plugin, "pilot", True, "cycle", 2)
        return
    for series, ism, route in (ORDER[:4] if args.diagnostic else ORDER):
        prefix = "diagnostic" if args.diagnostic else f"s{series}"
        label = f"{prefix}-{'ism' if ism else 'ordinary'}-{route}"
        if args.continue_series and (output / f"{label}.verified").is_file():
            continue
        run(output, plugin, label, ism, route, 30, args.diagnostic)
        (output / f"{label}.verified").touch(exist_ok=False)
        time.sleep(3)


if __name__ == "__main__":
    main()
