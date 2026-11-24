"""Validate the four F10 routes and report time-matched changes in private RAM."""

import argparse
import csv
import json
from pathlib import Path
import re
from statistics import median

from projection_memory_report import EVENT, SNAPSHOT, save
from run_projection_memory_trace import ORDER, SLOTS


RAM = ("private_working_set_mib", "private_bytes_mib")
MODES = ("Ghost", "Wireframe", "Footprint")
VIEWS = re.compile(
    r"views step=(\d+) cohort=(\w+) actors=(\d+) mesh=(\d+) ism=(\d+) registered=(\d+)"
    r" instances=(\d+) pending=(\d+) object_bytes=(\d+)"
)
ALLOCATOR = re.compile(
    r"allocator step=(\d+) name=(\w+) counters=([01]) allocated=(\d+) os_allocated=(\d+)"
    r"(?: os_cache=(\d+) immediate_cache=(\d+))?"
)
BUILD = re.compile(r"LogInit: Build: (\S+)")
LOADED_SLOT = re.compile(r"LogLoad: LoadMap: \S*\?loadgame=(\w+)\?sessionName=SAQMemory")
VIEW_FIELDS = ("actors", "mesh", "ism", "registered", "instances", "pending", "object_bytes")


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def read_run(directory, case):
    log = (directory / f"{case}.log").read_text(encoding="utf-8-sig")
    identity = json.loads((directory / f"{case}-identity.json").read_text(encoding="utf-8"))
    cycles = identity["cycles"]
    idle, ism = identity["route"] == "idle", identity["ism"]
    menu = 3 + 3 * cycles
    final = menu + 2
    if f"LogSaveCheckProjectionMemory: Display: pass=1 step={final} " not in log:
        raise ValueError(f"{case}: incomplete route")
    if LOADED_SLOT.findall(log) != [SLOTS[0], SLOTS[1], SLOTS[0]]:
        raise ValueError(f"{case}: unexpected fixture sequence")
    identity["game_build"] = BUILD.search(log)[1]
    events = [
        dict(case=case, event=event, phase=phase, step=int(step), cycle=int(cycle), mode=mode, qpc=int(qpc))
        for event, phase, step, cycle, mode, qpc in EVENT.findall(log)
    ]
    by_key = {(e["step"], e["event"]): e for e in events}
    for kind, expected in (
        ("ready", range(final + 1)), ("window_begin", range(final + 1)), ("window_end", range(final + 1)),
        ("travel", (1, 2, menu, final)), ("grid_cleanup", (menu,)),
        ("switch", () if idle else range(3, menu)), ("hold", range(3, menu) if idle else ()),
        ("gc_begin", (menu + 1,)), ("gc_end", (menu + 1,)),
    ):
        steps = [e["step"] for e in events if e["event"] == kind]
        if steps != list(expected):
            raise ValueError(f"{case}: unexpected {kind} steps {steps}")
    if not (by_key[menu, "travel"]["qpc"] < by_key[menu, "grid_cleanup"]["qpc"] < by_key[menu, "ready"]["qpc"]
            < by_key[menu, "window_end"]["qpc"] < by_key[menu + 1, "gc_begin"]["qpc"]
            < by_key[menu + 1, "gc_end"]["qpc"] < by_key[menu + 1, "window_begin"]["qpc"]):
        raise ValueError(f"{case}: cleanup or GC occurred outside its expected interval")

    geometry = (100, 100, 600, 500, 3700, 3800) if ism else (100, 100, 3800, 0, 0, 3800)
    snapshots = {int(step): tuple(map(int, counts)) for step, *counts in SNAPSHOT.findall(log)}
    for step in (1, *range(2, menu), final):
        mode = 0 if idle or step in (1, final) else (step - 2) % 3
        expected = (0,) * 6 if step in (1, final) else (100, 100, 100, 0, 0, 100) if mode == 2 else geometry
        if snapshots.get(step) != expected or by_key[step, "window_begin"]["mode"] != MODES[mode]:
            raise ValueError(f"{case}: wrong geometry or view in step {step}")

    rows = read_csv(directory / f"{case}.csv")
    process = {(r["pid"], r["qpc_hz"], r["ism"], r["version"], r["adapter"], r["luid"]) for r in rows}
    if len(process) != 1:
        raise ValueError(f"{case}: expected samples of exactly one game process")
    pid, frequency, raw_ism, version, adapter, luid = process.pop()
    frequency = int(frequency)
    if bool(int(raw_ism)) != ism:
        raise ValueError(f"{case}: observed unexpected ISM flag")
    identity.update(pid=int(pid), mod_version=version, adapter=adapter, luid=luid)
    series = [{"case": case, **r, "qpc": int(r["qpc"]), **{key: float(r[key]) for key in RAM}} for r in rows]
    if any(a["qpc"] >= b["qpc"] for a, b in zip(series, series[1:])):
        raise ValueError(f"{case}: out-of-order or duplicate samples")

    grid_qpc = by_key[2, "window_begin"]["qpc"]
    windows = {}
    for step in range(final + 1):
        mark = by_key[step, "window_begin"]
        start, end = mark["qpc"], by_key[step, "window_end"]["qpc"]
        samples = [row for row in series if start <= row["qpc"] < end]
        ticks = [start, *(r["qpc"] for r in samples), end]
        if len(samples) < 3 or max(b - a for a, b in zip(ticks, ticks[1:])) / frequency > 0.55:
            raise ValueError(f"{case}: low sample coverage at step {step}")
        windows[step] = {
            "case": case, "pid": int(pid), "ism": int(ism), "route": identity["route"],
            **{key: mark[key] for key in ("step", "cycle", "mode", "phase")},
            "start_qpc": start, "end_qpc": end, "samples": len(samples),
            "from_ghost_0_seconds": round(((start + end) / 2 - grid_qpc) / frequency, 4),
            "window_seconds": round((end - start) / frequency, 4),
            **{key: round(median(r[key] for r in samples), 3) for key in RAM},
        }

    points = (0, 1, 2, menu - 1, menu, menu + 1, final)
    counters = {int(step): (name, int(available), int(allocated), int(os_allocated), cache, immediate)
                for step, name, available, allocated, os_allocated, cache, immediate in ALLOCATOR.findall(log)}
    counts = {(int(step), cohort): tuple(map(int, values)) for step, cohort, *values in VIEWS.findall(log)}
    views = []
    for step in points:
        for cohort in ("current", "removed", "previous"):
            name, available, allocated, os_allocated, cache, immediate = counters[step]
            views.append({
                **windows[step], "cohort": cohort,
                **dict(zip(VIEW_FIELDS, counts[step, cohort])),
                "allocator": name, "allocator_counters": available,
                "allocator_used_bytes": allocated if available else "",
                "allocator_os_bytes": os_allocated if available else "",
                "allocator_os_cache_bytes": cache,
                "allocator_immediate_cache_bytes": immediate,
            })
    final_rows = [row for row in views if row["step"] == final]
    if any(row["registered"] for row in final_rows):
        raise ValueError(f"{case}: registered projections survived in the empty world")
    return identity, events, series, windows, views


def compare(series, ordinary, ism, cycles):
    ghost = [(f"ghost_{cycle}", 2 + 3 * cycle, 2, cycle) for cycle in range(cycles + 1)]
    tail = [("last_10_cycles", 2 + 3 * cycles, 2 + 3 * (cycles - 10), cycles)] if cycles >= 10 else []
    menu = 3 + 3 * cycles
    unloaded = [
        ("menu_pre_gc_minus_initial_menu", menu, 0, ""),
        ("menu_post_gc_minus_initial_menu", menu + 1, 0, ""),
        ("diagnostic_gc", menu + 1, menu, ""),
        ("returned_empty_minus_initial_empty", menu + 2, 1, ""),
    ]
    for kind, step, base, cycle in (*ghost, *tail, *unloaded):
        for key in RAM:
            oc, oi, ic, ii = (run[step][key] - run[base][key] for run in (*ordinary, *ism))
            yield dict(series=series, point=kind, cycle=cycle, metric=key, ordinary_cycle_mib=round(oc, 3),
                       ordinary_idle_mib=round(oi, 3), ordinary_switch_effect_mib=round(oc - oi, 3),
                       ism_cycle_mib=round(ic, 3), ism_idle_mib=round(ii, 3),
                       ism_switch_effect_mib=round(ic - ii, 3), extra_ism_switch_effect_mib=round((ic - ii) - (oc - oi), 3))


def case_name(prefix, ism, route):
    return f"{prefix}-{'ism' if ism else 'ordinary'}-{route}"


def common_configuration(identity):
    configuration = {key: value for key, value in identity.items()
                     if key not in {"command", "ism", "route", "pid"}}
    configuration["command"] = [
        arg for arg in identity["command"]
        if not arg.startswith(("-UserDir=", "-ABSLOG=", "-SaveCheckMemoryRoute=")) and arg != "-ShadowPerPlanISM"
    ]
    return configuration


def aggregate(directory, diagnostic):
    cases = [(case_name("diagnostic" if diagnostic else f"s{number}", ism, route), 0 if diagnostic else number,
              ism, route) for number, ism, route in (ORDER[:4] if diagnostic else ORDER)]
    runs, identities, events, samples, windows, views, regions = {}, {}, [], [], [], [], []
    reference = None
    for case, number, ism, route in cases:
        identity, marks, rows, intervals, cohorts = read_run(directory, case)
        if identity["ism"] != ism or identity["route"] != route or identity["cycles"] != 30:
            raise ValueError(f"{case}: wrong variant")
        common = common_configuration(identity)
        reference = common if reference is None else reference
        if common != reference:
            raise ValueError(f"{case}: package, fixtures, process hardware, or timings do not match")
        if any(old["pid"] == identity["pid"] for old in identities.values()):
            raise ValueError(f"{case}: expected a fresh game process")
        identities[case] = identity
        events.extend(marks)
        samples.extend(rows)
        windows.extend(intervals.values())
        views.extend(cohorts)
        runs[number, ism, route] = intervals
        if diagnostic:
            if any(row["allocator_os_cache_bytes"] == "" for row in cohorts):
                raise ValueError(f"{case}: missing allocator cache snapshots")
            scanned = read_csv(directory / f"{case}-regions.csv")
            if [int(row["step"]) for row in scanned] != [0, 1, 2, 92, 93, 94, 95]:
                raise ValueError(f"{case}: incomplete address space snapshots")
            for row in scanned:
                window = intervals[int(row["step"])]
                if int(row["pid"]) != identity["pid"] or not (
                    int(row["qpc"]) == window["start_qpc"] <= int(row["query_begin_qpc"])
                    < int(row["query_end_qpc"]) <= window["end_qpc"]
                ):
                    raise ValueError(f"{case}: address space snapshot outside its window")
                regions.append({"case": case, **row})
    deltas = []
    for number in sorted({number for _, number, _, _ in cases}):
        pairs = [tuple(runs[number, ism, route] for route in ("cycle", "idle")) for ism in (False, True)]
        for cycle, idle in pairs:
            skew = max(abs(cycle[step]["from_ghost_0_seconds"] - idle[step]["from_ghost_0_seconds"])
                       for step in range(2, 93))
            if skew > 0.35:
                raise ValueError(f"Series {number}: grid schedule differs by {skew:.3f} s")
        deltas.extend(compare(number, *pairs, 30))
    return identities, events, samples, windows, views, deltas, regions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--check", help="Validate one run by its label")
    parser.add_argument("--diagnostic", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.check:
        identity, _, rows, windows, views = read_run(args.directory, args.check)
        print(args.check, identity["pid"], len(rows), "samples", len(windows), "windows", len(views), "view cohorts")
        return
    identities, *data = aggregate(args.directory, args.diagnostic)
    if args.output:
        prefix = "F10-diagnostic" if args.diagnostic else "F10"
        for name, rows in zip(("events", "timeseries", "windows", "views", "comparisons", "regions"), data):
            if rows:
                save(args.output / f"{prefix}-{name}.csv", rows)
        (args.output / f"{prefix}-launches.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    print(len(identities), "validated runs")


if __name__ == "__main__":
    main()
