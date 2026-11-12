Ctrl-Z fixtures for `test_engine_2d_surface` (mesh open plan F4).

- `ctrlz.inp`, `ctrlz.2dm`: three vertex rows with a Ctrl-Z (0x1A) in a comment line
  before the third.

A text-mode read on Windows ends at the Ctrl-Z (two vertices); elsewhere the byte is part
of the comment (three). The .inp reader and the mesh-file reader must agree.
