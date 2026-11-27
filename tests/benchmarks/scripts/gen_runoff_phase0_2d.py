#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""2D rain-on-grid decks for the runoff / infiltration profiling round.

Derives variants of the 2026-08-22 2D performance-review meshes
(``bench2d_19602.inp``, ``bench2d_79202.inp``: a 40 mm/h half-hour storm on a
structured mesh, 1 s routing step, 1 h run) by appending the infiltration and
aquifer sections the review did not have. The mesh bytes are untouched;
``REPORT_2D`` is switched off so the 2D results writer does not sit in the
timings.

Variants per base deck (``<base>_<variant>.inp``):

================  ==========================================================
Variant           Adds
================  ==========================================================
noinfil           nothing (the control)
horton            ``[2D_INFILTRATION_DEFAULTS] * HORTON``
modhorton         ``* MODIFIED_HORTON``
grnampt           ``* GREEN_AMPT``
modgrnampt        ``* MODIFIED_GREEN_AMPT``
cn                ``* CURVE_NUMBER``
horton_step1      horton with ``INFIL_STEP 0:00:01`` (the routing step)
grnampt_step1     grnampt with ``INFIL_STEP 0:00:01``
aq_sigma_russo    whole-mesh ``[2D_AQUIFER]`` + horton row, SIGMA column, Russo
aq_closed         the same under the CLOSED_FORM closure
aq_enslaved       the same under the ENSLAVED closure
aq_sigma_gardner  SIGMA column, Gardner retention
aq_sigma_bc       SIGMA column, Brooks-Corey retention
aq_sigma_vg       SIGMA column, van Genuchten retention
================  ==========================================================

The aquifer decks keep an infiltration default row because the aquifer's
intake is published on the infiltration cadence, which only runs while the
2D infiltration model is active (SurfaceRouter2D::step). They also end after
10 simulated minutes (the first sanity run took 13 min of wall time for the
hour on the smaller mesh, 250 times the Horton deck), which still covers 600
routing steps of rain on the σ columns. The larger base gets the control,
horton, grnampt and aq_sigma_russo variants only.

Deterministic: pure text operations, no RNG.

Usage::

    python3 gen_runoff_phase0_2d.py --src <dir of bench2d_*.inp> --out-dir <dir>
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# SI project (CMS): rates in mm/h, depths in mm.
INFIL_ROWS = {
    "horton":     "*  HORTON               75  12  4  7  0",
    "modhorton":  "*  MODIFIED_HORTON      75  12  4  7  0",
    "grnampt":    "*  GREEN_AMPT           90  12  0.25",
    "modgrnampt": "*  MODIFIED_GREEN_AMPT  90  12  0.25",
    "cn":         "*  CURVE_NUMBER         80  -  7",
}

# KS 36 mm/h, 4 m column, porosity 0.45, residual 0.10, alpha 2 1/m — the
# whole-mesh row of tests/unit/engine/test_2d_aquifer.cpp.
AQ_ROW = "*  36.0  4.0  0.45  0.10  2.0"
AQ_VARIANTS = {
    "aq_sigma_russo":   ("SIGMA", "RUSSO", ""),
    "aq_closed":        ("CLOSED_FORM", "RUSSO", ""),
    "aq_enslaved":      ("ENSLAVED", "RUSSO", ""),
    "aq_sigma_gardner": ("SIGMA", "GARDNER", ""),
    "aq_sigma_bc":      ("SIGMA", "BROOKS_COREY", "  PSI_B 0.2  LAMBDA 0.4"),
    "aq_sigma_vg":      ("SIGMA", "VAN_GENUCHTEN", "  N 1.6  L 0.5"),
}

BASES = {
    "bench2d_19602": None,  # every variant
    "bench2d_79202": ("noinfil", "horton", "grnampt", "aq_sigma_russo"),
}


def infil_section(method: str, step: str | None) -> str:
    out = []
    if step:
        out.append("[2D_INFILTRATION_OPTIONS]")
        out.append(f"INFIL_STEP           {step}")
        out.append("")
    out.append("[2D_INFILTRATION_DEFAULTS]")
    out.append(";;TAG  METHOD  P1  P2  P3  P4  P5")
    out.append(INFIL_ROWS[method])
    out.append("")
    return "\n".join(out)


def aquifer_section(closure: str, soil: str, keywords: str) -> str:
    return "\n".join([
        "[2D_AQUIFER_OPTIONS]",
        f"CLOSURE              {closure}",
        f"SOIL_CHAR            {soil}",
        "",
        "[2D_AQUIFER]",
        ";;Scope  KS  ZS  THETA_S  THETA_R  ALPHA",
        AQ_ROW + keywords,
        "",
    ])


def variants() -> dict[str, str]:
    v = {"noinfil": ""}
    for m in INFIL_ROWS:
        v[m] = infil_section(m, None)
    v["horton_step1"] = infil_section("horton", "0:00:01")
    v["grnampt_step1"] = infil_section("grnampt", "0:00:01")
    for name, (closure, soil, kw) in AQ_VARIANTS.items():
        v[name] = infil_section("horton", None) + "\n" + aquifer_section(closure, soil, kw)
    return v


def derive(base_text: str, extra: str, end_time: str | None = None) -> str:
    text, n = re.subn(r"^(REPORT_2D\s+)YES\s*$", r"\1NO", base_text, flags=re.MULTILINE)
    if n != 1:
        raise ValueError("expected exactly one REPORT_2D YES line in the base deck")
    if end_time:
        text, n = re.subn(r"^(END_TIME\s+)\S+\s*$", rf"\g<1>{end_time}", text, flags=re.MULTILINE)
        if n != 1:
            raise ValueError("expected exactly one END_TIME line in the base deck")
    text = text.rstrip("\n") + "\n"
    if extra:
        text += "\n" + extra
    return text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True, help="directory holding bench2d_19602.inp / bench2d_79202.inp")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)

    src = Path(args.src)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    all_variants = variants()
    for base, allowed in BASES.items():
        path = src / f"{base}.inp"
        if not path.exists():
            print(f"missing base deck: {path}", file=sys.stderr)
            return 2
        base_text = path.read_text()
        for name, extra in all_variants.items():
            if allowed is not None and name not in allowed:
                continue
            target = out / f"{base}_{name}.inp"
            if target.exists() and not args.force:
                print(f"  skip (exists)  {target.name}")
                continue
            end_time = "00:10:00" if name.startswith("aq_") else None
            target.write_text(derive(base_text, extra, end_time))
            print(f"  wrote  {target.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
