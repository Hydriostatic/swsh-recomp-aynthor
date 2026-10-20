"""logsum.py: totals of the runtime's per-2-second counters over one run's log (recipes, holds, barriers, skips).

  python tools/swsh/logsum.py reports/runs/<scenario>/<run>[/log.txt] [...]
"""
import os
import re
import sys

PATS = {
    "recipes": re.compile(r"\| recipes (\d+): progs (\d+) pipes (\d+) adopted (\d+) new (\d+)"),
    "holds": re.compile(r"\| holds (\d+) \((\d+)ms, max ([\d.]+)ms, claimed (\d+), timeouts (\d+)\)"),
    "barriers": re.compile(r"\| nvn barriers (\d+)"),
    "skipped": re.compile(r"\((\d+) draws, (\d+) skipped"),
}


def summarize(path):
    if os.path.isdir(path):
        path = os.path.join(path, "log.txt")
    out = {"recipes_loaded": 0, "prewarm_progs": 0, "prewarm_pipes": 0, "adopted": 0, "recorded": 0, "holds": 0,
           "hold_ms": 0, "hold_max_ms": 0.0, "claimed": 0, "hold_timeouts": 0, "barriers": 0, "draws": 0, "skipped": 0}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("[vk] t="):
                continue
            m = PATS["recipes"].search(line)
            if m:
                out["recipes_loaded"] = int(m[1])
                out["prewarm_progs"] += int(m[2]); out["prewarm_pipes"] += int(m[3])
                out["adopted"] += int(m[4]); out["recorded"] += int(m[5])
            m = PATS["holds"].search(line)
            if m:
                out["holds"] += int(m[1]); out["hold_ms"] += int(m[2])
                out["hold_max_ms"] = max(out["hold_max_ms"], float(m[3]))
                out["claimed"] += int(m[4]); out["hold_timeouts"] += int(m[5])
            m = PATS["barriers"].search(line)
            if m:
                out["barriers"] += int(m[1])
            m = PATS["skipped"].search(line)
            if m:
                out["draws"] += int(m[1]); out["skipped"] += int(m[2])
    return out


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(p, summarize(p))
