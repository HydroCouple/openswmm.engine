#!/usr/bin/env python3
"""Run one openswmm case, time it, extract 1D+2D metrics, append a CSV row.

Usage: run_one.py TAG INP.inp [--cli PATH] [--outdir DIR] [--env KEY=VAL]...

Defaults pin the serial-benchmark environment:
  OPENSWMM_2D_BACKEND=cpu   (the >=20k-cell GPU plugin auto-gate must never fire)
  OPENSWMM_PERF=1           ([PERF] wall-time split line)
Outputs in --outdir (default out/): TAG.rpt, TAG.out, TAG.log, TAG.cverr, results.csv row.
"""
import sys, os, time, subprocess, re, csv, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
CLI_DEFAULT = os.environ.get(
    "OPENSWMM_CLI",
    os.path.normpath(os.path.join(HERE, "..", "..", "build", "darwin", "src", "cli", "openswmm")),
)

ap = argparse.ArgumentParser()
ap.add_argument("tag")
ap.add_argument("inp")
ap.add_argument("--cli", default=CLI_DEFAULT)
ap.add_argument("--outdir", default=os.path.join(HERE, "out"))
ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL")
args = ap.parse_args()

os.makedirs(args.outdir, exist_ok=True)
rpt = os.path.join(args.outdir, f"{args.tag}.rpt")
out = os.path.join(args.outdir, f"{args.tag}.out")
log = os.path.join(args.outdir, f"{args.tag}.log")
cverr = os.path.join(args.outdir, f"{args.tag}.cverr")

env = dict(os.environ)
env.setdefault("OPENSWMM_2D_BACKEND", "cpu")
env.setdefault("OPENSWMM_PERF", "1")
for kv in args.env:
    k, _, v = kv.partition("=")
    env[k] = v

t0 = time.time()
with open(log, "w") as lo, open(cverr, "w") as ce:
    rc = subprocess.call([args.cli, args.inp, rpt, out], stdout=lo, stderr=ce, env=env)
wall = time.time() - t0

logtxt = open(log).read().replace("\r", "\n")
errtxt = open(cverr).read().replace("\r", "\n") if os.path.exists(cverr) else ""
eng = re.search(r"completed in ([0-9.]+) seconds", logtxt)
perf = re.search(r"\[PERF\][^\n]*", logtxt + "\n" + errtxt)
ncv = sum(1 for ln in errtxt.splitlines() if ln.strip())

rpttxt = open(rpt).read() if os.path.exists(rpt) else ""
conts = re.findall(r"Continuity Error \(%\)\s*\.*\s*(-?[0-9.]+)", rpttxt)

def rgrab(label, txt=rpttxt):
    m = re.search(re.escape(label) + r"\s*\.*\s*(-?[0-9.eE+]+)", txt)
    return m.group(1) if m else ""

row = {
    "tag": args.tag,
    "wall_s": f"{wall:.1f}",
    "engine_s": eng.group(1) if eng else "",
    "solver_steps": rgrab("Internal BDF Steps") or rgrab("Internal Steps"),
    "avg_internal_step_s": rgrab("Avg Internal Step (s)"),
    "frozen_windows": rgrab("Frozen (Failed) Windows"),
    "err_fail_lines": ncv,
    "cont_err_pct": ";".join(conts),
    "perf": perf.group(0) if perf else "",
    "rc": rc,
}
csvp = os.path.join(args.outdir, "results.csv")
newfile = not os.path.exists(csvp)
with open(csvp, "a", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(row.keys()))
    if newfile:
        w.writeheader()
    w.writerow(row)

print(f"[{args.tag}] wall={wall:.1f}s engine={row['engine_s'] or '?'}s "
      f"steps={row['solver_steps'] or '?'} frozen={row['frozen_windows'] or '0'} "
      f"errlines={ncv} cont%={row['cont_err_pct']} rc={rc}")
sys.exit(rc)
