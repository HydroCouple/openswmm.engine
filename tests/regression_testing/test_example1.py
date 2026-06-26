"""
Placeholder Python regression test for the openswmm.engine CI pipeline.

Drives the legacy CLI (openswmm-legacy) over the same Example1.inp model the
C++ regression suite uses, and confirms it produces a non-empty .rpt report
and .out binary output. A summary file is written into ./results/ so the
workflow's artifact-upload step has something to attach.
"""

from __future__ import annotations

import subprocess
from pathlib import Path


def test_legacy_cli_runs_example1(
    example1_inp: Path,
    legacy_cli: Path,
    results_dir: Path,
    tmp_path: Path,
) -> None:
    rpt = tmp_path / "Example1.rpt"
    out = tmp_path / "Example1.out"

    completed = subprocess.run(
        [str(legacy_cli), str(example1_inp), str(rpt), str(out)],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )

    summary = results_dir / "example1_legacy_cli.txt"
    summary.write_text(
        "\n".join(
            [
                f"exe         : {legacy_cli}",
                f"inp         : {example1_inp}",
                f"rpt         : {rpt} (size={rpt.stat().st_size if rpt.exists() else 'missing'})",
                f"out         : {out} (size={out.stat().st_size if out.exists() else 'missing'})",
                f"returncode  : {completed.returncode}",
                "---- stdout ----",
                completed.stdout,
                "---- stderr ----",
                completed.stderr,
            ]
        ),
        encoding="utf-8",
    )

    assert completed.returncode == 0, (
        f"openswmm-legacy exited {completed.returncode}\n"
        f"stdout: {completed.stdout}\nstderr: {completed.stderr}"
    )
    assert rpt.is_file() and rpt.stat().st_size > 0, "report file missing or empty"
    assert out.is_file() and out.stat().st_size > 0, "output file missing or empty"
