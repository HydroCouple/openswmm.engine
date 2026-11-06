#!/usr/bin/env python3
"""Writer round-trip sweep — does a saved model still mean what it meant?

Nothing else in either tree compares a WRITTEN .inp against the original.
`tests/parity/run_corpus.sh` compares v6-base against v6-patched on pristine
decks; the benchmarks sweep compares legacy against v6, also on pristine decks.
Both are blind to the writer. This closes that gap.

Per deck, four independent gates:

  G1 legacy-accepts  legacy runs the round-tripped file without a fatal error
  G2 legacy-agrees   legacy(original).out == legacy(gen1).out
  G3 v6-agrees       v6(original).out     == v6(gen1).out
  G4 gen-zero diff   normalised token diff of original vs gen1

G2 is the arbiter. A deck can differ textually (G4) and still be faithful —
legacy itself normalises values, e.g. a negative conduit offset is clamped to
zero with a warning (link.c:1061-1069), and our engine reproduces that, so the
saved file carries the clamped value. Conversely a deck can pass G4 on the
sections we know how to compare and still fail G2. Read the two together.

Why not compare generation 1 against generation 2? Because that is what the
existing gtest suite does, and it cannot see a value that decays to a fixed
point: [LOSSES] seepage written in internal units prints 0.000000 after one
save and then converges perfectly at zero forever. Always compare against
generation ZERO.

Usage:
  run_writer_roundtrip.py --corpus <dir> --out <dir> [--limit N] [--workers N]
                          [--no-run]        # G1/G4 only, skip the .out gates
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import traceback
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENGINE_ROOT = HERE.parents[2]
BENCH_ROOT = ENGINE_ROOT.parent / "openswmm.engine.benchmarks"

sys.path.insert(0, str(HERE))
import inp_normalize  # noqa: E402

# compare_full lives in the benchmarks repo; the sweep degrades to G1/G4 if it
# is not importable rather than refusing to run at all.
_HAVE_COMPARE = False
try:
    sys.path.insert(0, str(BENCH_ROOT))
    from harness.compare import compare_full  # noqa: E402
    _HAVE_COMPARE = True
except Exception:  # pragma: no cover - environment dependent
    pass

RT_WRITE = HERE / "rt_write"
DEFAULT_LEGACY = (ENGINE_ROOT / "build/darwin-parity3/bin/Release/openswmm-legacy")
DEFAULT_V6 = (ENGINE_ROOT / "build/darwin/bin/Release/openswmm")

# Legacy writes an EMPTY .out unless the deck reports objects, which would make
# G2/G3 vacuously green. Both sides get full reporting forced on.
_REPORT_BLOCK = (
    "\n[REPORT]\n"
    "INPUT NO\nCONTINUITY YES\nFLOWSTATS YES\n"
    "SUBCATCHMENTS ALL\nNODES ALL\nLINKS ALL\n"
)
_SECT_RE = re.compile(r"^\s*\[([^\]]+)\]", re.M)

# A fatal legacy error appears in the .rpt; the EXIT CODE IS NOT RELIABLE —
# legacy exits 0 having written "ERROR 211: ..." and no results.
_ERR_RE = re.compile(r"^\s*ERROR\s+(\d+)", re.M)


def force_report_all(text: str) -> str:
    """Strip any [REPORT] section and append one that reports everything."""
    out, drop = [], False
    for line in text.splitlines(True):
        m = _SECT_RE.match(line)
        if m:
            drop = m.group(1).strip().upper() == "REPORT"
        if not drop:
            out.append(line)
    return "".join(out) + _REPORT_BLOCK


def run(cmd: list[str], timeout: int, cwd: str | None = None) -> tuple[int, str]:
    """Run a command, always inside `cwd`.

    Running the engines with the repo as the working directory litters the
    source tree: decks name report/LID output files by relative — and sometimes
    absolute Windows — paths, which land as literal filenames in the CWD, and
    legacy leaves `swmmXXXXXX` scratch files behind. Each deck gets its own
    directory so all of that stays with the deck's artifacts.
    """
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, errors="ignore", cwd=cwd)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return -9, "TIMEOUT"


def rpt_errors(rpt: Path) -> list[str]:
    """Fatal legacy errors, as whole lines.

    The full text carries the offending token and the section — "invalid number
    0:00:00 at line 50 of [OPTION] section" is the entire diagnosis, where a
    bare "ERROR 211" is not. Captured here because the .rpt itself is pruned.
    """
    if not rpt.exists():
        return ["no .rpt produced"]
    out = []
    for line in rpt.read_text(errors="ignore").splitlines():
        if _ERR_RE.match(line):
            out.append(line.strip()[:200])
    return out


def grade_deck(deck: Path, outdir: Path, legacy: Path, v6: Path,
               timeout: int, do_run: bool, keep: str = "fail") -> dict:
    """Round-trip one deck and grade it. Never raises: returns a record.

    `keep` controls what survives on disk: "all", "fail" (the default — only
    decks that fail a gate stay reviewable), or "none". A full gated sweep of
    this corpus writes four .out/.rpt pairs per deck; retaining every one filled
    a 900 GB volume at deck 738.
    """
    rec: dict = {"deck": str(deck), "status": "ok"}
    work = outdir / re.sub(r"[^A-Za-z0-9_.-]", "_", str(deck.relative_to(deck.anchor)))[-120:]
    work.mkdir(parents=True, exist_ok=True)
    try:
        src = work / "gen0.inp"
        # Legacy originals are frequently CRLF; strip \r once here so every
        # later line-based comparison is not swamped by false differences.
        raw = deck.read_text(errors="ignore").replace("\r\n", "\n")
        src.write_text(raw)

        gen1 = work / "gen1.inp"
        rc, log = run([str(RT_WRITE), str(src), str(gen1), "--lenient"],
                      timeout, cwd=str(work))
        rec["write_rc"] = rc
        rec["write_warnings"] = [l[5:] for l in log.splitlines()
                                 if l.startswith("WARN ")]
        if rc != 0 or not gen1.exists():
            rec["status"] = "write-failed"
            rec["write_log"] = log[:1000]
            return rec

        # ---- G4: normalised gen0 vs gen1 -------------------------------
        try:
            findings = inp_normalize.diff(raw, gen1.read_text(errors="ignore"))
            rec["g4_count"] = len(findings)
            rec["g4"] = "PASS" if not findings else "DIFF"
            rec["g4_findings"] = findings[:60]
            rec["g4_sections"] = sorted({f"{x['section']}/{x['kind']}"
                                         for x in findings})
        except Exception as e:
            rec["g4"] = "ERROR"
            rec["g4_error"] = f"{e}"

        if not do_run:
            return rec

        # ---- run both files through both engines -----------------------
        runs = {}
        for tag, text in (("gen0", raw), ("gen1", gen1.read_text(errors="ignore"))):
            rp = work / f"{tag}_run.inp"
            rp.write_text(force_report_all(text))
            for eng_tag, exe in (("legacy", legacy), ("v6", v6)):
                rpt = work / f"{tag}_{eng_tag}.rpt"
                out = work / f"{tag}_{eng_tag}.out"
                r, _ = run([str(exe), str(rp), str(rpt), str(out)],
                           timeout, cwd=str(work))
                runs[(tag, eng_tag)] = (r, rpt, out, rpt_errors(rpt))

        # G1 — legacy must accept the round-tripped file. Judged on the .rpt.
        g1_err = runs[("gen1", "legacy")][3]
        base_err = runs[("gen0", "legacy")][3]
        if base_err:
            # The original already fails legacy; the round trip cannot be
            # graded against it. Not a writer finding.
            rec["g1"] = "BASE-ERROR"
            rec["g1_detail"] = base_err[:5]
        else:
            rec["g1"] = "PASS" if not g1_err else "FAIL"
            if g1_err:
                rec["g1_detail"] = g1_err[:5]

        # G2 / G3 — same engine, original vs round-tripped.
        for gate, eng_tag in (("g2", "legacy"), ("g3", "v6")):
            a = runs[("gen0", eng_tag)]
            b = runs[("gen1", eng_tag)]
            if a[3] or b[3] or not a[2].exists() or not b[2].exists():
                rec[gate] = "SKIP"
                continue
            if not _HAVE_COMPARE:
                rec[gate] = "NO-COMPARE"
                continue
            try:
                res = compare_full(str(a[2]), str(b[2]))
                verdict = res.get("verdict") or res.get("status")
                rec[gate] = "PASS" if verdict in ("PASS", "IDENTICAL", True) else "FAIL"
                rec[gate + "_detail"] = {
                    k: res[k] for k in ("verdict", "status", "max_rel",
                                        "offenders", "fault")
                    if k in res}
            except Exception as e:
                rec[gate] = "ERROR"
                rec[gate + "_detail"] = f"{e}"
    except Exception as e:  # pragma: no cover
        rec["status"] = "harness-error"
        rec["error"] = f"{e}\n{traceback.format_exc()[:600]}"

    # Reclaim the per-deck artifacts. A failing deck stays on disk so the
    # evidence is reviewable (CLAUDE.md §4.1); a passing one is ~23 MB of
    # .out/.rpt nobody will read.
    try:
        failed = (rec.get("status") != "ok"
                  or any(rec.get(g) in ("FAIL", "ERROR")
                         for g in ("g1", "g2", "g3")))
        if keep == "none" or (keep == "fail" and not failed):
            shutil.rmtree(work, ignore_errors=True)
        elif keep != "all":
            # A kept failure needs the two .inp generations — that pair is what
            # diagnoses a writer bug. Everything else is regenerable and huge:
            # a single big deck's four .out files run to 250 MB and its .rpt
            # files to tens of MB, which at corpus scale is ~20 GB. The .rpt
            # error codes and the G4 findings are already in the record.
            for f in work.iterdir():
                if f.is_file() and f.name not in ("gen0.inp", "gen1.inp"):
                    f.unlink(missing_ok=True)
    except Exception:
        pass
    return rec


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=str(BENCH_ROOT / "corpus"))
    ap.add_argument("--out", default=str(HERE / "_sweep"))
    ap.add_argument("--legacy", default=str(DEFAULT_LEGACY))
    ap.add_argument("--v6", default=str(DEFAULT_V6))
    ap.add_argument("--limit", type=int, default=0)
    # 8 workers has produced false timeouts on this corpus before; 4 is the
    # measured-safe default. Raise it only with a raised --timeout.
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--no-run", action="store_true",
                    help="G1/G4 only — skip the engine runs")
    ap.add_argument("--keep", choices=("all", "fail", "none"), default="fail",
                    help="which per-deck artifacts survive (default: fail). "
                         "'all' needs ~25 GB for a full gated sweep.")
    a = ap.parse_args()

    if not RT_WRITE.exists():
        print(f"error: {RT_WRITE} not built. See build_rt_write.sh", file=sys.stderr)
        return 2

    decks = sorted(Path(a.corpus).rglob("*.inp"))
    if a.limit:
        decks = decks[: a.limit]
    # Absolute: every subprocess runs with cwd set to its own deck directory,
    # so a relative --out would resolve against the wrong place.
    outdir = Path(a.out).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    jsonl = outdir / "results.jsonl"

    do_run = not a.no_run
    if do_run and not _HAVE_COMPARE:
        print("warning: harness.compare unavailable — G2/G3 will report "
              "NO-COMPARE", file=sys.stderr)

    print(f"{len(decks)} decks -> {outdir}  (workers={a.workers}, "
          f"run={'yes' if do_run else 'no'})")

    tally: dict[str, int] = {}
    with jsonl.open("w") as fh, ThreadPoolExecutor(max_workers=a.workers) as ex:
        futs = {ex.submit(grade_deck, d, outdir,
                          Path(a.legacy).resolve(), Path(a.v6).resolve(),
                          a.timeout, do_run, a.keep): d for d in decks}
        for i, fut in enumerate(as_completed(futs), 1):
            rec = fut.result()
            fh.write(json.dumps(rec) + "\n")
            fh.flush()
            for g in ("status", "g1", "g2", "g3", "g4"):
                if g in rec:
                    tally[f"{g}={rec[g]}"] = tally.get(f"{g}={rec[g]}", 0) + 1
            if i % 25 == 0 or i == len(decks):
                print(f"  {i}/{len(decks)}", flush=True)

    print("\n--- tally ---")
    for k in sorted(tally):
        print(f"{tally[k]:6d}  {k}")
    print(f"\nrecords: {jsonl}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
