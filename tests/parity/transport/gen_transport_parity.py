#!/usr/bin/env python3
"""Generate the water-age and heat parity decks.

WHY THIS EXISTS
---------------
`tests/parity/README.md` §4: after roughly fifteen rounds of building water
age and heat, the bit-identity corpus had **0 water-age decks and 0 heat
decks**. "15/15 byte-identical" was structurally incapable of observing either
capability -- the same hole the snow deck closed for `[SNOWPACKS]`, and the
snow deck found a real ledger defect on its first run.

WHAT EACH DECK IS FOR
---------------------
Three decks, one generator, one shared network so that a difference between
them is the capability and not the model:

  age_legacy.inp   WATER_AGE ON, no pollutants, default (LEGACY) solver.
                   Age through the LEGACY CSTR mirror (A1b).
  age_ard.inp      The same body with QUALITY_SOLVER EULERIAN_ARD.
                   Age as a row on the ARD mesh (A1a).
  heat_parity.inp  One pollutant + WATER_AGE + heat: the reported stride is
                   np + age + heat = 3, which is D-UT10's seam and has never
                   appeared in any deck.  Carries heat_parity.heat.

`np = 0` on the two age decks is deliberate and is the interesting case: E5a
found all six `QualitySolver` loaders guarding `if (np <= 0) return`, which
blocked external-inflow VOLUME as well as mass, so a reserved-species-only
deck got zero boundary injection.  That was fixed, and nothing in any corpus
has re-checked it since.

THE NETWORK, AND WHY IT CASCADES
--------------------------------
`S1 -> S2 -> J1` with `S3 -> J1` direct.  Run-on is where both age defects
lived: A3 shipped run-on carrying the receiver's own age instead of the
donor's, and A4 found run-on counting one of three contributors, which
produced water younger than anything entering the model.  `S3` is the
no-run-on control -- without it a movement cannot be localised to the run-on
path rather than to subarea age in general.

THE MET SERIES
--------------
Seven days of wet/dry cycling.  Age needs dry spells to grow and rain to mix
old water with new, or every element reads the same number; heat needs
temperature contrast or the surface balance sits at one fixed point all run.
The dry days also reach the dry-element mask (age reports nothing when there
is no water) and the D-H5c `DRY_ELEMENT_TEMPERATURE HOLD` policy, which are
different answers to the same question and are both live.

DELIBERATELY NOT HERE
---------------------
  * **LID.**  A4/H5b put age and temperature through the LID layer stack, but
    issue #131 means a conventional [LID_CONTROLS] block reaches the solver
    unconverted -- a soil layer in inches arrives as 18 ft.  A corpus deck
    written today would bake in pre-#131 behaviour and move when #131 lands,
    which reads as a regression.  Owed until #131.
  * **Heat under EULERIAN_ARD** (H4's mesh row with per-cell surface fluxes).
    One more deck for one more solver; the LEGACY path is what an ordinary
    deck gets.  Owed, and cheap to add here when someone wants it.
  * **SI units.**  Register O6.  Every deck in the corpus is CFS.

Regeneration must be byte-reproducible: no RNG, no clock, no dict ordering.

USAGE
    python3 gen_transport_parity.py            # writes all four files here
    python3 gen_transport_parity.py --check    # regenerate and diff, no write
"""

import sys
import os
import difflib

HERE = os.path.dirname(os.path.abspath(__file__))

START = "01/01/2026"
END = "01/08/2026"
DAYS = 7
STEP_H = 1                       # met series interval, hours
N = DAYS * 24 // STEP_H + 1      # rows, inclusive of the final timestamp

# --- day table: (air_F, rain_in_hr, what it is for) -------------------------
# Air temperature spans 40-80 F.  The span matters more than the values: a
# flat series lets the surface energy balance reach one equilibrium and stay
# there, and then the deck cannot observe an integrator change.
DAYS_TABLE = [
    (45.0, 0.10, "wet-up -- every surface goes from dry to wet on day 1"),
    (70.0, 0.00, "dry and warm -- age grows, elements dry, heat drives up"),
    (55.0, 0.04, "light rain onto aged water -- the mixing contrast"),
    (80.0, 0.00, "dry and hot -- the widest air/water temperature gap"),
    (40.0, 0.00, "dry and cold -- the gap reverses sign"),
    (60.0, 0.15, "the largest event -- run-on actually reaches S2"),
    (50.0, 0.00, "tail -- drying down, the dry-element policies again"),
]


def series(name, value_of_day):
    """One row per STEP_H hours.  Values are held flat within a day, so any
    movement in the output attributes to a day, not to interpolation."""
    out = []
    for i in range(N):
        h = i * STEP_H
        day = min(h // 24, DAYS - 1)
        d = 1 + h // 24
        hh = h % 24
        out.append("%-8s 01/%02d/2026 %02d:00  %s"
                   % (name, d, hh, value_of_day(day)))
    return "\n".join(out) + "\n"


def rain_ts():
    return series("rain_ts", lambda d: "%.2f" % DAYS_TABLE[d][1])


def air_ts():
    return series("air_ts", lambda d: "%.1f" % DAYS_TABLE[d][0])


def options(extra):
    """Shared [OPTIONS].  KINWAVE for the same reason the snow deck uses it:
    routing is not what these decks observe, and dynamic wave costs an order
    of magnitude more wall clock for no coverage here."""
    o = [
        "[OPTIONS]",
        "FLOW_UNITS           CFS",
        ";; KINWAVE: routing is not what this deck observes.  See",
        ";; tests/parity/README.md §5 -- three sdm_fv_* decks are already",
        ";; 96 % of the corpus wall time and these must not add to that.",
        "FLOW_ROUTING         KINWAVE",
        "INFILTRATION         HORTON",
    ]
    o += extra
    o += [
        "START_DATE           " + START,
        "START_TIME           00:00:00",
        "REPORT_START_DATE    " + START,
        "REPORT_START_TIME    00:00:00",
        "END_DATE             " + END,
        "END_TIME             00:00:00",
        "WET_STEP             01:00:00",
        "DRY_STEP             01:00:00",
        "ROUTING_STEP         300",
        "REPORT_STEP          01:00:00",
        "ALLOW_PONDING        NO",
    ]
    return "\n".join(o) + "\n"


NETWORK = """
[EVAPORATION]
;; Zero and constant: a nonzero rate puts a second sink in the balance and
;; these decks are not about evaporation.  It is NOT inert for heat --
;; latent exchange is computed from the met forcing, not from this row.
CONSTANT             0.0
DRY_ONLY             NO

[RAINGAGES]
;;Name  Format     Interval  SCF  Source
RG1     INTENSITY  1:00      1.0  TIMESERIES rain_ts

[SUBCATCHMENTS]
;;Name  Gage  Outlet  Area  %Imperv  Width  Slope  CurbLen
;; S1 drains onto S2, not to the node.  Run-on is where both water-age
;; defects lived (A3: run-on carried the receiver's age, not the donor's;
;; A4: run-on counted one of three contributors).  Differing %Imperv and
;; slope give S1 and S2 genuinely different residence times, so donor and
;; receiver ages cannot coincide by accident -- a swap has to show.
S1      RG1   S2      5.0   70.0     500.0  1.0    0
S2      RG1   J1      5.0   30.0     300.0  0.5    0
;; The control: same gage, no run-on.  Without it a movement cannot be
;; localised to the run-on path rather than to subarea age in general.
S3      RG1   J1      5.0   50.0     400.0  0.8    0

[SUBAREAS]
;;Subcatch  Nimp   Nperv  Simp  Sperv  %Zero  RouteTo
S1         0.01   0.10   0.02  0.05   25     OUTLET
S2         0.01   0.10   0.05  0.10   25     OUTLET
S3         0.01   0.10   0.03  0.08   25     OUTLET

[INFILTRATION]
;;Subcatch  MaxRate  MinRate  Decay  DryTime  MaxInfil
S1         3.0      0.5      4      7        0
S2         3.0      0.5      4      7        0
S3         3.0      0.5      4      7        0

[JUNCTIONS]
;;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded
J1      10.0  10.0      0          0         0

[OUTFALLS]
;;Name  Elev  Type  Gated
OUT     9.0   FREE  NO

[CONDUITS]
;;Name  From  To   Length  N      Z1  Z2
C1      J1    OUT  400.0   0.013  0   0

[XSECTIONS]
;;Link  Shape     G1   G2  G3  G4  Barrels
C1      CIRCULAR  3.0  0   0   0   1
"""

COORDS_REPORT = """
[COORDINATES]
;;Node  X      Y
J1      100.0  100.0
OUT     200.0  100.0

[REPORT]
INPUT NO
CONTINUITY YES
"""


def timeseries():
    return "\n[TIMESERIES]\n;;Name   Date        Time   Value\n" \
           + rain_ts() + air_ts()


def age_deck(solver_row, title):
    parts = ["[TITLE]", title,
             ";; Generated by tests/parity/transport/gen_transport_parity.py;",
             ";; do not hand-edit.  Edit the generator and regenerate.",
             "", ""]
    extra = [
        ";; The reserved age species.  np = 0 here on purpose: a deck with",
        ";; age and NO pollutants is the configuration E5a found broken --",
        ";; six loaders guarded `if (np <= 0) return`, which blocked",
        ";; external-inflow volume as well as mass.  Fixed; unobserved since.",
        "WATER_AGE            ON",
    ]
    if solver_row:
        extra.append(solver_row)
    return "\n".join(parts) + options(extra) + NETWORK + timeseries() \
        + COORDS_REPORT


def heat_deck():
    parts = ["[TITLE]",
             "Heat + water age parity deck -- the np+age+heat stride",
             ";; Generated by tests/parity/transport/gen_transport_parity.py;",
             ";; do not hand-edit.  Edit the generator and regenerate.",
             "", ""]
    extra = [
        ";; All three reserved/reported classes at once.  The reported",
        ";; stride is np + age + heat = 3, which is D-UT10's seam: the",
        ";; decision to carry parallel per-capability accumulators rather",
        ";; than widen the loader tuple.  No deck has ever reached it.",
        "WATER_AGE            ON",
        "HEAT_TRANSPORT       ON",
    ]
    s = "\n".join(parts) + options(extra) + NETWORK
    # Air temperature is a TIMESERIES because that is the only air-temperature
    # source the engine reads: a MONTHLY row exists for wind and humidity but
    # not for temperature, and omitting the series leaves ClimateState at its
    # init value -- forcing the deck never set.
    s += "\n[TEMPERATURE]\nTIMESERIES air_ts\n"
    s += "WINDSPEED MONTHLY" + " 8.0" * 12 + "\n"
    s += "HUMIDITY" + " 60.0" * 12 + "\n"
    s += """
[POLLUTANTS]
;;Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  CoFrac  Cdwf  Cinit
;; One pollutant, zero decay.  Its job is to make np >= 1 so the stride is
;; np+age+heat rather than age+heat, and to be the row that must stay
;; bit-identical when a reserved column changes.
TSS     MG/L   2.0    0.0  0.0    0.0     NO        *         0.0     0.0   0.0

[PROCESS_COMPONENTS]
org.hydrocouple.openswmm.heat config="heat_parity.heat"
"""
    return s + timeseries() + COORDS_REPORT


HEAT_CFG = """;; Heat component configuration for heat_parity.inp.
;; Resolved RELATIVE TO THE .inp (SWMMEngine.cpp: base_dir =
;; parent_path(inp_path)), not to the working directory -- which is why this
;; sits beside the deck and why run_corpus.sh's per-deck cwd cannot break it.

[HEAT_SOURCES]
;; RAINFALL and INITIAL_STATE are deliberately different numbers.  If they
;; matched, "carried from the source" and "invented locally" would produce
;; the same value and the deck could not tell them apart (lesson 26).
RAINFALL      GLOBAL  8.0
INITIAL_STATE GLOBAL  14.0

[HEAT_FLUXES]
;; Both families ON.  D-H5e merged the node/link bindings so that every
;; enabled family is summed before ONE semi-implicit relaxation; with a
;; single family enabled that merge is unobservable, because there is
;; nothing to sum.  Two families is the minimum that can see it.
SURFACE_EXCHANGE      ON
RADIATIVE_EXCHANGE    ON
;; D-H5c: HOLD keeps a dry element's last temperature rather than resetting
;; it.  AIR and DEFAULT are the other two answers and are NOT exercised by
;; any corpus deck -- recorded in the round's handoff as owed.
DRY_ELEMENT_TEMPERATURE HOLD
"""

FILES = [
    ("age_legacy.inp",
     lambda: age_deck(None,
                      "Water age parity deck -- LEGACY CSTR mirror (A1b)")),
    ("age_ard.inp",
     lambda: age_deck("QUALITY_SOLVER       EULERIAN_ARD",
                      "Water age parity deck -- ARD mesh row (A1a)")),
    ("heat_parity.inp", heat_deck),
    ("heat_parity.heat", lambda: HEAT_CFG),
]


def main():
    check = "--check" in sys.argv
    bad = 0
    for name, gen in FILES:
        text = gen()
        path = os.path.join(HERE, name)
        if check:
            if not os.path.exists(path):
                print("MISSING: %s" % name)
                bad += 1
                continue
            have = open(path).read()
            if have != text:
                bad += 1
                print("DIFFERS: %s" % name)
                sys.stdout.writelines(
                    difflib.unified_diff(have.splitlines(True),
                                         text.splitlines(True),
                                         "on-disk", "generated"))
        else:
            with open(path, "w") as f:
                f.write(text)
            print("wrote %s (%d bytes)" % (name, len(text)))
    if check:
        print("check: %s" % ("FAIL" if bad else "all files match the generator"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
