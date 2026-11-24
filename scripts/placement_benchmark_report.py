"""Summarize F04 route frames and placement feedback by engine frame number."""

import argparse
import csv
import json
from pathlib import Path


SECTIONS = ("empty", "slab", "column", "multi", "free")
MODES = ("stowed", "plans", "world")
COUNTERS = (
    "plans_examined",
    "candidates",
    "part_pairs",
    "conflicting_plans",
    "outlined_placed_parts",
    "outlined_preview_parts",
)


def load_frames(path):
    frames = {}
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            frame = int(row["frame"])
            if frame in frames:
                raise ValueError(f"Duplicate frame {frame} in {path}")
            frames[frame] = row
    return frames


def distribution(values):
    values = sorted(values)
    if not values:
        return None

    def percentile(fraction):
        position = (len(values) - 1) * fraction
        lo = int(position)
        hi = min(lo + 1, len(values) - 1)
        return round(values[lo] + (values[hi] - values[lo]) * (position - lo), 6)

    return {"min": values[0], "p50": percentile(0.5), "p95": percentile(0.95), "max": values[-1]}


def summarize(frames, feedback, mode):
    samples = [feedback[frame] for frame in frames if frame in feedback]
    result = {
        "frames": len(frames),
        "feedback_samples": len(samples),
        "frame_ms": distribution(float(s["frame_ms"]) for s in frames.values()),
        "feedback_ms": distribution(float(s["feedback_ms"]) for s in samples),
        **{name: distribution(int(s[name]) for s in samples) for name in COUNTERS},
    }
    if mode != "first-preview":
        result["samples_with_expected_preview_and_support"] = sum(
            int(s["preview"]) == (mode != "stowed") and int(s["support"]) == (mode == "plans")
            for s in samples
        )
    return result


def read_run(directory):
    frames = load_frames(directory / "frames.csv")
    feedback = load_frames(directory / "feedback.csv")
    openings = {number: row for number, row in frames.items() if row["phase"] == "open"}
    if not openings:
        raise ValueError(f"First preview was not recorded in {directory}")
    windows = {}
    for number, row in frames.items():
        if row["phase"] == "measure":
            key = (row["mode"], row["section"], int(row["repeat"]))
            windows.setdefault(key, {})[number] = row

    expected = {
        (mode, section, repeat)
        for mode in MODES
        for section in (SECTIONS if mode != "world" else ("slab",))
        for repeat in range(3)
    }
    if windows.keys() != expected:
        raise ValueError(f"Unexpected route windows in {directory}: {sorted(windows.keys() ^ expected)}")

    sections = {
        (mode, section): [summarize(windows[mode, section, i], feedback, mode) for i in range(3)]
        for mode in MODES
        for section in SECTIONS
        if (mode, section, 0) in windows
    }

    comparisons = []
    for mode, control, points in (("plans", "stowed", SECTIONS), ("plans", "world", ("slab",))):
        for section in points:
            active = sections[mode, section]
            baseline = sections[control, section]
            delta = {
                measure: [round(a["frame_ms"][measure] - b["frame_ms"][measure], 6) for a, b in zip(active, baseline)]
                for measure in ("p50", "p95", "max")
            }
            comparisons.append({"mode": mode, "control": control, "section": section, "delta_frame_ms": delta})

    return {
        "directory": str(directory),
        "first_preview": summarize(openings, feedback, "first-preview"),
        "sections": [
            {"mode": mode, "section": section, "repeats": repeats}
            for (mode, section), repeats in sections.items()
        ],
        "comparisons": comparisons,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps([read_run(directory) for directory in args.directories], indent=2) + "\n"
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    else:
        print(report, end="")


if __name__ == "__main__":
    main()
