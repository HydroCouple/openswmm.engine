"""Public consumers of checked table, control and LID authoring."""

from openswmm.engine import Solver, LidExtraField, LidType, LidNodeLayer


def checked(solver: Solver, layers: list[LidNodeLayer]) -> None:
    table = solver.tables.add_timeseries("Series")
    table.validate_points([(45000.0, 1.0)], relative_count=1, anchor=45000.0)
    table.replace_points([(45000.0, 1.0)], file_token="")
    solver.controls.replace_program(["RULE R"], publish=False)
    lids = solver.infrastructure.lids
    lids.validate_type(0, LidType.BIO_CELL)
    value: float = lids.get_extra(0, LidExtraField.DRAIN_DELAY)
    lids.set_extra(0, LidExtraField.DRAIN_DELAY, value)
    token: str = lids.get_drain_curve(0)
    lids.validate_layers(0, layers, flow={"model": 0}, materials=[{}, {}])
    _ = token
