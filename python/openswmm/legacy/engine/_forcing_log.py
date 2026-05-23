"""External forcing audit log for mass-balance tracking.

Records every ``set_*`` call that modifies a flux or state variable so the
user can verify, post-simulation, that the injected volumes match the routing
or runoff totals reported by SWMM.

Usage::

    from openswmm.legacy.engine import ExternalForcingLog

    log = ExternalForcingLog()
    # ... during stepping loop:
    node.set_lateral_inflow(1.5, log=log)
    # ... after simulation:
    df = log.to_dataframe()   # requires pandas
    records = log.records      # plain list of dicts
"""

from datetime import datetime
from typing import Any, List, Optional


class ExternalForcingLog:
    """Append-only log that records external forcing applied during a simulation.

    Each entry stores the simulation time, target object, property modified,
    and the value applied.
    """

    __slots__ = ("_records",)

    def __init__(self):
        self._records: List[dict] = []

    # -----------------------------------------------------------------
    #  Recording
    # -----------------------------------------------------------------

    def record(
        self,
        sim_time: datetime,
        object_type: str,
        object_id: str,
        property_name: str,
        value: float,
        mass_balance_category: str = "",
    ) -> None:
        """Append a forcing record.

        :param sim_time: Current simulation datetime.
        :param object_type: E.g. ``"node"``, ``"link"``, ``"subcatchment"``.
        :param object_id: Object name or index string.
        :param property_name: Property that was set.
        :param value: The value applied.
        :param mass_balance_category: Which mass-balance bucket is affected.
        """
        self._records.append(
            {
                "time": sim_time,
                "object_type": object_type,
                "object_id": object_id,
                "property": property_name,
                "value": value,
                "mass_balance_category": mass_balance_category,
            }
        )

    # -----------------------------------------------------------------
    #  Access
    # -----------------------------------------------------------------

    @property
    def records(self) -> List[dict]:
        """Return the raw list of recorded forcing entries."""
        return list(self._records)

    def __len__(self) -> int:
        return len(self._records)

    def clear(self) -> None:
        """Remove all recorded entries."""
        self._records.clear()

    def to_dataframe(self):
        """Convert to a ``pandas.DataFrame``.

        Columns: ``time``, ``object_type``, ``object_id``, ``property``,
        ``value``, ``mass_balance_category``.

        :raises ImportError: If pandas is not installed.
        """
        import pandas as pd  # type: ignore[import-untyped]

        return pd.DataFrame(self._records)
