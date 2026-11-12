UTF-8 byte-order-mark fixtures for `test_engine_2d_surface` (mesh open plan F1).

- `bom_header.2dm`: a BOM directly before the first section header.
- `bom_units.2dm`: a BOM before a `;; UNITS: SI (m)` marker on the first line.

Before the fix the BOM hid the header (no vertices) and the marker (SI flag stayed off).
