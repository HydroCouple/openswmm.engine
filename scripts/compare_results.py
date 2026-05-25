#!/usr/bin/env python3
"""
Compare legacy vs refactored SWMM engine output files.

Reads two binary .out files and compares:
1. Header metadata (counts, flow units, variable counts)
2. Object IDs (must match exactly)
3. Per-timestep node/link/subcatchment results (numerical alignment)
4. Report file continuity errors

Usage:
    python compare_results.py <legacy.out> <refactored.out> [legacy.rpt] [refactored.rpt]
"""

import struct
import sys
import os
import numpy as np


# ============================================================================
# SWMM .out file reader (matching legacy output.c binary format)
# ============================================================================

SUBCATCH_RESULT_VARS = ["rainfall", "snow_depth", "evap_loss", "infil_loss",
                         "runoff", "gw_flow", "gw_elev", "soil_moist"]
NODE_RESULT_VARS = ["depth", "head", "volume", "lateral_inflow",
                     "total_inflow", "overflow"]
LINK_RESULT_VARS = ["flow", "depth", "velocity", "volume", "capacity"]


class SwmmOutReader:
    """Read a SWMM binary .out file."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self._read_header()

    def _read_int4(self):
        return struct.unpack("<i", self.f.read(4))[0]

    def _read_float4(self):
        return struct.unpack("<f", self.f.read(4))[0]

    def _read_header(self):
        self.magic = self._read_int4()
        self.version = self._read_int4()
        self.flow_units = self._read_int4()
        self.n_subcatch = self._read_int4()
        self.n_nodes = self._read_int4()
        self.n_links = self._read_int4()
        self.n_polluts = self._read_int4()

        # Read IDs
        self.subcatch_ids = [self._read_id() for _ in range(self.n_subcatch)]
        self.node_ids = [self._read_id() for _ in range(self.n_nodes)]
        self.link_ids = [self._read_id() for _ in range(self.n_links)]
        self.pollut_ids = [self._read_id() for _ in range(self.n_polluts)]

        # Pollutant units
        self.pollut_units = [self._read_int4() for _ in range(self.n_polluts)]

        # Input data
        self.input_start = self.f.tell()
        n_sc_input = self._read_int4()
        for _ in range(n_sc_input):
            self._read_int4()  # input code
        for _ in range(self.n_subcatch):
            self._read_float4()  # area

        n_nd_input = self._read_int4()
        for _ in range(n_nd_input):
            self._read_int4()
        for _ in range(self.n_nodes):
            for _ in range(n_nd_input):
                self._read_float4()

        n_lk_input = self._read_int4()
        for _ in range(n_lk_input):
            self._read_int4()
        for _ in range(self.n_links):
            for _ in range(n_lk_input):
                self._read_float4()

        self.output_start = self.f.tell()

        # Compute result sizes
        self.n_sc_vars = 8 + self.n_polluts
        self.n_nd_vars = 6 + self.n_polluts
        self.n_lk_vars = 5 + self.n_polluts

        # Bytes per output period. MAX_SYS_RESULTS = 15 since SWMM 5.3 (added
        # SYS_PET). See src/legacy/engine/enums.h:412 and
        # src/engine/plugins/DefaultOutputPlugin.hpp:110.
        self.bytes_per_period = (
            8 +  # date (double)
            self.n_subcatch * self.n_sc_vars * 4 +
            self.n_nodes * self.n_nd_vars * 4 +
            self.n_links * self.n_lk_vars * 4 +
            15 * 4  # system results
        )

        # Count periods from file size
        self.f.seek(0, 2)
        file_size = self.f.tell()
        # Last 24 bytes: id_start_pos, input_start_pos, output_start_pos,
        # n_periods, error_code, magic_end
        self.f.seek(-6 * 4, 2)
        self.id_start_pos_end = self._read_int4()
        self.input_start_pos_end = self._read_int4()
        self.output_start_pos_end = self._read_int4()
        self.n_periods = self._read_int4()
        self.error_code = self._read_int4()
        self.magic_end = self._read_int4()

        # The writer places the true output start AFTER the variable-code
        # tables, report start date (REAL8), and report step (INT4). The
        # initial f.tell() above lands before those — use the footer value.
        self.output_start = self.output_start_pos_end

    def _read_id(self):
        n = self._read_int4()
        return self.f.read(n).decode("ascii", errors="replace")

    def read_period(self, period_idx):
        """Read all results for a single output period."""
        offset = self.output_start + period_idx * self.bytes_per_period
        self.f.seek(offset)

        date = struct.unpack("<d", self.f.read(8))[0]

        sc = np.frombuffer(self.f.read(self.n_subcatch * self.n_sc_vars * 4),
                           dtype=np.float32).reshape(self.n_subcatch, self.n_sc_vars)
        nd = np.frombuffer(self.f.read(self.n_nodes * self.n_nd_vars * 4),
                           dtype=np.float32).reshape(self.n_nodes, self.n_nd_vars)
        lk = np.frombuffer(self.f.read(self.n_links * self.n_lk_vars * 4),
                           dtype=np.float32).reshape(self.n_links, self.n_lk_vars)
        sys_results = np.frombuffer(self.f.read(15 * 4), dtype=np.float32)

        return date, sc, nd, lk, sys_results

    def close(self):
        self.f.close()


def compare_outputs(legacy_path, refactored_path):
    """Compare two SWMM .out files."""
    print(f"\n{'='*70}")
    print(f"SWMM Output Comparison")
    print(f"{'='*70}")
    print(f"Legacy:     {legacy_path}")
    print(f"Refactored: {refactored_path}")
    print()

    leg = SwmmOutReader(legacy_path)
    ref = SwmmOutReader(refactored_path)

    # --- Header comparison ---
    print("--- Header Comparison ---")
    print(f"  {'Field':<25} {'Legacy':>10} {'Refactored':>10} {'Match':>8}")
    print(f"  {'-'*25} {'-'*10} {'-'*10} {'-'*8}")

    fields = [
        ("Magic", leg.magic, ref.magic),
        ("Version", leg.version, ref.version),
        ("Flow units", leg.flow_units, ref.flow_units),
        ("Subcatchments", leg.n_subcatch, ref.n_subcatch),
        ("Nodes", leg.n_nodes, ref.n_nodes),
        ("Links", leg.n_links, ref.n_links),
        ("Pollutants", leg.n_polluts, ref.n_polluts),
        ("Output periods", leg.n_periods, ref.n_periods),
    ]
    for name, lv, rv in fields:
        match = "YES" if lv == rv else "NO ***"
        print(f"  {name:<25} {lv:>10} {rv:>10} {match:>8}")

    # --- ID comparison ---
    print("\n--- Object ID Comparison ---")
    sc_match = leg.subcatch_ids == ref.subcatch_ids
    nd_match = leg.node_ids == ref.node_ids
    lk_match = leg.link_ids == ref.link_ids
    print(f"  Subcatchment IDs match: {sc_match}")
    print(f"  Node IDs match:         {nd_match}")
    print(f"  Link IDs match:         {lk_match}")

    if not (sc_match and nd_match and lk_match):
        print("\n  *** ID mismatch — cannot compare results element-by-element ***")
        leg.close()
        ref.close()
        return

    # --- Result comparison ---
    n_periods = min(leg.n_periods, ref.n_periods)
    if n_periods == 0:
        print("\n  No output periods to compare.")
        leg.close()
        ref.close()
        return

    print(f"\n--- Numerical Result Comparison ({n_periods} periods) ---")

    # Sample periods: first, middle, last, and every 10th
    sample_indices = sorted(set([0, n_periods // 4, n_periods // 2,
                                  3 * n_periods // 4, n_periods - 1]))

    # Aggregate statistics across ALL periods
    nd_max_diff = np.zeros(leg.n_nd_vars)
    nd_mean_diff = np.zeros(leg.n_nd_vars)
    lk_max_diff = np.zeros(leg.n_lk_vars)
    lk_mean_diff = np.zeros(leg.n_lk_vars)
    sc_max_diff = np.zeros(leg.n_sc_vars)
    sc_mean_diff = np.zeros(leg.n_sc_vars)

    for p in range(n_periods):
        ld, l_sc, l_nd, l_lk, l_sys = leg.read_period(p)
        rd, r_sc, r_nd, r_lk, r_sys = ref.read_period(p)

        # Absolute differences
        nd_diff = np.abs(l_nd - r_nd)
        lk_diff = np.abs(l_lk - r_lk)
        sc_diff = np.abs(l_sc - r_sc)

        nd_max_diff = np.maximum(nd_max_diff, nd_diff.max(axis=0))
        lk_max_diff = np.maximum(lk_max_diff, lk_diff.max(axis=0))
        sc_max_diff = np.maximum(sc_max_diff, sc_diff.max(axis=0))

        nd_mean_diff += nd_diff.mean(axis=0)
        lk_mean_diff += lk_diff.mean(axis=0)
        sc_mean_diff += sc_diff.mean(axis=0)

    nd_mean_diff /= n_periods
    lk_mean_diff /= n_periods
    sc_mean_diff /= n_periods

    # Print node results
    var_names = NODE_RESULT_VARS + [f"pollut_{i}" for i in range(leg.n_polluts)]
    print(f"\n  Node Results (across {leg.n_nodes} nodes × {n_periods} periods):")
    print(f"  {'Variable':<20} {'Max Abs Diff':>15} {'Mean Abs Diff':>15}")
    print(f"  {'-'*20} {'-'*15} {'-'*15}")
    for i, name in enumerate(var_names):
        print(f"  {name:<20} {nd_max_diff[i]:>15.6f} {nd_mean_diff[i]:>15.6f}")

    # Print link results
    var_names = LINK_RESULT_VARS + [f"pollut_{i}" for i in range(leg.n_polluts)]
    print(f"\n  Link Results (across {leg.n_links} links × {n_periods} periods):")
    print(f"  {'Variable':<20} {'Max Abs Diff':>15} {'Mean Abs Diff':>15}")
    print(f"  {'-'*20} {'-'*15} {'-'*15}")
    for i, name in enumerate(var_names):
        print(f"  {name:<20} {lk_max_diff[i]:>15.6f} {lk_mean_diff[i]:>15.6f}")

    # Print subcatchment results
    var_names = SUBCATCH_RESULT_VARS + [f"pollut_{i}" for i in range(leg.n_polluts)]
    print(f"\n  Subcatchment Results (across {leg.n_subcatch} subcatchments × {n_periods} periods):")
    print(f"  {'Variable':<20} {'Max Abs Diff':>15} {'Mean Abs Diff':>15}")
    print(f"  {'-'*20} {'-'*15} {'-'*15}")
    for i, name in enumerate(var_names):
        print(f"  {name:<20} {sc_max_diff[i]:>15.6f} {sc_mean_diff[i]:>15.6f}")

    # Overall summary
    all_max = max(nd_max_diff.max(), lk_max_diff.max(), sc_max_diff.max())
    all_mean = (nd_mean_diff.mean() + lk_mean_diff.mean() + sc_mean_diff.mean()) / 3
    print(f"\n  Overall maximum absolute difference: {all_max:.6f}")
    print(f"  Overall mean absolute difference:    {all_mean:.6f}")

    if all_max < 0.01:
        print(f"\n  *** EXCELLENT: Results are near-identical (max diff < 0.01) ***")
    elif all_max < 1.0:
        print(f"\n  *** GOOD: Results are closely aligned (max diff < 1.0) ***")
    elif all_max < 10.0:
        print(f"\n  *** MODERATE: Some divergence detected (max diff < 10) ***")
    else:
        print(f"\n  *** SIGNIFICANT: Results diverge substantially (max diff = {all_max:.2f}) ***")

    leg.close()
    ref.close()


def compare_reports(legacy_path, refactored_path):
    """Compare continuity errors from two .rpt files."""
    print(f"\n{'='*70}")
    print(f"Report File Comparison")
    print(f"{'='*70}")

    for label, path in [("Legacy", legacy_path), ("Refactored", refactored_path)]:
        if not os.path.exists(path):
            print(f"  {label}: File not found — {path}")
            continue
        print(f"\n  {label} ({path}):")
        # .rpt files contain Latin-1 symbols (e.g. ft³) in SI reports.
        with open(path, encoding="latin-1") as f:
            for line in f:
                if "Continuity Error" in line or "continuity error" in line:
                    print(f"    {line.rstrip()}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python compare_results.py <legacy.out> <refactored.out> [legacy.rpt] [refactored.rpt]")
        sys.exit(1)

    compare_outputs(sys.argv[1], sys.argv[2])

    if len(sys.argv) >= 5:
        compare_reports(sys.argv[3], sys.argv[4])

    print()
