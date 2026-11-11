"""Regression coverage for legacy solver report/output paths (issue #200)."""

from pathlib import Path

import pytest

from openswmm.legacy import engine as solver


@pytest.fixture(params=["model.inp", "model.INP", "model", "model.v1.data"])
def input_file(tmp_path, request):
    # An .inp suffix in a parent directory must never be replaced.
    input_dir = tmp_path / "project.inp"
    input_dir.mkdir()
    inp = input_dir / request.param
    source = Path(__file__).parents[1] / "data" / "solver" / "site_drainage_example.inp"
    inp.write_bytes(source.read_bytes())
    return inp


def assert_outputs(inp, original_input, paths, custom):
    assert inp.read_bytes() == original_input
    for suffix, path, supplied in zip((".rpt", ".out"), paths, custom):
        assert path.is_file()
        assert path.stat().st_size > 0
        if supplied:
            assert not inp.with_suffix(suffix).exists()


@pytest.mark.parametrize("custom_report", [False, True])
@pytest.mark.parametrize("custom_output", [False, True])
@pytest.mark.parametrize("with_callback", [False, True])
def test_run_solver_output_paths(
    tmp_path, input_file, custom_report, custom_output, with_callback,
):
    inp = input_file
    original_input = inp.read_bytes()
    output_dir = tmp_path / "custom outputs"
    output_dir.mkdir()
    rpt = output_dir / "named.rpt" if custom_report else inp.with_suffix(".rpt")
    out = output_dir / "named.out" if custom_output else inp.with_suffix(".out")
    kwargs = {}
    if custom_report:
        kwargs["rpt_file"] = str(rpt)
    if custom_output:
        kwargs["out_file"] = str(out)
    progress = []
    if with_callback:
        kwargs["swmm_progress_callback"] = progress.append

    assert solver.run_solver(str(inp), **kwargs) == 0

    assert_outputs(inp, original_input, (rpt, out), (custom_report, custom_output))
    if with_callback:
        assert progress
        assert all(0.0 <= value <= 1.0 for value in progress)


@pytest.mark.parametrize("custom_report", [False, True])
@pytest.mark.parametrize("custom_output", [False, True])
def test_solver_output_paths(tmp_path, input_file, custom_report, custom_output):
    inp = input_file
    original_input = inp.read_bytes()
    output_dir = tmp_path / "custom outputs"
    output_dir.mkdir()
    rpt = output_dir / "named.rpt" if custom_report else inp.with_suffix(".rpt")
    out = output_dir / "named.out" if custom_output else inp.with_suffix(".out")
    kwargs = {}
    if custom_report:
        kwargs["rpt_file"] = str(rpt)
    if custom_output:
        kwargs["out_file"] = str(out)

    solver.Solver(str(inp), **kwargs).execute()

    assert_outputs(inp, original_input, (rpt, out), (custom_report, custom_output))
