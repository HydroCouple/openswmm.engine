#!/usr/bin/env python3
"""Refresh the committed Doxygen tagfile for the SWMMVis (GUI) manual.

The engine manual links into the GUI manual with `\\ref manual_*` /
`\\ref tutorial_*`. Doxygen resolves those through a tagfile that must exist on
local disk when `doxygen Doxyfile` runs, so the tagfile is **committed** rather
than fetched at build time: a build-time `curl` fails on fork PRs and whenever
the GUI Pages site is briefly unavailable, and the hard requirement is that the
engine docs build never breaks because of the other repo.

Only `<compound kind="page">` entries are kept. The GUI Doxyfile's INPUT covers
its whole `include/` and `src/` tree, so its published tag is ~6 MB of C++ API
entries the engine has no use for; filtered to pages it is ~10 KB and stops the
engine repo churning multi-megabyte diffs on every refresh.

The page URLs in the filtered file are the ones Doxygen itself emitted from the
real HTML run, so they are correct under `CREATE_SUBDIRS = YES` (which hashes
page paths). That is why this filters a published tag rather than generating a
second pages-only tag whose hashes would have to be assumed to match.

    refresh_gui_tagfile.py            fetch and rewrite the committed tagfile
    refresh_gui_tagfile.py --check    exit non-zero if the committed one is stale
    refresh_gui_tagfile.py --from F   read a local tagfile instead of fetching

Run it by hand when the GUI manual gains, loses or renames a page.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
TAGFILE = ROOT / "docs" / "tags" / "swmmvis-manual.tag"
SOURCE_URL = "https://www.hydrocouple.org/openswmm.gui/openswmm.gui.tag"


# Only the GUI manual's own namespace is imported. Both repos declare a page
# called `authors`, and Doxygen resolves an imported page id ahead of the local
# one — so importing it would make every engine `@ref authors` point at the GUI
# site and drop the engine's own authors page from the nav, with the build still
# green. Restricting to these prefixes keeps the two namespaces disjoint.
KEEP_PREFIXES = ("manual_", "tutorial_")


def filtered(raw: bytes) -> bytes:
    root = ET.fromstring(raw)
    kept = [c for c in list(root)
            if c.get("kind") == "page"
            and (c.findtext("name") or "").startswith(KEEP_PREFIXES)]
    if not kept:
        raise SystemExit("FAIL: the source tagfile contains no GUI manual pages "
                         f"(expected ids starting with {' or '.join(KEEP_PREFIXES)})")
    for c in list(root):
        root.remove(c)
    for c in kept:
        root.append(c)
    return ET.tostring(root, encoding="utf-8", xml_declaration=True) + b"\n"


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--check", action="store_true",
                   help="exit non-zero if the committed tagfile differs")
    p.add_argument("--from", dest="src", help="read this local tagfile instead of fetching")
    p.add_argument("--url", default=SOURCE_URL, help=f"source URL (default {SOURCE_URL})")
    args = p.parse_args()

    if args.src:
        raw = pathlib.Path(args.src).read_bytes()
    else:
        try:
            with urllib.request.urlopen(args.url, timeout=30) as r:
                raw = r.read()
        except (urllib.error.URLError, TimeoutError) as exc:
            # Never fail a build because the other site is unreachable.
            print(f"could not fetch {args.url} ({exc}) — skipped")
            return 0

    fresh = filtered(raw)
    pages = len(list(ET.fromstring(fresh)))

    if args.check:
        if not TAGFILE.exists():
            print(f"FAIL: {TAGFILE.relative_to(ROOT)} does not exist", file=sys.stderr)
            return 1
        if TAGFILE.read_bytes() == fresh:
            print(f"{TAGFILE.relative_to(ROOT)} is current ({pages} pages)")
            return 0
        print(f"FAIL: {TAGFILE.relative_to(ROOT)} is stale; "
              f"run scripts/refresh_gui_tagfile.py", file=sys.stderr)
        return 1

    TAGFILE.parent.mkdir(parents=True, exist_ok=True)
    TAGFILE.write_bytes(fresh)
    print(f"wrote {TAGFILE.relative_to(ROOT)} ({pages} pages, {len(fresh)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
