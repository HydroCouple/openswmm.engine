"""
openswmm.legacy.output
======================

Legacy EPA SWMM 5.x binary output file reader (Cython).

This subpackage provides the :class:`Output` class for reading SWMM
``.out`` result files, and enumeration types for element and attribute
access.
"""

try:
    from ._output import (
        UnitSystem,
        FlowUnits,
        ConcentrationUnits,
        ElementType,
        TimeAttribute,
        SubcatchAttribute,
        NodeAttribute,
        LinkAttribute,
        SystemAttribute,
        SWMMOutputException,
        Output,
    )
except ImportError:
    pass
