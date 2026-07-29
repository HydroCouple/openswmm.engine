#!/usr/bin/env python3
"""Derive a time-sliced / option-overridden .inp variant from a base model.

Usage:
  make_slice.py BASE.inp OUT.inp [--start "MM/DD/YYYY HH:MM"] [--hours H]
                [--threads N]
                [--set2d KEY=VALUE]... [--del2d KEY]...

Only [OPTIONS] date/time/THREADS lines and [2D_OPTIONS] keys are touched; every
other byte passes through unchanged (mesh stays external via [2D_MESH_FILE]).
Paths that were relative to the base .inp are rewritten to absolute so the
variant can live anywhere.
"""
import os, re, argparse

ap = argparse.ArgumentParser()
ap.add_argument("base")
ap.add_argument("out")
ap.add_argument("--start", help='new start "MM/DD/YYYY HH:MM" (default: keep)')
ap.add_argument("--hours", type=float, help="duration in hours from start (default: keep end)")
ap.add_argument("--threads", type=int)
ap.add_argument("--set2d", action="append", default=[], metavar="KEY=VALUE")
ap.add_argument("--del2d", action="append", default=[], metavar="KEY")
args = ap.parse_args()

base_dir = os.path.dirname(os.path.abspath(args.base))
lines = open(args.base).read().splitlines()

start_date = start_time = None
for ln in lines:
    t = ln.split()
    if len(t) >= 2 and t[0] == "START_DATE":
        start_date = t[1]
    if len(t) >= 2 and t[0] == "START_TIME":
        start_time = t[1]

if args.start:
    start_date, start_time = args.start.split()
    if start_time.count(":") == 1:
        start_time += ":00"

end_date = end_time = None
if args.hours is not None:
    from datetime import datetime, timedelta
    st = datetime.strptime(f"{start_date} {start_time}", "%m/%d/%Y %H:%M:%S")
    en = st + timedelta(hours=args.hours)
    end_date, end_time = en.strftime("%m/%d/%Y"), en.strftime("%H:%M:%S")

set2d = {}
for kv in args.set2d:
    k, _, v = kv.partition("=")
    set2d[k.strip().upper()] = v.strip()
del2d = {k.strip().upper() for k in args.del2d}

out, section, pending_2d = [], None, dict(set2d)
for ln in lines:
    s = ln.strip()
    if s.startswith("["):
        # flush any 2D keys that were not present in the section body
        if section == "[2D_OPTIONS]" and pending_2d:
            for k, v in pending_2d.items():
                out.append(f"{k:<22} {v}")
            pending_2d = {}
        section = s.upper()
        out.append(ln)
        continue
    toks = s.split()
    key = toks[0].upper() if toks and not s.startswith(";") else None
    if section == "[OPTIONS]" and key:
        repl = {
            "START_DATE": start_date, "START_TIME": start_time,
            "REPORT_START_DATE": start_date, "REPORT_START_TIME": start_time,
            "END_DATE": end_date, "END_TIME": end_time,
            "THREADS": str(args.threads) if args.threads is not None else None,
        }
        if key in repl and repl[key] is not None:
            out.append(f"{key:<20} {repl[key]}")
            continue
    if section == "[2D_OPTIONS]" and key:
        if key in del2d:
            continue
        if key in pending_2d:
            out.append(f"{key:<22} {pending_2d.pop(key)}")
            continue
    if section == "[2D_MESH_FILE]" and key == "FILE" and len(toks) >= 2:
        p = s.split(None, 1)[1]
        if not os.path.isabs(p):
            out.append(f"FILE  {os.path.normpath(os.path.join(base_dir, p))}")
            continue
    if section == "[RAINGAGES]" and key and "FILE" in toks:
        i = next((j for j, t in enumerate(toks) if t.upper() == "FILE"), None)
        if i is not None and i + 1 < len(toks):
            p = toks[i + 1].strip('"')
            if not os.path.isabs(p):
                toks[i + 1] = f"\"{os.path.normpath(os.path.join(base_dir, p))}\""
                out.append(" ".join(toks))
                continue
    out.append(ln)

if pending_2d:  # [2D_OPTIONS] existed as last section or keys still unflushed
    out.append("")
    if not any(l.strip().upper() == "[2D_OPTIONS]" for l in lines):
        out.append("[2D_OPTIONS]")
    for k, v in pending_2d.items():
        out.append(f"{k:<22} {v}")

os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
open(args.out, "w").write("\n".join(out) + "\n")
print(f"wrote {args.out}  (start {start_date} {start_time}"
      + (f", {args.hours}h" if args.hours is not None else "") + ")")
