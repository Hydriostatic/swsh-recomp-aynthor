"""supervisor.py: the engineering loop's supervisor (state in .agent/, evidence in reports/).

  python tools/agent/supervisor.py status
  python tools/agent/supervisor.py regress [suite ...]        run tests/regression/*.json, update .agent/test-matrix.json
  python tools/agent/supervisor.py baseline [--runs 3] [--profiles 1440p30,1440p60] [--caches warm,cold]
                                            [--scenarios a,b]  benchmark matrix -> reports/baseline/<stamp>.json/.md
  python tools/agent/supervisor.py next                        pick the next executable backlog item (marks it active)
  python tools/agent/supervisor.py record <item> --status fixed|partial|failed|blocked|open --evidence "<text>"
                                          [--note "..."]       record a cycle's outcome (stagnation detection)
  python tools/agent/supervisor.py agent [--max-runs N] [--run-timeout S] [--budget-usd X]
                                         launch headless Claude Code runs that continue the loop, one at a time

State (.agent/):
  state.json      cycle number, reference commit, active item, last hypothesis / result / next action, agent runs
  backlog.json    items {id, priority P0-P3, title, status, attempts, max_attempts, stalls, repro, regression, ...}
  test-matrix.json last result of every regression check and benchmark scenario (PASSOU / FALHOU / NAO TESTADO)
  last-cycle.md, blockers.md
Rules the supervisor enforces:
  - one supervisor at a time (.agent/runtime/supervisor.lock, with the holder's pid; a dead holder's lock is taken over)
  - an item is attempted at most max_attempts times; a record whose evidence is identical to the previous one counts
    as a stall, and stall_limit stalls in a row block the item (it goes to blockers.md and the next item is chosen)
  - agent runs: wall-clock timeout per run, a maximum number of runs, an optional cost cap per run, and the loop stops
    when a run leaves the state unchanged (no new cycle recorded) twice in a row
  - nothing here deletes saves, caches or files outside reports/ and .agent/; game processes are only those it started
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AGENT = os.path.join(ROOT, ".agent")
RUNTIME = os.path.join(AGENT, "runtime")
sys.path.insert(0, os.path.join(ROOT, "tools", "swsh"))
STALL_LIMIT = 3
PRIO = {"P0": 0, "P1": 1, "P2": 2, "P3": 3}


def now():
    return datetime.datetime.now().isoformat(timespec="seconds")


def load(name, default):
    p = os.path.join(AGENT, name)
    if not os.path.exists(p):
        return default
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def save(name, data):
    os.makedirs(AGENT, exist_ok=True)
    p = os.path.join(AGENT, name)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
    os.replace(tmp, p)  # (atomic: an interrupted write never leaves half a state file)


def pid_alive(pid):
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout
    return str(pid) in out


class Lock:
    def __enter__(self):
        os.makedirs(RUNTIME, exist_ok=True)
        self.path = os.path.join(RUNTIME, "supervisor.lock")
        if os.path.exists(self.path):
            with open(self.path) as f:
                holder = f.read().strip()
            if holder.isdigit() and int(holder) != os.getpid() and pid_alive(int(holder)):
                sys.exit(f"another supervisor is running (pid {holder})")
        with open(self.path, "w") as f:
            f.write(str(os.getpid()))
        return self

    def __exit__(self, *a):
        try:
            os.remove(self.path)
        except OSError:
            pass


def git_head():
    return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()


# ------------------------------------------------------------------------------------------------ scenario runs
def run_scenario(name, profile, cache, label, extra=()):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "swsh", "scenario.py"), name, "--profile", profile, "--cache",
           cache, "--label", label, *extra]
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout
    last = [line for line in out.splitlines() if line.startswith("{")]
    if not last:
        return None, {"status": "HARNESS-ERROR", "output": out[-2000:]}
    info = json.loads(last[-1])
    run_dir = os.path.join(ROOT, info["run"])
    with open(os.path.join(run_dir, "summary.json")) as f:
        return run_dir, json.load(f)


def eval_check(c, run_dir, summary):
    import imgcheck
    fr = lambda n: os.path.join(run_dir, "frames", f"frame_{n:05d}.png")  # noqa: E731
    t = c["type"]
    try:
        if t == "status_ok":
            val, ok = summary["status"], summary["status"] == "OK"
        elif t == "upright":
            val = round(imgcheck.upright_margin(fr(c["a"]), fr(c["b"])), 3)
            ok = val >= c["min"]
        elif t == "black_fraction_max":
            val = round(imgcheck.black_fraction(fr(c["frame"]), c["region"]), 3)
            ok = val <= c["max"]
        elif t == "luma_min":
            val = round(imgcheck.luma(fr(c["frame"]), c["region"]), 1)
            ok = val >= c["min"]
        elif t == "skipped_window_max":
            val = summary.get("skipped_window")
            ok = val is not None and val <= c["max"]
        elif t == "alternation_max":  # mean SSIM(n, n+2) - SSIM(n, n+1) over consecutive shots: > 0 = alternating images
            fs = [fr(n) for n in range(c["frames"][0], c["frames"][1] + 1) if os.path.exists(fr(n))]
            if len(fs) < 4:
                return {"result": "NAO TESTADO", "value": "too few shots"}
            a = sum(imgcheck.ssim(fs[i], fs[i + 1]) for i in range(len(fs) - 1)) / (len(fs) - 1)
            b = sum(imgcheck.ssim(fs[i], fs[i + 2]) for i in range(len(fs) - 2)) / (len(fs) - 2)
            val = round(b - a, 3)
            ok = val <= c["max"]
        elif t == "skipped_total_max":
            val = summary.get("skipped_total")
            ok = val is not None and val <= c["max"]
        elif t == "vkval_errors_max":
            val = summary.get("vkval_errors")
            ok = val is not None and val <= c["max"]
        else:
            return {"result": "NAO TESTADO", "value": f"unknown check type {t}"}
    except FileNotFoundError as e:
        return {"result": "NAO TESTADO", "value": f"missing evidence: {os.path.basename(e.filename or '')}"}
    return {"result": "PASSOU" if ok else "FALHOU", "value": val}


def cmd_regress(args):
    suites = args.suites or [os.path.splitext(os.path.basename(p))[0] for p in sorted(glob.glob(os.path.join(ROOT, "tests", "regression", "*.json")))]
    matrix = load("test-matrix.json", {"checks": {}, "benchmarks": {}})
    failed_hard = 0
    for s in suites:
        with open(os.path.join(ROOT, "tests", "regression", s + ".json")) as f:
            suite = json.load(f)
        run_dir, summary = run_scenario(suite["scenario"], suite.get("profile", "1440p30"), suite.get("cache", "warm"), "regress")
        k = 2 if suite.get("profile", "1440p30").endswith("60") else 1  # (frames are given at 30 fps; shots double at 60)
        for c in suite["checks"]:
            key = f"{s}/{c['id']}"
            if k != 1:
                c = dict(c)
                for f in ("a", "b", "frame"):
                    if f in c: c[f] *= k
                if "frames" in c: c["frames"] = [x * k for x in c["frames"]]
            r = eval_check(c, run_dir, summary) if run_dir else {"result": "NAO TESTADO", "value": summary.get("status")}
            expect_fail = c.get("expect") == "fail"
            r.update({"bug": c.get("bug", ""), "expected": "fail (open bug)" if expect_fail else "pass",
                      "run": os.path.relpath(run_dir, ROOT) if run_dir else "", "when": now(), "commit": git_head()})
            matrix["checks"][key] = r
            mark = r["result"] + (" (esperado: bug aberto)" if expect_fail and r["result"] == "FALHOU" else "")
            if r["result"] == "FALHOU" and not expect_fail:
                failed_hard += 1
            if r["result"] == "PASSOU" and expect_fail:
                mark += " (bug aberto passou: conferir e fechar)"
            print(f"{key}: {mark} [{r['value']}]", flush=True)
    save("test-matrix.json", matrix)
    return 1 if failed_hard else 0


# ------------------------------------------------------------------------------------------------ baseline
def cmd_baseline(args):
    from statistics import median
    scenarios = args.scenarios.split(",") if args.scenarios else ["room", "groundfloor", "postwick", "stairs_roundtrip"]
    rows = []
    for prof in args.profiles.split(","):
        for cache in args.caches.split(","):
            for sc in scenarios:
                runs = []
                for i in range(args.runs):
                    run_dir, s = run_scenario(sc, prof, cache, f"bl-{prof}-{cache}")
                    runs.append({"run": os.path.relpath(run_dir, ROOT) if run_dir else "", **s})
                    print(f"{sc} {prof} {cache} #{i + 1}: {s.get('status')} "
                          f"fps {(s.get('frametime') or {}).get('fps_mean')} p99 {(s.get('frametime') or {}).get('p99_ms')}", flush=True)
                ok = [r for r in runs if r.get("frametime") and not r.get("contaminated")]  # (another runtime shared the machine)
                agg = {}
                for k in ("fps_mean", "median_ms", "p95_ms", "p99_ms", "max_ms", "over_50ms"):
                    vals = [r["frametime"][k] for r in ok]
                    agg[k] = {"median": median(vals), "min": min(vals), "max": max(vals)} if vals else None
                rows.append({"scenario": sc, "profile": prof, "cache": cache, "runs": runs, "aggregate": agg,
                             "skipped_total": [r.get("skipped_total") for r in runs],
                             "compiles_total": [r.get("compiles_total") for r in runs],
                             "statuses": [r.get("status") for r in runs]})
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.join(ROOT, "reports", "baseline")
    os.makedirs(out, exist_ok=True)
    data = {"when": now(), "commit": git_head(), "rows": rows}
    with open(os.path.join(out, f"{stamp}.json"), "w") as f:
        json.dump(data, f, indent=1)
    lines = [f"# Baseline {stamp} (commit {data['commit']})", "",
             "Mediana entre execuções (mín-máx). Frametime em ms, janela de medição de cada cenário (tests/scenarios).", "",
             "| cenário | perfil | cache | runs | status | FPS | mediana | P95 | P99 | máx | >50 ms | draws pulados (total) | compilações |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        a = r["aggregate"]

        def f(k, d=1):
            v = a.get(k)
            return f"{v['median']:.{d}f} ({v['min']:.{d}f}-{v['max']:.{d}f})" if v else "—"
        lines.append(f"| {r['scenario']} | {r['profile']} | {r['cache']} | {len(r['runs'])} | {','.join(r['statuses'])} | "
                     f"{f('fps_mean')} | {f('median_ms')} | {f('p95_ms')} | {f('p99_ms')} | {f('max_ms', 0)} | {f('over_50ms', 0)} | "
                     f"{r['skipped_total']} | {r['compiles_total']} |")
    with open(os.path.join(out, f"{stamp}.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    matrix = load("test-matrix.json", {"checks": {}, "benchmarks": {}})
    for r in rows:
        matrix["benchmarks"][f"{r['scenario']}/{r['profile']}/{r['cache']}"] = {
            "aggregate": r["aggregate"], "statuses": r["statuses"], "report": f"reports/baseline/{stamp}.md", "when": now()}
    save("test-matrix.json", matrix)
    print(f"reports/baseline/{stamp}.md")


# ------------------------------------------------------------------------------------------------ backlog
def cmd_next(args):
    backlog = load("backlog.json", {"items": []})
    state = load("state.json", {})
    open_items = [i for i in backlog["items"] if i["status"] in ("open", "active", "partial")
                  and i.get("attempts", 0) < i.get("max_attempts", 5) and not i.get("needs_human")]
    if not open_items:
        print("nothing executable left (see .agent/blockers.md)")
        return 1
    open_items.sort(key=lambda i: (i["status"] != "active", PRIO.get(i["priority"], 9), i.get("order", 99)))
    item = open_items[0]
    for i in backlog["items"]:
        if i["status"] == "active" and i is not item:
            i["status"] = "open"
    item["status"] = "active"
    state.update({"active_item": item["id"], "updated": now()})
    save("backlog.json", backlog)
    save("state.json", state)
    print(json.dumps(item, indent=1, ensure_ascii=False))
    return 0


def cmd_record(args):
    backlog = load("backlog.json", {"items": []})
    state = load("state.json", {})
    item = next((i for i in backlog["items"] if i["id"] == args.item), None)
    if not item:
        sys.exit(f"no backlog item {args.item}")
    ev_hash = hashlib.sha1(args.evidence.encode()).hexdigest()[:12]
    item["attempts"] = item.get("attempts", 0) + 1
    item["stalls"] = item.get("stalls", 0) + 1 if ev_hash == item.get("last_evidence_hash") else 0
    item["last_evidence_hash"] = ev_hash
    item.setdefault("history", []).append({"when": now(), "commit": git_head(), "status": args.status,
                                           "evidence": args.evidence, "note": args.note or ""})
    item["status"] = {"fixed": "fixed", "partial": "partial", "failed": "open", "blocked": "blocked", "open": "open"}[args.status]
    if item["status"] in ("open", "partial") and (item["stalls"] >= STALL_LIMIT or item["attempts"] >= item.get("max_attempts", 5)):
        item["status"] = "blocked"
        args.note = (args.note or "") + f" [supervisor: {'stalled' if item['stalls'] >= STALL_LIMIT else 'attempt limit'}]"
    if item["status"] == "blocked":
        with open(os.path.join(AGENT, "blockers.md"), "a", encoding="utf-8") as f:
            f.write(f"\n## {item['id']} ({now()}, commit {git_head()})\n{item['title']}\n\n- evidência: {args.evidence}\n"
                    f"- nota: {args.note or ''}\n")
    state["cycle"] = state.get("cycle", 0) + 1
    state.update({"last_item": item["id"], "last_result": args.status, "last_evidence": args.evidence,
                  "reference_commit": git_head(), "updated": now(), "active_item": None})
    save("backlog.json", backlog)
    save("state.json", state)
    print(f"cycle {state['cycle']}: {item['id']} -> {item['status']}")


# ------------------------------------------------------------------------------------------------ agent runs
AGENT_PROMPT = """Você continua o ciclo de engenharia do port do Pokémon Sword 1.3.2 em D:\\projects\\swsh-recomp.
Leia primeiro .agent/state.json, .agent/backlog.json, .agent/last-cycle.md, .agent/blockers.md e o plano em
docs/engineering/README.md. Rode `python tools/agent/supervisor.py next` para pegar o item ativo e trabalhe só nele:
reproduzir (tools/swsh/scenario.py), achar a causa raiz com evidência, a menor correção, compilar os dois runtimes
(make; make B=build/v132 GEN=generated-132), rodar o cenário e `python tools/agent/supervisor.py regress`, escrever
reports/cycles/<NNN>.md no formato do relatório de ciclo e .agent/last-cycle.md, e registrar com
`python tools/agent/supervisor.py record <item> --status ... --evidence "..."`. Commits em inglês, Conventional Commits,
sem linha Co-Authored-By. Nunca use save-sword-132 nem cache/ diretamente, nunca encerre processos que não iniciou,
nunca rode git push, reset --hard ou limpezas amplas. Se o item exigir o jogador (um save num lugar específico), marque
needs_human no backlog, registre como blocked e pare. Não invente resultados: o que não rodou é NÃO TESTADO."""


def cmd_agent(args):
    state = load("state.json", {})
    unchanged = 0
    for n in range(args.max_runs):
        before = state.get("cycle", 0)
        if cmd_next(argparse.Namespace()) != 0:
            break
        os.makedirs(RUNTIME, exist_ok=True)
        log = os.path.join(RUNTIME, f"agent-{datetime.datetime.now():%Y%m%d-%H%M%S}.json")
        cmd = ["claude", "-p", AGENT_PROMPT, "--output-format", "json", "--permission-mode", "acceptEdits",
               "--allowedTools", "Bash(make:*)", "Bash(python tools/swsh/*)", "Bash(python tools/agent/*)",
               "Bash(git status:*)", "Bash(git diff:*)", "Bash(git log:*)", "Bash(git add:*)", "Bash(git commit:*)",
               "Read", "Edit", "Write", "Grep", "Glob",
               "--disallowedTools", "Bash(git push:*)", "Bash(git reset:*)", "Bash(rm -rf:*)"]
        if args.budget_usd:
            cmd += ["--max-budget-usd", str(args.budget_usd)]
        state.setdefault("agent_runs", []).append({"started": now(), "log": os.path.relpath(log, ROOT)})
        save("state.json", state)
        try:
            with open(log, "w", encoding="utf-8") as f:
                code = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=args.run_timeout,
                                      shell=os.name == "nt").returncode
        except subprocess.TimeoutExpired:
            code = "timeout"
        state = load("state.json", {})
        state["agent_runs"][-1].update({"ended": now(), "exit": code})
        save("state.json", state)
        unchanged = unchanged + 1 if state.get("cycle", 0) == before else 0
        print(f"agent run {n + 1}: exit {code}, cycle {before} -> {state.get('cycle', 0)}", flush=True)
        if unchanged >= 2:
            print("two agent runs without a recorded cycle: stopping (stagnation)")
            break


def cmd_status(args):
    state = load("state.json", {})
    backlog = load("backlog.json", {"items": []})
    print(json.dumps(state, indent=1, ensure_ascii=False))
    for i in sorted(backlog["items"], key=lambda i: (PRIO.get(i["priority"], 9), i.get("order", 99))):
        print(f"{i['priority']} {i['id']:<14} {i['status']:<8} tentativas {i.get('attempts', 0)}/{i.get('max_attempts', 5)}  {i['title']}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    r = sub.add_parser("regress")
    r.add_argument("suites", nargs="*")
    b = sub.add_parser("baseline")
    b.add_argument("--runs", type=int, default=3)
    b.add_argument("--profiles", default="1440p30,1440p60")
    b.add_argument("--caches", default="warm,cold")
    b.add_argument("--scenarios", default="")
    sub.add_parser("next")
    c = sub.add_parser("record")
    c.add_argument("item")
    c.add_argument("--status", required=True, choices=["fixed", "partial", "failed", "blocked", "open"])
    c.add_argument("--evidence", required=True)
    c.add_argument("--note", default="")
    a = sub.add_parser("agent")
    a.add_argument("--max-runs", type=int, default=3)
    a.add_argument("--run-timeout", type=int, default=3 * 3600)
    a.add_argument("--budget-usd", type=float, default=0)
    args = ap.parse_args()
    fn = {"status": cmd_status, "regress": cmd_regress, "baseline": cmd_baseline, "next": cmd_next,
          "record": cmd_record, "agent": cmd_agent}[args.cmd]
    if args.cmd in ("regress", "baseline", "agent"):  # (the ones that run the game: one at a time)
        with Lock():
            sys.exit(fn(args) or 0)
    sys.exit(fn(args) or 0)


if __name__ == "__main__":
    main()
