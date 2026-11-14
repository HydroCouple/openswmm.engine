Inputs and pinned outputs for `test_engine_mesh_snapshots` (`tests/unit/engine/test_mesh_snapshots.cpp`).

These are the edge cases the mesh open program (2026-11) checked field by field, old reader
against new, at every step. They now pin the readers' behaviour.

- `mesh_file/*.2dm`: read through `load2DMeshExternalFile`, as a `[2D_MESH_FILE]` is loaded.
  Line endings (CRLF, lone CR, none at the end), BOMs, NUL and form-feed bytes, header forms,
  quotes, commas and `;`, number and index forms (hex, overflow, trailing text), short and
  erroneous rows, repeated headers, quads, node maps, boundary / conveyance / quality /
  infiltration rows, and every form of the `;; UNITS:` marker.
- `inp/*.inp`: whole models opened leniently, as the GUI's editor opens them: a small coupled
  2D model under line-ending, BOM, `[TITLE]`, blank-line, unknown-section and `;; UNITS:`
  marker variants.

Each `<name>.expected.txt` holds the dump of `<name>.2dm` or `<name>.inp`. Files with a Ctrl-Z
byte are not here: Windows ends a text-mode read there, so their result depends on the
platform (`CtrlZ.BothReadersEndTheFileThereOnlyOnWindows` covers that).

After an intended behaviour change, regenerate with `OPENSWMM_UPDATE_MESH_SNAPSHOTS=1` and
review the diff. After adding or removing a data member of a dumped record, rerun
`tests/scripts/gen_mesh_snapshot_fields.py <build-dir>` first.
