"""Log Satisfactory process memory once a minute during normal play on Windows."""

import csv
from datetime import datetime
from pathlib import Path
import subprocess

from sample_projection_memory import WindowsMemory


def main():
    pids = subprocess.check_output([
        "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
        "(Get-Process -Name 'FactoryGame*-Win64-Shipping' -ErrorAction Ignore).Id; exit 0",
    ], text=True).split()
    if len(pids) != 1:
        raise SystemExit("Сначала запустите один экземпляр Satisfactory и повторите команду.")

    pid = int(pids[0])
    directory = Path(__file__).resolve().parents[1] / "Saved" / "session-memory"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{datetime.now():%Y-%m-%d_%H-%M-%S_%f}_{pid}.csv"
    memory = WindowsMemory(pid, luid="", ram_only=True)
    try:
        with path.open("x", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["local_time", "pid", "private_bytes_mib", "private_working_set_mib"])
            print(f"Запись раз в минуту в {path}\nДля остановки нажмите Ctrl+C.", flush=True)
            while memory.running():
                sample = memory.measure()
                writer.writerow([datetime.now().astimezone().isoformat(timespec="seconds"), pid,
                                 round(sample["private_bytes_mib"], 2), round(sample["private_working_set_mib"], 2)])
                stream.flush()
                memory.wait(memory.process, 60_000)
    except KeyboardInterrupt:
        pass
    finally:
        memory.close()


if __name__ == "__main__":
    main()
