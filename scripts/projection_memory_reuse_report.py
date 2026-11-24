"""Validate repeated Shipping routes and compare RAM across unloads."""

import argparse
import json
from pathlib import Path
import re
from statistics import median

from projection_memory_report import EVENT, SNAPSHOT, save
from projection_memory_trace_report import BUILD, LOADED_SLOT, RAM, VIEWS, VIEW_FIELDS, read_csv
from run_projection_memory_trace import REUSE_ORDER, SLOTS, hash_file


SML = re.compile(r"LogSatisfactoryModLoader: Display: SML: (\S+)")


def read_case(directory, case):
    path = directory / f"{case}.log"
    log = path.read_text(encoding="utf-8-sig")
    identity_path = directory / f"{case}-identity.json"
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    cycles, rounds = identity["cycles"], identity["rounds"]
    stride = 3 * cycles + 4
    final = 1 + rounds * stride
    if f"LogSaveCheckProjectionMemory: Display: pass=1 step={final} " not in log:
        raise ValueError(f"{case}: incomplete route")
    if "LogInit: Build Configuration: Shipping" not in log:
        raise ValueError(f"{case}: expected Shipping")
    for field in ("save_sha256", "mod_dll_sha256", "savecheck_sha256"):
        if identity[field] != identity[field + "_after"]:
            raise ValueError(f"{case}: {field} changed")
    identity["game_build"] = BUILD.search(log)[1]
    identity["sml_version"] = SML.search(log)[1]
    control, ism = identity["empty_control"], identity["ism"]
    variant = "control" if control else "ism" if ism else "ordinary"
    expected_loads = [SLOTS[0], *([SLOTS[0 if control else 1], SLOTS[0]] * rounds)]
    if LOADED_SLOT.findall(log) != expected_loads:
        raise ValueError(f"{case}: wrong fixture sequence")

    events = {}
    for event, phase, raw_step, cycle, mode, qpc in EVENT.findall(log):
        step = int(raw_step)
        if (step, event) in events or event == "switch" or (phase == "grid" and mode != "Ghost"):
            raise ValueError(f"{case}: unexpected event {step} {event} {mode}")
        events[step, event] = int(qpc)
    for event in ("ready", "window_begin", "window_end"):
        if {step for step, kind in events if kind == event} != set(range(final + 1)):
            raise ValueError(f"{case}: incomplete timing for {event}")

    raw = directory / f"{case}.csv"
    rows = read_csv(raw)
    process = {(r["pid"], r["qpc_hz"], r["ism"], r["version"], r["adapter"], r["luid"]) for r in rows}
    if len(process) != 1:
        raise ValueError(f"{case}: expected one process from first menu through final empty world")
    pid, hz, raw_ism, version, adapter, luid = process.pop()
    hz = int(hz)
    if bool(int(raw_ism)) != ism:
        raise ValueError(f"{case}: unexpected ISM flag")
    identity.update(pid=int(pid), mod_version=version, adapter=adapter, luid=luid, qpc_hz=hz,
                    raw_log_sha256=hash_file(path), raw_ram_sha256=hash_file(raw))
    series = [(int(r["qpc"]), *(float(r[key]) for key in RAM)) for r in rows]
    if any(a[0] >= b[0] for a, b in zip(series, series[1:])):
        raise ValueError(f"{case}: invalid sampler timestamps")
    identity.update(first_sample_qpc=series[0][0], last_sample_qpc=series[-1][0])

    snapshots = {int(step): tuple(map(int, values)) for step, *values in SNAPSHOT.findall(log)}
    cohorts = {(int(step), cohort): dict(zip(VIEW_FIELDS, map(int, values)))
               for step, cohort, *values in VIEWS.findall(log)}
    empty = (0,) * 6
    grid = (100, 100, 600, 500, 3700, 3800) if ism else (100, 100, 3800, 0, 0, 3800)
    windows, views = [], []

    def window(step, lap, phase, geometry=None, **timing):
        if geometry is not None and snapshots.get(step) != geometry:
            raise ValueError(f"{case}: wrong layout at {step}, expected {geometry}, found {snapshots.get(step)}")
        start, end = events[step, "window_begin"], events[step, "window_end"]
        samples = [r for r in series if start <= r[0] < end]
        ticks = [start, *(r[0] for r in samples), end]
        if len(samples) < 3 or max(b - a for a, b in zip(ticks, ticks[1:])) / hz > 0.55:
            raise ValueError(f"{case}: low coverage at step {step}")
        record = dict(case=case, pid=int(pid), variant=variant, lap=lap, phase=phase, step=step,
                      ready_qpc=events[step, "ready"], start_qpc=start, end_qpc=end,
                      qpc_hz=hz, window_seconds=round((end - start) / hz, 4), samples=len(samples))
        for col, key in enumerate(RAM, start=1):
            values = [sample[col] for sample in samples]
            record[key] = round(median(values), 3)
            record[f"{key}_window_span"] = round(max(values) - min(values), 3)
        record.update(grid_seconds=timing.get("grid_seconds", ""), gc_seconds=timing.get("gc_seconds", ""))
        windows.append(record)
        for name in ("current", "removed", "previous"):
            views.append({**record, "cohort": name, **cohorts[step, name]})

    window(1, 0, "empty_before", empty)
    travel_steps = [1]
    for lap in range(1, rounds + 1):
        first = 2 + (lap - 1) * stride
        last = first + 3 * cycles
        menu, gc, returned = last + 1, last + 2, last + 3
        travel_steps.extend([first, menu, returned])
        if any((step, "hold") not in events for step in range(first + 1, last + 1)):
            raise ValueError(f"{case}: missing Ghost hold windows in lap {lap}")
        if not (events[menu, "travel"] < events[menu, "grid_cleanup"] < events[menu, "ready"]
                < events[menu, "window_end"] <= events[gc, "gc_begin"] < events[gc, "gc_end"]
                <= events[gc, "ready"] < events[gc, "window_begin"]):
            raise ValueError(f"{case}: wrong cleanup or GC ordering in lap {lap}")
        grid_seconds = (events[last, "window_begin"] - events[first, "window_begin"]) / hz
        nominal = 3 * cycles * (identity["grid_settle_seconds"] + identity["observation_seconds"])
        if abs(grid_seconds - nominal) > 0.35:
            raise ValueError(f"{case}: grid schedule differed from {nominal:.1f} s in lap {lap}: {grid_seconds:.3f}")
        gc_seconds = (events[gc, "gc_end"] - events[gc, "gc_begin"]) / hz
        expected_grid = empty if control else grid
        grid_checks = (first, last) if identity["sparse_checks"] else range(first, last + 1)
        if any(snapshots.get(step) != expected_grid for step in grid_checks):
            raise ValueError(f"{case}: invalid Ghost geometry in lap {lap}")
        window(first, lap, "grid_start", expected_grid)
        window(last, lap, "grid_end", expected_grid, grid_seconds=round(grid_seconds, 4))
        window(menu, lap, "menu_before_gc")
        window(gc, lap, "menu_after_gc", gc_seconds=round(gc_seconds, 4))
        window(returned, lap, "empty_after", empty)
    if sorted(step for step, event in events if event == "travel") != travel_steps:
        raise ValueError(f"{case}: unexpected world travel")
    return identity, windows, views


def return_deltas(windows):
    returns = [row for row in windows if row["phase"].startswith("empty_")]
    baseline = previous = returns[0]
    for current in returns:
        for key in RAM:
            yield dict(lap=current["lap"], metric=key, absolute_mib=current[key],
                       change_from_previous_mib=round(current[key] - previous[key], 3),
                       change_from_initial_mib=round(current[key] - baseline[key], 3),
                       window_span_mib=current[f"{key}_window_span"])
        previous = current


def aggregate(directory):
    identities, windows, views, results = {}, [], [], []
    by_key = {}
    reference = None
    for number, variant in REUSE_ORDER:
        case = f"reuse-s{number}-{variant}"
        identity, points, objects = read_case(directory, case)
        common = {key: identity[key] for key in (
            "archive_sha256", "mod_dll_sha256", "savecheck_sha256", "save_sha256", "route",
            "grid_settle_seconds", "observation_seconds", "game_build", "sml_version",
            "mod_version", "adapter", "luid", "qpc_hz", "sparse_checks",
        )}
        if reference is None:
            reference = common
        if (common != reference or identity["rounds"] != 5 or identity["cycles"] != 30
                or identity["ism"] != (variant == "ism") or identity["empty_control"] != (variant == "control")):
            raise ValueError(f"{case}: incompatible run setup")
        identities[case] = identity
        windows.extend(points)
        views.extend(objects)
        for row in return_deltas(points):
            result = dict(series=number, case=case, variant=variant, **row)
            by_key[number, variant, row["lap"], row["metric"]] = result
            results.append(result)
    ordered = list(identities.values())
    if any(a["last_sample_qpc"] >= b["first_sample_qpc"] for a, b in zip(ordered, ordered[1:])):
        raise ValueError("Expected fresh sequential processes in the specified order")
    for row in results:
        number, lap, key = row["series"], row["lap"], row["metric"]
        control = by_key[number, "control", lap, key]
        initial = by_key[number, row["variant"], 1, key]
        control_first = by_key[number, "control", 1, key]
        row["change_from_initial_minus_control_mib"] = round(row["change_from_initial_mib"] - control["change_from_initial_mib"], 3)
        row["change_since_first_return_mib"] = round(row["absolute_mib"] - initial["absolute_mib"], 3) if lap else ""
        row["change_since_first_return_minus_control_mib"] = round(
            row["absolute_mib"] - initial["absolute_mib"] - control["absolute_mib"] + control_first["absolute_mib"], 3
        ) if lap else ""
    return identities, windows, views, results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--check", help="Validate and print one run or a short pilot")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.check:
        identity, windows, views = read_case(args.directory, args.check)
        print(args.check, identity["pid"], len(windows), "windows", len(views), "view cohorts")
        for row in return_deltas(windows):
            print(row["lap"], row["metric"], row["absolute_mib"], row["change_from_previous_mib"])
        return
    identities, windows, views, results = aggregate(args.directory)
    if args.output:
        for name, rows in (("windows", windows), ("views", views), ("returns", results)):
            save(args.output / f"F11-reuse-{name}.csv", rows)
        (args.output / "F11-reuse-launches.json").write_text(json.dumps(identities, indent=2) + "\n", encoding="utf-8")
    print(len(identities), "validated processes", len(windows), "checkpoint windows")


if __name__ == "__main__":
    main()
