"""Legacy v0 test file — superseded by every *_pythonic.py file.

The Pythonic v1 bindings hard-replaced this file's API surface
(`Nodes(s).get_depth("J1")` -> `s.nodes["J1"].depth`, etc.); the
new test file covers the v1 surface. This stub is kept so the test
run still collects cleanly; `git rm` it on the next sweep.

See docs/PYTHONIC_BINDINGS_DONE.md §"What's deferred" for the
migration notes.
"""
import pytest
pytest.skip("superseded by every *_pythonic.py file", allow_module_level=True)
