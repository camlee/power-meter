#!/usr/bin/env python3
"""Build the compact history fixture from one timestamped demo-source window.

The selected source window is deliberately fixed so regeneration is
deterministic.  Missing source time is emitted as a gap between fixture
segments; it is never interpolated or converted to zero usage.
"""

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "demo-source"
OUTPUT = ROOT / "src/data/demo_history_profile.h"

FIRST_LOG = 0
LAST_LOG = 29
BUCKET_MINUTES = 15
BUCKET_MS = BUCKET_MINUTES * 60 * 1000
MIN_BUCKET_COVERAGE = 0.75
MAX_SEGMENT_MINUTES = 240


def load_rows(path, offset_ms):
    rows = []
    with path.open(newline="") as handle:
        for line_number, row in enumerate(csv.reader(handle), 1):
            if len(row) < 3:
                continue
            try:
                ticks_ms, in_ws, out_ws = (float(value) for value in row[:3])
            except ValueError as error:
                raise ValueError(f"{path}:{line_number}: non-numeric sample") from error
            rows.append((offset_ms + ticks_ms, in_ws, out_ws))
    if len(rows) < 2:
        raise ValueError(f"{path}: expected at least two samples")
    if any(right[0] <= left[0] for left, right in zip(rows, rows[1:])):
        raise ValueError(f"{path}: timestamps are not strictly increasing")
    return rows


def selected_logs():
    metadata = json.loads((SOURCE / "meta.json").read_text())["logs"]
    logs = []
    for log_number in range(FIRST_LOG, LAST_LOG + 1):
        path = SOURCE / f"{log_number}.csv"
        log = metadata.get(str(log_number))
        if not path.exists():
            # A missing source file is an honest no-data interval.  The next
            # timestamped file determines its exact duration.
            continue
        if log is None:
            raise ValueError(f"{path}: no timestamp mapping in meta.json")
        logs.append((log_number, load_rows(path, log["start_time_offset"])))
    if not logs or logs[0][0] != FIRST_LOG or logs[-1][0] != LAST_LOG:
        raise ValueError("selected source window endpoints are unavailable")
    return logs


def segment_ranges(valid_buckets):
    runs = []
    start = previous = None
    for bucket in valid_buckets:
        if start is None:
            start = previous = bucket
        elif bucket == previous + 1:
            previous = bucket
        else:
            runs.append((start * BUCKET_MINUTES, (previous + 1) * BUCKET_MINUTES))
            start = previous = bucket
    if start is not None:
        runs.append((start * BUCKET_MINUTES, (previous + 1) * BUCKET_MINUTES))

    segments = []
    for start_minute, end_minute in runs:
        while start_minute < end_minute:
            count = min(MAX_SEGMENT_MINUTES, end_minute - start_minute)
            segments.append((start_minute, count))
            start_minute += count
    return segments


def build():
    logs = selected_logs()
    window_start_ms = logs[0][1][0][0]
    window_end_ms = logs[-1][1][-1][0]
    bucket_count = (int(window_end_ms - window_start_ms) + BUCKET_MS - 1) // BUCKET_MS
    bins = defaultdict(lambda: [0.0] * 5)  # duration h, then four energy Wh values

    sample_intervals_ms = []
    for _, rows in logs:
        intervals = [rows[index + 1][0] - rows[index][0] for index in range(len(rows) - 1)]
        fallback_ms = statistics.median(intervals)
        sample_intervals_ms.extend(intervals)
        for index, (timestamp_ms, in_ws, out_ws) in enumerate(rows):
            duration_ms = intervals[index] if index < len(intervals) else fallback_ms
            bucket = int((timestamp_ms - window_start_ms) // BUCKET_MS)
            if not 0 <= bucket < bucket_count:
                continue
            incoming_wh = max(in_ws, 0.0) / 3600.0
            outgoing_wh = max(out_ws, 0.0) / 3600.0
            values = bins[bucket]
            values[0] += duration_ms / 3600000.0
            for target, value in enumerate((
                max(incoming_wh - outgoing_wh, 0.0),
                max(outgoing_wh - incoming_wh, 0.0),
                min(incoming_wh, outgoing_wh),
                incoming_wh * 0.25,
            ), 1):
                values[target] += value

    minimum_duration_h = BUCKET_MINUTES / 60.0 * MIN_BUCKET_COVERAGE
    profile = []
    valid_buckets = []
    for bucket in range(bucket_count):
        duration_h, charge, use, panel, surplus = bins[bucket]
        valid = duration_h >= minimum_duration_h
        if valid:
            valid_buckets.append(bucket)
            point = tuple(round(value / duration_h * 10) for value in (charge, use, panel, surplus))
        else:
            point = (0, 0, 0, 0)
        profile.append((*point, valid))

    segments = segment_ranges(valid_buckets)
    fixture_span_minutes = max(start + count for start, count in segments)
    # Drop only the trailing partial bucket after the last complete source
    # interval. Interior invalid buckets remain represented by segment gaps.
    profile = profile[: fixture_span_minutes // BUCKET_MINUTES]
    lines = [
        "// Generated by tools/build_demo_profile.py; do not edit by hand.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace historical_storage::demo {",
        "struct ProfilePoint {",
        "    int16_t chargeW10;",
        "    int16_t useW10;",
        "    int16_t panelW10;",
        "    int16_t surplusW10;",
        "    bool valid;",
        "};",
        "struct FixtureSegment { uint16_t firstMinute; uint16_t records; };",
        f"constexpr uint16_t kProfileStepMinutes = {BUCKET_MINUTES};",
        f"constexpr uint16_t kFixtureSpanMinutes = {fixture_span_minutes};",
        f"constexpr ProfilePoint kProfile[{len(profile)}] = {{",
    ]
    lines.extend(
        "    {%d, %d, %d, %d, %s}," % (*point[:4], "true" if point[4] else "false")
        for point in profile
    )
    lines.extend([
        "};",
        f"constexpr FixtureSegment kFixtureSegments[{len(segments)}] = {{",
    ])
    lines.extend("    {%d, %d}," % segment for segment in segments)
    lines.extend([
        "};",
        "constexpr uint16_t kFixtureSegmentCount =",
        "    sizeof(kFixtureSegments) / sizeof(kFixtureSegments[0]);",
        "} // namespace historical_storage::demo",
        "",
    ])

    gap_minutes = fixture_span_minutes - len(valid_buckets) * BUCKET_MINUTES
    stats = {
        "first_log": logs[0][0],
        "last_log": logs[-1][0],
        "window_minutes": (window_end_ms - window_start_ms) / 60000.0,
        "profile_minutes": fixture_span_minutes,
        "data_minutes": len(valid_buckets) * BUCKET_MINUTES,
        "gap_minutes": gap_minutes,
        "profile_points": len(profile),
        "segments": segments,
        "median_sample_interval_ms": statistics.median(sample_intervals_ms),
    }
    return "\n".join(lines), stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if the generated header is stale")
    args = parser.parse_args()
    content, stats = build()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != content:
            raise SystemExit(f"stale generated fixture: run {Path(__file__).name}")
    else:
        OUTPUT.write_text(content)
    action = "validated" if args.check else "generated"
    print(
        f"{action} {stats['profile_points']} profile points, "
        f"{stats['data_minutes']} data minutes in {len(stats['segments'])} segments; "
        f"source window {stats['window_minutes']:.3f} minutes, "
        f"explicit gap {stats['gap_minutes']} minutes"
    )


if __name__ == "__main__":
    main()
