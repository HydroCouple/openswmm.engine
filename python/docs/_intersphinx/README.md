# Intersphinx fallback inventories

Sphinx tries each project's live inventory first, with a 10-second network
timeout, then uses the corresponding snapshot here if that request fails.
This keeps Python and NumPy cross-references available during upstream outages
without disabling warnings or weakening the strict documentation build.
If both the live inventory and the snapshot fail, Sphinx still emits a warning
and the CI build fails.

The files contain symbol names and link targets, not the documentation pages.
Sphinx's [inventory fallback documentation](https://www.sphinx-doc.org/en/master/usage/extensions/intersphinx.html#confval-intersphinx_mapping)
describes the configuration used in `../conf.py`.

| File | Upstream source | Inventory version | Snapshot provenance |
| --- | --- | --- | --- |
| `python.inv` | <https://docs.python.org/3/objects.inv> | Python 3.14 | Recovered from the local Sphinx download cache dated 2026-08-31; the upstream host was returning HTTP 503 on 2026-10-01. |
| `numpy.inv` | <https://numpy.org/doc/stable/objects.inv> | NumPy 2.5 | Downloaded 2026-10-01. |

SHA-256 checksums:

```text
bcc3e9c5ee721856688c1995283f507bf7f852b8bb13606528d95a59eb1376b8  python.inv
d16572c0fac3f941a5eb831f393a18418e90429a7f9e439976f2f01a6b3ee877  numpy.inv
```

To refresh, download the sources above to temporary files using an HTTP client
that fails on unsuccessful responses (for example, `curl --fail --location
--retry 3`). Validate each download with `python -m sphinx.ext.intersphinx
<file>`, replace the snapshot, and update its version, provenance and checksum
in this file. Run the strict Sphinx HTML build with remote inventory requests
unavailable as well as with normal network access, and confirm the generated
HTML still links Python and NumPy types to their official documentation.
