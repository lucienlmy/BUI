"""Run the blenderui vs gpui / gpui-fast benchmark and print a Markdown table.

    cd bindings/rust/bench
    cargo build --release
    py run.py [--reps 3] [--seconds 10]

Each app measures its own process (CPU time, working set, private bytes,
frames) and prints one JSON line. GPU work does not show up as process CPU
time, so this script also samples the process's GPU engine utilisation and
GPU memory from the Windows performance counters while it runs.
"""

import argparse
import json
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXE = {"blenderui": HERE / "target/release/bui-bench.exe", "gpui": HERE / "target/release/gpui-bench.exe",
       "gpui-fast": HERE / "target/release/gpui-fast-bench.exe",
       "blenderui-gpu": HERE / "target/release/bui-bench.exe"}
# per-app environment: blenderui-gpu is blenderui with the GPU renderer
ENV = {"blenderui": {"BUI_GPU": "0"}, "blenderui-gpu": {"BUI_GPU": "1"}}


def gpu_sampler(pid, start_after, duration, out):
    """Average 3D-engine utilisation (%) and GPU memory (MB) of `pid`."""
    time.sleep(start_after)
    n = max(2, int(duration) - 1)
    script = (
        f"$c = Get-Counter -Counter '\\GPU Engine(pid_{pid}_*)\\Utilization Percentage',"
        f"'\\GPU Process Memory(pid_{pid}_*)\\Dedicated Usage','\\GPU Process Memory(pid_{pid}_*)\\Shared Usage'"
        f" -SampleInterval 1 -MaxSamples {n} -ErrorAction SilentlyContinue;"
        "foreach ($s in $c) { $u = 0; $m = 0; foreach ($v in $s.CounterSamples) {"
        " if ($v.Path -like '*utilization percentage') { $u += $v.CookedValue } else { $m += $v.CookedValue } };"
        " Write-Output (\"{0} {1}\" -f $u, $m) }"
    )
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                           capture_output=True, text=True, timeout=duration + 30)
        rows = [tuple(map(float, l.split())) for l in r.stdout.split("\n") if l.strip()]
        if rows:
            out["gpu_pct"] = statistics.mean(r[0] for r in rows)
            out["gpu_mem_mb"] = statistics.mean(r[1] for r in rows) / (1024 * 1024)
    except Exception as e:  # counters unavailable: leave the fields out
        out["gpu_error"] = str(e)


def run(app, cells, mode, warmup, seconds, extra=()):
    args = [str(EXE[app]), "--cells", str(cells), "--mode", mode,
            "--warmup", str(warmup), "--seconds", str(seconds), *extra]
    import os
    env = dict(os.environ, **ENV.get(app, {}))
    p = subprocess.Popen(args, stdout=subprocess.PIPE, text=True, env=env)
    gpu = {}
    t = threading.Thread(target=gpu_sampler, args=(p.pid, warmup + 1.0, seconds, gpu))
    t.start()
    out, _ = p.communicate(timeout=warmup + seconds + 60)
    t.join()
    line = [l for l in out.splitlines() if l.startswith("{")]
    if not line:
        raise RuntimeError(f"{app} printed no result: {out!r}")
    r = json.loads(line[-1])
    r.update(gpu)
    return r


def median(rows, key):
    vals = [r[key] for r in rows if key in r]
    return statistics.median(vals) if vals else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--warmup", type=float, default=3)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--cells", default="100,1000,3000")
    ap.add_argument("--modes", default="anim,one,idle,motion")
    ap.add_argument("--apps", default="gpui,gpui-fast,blenderui,blenderui-gpu")
    a = ap.parse_args()
    configs = []
    for cells in map(int, a.cells.split(",")):
        for mode in a.modes.split(","):
            for app in a.apps.split(","):
                configs.append((app, cells, mode, ()))
        for app in ("blenderui", "blenderui-gpu"):
            if app in a.apps.split(",") and "anim" in a.modes.split(","):
                configs.append((app, cells, "anim", ("--uncapped",)))
    results = []
    for app, cells, mode, extra in configs:
        rows = []
        for rep in range(a.reps):
            r = run(app, cells, mode, a.warmup, a.seconds, extra)
            print(f"  {app:13} {cells:5} {mode}{' uncapped' if extra else ''} #{rep + 1}: {json.dumps(r)}",
                  file=sys.stderr)
            rows.append(r)
        label = app + (" (uncapped)" if extra else "")
        results.append((label, cells, mode, {k: median(rows, k) for k in (
            "fps", "cpu_pct", "cpu_ms_per_frame", "gpu_pct", "ws_mb", "peak_ws_mb", "private_mb",
            "gpu_mem_mb", "first_frame_ms")}))
    (HERE / "results.json").write_text(json.dumps(
        [{"app": l, "cells": c, "mode": m, **v} for l, c, m, v in results], indent=1))

    def f(v, d=1):
        return "-" if v is None else f"{v:.{d}f}"

    print("| App | Cells | Mode | FPS | CPU % (1 core = 100) | CPU ms / frame | GPU 3D % | "
          "RAM working set MB | Private MB | GPU memory MB | First frame ms |")
    print("|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for label, cells, mode, v in results:
        print(f"| {label} | {cells} | {mode} | {f(v['fps'])} | {f(v['cpu_pct'])} | "
              f"{f(v['cpu_ms_per_frame'], 2) if mode != 'idle' else '-'} | {f(v['gpu_pct'])} | "
              f"{f(v['ws_mb'])} | {f(v['private_mb'])} | {f(v['gpu_mem_mb'])} | {f(v['first_frame_ms'], 0)} |")


if __name__ == "__main__":
    main()
