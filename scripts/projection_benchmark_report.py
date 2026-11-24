"""Join SaveCheck frames and projection-profile markers by engine frame number."""

import argparse
import csv
import json
import re
from pathlib import Path


MARKER = re.compile(
    r"Log(?:ShadowProjectionProfile|SaveCheckProjectionPerformance): Display: "
    r"(input|sample|operation|window|snapshot) (.+)"
)
FIELD = re.compile(r"(\w+)=([^\s]+)")


def value(text):
    try:
        return int(text)
    except ValueError:
        try:
            return float(text)
        except ValueError:
            return text


def load_frames(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        frames = {}
        for row in csv.DictReader(stream):
            frame = int(row["frame"])
            if frame in frames:
                raise ValueError(f"Duplicate frame {frame}: {path}")
            frames[frame] = {key: value(field) for key, field in row.items()}
        return frames


def frame_window(frames, frame):
    following = [frames[n]["frame_ms"] for n in range(frame + 1, frame + 6) if n in frames]
    return {
        "action_frame_ms": frames.get(frame, {}).get("frame_ms"),
        "next_five_max_ms": max(following) if len(following) == 5 else None,
        "nearby": [frames[n] for n in range(frame - 2, frame + 6) if n in frames],
    }


def timed_operation(samples, operation, frames):
    before = samples[0]
    after = samples[-1]
    commit_base = before["commit_mib"]
    sampled_commit = [
        row["commit_mib"]
        for row in frames.values()
        if before["mono_s"] <= row["mono_s"] <= after["mono_s"]
        and row["commit_mib"] >= 0
    ]
    peak = max([operation["sampled_peak_commit_mib"], *sampled_commit])
    return {
        **operation,
        "action_frame": before["frame"],
        "timing_source": "projection_profile",
        "phases": samples,
        "peak_meshes": max(sample["meshes"] for sample in samples),
        "peak_commit_delta_mib": round(peak - commit_base, 3),
        "after_commit_delta_mib": round(after["commit_mib"] - commit_base, 3),
        **frame_window(frames, before["frame"]),
    }


def read_run(path):
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    if not any("LogSaveCheckProjectionPerformance: Display: pass=1 " in line for line in lines):
        raise ValueError(f"Benchmark did not pass: {path}")
    frames = load_frames(path.with_suffix(".csv"))
    markers = {kind: [] for kind in ("input", "sample", "operation", "window", "snapshot")}
    for line in lines:
        match = MARKER.search(line)
        if match:
            fields = {key: value(field) for key, field in FIELD.findall(match[2])}
            markers[match[1]].append(fields)

    sampled = {}
    for sample in markers["sample"]:
        sampled.setdefault(sample["serial"], []).append(sample)
    operations = [
        timed_operation(sampled[operation["serial"]], operation, frames)
        for operation in markers["operation"]
    ]
    cycles = [operation for operation in operations if operation["op"] == "cycle"]
    inputs = markers["input"]
    if cycles and len(cycles) != len(inputs):
        raise ValueError(f"Projection-profile cycles do not match input markers: {path}")
    transitions = []
    for index, marker in enumerate(inputs):
        expected = marker["expected_action_frame"]
        if cycles:
            operation = cycles[index]
            if operation["to"] != marker["mode"]:
                raise ValueError(f"Unexpected mode at input {index}: {path}")
            frame_delta = operation["action_frame"] - expected
            if abs(frame_delta) > 2:
                raise ValueError(f"Input at frame {expected} does not match the operation: {path}")
            transitions.append({**marker, **operation, "marker_delta_frames": frame_delta})
        else:
            transitions.append({
                **marker,
                "action_frame": expected,
                "timing_source": "input_plus_one_frame",
                **frame_window(frames, expected),
            })
        transition = transitions[-1]
        if transition["action_frame_ms"] is None or transition["next_five_max_ms"] is None:
            raise ValueError(f"Missing frame samples around input at frame {expected}: {path}")
    return {
        "case": path.stem,
        "windows": markers["window"],
        "snapshots": markers["snapshot"],
        "restores": [operation for operation in operations if operation["op"] == "restore"],
        "transitions": transitions,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = json.dumps([read_run(log) for log in args.logs], ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(report + "\n", encoding="utf-8")
    else:
        print(report)


if __name__ == "__main__":
    main()
