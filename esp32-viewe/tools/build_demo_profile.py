#!/usr/bin/env python3
"""Turn downloaded MicroPython CSV logs into a compact 24-hour demo profile."""

import csv
import json
from collections import defaultdict
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "demo-source"
OUTPUT = ROOT / "src/data/demo_history_profile.h"


def main():
    meta = json.loads((SOURCE / "meta.json").read_text())["logs"]
    bins = defaultdict(lambda: [0.0] * 5)  # duration, charge, use, panel-to-load, surplus
    for path in SOURCE.glob("*.csv"):
        log = meta.get(path.stem)
        if not log:
            continue
        rows = []
        with path.open() as handle:
            for row in csv.reader(handle):
                if len(row) >= 3:
                    rows.append(tuple(float(value) for value in row[:3]))
        if len(rows) < 2:
            continue
        intervals = [rows[index + 1][0] - rows[index][0] for index in range(len(rows) - 1)]
        fallback_ms = sorted(intervals)[len(intervals) // 2]
        for index, (ticks_ms, in_ws, out_ws) in enumerate(rows):
            duration_h = ((intervals[index] if index < len(intervals) else fallback_ms) / 3600000.0)
            when = datetime.fromtimestamp((log["start_time_offset"] + ticks_ms) / 1000.0)
            bucket = (when.hour * 60 + when.minute) // 15
            incoming = max(in_ws, 0.0) / 3600.0
            outgoing = max(out_ws, 0.0) / 3600.0
            charge = max(incoming - outgoing, 0.0)
            use = max(outgoing - incoming, 0.0)
            panel = min(incoming, outgoing)
            # The legacy files have no available-power/duty field.  This is a
            # deliberately modest daylight-only synthetic surplus for demo UI.
            surplus = incoming * 0.25
            values = bins[bucket]
            values[0] += duration_h
            for target, value in enumerate((charge, use, panel, surplus), 1):
                values[target] += value

    profile = []
    for bucket in range(96):
        duration, charge, use, panel, surplus = bins[bucket]
        if duration:
            profile.append(tuple(round(value / duration * 10) for value in (charge, use, panel, surplus)))
        else:
            profile.append((0, 0, 0, 0))

    lines = ["#pragma once", "#include <stdint.h>", "", "namespace historical_storage::demo {",
             "struct ProfilePoint { int16_t chargeW10; int16_t useW10; int16_t panelW10; int16_t surplusW10; };",
             "constexpr ProfilePoint kDayProfile[96] = {"]
    lines.extend("    {%d, %d, %d, %d}," % point for point in profile)
    lines.extend(["};", "} // namespace historical_storage::demo", ""])
    OUTPUT.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
