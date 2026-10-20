"""stats.py: frame-time statistics and the runtime log's counters (shared by scenario.py and the supervisor).

Frame-time percentiles are nearest-rank over the game presents in [from, to) of LARECOMP_FRAMETIME_CSV
(present,dt_ms). FPS lows are derived from the frame times: "1% low" = 1000 / P99 frame time, "0.1% low" =
1000 / P99.9 frame time (the frame time the slowest 1% / 0.1% of frames reach).
"""
import csv
import math
import os
import re
import statistics


def _pct(ordered, p):
    return ordered[max(0, math.ceil(len(ordered) * p / 100) - 1)]


def frame_stats(path, first=0, last=None):
    if not os.path.exists(path):
        return None
    with open(path, newline="") as f:
        samples = [float(r["dt_ms"]) for r in csv.DictReader(f)
                   if int(r["present"]) >= first and (last is None or int(r["present"]) < last)]
    if not samples:
        return None
    o = sorted(samples)
    mean = sum(samples) / len(samples)
    return {
        "presents": len(samples), "mean_ms": round(mean, 3), "fps_mean": round(1000 / mean, 2),
        "median_ms": round(statistics.median(samples), 3), "p95_ms": round(_pct(o, 95), 3),
        "p99_ms": round(_pct(o, 99), 3), "p999_ms": round(_pct(o, 99.9), 3), "max_ms": round(o[-1], 3),
        "fps_1pct_low": round(1000 / _pct(o, 99), 2), "fps_01pct_low": round(1000 / _pct(o, 99.9), 2),
        "over_20ms": sum(t > 20 for t in samples), "over_33ms": sum(t > 33.4 for t in samples),
        "over_50ms": sum(t > 50 for t in samples),
    }


STAT_RE = re.compile(r"^\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms \((\d+) draws, (\d+) skipped.*?"
                     r"compile (\d+) \(translate (\d+)ms, spirv (\d+)ms, pipelines (\d+)ms, draws waited (\d+)ms\)")
SKIP_RE = re.compile(r"\[skip: noprog (\d+) compiling (\d+) failed (\d+) noobj (\d+) mode (\d+) pipe-building (\d+) "
                     r"pipe-failed (\d+)\]")
VAL_RE = re.compile(r"\| vkval E (\d+) W (\d+)")
SKIP_NAMES = ["noprog", "compiling", "failed", "noobj", "mode", "pipe_building", "pipe_failed"]


def parse_log(text, first=0, last=None):
    """Sums of the 2-second stats lines, whole run and inside [first, last) (by the line's frame number)."""
    out = {"crashes": 0, "skipped_total": 0, "skipped_window": 0, "skip_why_total": dict.fromkeys(SKIP_NAMES, 0),
           "compiles_total": 0, "compiles_window": 0, "shader_wait_ms_total": 0, "translate_ms_total": 0,
           "spirv_ms_total": 0, "pipeline_ms_total": 0, "vkval_errors": 0, "vkval_warnings": 0, "stat_lines": 0,
           "last_frame": 0}
    prev_frame = 0
    for line in text.splitlines():
        if line.startswith("FATAL") or re.match(r"^\[\w+\] exception|exception [0-9a-f]{8}", line):
            out["crashes"] += 1
        if line.startswith("[vk] gpu "):
            out["gpu"] = line[9:].strip()
        m = STAT_RE.match(line)
        if not m:
            continue
        out["stat_lines"] += 1
        frame, skipped, comp = int(m.group(2)), int(m.group(6)), int(m.group(7))
        out["last_frame"] = frame
        out["skipped_total"] += skipped
        out["compiles_total"] += comp
        out["translate_ms_total"] += int(m.group(8))
        out["spirv_ms_total"] += int(m.group(9))
        out["pipeline_ms_total"] += int(m.group(10))
        out["shader_wait_ms_total"] += int(m.group(11))
        # a stats line covers the frames since the previous one: count it in the window only when all of them are
        # (a line ending just inside the window can hold a transition's skips from before it)
        if prev_frame >= first and (last is None or frame <= last + 60):
            out["skipped_window"] += skipped
            out["compiles_window"] += comp
        prev_frame = frame
        s = SKIP_RE.search(line)
        if s:
            for name, v in zip(SKIP_NAMES, s.groups()):
                out["skip_why_total"][name] += int(v)
        v = VAL_RE.search(line)
        if v:
            out["vkval_errors"], out["vkval_warnings"] = int(v.group(1)), int(v.group(2))
    return out
