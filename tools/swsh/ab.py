"""ab.py: interleaved A/B of two runtime builds (or two environments) on one scenario.

  python tools/swsh/ab.py <scenario> --a build/ab/base/lasrecomp.exe --b build/v132/lasrecomp.exe
                          [--profile 1440p60] [--runs 3] [--b-env VAR=x ...] [--a-env VAR=x ...] [--label kwait]

Runs A, B, A, B... (the frame time of the same scene drifts between runs, so only runs taken in turn compare), each
through tools/swsh/scenario.py with the warm test cache. Per variant: the median over runs of mean / median / P95 /
P99 frame time and of the skipped draws; for B/A of the mean frame time and of P99, a bootstrap 95% interval over the
runs. An interval that contains 1.0 is not a result. Writes reports/ab/<stamp>-<label>.json.
"""
import argparse
import datetime
import json
import os
import random
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def run(scenario, exe, profile, envs, label):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "swsh", "scenario.py"), scenario, "--profile", profile, "--exe", exe,
           "--label", label]
    for e in envs:
        cmd += ["--env", e]
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout
    lines = [json.loads(x) for x in out.splitlines() if x.startswith("{")]
    info = lines[-1]
    with open(os.path.join(ROOT, info["run"], "summary.json")) as f:
        return info["run"], json.load(f)


def boot_ratio(a, b, n=4000):
    rnd = random.Random(1)
    r = []
    for _ in range(n):
        ma = statistics.median(rnd.choice(a) for _ in a)
        mb = statistics.median(rnd.choice(b) for _ in b)
        r.append(mb / ma)
    r.sort()
    return round(r[int(0.025 * n)], 4), round(r[int(0.975 * n)], 4)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario")
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--a-env", action="append", default=[])
    ap.add_argument("--b-env", action="append", default=[])
    ap.add_argument("--profile", default="1440p60")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--label", default="ab")
    args = ap.parse_args()
    res = {"A": [], "B": []}
    for i in range(args.runs):
        for v in ("A", "B"):
            exe, envs = (args.a, args.a_env) if v == "A" else (args.b, args.b_env)
            run_dir, s = run(args.scenario, exe, args.profile, envs, f"ab-{args.label}-{v}")
            ft = s.get("frametime") or {}
            res[v].append({"run": run_dir, "status": s["status"], "skipped_total": s.get("skipped_total"), **ft})
            print(f"{v} #{i + 1}: {s['status']} fps {ft.get('fps_mean')} p95 {ft.get('p95_ms')} p99 {ft.get('p99_ms')} "
                  f"skipped {s.get('skipped_total')}", flush=True)
    out = {"scenario": args.scenario, "profile": args.profile, "a": args.a, "b": args.b, "a_env": args.a_env,
           "b_env": args.b_env, "runs": res, "when": datetime.datetime.now().isoformat(timespec="seconds")}
    ok = {v: [r for r in res[v] if r["status"] == "OK" and r.get("mean_ms")] for v in res}
    if all(len(ok[v]) >= 2 for v in ok):
        agg = {}
        for v in ok:
            agg[v] = {k: statistics.median(r[k] for r in ok[v]) for k in ("fps_mean", "mean_ms", "median_ms", "p95_ms", "p99_ms",
                                                                          "over_20ms", "over_50ms", "skipped_total")}
        out["aggregate"] = agg
        out["ratio_mean_ms_B_over_A"] = boot_ratio([r["mean_ms"] for r in ok["A"]], [r["mean_ms"] for r in ok["B"]])
        out["ratio_p99_B_over_A"] = boot_ratio([r["p99_ms"] for r in ok["A"]], [r["p99_ms"] for r in ok["B"]])
        print(json.dumps({"A": agg["A"], "B": agg["B"], "mean_ms B/A 95%": out["ratio_mean_ms_B_over_A"],
                          "p99 B/A 95%": out["ratio_p99_B_over_A"]}, indent=1))
    os.makedirs(os.path.join(ROOT, "reports", "ab"), exist_ok=True)
    path = os.path.join(ROOT, "reports", "ab", f"{datetime.datetime.now():%Y%m%d-%H%M%S}-{args.label}.json")
    with open(path, "w") as f:
        json.dump(out, f, indent=1)
    print(os.path.relpath(path, ROOT))


if __name__ == "__main__":
    main()
