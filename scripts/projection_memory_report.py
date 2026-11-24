"""Validate F09 routes and produce per-window and per-pair memory results in MiB."""

import argparse
import csv
import json
from pathlib import Path
import re
from statistics import median


METRICS = ("private_working_set_mib", "private_bytes_mib", "dedicated_gpu_mib", "shared_gpu_mib")
EVENT = re.compile(
    r"LogSaveCheckProjectionMemory: Display: event=(\w+) phase=(\w+) step=(\d+) cycle=(\d+) mode=(\w+) qpc=(\d+)"
)
SNAPSHOT = re.compile(
    r"LogSaveCheckProjectionMemory: Display: snapshot step=(\d+) phase=\w+ mode=\w+ plans=(\d+)"
    r" actors=(\d+) meshes=(\d+) ism_groups=(\d+) ism_instances=(\d+) visual_instances=(\d+)"
)
CASE = re.compile(r"F09-([AB])-r(\d+)$")


def read_case(directory, case):
    path = directory / f"{case}.log"
    log = path.read_text(encoding="utf-8-sig")
    if "LogSaveCheckProjectionMemory: Display: pass=1 step=19 " not in log:
        raise ValueError(f"Route failed: {path}")
    identity = json.loads((directory / f"{case}-identity.json").read_text(encoding="utf-8-sig"))
    events = [
        {"case": case, "event": kind, "phase": phase, "step": int(step), "cycle": int(cycle), "mode": mode, "qpc": int(qpc)}
        for kind, phase, step, cycle, mode, qpc in EVENT.findall(log)
    ]
    snapshots = {int(step): tuple(map(int, counts)) for step, *counts in SNAPSHOT.findall(log)}
    expected_geometry = (100, 100, 600, 500, 3700, 3800) if identity["ism"] else (100, 100, 3800, 0, 0, 3800)
    for step in (1, *range(2, 18), 19):
        expected = (0, 0, 0, 0, 0, 0) if step in (1, 19) else (
            (100, 100, 100, 0, 0, 100) if (step - 2) % 3 == 2 else expected_geometry
        )
        if snapshots.get(step) != expected:
            raise ValueError(f"{case}: step {step} expected {expected}, found {snapshots.get(step)}")
    by_key = {(e["step"], e["event"]): e for e in events}
    for event in ("ready", "window_begin", "window_end"):
        if [e["step"] for e in events if e["event"] == event] != list(range(20)):
            raise ValueError(f"{case}: missing or duplicated {event}")
    if [e["step"] for e in events if e["event"] == "switch"] != list(range(3, 18)):
        raise ValueError(f"{case}: expected all 15 commands")
    if [e["step"] for e in events if e["event"] == "travel"] != [1, 2, 18, 19]:
        raise ValueError(f"{case}: unexpected world transitions")
    if not (by_key[18, "travel"]["qpc"] < by_key[18, "grid_cleanup"]["qpc"] < by_key[18, "ready"]["qpc"]):
        raise ValueError(f"{case}: no world cleanup before return to menu")
    with (directory / f"{case}.csv").open(encoding="utf-8-sig", newline="") as f:
        series = [{"case": case, **r, "qpc": int(r["qpc"]), **{metric: float(r[metric]) for metric in METRICS}}
                  for r in csv.DictReader(f)]
    process = {(r["pid"], r["luid"], r["adapter"], r["qpc_hz"], r["ism"]) for r in series}
    if len(process) != 1 or any(not r["gpu_instances"] for r in series):
        raise ValueError(f"{case}: process identity or GPU instances missing or changed")
    pid, luid, adapter, frequency, ism = process.pop()
    frequency = int(frequency)
    if bool(int(ism)) != identity["ism"] or any(a["qpc"] >= b["qpc"] for a, b in zip(series, series[1:])):
        raise ValueError(f"{case}: unexpected ISM or timestamps")

    def window(event, end, name=None, strict=True):
        start = event["qpc"]
        rows = [r for r in series if start <= r["qpc"] < end]
        if not rows or min(r["dedicated_gpu_mib"] for r in rows) <= 0:
            raise ValueError(f"{case}: dedicated GPU data missing around {event}")
        elapsed = (end - start) / frequency
        if strict:
            ticks = [start, *(r["qpc"] for r in rows), end]
            if len(rows) < elapsed * 4 or max(b - a for a, b in zip(ticks, ticks[1:])) > frequency * 0.55:
                raise ValueError(f"{case}: incomplete stable window at step {event['step']}")
        return {
            "case": case, "ism": int(ism), "pid": pid, "luid": luid, "adapter": adapter,
            "step": event["step"], "phase": name or event["phase"], "cycle": event["cycle"], "mode": event["mode"],
            "start_qpc": start, "end_qpc": end, "seconds": round(elapsed, 3), "samples": len(rows),
            **{f"{m}_{stat}": round(fn(r[m] for r in rows), 3)
               for m in METRICS for stat, fn in (("median", median), ("max", max))},
        }

    windows = [window(by_key[step, "window_begin"], by_key[step, "window_end"]["qpc"]) for step in range(20)]
    windows.append(window(by_key[2, "travel"], by_key[2, "window_begin"]["qpc"], "grid_load", strict=False))
    cleanup = by_key[18, "grid_cleanup"]
    windows.append(window(cleanup, cleanup["qpc"] + int(frequency * identity["observation_seconds"]),
                          "after_cleanup"))
    return identity, events, series, windows


def comparisons(pair, a, b):
    comparisons = [
        ("grid_ghost_0_minus_empty", 2, 1, "median"),
        ("grid_load_peak_minus_empty", 20, 1, "max"),
        ("grid_ghost_5_minus_empty", 17, 1, "median"),
        ("wireframe_5_minus_1", 15, 3, "median"),
        ("footprint_5_minus_1", 16, 4, "median"),
        ("after_cleanup_minus_menu_before", 21, 0, "median"),
        ("menu_after_minus_before", 18, 0, "median"),
        ("empty_after_minus_before", 19, 1, "median"),
    ]
    for cycle in range(1, 6):
        step = 2 + 3 * cycle
        comparisons.append((f"ghost_{cycle}_minus_0", step, 2, "median"))
        if cycle > 1:
            comparisons.append((f"ghost_{cycle}_minus_{cycle - 1}", step, step - 3, "median"))
    for label, current, baseline, stat in comparisons:
        for metric in METRICS:
            a_delta = a[current][f"{metric}_{stat}"] - a[baseline][f"{metric}_median"]
            b_delta = b[current][f"{metric}_{stat}"] - b[baseline][f"{metric}_median"]
            yield {
                "pair": pair, "comparison": label, "metric": metric,
                "A_mib": round(a_delta, 3), "B_mib": round(b_delta, 3), "B_minus_A_mib": round(b_delta - a_delta, 3),
            }


def save(path, rows):
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--check", help="Validate one pilot by its label")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.check:
        _, _, series, windows = read_case(args.directory, args.check)
        print(args.check, len(series), "samples", len(windows), "windows")
        return
    inputs = sorted(p.stem for p in args.directory.glob("F09-?-r*.log") if CASE.fullmatch(p.stem))
    pairs = {}
    all_series, all_windows, all_events = [], [], []
    reference = None
    for case in inputs:
        role, number = CASE.fullmatch(case).groups()
        identity, events, series, windows = read_case(args.directory, case)
        if identity.pop("ism") != (role == "B"):
            raise ValueError(f"Unexpected A/B mode: {case}")
        if reference is None:
            reference = identity
        if identity != reference:
            raise ValueError(f"Unmatched package, test plugin, inputs or timing in {case}")
        pairs.setdefault(int(number), {})[role] = windows
        all_series.extend(series)
        all_windows.extend(windows)
        all_events.extend(events)
    if len(pairs) < 3 or any(set(pair) != {"A", "B"} for pair in pairs.values()):
        raise ValueError("At least three complete pairs required")
    paired = [row for number, pair in pairs.items() for row in comparisons(number, pair["A"], pair["B"])]
    if args.output:
        for name, rows in (("timeseries", all_series), ("events", all_events), ("windows", all_windows), ("pairs", paired)):
            save(args.output / f"F09-{name}.csv", rows)
    for row in paired:
        if row["comparison"] in ("grid_ghost_0_minus_empty", "ghost_5_minus_0", "empty_after_minus_before"):
            print(*(row[key] for key in ("pair", "comparison", "metric", "A_mib", "B_mib", "B_minus_A_mib")))


if __name__ == "__main__":
    main()
