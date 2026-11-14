"""Street-inlet Python API smoke tests against ``examples/inlets/street_inlet_junction.inp``.

Covers the bindings added with the inlet-junction work: ``Inlets.get_design``
(including the fixed-buffer ``curve_id`` -> ``str`` conversion), the
``InletUsages`` view, ``Node.is_inlet`` and ``ModelEditor.fuse_inlet_junction``,
plus a write/reopen round trip.  Written decks land in ``output/inlet_api/``
next to this file so a failure can be inspected.
"""
from __future__ import annotations

import os

import pytest

from openswmm.engine import (
    Infrastructure, InletHostKind, InletType, ModelEditor, Nodes, Solver,
)

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_INP = os.path.join(_REPO_ROOT, "examples", "inlets", "street_inlet_junction.inp")
_OUT_DIR = os.path.join(os.path.dirname(__file__), "output", "inlet_api")

pytestmark = pytest.mark.skipif(not os.path.exists(_INP), reason="example deck not present")


def _open(inp: str, tag: str) -> Solver:
    os.makedirs(_OUT_DIR, exist_ok=True)
    s = Solver(inp, os.path.join(_OUT_DIR, tag + ".rpt"), os.path.join(_OUT_DIR, tag + ".out"))
    s.open()
    return s


def _section(text: str, name: str) -> list[str]:
    rows, on = [], False
    for ln in text.splitlines():
        st = ln.strip()
        if st.startswith("["):
            on = st.upper() == f"[{name}]"
            continue
        if on and st and not st.startswith(";"):
            rows.append(st.split())
    return rows


def test_combo_design_carries_both_grate_and_curb() -> None:
    s = _open(_INP, "design")
    try:
        inlets = Infrastructure(s).inlets
        assert set(inlets) >= {"Grate1", "Curb1", "Combo1", "Custom1"}
        d = inlets.get_design("Combo1")
        assert d["type"] == InletType.COMBO
        assert (d["grate_length"], d["grate_width"]) == pytest.approx((2.0, 2.0))
        assert (d["curb_length"], d["curb_height"]) == pytest.approx((3.0, 0.5))
        custom = inlets.get_design("Custom1")
        assert custom["type"] == InletType.CUSTOM
        assert custom["curve_id"] == "DIV_CAP"          # char[64] -> str, no padding
        assert isinstance(custom["curve_id"], str)
    finally:
        s.close()


def test_usage_view_and_inlet_junction_flag() -> None:
    s = _open(_INP, "usage")
    try:
        infra, nodes = Infrastructure(s), Nodes(s)
        usages = list(infra.inlet_usages)
        assert len(usages) == 2
        kinds = {u["host_kind"] for u in usages}
        assert kinds == {InletHostKind.LINK, InletHostKind.NODE}
        ij1 = nodes["IJ1"]
        assert ij1.is_inlet is True
        assert ij1.is_virtual is True
        assert nodes["MH2"].is_inlet is False
        node_row = next(u for u in usages if u["host_kind"] == InletHostKind.NODE)
        assert node_row["host_idx"] == ij1.index
        assert nodes[node_row["capture_node_idx"]].id == "MH2"
        assert infra.inlet_usages.find_node(ij1.index) >= 0
    finally:
        s.close()


def test_write_reopen_keeps_inlet_junction() -> None:
    s = _open(_INP, "rt1")
    try:
        path = os.path.join(_OUT_DIR, "roundtrip.inp")
        s.write(path)
    finally:
        s.close()
    text = open(path, encoding="utf-8").read()
    assert [r[0] for r in _section(text, "INLET_JUNCTIONS")] == ["IJ1"]
    assert "IJ1" not in {r[0] for r in _section(text, "JUNCTIONS")}
    assert "IJ1" not in {r[0] for r in _section(text, "VIRTUAL_JUNCTIONS")}
    assert [r[:3] for r in _section(text, "INLET_USAGE")] == [["ST_A", "Combo1", "MH1"]]
    s2 = _open(path, "rt2")
    try:
        assert Nodes(s2)["IJ1"].is_inlet is True
        assert len(Infrastructure(s2).inlet_usages) == 2
    finally:
        s2.close()


def test_fuse_inlet_junction_merges_the_street() -> None:
    s = _open(_INP, "fuse")
    try:
        ModelEditor(s).fuse_inlet_junction("IJ1")
        assert "IJ1" not in Nodes(s)
        assert len(Infrastructure(s).inlet_usages) == 1     # ST_A's conduit inlet survives
        path = os.path.join(_OUT_DIR, "fused.inp")
        s.write(path)
    finally:
        s.close()
    text = open(path, encoding="utf-8").read()
    assert _section(text, "INLET_JUNCTIONS") == []
    conduits = {r[0]: r for r in _section(text, "CONDUITS")}
    assert "ST_A" in conduits
    survivors = {"ST_B", "ST_C"} & set(conduits)          # ST_B + ST_C fused into one
    assert len(survivors) == 1
    assert conduits[survivors.pop()][1:3] == ["J_MID", "OUT_ST"]
    assert [r[:3] for r in _section(text, "INLET_USAGE")] == [["ST_A", "Combo1", "MH1"]]
